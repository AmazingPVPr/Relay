# Native add-ons, API v1

Relay add-ons are native Qt/C++ shared libraries loaded with `QPluginLoader`.
OBS input and Custom sounds are bundled add-ons. OBS input is enabled by
default; Custom sounds starts disabled. Relay does not load an add-on’s module
or create its service until you select its source or open its settings. Opening the OBS panel
does not start receiving a stream; use the panel's start control for that.

Use **Settings → Add-ons** to enable, disable, or configure an add-on. Enabled
share-source add-ons also appear under **Share Screen → What to share**. OBS
input configuration, preview, quality, audio and Start sharing are available
there without opening add-on settings.

Use **Settings → Add-ons** to open the legacy configuration panel. Disabling an
add-on closes its panel and stops its service. The preference survives restarts.
Closing a panel leaves its service available for reuse; an add-on can also stop
its work when its panel closes. The bundled OBS input stops on close unless it is actively streaming to voice. Disabling
an add-on always stops and releases its service.

## Install a local add-on

Use **Open add-ons folder** in the add-on settings. Copy a compatible `.so`
module into that folder, then choose **Refresh** or restart Relay. New local
add-ons start disabled. Enable one explicitly before opening it. Replacing or
removing an already discovered module requires restarting Relay; do not replace
a loaded library while the app is running.

The host examines only its bundled add-on directories and this designated local
folder. It does not download modules, search arbitrary folders, or load modules
just to list their metadata. Bundled IDs take precedence over local duplicates.

These are trusted native extensions, running with the same process privileges
as Relay. Install code you trust. This API is an extension interface, not a
sandbox or a signing system.

## Build the community example

The complete example in `addons/example` supplies a timer service and a settings
dialog. It needs CMake 3.24+, a C++20 compiler, and Qt 6.8+ development packages.
Build against the same Qt/toolchain and CPU architecture as your Relay build.

From the Relay source directory:

```sh
cmake -S addons/example -B build-example
cmake --build build-example
```

Copy `build-example/librelay_example_addon.so` into the add-ons folder, refresh
the list, then enable **Example timer**. The example is not included in the
bundled module directory or enabled by default.

The SDK header is `src/addons/AddonInterface.h`. A project outside this checkout
can point `RELAY_ADDON_SDK` at the directory containing that header. The example
has no dependency on Relay's private core library.

## Interface and metadata

Implement `RelayAddonInterface` on a `QObject` using:

```cpp
Q_PLUGIN_METADATA(IID RelayAddonInterface_iid FILE "metadata.json")
Q_INTERFACES(RelayAddonInterface)
```

The current IID is `org.relayclient.Addon/1.0`. The JSON metadata includes:

```json
{
  "apiVersion": 1,
  "id": "org.example.timer",
  "name": "Example timer",
  "description": "A small native add-on example.",
  "version": "1.0.0",
  "defaultEnabled": false
}
```

IDs use lowercase letters, digits, dots, and hyphens, and must contain a dot or
hyphen. An ID is also the settings namespace and must stay stable between
versions. `defaultEnabled` applies only to bundled modules; local modules always
require an explicit enable action when first discovered. `version` is displayed
to users; `apiVersion` and the IID select the supported interface.

The four native methods are:

| Method | Contract |
| --- | --- |
| `id()` | Return the same ID as the metadata. |
| `createService(QObject *owner)` | Return a fresh QObject parented to `owner`, on the application thread. Do not start capture or other work just because the service was constructed. |
| `stopService(QObject *service)` | Synchronously stop work, release devices, and join worker threads. It must be safe to call while idle. The host deletes the service afterwards. |
| `settingsUrl()` | Return a compiled Qt resource URL under `qrc:/relay/addons/<id>/`. |

Keep constructors and interface methods short. Put blocking or sustained work on
owned worker threads, then stop those threads in `stopService`. Do not throw
exceptions across the interface. Declare service operations with `Q_INVOKABLE`
and state with `Q_PROPERTY` plus change notifications.

