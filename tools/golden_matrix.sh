#!/bin/sh
# Run HP's original (x86_64, Rosetta) rasterToHPZJS over every page size and
# option in HP's PPD and record the XQX header values it emits.  This is the
# reference our native filter is checked against.  Needs Rosetta 2.
#
# Usage: tools/golden_matrix.sh HP_EXTRACT_DIR OUT_DIR TEST_PDF
set -eu

HPX=$1 OUT=$2 PDF=$3
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DECODE="$ROOT/build/xqxdecode"
mkdir -p "$OUT"

# Local copy of HP's PPD whose filter / firmware paths point at the extracted bundle
gunzip -c "$HPX/Library/Printers/PPDs/Contents/Resources/HP LaserJet P1005.gz" \
    | sed "s#/Library/Printers/hp/#$HPX/Library/Printers/hp/#g" > "$OUT/hp-local.ppd"

# decode_fields FILE -> one line of key=value pairs from the first page header
decode_fields() {
    off=$(grep -abo ",XQX" "$1" | head -1 | cut -d: -f1)
    tail -c +$((off + 1)) "$1" | "$DECODE" 2>/dev/null | awk '
        /XQX_START_PAGE/ { pages++ }
        pages == 1 && /XQXI_(COPIES|DMDEFAULTSOURCE|DMMEDIATYPE|RESOLUTION_X|RESOLUTION_Y|RASTER_X|RASTER_Y|VIDEO_BPP|VIDEO_X|VIDEO_Y|ECONOMODE|DMPAPER)/ {
            sub(/^[ \t]*XQXI_/, ""); sub(/,/, ""); printf "%s=%s ", $1, $2 }
        END { printf "pages=%d", pages }'
}

run() {   # run NAME cupsfilter-options...
    name=$1; shift
    /usr/sbin/cupsfilter -e -p "$OUT/hp-local.ppd" -m printer/foo "$@" "$PDF" > "$OUT/$name.bin" 2>/dev/null || true
    pjl=$(grep -a -o -E "@PJL SET (DENSITY|ECONOMODE)=[A-Z0-9]+" "$OUT/$name.bin" | tr '\n' ' ')
    printf "%-28s %s | %s\n" "$name" "$(decode_fields "$OUT/$name.bin")" "$pjl"
}

echo "# page sizes"
for ps in $(grep -E "^\*PageSize [^/]+/" "$OUT/hp-local.ppd" | sed -E 's/^\*PageSize ([^/]+)\/.*/\1/'); do
    run "PageSize=$ps" -o "PageSize=$ps"
done
run "PageSize=Custom.300x500" -o PageSize=Custom.300x500

echo "# media types"
for mt in $(grep -E "^\*MediaType [^/]+/" "$OUT/hp-local.ppd" | sed -E 's/^\*MediaType ([^/]+)\/.*/\1/'); do
    run "MediaType=$mt" -o PageSize=A4 -o "MediaType=$mt"
done

echo "# other options"
run "HPResolution=1200x600dpi" -o PageSize=A4 -o HPResolution=1200x600dpi
run "EconoMode=True" -o PageSize=A4 -o EconoMode=True
run "PrintDensity=1" -o PageSize=A4 -o PrintDensity=1
run "PrintDensity=5" -o PageSize=A4 -o PrintDensity=5
run "copies=2" -o PageSize=A4 -o copies=2
