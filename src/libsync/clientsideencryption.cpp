#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/engine.h>
#include <openssl/rand.h>


#include "clientsideencryption.h"
#include "account.h"
#include "capabilities.h"
#include "networkjobs.h"
#include "clientsideencryptionjobs.h"
#include "theme.h"
#include "creds/abstractcredentials.h"

#include <map>
#include <string>
#include <algorithm>

#include <cstdio>

#include <QDebug>
#include <QLoggingCategory>
#include <QFileInfo>
#include <QDir>
#include <QJsonObject>
#include <QXmlStreamReader>
#include <QXmlStreamNamespaceDeclaration>
#include <QStack>
#include <QInputDialog>
#include <QLineEdit>
#include <QIODevice>
#include <QUuid>
#include <QScopeGuard>
#include <QRandomGenerator>

#include <qt5keychain/keychain.h>
#include <common/utility.h>
#include <common/constants.h>

#include "wordlist.h"

QDebug operator<<(QDebug out, const std::string& str)
{
    out << QString::fromStdString(str);
    return out;
}

using namespace QKeychain;

namespace OCC
{

Q_LOGGING_CATEGORY(lcCse, "nextcloud.sync.clientsideencryption", QtInfoMsg)
Q_LOGGING_CATEGORY(lcCseDecryption, "nextcloud.e2e", QtInfoMsg)
Q_LOGGING_CATEGORY(lcCseMetadata, "nextcloud.metadata", QtInfoMsg)

QString baseUrl(){
    return QStringLiteral("ocs/v2.php/apps/end_to_end_encryption/api/v1/");
}

namespace {
    constexpr char accountProperty[] = "account";

    const char e2e_cert[] = "_e2e-certificate";
    const char e2e_private[] = "_e2e-private";
    const char e2e_mnemonic[] = "_e2e-mnemonic";

    const int blockSize = 1024;

    QList<QByteArray> oldCipherFormatSplit(const QByteArray &cipher)
    {
        const auto separator = QByteArrayLiteral("fA=="); // BASE64 encoded '|'
        auto result = QList<QByteArray>();

        auto data = cipher;
        auto index = data.indexOf(separator);
        while (index >=0) {
            result.append(data.left(index));
            data = data.mid(index + separator.size());
            index = data.indexOf(separator);
        }

        result.append(data);
        return result;
    }

    QList<QByteArray> splitCipherParts(const QByteArray &data)
    {
        const auto isOldFormat = !data.contains('|');
        const auto parts = isOldFormat ? oldCipherFormatSplit(data) : data.split('|');
        qCInfo(lcCse()) << "found parts:" << parts << "old format?" << isOldFormat;
        return parts;
    }
} // ns

namespace {
    unsigned char* unsignedData(QByteArray& array)
    {
        return (unsigned char*)array.data();
    }

    //
    // Simple classes for safe (RAII) handling of OpenSSL
    // data structures
    //

    class CipherCtx {
    public:
        CipherCtx()
            : _ctx(EVP_CIPHER_CTX_new())
        {
        }

        ~CipherCtx()
        {
            EVP_CIPHER_CTX_free(_ctx);
        }

        operator EVP_CIPHER_CTX*()
        {
            return _ctx;
        }

    private:
        Q_DISABLE_COPY(CipherCtx)

        EVP_CIPHER_CTX* _ctx;
    };

    class Bio {
    public:
        Bio()
            : _bio(BIO_new(BIO_s_mem()))
        {
        }

        ~Bio()
        {
            BIO_free_all(_bio);
        }

        operator BIO*()
        {
            return _bio;
        }

    private:
        Q_DISABLE_COPY(Bio)

        BIO* _bio;
    };

    class PKeyCtx {
    public:
        explicit PKeyCtx(int id, ENGINE *e = nullptr)
            : _ctx(EVP_PKEY_CTX_new_id(id, e))
        {
        }

        ~PKeyCtx()
        {
            EVP_PKEY_CTX_free(_ctx);
        }

        // The move constructor is needed for pre-C++17 where
        // return-value optimization (RVO) is not obligatory
        // and we have a `forKey` static function that returns
        // an instance of this class
        PKeyCtx(PKeyCtx&& other)
        {
            std::swap(_ctx, other._ctx);
        }

        PKeyCtx& operator=(PKeyCtx&& other) = delete;

        static PKeyCtx forKey(EVP_PKEY *pkey, ENGINE *e = nullptr)
        {
            PKeyCtx ctx;
            ctx._ctx = EVP_PKEY_CTX_new(pkey, e);
            return ctx;
        }

        operator EVP_PKEY_CTX*()
        {
            return _ctx;
        }

    private:
        Q_DISABLE_COPY(PKeyCtx)

        PKeyCtx() = default;

        EVP_PKEY_CTX* _ctx = nullptr;
    };

    class PKey {
    public:
        ~PKey()
        {
            EVP_PKEY_free(_pkey);
        }

        // The move constructor is needed for pre-C++17 where
        // return-value optimization (RVO) is not obligatory
        // and we have a static functions that return
        // an instance of this class
        PKey(PKey&& other)
        {
            std::swap(_pkey, other._pkey);
        }

        PKey& operator=(PKey&& other) = delete;

        static PKey readPublicKey(Bio &bio)
        {
            PKey result;
            result._pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
            return result;
        }

        static PKey readPrivateKey(Bio &bio)
        {
            PKey result;
            result._pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
            return result;
        }

        static PKey generate(PKeyCtx& ctx)
        {
            PKey result;
            if (EVP_PKEY_keygen(ctx, &result._pkey) <= 0) {
                result._pkey = nullptr;
            }
            return result;
        }

        operator EVP_PKEY*()
        {
            return _pkey;
        }

        operator EVP_PKEY*() const
        {
            return _pkey;
        }

    private:
        Q_DISABLE_COPY(PKey)

        PKey() = default;

        EVP_PKEY* _pkey = nullptr;
    };

    class X509Certificate {
    public:
        ~X509Certificate()
        {
            X509_free(_certificate);
        }

        // The move constructor is needed for pre-C++17 where
        // return-value optimization (RVO) is not obligatory
        // and we have a static functions that return
        // an instance of this class
        X509Certificate(X509Certificate&& other)
        {
            std::swap(_certificate, other._certificate);
        }

        X509Certificate& operator=(X509Certificate&& other) = delete;

        static X509Certificate readCertificate(Bio &bio)
        {
            X509Certificate result;
            result._certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
            return result;
        }

        operator X509*()
        {
            return _certificate;
        }

        operator X509*() const
        {
            return _certificate;
        }

    private:
        Q_DISABLE_COPY(X509Certificate)

        X509Certificate() = default;

        X509* _certificate = nullptr;
    };

    QByteArray BIO2ByteArray(Bio &b) {
        int pending = static_cast<int>(BIO_ctrl_pending(b));
        QByteArray res(pending, '\0');
        BIO_read(b, unsignedData(res), pending);
        return res;
    }

    QByteArray handleErrors()
    {
        Bio bioErrors;
        ERR_print_errors(bioErrors); // This line is not printing anything.
        return BIO2ByteArray(bioErrors);
    }
}


namespace EncryptionHelper {




QByteArray generateRandomFilename()
{
    return QUuid::createUuid().toRfc4122().toHex();
}

QByteArray generateRandom(int size)
{
    QByteArray result(size, '\0');

    int ret = RAND_bytes(unsignedData(result), size);
    if (ret != 1) {
        qCInfo(lcCse()) << "Random byte generation failed!";
        // Error out?
    }

    return result;
}

QByteArray generatePassword(const QString& wordlist, const QByteArray& salt) {
    qCInfo(lcCse()) << "Start encryption key generation!";

    const int iterationCount = 1024;
    const int keyStrength = 256;
    const int keyLength = keyStrength/8;

    QByteArray secretKey(keyLength, '\0');

    int ret = PKCS5_PBKDF2_HMAC_SHA1(
        wordlist.toLocal8Bit().constData(),     // const char *password,
        wordlist.size(),                        // int password length,
        (const unsigned char *)salt.constData(),// const unsigned char *salt,
        salt.size(),                            // int saltlen,
        iterationCount,                         // int iterations,
        keyLength,                              // int keylen,
        unsignedData(secretKey)                 // unsigned char *out
    );

    if (ret != 1) {
        qCInfo(lcCse()) << "Failed to generate encryption key";
        // Error out?
    }

    qCInfo(lcCse()) << "Encryption key generated!";

    return secretKey;
}

QByteArray encryptPrivateKey(
        const QByteArray& key,
        const QByteArray& privateKey,
        const QByteArray& salt
        ) {

    QByteArray iv = generateRandom(12);

    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Error creating cipher";
        handleErrors();
    }

