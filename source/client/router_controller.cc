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

#include "client/router_controller.h"

#include <QSet>

#include <limits>

#include "base/gui_application.h"
#include "base/logging.h"
#include "base/serialization.h"
#include "client/config.h"
#include "client/database.h"
#include "client/router_hosts_cleaner.h"
#include "client/workers/router_worker.h"
#include "proto/router.h"
#include "proto/router_constants.h"

namespace {

RouterController* g_instance = nullptr;

} // namespace

//--------------------------------------------------------------------------------------------------
RouterController::RouterController(QObject* parent)
    : QObject(parent)
{
    CHECK(!g_instance);
    g_instance = this;

    router_worker_ = GuiApplication::findWorker<RouterWorker>();
    if (!router_worker_)
    {
        LOG(ERROR) << "Router worker not found";
        return;
    }

    connect(router_worker_, &RouterWorker::sig_authenticated,
            this, &RouterController::onRouterAuthenticated, Qt::QueuedConnection);
    connect(router_worker_, &RouterWorker::sig_reconnecting,
            this, &RouterController::onRouterReconnecting, Qt::QueuedConnection);
    connect(router_worker_, &RouterWorker::sig_errorOccurred,
            this, &RouterController::onRouterError, Qt::QueuedConnection);
    connect(router_worker_, &RouterWorker::sig_messageReceived,
            this, &RouterController::onRouterMessage, Qt::QueuedConnection);
}

//--------------------------------------------------------------------------------------------------
RouterController::~RouterController()
{
    for (auto& [router_id, context] : contexts_)
    {
        if (context.session)
            context.session->rpc_.dropPending();

        if (router_worker_)
        {
            QMetaObject::invokeMethod(router_worker_, &RouterWorker::onDisconnect,
                                      Qt::QueuedConnection, router_id);
        }
    }

    g_instance = nullptr;
}

//--------------------------------------------------------------------------------------------------
// static
RouterController& RouterController::instance()
{
    CHECK(g_instance);
    return *g_instance;
}

//--------------------------------------------------------------------------------------------------
// static
RouterStatus RouterController::status(qint64 router_id)
{
    const RouterController& controller = instance();

    auto it = controller.contexts_.find(router_id);
    if (it == controller.contexts_.end())
        return RouterStatus::OFFLINE;

    const RouterContext& context = it->second;
    if (context.session)
        return RouterStatus::ONLINE;

    if (context.logging_in)
        return RouterStatus::CONNECTING;

    if (!context.config || !context.config->isValid())
        return RouterStatus::UNREADABLE;

    return RouterStatus::OFFLINE;
}

//--------------------------------------------------------------------------------------------------
// static
RouterSession* RouterController::session(qint64 router_id)
{
    const RouterController& controller = instance();

    auto it = controller.contexts_.find(router_id);
    return it != controller.contexts_.end() ? it->second.session.get() : nullptr;
}

