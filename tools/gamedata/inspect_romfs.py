# SPDX-License-Identifier: GPL-3.0-or-later
"""Look inside Fortune's Weave's RomFS data files: find which ones name a character, and decode them to readable text.

    python -I tools/gamedata/inspect_romfs.py list <romfs>                  > romfs_files.txt
    python -I tools/gamedata/inspect_romfs.py find <romfs> Anatolia Troy    (which files contain these words)
    python -I tools/gamedata/inspect_romfs.py dump <file> [-o out.json]     (decode one file)

Written for the unit randomizer (runtime/games/ObeliskChamber/ObeliskChamber_randomizer.cpp; randomize_units.py is its oracle), which
needs to know where the game keeps its characters and its map placements. Nothing here writes to the RomFS.

Formats it understands, after taking off zstd (magic 28 B5 2F FD) when a file has it:
  - MessagePack (the game's graphics settings are MessagePack, ObeliskChamber_detail.cpp): every top-level value, as JSON. Binary
    values become {"$bin": hex}, ext values {"$ext": type, "hex": ...}, and map keys that are not strings are written as text.
  - MSBT (MsgStdBn, the message files under Msg/<language>/): label -> text, with control tags as [group.type:params].
  - SARC (archives such as Layout/*.arc): the names and sizes of the files inside; --extract <dir> writes them out.
  - GameData tables (GameData/*.bin, GameData/Dispos/*.bin, ...): every table's records with the schema's field names, as
    {"$gamedata": {table: [records]}}, or {table: {group: [records]}} for grouped tables such as Dispos. gamedata_bin.py has the format.
  - Anything else: the first bytes as hex, so the format can be recognised.

zstd needs the `zstandard` package (pip install zstandard) or the zstd program on the PATH.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata_bin  # noqa: E402

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"


# ---- zstd ------------------------------------------------------------------------------------------------------------------------------
def unzstd(data):
    try:
        import zstandard
    except ImportError:
        zstandard = None
    if zstandard is not None:
        with zstandard.ZstdDecompressor().stream_reader(data) as r:
            return r.read()
    exe = shutil.which("zstd")
    if exe:
        return subprocess.run([exe, "-d", "-c", "-q"], input=data, stdout=subprocess.PIPE, check=True).stdout
    raise RuntimeError("this file is zstd-compressed: install the zstandard package (pip install zstandard) or the zstd program")


def load(path):
    """The file's bytes, without zstd; and whether it had zstd."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == ZSTD_MAGIC:
        return unzstd(data), True
    return data, False


# ---- MessagePack -----------------------------------------------------------------------------------------------------------------------
class MsgpackError(ValueError):
    pass


class Msgpack:
    """A MessagePack reader with no dependencies. Values come back as plain Python values, ready for JSON."""

    def __init__(self, data):
        self.d = data
        self.p = 0

    def need(self, n):
        if self.p + n > len(self.d):
            raise MsgpackError("truncated at 0x%x" % self.p)

    def take(self, n):
        self.need(n)
        b = self.d[self.p:self.p + n]
        self.p += n
        return b

    def uint(self, w):
        return int.from_bytes(self.take(w), "big")

    def sint(self, w):
        return int.from_bytes(self.take(w), "big", signed=True)

    def all(self):
        out = []
        while self.p < len(self.d):
            out.append(self.value(0))
        return out

    def key(self, v):
        if isinstance(v, str):
            return v
        return json.dumps(v, ensure_ascii=False)

    def text(self, n):
        b = self.take(n)
        try:
            return b.decode("utf-8")
        except UnicodeDecodeError:
            return {"$badutf8": b.hex()}

    def value(self, depth):
        if depth > 64:
            raise MsgpackError("nested too deep at 0x%x" % self.p)
        at = self.p
        t = self.uint(1)
        if t <= 0x7F:
            return t
        if t >= 0xE0:
            return t - 0x100
        if 0x80 <= t <= 0x8F:
            return self.map(t & 0x0F, depth)
        if 0x90 <= t <= 0x9F:
            return self.array(t & 0x0F, depth)
        if 0xA0 <= t <= 0xBF:
            return self.text(t & 0x1F)
        if t == 0xC0:
            return None
        if t == 0xC2:
            return False
        if t == 0xC3:
            return True
        if t in (0xC4, 0xC5, 0xC6):
            return {"$bin": self.take(self.uint({0xC4: 1, 0xC5: 2, 0xC6: 4}[t])).hex()}
        if t in (0xC7, 0xC8, 0xC9):
            n = self.uint({0xC7: 1, 0xC8: 2, 0xC9: 4}[t])
            typ = self.sint(1)
            return {"$ext": typ, "hex": self.take(n).hex()}
        if t == 0xCA:
            return struct.unpack(">f", self.take(4))[0]
        if t == 0xCB:
            return struct.unpack(">d", self.take(8))[0]
        if 0xCC <= t <= 0xCF:
            return self.uint(1 << (t - 0xCC))
        if 0xD0 <= t <= 0xD3:
            return self.sint(1 << (t - 0xD0))
        if 0xD4 <= t <= 0xD8:
            typ = self.sint(1)
            return {"$ext": typ, "hex": self.take(1 << (t - 0xD4)).hex()}
        if t in (0xD9, 0xDA, 0xDB):
            return self.text(self.uint({0xD9: 1, 0xDA: 2, 0xDB: 4}[t]))
        if t in (0xDC, 0xDD):
            return self.array(self.uint(2 if t == 0xDC else 4), depth)
        if t in (0xDE, 0xDF):
            return self.map(self.uint(2 if t == 0xDE else 4), depth)
        raise MsgpackError("unknown type byte 0x%02x at 0x%x" % (t, at))   # 0xC1

    def array(self, n, depth):
        return [self.value(depth + 1) for _ in range(n)]

    def map(self, n, depth):
        out = {}
        for _ in range(n):
            k = self.key(self.value(depth + 1))
            out[k] = self.value(depth + 1)
        return out


