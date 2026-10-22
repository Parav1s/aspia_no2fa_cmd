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

#include "base/update/update_installer.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include "base/logging.h"
#include "base/time_types.h"
#include "base/crypto/generic_hash.h"
#include "base/crypto/random.h"
#include "base/files/file_util.h"

#if defined(Q_OS_WINDOWS)
#include <qt_windows.h>
#include "base/process_util.h"
#endif // defined(Q_OS_WINDOWS)


#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <QProcess>
#include <QProcessEnvironment>
#endif

namespace {

const qint64 kHashBlockSize = 1024 * 1024;

// How long a package is left alone before it counts as forgotten. The installer of the system reads
// the package from a process of its own, and the host and the client of one machine are updated
// apart, so a directory that is still being written to or read from is not the one to remove.
const qint64 kLeftoverSeconds = 60 * 60;

#if !defined(Q_OS_WINDOWS)
// Every package waits in a directory of its own named after this.
const char kPackagePrefix[] = "aspia_update_";
#endif // !defined(Q_OS_WINDOWS)

#if defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
// The directory the packages wait in. A /tmp that lives in memory is no place for tens of
// megabytes, and what is left there after an installation has to survive a reboot to be found and
// removed by the update that follows.
const char kPackageDirectory[] = "/var/tmp";
#endif

//--------------------------------------------------------------------------------------------------
bool hasExpectedHash(const QString& file_path, const QString& expected)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly))
    {
        LOG(ERROR) << "Unable to open file:" << file.errorString();
        return false;
    }

    GenericHash hash(GenericHash::SHA256);

    while (!file.atEnd())
    {
        QByteArray block = file.read(kHashBlockSize);
        if (block.isEmpty())
        {
            LOG(ERROR) << "Unable to read file:" << file.errorString();
            return false;
        }

        hash.addData(block);
    }

    QString actual = QString::fromLatin1(hash.result().toHex());
    if (actual != expected)
    {
        LOG(ERROR) << "Hash of the downloaded file is" << actual << "and not" << expected;
        return false;
    }

    return true;
}

//--------------------------------------------------------------------------------------------------
void removeFile(const QString& file_path)
{
    if (!QFile::remove(file_path))
        LOG(ERROR) << "Unable to remove file:" << file_path;
}

//--------------------------------------------------------------------------------------------------
// Creates a directory nobody but the owner can write to. The package waits there between the check
// of its hash and the start of the installer of the system, and a directory a user can write to
// would let the file be replaced in between. An empty string is returned on failure.
QString createPrivateDirectory()
{
#if defined(Q_OS_WINDOWS)
    wchar_t buffer[MAX_PATH] = { 0 };
    if (!GetWindowsDirectoryW(buffer, MAX_PATH))
    {
        PLOG(ERROR) << "GetWindowsDirectoryW failed";
        return QString();
    }

    // The name is random because the temp directory of the system lists its contents to everyone
    // and lets everyone create entries in it.
    QString path = QString::fromWCharArray(buffer) + "/Temp/aspia_update_" +
        QString::fromLatin1(Random::byteArray(16).toHex());

    if (!QDir().mkdir(path))
    {
        LOG(ERROR) << "Unable to create directory:" << path;
        return QString();
    }

    return path;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    UpdateInstaller::removeLeftovers();

    QByteArray name = QByteArray(kPackageDirectory) + "/" + kPackagePrefix + "XXXXXX";
    if (!mkdtemp(name.data()))
    {
        PLOG(ERROR) << "mkdtemp failed";
        return QString();
    }

    QString path = QString::fromLocal8Bit(name);

    // The package itself is public, and the account the package manager reads local files as has
    // to reach it. Only writing has to stay with the owner.
    if (chmod(name.constData(), 0755) != 0)
    {
        PLOG(ERROR) << "Unable to set permissions:" << path;
        QDir().rmdir(path);
        return QString();
    }

    return path;
#else
    return QString();
#endif
}

} // namespace

