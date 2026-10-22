//
// Aspia Project
// Copyright (C) 2016-2026 Dmitry Chapyshev <dmitry@aspia.ru>
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

#include <QSysInfo>

#include "version.h"
#include "base/build_config.h"
#include "base/logging.h"
#include "base/sys_info.h"
#include "client/application.h"

#if defined(Q_OS_MACOS)
#include "base/process_util.h"
#endif // defined(Q_OS_MACOS)

#if defined(Q_OS_ANDROID)
#include <QCoreApplication>
#include <QJniObject>

#include "client/android/main_window.h"
#else
#include <QCommandLineParser>
#include <QDir>
#include <QTimer>

#include <functional>

#include "base/crypto/secure_string.h"
#include "client/config.h"
#include "client/database.h"
#include "client/host_url.h"
#include "client/master_password.h"
#include "client/router_controller.h"
#include "client/desktop/app_lock.h"
#include "client/desktop/main_window.h"
#include "client/desktop/chat/chat_window.h"
#include "client/desktop/desktop/desktop_window.h"
#include "client/desktop/file_transfer/file_transfer_window.h"
#include "client/desktop/sys_info/system_info_window.h"
#include "client/desktop/terminal/terminal_window.h"
#include "common/desktop/credentials_dialog.h"
#include "common/desktop/msg_box.h"
#include "common/desktop/update_dialog.h"
#include "proto/desktop_control.h"
#include "proto/peer.h"
#endif // defined(Q_OS_ANDROID)

#if !defined(Q_OS_ANDROID)
namespace {

//--------------------------------------------------------------------------------------------------
// Appends |port| to an address that does not carry one. An IPv6 literal is expected in brackets
// ("[::1]:8050"), as everywhere else in the client.
QString withPort(const QString& address, quint16 port)
{
    const QString trimmed = address.trimmed();

    if (trimmed.startsWith('['))
        return trimmed.contains("]:") ? trimmed : trimmed + ':' + QString::number(port);

    // A single colon separates a host from its port; more than one is a bare IPv6 literal.
    if (trimmed.count(':') == 1)
    {
        bool ok = false;
        trimmed.section(':', 1, 1).toUShort(&ok);
        if (ok)
            return trimmed;
    }

    return trimmed + ':' + QString::number(port);
}

//--------------------------------------------------------------------------------------------------
// Reads a "0"/"1" command line value. Returns false for anything else.
bool parseBool(const QString& value, bool* result)
{
    if (value == "0")
    {
        *result = false;
        return true;
    }

    if (value == "1")
    {
        *result = true;
        return true;
    }

    return false;
}

//--------------------------------------------------------------------------------------------------
void backupOnStartup()
{
    Database& db = Database::instance();
    if (!db.isBackupOnStartupEnabled())
        return;

    const QString directory = db.backupPath();

    switch (AutoBackup::run(db, directory, db.backupRetention()))
    {
        case Backup::Result::SUCCESS:
        case Backup::Result::NOTHING_EXPORTED:
            break;

        case Backup::Result::FILE_ERROR:
            MsgBox::warning(nullptr, QApplication::translate(
                "Client", "Unable to create a backup in the directory \"%1\".")
                    .arg(QDir::toNativeSeparators(directory)));
            break;

        case Backup::Result::UNREADABLE_RECORD:
            MsgBox::warning(nullptr, QApplication::translate(
                "Client", "Unable to create a backup. Some records of the database are damaged. Fix or "
                          "delete them."));
            break;

        default:
            MsgBox::warning(nullptr, QApplication::translate("Client", "Unable to create a backup."));
            break;
    }
}

} // namespace
#endif // !defined(Q_OS_ANDROID)

