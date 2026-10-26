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

#include "host/win/portable_host.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSettings>
#include <qt_windows.h>
#include <TlHelp32.h>

#include "base/crypto/generic_hash.h"
#include "base/files/base_paths.h"
#include "base/logging.h"
#include "base/process_util.h"
#include "base/service_controller.h"
#include "base/win/scoped_object.h"
#include "host/database.h"
#include "host/host_storage.h"
#include "host/win/portable_package.h"

namespace {

const char kDataDirectoryVariable[] = "ASPIA_QS_DATA_DIR";
const char kBaseDirectoryName[] = "AspiaQS";
const char kInstalledServiceName[] = "aspia-host-service";

// Marks that the ID in the database belongs to this data directory. It is absent in data
// directories created by versions that embedded the host ID in the package; the first start of
// such a directory discards that ID once.
const char kLocalIdMarker[] = "id-local";

// Held for the lifetime of the process: two runs of a package would share the same database (and
// therefore the same host ID), which the router cannot tell apart.
QLockFile* g_data_directory_lock = nullptr;
QString g_data_directory;

//--------------------------------------------------------------------------------------------------
QString executablePath()
{
    return ProcessUtil::filePath(ProcessUtil::currentProcessId());
}

//--------------------------------------------------------------------------------------------------
QString executableHash()
{
    const QByteArray path = QFileInfo(executablePath()).canonicalFilePath().toLower().toUtf8();
    return QString::fromLatin1(
        GenericHash::hash(GenericHash::BLAKE2s256, path).toHex().left(16));
}

//--------------------------------------------------------------------------------------------------
// Whether an installed host of this machine works right now. Its service is the usual case; a host
// started by hand (the same binary outside the service) is caught by the process name.
bool detectInstalledHost()
{
    if (ServiceController::isInstalled(QLatin1String(kInstalledServiceName)) &&
        ServiceController::isRunning(QLatin1String(kInstalledServiceName)))
    {
        return true;
    }

    const DWORD current_pid = GetCurrentProcessId();

    // The package itself and its agents run the same executable, and a package exported under the
    // name aspia_host.exe matches the process name below. They are not the installed host: a restart
    // of the package (the shield button) must not look like one and throw the host ID away.
    const QString current_path = QFileInfo(executablePath()).canonicalFilePath();

    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.isValid())
        return false;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry))
        return false;

    do
    {
        if (entry.th32ProcessID == current_pid)
            continue;

        if (QString::fromWCharArray(entry.szExeFile).compare(
                QLatin1String("aspia_host.exe"), Qt::CaseInsensitive) != 0)
        {
            continue;
        }

        if (!current_path.isEmpty())
        {
            const QString process_path =
                QFileInfo(ProcessUtil::filePath(entry.th32ProcessID)).canonicalFilePath();
            if (process_path == current_path)
                continue;
        }

        return true;
    }
    while (Process32NextW(snapshot, &entry));

    return false;
}

//--------------------------------------------------------------------------------------------------
// The ID the host of an installed Aspia is already known by. A support run on a machine that had
// the host installed keeps that ID instead of asking the router for a new one.
QByteArray installedHostKey()
{
    // A running installed host owns its ID; taking it over would make the router mix up the two.
    if (detectInstalledHost())
    {
        LOG(INFO) << "The installed host is running, its ID is not taken over";
        return QByteArray();
    }

    const QString dir = BasePaths::genericConfigDir();
    if (dir.isEmpty())
        return QByteArray();

    const QString file_path = dir + "/aspia/secure/host.db3";
    if (!QFileInfo::exists(file_path))
        return QByteArray();

    // The database of the installed host is readable only by SYSTEM and administrators; without
    // those rights the run simply does not see it.
    std::unique_ptr<Database> database = Database::openForTesting(file_path);
    if (!database || !database->isValid())
    {
        LOG(INFO) << "Unable to read the database of the installed host";
        return QByteArray();
    }

    const QByteArray host_key = database->hostKey();
    if (!host_key.isEmpty())
        LOG(INFO) << "The ID of the installed host is taken over";

    return host_key;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::installedHostIsRunning()
{
    return detectInstalledHost();
}

//--------------------------------------------------------------------------------------------------
// static
HostId PortableHost::installedHostId()
{
    const QString dir = BasePaths::genericConfigDir();
    if (dir.isEmpty())
        return kInvalidHostId;

    // HostStorage keeps the last ID in a machine-wide INI file. It is readable without
    // administrator rights even though the host database next to it is not.
    QSettings settings(dir + "/aspia/host_storage.ini", QSettings::IniFormat);
    const HostId host_id = settings.value("host_id").toULongLong();
    LOG(INFO) << "The last ID of the installed host:" << host_id;
    return host_id;
}

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::isPortable()
{
    return PortablePackage::isPortable();
}

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::isPortableInvocation(int argc, char* argv[])
{
    Q_UNUSED(argv)

    if (!isPortable())
        return false;

    // A bare invocation is what the user double-clicked. A role invocation (the window, an agent)
    // is never it.
    return argc == 1;
}

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::applyDataDirectory(const QString& path)
{
    if (path.isEmpty())
        return false;

    QDir dir(path);
    if (!dir.exists() && !QDir().mkpath(path))
    {
        LOG(ERROR) << "Unable to create the data directory:" << path;
        return false;
    }

    // One run at a time: a second one would reuse the same database and the same host ID.
    if (!g_data_directory_lock)
    {
        g_data_directory_lock = new QLockFile(dir.filePath("aspia-qs.lock"));
        if (!g_data_directory_lock->tryLock(10000))
        {
            LOG(ERROR) << "Another Quick Support run is using" << path;
            delete g_data_directory_lock;
            g_data_directory_lock = nullptr;
            return false;
        }
    }

    qputenv(kDataDirectoryVariable, path.toUtf8());
    g_data_directory = path;

    // The system settings of the host (host.ini) are machine-wide by default; inside a run they
    // live next to its database.
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::toNativeSeparators(path));

    return true;
}