    /* Initialise the decryption operation. */
    if(!EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Error initializing context with aes_256";
        handleErrors();
    }

    // No padding
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    /* Set IV length. */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Error setting iv length";
        handleErrors();
    }

    /* Initialise key and IV */
    if(!EVP_EncryptInit_ex(ctx, nullptr, nullptr, (unsigned char *)key.constData(), (unsigned char *)iv.constData())) {
        qCInfo(lcCse()) << "Error initialising key and iv";
        handleErrors();
    }

    // We write the base64 encoded private key
    QByteArray privateKeyB64 = privateKey.toBase64();

    // Make sure we have enough room in the cipher text
    QByteArray ctext(privateKeyB64.size() + 32, '\0');

    // Do the actual encryption
    int len = 0;
    if(!EVP_EncryptUpdate(ctx, unsignedData(ctext), &len, (unsigned char *)privateKeyB64.constData(), privateKeyB64.size())) {
        qCInfo(lcCse()) << "Error encrypting";
        handleErrors();
    }

    int clen = len;

    /* Finalise the encryption. Normally ciphertext bytes may be written at
     * this stage, but this does not occur in GCM mode
     */
    if(1 != EVP_EncryptFinal_ex(ctx, unsignedData(ctext) + len, &len)) {
        qCInfo(lcCse()) << "Error finalizing encryption";
        handleErrors();
    }
    clen += len;

    /* Get the tag */
    QByteArray tag(OCC::CommonConstants::e2EeTagSize, '\0');
    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, OCC::CommonConstants::e2EeTagSize, unsignedData(tag))) {
        qCInfo(lcCse()) << "Error getting the tag";
        handleErrors();
    }

    QByteArray cipherTXT;
    cipherTXT.reserve(clen + 16);
    cipherTXT.append(ctext, clen);
    cipherTXT.append(tag);

    QByteArray result = cipherTXT.toBase64();
    result += '|';
    result += iv.toBase64();
    result += '|';
    result += salt.toBase64();

    return result;
}

QByteArray decryptPrivateKey(const QByteArray& key, const QByteArray& data) {
    qCInfo(lcCse()) << "decryptStringSymmetric key: " << key;
    qCInfo(lcCse()) << "decryptStringSymmetric data: " << data;

    const auto parts = splitCipherParts(data);
    if (parts.size() < 2) {
        qCInfo(lcCse()) << "Not enough parts found";
        return QByteArray();
    }

    QByteArray cipherTXT64 = parts.at(0);
    QByteArray ivB64 = parts.at(1);

    qCInfo(lcCse()) << "decryptStringSymmetric cipherTXT: " << cipherTXT64;
    qCInfo(lcCse()) << "decryptStringSymmetric IV: " << ivB64;

    QByteArray cipherTXT = QByteArray::fromBase64(cipherTXT64);
    QByteArray iv = QByteArray::fromBase64(ivB64);

    QByteArray tag = cipherTXT.right(OCC::CommonConstants::e2EeTagSize);
    cipherTXT.chop(OCC::CommonConstants::e2EeTagSize);

    // Init
    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Error creating cipher";
        return QByteArray();
    }

    /* Initialise the decryption operation. */
    if(!EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Error initialising context with aes 256";
        return QByteArray();
    }

    /* Set IV length. Not necessary if this is 12 bytes (96 bits) */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Error setting IV size";
        return QByteArray();
    }

    /* Initialise key and IV */
    if(!EVP_DecryptInit_ex(ctx, nullptr, nullptr, (unsigned char *)key.constData(), (unsigned char *)iv.constData())) {
        qCInfo(lcCse()) << "Error initialising key and iv";
        return QByteArray();
    }

    QByteArray ptext(cipherTXT.size() + OCC::CommonConstants::e2EeTagSize, '\0');
    int plen = 0;

    /* Provide the message to be decrypted, and obtain the plaintext output.
     * EVP_DecryptUpdate can be called multiple times if necessary
     */
    if(!EVP_DecryptUpdate(ctx, unsignedData(ptext), &plen, (unsigned char *)cipherTXT.constData(), cipherTXT.size())) {
        qCInfo(lcCse()) << "Could not decrypt";
        return QByteArray();
    }

    /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), (unsigned char *)tag.constData())) {
        qCInfo(lcCse()) << "Could not set tag";
        return QByteArray();
    }

    /* Finalise the decryption. A positive return value indicates success,
     * anything else is a failure - the plaintext is not trustworthy.
     */
    int len = plen;
    if (EVP_DecryptFinal_ex(ctx, unsignedData(ptext) + plen, &len) == 0) {
        qCInfo(lcCse()) << "Tag did not match!";
        return QByteArray();
    }

    QByteArray result(ptext, plen);
    return QByteArray::fromBase64(result);
}

QByteArray extractPrivateKeySalt(const QByteArray &data)
{
    const auto parts = splitCipherParts(data);
    if (parts.size() < 3) {
        qCInfo(lcCse()) << "Not enough parts found";
        return QByteArray();
    }

    return QByteArray::fromBase64(parts.at(2));
}

QByteArray decryptStringSymmetric(const QByteArray& key, const QByteArray& data) {
    qCInfo(lcCse()) << "decryptStringSymmetric key: " << key;
    qCInfo(lcCse()) << "decryptStringSymmetric data: " << data;

    const auto parts = splitCipherParts(data);
    if (parts.size() < 2) {
        qCInfo(lcCse()) << "Not enough parts found";
        return QByteArray();
    }

    QByteArray cipherTXT64 = parts.at(0);
    QByteArray ivB64 = parts.at(1);

    qCInfo(lcCse()) << "decryptStringSymmetric cipherTXT: " << cipherTXT64;
    qCInfo(lcCse()) << "decryptStringSymmetric IV: " << ivB64;

    QByteArray cipherTXT = QByteArray::fromBase64(cipherTXT64);
    QByteArray iv = QByteArray::fromBase64(ivB64);

    QByteArray tag = cipherTXT.right(OCC::CommonConstants::e2EeTagSize);
    cipherTXT.chop(OCC::CommonConstants::e2EeTagSize);

    // Init
    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Error creating cipher";
        return QByteArray();
    }

    /* Initialise the decryption operation. */
    if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Error initialising context with aes 128";
        return QByteArray();
    }

    /* Set IV length. Not necessary if this is 12 bytes (96 bits) */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Error setting IV size";
        return QByteArray();
    }

    /* Initialise key and IV */
    if(!EVP_DecryptInit_ex(ctx, nullptr, nullptr, (unsigned char *)key.constData(), (unsigned char *)iv.constData())) {
        qCInfo(lcCse()) << "Error initialising key and iv";
        return QByteArray();
    }

    QByteArray ptext(cipherTXT.size() + OCC::CommonConstants::e2EeTagSize, '\0');
    int plen = 0;

    /* Provide the message to be decrypted, and obtain the plaintext output.
     * EVP_DecryptUpdate can be called multiple times if necessary
     */
    if(!EVP_DecryptUpdate(ctx, unsignedData(ptext), &plen, (unsigned char *)cipherTXT.constData(), cipherTXT.size())) {
        qCInfo(lcCse()) << "Could not decrypt";
        return QByteArray();
    }

    /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), (unsigned char *)tag.constData())) {
        qCInfo(lcCse()) << "Could not set tag";
        return QByteArray();
    }

    /* Finalise the decryption. A positive return value indicates success,
     * anything else is a failure - the plaintext is not trustworthy.
     */
    int len = plen;
    if (EVP_DecryptFinal_ex(ctx, unsignedData(ptext) + plen, &len) == 0) {
        qCInfo(lcCse()) << "Tag did not match!";
        return QByteArray();
    }

    return QByteArray::fromBase64(QByteArray(ptext, plen));
}

