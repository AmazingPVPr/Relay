#!/usr/bin/env bash
set -euo pipefail
# Rebuild the pinned official DAVE library inside the managed Vault container only.
base="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
project_dir="$(cd -- "$base/../.." && pwd)"
if [[ -e "$project_dir/.relay-runtime-only" || -e "$project_dir/../../.relay-runtime-only" || "${RELAY_VAULT_BUILD:-}" != 1 ]]; then
    printf '%s\n' 'Rebuild libdave only inside the managed Relay build container on Vault. Use the Vault development workflow.' >&2
    exit 1
fi
build_root="${1:-$base/build-work}"
mkdir -p "$build_root"
build_root="$(cd "$build_root" && pwd)"
fetch() {
    local url="$1" revision="$2" destination="$3"
    if [[ ! -d "$destination/.git" ]]; then git init "$destination"; git -C "$destination" remote add origin "$url"; fi
    git -C "$destination" fetch --depth 1 origin "$revision"
    git -C "$destination" checkout --detach FETCH_HEAD
}
fetch https://github.com/discord/libdave.git 8de72b1f8a2ac3c5a5270755bb8091a62e3c6169 "$build_root/libdave"
fetch https://github.com/cisco/mlspp.git 1cc50a124a3bc4e143a787ec934280dc70c1034d "$build_root/mlspp"
fetch https://github.com/nlohmann/json.git 55f93686c01528224f448c19128836e7df245f72 "$build_root/json"
prefix="$build_root/deps"
cmake -S "$build_root/json" -B "$build_root/json-build" -DJSON_BuildTests=OFF -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --install "$build_root/json-build"
cmake -S "$build_root/mlspp" -B "$build_root/mlspp-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DMLS_CXX_NAMESPACE=mlspp -DDISABLE_GREASE=ON -DTESTING=OFF -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$build_root/mlspp-build" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --install "$build_root/mlspp-build"
cmake -S "$build_root/libdave/cpp" -B "$build_root/dave-build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=ON -DTESTING=OFF -DPERSISTENT_KEYS=OFF
cmake --build "$build_root/dave-build" -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
cmake --install "$build_root/dave-build"
mkdir -p "$base/include" "$base/lib" "$base/licenses"
cp -r "$prefix/include/dave" "$base/include/"
cp "$prefix/lib/libdave.so" "$base/lib/"
cp "$build_root/libdave/LICENSE" "$base/licenses/libdave.txt"
cp "$build_root/mlspp/LICENSE" "$base/licenses/mlspp.txt"
cp "$build_root/json/LICENSE.MIT" "$base/licenses/nlohmann-json.txt"
