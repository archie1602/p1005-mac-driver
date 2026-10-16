#!/bin/sh
# Remove the p1005-native driver and restore the queue's previous driver.
#   sudo ./uninstall.sh [queue-name]        default queue: HP_LaserJet_P1005
set -eu

ROOT=$(cd "$(dirname "$0")" && pwd)
DEST=/Library/Printers/p1005-native
PPD_DEST="/Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005 native.gz"
QUEUE=${1:-HP_LaserJet_P1005}
BACKUP=$ROOT/backup

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run with sudo: sudo $0 $*"
    exit 1
fi

if lpstat -p "$QUEUE" >/dev/null 2>&1 && grep -q "p1005-native" "/etc/cups/ppd/$QUEUE.ppd" 2>/dev/null; then
    if [ -f "$BACKUP/$QUEUE.ppd" ]; then
        lpadmin -p "$QUEUE" -P "$BACKUP/$QUEUE.ppd"
        echo "Queue '$QUEUE' restored to its previous driver (backup/$QUEUE.ppd)."
    else
        lpadmin -x "$QUEUE"
        echo "No backup found; queue '$QUEUE' removed."
    fi
fi

rm -rf "$DEST"
rm -f "$PPD_DEST"
echo "Removed $DEST and the p1005-native PPD."