//--------------------------------------------------------------------------------------------------
void RouterController::reload()
{
    QList<RouterConfig> configs;
    if (Database::instance().routerList(&configs) == Database::ReadResult::FAILED)
    {
        LOG(ERROR) << "Failed to read the router list - keeping the current state";
        return;
    }

    QSet<qint64> present;
    present.reserve(configs.size());
    for (const RouterConfig& config : configs)
        present.insert(config.routerId());

    // The dead records go first. Whatever they run leaves with the context.
    QList<qint64> known;
    for (const auto& entry : contexts_)
        known.append(entry.first);
    for (qint64 router_id : std::as_const(known))
    {
        if (present.contains(router_id))
            continue;

        auto context_it = contexts_.find(router_id);
        if (context_it != contexts_.end() && context_it->second.temporary)
            continue;

        contexts_.erase(router_id);

        if (router_worker_)
        {
            QMetaObject::invokeMethod(router_worker_, &RouterWorker::onDisconnect,
                                      Qt::QueuedConnection, router_id);
        }

        updateStatus(router_id);
    }

    // Connect the new records, refresh the configuration of the ones already running.
    for (const RouterConfig& config : configs)
    {
        const qint64 router_id = config.routerId();

        auto it = contexts_.find(router_id);
        if (it != contexts_.end())
        {
            RouterContext& context = it->second;

            if (!config.isValid())
            {
                if (!context.session)
                {
                    context.logging_in = false;
                    context.peer_version = QVersionNumber();
                    context.config.reset(new RouterConfig(config));

                    if (router_worker_)
                    {
                        QMetaObject::invokeMethod(router_worker_, &RouterWorker::onDisconnect,
                                                  Qt::QueuedConnection, router_id);
                    }

                    updateStatus(router_id);
                }
                continue;
            }

            // A rename keeps what is running. Another account or another session type replaces
            // it whole: the login and the session belong to the account and the type they were
            // opened with. A context that runs nothing yet waits for its reconnect barrier and
            // starts with the config left here.
            if (context.config->hasSameAccount(config) &&
                context.config->sessionType() == config.sessionType())
            {
                // The instance is shared with whoever runs, so the edit is seen at once.
                *context.config = config;
                continue;
            }

            context.session.reset();
            context.logging_in = false;
            context.peer_version = QVersionNumber();
            context.config.reset(new RouterConfig(config));

            if (router_worker_)
            {
                addEvent(router_id, RouterEvent::Severity::INFO,
                         tr("Connecting to router %1...").arg(config.address()));
                QMetaObject::invokeMethod(router_worker_, &RouterWorker::onReconnect,
                                          Qt::QueuedConnection, router_id);
            }
            else
            {
                startLogin(router_id);
            }

            updateStatus(router_id);
            continue;
        }

        contexts_[router_id].config.reset(new RouterConfig(config));

        if (!config.isValid())
        {
            updateStatus(router_id);
            continue;
        }

        startLogin(router_id);

        if (router_worker_)
        {
            addEvent(router_id, RouterEvent::Severity::INFO,
                     tr("Connecting to router %1...").arg(config.address()));
            QMetaObject::invokeMethod(router_worker_, &RouterWorker::onConnect,
                                      Qt::QueuedConnection, router_id);
        }

        updateStatus(router_id);
    }
}

//--------------------------------------------------------------------------------------------------
qint64 RouterController::addTemporaryRouter(const QString& address, const QString& user_name,
                                            const QString& password)
{
    if (!router_worker_)
    {
        LOG(ERROR) << "Router worker not found";
        return -1;
    }

    // Ids of the stored records are autoincrement values and are small. Counting down from the top
    // of the range cannot collide with them.
    static qint64 next_id = std::numeric_limits<qint64>::max();
    const qint64 router_id = --next_id;

    RouterConfig config;
    config.setRouterId(router_id);
    config.setAddress(address);
    config.setUsername(user_name);
    config.setPassword(SecureString(password));
    config.setSessionType(proto::router::SESSION_TYPE_OPERATOR);

    if (!config.isValid())
    {
        LOG(ERROR) << "The router account passed on the command line is incomplete";
        return -1;
    }

    RouterContext& context = contexts_[router_id];
    context.config.reset(new RouterConfig(config));
    context.temporary = true;

    LOG(INFO) << "Connecting to router from the command line:" << address;
    startLogin(router_id);
    addEvent(router_id, RouterEvent::Severity::INFO,
             tr("Connecting to router %1...").arg(address));

    QMetaObject::invokeMethod(router_worker_, &RouterWorker::onConnectWithConfig,
                              Qt::QueuedConnection, router_id, address, user_name, password,
                              static_cast<quint32>(proto::router::SESSION_TYPE_OPERATOR));
    updateStatus(router_id);

    return router_id;
}

//--------------------------------------------------------------------------------------------------
QList<RouterEvent> RouterController::events(qint64 router_id) const
{
    auto it = contexts_.find(router_id);
    return it != contexts_.end() ? it->second.events : QList<RouterEvent>();
}

