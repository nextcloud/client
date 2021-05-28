/*
   This software is in the public domain, furnished "as is", without technical
   support, and with no warranty, express or implied, as to its usefulness for
   any purpose.
*/

#include <QtTest>

#include <QTemporaryFile>
#include <QRandomGenerator>

#include "clientsideencryption.h"

using namespace OCC;

class TestClientSideEncryption : public QObject
{
    Q_OBJECT

    QByteArray convertToOldStorageFormat(const QByteArray &data)
    {
        return data.split('|').join("fA==");
    }

private slots:
    void shouldEncryptPrivateKeys()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto privateKey = QByteArrayLiteral("bar");
        const auto originalSalt = QByteArrayLiteral("baz");

        // WHEN
        const auto cipher = EncryptionHelper::encryptPrivateKey(encryptionKey, privateKey, originalSalt);

        // THEN
        const auto parts = cipher.split('|');
        QCOMPARE(parts.size(), 3);

        const auto encryptedKey = QByteArray::fromBase64(parts[0]);
        const auto iv = QByteArray::fromBase64(parts[1]);
        const auto salt = QByteArray::fromBase64(parts[2]);

        // We're not here to check the merits of the encryption but at least make sure it's been
        // somewhat ciphered
        QVERIFY(!encryptedKey.isEmpty());
        QVERIFY(encryptedKey != privateKey);