QByteArray privateKeyToPem(const QByteArray key) {
    Bio privateKeyBio;
    BIO_write(privateKeyBio, key.constData(), key.size());
    auto pkey = PKey::readPrivateKey(privateKeyBio);

    Bio pemBio;
    PEM_write_bio_PKCS8PrivateKey(pemBio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    QByteArray pem = BIO2ByteArray(pemBio);

    return pem;
}

QByteArray encryptStringSymmetric(const QByteArray& key, const QByteArray& data) {
    QByteArray iv = generateRandom(16);

    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Error creating cipher";
        handleErrors();
        return {};
    }

    /* Initialise the decryption operation. */
    if(!EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Error initializing context with aes_128";
        handleErrors();
        return {};
    }

    // No padding
    EVP_CIPHER_CTX_set_padding(ctx, 0);

    /* Set IV length. */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Error setting iv length";
        handleErrors();
        return {};
    }

    /* Initialise key and IV */
    if(!EVP_EncryptInit_ex(ctx, nullptr, nullptr, (unsigned char *)key.constData(), (unsigned char *)iv.constData())) {
        qCInfo(lcCse()) << "Error initialising key and iv";
        handleErrors();
        return {};
    }

    // We write the data base64 encoded
    QByteArray dataB64 = data.toBase64();

    // Make sure we have enough room in the cipher text
    QByteArray ctext(dataB64.size() + 16, '\0');

    // Do the actual encryption
    int len = 0;
    if(!EVP_EncryptUpdate(ctx, unsignedData(ctext), &len, (unsigned char *)dataB64.constData(), dataB64.size())) {
        qCInfo(lcCse()) << "Error encrypting";
        handleErrors();
        return {};
    }

    int clen = len;

    /* Finalise the encryption. Normally ciphertext bytes may be written at
     * this stage, but this does not occur in GCM mode
     */
    if(1 != EVP_EncryptFinal_ex(ctx, unsignedData(ctext) + len, &len)) {
        qCInfo(lcCse()) << "Error finalizing encryption";
        handleErrors();
        return {};
    }
    clen += len;

    /* Get the tag */
    QByteArray tag(OCC::CommonConstants::e2EeTagSize, '\0');
    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, OCC::CommonConstants::e2EeTagSize, unsignedData(tag))) {
        qCInfo(lcCse()) << "Error getting the tag";
        handleErrors();
        return {};
    }

    QByteArray cipherTXT;
    cipherTXT.reserve(clen + 16);
    cipherTXT.append(ctext, clen);
    cipherTXT.append(tag);

    QByteArray result = cipherTXT.toBase64();
    result += '|';
    result += iv.toBase64();

    return result;
}

QByteArray decryptStringAsymmetric(EVP_PKEY *privateKey, const QByteArray& data) {
    int err = -1;

    qCInfo(lcCseDecryption()) << "Start to work the decryption.";
    auto ctx = PKeyCtx::forKey(privateKey, ENGINE_get_default_RSA());
    if (!ctx) {
        qCInfo(lcCseDecryption()) << "Could not create the PKEY context.";
        handleErrors();
        return {};
    }

    err = EVP_PKEY_decrypt_init(ctx);
    if (err <= 0) {
        qCInfo(lcCseDecryption()) << "Could not init the decryption of the metadata";
        handleErrors();
        return {};
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        qCInfo(lcCseDecryption()) << "Error setting the encryption padding.";
        handleErrors();
        return {};
    }

    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) {
        qCInfo(lcCseDecryption()) << "Error setting OAEP SHA 256";
        handleErrors();
        return {};
    }

    if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
        qCInfo(lcCseDecryption()) << "Error setting MGF1 padding";
        handleErrors();
        return {};
    }

    size_t outlen = 0;
    err = EVP_PKEY_decrypt(ctx, nullptr, &outlen,  (unsigned char *)data.constData(), data.size());
    if (err <= 0) {
        qCInfo(lcCseDecryption()) << "Could not determine the buffer length";
        handleErrors();
        return {};
    } else {
        qCInfo(lcCseDecryption()) << "Size of output is: " << outlen;
        qCInfo(lcCseDecryption()) << "Size of data is: " << data.size();
    }

    QByteArray out(static_cast<int>(outlen), '\0');

    if (EVP_PKEY_decrypt(ctx, unsignedData(out), &outlen, (unsigned char *)data.constData(), data.size()) <= 0) {
        const auto error = handleErrors();
        qCCritical(lcCseDecryption()) << "Could not decrypt the data." << error;
        return {};
    } else {
        qCInfo(lcCseDecryption()) << "data decrypted successfully";
    }

    qCInfo(lcCse()) << out;
    return out;
}

QByteArray encryptStringAsymmetric(EVP_PKEY *publicKey, const QByteArray& data) {
    int err = -1;

    auto ctx = PKeyCtx::forKey(publicKey, ENGINE_get_default_RSA());
    if (!ctx) {
        qCInfo(lcCse()) << "Could not initialize the pkey context.";
        exit(1);
    }

    if (EVP_PKEY_encrypt_init(ctx) != 1) {
        qCInfo(lcCse()) << "Error initilaizing the encryption.";
        exit(1);
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0) {
        qCInfo(lcCse()) << "Error setting the encryption padding.";
        exit(1);
    }

    if (EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) <= 0) {
        qCInfo(lcCse()) << "Error setting OAEP SHA 256";
        exit(1);
    }

    if (EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) <= 0) {
        qCInfo(lcCse()) << "Error setting MGF1 padding";
        exit(1);
    }

    size_t outLen = 0;
    if (EVP_PKEY_encrypt(ctx, nullptr, &outLen, (unsigned char *)data.constData(), data.size()) != 1) {
        qCInfo(lcCse()) << "Error retrieving the size of the encrypted data";
        exit(1);
    } else {
        qCInfo(lcCse()) << "Encryption Length:" << outLen;
    }

    QByteArray out(static_cast<int>(outLen), '\0');
    if (EVP_PKEY_encrypt(ctx, unsignedData(out), &outLen, (unsigned char *)data.constData(), data.size()) != 1) {
        qCInfo(lcCse()) << "Could not encrypt key." << err;
        exit(1);
    }

    // Transform the encrypted data into base64.
    qCInfo(lcCse()) << out.toBase64();
    return out.toBase64();
}

}


ClientSideEncryption::ClientSideEncryption() = default;

void ClientSideEncryption::initialize(const AccountPtr &account)
{
    Q_ASSERT(account);

    qCInfo(lcCse()) << "Initializing";
    if (!account->capabilities().clientSideEncryptionAvailable()) {
        qCInfo(lcCse()) << "No Client side encryption available on server.";
        emit initializationFinished();
        return;
    }

    fetchFromKeyChain(account);
}

void ClientSideEncryption::fetchFromKeyChain(const AccountPtr &account)
{
    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_cert,
                account->id()
    );

    auto *job = new ReadPasswordJob(Theme::instance()->appName());
    job->setProperty(accountProperty, QVariant::fromValue(account));
    job->setInsecureFallback(false);
    job->setKey(kck);
    connect(job, &ReadPasswordJob::finished, this, &ClientSideEncryption::publicKeyFetched);
    job->start();
}