//--------------------------------------------------------------------------------------------------
void RouterController::clearEvents(qint64 router_id)
{
    auto it = contexts_.find(router_id);
    if (it != contexts_.end())
        it->second.events.clear();

    addEvent(router_id, RouterEvent::Severity::INFO, tr("Event history cleared."));
}

//--------------------------------------------------------------------------------------------------
void RouterController::addEvent(qint64 router_id, RouterEvent::Severity severity,
                               const QString& text)
{
    const RouterEvent event{ QDateTime::currentDateTime(), severity, text };

    auto it = contexts_.find(router_id);
    if (it != contexts_.end())
    {
        it->second.events.append(event);

        while (it->second.events.size() > kMaxStoredEvents)
            it->second.events.removeFirst();
    }

    emit sig_event(router_id, event);
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRouterAuthenticated(qint64 router_id, const QVersionNumber& peer_version)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end() || !it->second.logging_in)
        return;

    LOG(INFO) << "Connected to router" << router_id;
    it->second.peer_version = peer_version;
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRouterReconnecting(qint64 router_id)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end() || it->second.session)
        return;

    startLogin(router_id);
    updateStatus(router_id);
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRouterError(qint64 router_id, TcpChannel::ErrorCode error_code)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end())
        return;

    RouterContext& context = it->second;

    RouterEvent::Severity severity = RouterEvent::Severity::WARNING;
    if (error_code == TcpChannel::ErrorCode::CRYPTO_ERROR ||
        error_code == TcpChannel::ErrorCode::ACCESS_DENIED)
    {
        severity = RouterEvent::Severity::CRITICAL;
    }

    addEvent(router_id, severity,
             tr("Network error: %1").arg(TcpChannel::errorToString(error_code)));

    // A login in progress survives the connection: the worker keeps the reconnect cycle running
    // by itself and the login starts over once the channel is up again.
    if (context.logging_in)
    {
        context.peer_version = QVersionNumber();
        return;
    }

    if (!context.session)
        return;

    addEvent(router_id, RouterEvent::Severity::WARNING,
             tr("Disconnected from router %1.").arg(context.session->config().address()));

    // The death of the session answers everything it still owes. The worker keeps the
    // reconnect cycle running by itself, so the record goes back to logging in.
    context.session.reset();

    if (!context.temporary)
    {
        RouterConfig config;
        const Database::FindResult found = Database::instance().findRouter(router_id, &config);
        if (found == Database::FindResult::UNREADABLE)
        {
            LOG(ERROR) << "Data of router" << router_id << "could not be read";
            context.config.reset(new RouterConfig(config));
            context.logging_in = false;
            context.peer_version = QVersionNumber();

            if (router_worker_)
            {
                QMetaObject::invokeMethod(router_worker_, &RouterWorker::onDisconnect,
                                          Qt::QueuedConnection, router_id);
            }

            updateStatus(router_id);
            return;
        }

        if (found == Database::FindResult::FOUND && config.isValid())
        {
            *context.config = config;
        }
        else
        {
            LOG(WARNING) << "Failed to re-read record (router_id:" << router_id
                         << "): using the in-memory copy";
        }
    }

    addEvent(router_id, RouterEvent::Severity::INFO,
             tr("Connecting to router %1...").arg(context.config->address()));
    startLogin(router_id);
    updateStatus(router_id);
}

