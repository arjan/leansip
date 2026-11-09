#!/bin/sh
# Builds leansip against the static Qt in $2 with the baresip in $3 embedded in leansipd, runs
# the tests (the loopback call test uses the embedded baresip) and writes
# $4/leansip-<version>-linux-static-<arch>.tar.gz.
set -eu
SRC=${1:?usage: build-leansip.sh <source> <qt prefix> <baresip dir> <output dir>}
QT=${2:?}
BARESIP=${3:?}
OUT=${4:?}
build=$(mktemp -d)

cmake -S "$SRC" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT" \
    -DLEANSIP_EMBED_BARESIP_DIR="$BARESIP" -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build "$build"
# No system baresip in this image: the loopback test must run the embedded one.
XDG_CACHE_HOME="$build/cache" LEANSIP_E2E_EMBEDDED=1 QT_QPA_PLATFORM=offscreen \
    ctest --test-dir "$build" --output-on-failure

version=$(sed -n 's/^project(leansip VERSION \([0-9.]*\).*/\1/p' "$SRC/CMakeLists.txt")
name=leansip-$version-linux-static-$(uname -m)
stage=$build/stage/$name
cmake --install "$build" --prefix "$stage" --strip
mkdir -p "$OUT"
tar -C "$build/stage" -czf "$OUT/$name.tar.gz" "$name"

echo "leansip links:"; ldd "$stage/bin/leansip"
for exe in leansip leansipd leansipctl; do
    QT_QPA_PLATFORM=offscreen "$stage/bin/$exe" --version
done
rm -rf "$build"
