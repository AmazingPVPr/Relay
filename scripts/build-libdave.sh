#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -e "$project_dir/.relay-runtime-only" || -e "$project_dir/../../.relay-runtime-only" || "${RELAY_VAULT_BUILD:-}" != 1 ]]; then
    printf '%s\n' 'Rebuild libdave only inside the managed Relay build container on Vault. Use the Vault development workflow.' >&2
    exit 1
fi
exec "$project_dir/third_party/libdave/build.sh" "${1:-$project_dir/build/deps-work}"
