#!/usr/bin/env python3
# Rewrite the table of contents of a flat package (a xar archive) so that
# every entry is owned by root:wheel and carries no extended attributes.
# pkgutil and productbuild copy owner names from the files on disk, which
# would put the login name of whoever built the package into it.
#
#   python3 pkg/scrub-toc.py OUT.pkg
import hashlib
import re
import struct
import sys
import zlib

path = sys.argv[1]
data = open(path, "rb").read()
magic, hsize, version, toc_len, _, alg = struct.unpack(">IHHQQI", data[:28])
if magic != 0x78617221 or alg != 1:
    sys.exit(f"{path}: not a xar archive with a SHA1 table of contents")
toc = zlib.decompress(data[hsize:hsize + toc_len]).decode("utf-8")
heap = bytearray(data[hsize + toc_len:])

toc = re.sub(r"<user>[^<]*</user>", "<user>root</user>", toc)
toc = re.sub(r"<group>[^<]*</group>", "<group>wheel</group>", toc)
toc = re.sub(r"<uid>\d+</uid>", "<uid>0</uid>", toc)
toc = re.sub(r"<gid>\d+</gid>", "<gid>0</gid>", toc)
toc = re.sub(r"\s*<ea id=\"\d+\">.*?</ea>", "", toc, flags=re.S)

raw = toc.encode("utf-8")
packed = zlib.compress(raw, 9)
# The heap holds a SHA1 of the compressed table of contents; refresh it.
m = re.search(r"<checksum style=\"sha1\">\s*<offset>(\d+)</offset>\s*<size>(\d+)</size>", toc)
if not m or int(m.group(2)) != 20:
    sys.exit(f"{path}: table of contents checksum not found")
off = int(m.group(1))
heap[off:off + 20] = hashlib.sha1(packed).digest()

header = struct.pack(">IHHQQI", magic, hsize, version, len(packed), len(raw), alg) + data[28:hsize]
open(path, "wb").write(header + packed + bytes(heap))
