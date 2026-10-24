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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QThread>
#include <qt_windows.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#include "base/crypto/generic_hash.h"
#include "base/crypto/random.h"
#include "base/logging.h"
#include "base/process_util.h"
#include "base/service_controller.h"
#include "base/time_types.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/scoped_com_initializer.h"
#include "base/win/scoped_object.h"
#include "base/win/security_helpers.h"
#include "host/database.h"
#include "host/win/portable_package.h"

namespace {

const char kLaunchOption[] = "--portable-launch";
const char kDataDirectoryVariable[] = "ASPIA_QS_DATA_DIR";
const char kExecutableName[] = "aspia_host.exe";
const char kBaseDirectoryName[] = "AspiaQS";
const char kDataDirectoryName[] = "data";
const char kServiceNamePrefix[] = "aspia-qs-";
const char kServiceDisplayName[] = "Aspia Quick Support";

// Only SYSTEM and Administrators may read the settings of the run (the database holds the password
// verifiers of the users built into the package).
const char kDataDirectorySddl[] = "O:SYG:SYD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)";

const MilliSeconds kPollInterval(100);
const int kServiceStartAttempts = 300;
const int kRemoveAttempts = 30;

//--------------------------------------------------------------------------------------------------
// Where the copies of the run live. It is Program Files: only administrators can create files
// there, so nobody can replace the executable the service runs as SYSTEM.
QString baseDirectory()
{
    ScopedCoMem<wchar_t> buffer;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &buffer);
    if (FAILED(hr))
    {
        LOG(ERROR) << "SHGetKnownFolderPath failed";
        return QString();
    }

    return QDir::fromNativeSeparators(QString::fromWCharArray(buffer)) + '/' + kBaseDirectoryName;
}

//--------------------------------------------------------------------------------------------------
QString executableHash()
{
    const QByteArray path =
        QFileInfo(ProcessUtil::filePath(ProcessUtil::currentProcessId())).canonicalFilePath().toLower().toUtf8();
    return QString::fromLatin1(
        GenericHash::hash(GenericHash::BLAKE2s256, path).toHex().left(16));
}

//--------------------------------------------------------------------------------------------------
QString serviceNameForId(const QString& run_id)
{
    return QString::fromLatin1(kServiceNamePrefix) + run_id;
}

//--------------------------------------------------------------------------------------------------
QString mutexName(const QString& run_id)
{
    return "Global\\aspia-qs-" + run_id;
}

//--------------------------------------------------------------------------------------------------
void removeService(const QString& name)
{
    if (!ServiceController::isInstalled(name))
        return;

    if (ServiceController::isRunning(name))
    {
        std::unique_ptr<ServiceController> service = ServiceController::open(name);
        if (service)
            service->stop();
    }

    if (ServiceController::remove(name))
        LOG(INFO) << "Service" << name << "is removed";
    else
        LOG(ERROR) << "Unable to remove service:" << name;
}

//--------------------------------------------------------------------------------------------------
// Terminates the processes still running from |run_dir| so the directory can be removed. The window
// outlives the service (like the installed host's) and a desktop agent may still be exiting.
void terminateRunProcesses(const QString& run_dir)
{
    ScopedHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.isValid())
    {
        PLOG(ERROR) << "CreateToolhelp32Snapshot failed";
        return;
    }

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot, &entry))
        return;

    const QString prefix = QDir::toNativeSeparators(run_dir) + '\\';

    do
    {
        if (QString::fromWCharArray(entry.szExeFile).compare(
                QLatin1String(kExecutableName), Qt::CaseInsensitive) != 0)
        {
            continue;
        }

        const QString path = QDir::toNativeSeparators(ProcessUtil::filePath(entry.th32ProcessID));
        if (!path.startsWith(prefix, Qt::CaseInsensitive))
            continue;

        ScopedHandle process(OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID));
        if (process.isValid() && TerminateProcess(process, 0))
            LOG(INFO) << "Terminated process from the run directory:" << path;
    }
    while (Process32NextW(snapshot, &entry));
}

//--------------------------------------------------------------------------------------------------
void removeDirectory(const QString& path)
{
    for (int i = 0; i < kRemoveAttempts; ++i)
    {
        if (!QFileInfo::exists(path) || QDir(path).removeRecursively())
            return;

        // The processes of the run started from the directory may still be exiting.
        QThread::sleep(Seconds(1));
    }

    LOG(ERROR) << "Unable to remove directory:" << path;
}

