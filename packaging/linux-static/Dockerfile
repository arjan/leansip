# Static Linux build of leansip: Qt, OpenSSL, opus, libsndfile, re and baresip are linked in;
# only libraries every Linux desktop has stay dynamic (glibc, ALSA, X11/xcb, fontconfig).
# baresip is built with all its modules inside and embedded in leansipd, which extracts it on
# first use. Built on Debian 12, so the result runs on glibc 2.36 or newer.
#
#   docker build -f packaging/linux-static/Dockerfile -o dist .
#
# leaves dist/leansip-<version>-linux-static-<arch>.tar.gz.
FROM debian:12 AS base
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl xz-utils git file build-essential cmake ninja-build pkg-config python3 jq \
      libssl-dev libopus-dev libasound2-dev libtomlplusplus-dev \
      libfontconfig1-dev libfreetype-dev libx11-dev libx11-xcb-dev libxkbcommon-dev libxkbcommon-x11-dev \
      libxcb1-dev libxcb-cursor-dev libxcb-icccm4-dev libxcb-image0-dev libxcb-keysyms1-dev \
      libxcb-randr0-dev libxcb-render-util0-dev libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev \
      libxcb-xfixes0-dev libxcb-xkb-dev libxcb-xinput-dev libxcb-util-dev libxcb-render0-dev \
    && rm -rf /var/lib/apt/lists/*

FROM base AS qt
COPY packaging/linux-static/build-qt.sh /build/
RUN /build/build-qt.sh /opt/qt

FROM base AS baresip
COPY packaging/linux-static/build-baresip.sh /build/
RUN /build/build-baresip.sh /opt/baresip

FROM qt AS leansip
COPY --from=baresip /opt/baresip /opt/baresip
COPY . /src
RUN /src/packaging/linux-static/build-leansip.sh /src /opt/qt /opt/baresip /out

FROM scratch AS dist
COPY --from=leansip /out/ /
