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

#include "router/workers/client_worker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "base/serialization.h"
#include "base/version_constants.h"
#include "router/client_admin.h"
#include "router/client_operator.h"
#include "router/fake_tcp_channel.h"
#include "router/router_test_base.h"
#include "router/router_test_worker.h"

namespace {

// The live sessions of a router: two ordinary ones and the administrator console that sends the
// commands (the last id).
const std::vector<qint64> kSessions = { 1, 2, 7 };
constexpr qint64 kAdminSession = 7;
constexpr qint64 kAllSessions = -1;

} // namespace

//--------------------------------------------------------------------------------------------------
// "Disconnect everybody" must leave the console that asked for it alone: the administrator would
// otherwise drop its own connection and have to sign in again to see the result of its own
// command. Every other command of the admin channel that acts on sessions follows the same rule.
TEST(ClientWorkerTest, DisconnectAllSpareTheRequestingSession)
{
    const std::vector<qint64> targets =
        ClientWorker::sessionsToStop(kSessions, kAllSessions, kAdminSession);

    EXPECT_EQ(targets, std::vector<qint64>({ 1, 2 }));
}

//--------------------------------------------------------------------------------------------------
// Nothing else is connected: the command has nothing to do, which is not a failure.
TEST(ClientWorkerTest, DisconnectAllWithNobodyElseSelectsNothing)
{
    const std::vector<qint64> targets =
        ClientWorker::sessionsToStop({ kAdminSession }, kAllSessions, kAdminSession);

    EXPECT_TRUE(targets.empty());
}

//--------------------------------------------------------------------------------------------------
TEST(ClientWorkerTest, DisconnectOneTargetsExactlyThatSession)
{
    const std::vector<qint64> targets = ClientWorker::sessionsToStop(kSessions, 2, kAdminSession);

    EXPECT_EQ(targets, std::vector<qint64>({ 2 }));
}

//--------------------------------------------------------------------------------------------------
// Picking your own session in the list is an explicit decision, so it is carried out.
TEST(ClientWorkerTest, OwnSessionCanBePickedExplicitly)
{
    const std::vector<qint64> targets =
        ClientWorker::sessionsToStop(kSessions, kAdminSession, kAdminSession);

    EXPECT_EQ(targets, std::vector<qint64>({ kAdminSession }));
}

//--------------------------------------------------------------------------------------------------
// The session ended between the list and the command: nothing is selected, and the caller answers
// with an invalid entry id instead of silently reporting success.
TEST(ClientWorkerTest, UnknownSessionSelectsNothing)
{
    EXPECT_TRUE(ClientWorker::sessionsToStop(kSessions, 12345, kAdminSession).empty());
    EXPECT_TRUE(ClientWorker::sessionsToStop({}, 1, kAdminSession).empty());
}

// The list of live sessions, the stop command and the timer are private to the worker; the peer
// reaches them so they can be exercised against real sessions.
class ClientWorkerTestPeer
{
public:
    static std::vector<ClientOperator*>& clients(ClientWorker& worker) { return worker.clients_; }

    static void stopClients(ClientWorker& worker, qint64 user_id)
    {
        worker.onStopClients(user_id);
    }

    static void fireTimer(ClientWorker& worker, TimePoint now) { worker.onTimer(now); }
    static void notifyChanged(ClientWorker& worker, quint32 flags) { worker.onNotifyChanged(flags); }
    static void updateClientsMask(ClientWorker& worker) { worker.updateClientsMask(); }

    // The removal of a session that ended on its own, wired the way onNewConnection does it.
    // The connection is made direct explicitly, because the worker of the stand is never
    // started and sits on its own thread, so an automatic connection would queue forever.
    static void connectFinished(ClientWorker& worker, ClientOperator* client)
    {
        QObject::connect(client, &ClientOperator::sig_finished,
                         &worker, &ClientWorker::onSessionFinished, Qt::DirectConnection);
    }
    static quint32 clientsMask(ClientWorker& worker) { return worker.clients_mask_; }
    static void stop(ClientWorker& worker) { worker.onStop(); }

