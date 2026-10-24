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

// The portable run of a Quick Support package.
//
// The user starts the exported copy of the host. The launcher asks for administrator rights, copies
// the executable to a fresh directory under Program Files, registers a temporary service for it and
// waits. The service is the ordinary host service: it connects to the router built into the package,
// authorizes the users built into the package (the one-time password is off) and starts the usual
// host window in the user's session. When the window is closed, the service stops and everything of
// the run is removed.
//
// Both the service and the window of the run work with the same executable, so the unique IPC
// channel suffix and the service name are derived from its path.
class PortableHost
{
public:
    // Whether the executable of this process is a Quick Support package.
    static bool isPortable();

    // Whether this invocation is the launcher the user started (not the service, the window or an
    // agent of the run).
    static bool isLauncherInvocation(int argc, char* argv[]);

    // Runs the launcher: asks for administrator rights and starts the temporary service.
    static int runLauncher();

    // Directory the run keeps its database and settings in. Empty for the installed host.
    static bool applyDataDirectory(const QString& path);
    static QString dataDirectory();

    // Name of the temporary service of this run.
    static QString serviceName();

    // Suffix that makes the IPC channels of a package unique on a machine where the installed host
    // runs as well. Empty for the installed host.
    static QString channelSuffix();

    // Writes the router settings and the users built into the package into the database of the run.
    static bool prepareDatabase();

private:
    Q_DISABLE_COPY_MOVE(PortableHost)
};

#endif // HOST_WIN_PORTABLE_HOST_H
