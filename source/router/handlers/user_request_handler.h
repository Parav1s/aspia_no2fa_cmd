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

#ifndef ROUTER_HANDLERS_USER_REQUEST_HANDLER_H
#define ROUTER_HANDLERS_USER_REQUEST_HANDLER_H

#include "router/handlers/request_caller.h"
#include "router/handlers/request_result.h"

namespace proto::router {
class ChangePasswordRequest;
class UserList;
class UserListRequest;
class UserRequest;
} // namespace proto::router

class Database;

// The requests over the user records. Everything they need is the database, so the logic lives
// here instead of inside the sessions, which stay thin adapters that frame the reply and turn the
// returned side effects into signals - the whole surface is testable against a temporary database.
// Queries fill the reply message (that is their whole result); commands return the RequestResult
// the session applies after the reply is sent.

// The user commands of the admin channel (add, modify, delete).
RequestResult handleUserRequest(Database& database, const RequestCaller& caller,
                                const proto::router::UserRequest& request);

// A page of the user list, or the single record a point lookup names. Fills |out| completely
// except for request_id (session framing).
void handleUserList(Database& database, const proto::router::UserListRequest& request,
                    proto::router::UserList* out);

// The rotation of the caller's own password over the client channel.
RequestResult handleChangePassword(Database& database, const RequestCaller& caller,
                                   const proto::router::ChangePasswordRequest& request);

#endif // ROUTER_HANDLERS_USER_REQUEST_HANDLER_H
