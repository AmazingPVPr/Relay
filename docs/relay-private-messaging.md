# Relay private messaging

## Connect from a Discord DM

Open a one-to-one Discord DM in Relay, choose **Relay**, and check **Enable Relay chat with this person**. Relay sends a clearly labelled automatic setup message. The other person receives an invitation notification and accepts by enabling the same checkbox. Both people must run this updated Relay client.

Discord carries the public Relay identity and encrypted connection setup, including encrypted ICE addresses. Messages begin “Relay connection setup — sent automatically. Please don't reply.” Relay displays compact descriptions of these automatic messages. The private chat text, delivery acknowledgements, local histories, and MCP data do not pass through Discord. The automatic path does **not** connect to the AmazingPVPR.com rendezvous service. STUN is still used for direct route discovery. There is no TURN relay or silent fallback for chat text; restrictive networks may fail to connect.

The first key is trusted through the authenticated Discord DM account. Relay pins it locally; a different key stops the connection and requires explicit review in **Details → Unlink trusted identity**. Compare the full identities with your friend over another trusted channel for stronger verification. Discord can observe that an invitation/setup exchange occurred. Initial discovery depends on trusting Discord's account and DM delivery; end-to-end encryption does not protect a compromised endpoint or Discord account during first pairing.

Unchecking the conversation disables its transport and AI grant, sends a best-effort automatic off notice, and preserves local history. Offline notices may not reach the other person until connectivity is available. An account change closes direct connections and revokes the running MCP endpoint. Connection choices and pinned keys are encrypted in this Relay profile's contact save. MCP still has only Relay list/read/send tools; it cannot read Discord history, send Discord messages, or enable a conversation.

The default chat page shows only the connection checkbox, status, MCP switch, messages, and composer. Identities, configuration JSON, local-save information, and manual connection controls are under **Details**. Enable MCP for the current launch, then separately allow AI in a conversation. Incoming text is untrusted content, never permission to execute instructions.

Offline invitation discovery examines the latest 50 messages in up to 128 changed, accepted one-to-one DMs per scan using the client's existing permissions and rate limiter. Older invitations can be resent with **Reconnect**. Invalid, foreign-account, blocked, bot, webhook, group, and server messages cannot authorize a connection. Encrypted setup chunks expire after 90 seconds, are bounded, and must authenticate to the pinned peer key.

## Manual connections and storage


Standalone manual mode remains available through **Relay private text** in the home sidebar or sign-in screen, or Ctrl+Alt+R. It exchanges contact cards explicitly and does not require Discord sign-in. The following setup-service steps apply only to that manual mode.

## Pair and connect

1. Both people install this Relay version. Open **Details → Manual connections**.
2. Exchange Relay contact cards over a channel you already trust. Each person adds the other's card. Check the full 64-character identity with your friend; names are local labels, not verified identities. The card contains a public identity and setup-service URL, never the private key or message history.
3. Enable Online on both clients. They authenticate to the setup service and automatically attempt a direct WebRTC connection using ICE and STUN. No router changes or VPN are required on supported networks.
4. Send plain text (up to 4,000 UTF-8 bytes per message). There are no attachments, images, media tracks, embeds, link previews, translations, or file-transfer tools in this channel. Pasted URLs and markup remain inert text.

Direct-only communication cannot cover every NAT/firewall combination. This version has no TURN relay, central message delivery, or fallback through Discord. If a direct route is impossible, messages remain locally pending. Both clients must overlap online to deliver messages. The setup service is necessary for fresh manual-mode connection attempts; established direct conversations survive its disconnection.

Manual Online and the local MCP listener start **off** after every application launch. Enabled Discord-assisted conversation choices resume after that Discord account reconnects. Per-conversation AI grants persist until revoked, a conversation is disabled, or its account changes. Independent account windows use distinct application profiles and Relay identities; changing Discord accounts within the main window does not change its independent Relay identity. There is no implicit Discord login requirement for Relay text.

## What each component sees

