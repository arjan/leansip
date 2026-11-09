#!/bin/sh
# Builds Qt Core, Network and Widgets as static libraries into $1, with the xcb platform plugin
# (Wayland desktops run it through XWayland) and OpenSSL linked in for HTTPS to ElevenLabs.
# Everything Qt can bundle (zlib, pcre2, harfbuzz, png, ...) is bundled; fontconfig, freetype,
# xcb and xkbcommon come from the system, as every Linux desktop has them.
set -eu
PREFIX=${1:?usage: build-qt.sh <prefix>}
QT_VERSION=6.10.3
QT_SHA256=383dc907816338f0cba72088a524c07458dfc69ce684ca9132fcc4fe91c24b0b

work=$(mktemp -d)
curl -fsSL -o "$work/qtbase.tar.xz" \
    "https://download.qt.io/official_releases/qt/${QT_VERSION%.*}/${QT_VERSION}/submodules/qtbase-everywhere-src-${QT_VERSION}.tar.xz"
echo "${QT_SHA256}  $work/qtbase.tar.xz" | sha256sum -c -
mkdir "$work/src"
tar -xJf "$work/qtbase.tar.xz" -C "$work/src" --strip-components=1

cmake -S "$work/src" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF \
    -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF \
    -DINPUT_opengl=no -DFEATURE_vulkan=OFF -DFEATURE_egl=OFF \
    -DFEATURE_sql=OFF -DFEATURE_printsupport=OFF -DFEATURE_testlib=ON \
    -DFEATURE_dbus=OFF -DFEATURE_glib=OFF -DFEATURE_icu=OFF \
    -DFEATURE_xcb=ON -DFEATURE_xlib=ON -DFEATURE_fontconfig=ON -DFEATURE_system_freetype=ON \
    -DFEATURE_system_zlib=OFF -DFEATURE_system_pcre2=OFF -DFEATURE_system_harfbuzz=OFF \
    -DFEATURE_system_png=OFF -DFEATURE_system_jpeg=OFF -DFEATURE_system_doubleconversion=OFF \
    -DFEATURE_system_libb2=OFF -DFEATURE_libudev=OFF -DFEATURE_libinput=OFF -DFEATURE_mtdev=OFF \
    -DFEATURE_tslib=OFF -DFEATURE_evdev=OFF -DFEATURE_linuxfb=OFF -DFEATURE_vnc=OFF -DFEATURE_eglfs=OFF \
    -DFEATURE_openssl_linked=ON -DOPENSSL_USE_STATIC_LIBS=ON \
    -DFEATURE_zstd=OFF -DFEATURE_brotli=OFF -DFEATURE_gssapi=OFF
cmake --build "$work/build"
cmake --install "$work/build"
rm -rf "$work"
