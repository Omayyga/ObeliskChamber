# SPDX-License-Identifier: GPL-3.0-or-later
"""Read Fortune's Weave's GameData tables (romfs/GameData/*.bin: Person, Dispos/*, Legion, AssetTable, ...).

Every one of these files is the same container, laid out like the FE14 GameData files Ignis reads but with 64-bit pointers, and it
describes itself: each table carries a schema that names its fields.

    0x00  u32 file size        u32 data size        u32 pointer count        u32 label count
    0x10  u32 0                u32 1                u64 0
    0x20  data (data size bytes). Every pointer in it is a u64 offset from 0x20.
          u64 x pointer count: the data offsets that hold a pointer (the game relocates them on load, so a null field is never listed)
          (u32 data offset, u32 name offset) x label count: named objects; names are offsets into the strings that follow
          strings: NUL-terminated UTF-8 (IDs such as PID_カイ, and the schema's Japanese field names)

Objects in the data:
    table descriptor  u32 0x20251201 (a date), u32 depth (1 flat, 2 grouped), ptr name, u32 record size, u32 field count, ptr schema
    object header     u64 0, ptr name, u32 (count << 16 | index), u32 ?, u64 0          (0x20 bytes, every object starts with one)
    table             header right after its descriptor, then ptr descriptor, ptr children (the label is the table's name)
    group             header, ptr children (records); depth-2 tables (Dispos: Player, Enemy, Reinforcement...) list groups
    record            header (name = its key, e.g. PID_カイ), then its fields at the offsets the schema gives
    schema field      ptr name, ptr type, f32 max, f32 min, u32 size, u32 offset              (0x20 bytes)

Field types: u8 s8 b8 u16 s16 b16 u32 s32 b32 f32 color (4 bytes), string (ptr), data (ptr to a key string, then 8 bytes the game
fills in), and arrays: data[] / string[] / data[,] / string[,] (u32 count | element size << 16, u32 0, ptr to the elements).
"""
import struct

HEADER = 0x20
TABLE_VERSION = 0x20251201
SCALARS = {"u8": "B", "s8": "b", "b8": "B", "u16": "H", "s16": "h", "b16": "H", "u32": "I", "s32": "i", "b32": "I", "f32": "f"}


class FormatError(ValueError):
    pass


class Field:
    __slots__ = ("name", "type", "size", "off")

    def __init__(self, name, type_, size, off):
        self.name, self.type, self.size, self.off = name, type_, size, off

    def __repr__(self):
        return "Field(%r, %r, %d, 0x%x)" % (self.name, self.type, self.size, self.off)


class Table:
    """One table: its name, record size, schema and records (data offsets of each record, in file order)."""

    def __init__(self, name, depth, rec_size, fields, groups):
        self.name, self.depth, self.rec_size, self.fields = name, depth, rec_size, fields
        self.groups = groups        # [(group name or None, [record offsets])]

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(name)

    def records(self):
        for _, recs in self.groups:
            yield from recs


class GameDataBin:
    def __init__(self, data):
        if len(data) < HEADER:
            raise FormatError("shorter than the header")
        size, dsz, nptr, nlab = struct.unpack_from("<4I", data, 0)
        rel = HEADER + dsz
        strings = rel + 8 * nptr + 8 * nlab
        if size != len(data) or strings > len(data):
            raise FormatError("header sizes do not match the file")
        self.d = data
        self.data_size = dsz
        self.strings = strings
        self.ptrs = set(struct.unpack_from("<%dQ" % nptr, data, rel))
        self.labels = []
        for i in range(nlab):
            at, name = struct.unpack_from("<II", data, rel + 8 * nptr + 8 * i)
            self.labels.append((at, self.cstr(strings + name)))

    # ---- raw access, by data offset ------------------------------------------------------------------------------------------------------
    def u32(self, off):
        return struct.unpack_from("<I", self.d, HEADER + off)[0]

    def u64(self, off):
        return struct.unpack_from("<Q", self.d, HEADER + off)[0]

    def cstr(self, file_off):
        end = self.d.index(b"\0", file_off)
        return self.d[file_off:end].decode("utf-8", "replace")

    def ptr(self, off):
        """The data offset a pointer field holds, or None when the field is not a pointer (null)."""
        return self.u64(off) if off in self.ptrs else None

    def string(self, off):
        """The string a pointer field points to; None when null; "->0x.." when it points into the data instead."""
        t = self.ptr(off)
        if t is None:
            return None
        if HEADER + t >= self.strings:
            return self.cstr(HEADER + t)
        return "->0x%x" % t

    # ---- tables ------------------------------------------------------------------------------------------------------------------------
    def schema(self, at, count):
        out = []
        for i in range(count):
            f = at + 0x20 * i
            size, off = struct.unpack_from("<II", self.d, HEADER + f + 24)
            out.append(Field(self.string(f), self.string(f + 8), size, off))
        return out

    def tables(self):
        out = []
        for at, name in self.labels:
            desc = at - 0x20
            if desc < 0 or self.ptr(at + 0x20) != desc or self.u32(desc) != TABLE_VERSION:
                continue
            depth = self.u32(desc + 4)
            rec_size, nfields = struct.unpack_from("<II", self.d, HEADER + desc + 16)
            fields = self.schema(self.ptr(desc + 24) or 0, nfields)
            count = self.u32(at + 0x10) >> 16
            first = self.ptr(at + 0x28)
            groups = []
            if first is not None and depth == 1:
                groups.append((None, [first + rec_size * i for i in range(count)]))
            elif first is not None and depth == 2:
                for g in range(count):
                    go = first + 0x30 * g
                    n = self.u32(go + 0x10) >> 16
                    child = self.ptr(go + 0x20)
                    groups.append((self.string(go + 8), [child + rec_size * i for i in range(n)] if child is not None else []))
            out.append(Table(name, depth, rec_size, fields, groups))
        return out

    def table(self, name):
        for t in self.tables():
            if t.name == name:
                return t
        raise KeyError(name)

    # ---- values ------------------------------------------------------------------------------------------------------------------------
    def key(self, rec):
        return self.string(rec + 8)

    def value(self, rec, f):
        off = rec + f.off
        t = f.type
        if t in SCALARS:
            return struct.unpack_from("<" + SCALARS[t], self.d, HEADER + off)[0]
        if t in ("string", "data"):
            return self.string(off)
        if t.endswith("]"):
            word = self.u32(off)
            n, es = word & 0xFFFF, word >> 16
            arr = self.ptr(off + 8)
            if arr is None or not n:
                return []
            items = []
            for i in range(n):
                e = arr + es * i
                items.append(self.string(e) if e in self.ptrs else self.d[HEADER + e:HEADER + e + es].hex())
            return items
        return self.d[HEADER + off:HEADER + off + f.size].hex()

    def record(self, table, rec):
        out = {"$key": self.key(rec)}
        for f in table.fields:
            v = self.value(rec, f)
            if v in (None, 0, [], "") and f.type not in SCALARS:
                continue
            name = f.name
            while name in out:      # Person names several stat blocks the same (HP, 力, ...): keep each
                name += "'"
            out[name] = v
        return out

    def dump(self):
        out = {}
        for t in self.tables():
            if t.depth == 1:
                out[t.name] = [self.record(t, r) for r in t.records()]
            else:
                out[t.name] = {g or "?": [self.record(t, r) for r in recs] for g, recs in t.groups}
        return out


def parse(data):
    """A GameDataBin, or None when the bytes are not one of these containers."""
    try:
        b = GameDataBin(data)
    except (FormatError, struct.error, ValueError):
        return None
    return b
