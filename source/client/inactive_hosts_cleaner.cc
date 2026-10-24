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

#include "client/inactive_hosts_cleaner.h"

#include <QDateTime>

#include <utility>

#include "base/logging.h"
#include "base/time_types.h"
#include "client/config.h"
#include "client/database.h"
#include "client/router_controller.h"
#include "client/router_session.h"
#include "client/router_types.h"
#include "common/desktop/msg_box.h"
#include "proto/router_client.h"

//--------------------------------------------------------------------------------------------------
InactiveHostsCleaner::InactiveHostsCleaner(int months, QObject* parent)
    : QObject(parent),
      months_(months),
      // Calendar months: "one month ago" means the same day of the previous month, not 30 days.
      threshold_(QDateTime::currentDateTime().addMonths(-months).toSecsSinceEpoch())
{
    LOG(TRACE) << "Ctor";
}

//--------------------------------------------------------------------------------------------------
InactiveHostsCleaner::~InactiveHostsCleaner()
{
    LOG(TRACE) << "Dtor";
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::start()
{
    QList<RouterConfig> configs;
    if (Database::instance().routerList(&configs) == Database::ReadResult::FAILED)
    {
        LOG(ERROR) << "Unable to read the router list";
        emit sig_collected({}, {});
        return;
    }

    for (const RouterConfig& config : configs)
    {
        if (RouterController::session(config.routerId()))
            routers_.append({config.routerId(), config.displayLabel()});
        else
            skipped_.append(tr("%1 (not connected)").arg(config.displayLabel()));
    }

    LOG(INFO) << "Inspecting" << routers_.size() << "routers for hosts inactive for" << months_
              << "months";

    if (routers_.isEmpty())
    {
        emit sig_collected(hosts_, skipped_);
        return;
    }

    fetchNextRouter();
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::remove(const QList<InactiveHost>& hosts)
{
    pending_ = hosts;
    removed_ = 0;
    failed_ = 0;

    LOG(INFO) << "Removing" << pending_.size() << "inactive hosts";
    removeNext();
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::fetchNextRouter()
{
    if (router_index_ >= routers_.size())
    {
        LOG(INFO) << "Inactive hosts found:" << hosts_.size();
        emit sig_collected(hosts_, skipped_);
        return;
    }

    fetchPage(routers_[router_index_].id, 0);
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::fetchPage(qint64 router_id, qint64 offset)
{
    RouterSession* session = RouterController::session(router_id);
    if (!session)
    {
        markUnavailable(router_id, tr("not connected"));
        ++router_index_;
        fetchNextRouter();
        return;
    }

    proto::router::HostListRequest request;
    request.set_mode(proto::router::HostListRequest::MODE_ALL);
    request.set_offset(offset);
    request.set_count(proto::router::kMaxHostPageSize);

    session->listHosts(RouterSession::CachePolicy::RELOAD, std::move(request),
                       { this, [this, router_id, offset](const RouterHostList& list)
    {
        onHostListReceived(router_id, offset, list);
    }});
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::onHostListReceived(qint64 router_id, qint64 offset,
                                              const RouterHostList& list)
{
    if (list.error_code != proto::router::kErrorOk)
    {
        // Not being an administrator of the router and a lost session look the same from here;
        // in both cases the router is reported as skipped instead of being guessed about.
        LOG(ERROR) << "Unable to get the host list of router" << router_id << ":" << list.error_code;
        markUnavailable(router_id, list.error_code == proto::router::kErrorAccessDenied
                                      ? tr("access denied")
                                      : tr("error: %1").arg(list.error_code));
        ++router_index_;
        fetchNextRouter();
        return;
    }

    QString router_name;
    for (const Router& router : std::as_const(routers_))
    {
        if (router.id == router_id)
        {
            router_name = router.name;
            break;
        }
    }

    int skipped_unknown_age = 0;

    for (const RouterHost& host : list.hosts)
    {
        // A live host is never a candidate, whatever its last connection time says.
        if (host.online)
            continue;

        // A host that has never been connected to has no age to judge it by; it may have just been
        // added, so it is left alone.
        if (host.last_connect <= 0)
        {
            ++skipped_unknown_age;
            continue;
        }

        if (host.last_connect >= threshold_)
            continue;

        hosts_.append({router_id, router_name, host.host_id, host.display_name});
    }

    if (skipped_unknown_age > 0)
        LOG(INFO) << "Skipped" << skipped_unknown_age << "hosts that have never been connected to";

    const qint64 next_offset = offset + list.hosts.size();
    if (list.hosts.isEmpty() || next_offset >= list.total_count)
    {
        ++router_index_;
        fetchNextRouter();
        return;
    }

    fetchPage(router_id, next_offset);
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::markUnavailable(qint64 router_id, const QString& reason)
{
    for (const Router& router : std::as_const(routers_))
    {
        if (router.id == router_id)
        {
            const QString entry = tr("%1 (%2)").arg(router.name, reason);
            if (!skipped_.contains(entry))
                skipped_.append(entry);
            return;
        }
    }
}

//--------------------------------------------------------------------------------------------------
void InactiveHostsCleaner::removeNext()
{
    if (pending_.isEmpty())
    {
        LOG(INFO) << "Removed:" << removed_ << "failed:" << failed_;
        emit sig_removed(removed_, failed_);
        return;
    }

    const InactiveHost host = pending_.takeFirst();

    RouterSession* session = RouterController::session(host.router_id);
    if (!session)
    {
        LOG(ERROR) << "Router" << host.router_id << "is not connected, host" << host.host_id
                   << "is left in place";
        ++failed_;
        removeNext();
        return;
    }

    session->removeHost(host.host_id, { this, [this, host](const proto::router::HostResult& result)
    {
        if (result.error_code() == proto::router::kErrorOk ||
            result.error_code() == proto::router::kErrorNotFound)
        {
            // The router no longer knows the host - either way nothing of it should stay here.
            if (!Database::instance().removeRouterHost(host.router_id, host.host_id))
                LOG(ERROR) << "Unable to remove the saved host" << host.host_id;

            ++removed_;
        }
        else
        {
            LOG(ERROR) << "Unable to remove host" << host.host_id << ":" << result.error_code();
            ++failed_;
        }

        removeNext();
    }});
}

//--------------------------------------------------------------------------------------------------
void runInactiveHostsCleanup(QWidget* parent)
{
    const int months = Database::instance().inactiveHostsPeriod();

    auto* cleaner = new InactiveHostsCleaner(months, parent);

    QObject::connect(cleaner, &InactiveHostsCleaner::sig_collected, cleaner,
                     [cleaner, parent, months](const QList<InactiveHost>& hosts,
                                               const QStringList& skipped_routers)
    {
        if (hosts.isEmpty())
        {
            LOG(INFO) << "No inactive hosts to remove";
            cleaner->deleteLater();
            return;
        }

        QString message = InactiveHostsCleaner::tr("Inactive hosts found: %1").arg(hosts.size()) + "\n" +
                          InactiveHostsCleaner::tr("Inactive period: %1 month(s)").arg(months) + "\n\n" +
                          InactiveHostsCleaner::tr("These hosts have not been connected to for longer than the "
                                      "inactive period. They will be deleted from the router and "
                                      "from the address book. This operation is irreversible.");

        if (!skipped_routers.isEmpty())
        {
            message += "\n\n" + InactiveHostsCleaner::tr("Skipped routers: %1")
                                    .arg(skipped_routers.join("; "));
        }

        message += "\n\n" + InactiveHostsCleaner::tr("Continue?");

        LOG(INFO) << "[ACTION] Remove inactive hosts requested:" << hosts.size();

        if (MsgBox::importantQuestion(parent, message, Seconds(10)) != MsgBox::Yes)
        {
            LOG(INFO) << "[ACTION] Remove inactive hosts rejected by user";
            cleaner->deleteLater();
            return;
        }

        cleaner->remove(hosts);
    });

    QObject::connect(cleaner, &InactiveHostsCleaner::sig_removed, cleaner,
                     [cleaner, parent](int removed, int failed)
    {
        const QString message = InactiveHostsCleaner::tr("Removed hosts: %1").arg(removed) + "\n" +
                                InactiveHostsCleaner::tr("Failed: %1").arg(failed);

        if (failed > 0)
            MsgBox::warning(parent, message);
        else
            MsgBox::information(parent, message);

        cleaner->deleteLater();
    });

    cleaner->start();
}
