# Validation for 20261104T040840Z-2075353

The Linux native build passed all 78 generated fixture suites, the isolated application demo, source-stability checks and runtime manifest verification on the dedicated builder.

Updater fixtures verify the visible manual check, error reporting and retry, restart confirmation, platform selection, numeric build order, stale cache fallback and interrupted cache recovery. Direct streaming fixtures exercise two real local WebRTC receivers, original H.264 bytes including fragmentation, an approximately 24 Mbps source, concurrent encrypted text, raw capture encoding, subscription withdrawal and stream restarts. OBS checks cover original video alongside conversion. UI checks cover stream controls and the separate viewer.

These results do not verify friend-to-friend connectivity over arbitrary residential networks, live Discord behavior, physical capture/audio hardware, or Windows media behavior. Windows and Flatpak releases carry separate validation records; this Linux release is not a replacement for those packages.

Application sources included here match the verified build for the published subset. The release's signed update metadata authenticates the Linux runtime archive.
