#!/usr/bin/env bash
set -euo pipefail
relay_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
relay_work="$relay_root/build/flatpak"
relay_arch="$(flatpak --default-arch)"
[[ "$relay_arch" == x86_64 ]] || { printf 'This initial bundle targets x86_64.\n' >&2; exit 64; }
mkdir -p "$relay_work"
dbus-run-session -- flatpak-builder --user --install-deps-from=flathub --force-clean --disable-rofiles-fuse \
    --state-dir="$relay_work/state" --jobs=4 --repo="$relay_work/repo" "$relay_work/app" "$relay_root/packaging/flatpak/org.relay.Relay.json"
flatpak build-bundle --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
    "$relay_work/repo" "$relay_work/Relay-linux-x86_64.flatpak" org.relay.Relay
sha256sum "$relay_work/Relay-linux-x86_64.flatpak" > "$relay_work/Relay-linux-x86_64.flatpak.sha256"
printf 'Linux test bundle: %s/Relay-linux-x86_64.flatpak\n' "$relay_work"