def decode_msgpack(data):
    """Every top-level value, or None when the bytes are not MessagePack from start to end. The first value must be a map or an
    array: any run of bytes below 0x80 (plain text, say) also reads as MessagePack, as one small integer per byte."""
    if not data or not (0x80 <= data[0] <= 0x9F or data[0] in (0xDC, 0xDD, 0xDE, 0xDF)):
        return None
    try:
        return Msgpack(data).all()
    except (MsgpackError, struct.error, RecursionError):
        return None


def as_text(data):
    """The file as text when it is UTF-8 (BOM optional) with no control characters but tab and line ends, else None."""
    if data[:3] == b"\xef\xbb\xbf":
        data = data[3:]
    try:
        s = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    if any(ord(c) < 0x20 and c not in "\t\r\n" for c in s):
        return None
    return s


# ---- MSBT ------------------------------------------------------------------------------------------------------------------------------
def decode_msbt(data):
    if data[:8] != b"MsgStdBn":
        return None
    e = "<" if data[8:10] == b"\xff\xfe" else ">"
    enc = data[0xC]
    nsec = struct.unpack_from(e + "H", data, 0xE)[0]
    width = {0: 1, 1: 2, 2: 4}.get(enc, 2)
    codec = {1: "utf-16-le" if e == "<" else "utf-16-be", 2: "utf-32-le" if e == "<" else "utf-32-be"}.get(enc, "utf-8")
    p = 0x20
    sections = {}
    for _ in range(nsec):
        if p + 0x10 > len(data):
            break
        magic = data[p:p + 4].decode("latin-1")
        size = struct.unpack_from(e + "I", data, p + 4)[0]
        sections[magic] = data[p + 0x10:p + 0x10 + size]
        p = (p + 0x10 + size + 0xF) & ~0xF

    labels = {}
    lbl = sections.get("LBL1")
    if lbl:
        slots = struct.unpack_from(e + "I", lbl, 0)[0]
        for i in range(slots):
            count, off = struct.unpack_from(e + "II", lbl, 4 + i * 8)
            for _ in range(count):
                n = lbl[off]
                name = lbl[off + 1:off + 1 + n].decode("utf-8", "replace")
                idx = struct.unpack_from(e + "I", lbl, off + 1 + n)[0]
                labels[idx] = name
                off += 1 + n + 4

    texts = []
    txt = sections.get("TXT2")
    if txt:
        count = struct.unpack_from(e + "I", txt, 0)[0]
        offs = [struct.unpack_from(e + "I", txt, 4 + i * 4)[0] for i in range(count)]
        ends = offs[1:] + [len(txt)]
        for a, b in zip(offs, ends):
            texts.append(msbt_text(txt[a:b], e, width, codec))

    out = {}
    for i, t in enumerate(texts):
        out[labels.get(i, "#%d" % i)] = t
    return {"$msbt": {"encoding": codec, "sections": list(sections)}, "messages": out}


def msbt_text(b, e, width, codec):
    """One message: text, with control tags written as [group.type:hex params] and closing tags as [/group.type]."""
    ch = {1: "B", 2: "H", 4: "I"}[width]
    out = []
    run = bytearray()
    p = 0

    def flush():
        if run:
            out.append(bytes(run).decode(codec, "replace"))
            run.clear()

    while p + width <= len(b):
        c = struct.unpack_from(e + ch, b, p)[0]
        if c == 0:
            break
        if c == 0x0E and p + 4 * width <= len(b):
            g, t, n = (struct.unpack_from(e + ch, b, p + width * k)[0] for k in (1, 2, 3))
            params = b[p + 4 * width:p + 4 * width + n]
            flush()
            out.append("[%d.%d%s]" % (g, t, ":" + params.hex() if params else ""))
            p += 4 * width + n
            continue
        if c == 0x0F and p + 3 * width <= len(b):
            g, t = (struct.unpack_from(e + ch, b, p + width * k)[0] for k in (1, 2))
            flush()
            out.append("[/%d.%d]" % (g, t))
            p += 3 * width
            continue
        run += b[p:p + width]
        p += width
    flush()
    return "".join(out)


