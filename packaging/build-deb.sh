#!/bin/bash
# Build a .deb from the already-built ./5chviewer  (usage: packaging/build-deb.sh [version])
set -e
cd "$(dirname "$0")/.."
VER=${1:-1.4.0}
ARCH=$(dpkg --print-architecture)
STAGE=build/deb/5chviewer_${VER}_${ARCH}
rm -rf "$STAGE"
make install DESTDIR="$STAGE" PREFIX=/usr
gzip -9n "$STAGE/usr/share/doc/5chviewer/README.md" 2>/dev/null || true
{ echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
  echo "Upstream-Name: 5chviewer"
  echo; echo "Files: *"; echo "Copyright: 2026 5ch Viewer authors"; echo "License: MIT"
  sed 's/^$/./; s/^/ /' LICENSE; } > "$STAGE/usr/share/doc/5chviewer/copyright"
chmod 644 "$STAGE/usr/share/doc/5chviewer/copyright"
strip --strip-unneeded "$STAGE/usr/bin/5chviewer"
SIZE=$(du -sk "$STAGE/usr" | cut -f1)
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CTL
Package: 5chviewer
Version: $VER
Section: web
Priority: optional
Architecture: $ARCH
Installed-Size: $SIZE
Maintainer: 5ch Viewer maintainers <maintainer@example.invalid>
Depends: libc6, libstdc++6, libwxgtk3.2-1t64 | libwxgtk3.2-1, libwxbase3.2-1t64 | libwxbase3.2-1, libcurl4t64 | libcurl4
Recommends: fonts-noto-cjk | fonts-ipaexfont | fonts-takao-gothic
Description: Tabbed 5ch.io viewer (wxWidgets)
 Board tree, tabbed thread lists and tabbed thread readers with
 reply helpers, new-post markers, NG filters, auto refresh and posting.
CTL
mkdir -p dist
dpkg-deb --root-owner-group --build "$STAGE" "dist/5chviewer_${VER}_${ARCH}.deb"
echo "=> dist/5chviewer_${VER}_${ARCH}.deb"
