# Relay microphone key for OpenDeck

This Linux OpenDeck action controls only Relay through its private local control socket. It does not change the operating system microphone, start Relay, join voice, or affect other microphone actions.

The icon refreshes every 500 ms while the key is visible. Red means **MUTED**, amber means **DEAFENED** with the mic unmuted, gray **READY** means Relay is open but outside voice, green **MIC ON** means connected to voice and unmuted, and gray **OFFLINE** means Relay is closed or its controls could not be read. Muted takes priority over the other voice states.

One physical key-down sends one `toggle-mute` request. Holding the key does not repeat it. The returned authoritative state is shown immediately, followed by a read-only refresh. A timed-out toggle is never retried automatically because it may already have taken effect.

The Node entry point uses only built-in modules and the OpenDeck WebSocket protocol. It checks that Relay's Unix socket and its private directory belong to the current user and refuses symbolic links or a directory accessible by other users. Requests use the existing version 1 newline-delimited JSON protocol and have a bounded response size and deadline. Installation and placement in an OpenDeck profile are separate from this bundle.