        QVERIFY(!iv.isEmpty());
        QCOMPARE(salt, originalSalt);
    }

    void shouldDecryptPrivateKeys()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto originalPrivateKey = QByteArrayLiteral("bar");
        const auto originalSalt = QByteArrayLiteral("baz");
        const auto cipher = EncryptionHelper::encryptPrivateKey(encryptionKey, originalPrivateKey, originalSalt);

        // WHEN
        const auto privateKey = EncryptionHelper::decryptPrivateKey(encryptionKey, cipher);
        const auto salt = EncryptionHelper::extractPrivateKeySalt(cipher);

        // THEN
        QCOMPARE(privateKey, originalPrivateKey);
        QCOMPARE(salt, originalSalt);
    }

    void shouldDecryptPrivateKeysInOldStorageFormat()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto originalPrivateKey = QByteArrayLiteral("bar");
        const auto originalSalt = QByteArrayLiteral("baz");
        const auto cipher = convertToOldStorageFormat(EncryptionHelper::encryptPrivateKey(encryptionKey, originalPrivateKey, originalSalt));

        // WHEN
        const auto privateKey = EncryptionHelper::decryptPrivateKey(encryptionKey, cipher);
        const auto salt = EncryptionHelper::extractPrivateKeySalt(cipher);

        // THEN
        QCOMPARE(privateKey, originalPrivateKey);
        QCOMPARE(salt, originalSalt);
    }

    void shouldSymmetricEncryptStrings()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto data = QByteArrayLiteral("bar");

        // WHEN
        const auto cipher = EncryptionHelper::encryptStringSymmetric(encryptionKey, data);

        // THEN
        const auto parts = cipher.split('|');
        QCOMPARE(parts.size(), 2);

        const auto encryptedData = QByteArray::fromBase64(parts[0]);
        const auto iv = QByteArray::fromBase64(parts[1]);

        // We're not here to check the merits of the encryption but at least make sure it's been
        // somewhat ciphered
        QVERIFY(!encryptedData.isEmpty());
        QVERIFY(encryptedData != data);

        QVERIFY(!iv.isEmpty());
    }

    void shouldSymmetricDecryptStrings()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto originalData = QByteArrayLiteral("bar");
        const auto cipher = EncryptionHelper::encryptStringSymmetric(encryptionKey, originalData);

        // WHEN
        const auto data = EncryptionHelper::decryptStringSymmetric(encryptionKey, cipher);

        // THEN
        QCOMPARE(data, originalData);
    }

    void shouldSymmetricDecryptStringsInOldStorageFormat()
    {
        // GIVEN
        const auto encryptionKey = QByteArrayLiteral("foo");
        const auto originalData = QByteArrayLiteral("bar");
        const auto cipher = convertToOldStorageFormat(EncryptionHelper::encryptStringSymmetric(encryptionKey, originalData));

        // WHEN
        const auto data = EncryptionHelper::decryptStringSymmetric(encryptionKey, cipher);

        // THEN
        QCOMPARE(data, originalData);
    }

    void testStreamingDecryptor()
    {
        QTemporaryFile dummyInputFile;

        QVERIFY(dummyInputFile.open());

        const auto dummyFileRandomContents = EncryptionHelper::generateRandom(272);

        QCOMPARE(dummyInputFile.write(dummyFileRandomContents), dummyFileRandomContents.size());

        const auto generateHash = [](const QByteArray &data) {
            QCryptographicHash hash(QCryptographicHash::Sha1);
            hash.addData(data);
            return hash.result();
        };

        const QByteArray originalFileHash = generateHash(dummyFileRandomContents);

        QVERIFY(!originalFileHash.isEmpty());

        dummyInputFile.close();
        QVERIFY(!dummyInputFile.isOpen());

        const auto encryptionKey = EncryptionHelper::generateRandom(16);
        const auto initializationVector = EncryptionHelper::generateRandom(16);

        // test normal file encryption/decryption
        QTemporaryFile dummyEncryptionOutputFile;

        QByteArray tag;

        QVERIFY(EncryptionHelper::fileEncryption(encryptionKey, initializationVector, &dummyInputFile, &dummyEncryptionOutputFile, tag));
        dummyInputFile.close();
        QVERIFY(!dummyInputFile.isOpen());

        dummyEncryptionOutputFile.close();
        QVERIFY(!dummyEncryptionOutputFile.isOpen());

        QTemporaryFile dummyDecryptionOutputFile;

        QVERIFY(EncryptionHelper::fileDecryption(encryptionKey, initializationVector, &dummyEncryptionOutputFile, &dummyDecryptionOutputFile));
        QVERIFY(dummyDecryptionOutputFile.open());
        const auto dummyDecryptionOutputFileHash = generateHash(dummyDecryptionOutputFile.readAll());
        QCOMPARE(dummyDecryptionOutputFileHash, originalFileHash);

        // test streaming decryptor
        EncryptionHelper::StreamingDecryptor streamingDecryptor(encryptionKey, initializationVector, dummyEncryptionOutputFile.size());
        QVERIFY(streamingDecryptor.isInitialized());

        QBuffer chunkedOutputDecrypted;
        QVERIFY(chunkedOutputDecrypted.open(QBuffer::WriteOnly));

        QVERIFY(dummyEncryptionOutputFile.open());

        const auto readBytesAvailable = dummyEncryptionOutputFile.bytesAvailable();
        while (dummyEncryptionOutputFile.pos() < readBytesAvailable) {
            // auto toRead = QRandomGenerator::global()->bounded(8, 128);
            // decryption is going to fail if last chunk does not include or does not equal to 16-bytes tag (accumulation required? - but where? in the caller's code?)
            auto toRead = 64;
            if (dummyEncryptionOutputFile.pos() + toRead > readBytesAvailable) {
                toRead = readBytesAvailable - dummyEncryptionOutputFile.pos();
            }
            const auto decryptedBytes = streamingDecryptor.chunkDecryption(dummyEncryptionOutputFile.read(toRead).constData(), &chunkedOutputDecrypted, toRead);
            QVERIFY(decryptedBytes != -1);
            QVERIFY(decryptedBytes == toRead || streamingDecryptor.isFinished());
        }
        chunkedOutputDecrypted.close();

        QVERIFY(chunkedOutputDecrypted.open(QBuffer::ReadOnly));
        QCOMPARE(generateHash(chunkedOutputDecrypted.readAll()), originalFileHash);
        chunkedOutputDecrypted.close();
    }
};

QTEST_APPLESS_MAIN(TestClientSideEncryption)
#include "testclientsideencryption.moc"
