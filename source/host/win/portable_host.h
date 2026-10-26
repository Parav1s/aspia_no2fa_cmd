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

#ifndef HOST_WIN_PORTABLE_HOST_H
#define HOST_WIN_PORTABLE_HOST_H

#include <QString>

#include "base/peer/host_id.h"

// The portable run of a Quick Support package.
//
// The user starts the exported copy of the host. It runs in the user's session without asking for
// administrator rights: the host service and the window live in one process and keep their
// settings and database in a directory of their own. What needs administrator rights (access to
// the logon screen, starting agents for other accounts) is simply unavailable - the run works with
// what the current user may do.
//
// The window has a shield button: it starts the desktop agent again with the administrator rights of
// the user. The service and the client session stay; the elevated agent supersedes the ordinary one.
class PortableHost
{
public:
    // Whether this process is the port run (the installed host is not).
    static bool isPortable();

    // Whether this invocation is the portable host the user started (a bare start), not a role
    // process (the service, an agent).
    static bool isPortableInvocation(int argc, char* argv[]);

    // Directory the run keeps its database and settings in. Empty for the installed host.
    static bool applyDataDirectory(const QString& path);

    // Remembers the directory without touching it (the window process gets it with --qs-dir).
    static void setDataDirectory(const QString& path);
    static QString dataDirectory();

    // %LOCALAPPDATA%\AspiaQS\<id of the executable>: writable without administrator rights and
    // separate for every copy of the package.
    static QString defaultDataDirectory();

    // Suffix that makes the IPC channels of a package unique on a machine where the installed host
    // runs as well. Empty for the installed host.
    static QString channelSuffix();

    // Writes the router settings and the users built into the package into the database of the run.
    static bool prepareDatabase();

    // Whether an installed Aspia host works on this machine right now. Its service is the usual
    // case; a host started by hand (the same binary outside the service) is caught by process name.
    static bool installedHostIsRunning();

    // The last ID the installed host received from the router. |kInvalidHostId| if the installed
    // host has never connected or its storage cannot be read.
    static HostId installedHostId();

private:
    Q_DISABLE_COPY_MOVE(PortableHost)
};

#endif // HOST_WIN_PORTABLE_HOST_H
