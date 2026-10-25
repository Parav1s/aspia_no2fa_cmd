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

#ifndef HOST_HOST_UTILS_H
#define HOST_HOST_UTILS_H

#include <QString>
#include <QtClassHelperMacros>
#include <QtTypes>

class HostUtils
{
public:
    static bool isMigrationNeeded();
    static void doMigrate();

    enum DebugInfoFeatures
    {
        INCLUDE_DEFAULT = 0,
        INCLUDE_VIDEO_ADAPTERS = 1,
        INCLUDE_WINDOW_STATIONS = 2
    };

    static void printDebugInfo(quint32 features = INCLUDE_DEFAULT);
    static void uninstallApplication();

#if defined(Q_OS_WINDOWS)
    // The Desktop of the user who works in this session. Same as the current user's Desktop unless
    // the process was elevated with another administrator's credentials: then the process lives in
    // that administrator's profile, and the dialogs must still start where the user expects.
    static QString sessionUserDesktop();
#endif // defined(Q_OS_WINDOWS)

private:
    Q_DISABLE_COPY_MOVE(HostUtils)
};

#endif // HOST_HOST_UTILS_H
