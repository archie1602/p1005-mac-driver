#!/bin/sh
# Install the p1005-native driver and point the P1005 print queue at it.
#
#   make                  (as your normal user)
#   sudo ./install.sh [queue-name]        default queue: HP_LaserJet_P1005
#
# Installs (about 0.5 MB):
#   /Library/Printers/p1005-native/rastertoxqx          CUPS filter (arm64 + x86_64)
#   /Library/Printers/p1005-native/P1005-firmware.acl   HP firmware, from firmware/HP1005.acl
#                                                       or downloaded by tools/fetch-firmware.sh
#   /Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005 native.gz
# The queue's previous PPD is saved to ./backup/ so uninstall.sh can restore it.
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
B=$ROOT/build
DEST=/Library/Printers/p1005-native
PPD_DEST="/Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005 native.gz"
QUEUE=${1:-HP_LaserJet_P1005}
BACKUP=$ROOT/backup

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo: sudo $0 $*"
    exit 1
fi
for f in rastertoxqx p1005-native.ppd; do
    [ -f "$B/$f" ] || { echo "Missing build/$f - run 'make' first (as your user, not with sudo)."; exit 1; }
done
lipo "$B/rastertoxqx" -verify_arch arm64 || { echo "build/rastertoxqx has no arm64 code"; exit 1; }
codesign --verify "$B/rastertoxqx" || { echo "build/rastertoxqx signature is invalid"; exit 1; }
grep -q "^\*cupsFilter: \"application/vnd.cups-raster 100 $DEST/rastertoxqx\"" "$B/p1005-native.ppd" \
    || { echo "build/p1005-native.ppd does not point at $DEST"; exit 1; }

echo "[1/3] Installing driver files"
install -d -o root -g wheel -m 755 "$DEST"
install -o root -g wheel -m 755 "$B/rastertoxqx" "$DEST/rastertoxqx"
FW_LOCAL=
[ -f "$ROOT/firmware/HP1005.acl" ] && FW_LOCAL=$ROOT/firmware/HP1005.acl
sh "$ROOT/tools/fetch-firmware.sh" "$DEST/P1005-firmware.acl" ${FW_LOCAL:+"$FW_LOCAL"}
chown root:wheel "$DEST/P1005-firmware.acl"
gzip -9 -n -c "$B/p1005-native.ppd" > "$PPD_DEST"
chown root:wheel "$PPD_DEST"
chmod 644 "$PPD_DEST"
# A folder copied by AirDrop or a browser carries a quarantine flag that
# would stop CUPS from running the filter.
xattr -c "$DEST/rastertoxqx" "$DEST/P1005-firmware.acl" "$PPD_DEST"
du -sh "$DEST" "$PPD_DEST" | sed 's/^/      /'

echo "[2/3] Configuring print queue '$QUEUE'"
if lpstat -p "$QUEUE" >/dev/null 2>&1; then
    CURRENT=/etc/cups/ppd/$QUEUE.ppd
    if [ -f "$CURRENT" ] && ! grep -q "p1005-native" "$CURRENT" && [ ! -f "$BACKUP/$QUEUE.ppd" ]; then
        mkdir -p "$BACKUP"
        cp "$CURRENT" "$BACKUP/$QUEUE.ppd"
        lpoptions -p "$QUEUE" > "$BACKUP/$QUEUE.options" 2>/dev/null || true
        [ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$BACKUP"
        echo "      previous driver saved to backup/$QUEUE.ppd"
    fi
    lpadmin -p "$QUEUE" -P "$B/p1005-native.ppd"
else
    URI=$(lpinfo -v 2>/dev/null | awk '/usb:\/\/Hewlett-Packard\/HP%20LaserJet%20P1005/ { print $2; exit }')
    if [ -z "$URI" ]; then
        echo "      No queue '$QUEUE' and no P1005 on USB. Connect and switch on the printer, then re-run."
        exit 1
    fi
    lpadmin -p "$QUEUE" -E -v "$URI" -P "$B/p1005-native.ppd" -D "HP LaserJet P1005"
    echo "      created queue for $URI"
fi
cupsenable "$QUEUE"
cupsaccept "$QUEUE"

echo "[3/3] Verifying"
grep -q "p1005-native" "/etc/cups/ppd/$QUEUE.ppd" && echo "      queue uses: $(lpstat -l -p "$QUEUE" | awk -F': ' '/Interface/ {print $2}')"
lpstat -p "$QUEUE"
echo
echo "Done. Print a test page with:"
echo "  lp -d $QUEUE \"$B/testpage-a4.pdf\""
