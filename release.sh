#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds mp3toxwm, linked statically so it runs on any x86-64 Linux, and packs it with README.md,
# LICENSE and THIRD_PARTY.md into release/mp3toxwm-<version>-linux-x64.tar.gz, with a .sha256
# file beside it. The version comes from src/version.h.
set -e
cd "$(dirname "$0")"

LDFLAGS="-static" sh build.sh

VERSION=$(sed -n 's/^#define MP3TOXWM_VERSION "\(.*\)"$/\1/p' src/version.h)
if [ -z "$VERSION" ]; then
    echo "No MP3TOXWM_VERSION in src/version.h." >&2
    exit 1
fi

NAME="mp3toxwm-$VERSION-linux-x64"
rm -rf "release/$NAME" "release/$NAME.tar.gz" "release/$NAME.tar.gz.sha256"
mkdir -p "release/$NAME"
cp build/mp3toxwm README.md LICENSE THIRD_PARTY.md "release/$NAME/"
chmod 755 "release/$NAME" "release/$NAME/mp3toxwm"
chmod 644 "release/$NAME/README.md" "release/$NAME/LICENSE" "release/$NAME/THIRD_PARTY.md"
tar -C release -czf "release/$NAME.tar.gz" --owner=0 --group=0 "$NAME"
(cd release && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
rm -rf "release/$NAME"
echo "Built release/$NAME.tar.gz"
