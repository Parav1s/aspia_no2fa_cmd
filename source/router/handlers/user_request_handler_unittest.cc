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

#include "router/handlers/user_request_handler.h"

#include "proto/router_admin.h"
#include "router/router_test_base.h"
#include "router/workers/client_worker.h"

// The whole user surface of the admin channel, exercised against an isolated database in the state
// --create-config leaves behind: the built-in administrator with id 1.
class UserRequestHandlerTest : public RouterTestBase
{
protected:
    RequestResult handle(const proto::router::UserRequest& request)
    {
        return handleUserRequest(db_, caller_, request);
    }

    // A request carrying the full record: name, credentials and flags.
    proto::router::UserRequest makeRequest(std::string_view command, const RouterUser& user)
    {
        proto::router::UserRequest request;
        request.set_command_name(std::string(command));
        request.mutable_user()->CopyFrom(user.serialize());
        return request;
    }

    // A request that only carries the id (delete).
    proto::router::UserRequest makeIdRequest(std::string_view command, qint64 user_id)
    {
        proto::router::UserRequest request;
        request.set_command_name(std::string(command));
        request.mutable_user()->set_entry_id(user_id);
        return request;
    }
};

//--------------------------------------------------------------------------------------------------
// A user that is not an administrator changes nothing about the workspaces, so only the user list
// is announced as stale.
TEST_F(UserRequestHandlerTest, AddClientNotifiesUsersOnly)
{
    const RequestResult result = handle(makeRequest(
        proto::router::kCommandUserAdd,
        makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));
    EXPECT_EQ(result.stop_user_id, 0);
    EXPECT_TRUE(findUser("bob").isValid());
}

//--------------------------------------------------------------------------------------------------
// A password rotation answers a leaked password: every live session of the user is dropped.
TEST_F(UserRequestHandlerTest, PasswordRotationStopsSessions)
{
    ASSERT_EQ(db_.addUser(makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)),
              proto::router::kErrorOk);
    const qint64 user_id = findUser("bob").entry_id;

    RouterUser rotated = RouterUser::create("bob", SecureString("Rotated1234!"));
    rotated.entry_id = user_id;
    rotated.sessions = proto::router::SESSION_TYPE_OPERATOR;
    rotated.flags = User::ENABLED;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, rotated));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.stop_user_id, user_id);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));
}

//--------------------------------------------------------------------------------------------------
// A request without credentials edits only the flags. Nothing here invalidates a session, so
// nobody is dropped.
TEST_F(UserRequestHandlerTest, PropertyEditStopsNobody)
{
    ASSERT_EQ(db_.addUser(makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)),
              proto::router::kErrorOk);
    const qint64 user_id = findUser("bob").entry_id;

    RouterUser flags_only;
    flags_only.entry_id = user_id;
    flags_only.flags = User::ENABLED;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, flags_only));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.stop_user_id, 0);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));
}

//--------------------------------------------------------------------------------------------------
// A disabled account must not keep the sessions it opened while it was enabled.
TEST_F(UserRequestHandlerTest, DisablingAUserStopsTheirSessions)
{
    ASSERT_EQ(db_.addUser(makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)),
              proto::router::kErrorOk);
    const qint64 user_id = findUser("bob").entry_id;

    RouterUser disabled;
    disabled.entry_id = user_id;
    disabled.flags = 0;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, disabled));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.stop_user_id, user_id);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));
}

//--------------------------------------------------------------------------------------------------
// The role of a session is fixed at login, so the sessions running with the old level are dropped
// and the new one takes effect with the next login.
TEST_F(UserRequestHandlerTest, LevelChangeStopsTheSessions)
{
    ASSERT_EQ(db_.addUser(makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)),
              proto::router::kErrorOk);
    const qint64 user_id = findUser("bob").entry_id;

    RouterUser promoted;
    promoted.entry_id = user_id;
    promoted.sessions = proto::router::SESSION_TYPE_MANAGER;
    promoted.flags = User::ENABLED;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, promoted));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.stop_user_id, user_id);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));
    EXPECT_EQ(findUser(user_id).sessions,
              proto::router::SESSION_TYPE_MANAGER | proto::router::SESSION_TYPE_OPERATOR);
}