    static bool isUserLimitReached(ClientWorker& worker, qint64 user_id)
    {
        return worker.isUserLimitReached(user_id);
    }
};

// The stop command against live sessions. The sessions are the real ClientOperator over the fake
// channel, created and stopped in the worker thread the way the router does it. The worker object
// itself is never started: the command is a plain call, and the thread the sessions need is the
// test worker's.
class ClientWorkerStopTest : public RouterTestBase
{
protected:
    void SetUp() override
    {
        RouterTestBase::SetUp();

        std::unique_ptr<RouterTestWorker> worker = std::make_unique<RouterTestWorker>(file_path_);
        worker_ = worker.get();

        workers_.add(std::move(worker));
        workers_.start();
    }

    // A signed-in session of |user| held by the worker. Of the wiring onNewConnection sets up, the
    // stand reproduces the mask update on authentication and the removal on sig_finished. Runs in
    // the worker thread.
    template <typename ClientT = ClientOperator>
    ClientT* addSession(ClientWorker& worker, const RouterUser& user,
                        FakeTcpChannel** channel_out = nullptr,
                        quint32 session_type = proto::router::SESSION_TYPE_OPERATOR)
    {
        FakeTcpChannel* channel = new FakeTcpChannel();
        channel->setPeer(user.entry_id, user.name.toStdString(), session_type,
                         kVersion_3_0_0);
        if (channel_out)
            *channel_out = channel;

        ClientT* client = new ClientT(worker_->database(), channel, nullptr);
        ClientWorkerTestPeer::clients(worker).push_back(client);
        QObject::connect(client, &ClientOperator::sig_authenticated, client,
                         [&worker]() { ClientWorkerTestPeer::updateClientsMask(worker); });
        ClientWorkerTestPeer::connectFinished(worker, client);

        client->start();
        EXPECT_TRUE(client->isAuthenticated());

        return client;
    }

    static bool holds(ClientWorker& worker, const ClientOperator* client)
    {
        const std::vector<ClientOperator*>& clients = ClientWorkerTestPeer::clients(worker);
        return std::ranges::find(clients, client) != clients.end();
    }

    // Destroyed before the database and the temporary directory of the base fixture: the worker
    // thread must be gone before the file it works with.
    WorkerManager workers_;
    RouterTestWorker* worker_ = nullptr;
};

//--------------------------------------------------------------------------------------------------
// A stop of the user takes every session of theirs and leaves the sessions of other users alone.
TEST_F(ClientWorkerStopTest, UserStopTakesEverySessionOfTheUser)
{
    const RouterUser bob = addUser("bob", proto::router::SESSION_TYPE_OPERATOR);
    ASSERT_GT(bob.entry_id, 0);

    worker_->invoke([&]()
    {
        ClientWorker worker;

        ClientOperator* first = addSession(worker, admin_);
        ClientOperator* second = addSession(worker, admin_);
        ClientOperator* other_user = addSession(worker, bob);

        ClientWorkerTestPeer::stopClients(worker, admin_.entry_id);

        EXPECT_FALSE(holds(worker, first));
        EXPECT_FALSE(holds(worker, second));
        EXPECT_TRUE(holds(worker, other_user));

        ClientWorkerTestPeer::stop(worker);
    });
}

//--------------------------------------------------------------------------------------------------
// An account may not pile up connections without end. The cap is reached by the connections of one
// account only, and the next one is refused instead of costing a live session.
TEST_F(ClientWorkerStopTest, ConnectionsOfOneAccountAreCapped)
{
    const RouterUser bob = addUser("bob", proto::router::SESSION_TYPE_OPERATOR);
    ASSERT_GT(bob.entry_id, 0);

    worker_->invoke([&]()
    {
        ClientWorker worker;

        addSession(worker, bob);

        for (size_t i = 0; i < ClientWorker::kMaxClientsPerUser - 1; ++i)
            addSession(worker, admin_);

        EXPECT_FALSE(ClientWorkerTestPeer::isUserLimitReached(worker, admin_.entry_id));

        addSession(worker, admin_);

        EXPECT_TRUE(ClientWorkerTestPeer::isUserLimitReached(worker, admin_.entry_id));
        EXPECT_FALSE(ClientWorkerTestPeer::isUserLimitReached(worker, bob.entry_id));

        ClientWorkerTestPeer::stop(worker);
    });
}

