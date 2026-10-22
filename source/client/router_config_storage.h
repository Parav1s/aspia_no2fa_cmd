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

#ifndef CLIENT_ROUTER_CONFIG_STORAGE_H
#define CLIENT_ROUTER_CONFIG_STORAGE_H

#include "client/config.h"

// The account of the router that was used last, kept for the command line. It lives outside the
// address book, so a shortcut that names a host ID works without the master password and without
// repeating the router options. On Windows the record is stored for the whole machine and the
// password is sealed with the machine key of DPAPI; if that place is not writable, the record
// falls back to the profile of the current user.
class RouterConfigStorage final
{
public:
    RouterConfigStorage() = default;
    ~RouterConfigStorage() = default;

    // The last router that completed a login. An invalid config when nothing has been stored yet.
    RouterConfig routerConfig() const;

    // Stores the account of |config|. Returns false when neither the machine-wide nor the
    // per-user place could be written.
    bool setRouterConfig(const RouterConfig& config);

private:
    Q_DISABLE_COPY_MOVE(RouterConfigStorage)
};

#endif // CLIENT_ROUTER_CONFIG_STORAGE_H