//--------------------------------------------------------------------------------------------------
int main(int argc, char* argv[])
{
#if defined(Q_OS_ANDROID)
    // When the Qt event loop ends, Qt's Android platform calls exit(), which runs the C++ runtime's
    // global destructors (__cxa_finalize). Those tear down std::thread/std::mutex statics while
    // Android's own threads (notably the HWUI render threads "hwuiTask") are still running, so a
    // render thread locks an already-destroyed mutex and the process aborts with
    // "FORTIFY: pthread_mutex_lock called on a destroyed mutex" (SIGABRT). This is a long-standing
    // Qt-on-Android shutdown race. QT_ANDROID_NO_EXIT_CALL makes Qt skip that exit() call, so the
    // global destructors are not run and the race cannot occur (Android reclaims the process).
    // Refs:
    //   https://bugreports.qt.io/browse/QTBUG-85449
    //   https://bugreports.qt.io/browse/QTBUG-82617
    //   https://developernote.com/2022/03/crash-at-std-thread-and-std-mutex-destructors-on-android/
    qputenv("QT_ANDROID_NO_EXIT_CALL", "1");

    // Force the software (raster) widget backing store. The app uses no OpenGL widgets, so the RHI
    // compositor is only pulled in for translucent top-level surfaces (dialogs, bottom sheets, menus).
    // Its QOpenGLContext::makeCurrent aborts with a qFatal (SIGABRT) when the surface is momentarily
    // invalid - e.g. while the on-screen keyboard is shown during a repaint. Raster avoids OpenGL
    // entirely and removes that crash.
    qputenv("QT_WIDGETS_RHI", "0");
#endif // defined(Q_OS_ANDROID)

    Q_INIT_RESOURCE(common);
    Q_INIT_RESOURCE(common_translations);

    LoggingSettings logging_settings;

#if defined(Q_OS_ANDROID)
    // Android defaults to logcat only (LOG_TO_STDOUT). Also write a log file so it can be pulled from
    // devices whose logcat is restricted (e.g. Huawei), then point that file at the external app files
    // dir (/sdcard/Android/data/<package>/files/log) so adb can pull it without root.
    logging_settings.destination = LOG_TO_ALL;

    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid())
    {
        QJniObject files_dir = context.callObjectMethod(
            "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", nullptr);
        if (files_dir.isValid())
        {
            logging_settings.log_dir =
                files_dir.callObjectMethod<jstring>("getAbsolutePath").toString() + "/log";
        }
    }
#endif // defined(Q_OS_ANDROID)

    ScopedLogging scoped_logging(logging_settings);

    Application::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

#if defined(Q_OS_MACOS)
    // The update is installed by an instance launched through Authorization Services, and without the
    // identity of root the installation is refused.
    ProcessUtil::restartAsRoot(argv);
#endif // defined(Q_OS_MACOS)

#if !defined(Q_OS_ANDROID)
    if (argc == 3)
    {
        auto option_value = [](const char* argument, const char* option)
        {
            size_t length = qstrlen(option);
            if (qstrncmp(argument, option, length) != 0)
                return QString();
            return QString::fromLocal8Bit(argument + length);
        };

        QString channel = option_value(argv[1], "--channel=");
        QString locale = option_value(argv[2], "--locale=");

        if (!channel.isEmpty() && !locale.isEmpty())
        {
#if defined(Q_OS_LINUX)
            qputenv("QT_QPA_PLATFORM", "xcb");
#endif // defined(Q_OS_LINUX)

            GuiApplication application(argc, argv);
            application.setLocale(locale);

            UpdateDialog dialog(channel, "client", UpdateDialog::Action::INSTALL);

            return dialog.exec() == QDialog::Accepted ?
                UpdateDialog::kInstalledExitCode : UpdateDialog::kClosedExitCode;
        }
    }
#endif // !defined(Q_OS_ANDROID)

    Application application(argc, argv);

    LOG(INFO) << "Version:" << ASPIA_VERSION_STRING << "(arch:" << QSysInfo::buildCpuArchitecture() << ")";
#if defined(GIT_CURRENT_BRANCH) && defined(GIT_COMMIT_HASH)
    LOG(INFO) << "Git branch:" << GIT_CURRENT_BRANCH;
    LOG(INFO) << "Git commit:" << GIT_COMMIT_HASH;
#endif
    LOG(INFO) << "OS:" << SysInfo::operatingSystemName()
              << "(version:" << SysInfo::operatingSystemVersion()
              <<  "arch:" << SysInfo::operatingSystemArchitecture() << ")";
    LOG(INFO) << "Qt version:" << QT_VERSION_STR;
    LOG(INFO) << "Command line:" << application.arguments();

#if defined(Q_OS_ANDROID)
    AndroidMainWindow main_window;
    main_window.show();

    return application.exec();
