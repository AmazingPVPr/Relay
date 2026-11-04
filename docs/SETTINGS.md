# Settings and external controls

Open the gear beside your account. The sidebar includes searchable categories and section links, with native Account, Messaging Permissions, Notifications, Voice & Video, Accessibility, System, Language & Time, and add-on screens. Local appearance, audio, and server-layout preferences save on this computer; account, privacy, security, and email controls change Discord only after an explicit action. The graphics backend requires a restart.

## Server order and folders

Drag a server or folder above or below another item to reorder it. Hold a server over the center of another server briefly, then release to create a folder. Drop onto a folder to add to it. Expanded folders show their servers; drag a child outside to remove it. Right-click a folder to change its name/color or ungroup its servers. Escape cancels a drag. The rail scrolls when dragging near either edge.

With a server or folder focused, Ctrl+Up/Down reorders it, Ctrl+Left moves a child out, and Shift+F10 opens arrangement options. Layouts save separately for each account, including folder expansion. This is local Relay organization; it does not overwrite Discord's server order. Temporarily unavailable servers retain their saved positions.

## Account and security

Native forms edit display name, username, email, and password. Sensitive changes require the current password when Discord requires it. Authenticator enrollment generates a temporary local key and QR code, then submits the code only when confirmed. Recovery codes remain in memory until the dialog closes or expires. Protected operations can show a native TOTP, backup-code, or password challenge using only methods Discord offers. Account secrets and responses are not saved in settings or diagnostic files; remembered-session rotation uses the desktop wallet.

Devices, authorized apps, connections, blocked/ignored people, billing, subscriptions, boost slots, gifts, and family links load when those pages are opened. Disconnect, deauthorize, unblock, stop ignoring, and other-device logout operate on the selected loaded record; Discord may require additional verification. Payment methods display limited identifying details; checkout, purchases, phone changes, passkey enrollment, account deletion, and full Family Center management are not implemented in this pass. Unsupported options are identified rather than presented as working switches.

Messaging controls use Discord's actual saved preference data and preserve unrelated fields. Sensitive-media and spam-filter choices, per-server DM overrides, and personalized friend-request messages are not editable here yet. The DM switch changes the saved default rather than overwriting each server's settings. Email preferences load only after entering their section or pressing Load; opening local notification settings does not request account data. Turning off marketing emails changes only Tips, Recommendations & Events, and Updates & Announcements. Live account-changing flows are deliberately not exercised by automated tests.

An account change already sent to Discord keeps its response handler alive, including any replacement sign-in credentials. Settings navigation and closing wait for that response or the request timeout; the dialog displays its saving state. Passwords and verification input clear after submission. Closing an idle authentication dialog clears its temporary secrets and backup codes.

## Accessibility and time

Chat text size (12–24 px), message spacing, compact messages, list density, role color display, link underlines, high contrast, reduced motion, and GIF animation apply to the interface and save locally. Message dates/times honor Auto, 12-hour, or 24-hour formatting and English US/UK locales. Other interface translations and text-to-speech are not included yet.

## Hidden channel metadata

Enable **Add-ons → Show hidden channels** to display locked channel metadata Discord has already delivered. Select a dimmed entry to see its name, category, topic, bitrate, and limit when those fields are present. Relay does not request messages, subscribe to hidden member lists, send messages/invites, or join voice through that view. Permission updates clear any displayed history and pending selection when access is revoked; a newly granted channel becomes usable normally. Disabling the add-on hides those entries again.

## Appearance

- Discord-style dark colors and blurple controls, rounded server tiles, native image rendering.
- Interface scale from 80–140%, independent chat text size, compact messages, reduced motion.
- Hardware acceleration chooses Qt's normal GPU renderer or its software renderer.
  This switch controls interface rendering. Stream decoding currently uses FFmpeg's
  CPU decoders on a separate worker thread; it is not a hardware video decoder toggle.

## Voice and audio

- Saved microphone/output choice, including following the system default.
- Input and voice output volume, 0–200%, with sample clipping limits.
- Private microphone playback test inside or outside a call. During a call, Relay temporarily shows you as muted and deafened, keeps the connection, and restores your previous choices when testing stops. Test audio is played locally without recording or transmission.
- RNNoise noise suppression, WebRTC echo cancellation, and automatic gain control.
- A voice activation threshold from −70 to −10 dBFS, with a 200 ms hold for word endings.

