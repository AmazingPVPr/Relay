# Relay on other computers

Relay has Linux and Windows desktop build paths. The existing Linux development
archive uses system libraries and is not a universal binary. For distribution
testing, use the Flatpak bundle or the Windows portable ZIP. Neither
copies the developer's accounts, settings, wallet or media.

## Testers

Linux: install Flatpak using your distribution and enable Flathub. Install the
provided Relay-linux-x86_64.flatpak bundle with your software manager, or:

    flatpak install --user Relay-linux-x86_64.flatpak
    flatpak run org.relay.Relay

The first install downloads the KDE runtime and graphics driver extension.
A desktop session with functioning Flatpak/desktop portals is required.
Wayland screen sharing uses your desktop's permission chooser. PulseAudio or
PipeWire's PulseAudio compatibility service supplies audio. Camera and screen
capture depend on desktop/runtime support. The Flatpak declares access to hardware devices for Qt's camera backend and
graphics. This broader permission was explicitly approved for these packages.
Device access still follows the operating system's permissions. Files use the system file chooser;
the package does not request blanket home access.

Windows 10/11 x64: double-click Relay-Setup.exe to install for your Windows user
and create a Start menu shortcut. No command prompt, Python, MSYS2, or administrator
access is required. For the portable option, extract the whole ZIP and double-click Relay.exe. Keep bin,
its DLLs, QML modules, plugins, and add-ons together. MSYS2, Qt and Python are not
required to launch the package. Allow desktop microphone/camera access in
Windows privacy settings and select the expected devices in Relay.

Sign in on each machine with your own account. QtKeychain uses Windows
Credential Manager or standard Linux Secret Service/KWallet. The Flatpak
requests access to those desktop credential services. Private certificate
authorities must be installed in the operating system trust store.

## Graphics

Hardware acceleration is enabled by default through Qt Quick. Qt chooses
Direct3D on Windows and its supported native backend on Linux. Relay does not
force a GPU vendor, device path, CUDA, Vulkan or desktop environment.
The saved hardware-acceleration setting applies after restarting.

If a driver prevents startup on Windows, make a shortcut to Relay.exe and add
`--software-rendering` to its Target. On Linux use:

    flatpak run org.relay.Relay --software-rendering

This one-launch override does not change the saved preference. Qt Multimedia
presentation uses Qt's platform paths. Direct Discord video decoding and
screen-share H.264 encoding still use the existing CPU codecs. The OBS add-on
can forward an already encoded OBS stream without that local re-encode.

For an offline startup check:

    flatpak run org.relay.Relay --demo --quit-after 3000 --diagnostic-status /tmp/relay-status.json

Windows: run bin\relay.exe with those arguments using a writable Windows status
path. The status reports Qt version, OS, architecture and rendering API without
messages or tokens. A non-software API does not prove physical GPU use;
WARP/llvmpipe may use the CPU.

## Test on each system

- Sign in, messages, Unicode/spaced attachment paths, clipboard images,
  notifications, notification clicks and remembered login after restarting.
- Voice, noise suppression, echo cancellation, device selection, changing the
  output device, screen/window capture, share audio and viewing a friend's stream.
- Shortcuts while another app has focus. Linux uses the standard desktop portal;
  Windows uses RegisterHotKey and reports reserved/already-used shortcuts.
- OBS input, start/stop and reconnect where OBS is installed.

Windows uses WASAPI default-output loopback for computer audio, SpeexDSP for
echo cancellation/automatic gain, and RNNoise for noise suppression. An output
device change can require sharing again. It never substitutes microphone audio.
Windows notifications use the system tray; sounds follow Windows policy. These
are not a full Action Center/toast implementation.

YouTube extraction is an optional separate helper. These portable recipes omit
the host's Linux helper. Direct media still plays through Qt; unsupported
YouTube embeds can be opened in a browser. A later helper bundle must contain
the correct platform executable and JavaScript runtime, plus their licenses.

Windows Relay.exe checks published Windows releases, including previews, on launch.
It verifies the Windows metadata signature and archive SHA-256 before installing
into %LOCALAPPDATA%/Relay/WindowsUpdates. It switches versions only after extraction
completes and retains the previous build. Skip update cancels a check or download;
an unavailable update leaves the installed version available. Failure details are
written to last-update-error.txt in that folder. Launching bin/relay.exe directly
bypasses update checks. Older preview ZIPs require one manual replacement because
they did not contain this updater. Keep the whole extracted folder together;
Relay.exe is the entry point, and no command window or Python installation is needed.

Flatpak updates use a repository or a replacement bundle. The development-host
Linux updater and LD_LIBRARY_PATH launcher are omitted from portable packages. Native add-ons must match the platform, architecture and Qt ABI.

## Builders

Development work in this checkout runs on Vault under the top-level AGENTS.md.
The following recipes also describe platform-native builds for downstream
builders of a source-only distribution. They do not publish, deploy or restart.

Linux: packaging/flatpak/build.sh builds the pinned manifest using KDE
Platform/Sdk 6.10 and exports a bundle and checksum. Source modules build
offline after downloading. Desktop/driver libraries come from the runtime;
application codecs, speech and encryption libraries are supplied by the recipe.

Windows: install the packages in packaging/windows/packages.txt in an MSYS2
UCRT64 toolchain, then run packaging/windows/build.sh. It builds pinned DAVE
and MLS++, fetches the pinned direct-messaging library and its submodules, requires encrypted voice, builds fixtures, deploys Qt/QML plugins,
collects DLL dependencies and license notices, checks the packaged demo with
the developer PATH removed, and creates the ZIP and checksum. Linux DBus
fixtures are excluded on Windows; they do not verify Windows integrations.

Use -DRELAY_AUDIO_PROCESSING=speex to test the portable speech backend where
SpeexDSP development files are installed. Linux retains its WebRTC default.
The Vault workflow still runs the complete Linux regression suite.

The preview packages are built on Vault. Windows uses a consistent UCRT64
GCC/Qt dependency set; the native MSYS2 recipe is also supplied for downstream
builders. The Windows executable is linked and checked under Wine with generated
fixtures and a clean packaged demo. This is not a physical Windows certification.
The Linux bundle is checked inside the KDE 6.10 SDK and its separately installed
desktop runtime. See build-validation.json with the release for the exact results.

Physical GPU acceleration, microphones, cameras, screen-sharing portals, OBS and
live Discord calls still require testing on each target computer. Record
OS/version, desktop session, GPU/driver and the failing step. Never send tokens,
wallet databases or private conversations.

Preserve all dependency notices and corresponding source revisions when
redistributing packages. Qt and codecs remain dynamically linked so users can
replace those libraries. FFmpeg/x264 packages may be GPL-enabled; retain their
notices and matching source locations with the package.
