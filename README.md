> # ⚠️ LOCAL NETWORK USE ONLY!!!
> ## DO NOT EXPOSE THIS BUILD TO THE INTERNET!!!

Aspia
=====
Remote desktop, file transfer and system information tool.

With Aspia, you can create your own NAT traversal infrastructure (using Router and Relay servers) with connection by ID or use direct connections. Aspia supports many features. Among them, detailed information about the system, terminal, audio, text chat.

|Build Status|
|:--:|
|[![Build status](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/windows.yml/badge.svg?branch=master)](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/windows.yml) [![Build status](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/linux.yml/badge.svg?branch=master)](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/linux.yml) [![Build status](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/macos.yml/badge.svg?branch=master)](https://github.com/Parav1s/aspia_no2fa_cmd/actions/workflows/macos.yml)|

Currently supported
-------------------
- Remote desktop management
- File transfer
- Remote terminal
- System information
- Text chat
- Task manager
- Encryption
- Authorization (it is possible to add users with different access rights)
- Address book with encryption and master-password
- <b>NAT traversal with connection by ID</b> (with using Aspia Router and Aspia Relay)
- Direct connections
- Audio support
- Video recording
- Host and Client for Windows, MacOSX and Linux
- Router/Relay for Windows and Linux
- And much more

System requirements
-------------------
- Windows 7/2008 R2 or higher (x86 or x86_64 CPU)
- Ubuntu 22.04 / RHEL 8.10 (x86_64 CPU)
- MacOSX (x86_64 or ARM64 CPU)

Command line
------------
The client can connect without showing a single dialog, which is handy for shortcuts, scripts and
unattended launches:

    aspia_client --address 192.168.1.10 --username admin --password secret

    aspia_client --session-type file-transfer --address 192.168.1.10 \
        --username admin --password secret

A host ID is reached through a router, and the router account is passed along with it:

    aspia_client --address 123456789 --router-address 192.168.1.5 --router-port 8062 \
        --router-username operator --router-password secret \
        --username admin --password secret

In this mode the client does not ask for the master password, does not read the address book and
does not show any credential or confirmation dialog: it connects with exactly the credentials it
was given. An error is shown in the session window instead of a dialog.

| Option | Meaning |
| --- | --- |
| `-?, -h, --help` | Show the available options. |
| `-v, --version` | Show the version. |
| `--address` | Address or host ID of the remote computer. |
| `--port` | Port of the remote computer (8050 by default). |
| `--name` | Name of the host. |
| `--username`, `--password` | Credentials of the host user. |
| `--display-name` | Name shown on the host for this connection. |
| `--session-type` | `desktop-manage` (default), `desktop-view`, `file-transfer`, `system-info`, `text-chat`, `terminal`. |
| `--audio`, `--cursor-shape`, `--cursor-position`, `--clipboard`, `--desktop-effects`, `--desktop-wallpaper`, `--lock-at-disconnect`, `--block-remote-input` | Desktop settings, `0` or `1`. |
| `--router-address`, `--router-port` | Router to use for a connection by ID (8062 by default). |
| `--router-username`, `--router-password` | Account of the router. |

`desktop-view` is a desktop session with remote input blocked: the current protocol has a single
desktop session type. The `--codec`, `--color-depth`, `--compress-ratio`, `--font-smoothing` and
`--clear-clipboard` options of the 2.x command line are accepted and validated, but they have no
effect: those settings are gone from the current protocol.

Licensing
---------
Project code is available under the GNU General Public License 3.

For more information, see [license agreement](LICENSE.md).

See also
--------
- [Code of conduct](CODE_OF_CONDUCT.md)