bool ClientSideEncryption::checkPublicKeyValidity(const AccountPtr &account) const
{
    QByteArray data = EncryptionHelper::generateRandom(64);

    Bio publicKeyBio;
    QByteArray publicKeyPem = account->e2e()->_publicKey.toPem();
    BIO_write(publicKeyBio, publicKeyPem.constData(), publicKeyPem.size());
    auto publicKey = PKey::readPublicKey(publicKeyBio);

    auto encryptedData = EncryptionHelper::encryptStringAsymmetric(publicKey, data.toBase64());

    Bio privateKeyBio;
    QByteArray privateKeyPem = account->e2e()->_privateKey;
    BIO_write(privateKeyBio, privateKeyPem.constData(), privateKeyPem.size());
    auto key = PKey::readPrivateKey(privateKeyBio);

    QByteArray decryptResult = QByteArray::fromBase64(EncryptionHelper::decryptStringAsymmetric( key, QByteArray::fromBase64(encryptedData)));

    if (data != decryptResult) {
        qCInfo(lcCse()) << "invalid private key";
        return false;
    }

    return true;
}

bool ClientSideEncryption::checkServerPublicKeyValidity(const QByteArray &serverPublicKeyString) const
{
    Bio serverPublicKeyBio;
    BIO_write(serverPublicKeyBio, serverPublicKeyString.constData(), serverPublicKeyString.size());
    const auto serverPublicKey = PKey::readPrivateKey(serverPublicKeyBio);

    Bio certificateBio;
    const auto certificatePem = _certificate.toPem();
    BIO_write(certificateBio, certificatePem.constData(), certificatePem.size());
    const auto x509Certificate = X509Certificate::readCertificate(certificateBio);
    if (!x509Certificate) {
        qCInfo(lcCse()) << "Client certificate is invalid. Could not check it against the server public key";
        return false;
    }

    if (X509_verify(x509Certificate, serverPublicKey) == 0) {
        qCInfo(lcCse()) << "Client certificate is not valid against the server public key";
        return false;
    }

    qCDebug(lcCse()) << "Client certificate is valid against server public key";
    return true;
}

void ClientSideEncryption::publicKeyFetched(Job *incoming)
{
    auto *readJob = static_cast<ReadPasswordJob *>(incoming);
    auto account = readJob->property(accountProperty).value<AccountPtr>();
    Q_ASSERT(account);

    // Error or no valid public key error out
    if (readJob->error() != NoError || readJob->binaryData().length() == 0) {
        getPublicKeyFromServer(account);
        return;
    }

    _certificate = QSslCertificate(readJob->binaryData(), QSsl::Pem);

    if (_certificate.isNull()) {
        getPublicKeyFromServer(account);
        return;
    }

    _publicKey = _certificate.publicKey();

    qCInfo(lcCse()) << "Public key fetched from keychain";

    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_private,
                account->id()
    );

    auto *job = new ReadPasswordJob(Theme::instance()->appName());
    job->setProperty(accountProperty, QVariant::fromValue(account));
    job->setInsecureFallback(false);
    job->setKey(kck);
    connect(job, &ReadPasswordJob::finished, this, &ClientSideEncryption::privateKeyFetched);
    job->start();
}

void ClientSideEncryption::privateKeyFetched(Job *incoming)
{
    auto *readJob = static_cast<ReadPasswordJob *>(incoming);
    auto account = readJob->property(accountProperty).value<AccountPtr>();
    Q_ASSERT(account);

    // Error or no valid public key error out
    if (readJob->error() != NoError || readJob->binaryData().length() == 0) {
        _certificate = QSslCertificate();
        _publicKey = QSslKey();
        getPublicKeyFromServer(account);
        return;
    }

    //_privateKey = QSslKey(readJob->binaryData(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    _privateKey = readJob->binaryData();

    if (_privateKey.isNull()) {
        getPrivateKeyFromServer(account);
        return;
    }

    qCInfo(lcCse()) << "Private key fetched from keychain";

    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_mnemonic,
                account->id()
    );

    auto *job = new ReadPasswordJob(Theme::instance()->appName());
    job->setProperty(accountProperty, QVariant::fromValue(account));
    job->setInsecureFallback(false);
    job->setKey(kck);
    connect(job, &ReadPasswordJob::finished, this, &ClientSideEncryption::mnemonicKeyFetched);
    job->start();
}

void ClientSideEncryption::mnemonicKeyFetched(QKeychain::Job *incoming)
{
    auto *readJob = static_cast<ReadPasswordJob *>(incoming);
    auto account = readJob->property(accountProperty).value<AccountPtr>();
    Q_ASSERT(account);

    // Error or no valid public key error out
    if (readJob->error() != NoError || readJob->textData().length() == 0) {
        _certificate = QSslCertificate();
        _publicKey = QSslKey();
        _privateKey = QByteArray();
        getPublicKeyFromServer(account);
        return;
    }

    _mnemonic = readJob->textData();

    qCInfo(lcCse()) << "Mnemonic key fetched from keychain: " << _mnemonic;

    emit initializationFinished();
}

void ClientSideEncryption::writePrivateKey(const AccountPtr &account)
{
    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_private,
                account->id()
    );

    auto *job = new WritePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    job->setBinaryData(_privateKey);
    connect(job, &WritePasswordJob::finished, [this](Job *incoming) {
        Q_UNUSED(incoming);
        qCInfo(lcCse()) << "Private key stored in keychain";
    });
    job->start();
}

void ClientSideEncryption::writeCertificate(const AccountPtr &account)
{
    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_cert,
                account->id()
    );

    auto *job = new WritePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    job->setBinaryData(_certificate.toPem());
    connect(job, &WritePasswordJob::finished, [this](Job *incoming) {
        Q_UNUSED(incoming);
        qCInfo(lcCse()) << "Certificate stored in keychain";
    });
    job->start();
}

void ClientSideEncryption::writeMnemonic(const AccountPtr &account)
{
    const QString kck = AbstractCredentials::keychainKey(
                account->url().toString(),
                account->credentials()->user() + e2e_mnemonic,
                account->id()
    );

    auto *job = new WritePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    job->setTextData(_mnemonic);
    connect(job, &WritePasswordJob::finished, [](Job *incoming) {
        Q_UNUSED(incoming);
        qCInfo(lcCse()) << "Mnemonic stored in keychain";
    });
    job->start();
}

void ClientSideEncryption::forgetSensitiveData(const AccountPtr &account)
{
    _privateKey = QByteArray();
    _certificate = QSslCertificate();
    _publicKey = QSslKey();
    _mnemonic = QString();

    auto startDeleteJob = [account](QString user) {
        auto *job = new DeletePasswordJob(Theme::instance()->appName());
        job->setInsecureFallback(false);
        job->setKey(AbstractCredentials::keychainKey(account->url().toString(), user, account->id()));
        job->start();
    };

    auto user = account->credentials()->user();
    startDeleteJob(user + e2e_private);
    startDeleteJob(user + e2e_cert);
    startDeleteJob(user + e2e_mnemonic);
}

void ClientSideEncryption::slotRequestMnemonic()
{
    emit showMnemonic(_mnemonic);
}

void ClientSideEncryption::generateKeyPair(const AccountPtr &account)
{
    // AES/GCM/NoPadding,
    // metadataKeys with RSA/ECB/OAEPWithSHA-256AndMGF1Padding
    qCInfo(lcCse()) << "No public key, generating a pair.";
    const int rsaKeyLen = 2048;


    // Init RSA
    PKeyCtx ctx(EVP_PKEY_RSA);

    if(EVP_PKEY_keygen_init(ctx) <= 0) {
        qCInfo(lcCse()) << "Couldn't initialize the key generator";
        return;
    }

    if(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, rsaKeyLen) <= 0) {
        qCInfo(lcCse()) << "Couldn't initialize the key generator bits";
        return;
    }

    auto localKeyPair = PKey::generate(ctx);
    if(!localKeyPair) {
        qCInfo(lcCse()) << "Could not generate the key";
        return;
    }

    qCInfo(lcCse()) << "Key correctly generated";
    qCInfo(lcCse()) << "Storing keys locally";

    Bio privKey;
    if (PEM_write_bio_PrivateKey(privKey, localKeyPair, nullptr, nullptr, 0, nullptr, nullptr) <= 0) {
        qCInfo(lcCse()) << "Could not read private key from bio.";
        return;
    }
    QByteArray key = BIO2ByteArray(privKey);
    //_privateKey = QSslKey(key, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    _privateKey = key;

    qCInfo(lcCse()) << "Keys generated correctly, sending to server.";
    generateCSR(account, localKeyPair);
}

