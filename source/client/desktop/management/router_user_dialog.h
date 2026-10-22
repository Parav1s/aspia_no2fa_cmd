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

#ifndef CLIENT_DESKTOP_MANAGEMENT_ROUTER_USER_DIALOG_H
#define CLIENT_DESKTOP_MANAGEMENT_ROUTER_USER_DIALOG_H

#include <QDialog>

#include <memory>

#include "base/peer/router_user.h"
#include "client/desktop/management/user_edit_model.h"

class QAbstractButton;

namespace Ui {
class RouterUserDialog;
} // namespace Ui

namespace proto::router {
class UserList;
class UserResult;
enum SessionType : int;
} // namespace proto::router

class RouterUserDialog final : public QDialog
{
    Q_OBJECT

public:
    // user_id == 0 means create mode; > 0 means modify mode.
    RouterUserDialog(qint64 router_id, qint64 user_id, QWidget* parent);
    ~RouterUserDialog() final;

protected:
    // QDialog implementation.
    bool eventFilter(QObject* object, QEvent* event) final;

private slots:
    void onUserListReceived(const proto::router::UserList& list);
    void onUserResultReceived(const proto::router::UserResult& result);
    void onButtonBoxClicked(QAbstractButton* button);

private:
    void fetchUser();
    void submitWithNameCheck(const RouterUser& request, const QString& username);
    void submitUser(const RouterUser& request);
    void setAccountChanged(bool changed);
    void updateLoadingState();
    void setAccessLevel(proto::router::SessionType session_type);
    static QString sessionTypeToString(proto::router::SessionType session_type);

    std::unique_ptr<Ui::RouterUserDialog> ui;
    qint64 router_id_ = 0;
    qint64 entry_id_ = 0;
    UserEditModel model_;

    // Guards against stacked error boxes and double reject: several replies can arrive with an
    // error while the first modal warning is still open.
    bool closing_ = false;

    friend class RouterUserDialogTestPeer;
    Q_DISABLE_COPY_MOVE(RouterUserDialog)
};

#endif // CLIENT_DESKTOP_MANAGEMENT_ROUTER_USER_DIALOG_H
