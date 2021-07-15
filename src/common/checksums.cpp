/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "config.h"
#include "filesystembase.h"
#include "common/checksums.h"
#include "asserts.h"

#include <QLoggingCategory>
#include <qtconcurrentrun.h>
#include <QCryptographicHash>

#ifdef ZLIB_FOUND
#include <zlib.h>
#endif

/** \file checksums.cpp
 *
 * \brief Computing and validating file checksums
 *
 * Overview
 * --------
 *
 * Checksums are used in two distinct ways during synchronization:
 *
 * - to guard uploads and downloads against data corruption
 *   (transmission checksum)
 * - to quickly check whether the content of a file has changed
 *   to avoid redundant uploads (content checksum)
 *
 * In principle both are independent and different checksumming
 * algorithms can be used. To avoid redundant computations, it can
 * make sense to use the same checksum algorithm though.
 *
 * Transmission Checksums
 * ----------------------
 *
 * The usage of transmission checksums is currently optional and needs
 * to be explicitly enabled by adding 'transmissionChecksum=TYPE' to
 * the '[General]' section of the config file.
 *
 * When enabled, the checksum will be calculated on upload and sent to
 * the server in the OC-Checksum header with the format 'TYPE:CHECKSUM'.
 *
 * On download, the header with the same name is read and if the
 * received data does not have the expected checksum, the download is
 * rejected.
 *
 * Transmission checksums guard a specific sync action and are not stored
 * in the database.
 *
 * Content Checksums
 * -----------------
 *
 * Sometimes the metadata of a local file changes while the content stays
 * unchanged. Content checksums allow the sync client to avoid uploading
 * the same data again by comparing the file's actual checksum to the
 * checksum stored in the database.
 *
 * Content checksums are not sent to the server.
 *
 * Checksum Algorithms
 * -------------------
 *
 * - Adler32 (requires zlib)
 * - MD5
 * - SHA1
 * - SHA256
 * - SHA3-256 (requires Qt 5.9)
 *
 */

