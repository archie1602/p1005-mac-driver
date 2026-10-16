#!/bin/sh
# Get the P1005 firmware (HP1005.acl) out of HP's official macOS driver
# package. The firmware belongs to HP, so this project does not ship it.
#
#   tools/fetch-firmware.sh OUTPUT [FILE]
#
# FILE can be HP1005.acl itself or HewlettPackardPrinterDrivers.dmg that you
# downloaded already. Without FILE the script first looks for HP's old driver
# on this Mac, and otherwise downloads the package (584 MB) from Apple, or
# from HP if Apple's server fails, into a temporary folder. The package and
# the firmware are both checked against known SHA-256 sums, and the download
# is deleted again at the end.
set -eu

DMG_SHA256=523836b630431bc39b0170a17099099d6f821ef62ff29e6ec64ebb69b9954133
FW_SHA256=6c45cc78aa81cf969d0a5bd819828bdfea95930b03b249365af769de16c4ea3d
DMG_URLS="https://updates.cdn-apple.com/2021/macos/071-46903-20211101-0BD2764A-901C-41BA-9573-C17B8FDC4D90/HewlettPackardPrinterDrivers.dmg
https://ftp.hp.com/pub/softlib/software12/HP_Quick_Start/osx/Applications/ASU/HewlettPackardPrinterDrivers.dmg"
PKG=HewlettPackardPrinterDrivers.pkg
FW_IN_PKG=./Library/Printers/hp/laserjet/hplaserjetzjs.bundle/Contents/Resources/HP1005.acl
HP_INSTALLED=/Library/Printers/hp/laserjet/hplaserjetzjs.bundle/Contents/Resources/HP1005.acl

[ $# -ge 1 ] || { echo "usage: $0 OUTPUT [HP1005.acl | HewlettPackardPrinterDrivers.dmg]" >&2; exit 2; }
OUT=$1 SRC=${2:-}
TMP= MNT=

msg() { echo "fetch-firmware: $*" >&2; }
die() { msg "$*"; exit 1; }
sha() { shasum -a 256 "$1" | awk '{ print $1 }'; }
good_fw() { [ -f "$1" ] && [ "$(sha "$1")" = "$FW_SHA256" ]; }
cleanup() {
    [ -z "$MNT" ] || hdiutil detach -quiet "$MNT" 2>/dev/null || hdiutil detach -quiet -force "$MNT" 2>/dev/null || true
    [ -z "$TMP" ] || rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

save() {
    mkdir -p "$(dirname "$OUT")"
    cp "$1" "$OUT.tmp" && chmod 644 "$OUT.tmp" && mv -f "$OUT.tmp" "$OUT"
    msg "firmware saved to $OUT"
    exit 0
}

if good_fw "$OUT"; then
    msg "$OUT is already in place"
    exit 0
fi

case $SRC in
    "")
        if good_fw "$HP_INSTALLED"; then
            msg "using the firmware from HP's old driver on this Mac"
            save "$HP_INSTALLED"
        fi ;;
    *.dmg) ;;
    *)
        good_fw "$SRC" || die "$SRC is not the expected HP1005.acl (SHA-256 differs)"
        save "$SRC" ;;
esac

TMP=$(mktemp -d /private/tmp/p1005-firmware.XXXXXX)
if [ -z "$SRC" ]; then
    free_kb=$(df -k "$TMP" | awk 'NR == 2 { print $4 }')
    [ "$free_kb" -gt 700000 ] || die "need about 600 MB of free disk space for the download"
    if [ -t 2 ]; then progress=-#; else progress=-sS; fi
    for url in $DMG_URLS; do
        msg "downloading HP's driver package (584 MB) from $(echo "$url" | cut -d/ -f3)"
        if curl -fL $progress --retry 3 --connect-timeout 30 --speed-limit 2000 --speed-time 120 \
                -o "$TMP/hp.dmg" "$url"; then
            [ "$(sha "$TMP/hp.dmg")" = "$DMG_SHA256" ] && { SRC=$TMP/hp.dmg; break; }
            msg "download from $url has the wrong checksum"
        fi
        rm -f "$TMP/hp.dmg"
    done
    [ -n "$SRC" ] || die "could not download HP's driver package; check the internet connection and try again"
else
    [ -f "$SRC" ] || die "$SRC not found"
    msg "checking $SRC"
    [ "$(sha "$SRC")" = "$DMG_SHA256" ] || die "$SRC is not the expected HP driver package (SHA-256 differs)"
fi

mkdir "$TMP/mnt"
hdiutil attach -quiet -nobrowse -readonly -noautoopen -mountpoint "$TMP/mnt" "$SRC" \
    || die "could not open $SRC"
MNT=$TMP/mnt
# Stream just the one file out of the package instead of unpacking 1.5 GB.
# The first tar stops early with a broken pipe once the file is found, so
# only the checksum below decides whether this worked.
tar -xOf "$MNT/$PKG" "$PKG/Payload" 2>/dev/null | tar -xOf - "$FW_IN_PKG" > "$TMP/HP1005.acl" 2>/dev/null || true
good_fw "$TMP/HP1005.acl" || die "firmware inside the package has an unexpected checksum"
save "$TMP/HP1005.acl"
