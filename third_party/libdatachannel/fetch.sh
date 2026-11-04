#!/usr/bin/env bash
set -euo pipefail
[[ "${RELAY_VAULT_BUILD:-}" == 1 ]] || { printf 'Fetch Relay dependencies through the isolated Vault executor.\n' >&2; exit 64; }
relay_peer_source="${1:-/workspace/work/deps/libdatachannel-source}"
relay_peer_commit=6b1e2e620f1e37f0eafeee702eaea0043cb305fd
if [[ ! -d "$relay_peer_source/.git" ]]; then
    git clone --branch v0.24.6 --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/paullouisageneau/libdatachannel.git "$relay_peer_source"
fi
[[ "$(git -C "$relay_peer_source" rev-parse HEAD)" == "$relay_peer_commit" ]] || { printf 'Unexpected libdatachannel revision.\n' >&2; exit 65; }
[[ -z "$(git -C "$relay_peer_source" status --porcelain --untracked-files=normal)" ]] || { printf 'Pinned libdatachannel tree is modified.\n' >&2; exit 65; }
if git -C "$relay_peer_source" submodule status --recursive | grep -Eq '^[+-U]'; then
    printf 'Incomplete or modified libdatachannel submodule revisions.\n' >&2; exit 65
fi
git -C "$relay_peer_source" submodule foreach --quiet --recursive \
    'test -z "$(git status --porcelain --untracked-files=normal)"' || { printf 'Modified libdatachannel submodule content.\n' >&2; exit 65; }
printf 'Verified pinned libdatachannel 0.24.6 and submodules.\n'