void ClientSideEncryption::generateCSR(const AccountPtr &account, EVP_PKEY *keyPair)
{
    // OpenSSL expects const char.
    auto cnArray = account->davUser().toLocal8Bit();
    qCInfo(lcCse()) << "Getting the following array for the account Id" << cnArray;

    auto certParams = std::map<const char *, const char*>{
      {"C", "DE"},
      {"ST", "Baden-Wuerttemberg"},
      {"L", "Stuttgart"},
      {"O","Nextcloud"},
      {"CN", cnArray.constData()}
    };

    int ret = 0;
    int nVersion = 1;

    // 2. set version of x509 req
    X509_REQ *x509_req = X509_REQ_new();
    auto release_on_exit_x509_req = qScopeGuard([&] {
                X509_REQ_free(x509_req);
            });

    ret = X509_REQ_set_version(x509_req, nVersion);

    // 3. set subject of x509 req
    auto x509_name = X509_REQ_get_subject_name(x509_req);

    for(const auto& v : certParams) {
        ret = X509_NAME_add_entry_by_txt(x509_name, v.first,  MBSTRING_ASC, (const unsigned char*) v.second, -1, -1, 0);
        if (ret != 1) {
            qCInfo(lcCse()) << "Error Generating the Certificate while adding" << v.first << v.second;
            return;
        }
    }

    ret = X509_REQ_set_pubkey(x509_req, keyPair);
    if (ret != 1){
        qCInfo(lcCse()) << "Error setting the public key on the csr";
        return;
    }

    ret = X509_REQ_sign(x509_req, keyPair, EVP_sha1());    // return x509_req->signature->length
    if (ret <= 0){
        qCInfo(lcCse()) << "Error setting the public key on the csr";
        return;
    }

    Bio out;
    ret = PEM_write_bio_X509_REQ(out, x509_req);
    QByteArray output = BIO2ByteArray(out);

    qCInfo(lcCse()) << "Returning the certificate";
    qCInfo(lcCse()) << output;

    auto job = new SignPublicKeyApiJob(account, baseUrl() + "public-key", this);
    job->setCsr(output);

    connect(job, &SignPublicKeyApiJob::jsonReceived, [this, account](const QJsonDocument& json, int retCode) {
        if (retCode == 200) {
            QString cert = json.object().value("ocs").toObject().value("data").toObject().value("public-key").toString();
            _certificate = QSslCertificate(cert.toLocal8Bit(), QSsl::Pem);
            _publicKey = _certificate.publicKey();
            fetchAndValidatePublicKeyFromServer(account);
        }
        qCInfo(lcCse()) << retCode;
    });
    job->start();
}

void ClientSideEncryption::encryptPrivateKey(const AccountPtr &account)
{
    QStringList list = WordList::getRandomWords(12);
    _mnemonic = list.join(' ');
    _newMnemonicGenerated = true;
    qCInfo(lcCse()) << "mnemonic Generated:" << _mnemonic;

    emit mnemonicGenerated(_mnemonic);

    QString passPhrase = list.join(QString()).toLower();
    qCInfo(lcCse()) << "Passphrase Generated:" << passPhrase;

    auto salt = EncryptionHelper::generateRandom(40);
    auto secretKey = EncryptionHelper::generatePassword(passPhrase, salt);
    auto cryptedText = EncryptionHelper::encryptPrivateKey(secretKey, EncryptionHelper::privateKeyToPem(_privateKey), salt);

    // Send private key to the server
    auto job = new StorePrivateKeyApiJob(account, baseUrl() + "private-key", this);
    job->setPrivateKey(cryptedText);
    connect(job, &StorePrivateKeyApiJob::jsonReceived, [this, account](const QJsonDocument& doc, int retCode) {
        Q_UNUSED(doc);
        switch(retCode) {
            case 200:
                qCInfo(lcCse()) << "Private key stored encrypted on server.";
                writePrivateKey(account);
                writeCertificate(account);
                writeMnemonic(account);
                emit initializationFinished();
                break;
            default:
                qCInfo(lcCse()) << "Store private key failed, return code:" << retCode;
        }
    });
    job->start();
}

bool ClientSideEncryption::newMnemonicGenerated() const
{
    return _newMnemonicGenerated;
}

void ClientSideEncryption::decryptPrivateKey(const AccountPtr &account, const QByteArray &key) {
    QString msg = tr("Please enter your end to end encryption passphrase:<br>"
                     "<br>"
                     "User: %2<br>"
                     "Account: %3<br>")
                      .arg(Utility::escape(account->credentials()->user()),
                           Utility::escape(account->displayName()));

    QInputDialog dialog;
    dialog.setWindowTitle(tr("Enter E2E passphrase"));
    dialog.setLabelText(msg);
    dialog.setTextEchoMode(QLineEdit::Normal);

    QString prev;

    while(true) {
        if (!prev.isEmpty()) {
            dialog.setTextValue(prev);
        }
        bool ok = dialog.exec();
        if (ok) {
            qCInfo(lcCse()) << "Got mnemonic:" << dialog.textValue();
            prev = dialog.textValue();

            _mnemonic = prev;
            QString mnemonic = prev.split(" ").join(QString()).toLower();
            qCInfo(lcCse()) << "mnemonic:" << mnemonic;

            // split off salt
            const auto salt = EncryptionHelper::extractPrivateKeySalt(key);

            auto pass = EncryptionHelper::generatePassword(mnemonic, salt);
            qCInfo(lcCse()) << "Generated key:" << pass;

            QByteArray privateKey = EncryptionHelper::decryptPrivateKey(pass, key);
            //_privateKey = QSslKey(privateKey, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
            _privateKey = privateKey;

            qCInfo(lcCse()) << "Private key: " << _privateKey;

            if (!_privateKey.isNull() && checkPublicKeyValidity(account)) {
                writePrivateKey(account);
                writeCertificate(account);
                writeMnemonic(account);
                break;
            }
        } else {
            _mnemonic = QString();
            _privateKey = QByteArray();
            qCInfo(lcCse()) << "Cancelled";
            break;
        }
    }

    emit initializationFinished();
}

void ClientSideEncryption::getPrivateKeyFromServer(const AccountPtr &account)
{
    qCInfo(lcCse()) << "Retrieving private key from server";
    auto job = new JsonApiJob(account, baseUrl() + "private-key", this);
    connect(job, &JsonApiJob::jsonReceived, [this, account](const QJsonDocument& doc, int retCode) {
            if (retCode == 200) {
                QString key = doc.object()["ocs"].toObject()["data"].toObject()["private-key"].toString();
                qCInfo(lcCse()) << key;
                qCInfo(lcCse()) << "Found private key, lets decrypt it!";
                decryptPrivateKey(account, key.toLocal8Bit());
            } else if (retCode == 404) {
                qCInfo(lcCse()) << "No private key on the server: setup is incomplete.";
            } else {
                qCInfo(lcCse()) << "Error while requesting public key: " << retCode;
            }
    });
    job->start();
}