//--------------------------------------------------------------------------------------------------
// Removes what is left of the runs whose launcher is gone (killed, or the user logged off).
void removeStaleRuns(const QString& base_dir)
{
    const QStringList run_ids = QDir(base_dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& run_id : run_ids)
    {
        ScopedHandle mutex(OpenMutexW(SYNCHRONIZE, FALSE, qUtf16Printable(mutexName(run_id))));
        if (mutex.isValid())
            continue;

        LOG(INFO) << "Removing stale run:" << run_id;
        removeService(serviceNameForId(run_id));
        terminateRunProcesses(base_dir + '/' + run_id);
        removeDirectory(base_dir + '/' + run_id);
    }
}

//--------------------------------------------------------------------------------------------------
bool createDirectoryWithSecurity(const QString& path)
{
    ScopedSd security_descriptor = convertSddlToSd(QString::fromLatin1(kDataDirectorySddl));
    if (!security_descriptor)
        return false;

    SECURITY_ATTRIBUTES attributes;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = security_descriptor.get();
    attributes.bInheritHandle = FALSE;

    if (!CreateDirectoryW(qUtf16Printable(QDir::toNativeSeparators(path)), &attributes))
    {
        PLOG(ERROR) << "CreateDirectoryW failed";
        return false;
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
// Copies the executable to |run_dir|, registers the temporary service for it and waits until the
// service stops.
bool runHost(const QString& run_dir, const QString& data_dir, const QString& run_id)
{
    if (!QDir().mkpath(run_dir) || !createDirectoryWithSecurity(data_dir))
    {
        LOG(ERROR) << "Unable to create directory:" << run_dir;
        return false;
    }

    const QString file_path = run_dir + '/' + kExecutableName;
    if (!QFile::copy(ProcessUtil::filePath(ProcessUtil::currentProcessId()), file_path))
    {
        LOG(ERROR) << "Unable to copy the executable to:" << file_path;
        return false;
    }

    if (!PortableHost::applyDataDirectory(data_dir))
        return false;

    if (!PortableHost::prepareDatabase())
    {
        LOG(ERROR) << "Unable to prepare the database of the run";
        return false;
    }

    ScopedScHandle manager(
        OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager.isValid())
    {
        PLOG(ERROR) << "OpenSCManagerW failed";
        return false;
    }

    const QString service_name = PortableHost::serviceName();
    const QString command_line = '"' + QDir::toNativeSeparators(file_path) +
        "\" --service --qs-dir=\"" + QDir::toNativeSeparators(data_dir) + '"';

    ScopedScHandle service(CreateServiceW(manager, qUtf16Printable(service_name),
        qUtf16Printable(QString::fromLatin1(kServiceDisplayName) + " (" + run_id + ')'),
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE,
        qUtf16Printable(command_line), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service.isValid())
    {
        PLOG(ERROR) << "CreateServiceW failed";
        return false;
    }

    if (!StartServiceW(service, 0, nullptr))
    {
        PLOG(ERROR) << "StartServiceW failed";
        return false;
    }

    SERVICE_STATUS_PROCESS status;
    memset(&status, 0, sizeof(status));

    for (int i = 0; i < kServiceStartAttempts; ++i)
    {
        DWORD bytes_needed = 0;
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                  sizeof(status), &bytes_needed))
        {
            PLOG(ERROR) << "QueryServiceStatusEx failed";
            return false;
        }

        if (status.dwCurrentState != SERVICE_START_PENDING)
            break;

        QThread::sleep(kPollInterval);
    }

    if (status.dwCurrentState != SERVICE_RUNNING)
    {
        LOG(ERROR) << "Service is not started (state:" << status.dwCurrentState << ")";
        return false;
    }

    ScopedHandle process(OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId));
    if (!process.isValid())
    {
        PLOG(ERROR) << "OpenProcess failed";
        return false;
    }

    LOG(INFO) << "Quick Support is started from:" << run_dir;
    WaitForSingleObject(process, INFINITE);
    LOG(INFO) << "Quick Support is stopped";
    return true;
}