namespace OCC {

Q_LOGGING_CATEGORY(lcChecksums, "nextcloud.sync.checksums", QtInfoMsg)

#define BUFSIZE qint64(500 * 1024) // 500 KiB

static QByteArray calcCryptoHash(QIODevice *device, QCryptographicHash::Algorithm algo)
{
     QByteArray arr;
     QCryptographicHash crypto( algo );

     if (crypto.addData(device)) {
         arr = crypto.result().toHex();
     }
     return arr;
 }

QByteArray calcMd5(QIODevice *device)
{
    return calcCryptoHash(device, QCryptographicHash::Md5);
}

QByteArray calcSha1(QIODevice *device)
{
    return calcCryptoHash(device, QCryptographicHash::Sha1);
}

#ifdef ZLIB_FOUND
QByteArray calcAdler32(QIODevice *device)
{
    if (device->size() == 0)
    {
        return QByteArray();
    }
    QByteArray buf(BUFSIZE, Qt::Uninitialized);

    unsigned int adler = adler32(0L, Z_NULL, 0);
    qint64 size = 0;
    while (!device->atEnd()) {
        size = device->read(buf.data(), BUFSIZE);
        if (size > 0)
            adler = adler32(adler, (const Bytef *)buf.data(), size);
    }

    return QByteArray::number(adler, 16);
}
#endif

QByteArray makeChecksumHeader(const QByteArray &checksumType, const QByteArray &checksum)
{
    if (checksumType.isEmpty() || checksum.isEmpty())
        return QByteArray();
    QByteArray header = checksumType;
    header.append(':');
    header.append(checksum);
    return header;
}

QByteArray findBestChecksum(const QByteArray &_checksums)
{
    const auto checksums = QString::fromUtf8(_checksums);
    int i = 0;
    // The order of the searches here defines the preference ordering.
    if (-1 != (i = checksums.indexOf(QLatin1String("SHA3-256:"), 0, Qt::CaseInsensitive))
        || -1 != (i = checksums.indexOf(QLatin1String("SHA256:"), 0, Qt::CaseInsensitive))
        || -1 != (i = checksums.indexOf(QLatin1String("SHA1:"), 0, Qt::CaseInsensitive))
        || -1 != (i = checksums.indexOf(QLatin1String("MD5:"), 0, Qt::CaseInsensitive))
        || -1 != (i = checksums.indexOf(QLatin1String("ADLER32:"), 0, Qt::CaseInsensitive))) {
        // Now i is the start of the best checksum
        // Grab it until the next space or end of string.
        return _checksums.mid(i, _checksums.indexOf(' ', i) - i);
    }
    qCWarning(lcChecksums) << "Failed to parse" << _checksums;
    return QByteArray();
}

bool parseChecksumHeader(const QByteArray &header, QByteArray *type, QByteArray *checksum)
{
    if (header.isEmpty()) {
        type->clear();
        checksum->clear();
        return true;
    }

    const auto idx = header.indexOf(':');
    if (idx < 0) {
        return false;
    }

    *type = header.left(idx);
    *checksum = header.mid(idx + 1);
    return true;
}


QByteArray parseChecksumHeaderType(const QByteArray &header)
{
    const auto idx = header.indexOf(':');
    if (idx < 0) {
        return QByteArray();
    }
    return header.left(idx);
}

bool uploadChecksumEnabled()
{
    static bool enabled = qEnvironmentVariableIsEmpty("OWNCLOUD_DISABLE_CHECKSUM_UPLOAD");
    return enabled;
}

static bool checksumComputationEnabled()
{
    static bool enabled = qEnvironmentVariableIsEmpty("OWNCLOUD_DISABLE_CHECKSUM_COMPUTATIONS");
    return enabled;
}

ComputeChecksum::ComputeChecksum(QObject *parent)
    : QObject(parent)
{
}

ComputeChecksum::~ComputeChecksum() = default;

void ComputeChecksum::setChecksumType(const QByteArray &type)
{
    _checksumType = type;
}

QByteArray ComputeChecksum::checksumType() const
{
    return _checksumType;
}

void ComputeChecksum::start(const QString &filePath)
{
    qCInfo(lcChecksums) << "Computing" << checksumType() << "checksum of" << filePath << "in a thread";
    startImpl(std::make_unique<QFile>(filePath));
}

void ComputeChecksum::start(std::unique_ptr<QIODevice> device)
{
    ENFORCE(device);
    qCInfo(lcChecksums) << "Computing" << checksumType() << "checksum of device" << device.get() << "in a thread";
    ASSERT(!device->parent());

    startImpl(std::move(device));
}

void ComputeChecksum::startImpl(std::unique_ptr<QIODevice> device)
{
    connect(&_watcher, &QFutureWatcherBase::finished,
        this, &ComputeChecksum::slotCalculationDone,
        Qt::UniqueConnection);

    // We'd prefer to move the unique_ptr into the lambda, but that's
    // awkward with the C++ standard we're on
    auto sharedDevice = QSharedPointer<QIODevice>(device.release());

    // Bug: The thread will keep running even if ComputeChecksum is deleted.
    auto type = checksumType();
    _watcher.setFuture(QtConcurrent::run([sharedDevice, type]() {
        if (!sharedDevice->open(QIODevice::ReadOnly)) {
            if (auto file = qobject_cast<QFile *>(sharedDevice.data())) {
                qCWarning(lcChecksums) << "Could not open file" << file->fileName()
                        << "for reading to compute a checksum" << file->errorString();
            } else {
                qCWarning(lcChecksums) << "Could not open device" << sharedDevice.data()
                        << "for reading to compute a checksum" << sharedDevice->errorString();
            }
            return QByteArray();
        }
        auto result = ComputeChecksum::computeNow(sharedDevice.data(), type);
        sharedDevice->close();
        return result;
    }));
}

QByteArray ComputeChecksum::computeNowOnFile(const QString &filePath, const QByteArray &checksumType)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(lcChecksums) << "Could not open file" << filePath << "for reading and computing checksum" << file.errorString();
        return QByteArray();
    }

    return computeNow(&file, checksumType);
}

