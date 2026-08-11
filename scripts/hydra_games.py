#!/usr/bin/env python3
"""Fallback Hydra game importer for epinephrine.

Reads games from Hydra Launcher's LevelDB tables (~/.config/hydralauncher/hydra-db/*.ldb).
Self-contained: embeds a pure-Python snappy decompressor (no external deps).

Usage: hydra_games.py [hydra-db-dir] [all|classics]
Prints tab-separated lines: title<TAB>objectId<TAB>shop<TAB>execPath<TAB>iconUrl
  all      = every non-deleted game (default)
  classics = only emulator ROMs (shop == "launchbox")
Deleted games (isDeleted=true) are always skipped.
"""

import json
import os
import struct
import sys

MAGIC = 0xDB4775248B80FB57


def varint(buf, pos):
    result = 0
    shift = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7


def snappy_decompress(data):
    out_len, pos = varint(data, 0)
    out = bytearray()
    n = len(data)
    while pos < n:
        tag = data[pos]
        pos += 1
        typ = tag & 0x03
        if typ == 0:
            l = (tag >> 2) & 0x3F
            if l >= 60:
                extra = l - 59
                l = int.from_bytes(data[pos:pos + extra], "little")
                pos += extra
            l += 1
            if pos + l > n:
                raise ValueError("literal oob")
            out += data[pos:pos + l]
            pos += l
        elif typ == 1:
            l = ((tag >> 2) & 0x07) + 4
            if pos >= n:
                raise ValueError("copy1 oob")
            off = ((tag >> 5) << 8) | data[pos]
            pos += 1
            for _ in range(l):
                out.append(out[-off])
        elif typ == 2:
            l = (tag >> 2) + 1
            if pos + 2 > n:
                raise ValueError("copy2 oob")
            off = data[pos] | (data[pos + 1] << 8)
            pos += 2
            for _ in range(l):
                out.append(out[-off])
        else:
            l = (tag >> 2) + 1
            if pos + 4 > n:
                raise ValueError("copy4 oob")
            off = struct.unpack("<I", data[pos:pos + 4])[0]
            pos += 4
            for _ in range(l):
                out.append(out[-off])
    if len(out) != out_len:
        raise ValueError(f"len mismatch {len(out)} != {out_len}")
    return bytes(out)


def decompress_block(data, off, size):
    typ = data[off + size]
    blk = data[off:off + size]
    if typ == 0x00:
        return blk
    if typ == 0x01:
        return snappy_decompress(blk)
    raise ValueError(f"block type 0x{typ:02x}")


def parse_block(data, off, size):
    dec = decompress_block(data, off, size)
    n = len(dec)
    cnt = int.from_bytes(dec[n - 4:n], "little")
    ra = n - 4 - cnt * 4
    pos = 0
    prev = b""
    out = []
    while pos < ra:
        sh, p2 = varint(dec, pos)
        ns, p3 = varint(dec, p2)
        vl, p4 = varint(dec, p3)
        key = prev[:sh] + dec[p4:p4 + ns]
        val = dec[p4 + ns:p4 + ns + vl]
        out.append((key, val))
        prev = key
        pos = p4 + ns + vl
    return out


def json_field(val, field):
    pat = ('"%s"' % field).encode()
    i = val.find(pat)
    if i < 0:
        return None
    j = i + len(pat)
    while j < len(val) and val[j] in b" :":
        j += 1
    if j >= len(val) or val[j] != ord('"'):
        return None
    j += 1
    end = j
    while end < len(val) and val[end] != ord('"'):
        if val[end] == ord('\\'):
            end += 1
        end += 1
    return val[j:end].decode("utf-8", "replace")


def parse_ldb(path):
    data = open(path, "rb").read()
    n = len(data)
    if n < 56 or int.from_bytes(data[n - 8:], "little") != MAGIC:
        return []
    f = n - 8 - 40
    p = f
    _, p = varint(data, p)
    _, p = varint(data, p)
    idx_off, p = varint(data, p)
    idx_size, p = varint(data, p)
    try:
        idx = parse_block(data, idx_off, idx_size)
    except Exception:
        return []
    blocks = []
    for _k, v in idx:
        off, p = varint(v, 0)
        size, _ = varint(v, p)
        if size:
            blocks.append((off, size))
    games = []
    for off, size in blocks:
        try:
            entries = parse_block(data, off, size)
        except Exception:
            continue
        for k, v in entries:
            uk = k[:-8] if len(k) > 8 and k[-5:] == b"\x00" * 5 else k
            if not uk.startswith(b"!games!"):
                continue
            try:
                g = json.loads(v)
            except Exception:
                continue
            if g.get("isDeleted"):
                continue
            title = g.get("title") or ""
            oid = str(g.get("objectId") or "")
            if not title or not oid:
                continue
            games.append({
                "title": title,
                "objectId": oid,
                "shop": g.get("shop") or "",
                "execPath": g.get("selectedDiscPath") or g.get("runExePath") or "",
                "iconUrl": g.get("iconUrl") or "",
            })
    return games


def main():
    db_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
        "~/.config/hydralauncher/hydra-db")
    classics = len(sys.argv) > 2 and sys.argv[2] == "classics"
    ldb_files = sorted((f for f in os.listdir(db_dir) if f.endswith(".ldb")),
                       reverse=True)  # newest first
    seen = set()
    games = []
    for f in ldb_files:
        try:
            gs = parse_ldb(os.path.join(db_dir, f))
        except Exception:
            continue
        for g in gs:
            if classics != (g["shop"] == "launchbox"):
                continue
            key = (g["shop"], g["objectId"])
            if key in seen:
                continue
            seen.add(key)
            games.append(g)
    for g in games:
        fields = [g["title"], g["objectId"], g["shop"], g["execPath"], g["iconUrl"]]
        fields = [f.replace("\t", " ").replace("\n", " ") for f in fields]
        print("\t".join(fields))


if __name__ == "__main__":
    main()
