//
// Aspia Project
// Copyright (C) 2016-2026 Dmitry Chapyshev <dmitry@aspia.ru>
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

#include "router/handlers/user_request_handler.h"

#include <vector>

#include "base/logging.h"
#include "base/peer/router_user.h"
#include "base/peer/user.h"
#include "proto/router_admin.h"
#include "proto/router_client.h"
#include "proto/router_constants.h"
#include "router/database.h"
#include "router/workers/client_worker.h"

namespace {

using Result = RequestResult;

//--------------------------------------------------------------------------------------------------
void handleAdd(Database& database, const proto::router::User& user, Result* result)
{
    LOG(INFO) << "User add request:" << user.name();

    RouterUser new_user = RouterUser::parseFrom(user);
    if (!new_user.isValid())
    {
        LOG(ERROR) << "Invalid user record:" << new_user.name;
        result->error_code = proto::router::kErrorInvalidData;
        return;
    }

    const std::string_view error_code = database.addUser(new_user);
    result->error_code = error_code;

    if (error_code != proto::router::kErrorOk)
    {
        LOG(ERROR) << "addUser failed:" << error_code;
        return;
    }

    result->notify_flags = ClientWorker::NOTIFY_USERS;
}

//--------------------------------------------------------------------------------------------------
void handleModify(Database& database, const proto::router::User& user, Result* result)
{
    LOG(INFO) << "User modify request:" << user.name();

    if (user.entry_id() <= 0)
    {
        LOG(ERROR) << "Invalid user ID:" << user.entry_id();
        result->error_code = proto::router::kErrorInvalidData;
        return;
    }

    RouterUser new_user = RouterUser::parseFrom(user);

    // A request with empty credentials changes only the flags of the record; the stored credentials
    // and the name are kept (see Database::modifyUser), so the fields it does not write are not
    // validated.
    const bool has_credentials = !new_user.salt.isEmpty() || !new_user.verifier.isEmpty();
    if (has_credentials && !new_user.isValid())
    {
        LOG(ERROR) << "Invalid user record:" << new_user.name;
        result->error_code = proto::router::kErrorInvalidData;
        return;
    }

    // The user update happens in one transaction inside modifyUser, which decides authoritatively
    // whether the password rotation happened at all.
    bool password_changed = false;
    bool sessions_changed = false;
    const std::string_view error_code =
        database.modifyUser(new_user, &password_changed, &sessions_changed);
    result->error_code = error_code;

    if (error_code != proto::router::kErrorOk)
    {
        LOG(ERROR) << "modifyUser failed:" << error_code;
        return;
    }

    const bool disabled = !(new_user.flags & User::ENABLED);
    if (password_changed || sessions_changed || disabled)
        result->stop_user_id = new_user.entry_id;

    result->notify_flags = ClientWorker::NOTIFY_USERS;
}

//--------------------------------------------------------------------------------------------------
void handleDelete(Database& database, const proto::router::User& user, Result* result)
{
    const qint64 entry_id = user.entry_id();

    LOG(INFO) << "User remove request:" << entry_id;

    const std::string_view error_code = database.removeUser(entry_id);
    result->error_code = error_code;

    if (error_code != proto::router::kErrorOk)
    {
        LOG(ERROR) << "removeUser failed:" << error_code;
        return;
    }

    result->stop_user_id = entry_id;

    // The cascade dropped the user's access entries and moved the revisions of the affected
    // workspaces, so the cached workspace lists are stale too.
    result->notify_flags = ClientWorker::NOTIFY_USERS | ClientWorker::NOTIFY_WORKSPACES;
}

//--------------------------------------------------------------------------------------------------
// Only the descriptive fields of a record are listed. The credential material never leaves the
// router.
void serializeUser(const RouterUser& user, proto::router::User* out)
{
    out->set_entry_id(user.entry_id);
    out->set_name(user.name.toStdString());
    out->set_group(user.group.toStdString());
    out->set_sessions(user.sessions);
    out->set_flags(user.flags);
}

} // namespace

