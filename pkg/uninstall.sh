#!/bin/sh
# Remove the p1005-native driver installed from P1005-Driver.pkg:
#   sudo /Library/Printers/p1005-native/uninstall.sh
# P1005 queues get their previous driver back if the installer saved one,
# otherwise they are removed.

DEST=/Library/Printers/p1005-native
PPD_FILE="/Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005 native.gz"
BACKUP=$DEST/backup
export LANG=C

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo: sudo $0"
    exit 1
fi

for q in $(lpstat -v 2>/dev/null | awk '{ q = $3; sub(/:$/, "", q); print q }'); do
    grep -q "p1005-native" "/etc/cups/ppd/$q.ppd" 2>/dev/null || continue
    if [ -f "$BACKUP/$q.ppd" ]; then
        lpadmin -p "$q" -P "$BACKUP/$q.ppd" && echo "Queue $q restored to its previous driver."
    else
        lpadmin -x "$q" && echo "Queue $q removed."
    fi
done

rm -f "$PPD_FILE"
rm -rf "$DEST"
pkgutil --forget org.p1005-native.driver >/dev/null 2>&1
echo "p1005-native driver removed."