//--------------------------------------------------------------------------------------------------
// The channel error is the only notice the worker gets of a socket that died under a session,
// and it must take exactly that session out of the list.
TEST_F(ClientWorkerStopTest, SocketDeathDropsOnlyTheDeadSession)
{
    worker_->invoke([&]()
    {
        ClientWorker worker;

        FakeTcpChannel* channel = nullptr;
        ClientOperator* dying = addSession(worker, admin_, &channel);
        ClientOperator* kept = addSession(worker, admin_);

        channel->fail(TcpChannel::ErrorCode::REMOTE_HOST_CLOSED);

        EXPECT_FALSE(holds(worker, dying));
        EXPECT_TRUE(holds(worker, kept));

        ClientWorkerTestPeer::stop(worker);
    });
}

//--------------------------------------------------------------------------------------------------
// The periodic notifications are data of a signed-in session: the dirty host list reaches the
// clients that are connected.
TEST_F(ClientWorkerStopTest, HostNotificationReachesTheSession)
{
    worker_->invoke([&]()
    {
        ClientWorker worker;

        FakeTcpChannel* channel = nullptr;
        addSession(worker, admin_, &channel);

        channel->clearSent();

        ClientWorkerTestPeer::notifyChanged(worker, ClientWorker::NOTIFY_HOSTS);
        ClientWorkerTestPeer::fireTimer(worker, Clock::now());

        ASSERT_EQ(channel->sent().size(), 1);
        proto::router::RouterToClient message;
        ASSERT_TRUE(parse(channel->sent().at(0).buffer, &message));
        ASSERT_TRUE(message.has_notification());
        EXPECT_TRUE(message.notification().hosts_dirty());

        ClientWorkerTestPeer::stop(worker);
    });
}

//--------------------------------------------------------------------------------------------------
// The user lists only the administrators watch ride the admin flush. The notification of an
// administrator carries the bit, and an operator hears nothing of it at all.
TEST_F(ClientWorkerStopTest, UsersDirtyReachesTheAdminSessions)
{
    worker_->invoke([&]()
    {
        ClientWorker worker;

        FakeTcpChannel* admin_channel = nullptr;
        FakeTcpChannel* operator_channel = nullptr;
        addSession<ClientAdmin>(worker, admin_, &admin_channel,
                                proto::router::SESSION_TYPE_ADMIN);
        addSession(worker, admin_, &operator_channel);

        admin_channel->clearSent();
        operator_channel->clearSent();

        ClientWorkerTestPeer::notifyChanged(worker, ClientWorker::NOTIFY_USERS);
        ClientWorkerTestPeer::fireTimer(worker, Clock::now());

        ASSERT_EQ(admin_channel->sent().size(), 1);
        proto::router::RouterToClient message;
        ASSERT_TRUE(parse(admin_channel->sent().at(0).buffer, &message));
        ASSERT_TRUE(message.has_notification());
        EXPECT_TRUE(message.notification().users_dirty());

        EXPECT_TRUE(operator_channel->nothingSent());

        ClientWorkerTestPeer::stop(worker);
    });
}

//--------------------------------------------------------------------------------------------------
// The kinds mask feeds the relay statistics polling, so it counts the sessions that are signed in,
// by type.
TEST_F(ClientWorkerStopTest, ClientsMaskFollowsTheSessionTypes)
{
    worker_->invoke([&]()
    {
        ClientWorker worker;

        addSession<ClientAdmin>(worker, admin_, nullptr, proto::router::SESSION_TYPE_ADMIN);
        addSession(worker, admin_);

        EXPECT_EQ(ClientWorkerTestPeer::clientsMask(worker),
                  quint32(ClientWorker::CLIENT_ADMINS | ClientWorker::CLIENT_OPERATORS));

        ClientWorkerTestPeer::stop(worker);
    });
}