void ClientSideEncryption::getPublicKeyFromServer(const AccountPtr &account)
{
    qCInfo(lcCse()) << "Retrieving public key from server";
    auto job = new JsonApiJob(account, baseUrl() + "public-key", this);
    connect(job, &JsonApiJob::jsonReceived, [this, account](const QJsonDocument& doc, int retCode) {
            if (retCode == 200) {
                QString publicKey = doc.object()["ocs"].toObject()["data"].toObject()["public-keys"].toObject()[account->davUser()].toString();
                _certificate = QSslCertificate(publicKey.toLocal8Bit(), QSsl::Pem);
                _publicKey = _certificate.publicKey();
                qCInfo(lcCse()) << "Found Public key, requesting Server Public Key. Public key:" << publicKey;
                fetchAndValidatePublicKeyFromServer(account);
            } else if (retCode == 404) {
                qCInfo(lcCse()) << "No public key on the server";
                generateKeyPair(account);
            } else {
                qCInfo(lcCse()) << "Error while requesting public key: " << retCode;
            }
    });
    job->start();
}

void ClientSideEncryption::fetchAndValidatePublicKeyFromServer(const AccountPtr &account)
{
    qCInfo(lcCse()) << "Retrieving public key from server";
    auto job = new JsonApiJob(account, baseUrl() + "server-key", this);
    connect(job, &JsonApiJob::jsonReceived, [this, account](const QJsonDocument& doc, int retCode) {
        if (retCode == 200) {
            const auto serverPublicKey = doc.object()["ocs"].toObject()["data"].toObject()["public-key"].toString().toLatin1();
            qCInfo(lcCse()) << "Found Server Public key, checking it. Server public key:" << serverPublicKey;
            if (checkServerPublicKeyValidity(serverPublicKey)) {
                if (_privateKey.isEmpty()) {
                    qCInfo(lcCse()) << "Valid Server Public key, requesting Private Key.";
                    getPrivateKeyFromServer(account);
                } else {
                    qCInfo(lcCse()) << "Certificate saved, Encrypting Private Key.";
                    encryptPrivateKey(account);
                }
            } else {
                qCInfo(lcCse()) << "Error invalid server public key";
                _certificate = QSslCertificate();
                _publicKey = QSslKey();
                _privateKey = QByteArray();
                getPublicKeyFromServer(account);
                return;
            }
        } else {
            qCInfo(lcCse()) << "Error while requesting server public key: " << retCode;
        }
    });
    job->start();
}

FolderMetadata::FolderMetadata(AccountPtr account, const QByteArray& metadata, int statusCode) : _account(account)
{
    if (metadata.isEmpty() || statusCode == 404) {
        qCInfo(lcCseMetadata()) << "Setupping Empty Metadata";
        setupEmptyMetadata();
    } else {
        qCInfo(lcCseMetadata()) << "Setting up existing metadata";
        setupExistingMetadata(metadata);
    }
}

void FolderMetadata::setupExistingMetadata(const QByteArray& metadata)
{
  /* This is the json response from the server, it contains two extra objects that we are *not* interested.
  * ocs and data.
  */
  QJsonDocument doc = QJsonDocument::fromJson(metadata);
  qCInfo(lcCseMetadata()) << doc.toJson(QJsonDocument::Compact);

  // The metadata is being retrieved as a string stored in a json.
  // This *seems* to be broken but the RFC doesn't explicits how it wants.
  // I'm currently unsure if this is error on my side or in the server implementation.
  // And because inside of the meta-data there's an object called metadata, without '-'
  // make it really different.

  QString metaDataStr = doc.object()["ocs"]
                         .toObject()["data"]
                         .toObject()["meta-data"]
                         .toString();

  QJsonDocument metaDataDoc = QJsonDocument::fromJson(metaDataStr.toLocal8Bit());
  QJsonObject metadataObj = metaDataDoc.object()["metadata"].toObject();
  QJsonObject metadataKeys = metadataObj["metadataKeys"].toObject();
  QByteArray sharing = metadataObj["sharing"].toString().toLocal8Bit();
  QJsonObject files = metaDataDoc.object()["files"].toObject();

  QJsonDocument debugHelper;
  debugHelper.setObject(metadataKeys);
  qCDebug(lcCse) << "Keys: " << debugHelper.toJson(QJsonDocument::Compact);

  // Iterate over the document to store the keys. I'm unsure that the keys are in order,
  // perhaps it's better to store a map instead of a vector, perhaps this just doesn't matter.
  for(auto it = metadataKeys.constBegin(), end = metadataKeys.constEnd(); it != end; it++) {
    QByteArray currB64Pass = it.value().toString().toLocal8Bit();
    /*
     * We have to base64 decode the metadatakey here. This was a misunderstanding in the RFC
     * Now we should be compatible with Android and IOS. Maybe we can fix it later.
     */
    QByteArray b64DecryptedKey = decryptMetadataKey(currB64Pass);
    if (b64DecryptedKey.isEmpty()) {
      qCDebug(lcCse()) << "Could not decrypt metadata for key" << it.key();
      continue;
    }

    QByteArray decryptedKey = QByteArray::fromBase64(b64DecryptedKey);
    _metadataKeys.insert(it.key().toInt(), decryptedKey);
  }

  // Cool, We actually have the key, we can decrypt the rest of the metadata.
  qCDebug(lcCse) << "Sharing: " << sharing;
  if (sharing.size()) {
      auto sharingDecrypted = decryptJsonObject(sharing, _metadataKeys.last());
      qCDebug(lcCse) << "Sharing Decrypted" << sharingDecrypted;

      //Sharing is also a JSON object, so extract it and populate.
      auto sharingDoc = QJsonDocument::fromJson(sharingDecrypted);
      auto sharingObj = sharingDoc.object();
      for (auto it = sharingObj.constBegin(), end = sharingObj.constEnd(); it != end; it++) {
        _sharing.push_back({it.key(), it.value().toString()});
      }
  } else {
      qCDebug(lcCse) << "Skipping sharing section since it is empty";
  }

    for (auto it = files.constBegin(), end = files.constEnd(); it != end; it++) {
        EncryptedFile file;
        file.encryptedFilename = it.key();

        auto fileObj = it.value().toObject();
        file.metadataKey = fileObj["metadataKey"].toInt();
        file.authenticationTag = QByteArray::fromBase64(fileObj["authenticationTag"].toString().toLocal8Bit());
        file.initializationVector = QByteArray::fromBase64(fileObj["initializationVector"].toString().toLocal8Bit());

        //Decrypt encrypted part
        QByteArray key = _metadataKeys[file.metadataKey];
        auto encryptedFile = fileObj["encrypted"].toString().toLocal8Bit();
        auto decryptedFile = decryptJsonObject(encryptedFile, key);
        auto decryptedFileDoc = QJsonDocument::fromJson(decryptedFile);
        auto decryptedFileObj = decryptedFileDoc.object();

        file.originalFilename = decryptedFileObj["filename"].toString();
        file.encryptionKey = QByteArray::fromBase64(decryptedFileObj["key"].toString().toLocal8Bit());
        file.mimetype = decryptedFileObj["mimetype"].toString().toLocal8Bit();
        file.fileVersion = decryptedFileObj["version"].toInt();

        // In case we wrongly stored "inode/directory" we try to recover from it
        if (file.mimetype == QByteArrayLiteral("inode/directory")) {
            file.mimetype = QByteArrayLiteral("httpd/unix-directory");
        }

        _files.push_back(file);
    }
}

// RSA/ECB/OAEPWithSHA-256AndMGF1Padding using private / public key.
QByteArray FolderMetadata::encryptMetadataKey(const QByteArray& data) const
{
    Bio publicKeyBio;
    QByteArray publicKeyPem = _account->e2e()->_publicKey.toPem();
    BIO_write(publicKeyBio, publicKeyPem.constData(), publicKeyPem.size());
    auto publicKey = PKey::readPublicKey(publicKeyBio);

    // The metadata key is binary so base64 encode it first
    return EncryptionHelper::encryptStringAsymmetric(publicKey, data.toBase64());
}

