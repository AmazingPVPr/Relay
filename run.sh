#!/usr/bin/env bash
set -euo pipefail
relay_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
relay_runtime="$relay_root"
if [[ -f "$relay_root/updater/relay-updater.py" ]] && command -v python3 >/dev/null && command -v openssl >/dev/null; then
    relay_updater="$relay_root/updater/relay-updater.py"
    relay_best="$(python3 "$relay_updater" "$relay_root" --select 2>/dev/null)" || relay_best="$relay_root"
    if [[ -f "$relay_best/updater/relay-updater.py" ]]; then
        relay_updater="$relay_best/updater/relay-updater.py"
    fi
    relay_selected="$(python3 "$relay_updater" "$relay_root")" || relay_selected="$relay_root"
    [[ -x "$relay_selected/bin/relay" ]] && relay_runtime="$relay_selected"
fi
if [[ ! -x "$relay_runtime/bin/relay" ]]; then
    printf '%s\n' 'Relay is not installed in this folder. Build and deploy it from Vault; starting Relay never runs a local compiler.' >&2
    exit 1
fi
export RELAY_INSTALL_ROOT="$relay_root"
export LD_LIBRARY_PATH="$relay_runtime/third_party/libdave/lib:$relay_runtime/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$relay_runtime/bin/relay" "$@"
