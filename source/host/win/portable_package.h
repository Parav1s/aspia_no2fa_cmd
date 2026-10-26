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

#ifndef HOST_WIN_PORTABLE_PACKAGE_H
#define HOST_WIN_PORTABLE_PACKAGE_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

#include "base/build_config.h"
#include "base/net/address.h"
#include "base/peer/user.h"

// A copy of the host executable with the settings of this host built in: the router it connects
// to and the users it authorizes. Such a copy is a Quick Support package: the user runs it, the
// window shows the host ID, and the operator connects with the built-in user name and password.
// The package does not carry the ID of the host it was exported from: the machine that runs it
// gets its own ID (or takes the ID of its installed host).
//
// The one-time password is not exported: the package authorizes by the user password only.
class PortablePackage
{
public:
    enum class Result
    {
        SUCCESS,
        NO_ROUTER,  // The host is not configured for a router.
        NO_USERS,   // The host has no users to connect with.
        FAILED
    };

    struct Settings
    {
        Address router_address { kDefaultRouterHostTcpPort };
        QByteArray router_public_key;
        QVector<User> users;
    };

    // Saves a copy of the host executable to |file_path| with the router settings and the users of
    // this host built in.
    static Result exportWithSettings(const QString& file_path);

    // Returns the settings built into the executable of the current process, if there are any.
    static std::optional<Settings> builtInSettings();

    // Whether the executable of this process is a Quick Support package.
    static bool isPortable();

private:
    Q_DISABLE_COPY_MOVE(PortablePackage)
};

#endif // HOST_WIN_PORTABLE_PACKAGE_H