#else
    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::translate("Client", "Aspia Client"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("url",
        QApplication::translate("Client", "An aspia:// link to connect to a host."), "[url]");

    // Command line connection. The set mirrors the one the 2.x client had, so existing shortcuts
    // and scripts keep working. Options of the old protocol that the current one no longer has
    // (codec, color depth, compression ratio, font smoothing, clear clipboard) are accepted and
    // ignored.
    QCommandLineOption address_option("address",
        QApplication::translate("Client", "Remote computer address or host ID."), "address");
    QCommandLineOption port_option("port",
        QApplication::translate("Client", "Remote computer port."), "port",
        QString::number(kDefaultHostTcpPort));
    QCommandLineOption name_option("name",
        QApplication::translate("Client", "Name of the host."), "name");
    QCommandLineOption username_option("username",
        QApplication::translate("Client", "User name."), "username");
    QCommandLineOption password_option("password",
        QApplication::translate("Client", "Password of the user."), "password");
    QCommandLineOption display_name_option("display-name",
        QApplication::translate("Client", "Display name when connected."), "display-name");
    QCommandLineOption session_type_option("session-type",
        QApplication::translate("Client", "Session type. Possible values: desktop-manage, "
                                "desktop-view, file-transfer, system-info, text-chat, terminal."),
        "session-type", "desktop-manage");
    QCommandLineOption codec_option("codec",
        QApplication::translate("Client", "Type of codec. Possible values: vp8, vp9, zstd. "
                                "Not supported by the current protocol."), "codec");
    QCommandLineOption color_depth_option("color-depth",
        QApplication::translate("Client", "Color depth. Possible values: 3, 6, 8, 16, 32. "
                                "Not supported by the current protocol."), "color-depth");
    QCommandLineOption compress_ratio_option("compress-ratio",
        QApplication::translate("Client", "Compression ratio. Possible values: 1-22. "
                                "Not supported by the current protocol."), "compress-ratio");
    QCommandLineOption audio_option("audio",
        QApplication::translate("Client", "Enable or disable audio. Possible values: 0 or 1."),
        "audio");
    QCommandLineOption cursor_shape_option("cursor-shape",
        QApplication::translate("Client", "Enable or disable cursor shape. Possible values: 0 or 1."),
        "cursor-shape");
    QCommandLineOption cursor_position_option("cursor-position",
        QApplication::translate("Client", "Enable or disable cursor position. Possible values: 0 or 1."),
        "cursor-position");
    QCommandLineOption clipboard_option("clipboard",
        QApplication::translate("Client", "Enable or disable clipboard. Possible values: 0 or 1."),
        "clipboard");
    QCommandLineOption desktop_effects_option("desktop-effects",
        QApplication::translate("Client", "Enable or disable desktop effects. Possible values: 0 or 1."),
        "desktop-effects");
    QCommandLineOption desktop_wallpaper_option("desktop-wallpaper",
        QApplication::translate("Client", "Enable or disable desktop wallpaper. Possible values: 0 or 1."),
        "desktop-wallpaper");
    QCommandLineOption font_smoothing_option("font-smoothing",
        QApplication::translate("Client", "Enable or disable font smoothing. Possible values: 0 or 1. "
                                "Not supported by the current protocol."), "font-smoothing");
    QCommandLineOption clear_clipboard_option("clear-clipboard",
        QApplication::translate("Client", "Clear clipboard at disconnect. Possible values: 0 or 1. "
                                "Not supported by the current protocol."), "clear-clipboard");
    QCommandLineOption lock_at_disconnect_option("lock-at-disconnect",
        QApplication::translate("Client", "Lock computer at disconnect. Possible values: 0 or 1."),
        "lock-at-disconnect");
    QCommandLineOption block_remote_input_option("block-remote-input",
        QApplication::translate("Client", "Block remote input. Possible values: 0 or 1."),
        "block-remote-input");
    QCommandLineOption router_address_option("router-address",
        QApplication::translate("Client", "Router address."), "router-address");
    QCommandLineOption router_port_option("router-port",
        QApplication::translate("Client", "Router port."), "router-port",
        QString::number(kDefaultRouterClientTcpPort));
    QCommandLineOption router_username_option("router-username",
        QApplication::translate("Client", "Router user name."), "router-username");
    QCommandLineOption router_password_option("router-password",
        QApplication::translate("Client", "Router password of the user."), "router-password");

    parser.addOption(address_option);
    parser.addOption(port_option);
    parser.addOption(name_option);
    parser.addOption(username_option);
    parser.addOption(password_option);
    parser.addOption(display_name_option);
    parser.addOption(session_type_option);
    parser.addOption(codec_option);
    parser.addOption(color_depth_option);
    parser.addOption(compress_ratio_option);
    parser.addOption(audio_option);
    parser.addOption(cursor_shape_option);
    parser.addOption(cursor_position_option);
    parser.addOption(clipboard_option);
    parser.addOption(desktop_effects_option);
    parser.addOption(desktop_wallpaper_option);
    parser.addOption(font_smoothing_option);
    parser.addOption(clear_clipboard_option);
    parser.addOption(lock_at_disconnect_option);
    parser.addOption(block_remote_input_option);
    parser.addOption(router_address_option);
    parser.addOption(router_port_option);
    parser.addOption(router_username_option);
    parser.addOption(router_password_option);

    parser.process(application);

    // Command line connection: no address book, no master password, no dialogs. The session window
    // either appears already connecting or reports the error and exits when it is closed.
    if (parser.isSet(address_option))
    {
        LOG(INFO) << "Command line start";

        const QString address = parser.value(address_option);
        const QString user_name = parser.value(username_option);
        const QString password = parser.value(password_option);

        proto::peer::SessionType session_type = proto::peer::SESSION_TYPE_DESKTOP;
        proto::control::Config desktop_config = defaultDesktopConfig();
        bool desktop_session = true;

        const QString session_type_name = parser.value(session_type_option);

        if (session_type_name == "desktop-manage" || session_type_name == "desktop")
        {
            session_type = proto::peer::SESSION_TYPE_DESKTOP;
        }
        else if (session_type_name == "desktop-view")
        {
            // The current protocol has a single desktop session; view only is the host-side
            // "block input" switch.
            session_type = proto::peer::SESSION_TYPE_DESKTOP;
            desktop_config.set_block_input(true);
        }
        else if (session_type_name == "file-transfer")
        {
            session_type = proto::peer::SESSION_TYPE_FILE_TRANSFER;
            desktop_session = false;
        }
        else if (session_type_name == "system-info")
        {
            session_type = proto::peer::SESSION_TYPE_SYSTEM_INFO;
            desktop_session = false;
        }
        else if (session_type_name == "text-chat")
        {
            session_type = proto::peer::SESSION_TYPE_CHAT;
            desktop_session = false;
        }
        else if (session_type_name == "terminal")
        {
            session_type = proto::peer::SESSION_TYPE_TERMINAL;
            desktop_session = false;
        }
        else
        {
            LOG(ERROR) << "Unknown session type:" << session_type_name;
            return 1;
        }

        auto apply_bool = [&parser](const QCommandLineOption& option, const char* name,
                                    const std::function<void(bool)>& setter) -> bool
        {
            if (!parser.isSet(option))
                return true;

            bool value = false;
            if (!parseBool(parser.value(option), &value))
            {
                LOG(ERROR) << "Incorrect value for --" << name << "(expected 0 or 1)";
                return false;
            }

            setter(value);
            return true;
        };

        if (desktop_session)
        {
            if (!apply_bool(audio_option, "audio", [&](bool value)
                    { desktop_config.set_audio(value); }) ||
                !apply_bool(cursor_shape_option, "cursor-shape", [&](bool value)
                    { desktop_config.set_cursor_shape(value); }) ||
                !apply_bool(cursor_position_option, "cursor-position", [&](bool value)
                    { desktop_config.set_cursor_position(value); }) ||
                !apply_bool(clipboard_option, "clipboard", [&](bool value)
                    { desktop_config.set_clipboard(value); }) ||
                !apply_bool(desktop_effects_option, "desktop-effects", [&](bool value)
                    { desktop_config.set_effects(value); }) ||
                !apply_bool(desktop_wallpaper_option, "desktop-wallpaper", [&](bool value)
                    { desktop_config.set_wallpaper(value); }) ||
                !apply_bool(lock_at_disconnect_option, "lock-at-disconnect", [&](bool value)
                    { desktop_config.set_lock_at_disconnect(value); }) ||
                !apply_bool(block_remote_input_option, "block-remote-input", [&](bool value)
                    { desktop_config.set_block_input(value); }))
            {
                return 1;
            }

            // Accepted for compatibility with the 2.x command line; the current protocol has no
            // such settings any more.
            if (parser.isSet(codec_option))
            {
                const QString codec = parser.value(codec_option);
                if (codec != "vp8" && codec != "vp9" && codec != "zstd")
                {
                    LOG(ERROR) << "Incorrect value for --codec (vp8, vp9, zstd)";
                    return 1;
                }
                LOG(WARNING) << "The video codec is chosen by the host now - ignoring --codec";
            }

            if (parser.isSet(color_depth_option))
            {
                const QString color_depth = parser.value(color_depth_option);
                if (color_depth != "3" && color_depth != "6" && color_depth != "8" &&
                    color_depth != "16" && color_depth != "32")
                {
                    LOG(ERROR) << "Incorrect value for --color-depth (3, 6, 8, 16, 32)";
                    return 1;
                }
                LOG(WARNING) << "The color depth is chosen by the host now - ignoring --color-depth";
            }

            if (parser.isSet(compress_ratio_option))
            {
                bool ok = false;
                const uint ratio = parser.value(compress_ratio_option).toUInt(&ok);
                if (!ok || ratio < 1 || ratio > 22)
                {
                    LOG(ERROR) << "Incorrect value for --compress-ratio (1-22)";
                    return 1;
                }
                LOG(WARNING) << "The compression ratio is chosen by the host now - ignoring "
                                "--compress-ratio";
            }

            bool ignored = false;
            if ((parser.isSet(font_smoothing_option) &&
                 !parseBool(parser.value(font_smoothing_option), &ignored)) ||
                (parser.isSet(clear_clipboard_option) &&
                 !parseBool(parser.value(clear_clipboard_option), &ignored)))
            {
                LOG(ERROR) << "Incorrect value for --font-smoothing or --clear-clipboard "
                              "(expected 0 or 1)";
                return 1;
            }

            if (parser.isSet(font_smoothing_option))
                LOG(WARNING) << "Font smoothing is not supported by the current protocol - ignoring";
            if (parser.isSet(clear_clipboard_option))
                LOG(WARNING) << "Clearing the clipboard is not supported by the current protocol - "
                                "ignoring";
        }

        HostConfig host;
        host.setAddress(address);
        host.setName(parser.value(name_option));
        host.setUsername(user_name);
        host.setPassword(SecureString(password));

        if (isHostId(address))
        {
            LOG(INFO) << "Connection by host ID";

            if (!parser.isSet(router_address_option))
            {
                LOG(ERROR) << "A host ID requires --router-address, --router-username and "
                              "--router-password";
                return 1;
            }

            bool ok = false;
            const quint16 router_port = parser.value(router_port_option).toUShort(&ok);
            if (!ok || router_port == 0)
            {
                LOG(ERROR) << "Incorrect value for --router-port";
                return 1;
            }

            const QString router_address = withPort(parser.value(router_address_option), router_port);

            RouterController* controller = new RouterController(&application);
            const qint64 router_id = controller->addTemporaryRouter(
                router_address, parser.value(router_username_option),
                parser.value(router_password_option));
            if (router_id < 0)
                return 1;

            host.setRouterId(router_id);
        }
        else
        {
            LOG(INFO) << "Direct connection";

            bool ok = false;
            const quint16 host_port = parser.value(port_option).toUShort(&ok);
            if (!ok || host_port == 0)
            {
                LOG(ERROR) << "Incorrect value for --port";
                return 1;
            }

            host.setAddress(withPort(address, host_port));
        }

        ClientWindow* client_window = nullptr;

        switch (session_type)
        {
            case proto::peer::SESSION_TYPE_DESKTOP:
                client_window = new DesktopWindow(desktop_config);
                break;

            case proto::peer::SESSION_TYPE_FILE_TRANSFER:
                client_window = new FileTransferWindow();
                break;

            case proto::peer::SESSION_TYPE_SYSTEM_INFO:
                client_window = new SystemInfoWindow();
                break;

            case proto::peer::SESSION_TYPE_CHAT:
                client_window = new ChatWindow();
                break;

            case proto::peer::SESSION_TYPE_TERMINAL:
                client_window = new TerminalWindow();
                break;

            default:
                break;
        }

        if (!client_window)
        {
            LOG(ERROR) << "Unable to create the session window";
            return 1;
        }

        client_window->setAttribute(Qt::WA_DeleteOnClose);

        if (!client_window->connectToHost(host, parser.value(display_name_option), true))
        {
            delete client_window;
            return 0;
        }

        client_window->show();
        return application.exec();
    }

    QString start_url;
    QStringList positional_arguments = parser.positionalArguments();
    if (!positional_arguments.isEmpty() && HostUrl::isHostUrl(positional_arguments.front()))
        start_url = positional_arguments.front();

    // A link can arrive before the main window exists: forwarded by a second instance or
    // delivered by a URL open event while the master password dialog is still on the screen.
    // Remember it and open it after the main window is created.
    auto remember_url = [&start_url](const QString& url) { start_url = url; };
    QMetaObject::Connection pending_url_connection = QObject::connect(
        &application, &Application::sig_urlOpened, &application, remember_url);

    if (application.isRunning())
    {
        if (!start_url.isEmpty())
        {
            LOG(INFO) << "Another instance is already running, forwarding URL to it";
            application.openUrl(start_url);
        }
        else
        {
            LOG(INFO) << "Another instance is already running, activating its window";
            application.activateWindow();
        }
        return 0;
    }

    if (MasterPassword::autoUnlock())
    {
        LOG(INFO) << "Unlocked automatically";
    }
    else if (MasterPassword::isSet())
    {
        LOG(INFO) << "Master password is set, prompting user";

        switch (AppLock::unlock())
        {
            case AppLock::Result::UNLOCKED:
                break;

            case AppLock::Result::CANCELLED:
                return 0;

            case AppLock::Result::FAILED:
                return 1;
        }
    }
    else
    {
        LOG(INFO) << "Master password is not set, prompting user to set one";

        CredentialsDialog dialog(CredentialsDialog::Type::SET_PASSWORD, nullptr);
        dialog.setWindowTitle(QApplication::translate("Client", "Set Master Password"));
        dialog.setHeaderIcon(":/img/lock.svg");
        dialog.setHeaderText(QApplication::translate(
            "Client", "Set a master password required to unlock the application."));
        dialog.setValidator([](CredentialsDialog* d) -> bool
        {
            SecureString new_password = d->password();

            if (new_password.size() < MasterPassword::kMinPasswordLength)
            {
                MsgBox::warning(d, QApplication::translate(
                    "Client", "The password can not be shorter than %n characters.",
                    "", MasterPassword::kMinPasswordLength));
                return false;
            }

            if (!MasterPassword::isSafePassword(new_password))
            {
                QString unsafe = QApplication::translate(
                    "Client", "Password you entered does not meet the security requirements!");
                QString safe = QApplication::translate("Client",
                    "The password must contain lowercase and uppercase characters, "
                    "numbers and should not be shorter than %n characters.",
                    "", MasterPassword::kSafePasswordLength);
                QString question = QApplication::translate(
                    "Client", "Do you want to enter a different password?");

                if (MsgBox::warning(d, QString("<b>%1</b><br/>%2<br/>%3").arg(unsafe, safe, question),
                                    MsgBox::Yes | MsgBox::No) == MsgBox::Yes)
                    return false;
            }

            if (MasterPassword::setNew(new_password) != MasterPassword::Result::SUCCESS)
            {
                MsgBox::warning(d, QApplication::translate("Client", "Unable to set master password."));
                return false;
            }

            return true;
        });

        if (dialog.exec() != QDialog::Accepted)
        {
            LOG(INFO) << "Master password set cancelled by user";
            return 0;
        }

        LOG(INFO) << "Master password set";
    }

    backupOnStartup();

    std::unique_ptr<MainWindow> main_window;

    // The lock destroys the window with everything it shows, and the unlock creates a new one.
    std::function<void()> create_main_window = [&]()
    {
        main_window = std::make_unique<MainWindow>();

        QObject::disconnect(pending_url_connection);

        QObject::connect(&application, &Application::sig_windowActivated,
                         main_window.get(), &MainWindow::showAndActivate);
        QObject::connect(&application, &Application::sig_urlOpened,
                         main_window.get(), &MainWindow::connectToUrl);
        QObject::connect(main_window.get(), &MainWindow::sig_lockRequested, &application, [&]()
        {
            // Until the new window there is no window on the screen, and closing the unlock dialog
            // would otherwise quit the application.
            const bool quit_on_last_window_closed = QApplication::quitOnLastWindowClosed();
            QApplication::setQuitOnLastWindowClosed(false);

            main_window.reset();
            pending_url_connection = QObject::connect(
                &application, &Application::sig_urlOpened, &application, remember_url);

            if (AppLock::unlock() != AppLock::Result::UNLOCKED)
            {
                QApplication::quit();
                return;
            }

            create_main_window();
            QApplication::setQuitOnLastWindowClosed(quit_on_last_window_closed);
        }, Qt::QueuedConnection);

        main_window->show();
        main_window->activateWindow();

        if (!start_url.isEmpty())
        {
            // Start the connection once the event loop is up and the window is shown.
            QTimer::singleShot(MilliSeconds(0), main_window.get(), [window = main_window.get(), start_url]()
            {
                window->connectToUrl(start_url);
            });
            start_url.clear();
        }
    };

    create_main_window();
    return application.exec();
#endif // defined(Q_OS_ANDROID)
}
