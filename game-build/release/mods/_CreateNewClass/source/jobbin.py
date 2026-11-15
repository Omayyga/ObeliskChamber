"""Job.bin of Fire Emblem: Fortune's Weave: check the container and append class rows to sheet 1.

Container (little endian): u32 file size, u32 data size, u32 pointer count, u32 label count, 16 bytes; data; pointer table (u64 data
offsets of the 8-byte slots that hold a pointer; the slots pointing into data first, then those pointing at strings, each group
ascending); labels (u32 data offset, u32 name offset in the string area); strings. A pointer slot holds a file offset minus 0x20.
Sheet 1 (classes): 134 column descriptors of 0x20 bytes at data 0, rows of 0x230 bytes from 0x10c0, their list arrays, then the sheet
header (label 兵種): +0x10 u32 = row count << 16, +0x28 pointer to the rows. A row: +8 key string, +0x10 u16 index, u16 1, u16 0,
u16 line number (unique in the sheet).

usage: jobbin.py check FILE | jobbin.py add IN OUT COUNT [KEYPREFIX]
"""
import struct, sys

ROW = 0x230

class Bin:
    def __init__(self, raw):
        self.size, self.dsz, np, nl = struct.unpack_from('<4I', raw, 0)
        assert self.size == len(raw), 'file size field'
        self.head = raw[0x10:0x20]
        self.data = bytearray(raw[0x20:0x20 + self.dsz])
        pt = 0x20 + self.dsz
        self.ptrs = list(struct.unpack_from('<%dQ' % np, raw, pt))
        lt = pt + np * 8
        self.labels = [list(struct.unpack_from('<II', raw, lt + i * 8)) for i in range(nl)]
        self.st = lt + nl * 8                      # file offset of the string area
        self.strings = bytearray(raw[self.st:])
    def q(self, o): return struct.unpack_from('<Q', self.data, o)[0]
    def setq(self, o, v): struct.pack_into('<Q', self.data, o, v)
    def text(self, v):                             # the string a slot value points at
        o = v + 0x20 - self.st
        return self.strings[o:self.strings.index(b'\0', o)].decode('utf-8')
    def label(self, i): 
        o = self.labels[i][1]
        return self.strings[o:self.strings.index(b'\0', o)].decode('utf-8')
    def sheet1(self):
        hdr = next(a for i, (a, b) in enumerate(self.labels) if self.label(i) == '兵種')
        count = struct.unpack_from('<I', self.data, hdr + 0x10)[0] >> 16
        return hdr, self.q(hdr + 0x28), count
    def build(self):
        np, nl = len(self.ptrs), len(self.labels)
        out = bytearray(struct.pack('<4I', 0, len(self.data), np, nl)) + self.head + self.data
        out += struct.pack('<%dQ' % np, *self.ptrs)
        for a, b in self.labels: out += struct.pack('<II', a, b)
        out += self.strings
        struct.pack_into('<I', out, 0, len(out))
        return bytes(out)

def check(b, say=print):
    bad = []
    dsz = len(b.data); lo = b.st - 0x20; hi = lo + len(b.strings)
    if len(set(b.ptrs)) != len(b.ptrs): bad.append('duplicate pointer slots')
    for p in b.ptrs:
        if p % 8 or p + 8 > dsz: bad.append('slot %x outside the data' % p); continue
        v = b.q(p)
        if v < dsz: continue
        if not (lo <= v < hi): bad.append('slot %x -> %x is neither data nor a string' % (p, v)); continue
        o = v - lo
        if o and b.strings[o - 1] != 0: bad.append('slot %x -> %x is inside a string' % (p, v))
    for i, (a, s) in enumerate(b.labels):
        if a >= dsz or s >= len(b.strings): bad.append('label %d out of range' % i)
    hdr, rows, count = b.sheet1()
    ids = set()
    for i in range(count):
        r = rows + i * ROW
        idx, one, zero, line = struct.unpack_from('<4H', b.data, r + 0x10)
        if idx != i or one != 1 or zero != 0: bad.append('row %d index field %d/%d/%d' % (i, idx, one, zero))
        if line in ids: bad.append('row %d line number %d used twice' % (i, line))
        ids.add(line)
        key = b.text(b.q(r + 8))
        if not key.startswith('JID_'): bad.append('row %d key %r' % (i, key))
        if r not in [a for a, _ in b.labels]: bad.append('row %d has no label' % i)
        elif b.label([a for a, _ in b.labels].index(r)) != key: bad.append('row %d label differs from its key' % i)
    say('data %#x, %d pointers, %d labels, strings %#x; sheet 1: %d rows at %#x, header %#x, lines up to %d, last key %s' %
        (dsz, len(b.ptrs), len(b.labels), len(b.strings), count, rows, hdr, max(ids), b.text(b.q(rows + (count - 1) * ROW + 8))))
    for m in bad[:20]: say('  PROBLEM: ' + m)
    return not bad

