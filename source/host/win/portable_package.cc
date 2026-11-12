//
// Aspia Project
// Copyright (C) 2016-2026 Dmitry Chapyshev
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "host/win/portable_package.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <qt_windows.h>

#include "base/logging.h"
#include "base/process_util.h"
#include "host/database.h"

namespace {

// Name of the resource the settings are embedded under. Written by the export of a Quick Support
// package, read by the portable run of the same executable.
const wchar_t kResourceName[] = L"ASPIA_QS_SETTINGS";
const WORD kResourceLanguage = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);

// The 32-bit build installed next to the 64-bit one: the package exported by a 64-bit host is made
// of it, so that it runs on a 32-bit Windows as well.
const char kHostX86FileName[] = "aspia_host_x86.exe";

const char kVersion[] = "version";
const char kRouterAddress[] = "router_address";
const char kRouterPublicKey[] = "router_public_key";
const char kUsers[] = "users";

const int kSettingsVersion = 1;
const qsizetype kRouterPublicKeySize = 32;

//--------------------------------------------------------------------------------------------------
// Returns the path of the 32-bit build installed next to this executable, or an empty string when
// this process is the 32-bit build itself.
QString hostX86Path()
{
#if defined(Q_PROCESSOR_X86_64)
    const QString executable_path = ProcessUtil::filePath(ProcessUtil::currentProcessId());
    return QFileInfo(executable_path).absolutePath() + "/" + QLatin1String(kHostX86FileName);
#else
    return QString();
#endif
}

//--------------------------------------------------------------------------------------------------
QByteArray serializeUsers(const QVector<User>& users)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);

    stream << static_cast<quint32>(users.size());

    for (const User& user : users)
    {
        stream << user.entry_id << user.name << user.group << user.salt << user.verifier
               << user.sessions << user.flags;
    }

    return data;
}

//--------------------------------------------------------------------------------------------------
QVector<User> deserializeUsers(const QByteArray& data)
{
    QVector<User> users;

    QDataStream stream(data);
    stream.setVersion(QDataStream::Qt_6_0);

    quint32 count = 0;
    stream >> count;

    // The count comes from the package: a damaged value must not make the loop allocate forever.
    if (stream.status() != QDataStream::Ok || count > 10000)
        return users;

    users.reserve(static_cast<qsizetype>(count));

    for (quint32 i = 0; i < count; ++i)
    {
        User user;
        stream >> user.entry_id >> user.name >> user.group >> user.salt >> user.verifier
               >> user.sessions >> user.flags;

        if (stream.status() != QDataStream::Ok)
            return QVector<User>();

        if (user.isValid())
            users.append(user);
    }

    return users;
}

//--------------------------------------------------------------------------------------------------
bool isValidSettings(const PortablePackage::Settings& settings)
{
    return settings.router_address.isValid() &&
           settings.router_public_key.size() == kRouterPublicKeySize &&
           !settings.users.isEmpty();
}