//--------------------------------------------------------------------------------------------------
// static
QString PortableHost::dataDirectory()
{
    if (!g_data_directory.isEmpty())
        return g_data_directory;

    return qEnvironmentVariable(kDataDirectoryVariable);
}

//--------------------------------------------------------------------------------------------------
// static
void PortableHost::setDataDirectory(const QString& path)
{
    g_data_directory = path;
}

//--------------------------------------------------------------------------------------------------
// static
QString PortableHost::defaultDataDirectory()
{
    const QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty())
        return QString();

    // One directory per user: the settings and the ID belong to this machine, not to a package
    // exported elsewhere. A package run without an installed host asks for its own ID.
    return QDir::fromNativeSeparators(base) + '/' + kBaseDirectoryName;
}

//--------------------------------------------------------------------------------------------------
// static
QString PortableHost::channelSuffix()
{
    if (!isPortable())
        return QString();

    return '-' + executableHash();
}

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::prepareDatabase()
{
    std::optional<PortablePackage::Settings> settings = PortablePackage::builtInSettings();
    if (!settings.has_value())
        return false;

    Database& db = Database::instance();
    if (!db.isValid())
    {
        LOG(ERROR) << "Unable to open the database of the run";
        return false;
    }

    // Old packages embedded the ID of the host they were exported from. A data directory created
    // by one of them has no marker; discard its stored ID once so the target machine asks the
    // router for its own. The marker makes sure a legitimate ID survives later starts.
    const QString data_dir = dataDirectory();
    if (!data_dir.isEmpty())
    {
        const QString marker_path = data_dir + '/' + QLatin1String(kLocalIdMarker);
        if (!QFileInfo::exists(marker_path))
        {
            LOG(INFO) << "The Quick Support data directory has no local ID marker; resetting the ID";

            if (!db.hostKey().isEmpty() && !db.setHostKey(QByteArray()))
                LOG(ERROR) << "Unable to clear the host ID";

            HostStorage().setLastHostId(kInvalidHostId);

            QFile marker(marker_path);
            if (!marker.open(QIODevice::WriteOnly))
                LOG(ERROR) << "Unable to create the local ID marker:" << marker.errorString();
        }
    }

    // A running installed host owns the ID of this machine. The package must not take it over (nor
    // keep the ID of a previous run): the router accepts only one of them, and the user would not
    // be able to tell which host they are looking at. In this case the run asks for an ID of its own.
    QByteArray host_key;

    if (PortableHost::installedHostIsRunning())
    {
        // The installed host owns the machine's ID. A unique ID from a previous run is fine to
        // keep: the router already knows it and it does not conflict. Only the installed host's
        // own ID (taken over while its service was stopped) has to go.
        const QByteArray own_key = db.hostKey();

        const QByteArray installed_key = installedHostKey();

        if (!own_key.isEmpty() && own_key != installed_key)
        {
            LOG(INFO) << "The installed host is running, the package keeps its own ID";
            host_key = own_key;
        }
        else
        {
            LOG(INFO) << "The installed host is running, the package asks for an ID of its own";

            if (!own_key.isEmpty() && !db.setHostKey(QByteArray()))
                LOG(ERROR) << "Unable to clear the host ID";
        }
    }
    else
    {
        // The installed host is stopped: its identity is what the clients already have in their
        // address books, so the package stands in for it. The installed database is readable only
        // with administrator rights; the ID this run used before is the last fallback.
        LOG(INFO) << "The installed host is stopped, the package takes its ID";

        host_key = installedHostKey();
        if (host_key.isEmpty())
            host_key = db.hostKey();
    }

    if (!db.setRouterEnabled(true))
        LOG(ERROR) << "Unable to enable the router";

    if (!db.setRouterAddress(settings->router_address))
        LOG(ERROR) << "Unable to write the router address";

    if (!db.setRouterPublicKey(settings->router_public_key))
        LOG(ERROR) << "Unable to write the router public key";

    // A Quick Support package authorizes by the user password only.
    if (!db.setOneTimePassword(false))
        LOG(ERROR) << "Unable to disable the one-time password";

    // The users and the router always follow the package: a package exported again with new users
    // works without losing the ID.
    if (!db.replaceUsers(settings->users))
    {
        LOG(ERROR) << "Unable to write the users of the package";
        return false;
    }

    if (!host_key.isEmpty() && !db.setHostKey(host_key))
        LOG(ERROR) << "Unable to keep the host ID";

    LOG(INFO) << "Quick Support database is prepared (users:" << settings->users.size()
              << "id kept:" << !host_key.isEmpty() << ")";
    return true;
}