QByteArray ComputeChecksum::computeNow(QIODevice *device, const QByteArray &checksumType)
{
    if (!checksumComputationEnabled()) {
        qCWarning(lcChecksums) << "Checksum computation disabled by environment variable";
        return QByteArray();
    }

    if (checksumType == checkSumMD5C) {
        return calcMd5(device);
    } else if (checksumType == checkSumSHA1C) {
        return calcSha1(device);
    } else if (checksumType == checkSumSHA2C) {
        return calcCryptoHash(device, QCryptographicHash::Sha256);
    }
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    else if (checksumType == checkSumSHA3C) {
        return calcCryptoHash(device, QCryptographicHash::Sha3_256);
    }
#endif
#ifdef ZLIB_FOUND
    else if (checksumType == checkSumAdlerC) {
        return calcAdler32(device);
    }
#endif
    // for an unknown checksum or no checksum, we're done right now
    if (!checksumType.isEmpty()) {
        qCWarning(lcChecksums) << "Unknown checksum type:" << checksumType;
    }
    return QByteArray();
}

void ComputeChecksum::slotCalculationDone()
{
    QByteArray checksum = _watcher.future().result();
    if (!checksum.isNull()) {
        emit done(_checksumType, checksum);
    } else {
        emit done(QByteArray(), QByteArray());
    }
}


ValidateChecksumHeader::ValidateChecksumHeader(QObject *parent)
    : QObject(parent)
{
}

ComputeChecksum *ValidateChecksumHeader::prepareStart(const QByteArray &checksumHeader)
{
    // If the incoming header is empty no validation can happen. Just continue.
    if (checksumHeader.isEmpty()) {
        emit validated(QByteArray(), QByteArray());
        return nullptr;
    }

    if (!parseChecksumHeader(checksumHeader, &_expectedChecksumType, &_expectedChecksum)) {
        qCWarning(lcChecksums) << "Checksum header malformed:" << checksumHeader;
        emit validationFailed(tr("The checksum header is malformed."), {}, {}, _filePath);
        return nullptr;
    }

    auto calculator = new ComputeChecksum(this);
    calculator->setChecksumType(_expectedChecksumType);
    connect(calculator, &ComputeChecksum::done,
        this, &ValidateChecksumHeader::slotChecksumCalculated);
    return calculator;
}

void ValidateChecksumHeader::start(const QString &filePath, const QByteArray &checksumHeader)
{
    _filePath = filePath;
    if (auto calculator = prepareStart(checksumHeader))
        calculator->start(filePath);
}

void ValidateChecksumHeader::start(std::unique_ptr<QIODevice> device, const QByteArray &checksumHeader)
{
    if (auto calculator = prepareStart(checksumHeader))
        calculator->start(std::move(device));
}

void ValidateChecksumHeader::slotChecksumCalculated(const QByteArray &checksumType,
    const QByteArray &checksum)
{
    if (checksumType != _expectedChecksumType) {
        emit validationFailed(tr("The checksum header contained an unknown checksum type '%1'").arg(QString::fromLatin1(_expectedChecksumType)), {}, {}, _filePath);
        return;
    }
    if (checksum != _expectedChecksum) {
        emit validationFailed(tr("The downloaded file does not match the checksum, it will be resumed. '%1' != '%2'").arg(QString::fromUtf8(_expectedChecksum), QString::fromUtf8(checksum)), checksumType, checksum, _filePath);
        return;
    }
    emit validated(checksumType, checksum);
}

CSyncChecksumHook::CSyncChecksumHook() = default;

QByteArray CSyncChecksumHook::hook(const QByteArray &path, const QByteArray &otherChecksumHeader, void * /*this_obj*/)
{
    QByteArray type = parseChecksumHeaderType(QByteArray(otherChecksumHeader));
    if (type.isEmpty())
        return nullptr;

    qCInfo(lcChecksums) << "Computing" << type << "checksum of" << path << "in the csync hook";
    QByteArray checksum = ComputeChecksum::computeNowOnFile(QString::fromUtf8(path), type);
    if (checksum.isNull()) {
        qCWarning(lcChecksums) << "Failed to compute checksum" << type << "for" << path;
        return nullptr;
    }

    return makeChecksumHeader(type, checksum);
}

}
