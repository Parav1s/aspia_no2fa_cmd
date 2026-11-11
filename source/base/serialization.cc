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

#include "base/serialization.h"

#include "base/logging.h"
#include "proto/desktop_screen.h"
#include "proto/desktop_video.h"
#include "proto/peer.h"

//--------------------------------------------------------------------------------------------------
QByteArray serialize(const google::protobuf::MessageLite& message)
{
    const size_t size = message.ByteSizeLong();
    if (!size)
        return QByteArray();

    if (size > static_cast<size_t>(std::numeric_limits<qsizetype>::max()))
        return QByteArray();

    QByteArray buffer(static_cast<qsizetype>(size), Qt::Uninitialized);
    message.SerializeWithCachedSizesToArray(reinterpret_cast<quint8*>(buffer.data()));
    return buffer;
}

//--------------------------------------------------------------------------------------------------
proto::peer::Version serialize(const QVersionNumber& version)
{
    const int segment_count = version.segmentCount();

    if (segment_count != 3 && segment_count != 4)
    {
        LOG(ERROR) << "Invalid version segments count:" << segment_count;
        return proto::peer::Version();
    }

    proto::peer::Version proto_version;
    proto_version.set_major(static_cast<quint32>(version.majorVersion()));
    proto_version.set_minor(static_cast<quint32>(version.minorVersion()));
    proto_version.set_patch(static_cast<quint32>(version.microVersion()));

    if (segment_count == 4)
        proto_version.set_revision(static_cast<quint32>(version.segmentAt(3)));

    return proto_version;
}

//--------------------------------------------------------------------------------------------------
// The fourth part is the build number. Peers that predate it send zero, and then the version stays
// three-part: appending ".0" would only add noise to the version shown to a person.
QVersionNumber parse(const proto::peer::Version& version)
{
    const int major = static_cast<int>(version.major());
    const int minor = static_cast<int>(version.minor());
    const int patch = static_cast<int>(version.patch());

    if (version.revision() != 0)
        return QVersionNumber({ major, minor, patch, static_cast<int>(version.revision()) });

    return QVersionNumber({ major, minor, patch });
}

//--------------------------------------------------------------------------------------------------
proto::screen::Point serialize(const QPoint& point)
{
    proto::screen::Point result;

    result.set_x(point.x());
    result.set_y(point.y());

    return result;
}

//--------------------------------------------------------------------------------------------------
QPoint parse(const proto::screen::Point& point)
{
    return QPoint(point.x(), point.y());
}

//--------------------------------------------------------------------------------------------------
proto::screen::Size serialize(const QSize& size)
{
    proto::screen::Size result;

    result.set_width(size.width());
    result.set_height(size.height());

    return result;
}

//--------------------------------------------------------------------------------------------------
QSize parse(const proto::screen::Size& size)
{
    return QSize(size.width(), size.height());
}

//--------------------------------------------------------------------------------------------------
proto::video::Rect serialize(const QRect& rect)
{
    proto::video::Rect result;

    result.set_x(rect.x());
    result.set_y(rect.y());
    result.set_width(rect.width());
    result.set_height(rect.height());

    return result;
}

//--------------------------------------------------------------------------------------------------
QRect parse(const proto::video::Rect& rect)
{
    return QRect(QPoint(rect.x(), rect.y()), QSize(rect.width(), rect.height()));
}
