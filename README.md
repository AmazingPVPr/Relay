# Relay

Relay is an unofficial native Discord client with optional direct Relay-to-Relay streaming and encrypted text chat.

## Downloads and updates

Download builds from [Releases](https://github.com/AmazingPVPr/Relay/releases).
The Linux runtime targets x86-64 Arch-compatible systems and requires system Qt and multimedia libraries. Extract the complete archive and run `run.sh`. The launcher checks authenticated updates on startup and keeps the installed version if an update is unavailable or cannot be verified.
Windows installers and portable ZIPs are published separately; a Linux update does not update Windows clients. Check each release's platform and validation notes. The original Flatpak preview is a separate package.

## Check for updates

The main window always shows **Check for updates**. **Settings → Updates** shows the running build, available build, last check and any error. **Update and restart** asks before disconnecting calls and streams. Windows uses its bundled native updater, with no Python requirement.

## Direct streaming alongside Discord

Start a screen, window, or OBS add-on stream normally. Leave **Also share directly with Relay contacts** enabled. Discord viewers use the normal Discord stream; connected, accepted Relay contacts can choose **Watch via Relay** in chat or **Relay** beside your voice-channel entry.

Both people need a compatible updated client and an enabled Relay chat connection. The separate screen encoder supports a 1–40 Mbps target (20 Mbps initially). Compatible OBS H.264 video retains its original bitrate and resolution. Each of up to eight direct viewers uses an additional copy of your upload. Direct connections use encryption and expose peers' network addresses. Some networks cannot connect directly; there is no TURN fallback. For OBS, use a two-second keyframe interval.

See [Relay chat and streaming](docs/relay-private-messaging.md), [portable builds](docs/PORTABILITY.md), and [validation](TESTING.md). Application source, build recipes, and generated regression fixtures are included in this repository. Optional private add-ons, accounts, settings, private keys, personal media, and build outputs are excluded.
