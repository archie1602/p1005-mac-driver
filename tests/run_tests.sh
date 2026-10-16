#!/bin/sh
# Offline test suite for p1005-native (no printer and no HP files needed).  Run via `make test`.
#
#  1. firmware framing is byte-identical to foo2zjs' arm2hpdl
#  2. firmware decision logic against the fake printer (6 scenarios)
#  3. XQX page headers match HP's own driver for every page size / media type
#     (tests/golden_hp_matrix.txt, produced by tools/golden_matrix.sh)
#  4. JBIG round trip is lossless: decoded pages == input raster, bit for bit
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build
OUT=$ROOT/tests/out/suite
PPD=$B/p1005-native-local.ppd
FILTER=$B/rastertoxqx
GOLDEN=$ROOT/tests/golden_hp_matrix.txt
export P1005_FIRMWARE=$OUT/test-firmware.acl
pass=0 fail=0

ok()   { pass=$((pass + 1)); echo "  PASS  $1"; }
bad()  { fail=$((fail + 1)); echo "  FAIL  $1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

rm -rf "$OUT" && mkdir -p "$OUT" && cd "$OUT" || exit 1
# Stand-in for HP's firmware, same layout (date stamp + ELF header), so the
# suite needs no HP files
{ printf '20080505\177ELF'; head -c 65536 /dev/zero; } > test-firmware.acl
python3 -I "$ROOT/tools/make_testpage.py" a4.pdf
raster() {   # raster OUT.ras cupsfilter-options...
    o=$1; shift
    /usr/sbin/cupsfilter -p "$PPD" -m application/vnd.cups-raster "$@" a4.pdf > "$o" 2>/dev/null
}
# fields FILE -> header values of page 1 (+ page count), as in golden_matrix.sh
fields() {
    off=$(grep -abo ",XQX" "$1" | head -1 | cut -d: -f1)
    [ -n "$off" ] || { echo "no-xqx"; return; }
    tail -c +$((off + 1)) "$1" | "$B/xqxdecode" 2>/dev/null | awk '
        /XQX_START_PAGE/ { pages++ }
        pages == 1 && /XQXI_(COPIES|DMDEFAULTSOURCE|DMMEDIATYPE|RESOLUTION_X|RASTER_X|RASTER_Y|VIDEO_BPP|VIDEO_X|VIDEO_Y|ECONOMODE|DMPAPER)/ {
            sub(/^[ \t]*XQXI_/, ""); sub(/,/, ""); printf "%s=%s ", $1, $2 }
        END { printf "pages=%d", pages }'
}
fake() {     # fake LOG fakeprinter-options... -- runs the filter on $RAS like cupsd would
    log=$1; shift
    DEVICE_URI="usb://Hewlett-Packard/HP%20LaserJet%20P1005?serial=TEST" \
        "$B/fakeprinter" "$@" -o "$log.bin" -- "$FILTER" 7 tester "Test Job" 1 "" "$RAS" 2> "$log.log"
    echo $?
}
summary() { sed -n 's/^fakeprinter: filter exit=\([0-9]*\) .*firmware_uploads=\([0-9]*\) xqx_before_fw=\([A-Za-z]*\).*/exit=\1 uploads=\2 early=\3/p' "$1.log"; }

echo "== 1. firmware framing"
raster a4-1200.ras
RAS=a4-1200.ras
fake fw-needed -w 3000 >/dev/null
"$B/arm2hpdl" "$P1005_FIRMWARE" > arm2hpdl.dl
n=$(wc -c < arm2hpdl.dl | tr -d ' ')
head -c "$n" fw-needed.bin > ours.dl
if cmp -s ours.dl arm2hpdl.dl; then ok "firmware bytes identical to arm2hpdl ($n bytes)"; else bad "firmware bytes differ from arm2hpdl"; fi

echo "== 2. firmware decision logic (fake printer)"
check "printer without firmware -> upload once, then print" "$(summary fw-needed)" "exit=0 uploads=1 early=no"
grep -q "firmware running after" fw-needed.log && ok "filter waited for FWVER before sending the job" || bad "filter did not wait for FWVER"
fake fw-present -f >/dev/null
check "printer with firmware -> no upload" "$(summary fw-present)" "exit=0 uploads=0 early=no"
fake no-sidechannel -n -f >/dev/null
check "no side channel -> no blind upload" "$(summary no-sidechannel)" "exit=0 uploads=0 early=no"
grep -q "WARNING: Cannot read printer status" no-sidechannel.log && ok "warning logged when status unknown" || bad "no warning without side channel"
t0=$(date +%s); fake late-poweron -d 8000 -w 2000 >/dev/null; t1=$(date +%s)
check "printer switched on 8 s after Print -> waits, then uploads" "$(summary late-poweron)" "exit=0 uploads=1 early=no"
[ $((t1 - t0)) -lt 30 ] && ok "late power-on handled in $((t1 - t0)) s" || bad "late power-on took $((t1 - t0)) s"
raster always.ras -o P1005Firmware=Always
RAS=always.ras fake fw-always -f >/dev/null
check "Firmware Upload=Always -> upload even if loaded" "$(summary fw-always)" "exit=0 uploads=1 early=no"
raster never.ras -o P1005Firmware=Never
RAS=never.ras fake fw-never -w 3000 >/dev/null
check "Firmware Upload=Never -> no upload (fake flags early job)" "$(summary fw-never)" "exit=0 uploads=0 early=YES"

echo "== 3. page headers vs HP's driver (FastRes 1200; RESOLUTION_Y differs by design)"
golden() { grep "^$1 " "$GOLDEN" | head -1 | sed -e 's/^[^ ]* *//' -e 's/ | .*//' -e 's/RESOLUTION_Y=[0-9]* //'; }
for name in $(grep -E "^(PageSize|MediaType)=" "$GOLDEN" | awk '{print $1}'); do
    opt=${name}
    case $name in MediaType=*) extra="-o PageSize=A4";; *) extra="";; esac
    /usr/sbin/cupsfilter -e -p "$PPD" -m printer/foo -o "$opt" $extra a4.pdf > hdr.bin 2>/dev/null
    want=$(golden "$name")
    got=$(fields hdr.bin)
    if [ "$got" = "$want" ]; then ok "$name"; else bad "$name"; echo "        want: $want"; echo "        got:  $got"; fi