//--------------------------------------------------------------------------------------------------
int launch()
{
    // The firewall rules are removed through COM.
    ScopedCOMInitializer com_initializer;

    const QString base_dir = baseDirectory();
    if (base_dir.isEmpty())
        return 1;

    removeStaleRuns(base_dir);

    QString run_id;
    QString run_dir;
    do
    {
        run_id = QString::fromLatin1(Random::byteArray(8).toHex());
        run_dir = base_dir + '/' + run_id;
    }
    while (QFileInfo::exists(run_dir));

    ScopedHandle mutex(CreateMutexW(nullptr, FALSE, qUtf16Printable(mutexName(run_id))));
    if (!mutex.isValid())
    {
        PLOG(ERROR) << "CreateMutexW failed";
        return 1;
    }

    const QString data_dir = run_dir + '/' + kDataDirectoryName;
    const bool result = runHost(run_dir, data_dir, run_id);

    removeService(serviceNameForId(run_id));
    terminateRunProcesses(run_dir);
    removeDirectory(run_dir);

    // Removed only when there are no other runs.
    QDir().rmdir(base_dir);

    if (!result)
    {
        MessageBoxW(nullptr,
                    L"The Quick Support host could not be started.\n"
                    L"See the log files in %TEMP%\\aspia for details.",
                    L"Aspia Quick Support", MB_ICONERROR | MB_OK);
    }

    return result ? 0 : 1;
}

} // namespace

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::isPortable()
{
    return PortablePackage::isPortable();
}

//--------------------------------------------------------------------------------------------------
// static
bool PortableHost::isLauncherInvocation(int argc, char* argv[])
{
    if (!isPortable())
        return false;

    // The internal relaunch (asking for elevation) is always the launcher.
    if (argc == 2 && qstrcmp(argv[1], kLaunchOption) == 0)
        return true;

    // A role invocation (the service, the window, an agent) is never the launcher. A bare invocation
    // is what the user double-clicked.
    return argc == 1;
}

//--------------------------------------------------------------------------------------------------
// static
int PortableHost::runLauncher()
{
    if (!ProcessUtil::isProcessElevated())
    {
        // Ask for elevation (the temporary service needs administrator rights).
        if (ProcessUtil::createProcess(ProcessUtil::filePath(ProcessUtil::currentProcessId()),
                                       QString::fromLatin1(kLaunchOption),
                                       ProcessUtil::ExecuteMode::ELEVATE))
        {
            return 0;
        }

        // The UAC prompt was declined: without the service the host cannot serve the session.
        MessageBoxW(nullptr,
                    L"Aspia Quick Support needs administrator rights to start.\n"
                    L"Start the program again and allow the elevation request.",
                    L"Aspia Quick Support", MB_ICONWARNING | MB_OK);
        return 1;
    }

    return launch();
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

    qputenv(kDataDirectoryVariable, path.toUtf8());

    // The system settings of the host (host.ini) are machine-wide by default; inside a run they
    // live next to its database.
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, QDir::toNativeSeparators(path));

    return true;
}

//--------------------------------------------------------------------------------------------------
// static
QString PortableHost::dataDirectory()
{
    return qEnvironmentVariable(kDataDirectoryVariable);
}

//--------------------------------------------------------------------------------------------------
// static
QString PortableHost::serviceName()
{
    QString run_id;

    const QString data_dir = dataDirectory();
    if (!data_dir.isEmpty())
        run_id = QFileInfo(data_dir).dir().dirName();

    if (run_id.isEmpty())
        run_id = executableHash();

    return serviceNameForId(run_id);
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

    // Prepared by the launcher already.
    if (!db.userList().isEmpty())
        return true;

    if (!db.setRouterEnabled(true))
        LOG(ERROR) << "Unable to enable the router";

    if (!db.setRouterAddress(settings->router_address))
        LOG(ERROR) << "Unable to write the router address";

    if (!db.setRouterPublicKey(settings->router_public_key))
        LOG(ERROR) << "Unable to write the router public key";

    // A Quick Support package authorizes by the user password only.
    if (!db.setOneTimePassword(false))
        LOG(ERROR) << "Unable to disable the one-time password";

    if (!db.replaceUsers(settings->users))
    {
        LOG(ERROR) << "Unable to write the users of the package";
        return false;
    }

    LOG(INFO) << "Quick Support database is prepared (users:" << settings->users.size() << ")";
    return true;
}