//--------------------------------------------------------------------------------------------------
UpdateInstaller::UpdateInstaller(Mode mode, QObject* parent)
    : QObject(parent),
      mode_(mode)
{
    LOG(TRACE) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
UpdateInstaller::~UpdateInstaller()
{
    LOG(TRACE) << "Dtor";
    cleanup();
}

//--------------------------------------------------------------------------------------------------
// static
bool UpdateInstaller::isSupported(const QString& format)
{
#if defined(Q_OS_WINDOWS)
    return format == "msi";
#elif defined(Q_OS_LINUX)
    // The package is handed to the manager that knows its format, and only that one.
    if (format == "deb")
        return !QStandardPaths::findExecutable("apt-get").isEmpty();

    if (format == "rpm")
        return !QStandardPaths::findExecutable("dnf").isEmpty();

    return false;
#elif defined(Q_OS_MACOS)
    return format == "pkg";
#else
    Q_UNUSED(format);
    return false;
#endif
}

//--------------------------------------------------------------------------------------------------
// static
bool UpdateInstaller::canInstall()
{
    return true;
}

//--------------------------------------------------------------------------------------------------
// static
void UpdateInstaller::openInstallPermission()
{
}

//--------------------------------------------------------------------------------------------------
// static
void UpdateInstaller::removeLeftovers()
{
#if defined(Q_OS_WINDOWS)
    // What the installer of the system was given is removed at the next start of the system, so
    // nothing waits here to be found.
#else
    QDir directory(QString::fromLatin1(kPackageDirectory));

    QDateTime now = QDateTime::currentDateTime();

    for (const QFileInfo& entry : directory.entryInfoList({ QString(kPackagePrefix) + "*" },
                                                          QDir::Dirs | QDir::NoSymLinks))
    {
        QDateTime last_change = entry.lastModified();

        for (const QFileInfo& file : QDir(entry.filePath()).entryInfoList(QDir::Files))
        {
            if (file.lastModified() > last_change)
                last_change = file.lastModified();
        }

        if (last_change.secsTo(now) < kLeftoverSeconds)
            continue;

        LOG(INFO) << "Removing the package of a previous update:" << entry.fileName();
        QDir(entry.filePath()).removeRecursively();
    }
#endif // defined(Q_OS_WINDOWS)
}

//--------------------------------------------------------------------------------------------------
QString UpdateInstaller::createPackageFile(const UpdateInfo& update_info)
{
    cleanup();

    QString directory = createPrivateDirectory();
    if (directory.isEmpty())
        return QString();

    // The package manager of the system is given a file it recognizes by its name. The name is
    // random for the same reason the name of the directory is.
    QString file_path = QDir::toNativeSeparators(
        directory + "/" + QString::fromLatin1(Random::byteArray(16).toHex()) + "." +
        update_info.format());

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly))
    {
        LOG(ERROR) << "Unable to create file:" << file.errorString();
        QDir().rmdir(directory);
        return QString();
    }

    file.close();

    update_info_ = update_info;
    directory_ = directory;
    file_path_ = file_path;

    return file_path;
}

//--------------------------------------------------------------------------------------------------
UpdateInstaller::Result UpdateInstaller::install()
{
    if (file_path_.isEmpty())
    {
        LOG(ERROR) << "No package to install";
        return Result::FAILED;
    }

    if (!hasExpectedHash(file_path_, update_info_.sha256()))
        return Result::DAMAGED;

    if (!startInstaller())
        return Result::FAILED;

    return Result::STARTED;
}

//--------------------------------------------------------------------------------------------------
void UpdateInstaller::cleanup()
{
    if (!file_path_.isEmpty())
    {
        removeFile(file_path_);
        file_path_.clear();
    }

    if (!directory_.isEmpty())
    {
        if (!QDir().rmdir(directory_))
            LOG(ERROR) << "Unable to remove directory:" << directory_;

        directory_.clear();
    }
}

