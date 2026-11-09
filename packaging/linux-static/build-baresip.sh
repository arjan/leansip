#!/bin/sh
# Builds a self-contained baresip for embedding in leansipd, into $1:
#   $1/baresip       the executable, every module leansip uses compiled in (STATIC=ON);
#   $1/sounds/*.wav  its stock sounds (ring, ringback, busy, ...).
# re, OpenSSL, opus and libsndfile are linked statically; glibc and ALSA (libasound.so.2, on
# every Linux desktop, and the route to PulseAudio/PipeWire) stay dynamic.
set -eu
OUT=${1:?usage: build-baresip.sh <output dir>}
BARESIP_VERSION=v4.13.0  # re and baresip are released together
SNDFILE_VERSION=1.2.2
SNDFILE_SHA256=3799ca9924d3125038880367bf1468e53a1b7e3686a934f098b7e1d286cdb80e
MODULES="account;alsa;auconv;aufile;auresamp;contact;ctrl_tcp;debug_cmd;dtls_srtp;g711;libg722;g722;ice;menu;opus;sndfile;srtp;stun;turn;uuid"

work=$(mktemp -d)
deps=$work/deps
lib=/usr/lib/$(gcc -dumpmachine)

# libsndfile with WAV only: Debian's has no static library, and recordings are plain WAV.
curl -fsSL -o "$work/sndfile.tar.xz" \
    "https://github.com/libsndfile/libsndfile/releases/download/$SNDFILE_VERSION/libsndfile-$SNDFILE_VERSION.tar.xz"
echo "$SNDFILE_SHA256  $work/sndfile.tar.xz" | sha256sum -c -
mkdir "$work/sndfile" && tar -xJf "$work/sndfile.tar.xz" -C "$work/sndfile" --strip-components=1
cmake -S "$work/sndfile" -B "$work/sndfile-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF \
    -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CPACK=OFF
cmake --build "$work/sndfile-build"
cmake --install "$work/sndfile-build"

git -c advice.detachedHead=false clone -q --depth 1 -b "$BARESIP_VERSION" https://github.com/baresip/re "$work/re"
git -c advice.detachedHead=false clone -q --depth 1 -b "$BARESIP_VERSION" https://github.com/baresip/baresip "$work/baresip"

cmake -S "$work/re" -B "$work/re-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
    -DLIBRE_BUILD_SHARED=OFF -DLIBRE_BUILD_STATIC=ON -DOPENSSL_USE_STATIC_LIBS=ON \
    -DZLIB_LIBRARY="$lib/libz.a"
cmake --build "$work/re-build"
cmake --install "$work/re-build"

cmake -S "$work/baresip" -B "$work/baresip-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$work/install" -DCMAKE_PREFIX_PATH="$deps" -DSTATIC=ON \
    -DMODULES="$MODULES" -DOPENSSL_USE_STATIC_LIBS=ON -DZLIB_LIBRARY="$lib/libz.a" \
    -DOPUS_LIBRARY="$lib/libopus.a" -DSNDFILE_LIBRARIES="$deps/lib/libsndfile.a" \
    -DSNDFILE_INCLUDE_DIR="$deps/include"
cmake --build "$work/baresip-build"
cmake --install "$work/baresip-build"

mkdir -p "$OUT/sounds"
install -m 755 -s "$work/install/bin/baresip" "$OUT/baresip"
cp "$work/install/share/baresip/"*.wav "$OUT/sounds/"
rm -rf "$work"

# Fail here, not on a user's machine, if anything beyond the system libraries crept in.
echo "baresip links:"; ldd "$OUT/baresip"
if ldd "$OUT/baresip" | grep -vE 'linux-vdso|ld-linux|lib(c|m|asound|stdc\+\+|gcc_s|mvec)\.so' | grep -q '=>'; then
    echo "baresip links a non-system library" >&2
    exit 1
fi