done
/usr/sbin/cupsfilter -e -p "$PPD" -m printer/foo -o copies=2 a4.pdf > copies.bin 2>/dev/null
check "copies=2 -> 4 pages, COPIES=1 (as HP)" "$(fields copies.bin | grep -o 'COPIES=[0-9]*\|pages=[0-9]*' | tr '\n' ' ')" "COPIES=1 pages=4 "

echo "== 4. lossless JBIG round trip"
raster a4-600.ras -o Resolution=600dpi
for r in a4-1200 a4-600; do
    "$B/ras2png" -p $r.ras $r-src >/dev/null
    RAS=$r.ras fake $r -f >/dev/null
    off=$(grep -abo ",XQX" $r.bin | head -1 | cut -d: -f1)
    tail -c +$((off + 1)) $r.bin | "$B/xqxdecode" -d $r-dec >/dev/null 2>&1
    for p in 1 2; do
        # compare pixel data only: xqxdecode pads its PBM header with spaces
        dims=$(sed -n 2p $r-src-$p.pbm); n=$(( (${dims% *} / 8) * ${dims#* } ))
        tail -c $n $r-src-$p.pbm > src.bits; tail -c $n "$r-dec-0$p-4.pbm" > dec.bits
        if [ "$(sed -n 2p "$r-dec-0$p-4.pbm" | tr -s ' ' | sed 's/^ //;s/ $//')" = "$dims" ] && cmp -s src.bits dec.bits
        then ok "$r page $p decodes bit-identical to the input raster ($dims)"
        else bad "$r page $p differs after decode"; fi
    done
done

# Rasters and bitmaps are ~150 MB; keep only logs unless KEEP=1
[ "${KEEP:-0}" = 1 ] || rm -f "$OUT"/*.ras "$OUT"/*.pbm "$OUT"/*.bits "$OUT"/*.bin

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