# ---- SARC ------------------------------------------------------------------------------------------------------------------------------
def sarc_files(data):
    """[(name, bytes)] or None."""
    if data[:4] != b"SARC":
        return None
    e = "<" if data[6:8] == b"\xff\xfe" else ">"
    data_off = struct.unpack_from(e + "I", data, 0xC)[0]
    hdr = struct.unpack_from(e + "H", data, 4)[0]
    sfat = hdr
    if data[sfat:sfat + 4] != b"SFAT":
        return None
    sfat_hdr, count = struct.unpack_from(e + "HH", data, sfat + 4)
    nodes = sfat + sfat_hdr
    sfnt = nodes + count * 16
    if data[sfnt:sfnt + 4] != b"SFNT":
        return None
    names = sfnt + struct.unpack_from(e + "H", data, sfnt + 4)[0]
    out = []
    for i in range(count):
        _h, attr, a, b = struct.unpack_from(e + "IIII", data, nodes + i * 16)
        if attr & 0x01000000:
            s = names + (attr & 0xFFFF) * 4
            name = data[s:data.index(b"\0", s)].decode("utf-8", "replace")
        else:
            name = "#%d" % i
        out.append((name, data[data_off + a:data_off + b]))
    return out


# ---- commands --------------------------------------------------------------------------------------------------------------------------
def kind(data):
    if data[:8] == b"MsgStdBn":
        return "msbt"
    if data[:4] == b"SARC":
        return "sarc"
    if gamedata_bin.parse(data) is not None:
        return "gamedata"
    if decode_msgpack(data) is not None:
        return "msgpack"
    if data and as_text(data[:4096]) is not None and as_text(data) is not None:
        return "text"
    return "magic:" + data[:4].hex()


def decode(data, extract=None):
    m = decode_msbt(data)
    if m is not None:
        return m
    s = sarc_files(data)
    if s is not None:
        if extract:
            for name, b in s:
                out = os.path.join(extract, *name.split("/"))
                os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
                with open(out, "wb") as f:
                    f.write(b)
        return {"$sarc": [{"name": n, "size": len(b), "kind": kind(unzstd(b) if b[:4] == ZSTD_MAGIC else b)} for n, b in s]}
    g = gamedata_bin.parse(data)
    if g is not None:
        return {"$gamedata": g.dump()}
    v = decode_msgpack(data)
    if v is not None:
        return {"$msgpack": v[0] if len(v) == 1 else v}
    t = as_text(data)
    if t is not None:
        return {"$text": t.splitlines()}
    return {"$unknown": {"size": len(data), "head": data[:256].hex()}}


def walk(root):
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for name in sorted(files):
            p = os.path.join(d, name)
            yield p, os.path.relpath(p, root).replace(os.sep, "/")


def cmd_list(a):
    for p, rel in walk(a.romfs):
        size = os.path.getsize(p)
        if a.no_kind or size > a.max_mb << 20:
            print("%s\t%d" % (rel, size))
            continue
        try:
            data, z = load(p)
            print("%s\t%d\t%s%s" % (rel, size, kind(data), "+zstd" if z else ""))
        except Exception as ex:   # a file we cannot open: listed with the reason
            print("%s\t%d\t?%s" % (rel, size, ex))


def needles(words):
    out = []
    for w in words:
        forms = {"utf-8": w.encode("utf-8"), "utf-16": w.encode("utf-16-le")}
        out.append((w, forms))
    return out


def cmd_find(a):
    ns = needles(a.words)
    hits = 0
    for p, rel in walk(a.romfs):
        if a.ext and not any(rel.lower().endswith(x.lower()) for x in a.ext):
            continue
        if os.path.getsize(p) > a.max_mb << 20:
            continue
        try:
            data, _ = load(p)
        except Exception:
            continue
        found = []
        for w, forms in ns:
            for enc, b in forms.items():
                n = data.count(b)
                if n:
                    found.append("%s x%d (%s)" % (w, n, enc))
        if found:
            hits += 1
            print("%s\t%s" % (rel, ", ".join(found)))
    print("%d files" % hits, file=sys.stderr)


def cmd_dump(a):
    data, _ = load(a.file)
    out = decode(data, a.extract)
    text = json.dumps(out, ensure_ascii=False, indent=1)
    if a.o:
        with open(a.o, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list", help="every file under the RomFS folder: path, size and format")
    p.add_argument("romfs")
    p.add_argument("--max-mb", type=int, default=32, help="files larger than this are listed without their format (default 32)")
    p.add_argument("--no-kind", action="store_true", help="paths and sizes only (fast)")
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("find", help="files that contain any of the words (UTF-8 or UTF-16, after zstd)")
    p.add_argument("romfs")
    p.add_argument("words", nargs="+")
    p.add_argument("--ext", action="append", help="only files ending in this (repeatable), e.g. --ext .zst")
    p.add_argument("--max-mb", type=int, default=32)
    p.set_defaults(fn=cmd_find)
    p = sub.add_parser("dump", help="decode one file to JSON")
    p.add_argument("file")
    p.add_argument("-o", help="write the JSON here instead of the console")
    p.add_argument("--extract", help="for a SARC: write the files inside to this folder")
    p.set_defaults(fn=cmd_dump)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