def add_rows(b, n, prefix='JID_Custom', template=0):
    """Appends n rows to sheet 1: copies of row `template` with keys <prefix>01.. and new line numbers."""
    hdr, rows, count = b.sheet1()
    at = rows + count * ROW                     # where the new rows go: straight after the last row
    delta = n * ROW
    old_dsz = len(b.data)
    lo = b.st - 0x20                            # slot value of the first string, before the change
    in_data = [p for p in b.ptrs if b.q(p) < old_dsz]
    in_str = [p for p in b.ptrs if b.q(p) >= old_dsz]
    tpl = rows + template * ROW
    tpl_bytes = bytes(b.data[tpl:tpl + ROW])
    tpl_slots = [(p - tpl, b.q(p) < old_dsz) for p in b.ptrs if tpl <= p < tpl + ROW]
    lines = max(struct.unpack_from('<H', b.data, rows + i * ROW + 0x16)[0] for i in range(count))
    # 1. the data grows at `at`: every slot and every pointer into the data at or after it moves
    b.data[at:at] = bytes(delta)
    mv = lambda o: o + delta if o >= at else o
    in_data = [mv(p) for p in in_data]; in_str = [mv(p) for p in in_str]
    for p in in_data: b.setq(p, mv(b.q(p)))
    b.labels = [[mv(a), s] for a, s in b.labels]
    hdr = mv(hdr)
    # 2. the new rows
    keys = []
    for k in range(n):
        r = at + k * ROW
        b.data[r:r + ROW] = tpl_bytes
        struct.pack_into('<4H', b.data, r + 0x10, count + k, 1, 0, lines + 1 + k)
        for off, to_data in tpl_slots:
            if to_data: in_data.append(r + off); b.setq(r + off, mv(b.q(r + off)))
            else: in_str.append(r + off)
        key = '%s%02d' % (prefix, k + 1)
        keys.append((r, len(b.strings), key))
        b.strings += key.encode('utf-8') + b'\0'
    struct.pack_into('<I', b.data, hdr + 0x10, (count + n) << 16 | (struct.unpack_from('<I', b.data, hdr + 0x10)[0] & 0xffff))
    # 3. labels for the new rows, after the last old row's
    pos = max(i for i, (a, _) in enumerate(b.labels) if a < at) + 1
    b.labels[pos:pos] = [[r, s] for r, s, _ in keys]
    # 4. the tables grew, so the string area moved: every string pointer follows it
    b.ptrs = sorted(in_data) + sorted(in_str)
    new_st = 0x20 + len(b.data) + len(b.ptrs) * 8 + len(b.labels) * 8
    shift = new_st - b.st
    for p in in_str:
        b.setq(p, b.q(p) + shift)
    b.st = new_st
    for r, s, _ in keys: b.setq(r + 8, s + new_st - 0x20)
    return [k for _, _, k in keys]

if __name__ == '__main__':
    cmd = sys.argv[1]
    b = Bin(open(sys.argv[2], 'rb').read())
    if cmd == 'check':
        same = b.build() == open(sys.argv[2], 'rb').read()
        ok = check(b); print('rebuilds to the same bytes:', same); sys.exit(0 if ok and same else 1)
    if cmd == 'add':
        keys = add_rows(b, int(sys.argv[4]), *(sys.argv[5:6]))
        out = b.build(); open(sys.argv[3], 'wb').write(out)
        ok = check(Bin(out)); print('added', keys); sys.exit(0 if ok else 1)