//--------------------------------------------------------------------------------------------------
RequestResult handleUserRequest(Database& database, const RequestCaller& caller,
                                const proto::router::UserRequest& request)
{
    RequestResult result;
    const std::string& command_name = request.command_name();

    if (command_name == proto::router::kCommandUserAdd)
    {
        handleAdd(database, request.user(), &result);
    }
    else if (command_name == proto::router::kCommandUserModify)
    {
        handleModify(database, request.user(), &result);
    }
    else if (command_name == proto::router::kCommandUserDelete)
    {
        handleDelete(database, request.user(), &result);
    }
    else
    {
        LOG(ERROR) << "Unknown user request command:" << command_name;
        result.error_code = proto::router::kErrorInvalidRequest;
    }

    return result;
}

//--------------------------------------------------------------------------------------------------
void handleUserList(Database& database, const proto::router::UserListRequest& request,
                    proto::router::UserList* out)
{
    const qint64 entry_id = request.entry_id();
    const std::string& name = request.name();

    if (entry_id != 0 && !name.empty())
    {
        LOG(ERROR) << "Ambiguous user lookup: both entry_id and name are set";
        out->set_error_code(proto::router::kErrorInvalidRequest);
        return;
    }

    if (entry_id != 0 || !name.empty())
    {
        RouterUser user;
        const std::string_view error_code = entry_id != 0 ?
            database.findUser(entry_id, &user) : database.findUser(QString::fromStdString(name), &user);

        // A lookup asks whether the record is there, so a miss is an empty list and not an error.
        // total_count stays unset, because a lookup says nothing about the size of the list.
        if (error_code == proto::router::kErrorOk)
            serializeUser(user, out->add_user());
        else if (error_code != proto::router::kErrorNotFound)
        {
            out->set_error_code(error_code);
            return;
        }

        out->set_error_code(proto::router::kErrorOk);
        return;
    }

    // A zero count from a failed query would make the client truncate its pagination while the
    // page itself arrives non-empty, so a count failure fails the whole request.
    qint64 total_count = 0;
    const std::string_view count_code = database.userCount(&total_count);
    if (count_code != proto::router::kErrorOk)
    {
        out->set_error_code(count_code);
        return;
    }

    std::vector<RouterUser> users;
    const std::string_view list_code = database.userList(request.offset(), request.count(), &users);
    if (list_code != proto::router::kErrorOk)
    {
        out->set_error_code(list_code);
        return;
    }

    out->set_total_count(total_count);

    for (const RouterUser& user : users)
        serializeUser(user, out->add_user());

    out->set_error_code(proto::router::kErrorOk);
}

//--------------------------------------------------------------------------------------------------
RequestResult handleChangePassword(Database& database, const RequestCaller& caller,
                                   const proto::router::ChangePasswordRequest& request)
{
    RequestResult result;

    // Read-modify-write outside a transaction: the window between this findUser and the modifyUser
    // below is closed only by every users/workspaces write going through the single ClientWorker
    // thread. If client sessions are ever spread over several workers, this must move inside one
    // transaction.
    RouterUser user;
    const std::string_view find_error_code = database.findUser(caller.user_id, &user);
    if (find_error_code != proto::router::kErrorOk)
    {
        LOG(WARNING) << "Failed to read authenticated user (user_id:" << caller.user_id
                     << "):" << find_error_code;
        result.error_code = find_error_code;
        return result;
    }

    // Replace only the password-derived fields; keep name, group, sessions, flags intact.
    user.salt     = QByteArray::fromStdString(request.salt());
    user.verifier = QByteArray::fromStdString(request.verifier());

    if (!user.isValid())
    {
        LOG(ERROR) << "Rotated credentials produced an invalid user record";
        result.error_code = proto::router::kErrorInvalidData;
        return result;
    }

    // Only the password-derived fields differ here (the rest were loaded from the database), so
    // reusing modifyUser writes back identical values.
    const std::string_view error_code = database.modifyUser(user);
    result.error_code = error_code;

    if (error_code != proto::router::kErrorOk)
    {
        LOG(ERROR) << "Failed to change password for user" << caller.name << ":" << error_code;
        return result;
    }

    // NOTIFY_USERS only: the rotation touches the user row; the workspaces are not involved.
    result.notify_flags = ClientWorker::NOTIFY_USERS;
    return result;
}
