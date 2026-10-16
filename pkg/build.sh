#!/bin/sh
# Build the installer package. Called by `make pkg` and `make pkg-offline`.
#
#   pkg/build.sh OUT.pkg VERSION [HP1005.acl]
#
# Without a firmware file the package contains no HP files at all and its
# preinstall script downloads the firmware from HP's official driver package.
# With one, the firmware travels inside the package (for your own Macs only,
# don't publish that package).
#
# The payload is only /Library/Printers/p1005-native; pkg/postinstall
# registers the PPD with macOS and sets up the print queue.
set -eu

OUT=$1 VERSION=$2 FW=${3:-}
B=build
INSTALL_DIR=/Library/Printers/p1005-native
PKG_ID=org.p1005-native.driver
PPD_GZ_NAME="HP LaserJet P1005 native.gz"
W=$B/pkgwork

rm -rf "$W" && mkdir -p "$W/root" "$W/scripts"
cp "$B/rastertoxqx" pkg/uninstall.sh tools/fetch-firmware.sh "$W/root/"
gzip -9 -n -c "$B/p1005-native.ppd" > "$W/root/$PPD_GZ_NAME"
cp pkg/preinstall pkg/postinstall tools/fetch-firmware.sh "$W/scripts/"
[ -z "$FW" ] || cp "$FW" "$W/scripts/HP1005.acl"
chmod 755 "$W/root" "$W/root/rastertoxqx" "$W/root/uninstall.sh" "$W/root/fetch-firmware.sh" "$W/scripts"/*
chmod 644 "$W/root/$PPD_GZ_NAME"
[ -z "$FW" ] || chmod 644 "$W/scripts/HP1005.acl"
xattr -cr "$W"

pkgbuild --quiet --root "$W/root" --install-location "$INSTALL_DIR" --scripts "$W/scripts" \
    --identifier "$PKG_ID" --version "$VERSION" --ownership recommended "$W/raw.pkg"

# pkgbuild stores the system managed com.apple.provenance attribute as empty
# "._*" AppleDouble files; rebuild Payload and Bom without them and drop
# them from Scripts.
pkgutil --expand "$W/raw.pkg" "$W/expanded"
(cd "$W/root" && tar --format cpio --uid 0 --gid 0 --uname root --gname wheel \
    --no-mac-metadata --no-xattrs -cf - .) | gzip -9 > "$W/expanded/Payload"
find "$W/expanded/Scripts" -name '._*' -delete
lsbom "$W/expanded/Bom" | grep -v '/\._' > "$W/bom.txt"
rm "$W/expanded/Bom" && mkbom -i "$W/bom.txt" "$W/expanded/Bom"
n=$(wc -l < "$W/bom.txt" | tr -d ' ')
sed -i '' "s/numberOfFiles=\"[0-9]*\"/numberOfFiles=\"$n\"/" "$W/expanded/PackageInfo"
pkgutil --flatten "$W/expanded" "$B/p1005-native-component.pkg"

sed "s/@VERSION@/$VERSION/" pkg/distribution.xml > "$W/distribution.xml"
productbuild --quiet --distribution "$W/distribution.xml" --resources pkg/resources \
    --package-path "$B" "$OUT"
python3 -I pkg/scrub-toc.py "$OUT"
rm -rf "$W" "$B/p1005-native-component.pkg"
ls -l "$OUT"
