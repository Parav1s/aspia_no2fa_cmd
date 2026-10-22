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

#include "client/router_config_storage.h"

#include <QByteArray>
#include <QDateTime>
#include <QSettings>

#include "base/crypto/secure_string.h"
#include "base/logging.h"

#if defined(Q_OS_WINDOWS)
#include <qt_windows.h>
#include <wincrypt.h>
#else
#include "base/crypto/os_crypt.h"
#endif

namespace {

const QString kOrganization = "aspia";
const QString kApplication = "client_router";

const QString kAddressKey = "router/address";
const QString kUsernameKey = "router/username";
const QString kPasswordKey = "router/password";
const QString kUpdatedKey = "router/updated";

//--------------------------------------------------------------------------------------------------
bool sealPassword(const QByteArray& plain, QByteArray* sealed)
{
#if defined(Q_OS_WINDOWS)
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.constData()));
    input.cbData = static_cast<DWORD>(plain.size());

    DATA_BLOB output;
    if (!CryptProtectData(&input, L"aspia-router", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_LOCAL_MACHINE, &output))
    {
        PLOG(ERROR) << "Unable to seal the router password";
        return false;
    }

    *sealed = QByteArray(reinterpret_cast<const char*>(output.pbData),
                         static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return true;
#else
    return OSCrypt::encryptBytes(plain, sealed);
#endif
}

//--------------------------------------------------------------------------------------------------
bool unsealPassword(const QByteArray& sealed, QByteArray* plain)
{
    if (sealed.isEmpty())
        return false;

#if defined(Q_OS_WINDOWS)
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(sealed.constData()));
    input.cbData = static_cast<DWORD>(sealed.size());

    DATA_BLOB output;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
    {
        PLOG(WARNING) << "Unable to open the sealed router password";
        return false;
    }

    *plain = QByteArray(reinterpret_cast<const char*>(output.pbData),
                        static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
#else
    return OSCrypt::decryptBytes(sealed, plain);
#endif
}

//--------------------------------------------------------------------------------------------------
QSettings machineSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::SystemScope, kOrganization, kApplication);
}

//--------------------------------------------------------------------------------------------------
QSettings userSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope, kOrganization, kApplication);
}

//--------------------------------------------------------------------------------------------------
bool read(const QSettings& settings, RouterConfig* config, qint64* updated)
{
    if (!settings.contains(kAddressKey))
        return false;

    const QString address = settings.value(kAddressKey).toString();
    const QString username = settings.value(kUsernameKey).toString();
    const QByteArray sealed = QByteArray::fromBase64(
        settings.value(kPasswordKey).toString().toLatin1());

    QByteArray plain;
    if (address.isEmpty() || username.isEmpty() || !unsealPassword(sealed, &plain))
        return false;

    config->setAddress(address);
    config->setUsername(username);
    config->setPassword(SecureString(QString::fromUtf8(plain)));

    if (updated)
        *updated = settings.value(kUpdatedKey, 0).toLongLong();

    return config->isValid();
}

//--------------------------------------------------------------------------------------------------
bool write(QSettings* settings, const RouterConfig& config)
{
    QByteArray plain = config.password().toString().toUtf8();
    QByteArray sealed;
    if (!sealPassword(plain, &sealed))
        return false;

    settings->setValue(kAddressKey, config.address());
    settings->setValue(kUsernameKey, config.username());
    settings->setValue(kPasswordKey, QString::fromLatin1(sealed.toBase64()));
    settings->setValue(kUpdatedKey, QDateTime::currentSecsSinceEpoch());
    settings->sync();

    return settings->status() == QSettings::NoError;
}

} // namespace

//--------------------------------------------------------------------------------------------------
RouterConfig RouterConfigStorage::routerConfig() const
{
    RouterConfig machine_config;
    qint64 machine_updated = 0;
    const bool has_machine = read(machineSettings(), &machine_config, &machine_updated);

    RouterConfig user_config;
    qint64 user_updated = 0;
    const bool has_user = read(userSettings(), &user_config, &user_updated);

    if (has_machine && (!has_user || machine_updated >= user_updated))
        return machine_config;

    if (has_user)
        return user_config;

    return RouterConfig();
}

//--------------------------------------------------------------------------------------------------
bool RouterConfigStorage::setRouterConfig(const RouterConfig& config)
{
    if (!config.isValid())
    {
        LOG(ERROR) << "Invalid router config";
        return false;
    }

    QSettings machine = machineSettings();
    if (write(&machine, config))
    {
        // A record from an older fallback is not needed once the machine-wide one is in place.
        QSettings user = userSettings();
        user.remove("router");
        user.sync();
        return true;
    }

    LOG(WARNING) << "Unable to write the machine-wide router config, using the user profile";

    QSettings user = userSettings();
    if (write(&user, config))
        return true;

    LOG(ERROR) << "Unable to save the router config";
    return false;
}
