#!/usr/bin/env bash
set -euo pipefail
[[ "${MSYSTEM:-}" == UCRT64 ]] || { printf 'Run in an MSYS2 UCRT64 shell.\n' >&2; exit 64; }
relay_source="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
relay_build="$relay_source/build/windows"
relay_prefix="$relay_build/deps"
relay_stage="$relay_build/Relay"
relay_jobs="${RELAY_BUILD_JOBS:-4}"
relay_release_id="${RELAY_RELEASE_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
[[ "$relay_jobs" =~ ^[1-6]$ ]] || exit 64
while IFS= read -r relay_package; do
    [[ -z "$relay_package" || "$relay_package" == \#* ]] && continue
    pacman -Q "$relay_package" >/dev/null || { printf 'Missing package: %s\n' "$relay_package" >&2; exit 69; }
done < "$relay_source/packaging/windows/packages.txt"
# RNNoise is built from the pinned upstream release when no MSYS2 package exists.
mkdir -p "$relay_build/sources" "$relay_prefix"
if [[ ! -f "$relay_prefix/lib/pkgconfig/rnnoise.pc" ]]; then
    relay_tar="$relay_build/sources/rnnoise-0.2.tar.gz"
    curl --fail --location --output "$relay_tar" https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
    printf '90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37  %s\n' "$relay_tar" | sha256sum --check -
    tar -xzf "$relay_tar" -C "$relay_build/sources"
    (
      cd "$relay_build/sources/rnnoise-0.2"
      ./configure --host=x86_64-w64-mingw32 --prefix="$(cygpath -m "$relay_prefix")" --disable-static --enable-shared --disable-examples
      make -j"$relay_jobs"
      make install
    )
    mkdir -p "$relay_prefix/share/relay/licenses"
    cp "$relay_build/sources/rnnoise-0.2/COPYING" "$relay_prefix/share/relay/licenses/rnnoise.txt"
fi
export PKG_CONFIG_PATH="$(cygpath -m "$relay_prefix/lib/pkgconfig")${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
python "$relay_source/scripts/build-portable-deps.py" --work "$relay_build/voice" --prefix "$relay_prefix" --jobs "$relay_jobs"
cmake -S "$relay_source" -B "$relay_build/app" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DRELAY_BUILD_RELEASE_ID=$relay_release_id" \
    -DCMAKE_PREFIX_PATH="$(cygpath -m "$relay_prefix")" -DRELAY_AUDIO_PROCESSING=speex \
    -DRELAY_REQUIRE_DAVE=ON -DRELAY_DEPLOY_QT=ON -DRELAY_INSTALL_MEDIA_HELPER=OFF -DBUILD_TESTING=ON \
    -DDAVE_INCLUDE_DIR="$(cygpath -m "$relay_prefix/include")" \
    -DRELAY_DATACHANNEL_SOURCE="$(cygpath -m "$relay_build/voice/libdatachannel")" \
    -DRELAY_MLSPP_TEST_INCLUDE_DIR="$(cygpath -m "$relay_prefix/include/mlspp")"
cmake --build "$relay_build/app" --parallel "$relay_jobs"
export PATH="$relay_prefix/bin:$PATH"
ctest --test-dir "$relay_build/app" --output-on-failure --parallel 2
[[ ! -e "$relay_stage" ]] || mv "$relay_stage" "$relay_build/Relay-previous-$(date -u +%Y%m%dT%H%M%SZ)"
cmake --install "$relay_build/app" --prefix "$(cygpath -m "$relay_stage")"
cmake -S "$relay_source/packaging/windows/launcher" -B "$relay_build/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$relay_build/launcher" --parallel "$relay_jobs"
ctest --test-dir "$relay_build/launcher" --output-on-failure
cmake --install "$relay_build/launcher" --prefix "$(cygpath -m "$relay_stage")"

python "$relay_source/packaging/windows/package.py" "$relay_stage" "$relay_prefix" "$MINGW_PREFIX"
printf 'Windows test package: %s/Relay-windows-x86_64.zip\n' "$relay_build"

if command -v makensis >/dev/null; then
    relay_release_id="$(python -c 'import json,sys; print(json.load(open(sys.argv[1]))["release_id"])' "$relay_stage/build-info.json")"
    makensis -V2 -DSTAGE="$(cygpath -m "$relay_stage")" -DOUTPUT="$(cygpath -m "$relay_build/Relay-Setup.exe")" -DRELEASE_ID="$relay_release_id" "$relay_source/packaging/windows/installer.nsi"
fi
