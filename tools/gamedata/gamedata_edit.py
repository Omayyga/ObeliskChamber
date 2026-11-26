# SPDX-License-Identifier: GPL-3.0-or-later
"""Grow a GameData container: insert bytes into its data, add pointers, labels and strings, and write it back.

gamedata_bin.py describes the layout. Every pointer is a u64 offset from 0x20 that the pointer list names, labels are (data offset,
name offset into the strings) sorted by data offset, and the strings come last. So growing the file is mechanical:

    insert(at, raw)      raw bytes go in at data offset `at`; every pointer-list entry, pointer value and label at or after `at`
                         moves by len(raw) (a pointer to `at` itself moves too: it pointed at what now follows the new bytes)
    set_ptr(at, target)  the u64 at data offset `at` becomes a pointer: to a data offset (int), or to a string (str); the string is
                         reused when the pool already has it, else appended to the pool
    copy_ptr(at, from)   the u64 at `at` points where the pointer at `from` does (copy_ptrs: several, all read before any is written)
    add_label(at, name)  a label for the object at data offset `at`
    build()              the file: header, data, the pointer list (the file's order, new pointers last), the labels in offset
                         order, the strings

Pointers into the strings move with the pool, which starts later once the data, the pointer list or the labels grow. Nothing else in
these files holds an offset, so nothing else needs fixing. The runtime does the same in ObeliskChamber_randomizer.cpp (Grow), step for
step: both must give the same bytes.
"""
import struct

HEADER = 0x20


class Editor:
    def __init__(self, data):
        size, dsz, nptr, nlab = struct.unpack_from("<4I", data, 0)
        rel = HEADER + dsz
        self.head = bytes(data[:HEADER])
        self.data = bytearray(data[HEADER:rel])
        self.old_strings = rel + 8 * nptr + 8 * nlab            # file offset of the pool, before any change
        self.pool = bytes(data[self.old_strings:])
        self.old_dsz = dsz
        self.ptrs = list(struct.unpack_from("<%dQ" % nptr, data, rel))     # in file order: the files do not sort it
        self.labels = [struct.unpack_from("<II", data, rel + 8 * nptr + 8 * i) for i in range(nlab)]
        # pointer values, by pointer offset, in pointer-list order (new pointers go last): ("data", offset), ("pool", offset in the
        # pool) or, for a string added to the pool, ("new", text)
        self.vals = {}
        for p in self.ptrs:
            v = struct.unpack_from("<Q", self.data, p)[0]
            self.vals[p] = ("data", v) if v < dsz else ("pool", v + HEADER - self.old_strings)
        self.label_names = {}           # new labels: data offset -> text
        self.new_strings = []           # appended to the pool, in order
        self.index = {}                 # text -> offset in the pool of its first occurrence
        i = 0
        while i < len(self.pool):
            end = self.pool.index(b"\0", i)
            self.index.setdefault(self.pool[i:end].decode("utf-8", "surrogateescape"), i)
            i = end + 1

    # ---- edits ----------------------------------------------------------------------------------------------------------------------
    def insert(self, at, raw):
        n = len(raw)
        self.data[at:at] = raw

        def mv(o):
            return o + n if o >= at else o
        self.vals = {mv(p): (("data", mv(v[1])) if v[0] == "data" else v) for p, v in self.vals.items()}
        self.labels = [(mv(o), name) for o, name in self.labels]
        self.label_names = {mv(o): t for o, t in self.label_names.items()}

    def set_ptr(self, at, target):
        self.vals[at] = ("data", target) if isinstance(target, int) else self.string_ref(target)

    def copy_ptr(self, at, from_at):
        """The u64 at `at` becomes a pointer to what the pointer at `from_at` points to."""
        self.vals[at] = self.vals[from_at]

    def copy_ptrs(self, pairs):
        """copy_ptr for each (at, from_at), every source read before any is written."""
        vals = [self.vals[f] for _, f in pairs]
        for (at, _), v in zip(pairs, vals):
            self.vals[at] = v

    def string_ref(self, text):
        """The first string of the pool that is exactly `text`, or a new one appended to the pool."""
        if text in self.index:
            return ("pool", self.index[text])
        if text not in self.new_strings:
            self.new_strings.append(text)
        return ("new", text)

    def add_label(self, at, text):
        self.string_ref(text)
        self.label_names[at] = text

    def u32(self, at, value=None):
        if value is not None:
            struct.pack_into("<I", self.data, at, value)
        return struct.unpack_from("<I", self.data, at)[0]

    def u64(self, at):
        return struct.unpack_from("<Q", self.data, at)[0]

    # ---- the file -------------------------------------------------------------------------------------------------------------------
    def build(self):
        pool = bytearray(self.pool)
        new_at = {}
        for text in self.new_strings:
            new_at[text] = len(pool)
            pool += text.encode("utf-8") + b"\0"
        labels = list(self.labels)
        for o, text in self.label_names.items():
            kind, v = self.string_ref(text)
            labels.append((o, v if kind == "pool" else new_at[v]))
        labels.sort(key=lambda x: x[0])
        ptrs = list(self.vals)
        dsz = len(self.data)
        strings = HEADER + dsz + 8 * len(ptrs) + 8 * len(labels)
        data = bytearray(self.data)
        for p in ptrs:
            kind, v = self.vals[p]
            if kind == "data":
                value = v
            elif kind == "pool":
                value = strings - HEADER + v
            else:
                value = strings - HEADER + new_at[v]
            struct.pack_into("<Q", data, p, value)
        head = bytearray(self.head)
        struct.pack_into("<4I", head, 0, strings + len(pool), dsz, len(ptrs), len(labels))
        out = bytes(head) + bytes(data) + b"".join(struct.pack("<Q", p) for p in ptrs)
        out += b"".join(struct.pack("<II", o, n) for o, n in labels) + bytes(pool)
        return out