//--------------------------------------------------------------------------------------------------
void RouterController::onRouterMessage(qint64 router_id, quint8 channel_id, const QByteArray& buffer)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end())
        return;

    RouterSession* session = it->second.session.get();
    if (!session)
    {
        // Before the session is born only the login answer is expected.
        if (channel_id != proto::router::CHANNEL_ID_CLIENT)
        {
            LOG(WARNING) << "Unexpected message from channel" << channel_id << "during login";
            return;
        }

        proto::router::RouterToClient message;
        if (!parse(buffer, &message))
        {
            LOG(ERROR) << "Unable to parse client message";
            return;
        }

        if (message.has_login_result())
            finishLogin(router_id, message.login_result().user_id());
        else
            LOG(WARNING) << "Unhandled client message during login";

        return;
    }

    if (channel_id == proto::router::CHANNEL_ID_ADMIN)
    {
        proto::router::RouterToAdmin message;
        if (!parse(buffer, &message))
        {
            LOG(ERROR) << "Unable to parse admin message";
            return;
        }

        session->onMessageReceived(message);
    }
    else if (channel_id == proto::router::CHANNEL_ID_MANAGER)
    {
        proto::router::RouterToManager message;
        if (!parse(buffer, &message))
        {
            LOG(ERROR) << "Unable to parse manager message";
            return;
        }

        session->onMessageReceived(message);
    }
    else if (channel_id == proto::router::CHANNEL_ID_CLIENT)
    {
        proto::router::RouterToClient message;
        if (!parse(buffer, &message))
        {
            LOG(ERROR) << "Unable to parse client message";
            return;
        }

        session->onMessageReceived(message);

        // The signals go after the handoff, so a subscriber that refetches on one of them is
        // not served the lists the notification just outdated.
        if (message.has_notification())
        {
            const proto::router::Notification& notification = message.notification();
            if (notification.temp_hosts_dirty())
                emit sig_tempHostsChanged(router_id);
            if (notification.hosts_dirty())
                emit sig_hostsChanged(router_id);
            if (notification.relays_dirty())
                emit sig_relaysChanged(router_id);
            if (notification.clients_dirty())
                emit sig_clientsChanged(router_id);
            if (notification.users_dirty())
                emit sig_usersChanged(router_id);
            if (notification.workspaces_dirty())
                emit sig_workspacesChanged(router_id);
            if (notification.groups_dirty())
                emit sig_groupsChanged(router_id);
        }
    }
    else
    {
        LOG(WARNING) << "Unexpected message from channel" << channel_id;
    }
}

//--------------------------------------------------------------------------------------------------
void RouterController::startLogin(qint64 router_id)
{
    auto it = contexts_.find(router_id);
    CHECK(it != contexts_.end() && it->second.config);
    RouterContext& context = it->second;

    context.logging_in = true;
    context.peer_version = QVersionNumber();
}

//--------------------------------------------------------------------------------------------------
void RouterController::finishLogin(qint64 router_id, qint64 user_id)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end() || !it->second.logging_in)
        return;

    RouterContext& context = it->second;
    context.logging_in = false;

    RouterConfig config;

    if (!context.temporary)
    {
        if (Database::instance().findRouter(router_id, &config) == Database::FindResult::FOUND &&
            config.isValid())
        {
            *context.config = config;
        }
        else
        {
            LOG(WARNING) << "Failed to re-read record (router_id:" << router_id
                         << "): using the in-memory copy";
        }
    }

    LOG(INFO) << "Login completed for router" << router_id;
    context.session = new RouterSession(context.config, user_id, context.peer_version, this);

    addEvent(router_id, RouterEvent::Severity::INFO,
             tr("Connection to router %1 established.").arg(context.config->address()));
    updateStatus(router_id);

    // A router the caller supplied has no record of its own, so there is nothing to clean up here.
    if (context.temporary)
        return;

    // The saved credentials of hosts this router no longer knows are of no use to anyone.
    RouterHostsCleaner* cleaner = new RouterHostsCleaner(router_id, this);
    connect(cleaner, &RouterHostsCleaner::sig_finished, cleaner, &QObject::deleteLater);
    cleaner->start();
}

//--------------------------------------------------------------------------------------------------
void RouterController::updateStatus(qint64 router_id)
{
    auto it = contexts_.find(router_id);
    if (it == contexts_.end())
    {
        // The record just left. Its removal is the one transition announced without a context.
        emit sig_statusChanged(router_id, RouterStatus::OFFLINE);
        return;
    }

    const RouterStatus current = status(router_id);
    if (it->second.last_status == current)
        return;
    it->second.last_status = current;

    emit sig_statusChanged(router_id, current);
}