The host creates at most one live service per enabled add-on. A disabled service
is stopped immediately, then its QObject is deleted after the settings UI has
been told to close. Re-enabling creates a fresh service when next opened. The
module code and resources remain loaded until the process exits, avoiding stale
QML component references to unloaded native code. The Qt plugin root is not the
service and must never be returned from `createService`.

## Settings UI

Bundle `Settings.qml` into the shared library using `qt_add_resources`. Use the
ID namespace in the resource prefix, as the example's CMake file does. The
settings root is a `Dialog` with:

```qml
required property var service
```

The host supplies this property before creation and opens the dialog. Guard
bindings against a null service because disabling the add-on deletes it. The
example uses standard Qt Quick Controls; the bundled OBS panel also uses Relay's
shared presentation components. No browser engine is involved.

The native interface does not receive the Discord client, authentication tokens,
voice engine, or a message API. It supports a service, its own settings UI, and
the optional voice PCM hook below. It does not offer message interception or
account commands. The QML settings loader shares Relay's engine and process;
these restrictions are not a security boundary against trusted native code.

## Optional voice PCM capability

The bundled Custom sounds add-on declares `"capabilities": ["voice-pcm"]` in
metadata. An enabled add-on with this capability can expose these Qt signals:

```cpp
void voiceClipRequested(const QByteArray &pcm);
void stopVoiceClip();
```

The host supplies writable service properties `voiceAvailable` (bool) and
`outputDeviceId` (QString) if declared. PCM must be interleaved stereo signed
16-bit samples at 48 kHz, at most 30 seconds per clip. The host mixes one clip
at a time into outgoing encrypted voice after microphone processing. A new
clip replaces the previous clip. Mute, deafen, server mute/deafen, leaving voice,
or disabling the add-on stops its clip. A clip never unmutes or joins a call.
The output ID lets a local preview follow Relay's selected speakers.

Custom sounds imports local files only after an explicit file selection, copies
them into its application-data library, and provides rename/remove, local preview,
volume and Play in voice controls. It limits files to 16 MB, the library to 200
clips and decoded voice clips to 30 seconds. It supports WAV, MP3, Ogg, FLAC, M4A
and Opus when the installed native media decoder supports them. Disabling the
add-on stops decoding and previews. Core Discord server/default soundboards are
separate and remain available without this add-on.

## OBS module

The `relay_obs` module contains `StudioBridge`, its FFmpeg receiver, its settings
resource, and its metadata. Relay's core no longer instantiates `StudioBridge`.
It accepts a local RTMP feed or reads an existing RTMP/RTMPS server and previews
video. Existing-stream mode is a playback client; it does not replace the OBS
publisher or change other viewers. Input changes are available while stopped.
Remote reads have bounded network waits and explicit cancellation; connection
details remain in memory and are excluded from status/errors and saved settings.
The module declares the optional broadcast-source capability below. The host
can stream its bounded video/audio mailboxes to the current voice channel after
an explicit Start sharing action. Disabling the module stops its Discord stream,
receiver, and video sink.
Removing the OBS module does not remove messaging or voice from Relay.

## Optional Share Screen contribution

A broadcast-source plug-in may additionally declare `"share-source"` in its
metadata capabilities and implement `RelayShareSourceInterface` on its plug-in
root QObject, using `Q_INTERFACES(RelayShareSourceInterface)`. This separate IID
preserves the base add-on and existing media-source interfaces.
`shareSourceUrl()` returns a compiled resource under
`qrc:/relay/addons/<id>/`. The editor root is an `Item` with
`required property var service`; it can optionally expose `hostHeight` to adapt
its layout to the share dialog. The host validates the URL namespace and
resource, then supplies the same manager-owned service used by Settings.

Enabled contributions are listed from metadata without executing their module.
Only selecting a contribution loads its module and editor. Disabled, absent and
noncontributing add-ons are excluded. Settings remains available independently.
The host provides one resolution/frame-rate/audio selection and an explicit
Start sharing/Stop sharing control for both built-in and contributed sources.