QByteArray FolderMetadata::decryptMetadataKey(const QByteArray& encryptedMetadata) const
{
    Bio privateKeyBio;
    QByteArray privateKeyPem = _account->e2e()->_privateKey;
    BIO_write(privateKeyBio, privateKeyPem.constData(), privateKeyPem.size());
    auto key = PKey::readPrivateKey(privateKeyBio);

    // Also base64 decode the result
    QByteArray decryptResult = EncryptionHelper::decryptStringAsymmetric(
                    key, QByteArray::fromBase64(encryptedMetadata));

    if (decryptResult.isEmpty())
    {
      qCDebug(lcCse()) << "ERROR. Could not decrypt the metadata key";
      return {};
    }
    return QByteArray::fromBase64(decryptResult);
}

// AES/GCM/NoPadding (128 bit key size)
QByteArray FolderMetadata::encryptJsonObject(const QByteArray& obj, const QByteArray pass) const
{
    return EncryptionHelper::encryptStringSymmetric(pass, obj);
}

QByteArray FolderMetadata::decryptJsonObject(const QByteArray& encryptedMetadata, const QByteArray& pass) const
{
    return EncryptionHelper::decryptStringSymmetric(pass, encryptedMetadata);
}

void FolderMetadata::setupEmptyMetadata() {
    qCDebug(lcCse) << "Settint up empty metadata";
    QByteArray newMetadataPass = EncryptionHelper::generateRandom(16);
    _metadataKeys.insert(0, newMetadataPass);

    QString publicKey = _account->e2e()->_publicKey.toPem().toBase64();
    QString displayName = _account->displayName();

    _sharing.append({displayName, publicKey});
}

QByteArray FolderMetadata::encryptedMetadata() {
    qCDebug(lcCse) << "Generating metadata";

    QJsonObject metadataKeys;
    for (auto it = _metadataKeys.constBegin(), end = _metadataKeys.constEnd(); it != end; it++) {
        /*
         * We have to already base64 encode the metadatakey here. This was a misunderstanding in the RFC
         * Now we should be compatible with Android and IOS. Maybe we can fix it later.
         */
        const QByteArray encryptedKey = encryptMetadataKey(it.value().toBase64());
        metadataKeys.insert(QString::number(it.key()), QString(encryptedKey));
    }

    /* NO SHARING IN V1
    QJsonObject recepients;
    for (auto it = _sharing.constBegin(), end = _sharing.constEnd(); it != end; it++) {
        recepients.insert(it->first, it->second);
    }
    QJsonDocument recepientDoc;
    recepientDoc.setObject(recepients);
    QString sharingEncrypted = encryptJsonObject(recepientDoc.toJson(QJsonDocument::Compact), _metadataKeys.last());
    */

    QJsonObject metadata = {
      {"metadataKeys", metadataKeys},
      // {"sharing", sharingEncrypted},
      {"version", 1}
    };

    QJsonObject files;
    for (auto it = _files.constBegin(), end = _files.constEnd(); it != end; it++) {
        QJsonObject encrypted;
        encrypted.insert("key", QString(it->encryptionKey.toBase64()));
        encrypted.insert("filename", it->originalFilename);
        encrypted.insert("mimetype", QString(it->mimetype));
        encrypted.insert("version", it->fileVersion);
        QJsonDocument encryptedDoc;
        encryptedDoc.setObject(encrypted);

        QString encryptedEncrypted = encryptJsonObject(encryptedDoc.toJson(QJsonDocument::Compact), _metadataKeys.last());
        if (encryptedEncrypted.isEmpty()) {
          qCDebug(lcCse) << "Metadata generation failed!";
        }

        QJsonObject file;
        file.insert("encrypted", encryptedEncrypted);
        file.insert("initializationVector", QString(it->initializationVector.toBase64()));
        file.insert("authenticationTag", QString(it->authenticationTag.toBase64()));
        file.insert("metadataKey", _metadataKeys.lastKey());

        files.insert(it->encryptedFilename, file);
    }

    QJsonObject metaObject = {
      {"metadata", metadata},
      {"files", files}
    };

    QJsonDocument internalMetadata;
    internalMetadata.setObject(metaObject);
    return internalMetadata.toJson();
}

void FolderMetadata::addEncryptedFile(const EncryptedFile &f) {

    for (int i = 0; i < _files.size(); i++) {
        if (_files.at(i).originalFilename == f.originalFilename) {
            _files.removeAt(i);
            break;
        }
    }

    _files.append(f);
}

void FolderMetadata::removeEncryptedFile(const EncryptedFile &f)
{
    for (int i = 0; i < _files.size(); i++) {
        if (_files.at(i).originalFilename == f.originalFilename) {
            _files.removeAt(i);
            break;
        }
    }
}

void FolderMetadata::removeAllEncryptedFiles()
{
    _files.clear();
}

QVector<EncryptedFile> FolderMetadata::files() const {
    return _files;
}

bool EncryptionHelper::fileEncryption(const QByteArray &key, const QByteArray &iv, QFile *input, QFile *output, QByteArray& returnTag)
{
    if (!input->open(QIODevice::ReadOnly)) {
      qCDebug(lcCse) << "Could not open input file for reading" << input->errorString();
    }
    if (!output->open(QIODevice::WriteOnly)) {
      qCDebug(lcCse) << "Could not oppen output file for writing" << output->errorString();
    }

    // Init
    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Could not create context";
        return false;
    }

    /* Initialise the decryption operation. */
    if(!EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Could not init cipher";
        return false;
    }

    EVP_CIPHER_CTX_set_padding(ctx, 0);

    /* Set IV length. */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Could not set iv length";
        return false;
    }

    /* Initialise key and IV */
    if(!EVP_EncryptInit_ex(ctx, nullptr, nullptr, (const unsigned char *)key.constData(), (const unsigned char *)iv.constData())) {
        qCInfo(lcCse()) << "Could not set key and iv";
        return false;
    }

    QByteArray out(blockSize + OCC::CommonConstants::e2EeTagSize - 1, '\0');
    int len = 0;
    int total_len = 0;

    qCDebug(lcCse) << "Starting to encrypt the file" << input->fileName() << input->atEnd();
    while(!input->atEnd()) {
        QByteArray data = input->read(blockSize);

        if (data.size() == 0) {
            qCInfo(lcCse()) << "Could not read data from file";
            return false;
        }

        qCDebug(lcCse) << "Encrypting " << data;
        if(!EVP_EncryptUpdate(ctx, unsignedData(out), &len, (unsigned char *)data.constData(), data.size())) {
            qCInfo(lcCse()) << "Could not encrypt";
            return false;
        }

        output->write(out, len);
        total_len += len;
    }

    if(1 != EVP_EncryptFinal_ex(ctx, unsignedData(out), &len)) {
        qCInfo(lcCse()) << "Could finalize encryption";
        return false;
    }
    output->write(out, len);
    total_len += len;

    /* Get the tag */
    QByteArray tag(OCC::CommonConstants::e2EeTagSize, '\0');
    if(1 != EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, OCC::CommonConstants::e2EeTagSize, unsignedData(tag))) {
        qCInfo(lcCse()) << "Could not get tag";
        return false;
    }

    returnTag = tag;
    output->write(tag, OCC::CommonConstants::e2EeTagSize);

    input->close();
    output->close();
    qCDebug(lcCse) << "File Encrypted Successfully";
    return true;
}

