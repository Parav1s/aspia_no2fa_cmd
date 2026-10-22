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

#include "base/net/curl_util.h"

#include "base/logging.h"

#if defined(Q_OS_LINUX)
#include <QFile>
#endif

namespace {

#if defined(Q_OS_LINUX)

//--------------------------------------------------------------------------------------------------
QByteArray systemCaBundlePath()
{
    static const char* const kPaths[] =
    {
        "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Arch, Gentoo.
        "/etc/pki/tls/certs/ca-bundle.crt",   // Red Hat family.
        "/etc/ssl/ca-bundle.pem",             // openSUSE.
        "/etc/pki/tls/cacert.pem",
        "/etc/ssl/cert.pem"
    };

    for (const char* path : kPaths)
    {
        if (QFile::exists(QString::fromLatin1(path)))
            return QByteArray(path);
    }

    return QByteArray();
}

#endif

} // namespace

//--------------------------------------------------------------------------------------------------
ScopedCURL::ScopedCURL()
    : curl_(curl_easy_init())
{
    CHECK(curl_);

#if defined(Q_OS_LINUX)
    static const QByteArray bundle_path = systemCaBundlePath();

    if (bundle_path.isEmpty())
    {
        LOG(ERROR) << "System has no known certificate authorities";
        return;
    }

    curl_easy_setopt(curl_, CURLOPT_CAINFO, bundle_path.constData());
#endif
}

//--------------------------------------------------------------------------------------------------
ScopedCURL::~ScopedCURL()
{
    curl_easy_cleanup(curl_);
}

//--------------------------------------------------------------------------------------------------
ScopedCURLM::ScopedCURLM()
    : curlm_(curl_multi_init())
{
    CHECK(curlm_);
}

//--------------------------------------------------------------------------------------------------
ScopedCURLM::~ScopedCURLM()
{
    curl_multi_cleanup(curlm_);
}