- Clients exchange text through an ordered WebRTC SCTP data channel protected by DTLS and fingerprint verification. DTLS provides ephemeral session encryption. Ed25519 identities authenticate the setup exchange; libsodium's Ed25519-to-X25519 conversion and authenticated `crypto_box` protect SDP (including the DTLS fingerprint) end to end between pinned contacts.
- In manual mode, the memory-only rendezvous service authenticates each connection with a fresh signed challenge and routes encrypted connection descriptions to online identities. It sees IP addresses, public Relay identities, online status, connection timing and routing relationships. It cannot decrypt SDP or chat text. It stores no messages and writes no conversation records to disk. A hostile or unavailable service can disrupt setup, but cannot substitute another peer's authenticated description.
- STUN services learn your public network address. Peers also learn each other's network addresses. This is encrypted messaging, not anonymity.
- The optional manual setup endpoint is `wss://media.amazingpvper.com/relay-rendezvous`. TLS certificate validation stays enabled. The discovery servers are `stun:stun.l.google.com:19302` and `stun:stun.cloudflare.com:3478`. Neither is a chat relay.
- Each participant can read, copy, retain or disclose their own messages. Authorized AI software can read the conversations granted to it. Compromised endpoints, desktop sessions, local backups and screen captures are outside transport encryption's protection. This implementation has not received an independent cryptographic security audit.

## Local saves

The UI shows the exact profile directory. Normally it is the application's local data directory followed by `relay-peers/`:

- `identity.key`: a 32-byte identity seed and independent 32-byte local encryption key. Owner-only permissions; the enclosing directory is owner-only. Do not share this file. Back it up privately if you want to preserve the identity and decrypt your saved conversations.
- `contacts.relay`: authenticated encrypted contact names, local DM associations, setup-service choice, automatic connection choices, pinned identities and AI grants.
- `<peer-public-key>.relay`: one authenticated encrypted history/outbox file per peer and per Relay profile. Each encrypted document binds its owner, peer and format version so swapped files fail validation.

The save cipher is libsodium XSalsa20-Poly1305 secretbox with a fresh random nonce on every atomic save. Keys are protected by the local operating-system account, not by a separate passphrase or hardware wallet. Someone who can read the whole profile, including `identity.key`, can decrypt it. Encryption prevents plaintext history files; it does not replace OS and backup security.

Messages are saved before transmission. Incoming messages are acknowledged only after a successful atomic local save. Pending means no peer-save acknowledgement yet; saved by peer never means read. Retries reuse an ID and are deduplicated against retained history. Each conversation holds up to 10,000 messages; it refuses additional messages at the limit instead of silently deleting old ones. The UI shows the latest 500; MCP can page all retained records. Clear local history explicitly to reclaim space. Clearing history removes deduplication records too, so a peer retrying an older unacknowledged message can deliver it again. Removing a contact preserves its encrypted history for later re-addition. A damaged history blocks new sends and is preserved for recovery. Missing or damaged identity material is never silently replaced beside existing saves.

## MCP for AI applications

Check **Enable Relay MCP**, then open **Details** and check **Allow AI in this conversation** for each conversation you want to share. Copy the MCP JSON configuration from Details into an AI application that supports local stdio MCP. The command is the installed Relay executable with `--relay-mcp` and that profile's private local endpoint. No browser-accessible HTTP listener is created. Each instance has its own endpoint and same-user access controls.

Tools:

- `relay_list_conversations`: only conversations explicitly granted to AI; no Discord IDs or account tokens.
- `relay_read_messages(peer, after?, limit?)`: chronological pages of 1–100 retained messages, default 50; returns an ID cursor and delivery state.
- `relay_send_text(peer, text)`: saves and queues plain text to a granted peer. The AI application should obtain the local user's authorization to send.

Disable a grant to revoke subsequent reads/sends immediately. Disabling MCP closes its local listener. Previously returned data cannot be recalled from an AI provider or its logs. This is a local API trust boundary: software already running as your OS user has your user's privileges. Peer text is explicitly labeled untrusted third-party content; receiving a message never automatically executes instructions or grants the sender authority. Each person's AI connects to their own local Relay client. No AI credential or model runtime travels to the other person.

The MCP bridge implements the stdio JSON-RPC lifecycle for protocol versions 2024-11-05 through 2025-11-25. It launches without constructing a GUI, Discord client, authentication service or upload service. The API exposes only the three Relay tools, validates argument keys/types and checks the per-peer grant on every call.