bool EncryptionHelper::fileDecryption(const QByteArray &key, const QByteArray& iv,
                               QFile *input, QFile *output)
{
    input->open(QIODevice::ReadOnly);
    output->open(QIODevice::WriteOnly);

    // Init
    CipherCtx ctx;

    /* Create and initialise the context */
    if(!ctx) {
        qCInfo(lcCse()) << "Could not create context";
        return false;
    }

    /* Initialise the decryption operation. */
    if(!EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr)) {
        qCInfo(lcCse()) << "Could not init cipher";
        return false;
    }

    EVP_CIPHER_CTX_set_padding(ctx, 0);

    /* Set IV length. */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,  iv.size(), nullptr)) {
        qCInfo(lcCse()) << "Could not set iv length";
        return false;
    }

    /* Initialise key and IV */
    if(!EVP_DecryptInit_ex(ctx, nullptr, nullptr, (const unsigned char *) key.constData(), (const unsigned char *) iv.constData())) {
        qCInfo(lcCse()) << "Could not set key and iv";
        return false;
    }

    qint64 size = input->size() - OCC::CommonConstants::e2EeTagSize;

    QByteArray out(blockSize + OCC::CommonConstants::e2EeTagSize - 1, '\0');
    int len = 0;

    while(input->pos() < size) {

        auto toRead = size - input->pos();
        if (toRead > blockSize) {
            toRead = blockSize;
        }

        QByteArray data = input->read(toRead);

        if (data.size() == 0) {
            qCInfo(lcCse()) << "Could not read data from file";
            return false;
        }

        if(!EVP_DecryptUpdate(ctx, unsignedData(out), &len, (unsigned char *)data.constData(), data.size())) {
            qCInfo(lcCse()) << "Could not decrypt";
            return false;
        }

        output->write(out, len);
    }

    QByteArray tag = input->read(OCC::CommonConstants::e2EeTagSize);

    /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
    if(!EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), (unsigned char *)tag.constData())) {
        qCInfo(lcCse()) << "Could not set expected tag";
        return false;
    }

    if(1 != EVP_DecryptFinal_ex(ctx, unsignedData(out), &len)) {
        qCInfo(lcCse()) << "Could finalize decryption";
        return false;
    }
    output->write(out, len);

    input->close();
    output->close();
    return true;
}

EncryptionHelper::StreamingDecryptor::StreamingDecryptor(const QByteArray &key, const QByteArray &iv, quint64 totalSize) : _totalSize(totalSize)
{
    if (_ctx && !key.isEmpty() && !iv.isEmpty() && totalSize > 0) {
        _isInitialized = true;

        /* Initialize the decryption operation. */
        if(!EVP_DecryptInit_ex(_ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr)) {
            qCritical(lcCse()) << "Could not init cipher";
            _isInitialized = false;
        }

        EVP_CIPHER_CTX_set_padding(_ctx, 0);

        /* Set IV length. */
        if(!EVP_CIPHER_CTX_ctrl(_ctx, EVP_CTRL_GCM_SET_IVLEN, iv.size(), nullptr)) {
            qCritical(lcCse()) << "Could not set iv length";
            _isInitialized = false;
        }

        /* Initialize key and IV */
        if(!EVP_DecryptInit_ex(_ctx, nullptr, nullptr, reinterpret_cast<const unsigned char*>(key.constData()), reinterpret_cast<const unsigned char*>(iv.constData()))) {
            qCritical(lcCse()) << "Could not set key and iv";
            _isInitialized = false;
        }
    }
}

qint32 EncryptionHelper::StreamingDecryptor::chunkDecryption(const char *input, QIODevice *output, quint32 chunkSize)
{
    Q_ASSERT(isInitialized());
    if (!isInitialized()) {
        qCritical(lcCse()) << "Decryption failed. Decryptor is not initialized!";
        return -1;
    }

    Q_ASSERT(output && output->isOpen() && output->isWritable());
    if (!output || !output->isOpen() || !output->isWritable()) {
        qCritical(lcCse()) << "Decryption failed. Incorrect output device!";
        return -1;
    }

    Q_ASSERT(input);
    if (!input) {
        qCritical(lcCse()) << "Decryption failed. Incorrect input!";
        return -1;
    }

    Q_ASSERT(chunkSize > 0);
    if (chunkSize <= 0) {
        qCritical(lcCse()) << "Decryption failed. Incorrect chunkSize!";
        return -1;
    }

    if (_decryptedSoFar == 0) {
        qCDebug(lcCse()) << "Decryption started";
    }

    Q_ASSERT(_decryptedSoFar + chunkSize <= _totalSize);
    if (_decryptedSoFar + chunkSize > _totalSize) {
        qCritical(lcCse()) << "Decryption failed. Chunk is out of range!";
        return -1;
    }

    const bool isLastChunk = _decryptedSoFar + chunkSize == _totalSize;

    // last OCC::CommonConstants::e2EeTagSize bytes is ALWAYS a tag!!!
    const qint32 size = isLastChunk ? static_cast<qint32>(chunkSize) - OCC::CommonConstants::e2EeTagSize : static_cast<qint32>(chunkSize);

    Q_ASSERT(size > 0);
    if (size <= 0) {
        qCritical(lcCse()) << "Decryption failed. Invalid input size: " << size << " !";
        return -1;
    }

    int bytesWritten = 0;

    QByteArray decryptedBlock(blockSize + OCC::CommonConstants::e2EeTagSize - 1, '\0');
    int inputPos = 0;

    while(inputPos < size) {
        // read blockSize or less bytes
        const QByteArray encryptedBlock = QByteArray(input + inputPos, qMin(size - inputPos, blockSize));

        if (encryptedBlock.size() == 0) {
            qCritical(lcCse()) << "Could not read data from the input buffer.";
            return -1;
        }

        int outLen = 0;

        if(!EVP_DecryptUpdate(_ctx, unsignedData(decryptedBlock), &outLen, reinterpret_cast<const unsigned char*>(encryptedBlock.data()), encryptedBlock.size())) {
            qCritical(lcCse()) << "Could not decrypt";
            return -1;
        }

        const auto writtenToOutput = output->write(decryptedBlock, outLen);

        Q_ASSERT(writtenToOutput == outLen);
        if (writtenToOutput != outLen) {
            qCritical(lcCse()) << "Failed to write decrypted data to device.";
            return -1;
        }

        bytesWritten += writtenToOutput;

        // advance input position for further read
        inputPos += encryptedBlock.size();

        _decryptedSoFar += encryptedBlock.size();
    }

    if (isLastChunk) {
        // if it's a last chunk, we'd need to read a tag at the end and finalize the decryption

        Q_ASSERT(chunkSize - inputPos == OCC::CommonConstants::e2EeTagSize);
        if (chunkSize - inputPos != OCC::CommonConstants::e2EeTagSize) {
            qCritical(lcCse()) << "Decryption failed. E2EE tag is missing!";
            return -1;
        }

        int outLen = 0;

        QByteArray tag = QByteArray(input + inputPos, OCC::CommonConstants::e2EeTagSize);

        /* Set expected tag value. Works in OpenSSL 1.0.1d and later */
        if(!EVP_CIPHER_CTX_ctrl(_ctx, EVP_CTRL_GCM_SET_TAG, tag.size(), reinterpret_cast<unsigned char*>(tag.data()))) {
            qCritical(lcCse()) << "Could not set expected tag";
            return -1;
        }

        if(1 != EVP_DecryptFinal_ex(_ctx, unsignedData(decryptedBlock), &outLen)) {
            qCritical(lcCse()) << "Could finalize decryption";
            return -1;
        }

        const auto writtenToOutput = output->write(decryptedBlock, outLen);

        Q_ASSERT(writtenToOutput == outLen);
        if (writtenToOutput != outLen) {
            qCritical(lcCse()) << "Failed to write decrypted data to device.";
            return -1;
        }

        bytesWritten += writtenToOutput;

        _decryptedSoFar += OCC::CommonConstants::e2EeTagSize;

        _isFinished = true;
    }

    // qCDebug(lcCse()) <<"Decrypting:" << _decryptedSoFar << "/" << _totalSize;

    if (isFinished()) {
        qCDebug(lcCse()) << "Decryption complete";
    }

    return bytesWritten;
}

bool EncryptionHelper::StreamingDecryptor::isInitialized() const
{
    return _isInitialized;
}

bool EncryptionHelper::StreamingDecryptor::isFinished() const
{
    return _isFinished;
}
};