//--------------------------------------------------------------------------------------------------
// The built-in administrator is the account that recovers the router, so it cannot be disabled.
TEST_F(UserRequestHandlerTest, DisablingTheBuiltInUserIsRefused)
{
    RouterUser disabled;
    disabled.entry_id = admin_.entry_id;
    disabled.flags = 0;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, disabled));

    EXPECT_EQ(result.error_code, proto::router::kErrorAccessDenied);
    EXPECT_EQ(result.stop_user_id, 0);
    EXPECT_EQ(result.notify_flags, 0u);
    EXPECT_TRUE(findUser(admin_.entry_id).flags & User::ENABLED);
}

//--------------------------------------------------------------------------------------------------
TEST_F(UserRequestHandlerTest, ModifyOfUnknownUserIsNotFound)
{
    RouterUser gone;
    gone.entry_id = 12345;
    gone.flags = User::ENABLED;

    const RequestResult result = handle(makeRequest(proto::router::kCommandUserModify, gone));

    EXPECT_EQ(result.error_code, proto::router::kErrorNotFound);
    EXPECT_EQ(result.stop_user_id, 0);
    EXPECT_EQ(result.notify_flags, 0u);
}

//--------------------------------------------------------------------------------------------------
// The cascade of the delete drops the workspace access rows of the user, so both the user list and
// the workspace lists of the clients go stale, and the sessions of the user go down.
TEST_F(UserRequestHandlerTest, DeleteStopsSessionsAndMarksWorkspacesStale)
{
    ASSERT_EQ(db_.addUser(makeUser("bob", proto::router::SESSION_TYPE_OPERATOR)),
              proto::router::kErrorOk);
    const qint64 user_id = findUser("bob").entry_id;

    const RequestResult result = handle(makeIdRequest(proto::router::kCommandUserDelete, user_id));

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.stop_user_id, user_id);
    EXPECT_EQ(result.notify_flags,
              quint32(ClientWorker::NOTIFY_USERS | ClientWorker::NOTIFY_WORKSPACES));
    EXPECT_FALSE(findUser(user_id).isValid());
}

//--------------------------------------------------------------------------------------------------
TEST_F(UserRequestHandlerTest, DeleteOfUnknownUserIsNotFound)
{
    const RequestResult result = handle(makeIdRequest(proto::router::kCommandUserDelete, 12345));

    EXPECT_EQ(result.error_code, proto::router::kErrorNotFound);
    EXPECT_EQ(result.stop_user_id, 0);
    EXPECT_EQ(result.notify_flags, 0u);
}

//--------------------------------------------------------------------------------------------------
TEST_F(UserRequestHandlerTest, UnknownCommandIsInvalidRequest)
{
    const RequestResult result = handle(makeIdRequest("user_frobnicate", 1));

    EXPECT_EQ(result.error_code, proto::router::kErrorInvalidRequest);
    EXPECT_EQ(result.notify_flags, 0u);
    EXPECT_EQ(result.stop_user_id, 0);
}

// The listing of the user records.
class UserListTest : public RouterTestBase
{
protected:
    static proto::router::UserListRequest pageRequest(qint64 offset, qint64 count)
    {
        proto::router::UserListRequest request;
        request.set_offset(offset);
        request.set_count(count);
        return request;
    }

    proto::router::UserList list(const proto::router::UserListRequest& request)
    {
        proto::router::UserList out;
        handleUserList(db_, request, &out);
        return out;
    }
};