Processing is optional and initially disabled. RNNoise uses 10 ms frames and a
small lookahead delay; the client continues to encode 20 ms Opus packets.
Quality depends on the microphone and environment. These checks do not establish
quality equivalence to Krisp. Echo cancellation uses Relay's voice playback as
its reference, not all sound played by other applications.

Krisp integration requires a commercial license. The native alternative here uses
system RNNoise and WebRTC Audio Processing libraries:
[Krisp licensing](https://sdk-docs.krisp.ai/docs/licensing-information),
[RNNoise](https://jmvalin.ca/demo/rnnoise/).

## Buttons and automation

These commands control the running Relay app and its actual Discord voice state:

```sh
./run.sh --toggle-mute
./run.sh --mute
./run.sh --unmute
./run.sh --toggle-deafen
./run.sh --deafen
./run.sh --undeafen
./run.sh --disconnect-voice
./run.sh --status
```

Assign a command to a desktop shortcut, stream-deck button, or another local
controller. Each command returns one JSON result and a success/failure exit code.
Only one action may be supplied per call. Status includes connection/mute/deafen
flags, with no account identities or messages.

The local socket is restricted to the same OS user; it is not a network service.
No listening TCP port, token, or Discord credential is needed. For a remote device,
use an existing authenticated connection or button companion on this computer to
run the command; Relay does not expose an unauthenticated remote-control port.

Keyboard settings remap microphone, deafen, and disconnect actions, validate
conflicts, and save the choices. Default shortcuts are Ctrl+Shift+M,
Ctrl+Shift+D, and Ctrl+Shift+H. Desktop-wide shortcuts use the system's Global
Shortcuts portal after an explicit enable action and its approval dialog. Enable
them each time Relay starts; your desktop manages its own remembered choices. The
desktop can override or reject a requested binding; unsupported desktops can use
the commands above in their own shortcut settings.

## Add-ons

OBS is under **Add-ons → OBS input → Open**. It is bundled and enabled by default,
but is loaded only when opened. **Direct from OBS** receives a stream on this
computer after **Start preview**. **Existing RTMP stream** reads from your
existing server after **Connect**: use the server and stream key configured in
OBS, or the full playback URL with the key blank. Your RTMP server must allow
viewers; OBS can keep its existing destination and the server can serve both
Relay and the other PC. The stream key is masked. **Remember this input** is on
by default and saves the input source, server or full URL, and stream key in
your desktop wallet. The saved fields refill when you open the add-on after
restarting Relay; connecting and sharing still start only when you choose.
Turn Remember off or select **Forget saved input** to remove the saved copy
and disable automatic restore. Your current fields remain usable for this
session. If the wallet cannot save the input, Relay explains the problem and
offers **Save again**; it keeps the current fields without saving secrets in
ordinary preferences. Closing the panel disconnects an idle preview. Once connected, join voice, choose
resolution/frame rate and **Stream audio**, then press **Start sharing**. Closing
the panel keeps a live stream running. **Stop sharing** ends the Discord stream
while keeping its input connected; **Stop input** disconnects the feed too.
Disabling the add-on or leaving/moving voice channels stops its stream.
It can be disabled. **Custom sounds** is optional and starts disabled; it can also be enabled from the Soundboard window. See [the add-on SDK](ADDONS.md) for an example community module.

## Streams

Join the sender’s voice channel and click the red **LIVE** badge. The player
appears inside Relay. **Pop out** moves it to an independent window; activating
Relay does not raise that window. **Return to app** brings it back. **Fit** shows
the full image. Crop presets include 16:9, 21:9, 32:9, 16:10, 4:3 and 1:1;
Custom accepts width:height ratios from 1:10 to 10:1. Drag to pan, scroll to zoom,
and double-click to reset. Hover controls overlay the video without resizing it.
Fullscreen uses the window's current monitor. Cropping never changes the sender's
stream or another viewer's image. Audio volume/mute in this window is independent
of voice volume; deafen silences both.

The native receiver negotiates H.264 or VP8 with Opus audio and DAVE encryption.
Codecs not advertised by this build are not supported. Decoding preserves source
resolution, including 5120×1440, with bounded packet and decode queues.
Live Discord stream video and audio were confirmed in an account-level viewing test.
Local crop geometry and software-rendered pixels were verified with generated media.

Streams start in a floating player above the message feed. Drag its title bar to move it and its bottom-right corner to resize it. Expand stream uses the main area; Show chat returns it to the floating view. Selecting a text channel also restores chat while playback continues. Pop out keeps an independent window.

Voice activity rings follow decoded audio levels, ignore comfort noise, and release after a brief pause. Gateway speaking flags alone do not illuminate the ring.

## Layout, quick controls and exact values

Drag the dividers to resize the server rail, channel sidebar or member sidebar.
Wheel scrolling moves 160 logical pixels per step by default and is adjustable in Appearance. Mouse drivers that report pixel movement without wheel angles use the same setting; trackpad pixel scrolling
keeps its precision. Discrete mouse-wheel steps also use the selected speed when Wayland labels the device as a touchpad. Lists stop at their boundaries without a bounce. Click a
displayed numeric value beside a slider, where available, to enter an exact value, then press Enter. The microphone
and headset arrows open quick device/volume menus. Opening a menu never starts
microphone capture. The voice-connection panel displays measured heartbeat round
trip times, their recent average and encryption status; it does not estimate an
audio latency or invent a packet-loss measurement.

## Notifications and sounds

Settings → Notifications controls desktop notifications, direct messages,
mentions, previews, focused-window behavior, and notification sound cues. Sound-effect volume is under Voice & Video. The desktop’s
Do Not Disturb settings still apply. Incoming notifications follow known Discord
mute settings and only arise from new gateway messages. Clicking a notification
opens the matching conversation. Test notification creates only a local toast.

Join/leave, mute/unmute and deafen/undeafen have distinct cues. Stream start/stop
and starting/stopping a stream or camera viewer also have distinct cues. Joining
or leaving server membership also produces a cue; initial server loading does
not. Sounds follow your selected voice output device and remain audible when
you deafen; they are local feedback and are never mixed into your microphone.
Sound effects have their own volume, independent of voice volume, and start at
50%. The bundled cues use consistent levels with headroom to avoid clipping.

Settings → Voice & video also includes activity-sound enable/volume controls.
Press **Preview Sound** beside a cue to test your selected output without
changing your microphone or call. This explicit preview works even when its
automatic cue or activity category is disabled; sound-effect volume still applies,
so zero volume stays silent. If the chosen output was unplugged, cues use the default output until
it returns. A missing output or failed cue is reported beside the preview.

The included short sound cues are original synthesized assets, not recordings
of Discord’s sounds. Voice participant and stream cues apply to your current
voice channel, not every server.

## Soundboard

The **Soundboard** button beside Share screen opens server and default sounds.
Search or switch tabs, use the small speaker button for a local preview, and
click a sound tile to play it in your current voice channel. Voice and soundboard
permissions apply. The native server board follows Discord's rules: microphone
self-mute can remain enabled, while deafen and server restrictions prevent play.
Soundboard volume affects what you hear. Received sounds respect local per-user
mute and deafen. Short clips download fully before decoding, use a bounded memory
cache, and display loading/download/decoding errors rather than failing silently.

**Enable custom sounds** opens the optional add-on. Import local clips, rename
or remove them, preview them locally, and press **Play in voice** while connected
and unmuted. These clips mix with your microphone before encrypted voice
transmission. Mute/deafen, leaving voice or disabling the add-on stops the clip.
The local library is stored in Relay's application-data folder. Custom sounds
are limited to 16 MB per imported file and 30 seconds when played into voice.
Uploading or administering Discord server soundboards is not implemented.

## Screen sharing

Join voice, choose **Share screen**, pick a screen/window, quality and frame rate,
and start sharing. Wayland uses the desktop’s screen picker. Optional computer
audio captures the default output’s monitor through PulseAudio/PipeWire, not the
microphone. Stop sharing releases capture. Existing voice audio remains separate.
The OBS add-on can broadcast a direct OBS or RTMP input instead of a screen.
Its default **Source quality / Source fps** preserves compatible H.264 video
without a second compression pass, up to 4K/60. The live quality line shows the
actual resolution and source frame rate, and **Original video** when forwarding
is active. Inputs requiring conversion and manual quality choices use a higher
quality encoder; a smaller source is never enlarged. Discord and network limits
still apply. OBS keyframes every two seconds allow prompt starting/recovery.
Its **Stream audio** option includes only that feed’s audio, without recording
the computer’s output or microphone, encoded as 256 kb/s stereo Opus.

## Messages and profiles

Click a name/avatar to see the user’s profile; click a server tag to see available
server details. One-to-one direct messages have a profile sidebar. Role/device/
activity indicators reflect information actually supplied by Discord; unavailable
presence or partial member lists are labeled rather than inferred. Message times
include Today, Yesterday or a full date; hovering shows the full timestamp.

Member sections use each online member’s highest role configured to display
separately, followed by Online and Offline. Offline members leave their role
section and appear dimmed under Offline; unknown presence is labeled Members.
Names are sorted alphabetically within each section.

Scroll upward to load earlier messages automatically. Your reading position is
preserved as pages arrive. History stays bounded in memory while you can continue
scrolling in either direction; **Jump to present** returns to the latest messages.
The visible message stays at its pixel position when older pages arrive, even at
the exact top. Layout changes alone cannot request another page. Scrolling toward
the edge arms the next request; repeated or invalid pages stop instead of looping.
Live messages cannot evict the row being read from a full history window.
Click a reply preview to center and briefly highlight its original message. Relay
loads surrounding history if needed. Deleted messages cannot be opened; an
unavailable preview can still be tried, and failed loads leave your history intact.
Mouse-wheel movement eases out over 110 ms with the same configured distance.
Touchpad motion stays direct; Reduced motion disables wheel smoothing.
Multiline channel descriptions have a single-line header preview and show their
full text on hover. Video fullscreen opens on the video’s current monitor.

GIF search uses Discord’s public Klipy endpoint. Selecting a GIF, emoji or date
inserts it into the draft and does not send it. GIF favorites are saved locally.
Translation is explicit: opening its dialog makes no request; **Translate** sends
only the selected text to Google. A translation can be copied or inserted into a
draft, never sent automatically. Provider limits/errors are shown in the dialog.

Hover over a server icon for its name, cached voice participants, stream
indicators and notification mute state. Online/offline counts appear when Discord
provides the required totals; unknown counts are omitted. Hovering does not join
voice, start a stream or fetch server management data.

## People and server management

Right-click a person to view their profile, message or mention them, change local
voice volume/mute, block/unblock, or use permitted server moderation actions.
Local voice volume ranges from 0–200% and is saved for the current Relay account.
Local mute affects only what you hear; server mute/deafen changes Discord's voice
state for that person. Role and moderation options follow server permissions and
role hierarchy.

Right-click a channel for its chat, voice invite, link, editing, duplication or
deletion. A server's header or icon opens its menu. **Server Settings** provides
profile and image editing, roles and permissions, member lists, invites, bans,
audit history, boost information and scheduled events. Channel creation supports
text, voice, category and forum types. Private-channel changes preserve unrelated
permission overwrites.

## Forum discussions

Select a forum channel to browse active and archived posts. Tags, title search
within loaded posts and activity/newest sorting help find discussions. **More
posts** loads another archived page, up to a 500-post window. Open a card to read
and reply in the normal chat view. Its back arrow returns to the forum and keeps
your discussion draft.

**New post** accepts a title, message and up to five server tags. Required and
moderator-only tags follow the forum settings; known permissions control creating
posts and replying. Locked discussions stay readable. New posts currently support
text; outgoing attachment uploads remain unavailable.

Event dates are entered in your local time and sent as a shared absolute time.
Marking an event as interested updates your Discord RSVP. Boost billing and the
remaining Discord management features are listed under Current limits in README.
No moderation, role, channel, invite, or event writes are performed automatically
when a menu or settings section is opened.

## Watching cameras and stream quality

Join the participant’s voice channel, then click the camera button next to their
name to watch their live camera. The feed can float over chat, expand, pop out or
use fullscreen. Camera audio uses your normal voice-call controls. Stopping the
viewer leaves the voice call connected; the viewer closes when the camera turns
off, its participant leaves, or you leave/move voice. Viewing never opens or
transmits your own camera.

Received H.264/VP8 video retains source resolution and decoded color/range
metadata. Relay requests the best active rendition and recovers damaged or
overloaded prediction chains from a clean keyframe rather than displaying
corrupted dependent frames. Presentation polling supports 60 fps input. Sender,
codec and network limits can still affect the actual received picture.

Voice & Video and Notifications settings have an activity-sound master switch
and individual cue previews; Voice & Video also has the sound-effect volume control. Stream activity sounds include starting and
stopping viewing, for both streams and cameras. Previews follow the selected
output device. Disabled categories silence automatic cues; explicit previews still use the selected sound-effect volume.