A contributed service implements `RelayBroadcastSourceInterface` and exposes
notifying boolean `running` and `receiving` properties, plus a notifying
`QVariantMap broadcastContext` exposing the supplied context and host errors.
Both readiness booleans must be true before
the host permits publication. Constructing a service or opening an editor must
never start capture or broadcast. The editor may offer explicit Connect/Preview
controls. Releasing its editor must detach preview sinks and release idle input;
when `broadcastContext.active` is true, retain the source independently of the
editor, including while Discord allocation is still pending and `sharing` is
false. Disabling an add-on always stops its input and publication. No Discord
credentials, channel IDs or account objects are passed through this interface.

## Optional broadcast-source capability

An add-on declaring `"capabilities": ["broadcast-source"]` can implement
`RelayBroadcastSourceInterface` from `src/addons/BroadcastSourceInterface.h` on
its service QObject, using `Q_INTERFACES(RelayBroadcastSourceInterface)`. This
optional interface requires Qt Multimedia and retains API v1 of the base add-on.

The host supplies `setBroadcastContext()` with availability, voice channel display
name, active/sharing/busy status and errors. This contains no Discord credentials,
voice sessions or account/channel IDs. The service can expose this state to its
settings UI. An explicit user action emits `broadcastRequested(int height, int
fps, bool audio)`; `broadcastStopRequested()` stops only that service’s stream.
The host rejects requests without a ready input and current voice connection,
or while another stream is active.

`setBroadcastCaptureEnabled()` arms or disarms media production. The host polls
`takeBroadcastMedia()` at a bounded cadence; each call consumes the latest video
frame and available 48 kHz stereo signed-16 PCM. Retain at most one video frame
and 200 ms of audio, discarding older media under pressure. Report ended input
or a fatal error to stop streaming, and an audio-only warning to keep video going.
The host owns encoding, Discord signaling, DAVE encryption and transport. The OBS
module preserves video up to the selected output quality and uses planar YUV
instead of scaling the broadcast down to preview resolution.

Services can additionally implement `RelayEncodedBroadcastSourceInterface`
(`src/addons/EncodedBroadcastSourceInterface.h`). This separate optional IID
preserves the existing decoded-source ABI. A request with height=0 and fps=0
selects source quality. The host passes those zeros only to providers exposing
the encoded interface; legacy services receive explicit height/frame-rate limits.
`takeEncodedBroadcastVideo()` consumes bounded complete Annex B H.264 access units
with original presentation timestamps in 90 kHz units. Compatible 8-bit 4:2:0
baseline/main/high video up to 3840×2160/60 may bypass the host encoder. Include
SPS/PPS with each IDR; retain at most 16 access units and 8 MiB. On overflow,
discard the dependent frames and resume at an IDR. Disarming must clear both
mailboxes. DAVE encryption and packet transport remain owned by the host, and
unavailable/incompatible encoded input retains the decoded-frame conversion path.
The host context also supplies a credential-free `quality` display string.

Leaving or moving voice channels, disabling the source, or destroying it stops
the stream. Closing settings may preserve an active source; the OBS panel does
this and detaches its preview sink when destroyed. Stopping sharing alone may
leave the local input available for another start. Receiving input and opening
settings never imply consent to publish it.

## Tests and compatibility

`tests/addon_test.cpp` loads a real fixture `.so` from a separate test directory.
It verifies discovery without service creation, lazy activation, live QML
bindings during disable, deferred destruction, persisted preferences, local
opt-in, shutdown, and duplicate ID handling. The test fixture is never installed.

API v1 is an initial C++ ABI, not a promise that binaries built with any compiler
or Qt release will be interchangeable. Rebuild community modules when the host
toolchain or interface changes. Qt also performs plugin compatibility checks.
See Qt's [plugin guide](https://doc.qt.io/qt-6/plugins-howto.html) and
[QPluginLoader reference](https://doc.qt.io/qt-6/qpluginloader.html).