//--------------------------------------------------------------------------------------------------
bool updateResource(const QString& file_path, const QByteArray& data)
{
    HANDLE update = BeginUpdateResourceW(qUtf16Printable(file_path), FALSE);
    if (!update)
    {
        PLOG(ERROR) << "BeginUpdateResourceW failed";
        return false;
    }

    if (!UpdateResourceW(update, RT_RCDATA, const_cast<wchar_t*>(kResourceName), kResourceLanguage,
                         const_cast<char*>(data.constData()), static_cast<DWORD>(data.size())))
    {
        PLOG(ERROR) << "UpdateResourceW failed";
        EndUpdateResourceW(update, TRUE);
        return false;
    }

    if (!EndUpdateResourceW(update, FALSE))
    {
        PLOG(ERROR) << "EndUpdateResourceW failed";
        return false;
    }

    return true;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// static
QVector<PortablePackage::Architecture> PortablePackage::availableArchitectures()
{
    QVector<Architecture> architectures;

#if defined(Q_PROCESSOR_X86_64)
    if (QFileInfo::exists(hostX86Path()))
        architectures.append(Architecture::X86);

    architectures.append(Architecture::X64);
#else
    architectures.append(Architecture::X86);
#endif

    return architectures;
}

//--------------------------------------------------------------------------------------------------
// static
PortablePackage::Result PortablePackage::exportWithSettings(const QString& file_path,
                                                            Architecture architecture)
{
    Database& db = Database::instance();

    Settings settings;
    settings.router_address = db.routerAddress();
    settings.router_public_key = db.routerPublicKey();
    settings.users = db.userList();

    if (!db.isRouterEnabled() || !settings.router_address.isValid() ||
        settings.router_public_key.size() != kRouterPublicKeySize)
    {
        LOG(ERROR) << "Router is not configured";
        return Result::NO_ROUTER;
    }

    if (settings.users.isEmpty())
    {
        LOG(ERROR) << "The host has no users";
        return Result::NO_USERS;
    }

    QJsonObject object;
    object[kVersion] = kSettingsVersion;
    object[kRouterAddress] = settings.router_address.toString();
    object[kRouterPublicKey] = QString::fromLatin1(settings.router_public_key.toHex());
    object[kUsers] = QString::fromLatin1(serializeUsers(settings.users).toBase64());

    const QString executable_path = ProcessUtil::filePath(ProcessUtil::currentProcessId());

    QString source_path;
#if defined(Q_PROCESSOR_X86_64)
    source_path = (architecture == Architecture::X86) ? hostX86Path() : executable_path;
#else
    if (architecture != Architecture::X86)
    {
        LOG(ERROR) << "The 64-bit package cannot be exported by a 32-bit host";
        return Result::FAILED;
    }

    source_path = executable_path;
#endif

    if (source_path.isEmpty() || !QFileInfo::exists(source_path))
    {
        LOG(ERROR) << "The build to export is not installed:" << source_path;
        return Result::FAILED;
    }

    if (QFile::exists(file_path) && !QFile::remove(file_path))
    {
        LOG(ERROR) << "Unable to remove existing file:" << file_path;
        return Result::FAILED;
    }

    if (!QFile::copy(source_path, file_path))
    {
        LOG(ERROR) << "Unable to copy" << source_path << "to:" << file_path;
        return Result::FAILED;
    }

    if (!updateResource(file_path, QJsonDocument(object).toJson(QJsonDocument::Compact)))
    {
        QFile::remove(file_path);
        return Result::FAILED;
    }

    LOG(INFO) << "Quick Support package exported to:" << file_path
              << "(arch:" << ((architecture == Architecture::X86) ? "x86" : "x64")
              << "users:" << settings.users.size() << ")";

    return Result::SUCCESS;
}

//--------------------------------------------------------------------------------------------------
// static
std::optional<PortablePackage::Settings> PortablePackage::builtInSettings()
{
    HRSRC resource = FindResourceExW(nullptr, RT_RCDATA, kResourceName, kResourceLanguage);
    if (!resource)
        return std::nullopt;

    HGLOBAL handle = LoadResource(nullptr, resource);
    const DWORD size = SizeofResource(nullptr, resource);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || !size)
    {
        LOG(ERROR) << "Unable to load built-in settings";
        return std::nullopt;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromRawData(static_cast<const char*>(data), static_cast<qsizetype>(size)), &error);
    if (!document.isObject())
    {
        LOG(ERROR) << "Unable to parse built-in settings:" << error.errorString();
        return std::nullopt;
    }

    const QJsonObject object = document.object();

    Settings settings;
    settings.router_address =
        Address::fromString(object[kRouterAddress].toString(), kDefaultRouterHostTcpPort);
    settings.router_public_key = QByteArray::fromHex(object[kRouterPublicKey].toString().toLatin1());
    settings.users = deserializeUsers(
        QByteArray::fromBase64(object[kUsers].toString().toLatin1()));

    if (!isValidSettings(settings))
    {
        LOG(ERROR) << "Invalid built-in settings";
        return std::nullopt;
    }

    return settings;
}

//--------------------------------------------------------------------------------------------------
// static
bool PortablePackage::isPortable()
{
    static const bool portable = builtInSettings().has_value();
    return portable;
}
