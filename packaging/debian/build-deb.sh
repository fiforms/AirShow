#!/usr/bin/env bash
# Build the AirShow .deb for Debian 13 (trixie) / Raspberry Pi OS.
#
#   packaging/debian/build-deb.sh [arch] [output-dir]
#
# arch defaults to arm64 (64-bit Raspberry Pi OS); amd64 also works. Runs
# dpkg-buildpackage inside a debian:trixie container, so it works from any
# Linux host with Docker. Building for another architecture needs qemu-user
# binfmt support (e.g. `apt install qemu-user-binfmt`) and is several times
# slower than a native build.
#
# Already on Debian trixie (e.g. on the Pi itself)? Skip the container:
#   sudo apt build-dep ./ && dpkg-buildpackage -us -uc -b
# and the .deb lands in the parent directory.
set -euo pipefail

ARCH=${1:-arm64}
OUT=${2:-dist}
IMAGE=${IMAGE:-debian:trixie}
SRC=$(cd "$(dirname "$0")/../.." && pwd)

if [[ ! -f "$SRC/vendor/uxplay/lib/raop.c" ]]; then
  echo "vendor/uxplay is missing: run 'git submodule update --init'" >&2
  exit 1
fi

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

# Build from a clean copy so host build trees (build/, obj-*/) don't leak in.
docker run --rm --platform "linux/$ARCH" \
  -v "$SRC:/src:ro" -v "$OUT:/out" \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      build-essential devscripts equivs rsync >/dev/null
    mkdir -p /work/airshow
    rsync -a --exclude=/build --exclude="/obj-*" --exclude=/dist --exclude=.git \
      /src/ /work/airshow/
    cd /work/airshow
    mk-build-deps --install --remove \
      --tool "apt-get -y -qq --no-install-recommends" debian/control >/dev/null
    dpkg-buildpackage -us -uc -b
    cp ../airshow_*.deb /out/
    chown "$HOST_UID:$HOST_GID" /out/airshow_*.deb
  '
ls -l "$OUT"/airshow_*.deb
