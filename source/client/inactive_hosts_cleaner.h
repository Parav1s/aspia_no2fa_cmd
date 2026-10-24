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

#ifndef CLIENT_INACTIVE_HOSTS_CLEANER_H
#define CLIENT_INACTIVE_HOSTS_CLEANER_H

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include "base/peer/host_id.h"

class QWidget;
struct RouterHostList;

// A host that is a candidate for removal, together with the router it belongs to.
struct InactiveHost
{
    qint64 router_id = 0;
    QString router_name;
    HostId host_id = kInvalidHostId;
    QString display_name;
};

// Finds the hosts no one has connected to for longer than the given number of months and, when
// asked, deletes them from the router and from the local database. Only the routers with a live
// session are inspected; a host that is online right now is never touched, and neither is a host
// that has never been connected to (its age is unknown).
//
// The work is split in two steps so the caller can show a confirmation with the exact count:
// start() only collects and reports sig_collected(), remove() deletes what it is given.
class InactiveHostsCleaner final : public QObject
{
    Q_OBJECT

public:
    explicit InactiveHostsCleaner(int months, QObject* parent = nullptr);
    ~InactiveHostsCleaner() final;

    // Collects the inactive hosts of every router that has a live session.
    void start();

    // Deletes the given hosts from their routers and from the local database, one by one.
    void remove(const QList<InactiveHost>& hosts);

signals:
    // |skipped_routers| lists the configured routers that could not be inspected, with the reason
    // ("not connected", "access denied", ...); nothing was read from them.
    void sig_collected(const QList<InactiveHost>& hosts, const QStringList& skipped_routers);
    void sig_removed(int removed, int failed);

private:
    struct Router
    {
        qint64 id = 0;
        QString name;
    };

    void fetchNextRouter();
    void fetchPage(qint64 router_id, qint64 offset);
    void onHostListReceived(qint64 router_id, qint64 offset, const RouterHostList& list);
    void markUnavailable(qint64 router_id, const QString& reason);
    void removeNext();

    const int months_;
    const qint64 threshold_ = 0;

    QList<Router> routers_;
    qsizetype router_index_ = 0;

    QList<InactiveHost> hosts_;
    QStringList skipped_;

    QList<InactiveHost> pending_;
    int removed_ = 0;
    int failed_ = 0;

    Q_DISABLE_COPY_MOVE(InactiveHostsCleaner)
};

// Runs the whole flow for the period stored in the settings: collect, confirm with the user,
// delete, report. |parent| owns the cleaner and is used for the dialogs.
void runInactiveHostsCleanup(QWidget* parent);

#endif // CLIENT_INACTIVE_HOSTS_CLEANER_H