//--------------------------------------------------------------------------------------------------
// The page names the records to send, and total_count tells the client how many pages there are.
TEST_F(UserListTest, PageCarriesTotalCount)
{
    for (int i = 0; i < 3; ++i)
        ASSERT_TRUE(addUser(QString("user%1").arg(i), proto::router::SESSION_TYPE_OPERATOR).isValid());

    const proto::router::UserList first = list(pageRequest(0, 2));
    ASSERT_EQ(first.error_code(), proto::router::kErrorOk);
    EXPECT_EQ(first.total_count(), 4);
    ASSERT_EQ(first.user_size(), 2);
    EXPECT_EQ(first.user(0).name(), "admin");

    const proto::router::UserList second = list(pageRequest(2, 2));
    ASSERT_EQ(second.error_code(), proto::router::kErrorOk);
    EXPECT_EQ(second.total_count(), 4);
    EXPECT_EQ(second.user_size(), 2);
}

//--------------------------------------------------------------------------------------------------
TEST_F(UserListTest, UnboundedPageIsRefused)
{
    EXPECT_EQ(list(pageRequest(0, 0)).error_code(), proto::router::kErrorInvalidRequest);
    EXPECT_EQ(list(pageRequest(0, proto::router::kMaxUserPageSize + 1)).error_code(),
              proto::router::kErrorInvalidRequest);
}

//--------------------------------------------------------------------------------------------------
// A lookup answers the single record it names, and needs no page.
TEST_F(UserListTest, LookupAnswersOneRecord)
{
    const RouterUser bob = addUser("bob", proto::router::SESSION_TYPE_OPERATOR);
    ASSERT_TRUE(bob.isValid());

    proto::router::UserListRequest by_id;
    by_id.set_entry_id(bob.entry_id);

    const proto::router::UserList id_result = list(by_id);
    ASSERT_EQ(id_result.error_code(), proto::router::kErrorOk);
    ASSERT_EQ(id_result.user_size(), 1);
    EXPECT_EQ(id_result.user(0).name(), "bob");
    EXPECT_EQ(id_result.total_count(), 0);

    proto::router::UserListRequest by_name;
    by_name.set_name("bob");

    const proto::router::UserList name_result = list(by_name);
    ASSERT_EQ(name_result.error_code(), proto::router::kErrorOk);
    ASSERT_EQ(name_result.user_size(), 1);
    EXPECT_EQ(name_result.user(0).entry_id(), bob.entry_id);
}

//--------------------------------------------------------------------------------------------------
// The lookup asks whether the record is there, so a miss is an empty list.
TEST_F(UserListTest, LookupMissIsAnEmptyList)
{
    proto::router::UserListRequest request;
    request.set_name("nobody");

    const proto::router::UserList result = list(request);
    EXPECT_EQ(result.error_code(), proto::router::kErrorOk);
    EXPECT_EQ(result.user_size(), 0);
}

//--------------------------------------------------------------------------------------------------
TEST_F(UserListTest, LookupByIdAndNameAtOnceIsRefused)
{
    proto::router::UserListRequest request;
    request.set_entry_id(admin_.entry_id);
    request.set_name("admin");

    EXPECT_EQ(list(request).error_code(), proto::router::kErrorInvalidRequest);
}

//--------------------------------------------------------------------------------------------------
// The credential material of a user stays in the router whatever the listing path.
TEST_F(UserListTest, RecordsCarryNoCredentials)
{
    const proto::router::UserList result = list(pageRequest(0, 10));
    ASSERT_EQ(result.error_code(), proto::router::kErrorOk);
    ASSERT_EQ(result.user_size(), 1);

    const proto::router::User& user = result.user(0);
    EXPECT_EQ(user.entry_id(), admin_.entry_id);
    EXPECT_EQ(user.name(), "admin");
    EXPECT_TRUE(user.salt().empty());
    EXPECT_TRUE(user.verifier().empty());
}

// The rotation of a session's own password over the client channel.
class ChangePasswordTest : public RouterTestBase
{
protected:
    void SetUp() override
    {
        RouterTestBase::SetUp();

        caller_.session_type = proto::router::SESSION_TYPE_ADMIN;

        workspace_id_ = addWorkspace("alpha");
        ASSERT_GT(workspace_id_, 0);
    }