//--------------------------------------------------------------------------------------------------
bool UpdateInstaller::startInstaller()
{
#if defined(Q_OS_WINDOWS)
    // A person watching gets a basic UI with no modal dialog boxes, the service and the one who
    // administers the machine install with no UI at all.
    QString arguments = "/i \"" + file_path_ + "\"" + (mode_ == Mode::USER ? " /qb-!" : " /qn");

    // The one who administers the machine already has the rights and they were checked before the
    // download, so the shell is not asked for them a second time.
    ProcessUtil::ExecuteMode execute_mode = (mode_ == Mode::ADMIN) ?
        ProcessUtil::ExecuteMode::NORMAL : ProcessUtil::ExecuteMode::ELEVATE;

    if (!ProcessUtil::createProcess("msiexec", arguments, execute_mode))
    {
        LOG(ERROR) << "Unable to start msiexec process (cmd:" << arguments << ")";
        return false;
    }

    LOG(INFO) << "msiexec is started (cmd:" << arguments << ")";

    // The package is msiexec's from here on, and it is still holding it when this process ends.
    if (!removeAtNextStart(file_path_))
        PLOG(ERROR) << "Unable to remove file at the next start:" << file_path_;

    if (!removeAtNextStart(directory_))
        PLOG(ERROR) << "Unable to remove directory at the next start:" << directory_;

    file_path_.clear();
    directory_.clear();

    return true;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    QStringList arguments;

#if defined(Q_OS_MACOS)
    // The installer of the system puts the package where the package itself says it goes.
    arguments << "installer" << "-pkg" << file_path_ << "-target" << "/";
#else
    // Both managers install a local file and pull in what it depends on.
    if (update_info_.format() == "deb")
        arguments << "apt-get" << "install" << "-y" << file_path_;
    else if (update_info_.format() == "rpm")
        arguments << "dnf" << "install" << "-y" << file_path_;
    else
    {
        LOG(ERROR) << "No package manager for format:" << update_info_.format();
        return false;
    }
#endif // defined(Q_OS_MACOS)

#if defined(Q_OS_LINUX)
    // The package restarts the service, and systemd kills the whole control group of a unit it
    // stops, so an installer left in that group dies with it. A transient unit of its own outlives
    // whoever asked for the installation. The name carries the process id: the host and the client
    // of one machine can be updated at the same moment, and a name already taken is a refusal.
    if (!QStandardPaths::findExecutable("systemd-run").isEmpty())
    {
        QStringList unit_arguments = QStringList() << "systemd-run" << "--collect" << "--quiet"
                                                   << ("--unit=aspia-update-" + QString::number(getpid()));

        // The unit does not inherit the environment of this process.
        if (update_info_.format() == "deb")
            unit_arguments << "--setenv=DEBIAN_FRONTEND=noninteractive";

        arguments = unit_arguments + arguments;
    }
#endif // defined(Q_OS_LINUX)

    QProcess process;
    process.setProgram(arguments.takeFirst());
    process.setArguments(arguments);

#if defined(Q_OS_LINUX)
    if (update_info_.format() == "deb")
    {
        // The package manager stops on a question about a configuration file it did not put there,
        // and there is nobody to answer it.
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("DEBIAN_FRONTEND", "noninteractive");
        process.setProcessEnvironment(environment);
    }
#endif // defined(Q_OS_LINUX)

    if (!process.startDetached())
    {
        LOG(ERROR) << "Unable to create update process (cmd:" << process.program()
                   << process.arguments() << ")";
        return false;
    }

    LOG(INFO) << "Update process started (cmd:" << process.program() << process.arguments() << ")";

    // The process is detached, so there is nobody left to clean up after it.
    file_path_.clear();
    directory_.clear();

    return true;
#else
    return false;
#endif
}