## Hosting the setup service

`relay-rendezvous` is a separate small executable. By default it binds loopback port 39872; put it behind a TLS WebSocket reverse proxy. For a private container network, explicitly pass `--listen 0.0.0.0` and expose it only to the reverse proxy, with no published host ports. Clients require a `wss://` URL. The included service uses strict connection, message, authentication deadline, heartbeat and request-rate bounds. Keep proxy body limits and infrastructure monitoring appropriate to deployment size; this is not an anonymity or abuse-prevention network.

The client has no configurable TURN setting. Incoming SDP media and relay candidates are rejected. The official rendezvous client constructs only encrypted, versioned connection descriptions; it never posts chat frames or history to the service.

## Verification

Generated fixtures also exercise two checkboxes through a generated Discord-shaped REST/Gateway service, invitation notifications, a real direct WebRTC text exchange without rendezvous, encrypted setup chunking, opt-out and AI revocation, identity-change blocking, account changes, callback teardown and setup-send failure. The empty-page UI check clicks the actual checkbox and verifies that configuration stays inside the closed Details dialog. Generated fixtures cover encrypted per-peer persistence and profile locks, restart recovery, corruption/missing-key preservation, text validation, two real local WebRTC endpoints, delivery after setup disconnect, deduplication, rejection of non-text frames, MCP grants and paging, a real stdio subprocess to private local IPC, and independent UI views for two different peers. These fixtures do not prove connectivity between every pair of real-world NATs, live friend identity verification, Windows runtime behavior or an external AI application's configuration.


## Simultaneous Discord and Relay streaming

Start a screen/window share or the OBS add-on stream as usual. **Also share directly with Relay contacts** is enabled by default in the sharing dialog for this launch. Discord viewers join the ordinary Discord stream. A connected, accepted Relay contact sees **Watch via Relay** in your Relay conversation (or the **Relay** button beside your voice-channel entry) and can open a separate direct viewer with mute, full-screen and stop controls. Up to eight Relay contacts can watch at once; one stream can be watched per client. Both clients need this version. Existing older clients can still exchange text.

The Relay screen encoder has an independent **Relay Mbps** setting (1–40 Mbps, initially 20), using the selected capture resolution and frame rate. Compatible OBS H.264 Annex B video is forwarded at its original bitrate, resolution and frame rate, independently of Discord's bandwidth allocation or conversion. Other inputs use the separate Relay encoder. Stream audio is encoded as stereo Opus at 256 kbps; microphone-test suppression applies to both paths. Receiving audio follows the chosen output device and deafen state; the viewer has its own mute switch. The video setting is a target; simple pictures can use less bandwidth. Bitrate availability depends on your upload and each viewer's download.

For original OBS video, use a two-second keyframe interval. A new viewer or a viewer recovering from congestion waits for the next source keyframe; Relay cannot remotely command OBS to generate one. Very long OBS keyframe intervals delay joining/recovery. Inputs outside the existing supported codec/size range use converted video. The current direct frame limit is 4 MiB per compressed picture; video beyond that limit cannot be forwarded unchanged.

Direct video, stream controls and Opus use separate encrypted WebRTC data channels. They do not enter saved chat histories or MCP. The existing authenticated setup exchanges only connection descriptions. No media is sent through Discord, the manual rendezvous service or TURN for the Relay path. Discord simultaneously receives its own ordinary broadcast. Stream offers go only to connected, accepted Relay contacts; watching requires their explicit click. Disable direct sharing to withdraw offers and disconnect all Relay viewers immediately, while keeping the Discord broadcast active. Stopping the broadcast ends both paths. Disabling a contact/account closes its direct stream along with its text connection.

Direct peers learn each other's network addresses. Each viewer consumes a separate copy of your upload: a 20 Mbps stream with three Relay viewers needs roughly 60 Mbps plus Discord, audio and network overhead. Slow viewers have bounded send/receive queues and skip video until a fresh keyframe after congestion; the client does not silently reduce all viewers to Discord's bitrate. Some NAT/firewall combinations cannot make a direct connection; this version has no TURN fallback. It uses ordered SCTP delivery, so packet loss can increase latency. Live WAN quality and Windows media behavior require testing on the actual clients.