    // A rotation request built from a freshly generated set of credentials.
    proto::router::ChangePasswordRequest makeRequest(const RouterUser& rotated)
    {
        proto::router::ChangePasswordRequest request;
        request.set_salt(toStdString(rotated.salt));
        request.set_verifier(toStdString(rotated.verifier));
        return request;
    }

    qint64 workspace_id_ = 0;
};

//--------------------------------------------------------------------------------------------------
// The rotation replaces the credentials in one transaction.
TEST_F(ChangePasswordTest, RotatesCredentials)
{
    const RouterUser rotated = makeUser("admin", kAllSessions);

    proto::router::ChangePasswordRequest request = makeRequest(rotated);

    const RequestResult result = handleChangePassword(db_, caller_, request);

    EXPECT_EQ(result.error_code, proto::router::kErrorOk);
    EXPECT_EQ(result.notify_flags, quint32(ClientWorker::NOTIFY_USERS));

    const RouterUser stored = findUser(admin_.entry_id);
    EXPECT_EQ(stored.verifier, rotated.verifier);

    // The name, the session mask and the flags of the record are not the caller's to change here.
    EXPECT_EQ(stored.name, admin_.name);
    EXPECT_EQ(stored.sessions, admin_.sessions);
    EXPECT_EQ(stored.flags, admin_.flags);
}

//--------------------------------------------------------------------------------------------------
TEST_F(ChangePasswordTest, RejectsInvalidCredentials)
{
    proto::router::ChangePasswordRequest request; // No credentials at all.

    const RequestResult result = handleChangePassword(db_, caller_, request);

    EXPECT_EQ(result.error_code, proto::router::kErrorInvalidData);
    EXPECT_EQ(findUser(admin_.entry_id).verifier, admin_.verifier);
}

//--------------------------------------------------------------------------------------------------
// Every blob here is a fixed-size product of the crypto, so anything larger is not a credential.
TEST_F(ChangePasswordTest, RejectsOversizedCredentials)
{
    const RouterUser rotated = makeUser("admin", kAllSessions);
    const std::string oversized(64 * 1024, 'x');

    struct Field
    {
        const char* name;
        void (proto::router::ChangePasswordRequest::*setter)(const std::string&);
    };

    const Field fields[] =
    {
        { "salt",     &proto::router::ChangePasswordRequest::set_salt },
        { "verifier", &proto::router::ChangePasswordRequest::set_verifier },
    };

    for (const Field& field : fields)
    {
        proto::router::ChangePasswordRequest request = makeRequest(rotated);

        (request.*(field.setter))(oversized);

        const RequestResult result = handleChangePassword(db_, caller_, request);

        EXPECT_EQ(result.error_code, proto::router::kErrorInvalidData) << field.name;
        EXPECT_EQ(findUser(admin_.entry_id).verifier, admin_.verifier) << field.name;
    }
}

//--------------------------------------------------------------------------------------------------
// The account was deleted while its session was live: the answer is the same one the modify path
// gives a moment later.
TEST_F(ChangePasswordTest, OfDeletedUserIsNotFound)
{
    const RouterUser client = addUser("client",
                                      proto::router::SESSION_TYPE_OPERATOR);
    ASSERT_TRUE(client.isValid());
    setCaller(client, proto::router::SESSION_TYPE_OPERATOR);
    ASSERT_EQ(db_.removeUser(client.entry_id), proto::router::kErrorOk);

    const RouterUser rotated = makeUser("client",
                                        proto::router::SESSION_TYPE_OPERATOR);

    const RequestResult result = handleChangePassword(db_, caller_, makeRequest(rotated));

    EXPECT_EQ(result.error_code, proto::router::kErrorNotFound);
    EXPECT_EQ(result.notify_flags, 0u);
}

