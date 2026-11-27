# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for gamedata_bin.py and randomize_units.py on hand-built files.  python -I tools/gamedata/test_randomize_units.py"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata_bin as gb  # noqa: E402
import gamedata_edit as ge  # noqa: E402
import inspect_romfs as ir  # noqa: E402
import randomize_units as ru  # noqa: E402


class Builder:
    """Writes a GameData container: a schema at data offset 0, then one flat table (descriptor, object, records)."""

    def __init__(self):
        self.data = bytearray()
        self.fix = []           # (data offset, string) pointers to strings
        self.rel = []           # data offsets of pointers into the data (value already written)
        self.labels = []        # (data offset, name)

    def strptr(self, at, s):
        if s is not None:
            self.fix.append((at, s))

    def build(self):
        strs = bytearray()
        where = {}

        def soff(s):
            if s not in where:
                where[s] = len(strs)
                strs.extend(s.encode("utf-8") + b"\0")
            return where[s]

        names = [soff(n) for _, n in self.labels]
        for _, s in self.fix:
            soff(s)
        nptr = len(self.fix) + len(self.rel)
        start = len(self.data) + 8 * nptr + 8 * len(self.labels)   # strings, as a data offset
        d = bytearray(self.data)
        for at, s in self.fix:
            struct.pack_into("<Q", d, at, start + where[s])
        ptrs = sorted([at for at, _ in self.fix] + self.rel)
        body = bytes(d) + b"".join(struct.pack("<Q", p) for p in ptrs)
        body += b"".join(struct.pack("<II", at, n) for (at, _), n in zip(self.labels, names)) + bytes(strs)
        hdr = struct.pack("<4I", gb.HEADER + len(body), len(self.data), nptr, len(self.labels)) + struct.pack("<IIQ", 0, 1, 0)
        return hdr + body


PERSON_FIELDS = [("名前", "string", 8, 0x20), ("識別子", "string", 8, 0x28), ("性別", "u8", 1, 0x30), ("兵種", "data", 16, 0x38),
                 ("アセット流用", "data", 16, 0x48), ("アセットフラグ", "string", 8, 0x58)]
REC = 0x60


def person(rows):
    """rows: (pid, mpid, ident, gender, job, reuse, aid). Returns Person.bin bytes."""
    b = Builder()
    d = b.data
    for name, typ, size, off in PERSON_FIELDS:
        at = len(d)
        d += bytes(0x20)
        b.strptr(at, name)
        b.strptr(at + 8, typ)
        struct.pack_into("<II", d, at + 24, size, off)
    first = len(d)
    for i, (pid, mpid, ident, gender, job, reuse, aid) in enumerate(rows):
        at = len(d)
        d += bytes(REC)
        b.strptr(at + 8, pid)
        struct.pack_into("<I", d, at + 0x10, 0x10000 | i)
        b.strptr(at + 0x20, mpid)
        b.strptr(at + 0x28, ident)
        d[at + 0x30] = gender
        b.strptr(at + 0x38, job)
        b.strptr(at + 0x48, reuse)
        b.strptr(at + 0x58, aid)
        b.labels.append((at, pid))
    desc = len(d)
    d += bytes(0x40)
    struct.pack_into("<II", d, desc, gb.TABLE_VERSION, 1)
    b.strptr(desc + 8, "個人")
    struct.pack_into("<IIQ", d, desc + 16, REC, len(PERSON_FIELDS), 0)
    b.rel.append(desc + 24)                       # the schema pointer: data offset 0
    obj = desc + 0x20
    d += bytes(0x10)
    b.strptr(obj + 8, "個人")
    struct.pack_into("<I", d, obj + 0x10, len(rows) << 16)
    struct.pack_into("<QQ", d, obj + 0x20, desc, first)
    b.rel += [obj + 0x20, obj + 0x28]
    b.labels.append((obj, "個人"))
    return b.build()


def table(b, name, fields, rec_size, rows, write, label_records=False):
    """Adds a flat table to Builder b: schema, records (write(at, row) fills each), descriptor and object."""
    d = b.data
    schema = len(d)
    for fname, typ, size, off in fields:
        at = len(d)
        d += bytes(0x20)
        b.strptr(at, fname)
        b.strptr(at + 8, typ)
        struct.pack_into("<II", d, at + 24, size, off)
    first = len(d)
    for i, row in enumerate(rows):
        at = len(d)
        d += bytes(rec_size)
        b.strptr(at + 8, row[0])
        struct.pack_into("<II", d, at + 0x10, 0x10000 | i, (3 + i) << 16)
        if label_records and row[0]:
            b.labels.append((at, row[0]))
        write(at, row)
    desc = len(d)
    d += bytes(0x40)
    struct.pack_into("<II", d, desc, gb.TABLE_VERSION, 1)
    b.strptr(desc + 8, name)
    struct.pack_into("<IIQ", d, desc + 16, rec_size, len(fields), schema)
    b.rel.append(desc + 24)
    obj = desc + 0x20
    d += bytes(0x10)
    b.strptr(obj + 8, name)
    struct.pack_into("<I", d, obj + 0x10, len(rows) << 16)
    struct.pack_into("<QQ", d, obj + 0x20, desc, first)
    b.rel += [obj + 0x20, obj + 0x28]
    b.labels.append((obj, name))


def string_list(b, at, items, width=8):
    """A string[] value at `at`: count | 8 << 16, then a pointer to an array of string pointers."""
    d = b.data
    struct.pack_into("<I", d, at, len(items) | width << 16)
    if not items:
        return
    arr = len(d)
    d += bytes(8 * len(items))
    for i, s in enumerate(items):
        b.strptr(arr + 8 * i, s)
    struct.pack_into("<Q", d, at + 8, arr)
    b.rel.append(at + 8)


def asset_table(entries, faces):
    """entries: (key, [[condition strings], ...], look, [keywords]); faces: (FID key, voice, face, 特殊), each labelled as in the game.
    Returns AssetTable.bin bytes."""
    b = Builder()
    lists = []          # string lists to lay out after the tables: (at, items)
    conds = []          # (at, alternatives)

    def write_entry(at, row):
        _, alts, look, kws = row
        conds.append((at + 0x20, alts))
        b.strptr(at + 0x30, look)
        lists.append((at + 0x40, kws))

    def write_face(at, row):
        b.strptr(at + 0x20, row[1])
        b.strptr(at + 0x28, row[2])
        b.data[at + 0x30] = row[3]

    table(b, "アセットテーブル", [("条件", "string[,]", 16, 0x20), ("容姿", "data", 16, 0x30), ("キーワード", "string[]", 16, 0x40)],
          0x50, entries, write_entry)
    table(b, "容姿情報", [("ボイス", "string", 8, 0x20), ("フェイス", "string", 8, 0x28), ("特殊", "u8", 1, 0x30)], 0x38, faces, write_face,
          label_records=True)
    for at, alts in conds:
        struct.pack_into("<I", b.data, at, len(alts) | 16 << 16)
        arr = len(b.data)
        b.data += bytes(16 * len(alts))
        struct.pack_into("<Q", b.data, at + 8, arr)
        b.rel.append(at + 8)
        for i, alt in enumerate(alts):
            string_list(b, arr + 16 * i, alt)
    for at, items in lists:
        string_list(b, at, items)
    b.labels.sort()
    return b.build()


ROWS = [
    ("PID_無し", None, None, 0, None, None, None),
    ("PID_カイ", "MPID_Kai", "Kai", 1, "JID_無職", None, "AID_カイ"),
    ("PID_トロイア", "MPID_Troja", "Tro", 2, "JID_バトルモンク", None, "AID_トロイア"),
    ("PID_リンド", "MPID_Lindo", "Lin", 1, None, None, "AID_リンド"),
    ("PID_イシュマール", "MPID_Ishmal", "Ism", 1, None, None, "AID_イシュマール"),
    ("PID_ダグザ", "MPID_Dagda", "Dag", 1, None, None, "AID_ダグザ"),
    ("PID_SC00_05_カイ", "MPID_Kai", "Kai", 1, "JID_オリハルディア", "PID_カイ", None),
    ("PID_SC00_05_リンド", "MPID_Lindo", "Lin", 1, "JID_ハイエピタフ", "PID_リンド", None),
    ("PID_SC00_06_ダグザ竜", "MPID_Dagda", "Dag", 0, "JID_ダグザ竜", None, None),
    ("PID_SC00_06_ダグザ", "MPID_Dagda", "Dag", 1, "JID_ジ・エターナル", "PID_ダグザ", "AID_ダグザ_冥将"),
    ("PID_SC00_13_アナトリア替", "MPID_WhitePeople", None, 1, None, None, "AID_モブ"),
    ("PID_SC11_05_リンド代", "MPID_Lindo", "Lin", 1, None, "PID_カイ", None),     # borrows another character's base (as SC11_05_ザーコネ)
]


class TestGameDataBin(unittest.TestCase):
    def setUp(self):
        self.data = person(ROWS)
        self.b = gb.GameDataBin(self.data)

    def test_table_and_schema(self):
        t = self.b.table("個人")
        self.assertEqual((t.depth, t.rec_size), (1, REC))
        self.assertEqual([(f.name, f.type, f.size, f.off) for f in t.fields], PERSON_FIELDS)
        self.assertEqual([self.b.key(r) for r in t.records()], [r[0] for r in ROWS])

    def test_values(self):
        t = self.b.table("個人")
        kai = list(t.records())[1]
        self.assertEqual(self.b.string(kai + 0x20), "MPID_Kai")
        self.assertEqual(self.b.value(kai, t.field("性別")), 1)
        self.assertIsNone(self.b.ptr(kai + 0x48))                  # a null field is not in the pointer list
        rec = self.b.record(t, kai)
        self.assertEqual(rec["$key"], "PID_カイ")
        self.assertEqual(rec["アセットフラグ"], "AID_カイ")
        self.assertNotIn("アセット流用", rec)

    def test_not_a_container(self):
        self.assertIsNone(gb.parse(b"\x00" * 64))
        self.assertIsNone(gb.parse(self.data[:-1]))
        self.assertIsNone(gb.parse(b"MsgStdBn"))

    def test_inspect_dump(self):
        self.assertEqual(ir.kind(self.data), "gamedata")
        out = ir.decode(self.data)
        self.assertEqual(out["$gamedata"]["個人"][2]["名前"], "MPID_Troja")


class TestSplitMix(unittest.TestCase):
    def test_reference_values(self):
        r = ru.SplitMix64(1234567)   # the reference implementation's published sequence
        self.assertEqual([r.next() for _ in range(3)], [6457827717110365317, 3203168211198807973, 9817491932198370423])

    def test_fnv(self):
        self.assertEqual(ru.fnv1a64(b""), 0xCBF29CE484222325)
        self.assertEqual(ru.fnv1a64(b"a"), 0xAF63DC4C8601EC8C)


class TestMapping(unittest.TestCase):
    def setUp(self):
        self.data = person(ROWS)
        self.p = ru.Person(self.data)

    def test_characters(self):
        # base records only: an AID of their own and nothing borrowed; the mob stand-in has no identifier
        self.assertEqual([c.ident for c in self.p.chars], ["Kai", "Tro", "Lin", "Ism", "Dag"])

    def test_find(self):
        for n in ("Kai", "kai", "MPID_Troja", "troja", "PID_リンド", "リンド"):
            self.assertIsNotNone(self.p.find(n), n)
        self.assertEqual(self.p.find("Lindo").ident, "Lin")
        self.assertIsNone(self.p.find("Anatolia"))

    POOL = ["Kai", "Tro", "Lin", "Dag"]

    def idents(self, m):
        return [(o.ident, r.ident) for o, r in m]

    def test_shuffle_seeded_and_never_self(self):
        seen = set()
        for seed in range(200):
            m, warn = ru.make_mapping(self.p, seed, self.POOL)
            self.assertEqual(warn, [])
            self.assertEqual([o.ident for o, _ in m], ["Kai", "Tro", "Lin", "Dag"])      # Person.bin's order
            reps = [r.ident for _, r in m]
            self.assertEqual(sorted(reps), sorted(self.POOL))                           # a shuffle of the pool: Ism never drawn
            self.assertTrue(all(o.ident != r.ident for o, r in m), (seed, reps))
            self.assertEqual(m, ru.make_mapping(self.p, seed, self.POOL)[0])
            seen.add(tuple(reps))
        self.assertEqual(len(seen), 9)                                                  # every derangement of four turns up

    def test_draw_is_even(self):
        # each of the 9 derangements of four about equally often (a fix-up that hands a self-draw to a neighbour skews this)
        counts = {}
        for seed in range(2700):
            reps = tuple(r.ident for _, r in ru.make_mapping(self.p, seed, self.POOL)[0])
            counts[reps] = counts.get(reps, 0) + 1
        self.assertEqual(len(counts), 9)
        self.assertTrue(all(220 <= c <= 380 for c in counts.values()), counts)

    def test_one_left_keeps_itself(self):
        self.assertEqual(self.idents(ru.make_mapping(self.p, 3, ["Kai"])[0]), [("Kai", "Kai")])

    def test_force(self):
        m, warn = ru.make_mapping(self.p, 7, self.POOL, ru.parse_force("Lindo=Troy, Kai=Dagda")[0])
        self.assertEqual(len(warn), 1)                       # "Troy" is not a name the data knows (Troja is)
        m, warn = ru.make_mapping(self.p, 7, self.POOL, ru.parse_force("Lindo=Troja, Kai=Dag, Tro=Dag")[0])
        self.assertEqual(len(warn), 1)                       # Dag is taken
        pairs = self.idents(m)
        self.assertEqual(pairs[:2], [("Lin", "Tro"), ("Kai", "Dag")])
        self.assertEqual([o for o, _ in pairs[2:]], ["Tro", "Dag"])          # the rest share what is left
        self.assertEqual(sorted(r for _, r in pairs[2:]), ["Kai", "Lin"])
        self.assertEqual(ru.parse_force(" Kai , Lin = Tro,"), ([("Lin", "Tro")], ['randomize_force: "Kai" is not original=replacement']))

    def test_force_self(self):
        m, _ = ru.make_mapping(self.p, 7, self.POOL, ru.parse_force("Kai=Kai")[0])
        self.assertEqual(self.idents(m)[0], ("Kai", "Kai"))
        self.assertEqual(len(m), 4)

    def test_force_outside_pool(self):
        m, _ = ru.make_mapping(self.p, 1, [], ru.parse_force("Dag=Kai")[0])
        self.assertEqual(self.idents(m), [("Dag", "Kai")])
        for seed in range(20):   # Tro is the one original left and Kai a spare candidate: Tro never draws itself
            m, warn = ru.make_mapping(self.p, seed, ["Kai", "Tro"], ru.parse_force("Kai=Ism")[0])
            self.assertEqual((self.idents(m), warn), ([("Kai", "Ism"), ("Tro", "Kai")], []))
        m, warn = ru.make_mapping(self.p, 1, ["Kai", "Tro"], ru.parse_force("Ism=Kai")[0])   # one candidate (Tro) for two
        self.assertEqual(self.idents(m)[0], ("Ism", "Kai"))
        self.assertEqual(len(m), 2)
        self.assertEqual(warn, ["pool: no replacement left for Tro"] if m[1][0].ident == "Kai" else ["pool: no replacement left for Kai"])

    def test_missing_pool_name(self):
        _, warn = ru.make_mapping(self.p, 1, ["Kai", "Tro", "Xyz"])
        self.assertEqual(warn, ["pool: no character Xyz in Person.bin"])

    def test_pool_list(self):
        self.assertEqual(len(ru.POOL), 113)
        self.assertEqual(len(set(ru.POOL)), 113)
        for out in ("Ism", "Bal", "Oct", "Ger", "Lan"):   # Eshmel, no model (Balor, Oktar), generic (Gervasio), stock model (Randa)
            self.assertNotIn(out, ru.POOL)
        self.assertEqual(len(ru.LIVING_LOOKS), 7)                 # the pool's Underworld-only looks
        self.assertEqual(len(ru.UNDERWORLD_FORMS), 75)            # the pool characters with no Underworld form
        self.assertEqual(len(set(ru.UNDERWORLD_FORMS)), 75)
        self.assertTrue(all(x.endswith("冥闘士") for x in ru.LIVING_LOOKS))
        for kept in ("Ara", "Ban", "Dra", "Kat"):         # portraits on Underworld entries (Arago, Vandahl, Drake); Katarina
            self.assertIn(kept, ru.POOL)
        self.assertIn("Ma\uff44", ru.POOL)


class TestApply(unittest.TestCase):
    def setUp(self):
        self.data = person(ROWS)
        self.p = ru.Person(self.data)

    def pair(self, a, b):
        return (self.p.by_ident[a], self.p.by_ident[b])

    def rec(self, data, key):
        b = gb.GameDataBin(data)
        t = b.table("個人")
        for r in t.records():
            if b.key(r) == key:
                return b.record(t, r)
        raise KeyError(key)

    def look(self, data, key):
        """The model a record shows: its own AID, else the AID of the record it borrows from."""
        r = self.rec(data, key)
        return r.get("アセットフラグ") or self.rec(data, r["アセット流用"]).get("アセットフラグ")

    def test_swap(self):
        out, changed, skipped = ru.apply(self.data, [self.pair("Kai", "Tro"), self.pair("Tro", "Lin")])
        self.assertEqual(len(out), len(self.data))
        kai = self.rec(out, "PID_カイ")
        self.assertEqual((kai["名前"], kai["アセットフラグ"], kai["性別"], kai["識別子"], kai["兵種"]),
                         ("MPID_Troja", "AID_トロイア", 2, "Kai", "JID_無職"))
        sc = self.rec(out, "PID_SC00_05_カイ")
        self.assertEqual((sc["名前"], sc["アセット流用"], sc["兵種"]), ("MPID_Troja", "PID_カイ", "JID_オリハルディア"))
        tro = self.rec(out, "PID_トロイア")       # Troy takes Lind's look; Cai still got Troy's own
        self.assertEqual((tro["名前"], tro["アセットフラグ"], tro["性別"]), ("MPID_Lindo", "AID_リンド", 1))
        self.assertEqual(self.rec(out, "PID_リンド")["名前"], "MPID_Lindo")   # not in the mapping
        self.assertEqual([k for k, _ in changed], ["PID_カイ", "PID_トロイア", "PID_SC00_05_カイ"])
        self.assertEqual(dict(changed)["PID_SC00_05_カイ"], ["name"])      # it keeps borrowing PID_カイ, which now looks like Troy
        self.assertEqual(skipped, [])

    def test_chain_keeps_each_look(self):
        # The in-game seed 5664321453894125683 had Lin -> Tro and Tro -> Mhm: Lind's story slot borrowed PID_トロイア, which by then
        # looked like Mahadu. Every slot must show its own replacement's original model, however the pairs chain.
        before = {k: self.look(self.data, k) for k in ("PID_カイ", "PID_トロイア", "PID_リンド")}
        m = [self.pair("Lin", "Tro"), self.pair("Tro", "Kai"), self.pair("Kai", "Lin")]
        out, _, _ = ru.apply(self.data, m)
        self.assertEqual(self.look(out, "PID_SC00_05_リンド"), before["PID_トロイア"])
        self.assertEqual(self.look(out, "PID_リンド"), before["PID_トロイア"])
        self.assertEqual(self.look(out, "PID_トロイア"), before["PID_カイ"])
        self.assertEqual(self.look(out, "PID_SC00_05_カイ"), before["PID_リンド"])
        self.assertEqual(self.look(out, "PID_カイ"), before["PID_リンド"])
        odd = self.rec(out, "PID_SC11_05_リンド代")       # borrowed Cai's base: now its own (Lind's), which looks like Troy
        self.assertEqual((odd["アセット流用"], self.look(out, "PID_SC11_05_リンド代"), odd["名前"]),
                         ("PID_リンド", before["PID_トロイア"], "MPID_Troja"))

    def test_dragon_left_alone(self):
        out, changed, skipped = ru.apply(self.data, [self.pair("Dag", "Kai")])
        self.assertEqual(skipped, ["PID_SC00_06_ダグザ竜"])
        dragon = self.rec(out, "PID_SC00_06_ダグザ竜")
        self.assertEqual((dragon["名前"], dragon["性別"]), ("MPID_Dagda", 0))
        human = self.rec(out, "PID_SC00_06_ダグザ")
        self.assertEqual((human["名前"], human["アセット流用"], human["アセットフラグ"]), ("MPID_Kai", "PID_ダグザ", "AID_カイ"))
        self.assertEqual(self.look(out, "PID_ダグザ"), "AID_カイ")
        self.assertEqual(dict(changed)["PID_SC00_06_ダグザ"], ["name", "model"])

    def test_only_those_bytes(self):
        out, _, _ = ru.apply(self.data, [self.pair("Lin", "Kai")])
        diff = [i for i in range(len(out)) if out[i] != self.data[i]]
        b = gb.GameDataBin(self.data)
        t = b.table("個人")
        allowed = set()
        for r in t.records():
            if b.string(r + 0x28) == "Lin":
                for off, n in ((0x20, 8), (0x30, 1), (0x48, 8), (0x58, 8)):
                    allowed.update(range(gb.HEADER + r + off, gb.HEADER + r + off + n))
        self.assertTrue(diff)
        self.assertTrue(set(diff) <= allowed)

    def test_self_and_empty(self):
        out, changed, _ = ru.apply(self.data, [self.pair("Kai", "Kai")])
        self.assertEqual((out, changed), (self.data, []))

    def test_lines(self):
        m = [self.pair("Kai", "Tro")]
        self.assertEqual(ru.mapping_lines(5, m), ["randomizer: seed 5: 1 pairs", "randomizer:   Kai(Kai) -> Tro(Troja)"])
        lines = ru.mapping_lines(5, m * 21)
        self.assertEqual([len(x.split(" -> ")) - 1 for x in lines[1:]], [10, 10, 1])
        self.assertEqual(lines[0], "randomizer: seed 5: 21 pairs")
        out, changed, skipped = ru.apply(self.data, m)
        self.assertRegex(ru.summary_line(changed, skipped, out), r"^randomizer: Person.bin: 2 records changed, 0 dragon forms left as "
                                                                 r"they are \(fnv1a64 [0-9a-f]{16}\)$")


class TestEditor(unittest.TestCase):
    def test_round_trip(self):
        data = person(ROWS)
        self.assertEqual(ge.Editor(data).build(), data)

    def test_insert_moves_what_follows(self):
        data = person(ROWS)
        b = gb.GameDataBin(data)
        t = b.table("個人")
        recs = list(t.records())
        e = ge.Editor(data)
        e.insert(recs[3], bytes(0x40))                          # 0x40 bytes before Lind's base record
        e.set_ptr(recs[3], "PID_新しい")                        # a new pointer to a new string, in the inserted bytes
        e.set_ptr(recs[3] + 8, "PID_カイ")                      # one to a string the pool already has
        e.add_label(recs[3], "新しい")
        out = gb.GameDataBin(e.build())
        t2 = out.table("個人")
        # the table still counts its records from the same first one: the inserted bytes now sit where Lind's base record was, and
        # their key field was just pointed at PID_カイ
        self.assertEqual([out.key(r) for r in t2.records()][:4], [r[0] for r in ROWS][:3] + ["PID_カイ"])
        self.assertEqual(out.string(recs[3]), "PID_新しい")
        self.assertEqual(out.string(recs[3] + 8), "PID_カイ")
        moved = recs[3] + 0x40
        self.assertEqual(out.key(moved), "PID_リンド")
        self.assertEqual(out.string(moved + 0x20), "MPID_Lindo")
        self.assertIn((recs[3], "新しい"), out.labels)
        self.assertIn((moved, "PID_リンド"), out.labels)        # labels at or after the insert move
        self.assertEqual(len(out.ptrs), len(b.ptrs) + 2)


class TestUnderworldForms(unittest.TestCase):
    ENTRIES = [
        ("e0", [["AID_ブレザ"]], None, []),                                                     # Breza's plain look: no portrait
        ("e1", [["AID_ブレザ", "SID_冥界兵"], ["PID_冥将_ブレザ"]], "ブレザ冥闘士", ["SID_冥界兵"]),   # the template
        ("e2", [["AID_ドレイク"]], "ドレイク冥闘士", ["SID_冥界兵"]),                              # Drake: Underworld only
        ("e3", [["AID_カイ"]], "カイ", []),
        ("e4", [["AID_カイ", "SID_冥界兵"]], "カイ冥闘士", ["SID_冥界兵"]),                       # Cai: has both
        ("e5", [["AID_コウカ"]], "コウカ", []),                                                 # Hong Hua: living only
        ("e6", [["AID_アラゴ"]], "アラゴ大王冥闘士", ["SID_冥界兵"]),
    ]
    FACES = [("FID_ブレザ冥闘士", "Bre0", "168", 1), ("FID_ドレイク冥闘士", "Dra0", "166", 1), ("FID_カイ", "Kai0", "017", 0),
             ("FID_カイ冥闘士", "Kai9", "017", 1), ("FID_コウカ", "Kou0", "107", 0), ("FID_アラゴ大王冥闘士", "Mme9", "170", 1)]
    LOOKS = ["ブレザ冥闘士", "ドレイク冥闘士", "アラゴ大王冥闘士"]

    def read(self, data):
        b = gb.GameDataBin(data)
        t, f = b.table("アセットテーブル"), b.table("容姿情報")

        def strs(at):
            n, arr = b.u32(at) & 0xFFFF, b.ptr(at + 8)
            return [] if arr is None else [b.string(arr + 8 * i) for i in range(n)]
        entries = []
        for r in t.records():
            word, arr = b.u32(r + 0x20), b.ptr(r + 0x28)
            entries.append(([strs(arr + 16 * i) for i in range(word & 0xFFFF)], b.string(r + 0x30), strs(r + 0x40)))
        faces = {b.key(r): (b.string(r + 0x20), b.string(r + 0x28), b.d[gb.HEADER + r + 0x30]) for r in f.records()}
        return b, entries, faces

    def test_builder(self):
        _, entries, faces = self.read(asset_table(self.ENTRIES, self.FACES))
        self.assertEqual(entries[1], (self.ENTRIES[1][1], "ブレザ冥闘士", ["SID_冥界兵"]))
        self.assertEqual(faces["FID_カイ冥闘士"], ("Kai9", "017", 1))

    def test_forms(self):
        data = asset_table(self.ENTRIES, self.FACES)
        out, changes = ru.apply_assets(data, self.LOOKS, ["コウカ", "カイ"])
        b, entries, faces = self.read(out)
        # the living looks: a new untinted portrait entry each; plain looks point at it, without the Underworld keyword
        self.assertEqual(faces["FID_ブレザ"], ("Bre0", "168", 0))
        self.assertEqual(faces["FID_ドレイク"], ("Dra0", "166", 0))
        self.assertEqual(faces["FID_アラゴ大王"], ("Mme0", "170", 0))                      # the generic voice's dry set
        self.assertEqual(entries[0], ([["AID_ブレザ"]], "ブレザ", []))
        self.assertEqual(entries[2], ([["AID_ドレイク"]], "ドレイク", []))
        self.assertEqual(entries[1], (self.ENTRIES[1][1], "ブレザ冥闘士", ["SID_冥界兵"]))  # Breza's Underworld entry stays
        self.assertEqual(faces["FID_ブレザ冥闘士"], ("Bre0", "168", 1))                     # and so does his tinted portrait
        # Hong Hua gets an Underworld form; Cai already has one
        self.assertEqual(faces["FID_コウカ冥闘士"], ("Kou0", "107", 1))
        self.assertNotIn("FID_カイ冥闘士冥闘士", faces)
        added = entries[len(self.ENTRIES):]
        self.assertEqual(added, [([["AID_ドレイク", "SID_冥界兵"]], "ドレイク冥闘士", ["SID_冥界兵"]),
                                 ([["AID_アラゴ", "SID_冥界兵"]], "アラゴ大王冥闘士", ["SID_冥界兵"]),
                                 ([["AID_コウカ", "SID_冥界兵"]], "コウカ冥闘士", ["SID_冥界兵"])])
        self.assertIn("AID_カイ: skipped", changes)
        # numbering, labels, counts
        f = b.table("容姿情報")
        recs = list(f.records())
        self.assertEqual([b.u32(r + 0x10) for r in recs], [0x10000 | i for i in range(len(recs))])
        self.assertEqual(sorted(b.u32(r + 0x14) >> 16 for r in recs), list(range(3, 3 + len(recs))))
        self.assertEqual({n for _, n in b.labels if n.startswith("FID_")}, set(faces))
        # the rest of the file reads as before
        b0 = gb.GameDataBin(data)
        self.assertEqual([b0.key(r) for r in b0.table("容姿情報").records()], [b.key(r) for r in recs][:len(self.FACES)])

    def test_nothing_to_do(self):
        data = asset_table(self.ENTRIES, self.FACES)
        out, changes = ru.apply_assets(data, [], [])
        self.assertEqual(out, data)
        self.assertEqual(changes, [])


class TestCutscenes(unittest.TestCase):
    """Steps 3 and 4 of apply_assets: the portraits Prologue Ch. 2's cutscene names, and the chapter flags (AID_x冥将)."""
    read = TestUnderworldForms.read
    ENTRIES = [
        ("e0", [["AID_ブレザ"]], None, []),
        ("e1", [["AID_ブレザ", "SID_冥界兵"], ["PID_冥将_ブレザ"]], "ブレザ冥闘士", ["SID_冥界兵"]),   # the template
        ("e2", [["AID_カイ"]], "カイ", []),
        ("e3", [["AID_カイ", "SID_冥界兵"], ["AID_カイ", "AID_カイ冥将"], ["PID_冥将_カイ"]], "カイ冥闘士", ["SID_冥界兵"]),
        ("e4", [["AID_レダ"]], "レダ", []),
        ("e5", [["AID_レダ", "SID_冥界兵"], ["AID_レダ", "AID_レダ冥将"]], "レダ冥闘士", ["SID_冥界兵"]),
        ("e6", [], "レダ冥闘士", ["SID_冥界兵"]),                                                 # a named look entry (レダ冥将)
        ("e7", [["AID_コウカ"]], "コウカ", []),                                                 # Hong Hua: living only
        ("e8", [["AID_ダグザ", "AID_ダグザ竜_冥将"]], "ダグザ竜冥闘士", []),                         # no SID_冥界兵 beside it: not a flag
    ]
    FACES = [("FID_ブレザ冥闘士", "Bre0", "168", 1), ("FID_カイ", "Kai0", "017", 0), ("FID_カイ冥闘士", "Kai9", "017", 1),
             ("FID_レダ", "Led0", "023", 0), ("FID_レダ冥闘士", "Led9", "023", 1), ("FID_コウカ", "Kou0", "107", 0)]

    def test_lord_played_by_another(self):
        data = asset_table(self.ENTRIES, self.FACES)
        swaps = {"AID_カイ": "AID_コウカ", "AID_コウカ": "AID_ダグザ", "AID_ダグザ": "AID_カイ"}
        out, changes = ru.apply_assets(data, [], ["コウカ"], swaps, ["カイ冥闘士", "レダ冥闘士"])
        b, entries, faces = self.read(out)
        # the cutscene's FID_カイ冥闘士 shows Hong Hua's new Underworld portrait; Cai's own, for his look in battle, is a copy
        self.assertEqual(faces["FID_カイ冥闘士"], ("Kou0", "107", 1))
        self.assertEqual(faces["FID_カイ冥闘士本人"], ("Kai9", "017", 1))
        self.assertEqual(faces["FID_コウカ冥闘士"], ("Kou0", "107", 1))
        self.assertEqual(entries[3], ([["AID_カイ", "SID_冥界兵"], ["AID_カイ", "SID_冥界兵"], ["PID_冥将_カイ"]], "カイ冥闘士本人",
                                      ["SID_冥界兵"]))
        # Leda's slot is not replaced: her portrait, flag and looks stay
        self.assertEqual(faces["FID_レダ冥闘士"], ("Led9", "023", 1))
        self.assertNotIn("FID_レダ冥闘士本人", faces)
        self.assertEqual(entries[5:7], [(self.ENTRIES[5][1], "レダ冥闘士", ["SID_冥界兵"]), ([], "レダ冥闘士", ["SID_冥界兵"])])
        self.assertEqual(entries[8], (self.ENTRIES[8][1], "ダグザ竜冥闘士", []))
        # whoever plays Cai is Underworld while his flag is set
        self.assertEqual(entries[len(self.ENTRIES):], [([["AID_コウカ", "SID_冥界兵"]], "コウカ冥闘士", ["SID_冥界兵"]),
                                                       ([["AID_コウカ", "AID_カイ冥将"]], "コウカ冥闘士", ["SID_冥界兵"])])
        self.assertIn("FID_カイ冥闘士 shows コウカ冥闘士", changes)
        self.assertEqual({n for _, n in b.labels if n.startswith("FID_")}, set(faces))

    def test_lords_swap(self):
        # Cai and Leda play each other: each portrait shows the other's vanilla one, read before either was written
        data = asset_table(self.ENTRIES, self.FACES)
        out, changes = ru.apply_assets(data, [], [], {"AID_カイ": "AID_レダ", "AID_レダ": "AID_カイ"}, ["カイ冥闘士", "レダ冥闘士"])
        b, entries, faces = self.read(out)
        self.assertEqual(faces["FID_カイ冥闘士"], ("Led9", "023", 1))
        self.assertEqual(faces["FID_レダ冥闘士"], ("Kai9", "017", 1))
        self.assertEqual(faces["FID_カイ冥闘士本人"], ("Kai9", "017", 1))
        self.assertEqual(faces["FID_レダ冥闘士本人"], ("Led9", "023", 1))
        self.assertEqual([x[1] for x in entries[3:7]], ["カイ冥闘士本人", "レダ", "レダ冥闘士本人", "レダ冥闘士本人"])
        self.assertEqual(entries[len(self.ENTRIES):], [([["AID_レダ", "AID_カイ冥将"]], "レダ冥闘士本人", ["SID_冥界兵"]),
                                                       ([["AID_カイ", "AID_レダ冥将"]], "カイ冥闘士本人", ["SID_冥界兵"])])

    def test_voiceless_replacement(self):
        # Katarina has no voice: the portrait the cutscene names loses its voice too, and the copy keeps Leda's
        faces = self.FACES + [("FID_カタリナ", None, "184", 5)]
        entries = self.ENTRIES + [("e9", [["AID_カタリナ"]], "カタリナ", [])]
        data = asset_table(entries, faces)
        out, changes = ru.apply_assets(data, [], ["カタリナ"], {"AID_レダ": "AID_カタリナ"}, ["カイ冥闘士", "レダ冥闘士"])
        b, _, got = self.read(out)
        self.assertEqual(got["FID_レダ冥闘士"], (None, "184", 1))
        self.assertEqual(got["FID_レダ冥闘士本人"], ("Led9", "023", 1))
        self.assertEqual(got["FID_カタリナ冥闘士"], (None, "184", 1))
        self.assertIn("FID_レダ冥闘士 shows カタリナ冥闘士", changes)
        self.assertNotIn(next(r for r in b.table("容姿情報").records() if b.key(r) == "FID_レダ冥闘士") + 0x20, b.ptrs)

    def test_no_mapping(self):
        data = asset_table(self.ENTRIES, self.FACES)
        self.assertEqual(ru.apply_assets(data, [], [], None), (data, []))

    def test_asset_swaps(self):
        p = ru.Person(person(ROWS))
        kai, tro, lin = p.find("Kai"), p.find("Tro"), p.find("Lin")
        self.assertEqual(ru.asset_swaps([(kai, tro), (tro, kai), (lin, lin)]), {"AID_カイ": "AID_トロイア", "AID_トロイア": "AID_カイ"})


def skeleton(slots, version="4.1.24", nonessential=True, bones=2):
    """A Spine binary skeleton up to its slots (then a byte the edit must keep): slot names as given, each on bone 0."""
    s = ru._skel_string
    out = b"\x11" * 8 + s(version) + struct.pack(">4f", 0, 0, 100, 100) + bytes([nonessential])
    if nonessential:
        out += struct.pack(">f", 30) + s("./images/") + s("")
    out += bytes([2]) + s("00_Eye") + s("x")                       # the string table (attachment names)
    out += bytes([bones])
    for i in range(bones):
        out += s("bone%d" % i) + (bytes([0]) if i else b"") + bytes(32) + bytes([0, 1]) + (b"\xff" * 4 if nonessential else b"")
    out += bytes([len(slots)])
    for name in slots:
        out += s(name) + bytes([0]) + b"\xff" * 8 + bytes([1, 0])
    return out + b"\x7e"


class TestEyeSlots(unittest.TestCase):
    def test_rename(self):
        data = skeleton(["Body", "Eye_W", "Eye", "Eye_Highlight", "EyeShadow", "Bear_Eye", "Eye_L"])
        out, renames = ru.rename_eye_slots(data)
        self.assertEqual(renames, [("Eye_W", "00_Eye_W"), ("Eye", "00_Eye"), ("Eye_Highlight", "00_Eye_Hi"), ("Eye_L", "00_Eye_L")])
        self.assertEqual(out, skeleton(["Body", "00_Eye_W", "00_Eye", "00_Eye_Hi", "EyeShadow", "Bear_Eye", "00_Eye_L"]))

    def test_long_name(self):
        # a name of 127 bytes or more takes two bytes of length
        long = "x" * 130
        data = skeleton([long, "Eye"], nonessential=False)
        out, renames = ru.rename_eye_slots(data)
        self.assertEqual(out, skeleton([long, "00_Eye"], nonessential=False))

    def test_whites_only(self):
        # Gladius (119): the iris is right, the whites are 00_EyeWhite
        out, renames = ru.rename_eye_slots(skeleton(["00_Eye", "00_EyeWhite", "Body"]))
        self.assertEqual(renames, [("00_EyeWhite", "00_Eye_W")])
        self.assertEqual(out, skeleton(["00_Eye", "00_Eye_W", "Body"]))

    def test_left_alone(self):
        for data in (skeleton(["00_Eye", "00_Eye_W", "Body"]),  # already has the names the effect looks for
                     skeleton(["Eye", "00_eye_w", "Eye_W"]),   # a new name is taken
                     skeleton(["Eye"], version="4.2.01"),      # another format
                     skeleton(["Eye"])[:40]):                  # cut short
            self.assertEqual(ru.rename_eye_slots(data), (data, []))


def msbt(speakers, rec=20, tail=b"TAILDATA", labels=None):
    """An MSBT file like Levelup.msbt: LBL1 (the labels, one hash slot; MID_LevelUp_Kra0_00, _01... unless given), ATR1 with a record a
    line whose field 3 is the offset of its speaker's string in the area after the records (0 for none), and TXT2 (filler, `tail`)."""
    labels = labels or ["MID_LevelUp_Kra0_%02d" % i for i in range(len(speakers))]
    lbl = struct.pack("<III", 1, len(labels), 12)
    for i, name in enumerate(labels):
        lbl += bytes([len(name)]) + name.encode() + struct.pack("<I", i)
    area = bytearray()
    recs = bytearray()
    base = 8 + rec * len(speakers)
    for sp in speakers:
        r = bytearray(rec)
        if sp is not None:
            struct.pack_into("<I", r, 12, base + len(area))
            area += sp.encode() + b"\0"
        recs += r
    atr = struct.pack("<II", len(speakers), rec) + recs + area

    def section(magic, data):
        out = magic + struct.pack("<I", len(data)) + bytes(8) + data
        return out + b"\xab" * (-len(out) % 16)
    body = section(b"LBL1", lbl) + section(b"ATR1", bytes(atr)) + section(b"TXT2", tail)
    head = bytearray(b"MsgStdBn\xff\xfe" + bytes(22))
    head[0xC] = 0
    struct.pack_into("<H", head, 0xE, 3)
    struct.pack_into("<I", head, 0x12, 0x20 + len(body))
    return bytes(head) + body


class TestSpeakers(unittest.TestCase):
    def speakers(self, data):
        p, atr = 0x20, None
        for _ in range(struct.unpack_from("<H", data, 0xE)[0]):
            size = struct.unpack_from("<I", data, p + 4)[0]
            if data[p:p + 4] == b"ATR1":
                atr = data[p + 16:p + 16 + size]
            p = (p + 16 + size + 15) & ~15
        n, rec = struct.unpack_from("<II", atr, 0)
        out = []
        for k in range(n):
            v = struct.unpack_from("<I", atr, 8 + k * rec + 12)[0]
            out.append(None if v == 0 else atr[v:atr.index(b"\0", v)].decode())
        return out

    def test_rewrite(self):
        data = msbt(["クラプカ", "クラプカ", None, "カイ", "ライラー"])
        out, n = ru.rewrite_speakers(data, {"クラプカ": "ライラー", "カイ": "マーナ", "ハンノ": "カイ"})
        self.assertEqual(n, 3)
        self.assertEqual(self.speakers(out), ["ライラー", "ライラー", None, "マーナ", "ライラー"])
        self.assertEqual(struct.unpack_from("<I", out, 0x12)[0], len(out))
        self.assertEqual(len(out) % 16, 0)
        self.assertTrue(out.endswith(b"TAILDATA" + b"\xab" * 8))       # TXT2 moved, kept whole
        self.assertEqual(len(out) - len(data), 16)                     # マーナ appended (10 bytes): one more 16-byte line
        p, magics = 0x20, []
        for _ in range(3):
            magics.append(out[p:p + 4])
            p = (p + 16 + struct.unpack_from("<I", out, p + 4)[0] + 15) & ~15
        self.assertEqual((magics, p), ([b"LBL1", b"ATR1", b"TXT2"], len(out)))

    def test_reuse(self):
        # ライラー is already in the area: no growth
        data = msbt(["クラプカ", "ライラー"])
        out, n = ru.rewrite_speakers(data, {"クラプカ": "ライラー"})
        self.assertEqual((n, len(out)), (1, len(data)))
        self.assertEqual(self.speakers(out), ["ライラー", "ライラー"])

    def test_left_alone(self):
        data = msbt(["イシュマール", None])
        self.assertEqual(ru.rewrite_speakers(data, {"クラプカ": "ライラー"}), (data, 0))
        self.assertEqual(ru.rewrite_speakers(data[:40], {"イシュマール": "x"}), (data[:40], 0))
        utf16 = bytearray(data)
        utf16[0xC] = 1
        self.assertEqual(ru.rewrite_speakers(bytes(utf16), {"イシュマール": "x"}), (bytes(utf16), 0))

    def test_only_quotes(self):
        # Die.msbt: the voice quotes change; the story's own deaths (a role, as in cutscenes) and the header labels do not
        labels = ["MID_DIE_Kra0", "MID_DIE_Kra0_01", "MID_RETREAT_Kra0_01", "MID_DIE_SCXX_00_Kai_001", "MID_EV_DIE_SC00_05_Kai_001",
                  "MID_LevelUp_Max_Kai0_00", "MID_Skill_LevelUp_Ism0M_00"]
        data = msbt(["クラプカ"] * 3 + ["カイ"] * 4, labels=labels)
        out, n = ru.rewrite_speakers(data, {"クラプカ": "ライラー", "カイ": "マーナ"})
        self.assertEqual(n, 4)
        self.assertEqual(self.speakers(out), ["クラプカ", "ライラー", "ライラー", "カイ", "カイ", "マーナ", "マーナ"])

    def test_quote_labels(self):
        for label in ("MID_LevelUp_Kra0_00", "MID_LevelUp_Min_Ism0F_02", "MID_LevelUp_Max0_00", "MID_LevelUp_Max_Max0_00",
                      "MID_Skill_LevelUp_Max_Kai1_00", "MID_DIE_Tah0_01", "MID_RETREAT_Tah0_01"):
            self.assertTrue(ru.is_quote_label(label), label)
        for label in ("MID_DIE_Tah0", "MID_DIE_SCXX_00_Kai_001", "MID_EV_DIE_SC00_05_Kai_001", "MID_EV_RETREAT_SC30_00_Nat_001",
                      "MID_LevelUp_Max", "MID_DIE_Tah0_", "MID_DIE_Tah0_01x", "MID_DIE_SCXX_Termination", "MID_LevelUp_Kra00_00"):
            self.assertFalse(ru.is_quote_label(label), label)

    def test_renames(self):
        p = ru.Person(person(ROWS))
        kai, tro, lin = p.find("Kai"), p.find("Tro"), p.find("Lin")
        # Troy's slot wears Cai: Cai's lines name トロイア; Lind keeps his own
        self.assertEqual(ru.speaker_renames([(tro, kai), (kai, lin), (lin, lin)]), {"カイ": "トロイア", "リンド": "カイ"})


KON = ("PID_コン・ムー", "MPID_KonMoe", "Kon", 1, None, None, "AID_コン・ムー")


class TestKasumi(unittest.TestCase):
    """The second Eshmel ([fefw] randomize_eshmel): its look, its place in the mapping, Person.bin and AssetTable.bin."""
    EGG = ru.KasumiLook(*ru.KASUMI_EGG, hood=True)
    LOOK = ru.KasumiLook("男", "C", "E", 7, 12)

    def test_draw(self):
        looks = [ru.draw_kasumi(seed) for seed in range(5000)]
        self.assertEqual([x.describe() for x in looks[:50]], [ru.draw_kasumi(s).describe() for s in range(50)])   # seeded
        eggs = [x for x in looks if x.hood]
        self.assertTrue(60 <= len(eggs) <= 140, len(eggs))                       # about 1 in 50
        self.assertTrue(all(x.describe() == "女 頭部B 身体B 肌色2 髪色3 フード" for x in eggs))
        rest = [x for x in looks if not x.hood]
        self.assertEqual({x.sex for x in rest}, {"男", "女"})
        self.assertEqual({x.head for x in rest}, set("ABCD"))
        self.assertEqual({x.body for x in rest}, set("ABCDE"))
        self.assertEqual({x.skin for x in rest}, set(range(1, 13)))
        self.assertEqual({x.hair for x in rest}, set(range(1, 17)))
        self.assertTrue(all(ru.draw_kasumi(s, hood=True).describe() == self.EGG.describe() for s in range(20)))
        self.assertEqual((self.LOOK.gender(), self.EGG.gender()), (1, 2))

    def test_alternatives(self):
        alts = [["AID_イシュマール", "男"], ["AID_イシュマール", "女"], ["AID_イシュマール", "男", "頭部C"], ["AID_イシュマール", "男", "頭部A"],
                ["JID_闘士", "男", "AID_イシュマール", "頭部C", "3部"], ["AID_イシュマール", "髪色12", "Spine"], ["AID_イシュマール", "髪色1"],
                ["AID_イシュマール", "AID_イシュマールフード", "男", "頭部C", "!Spine", "!AID_フード無し"], ["AID_イシュマール", "男", "AID_イシュマール浮遊"],
                ["AID_イシュマール", "1部", "!イシュマール支援"], ["AID_イシュマール", "男", "身体E", "デモ"], ["AID_イシュマール", "男", "身体E", "拠点"],
                ["AID_オルヘル", "3部"], ["AID_イシュマール", "肌色7", "肌色7"]]
        self.assertEqual(ru.kasumi_alternatives(alts, self.LOOK),
                         [["AID_カスミ"], ["JID_闘士", "AID_カスミ", "3部"], ["AID_カスミ", "Spine"], ["AID_カスミ", "デモ"],
                          ["AID_カスミ", "拠点"]])
        hood = ru.kasumi_alternatives([["AID_イシュマール", "AID_イシュマールフード", "女", "頭部B", "!Spine", "!AID_フード無し"]], self.EGG)
        self.assertEqual(hood, [["AID_カスミ", "!Spine", "!AID_フード無し"]])
        self.assertEqual(ru.kasumi_alternatives([["AID_カイ"], ["AID_イシュマール", "頭部E"]], self.LOOK),
                         [["AID_カスミ", "頭部E"]])        # no hairstyle E: not a part, so kept as any other condition
        self.assertTrue(ru.is_avatar_term("髪色16") and ru.is_avatar_term("身体E") and not ru.is_avatar_term("頭部E"))

    def test_mapping(self):
        p = ru.Person(person(ROWS + [KON]))
        pool = ["Kai", "Tro", "Lin", "Dag"]
        k = ru.kasumi_character(self.LOOK)
        seen = set()
        for seed in range(100):
            m, warn = ru.make_mapping(p, seed, pool, kasumi=k)
            self.assertEqual(warn, [])
            self.assertEqual([o.ident for o, _ in m], ["Kai", "Tro", "Lin", "Dag", "Kon"])   # Kumos: a slot only
            self.assertEqual(sorted(r.ident for _, r in m), sorted(pool + ["Ksm"]))         # Kasumi: a look only
            seen.add(next(o.ident for o, r in m if r is k))
        self.assertEqual(seen, {"Kai", "Tro", "Lin", "Dag", "Kon"})
        self.assertEqual([(o.ident, r.ident) for o, r in ru.make_mapping(p, 1, pool)[0]].count(("Kon", "Kai")), 0)   # off: no Kumos
        m, warn = ru.make_mapping(p, 1, pool, ru.parse_force("Lin=Kasumi, Ksm=Kai")[0], k)
        self.assertEqual((m[0][0].ident, m[0][1]), ("Lin", k))
        self.assertEqual(warn, ["randomize_force: Ksm=Kai: no character named Ksm"])           # it has no slot
        self.assertEqual(ru.make_mapping(p, 1, pool, ru.parse_force("Lin=Ksm")[0])[1],
                         ["randomize_force: Lin=Ksm: no character named Ksm"])                 # off: no such character
        self.assertEqual(ru.mapping_lines(1, [(m[0][0], k)])[1], "randomizer:   Lin(Lindo) -> Ksm(Kasumi)")

    def test_person(self):
        data = person(ROWS)
        p = ru.Person(data)
        k = ru.kasumi_character(self.EGG)
        out, changed, _ = ru.apply(data, [(p.find("Lin"), k), (p.find("Kai"), p.find("Tro"))])
        t = TestApply()
        rows = lambda _, d, key: TestApply.rec(t, d, key)  # noqa: E731
        self.assertEqual({x: rows(t, out, "PID_リンド").get(x) for x in ("名前", "アセットフラグ", "性別")},
                         {"名前": "MPID_Kasumi", "アセットフラグ": "AID_カスミ", "性別": 2})
        story = rows(t, out, "PID_SC00_05_リンド")
        self.assertEqual((story["名前"], story["アセット流用"], story["性別"]), ("MPID_Kasumi", "PID_リンド", 2))
        self.assertEqual(rows(t, out, "PID_カイ")["アセットフラグ"], "AID_トロイア")
        self.assertEqual(rows(t, out, "PID_トロイア")["アセットフラグ"], "AID_トロイア")       # untouched
        self.assertEqual(len(out), len(data) + len("MPID_Kasumi\0AID_カスミ\0".encode("utf-8")))   # only the two strings
        self.assertEqual(ru.Person(out).find("Lin").aid, "AID_カスミ")

    ENTRIES = [
        ("e0", [["AID_ブレザ", "SID_冥界兵"]], "ブレザ冥闘士", ["SID_冥界兵"]),                        # the template
        ("e1", [["JID_闘士", "女", "AID_イシュマール", "頭部B"], ["JID_闘士", "女", "AID_オルヘル"]], None, []),
        ("e2", [["AID_イシュマール", "女"]], None, []),
        ("e3", [["AID_イシュマール", "女", "頭部B"]], "イシュマール女B", []),
        ("e4", [["AID_イシュマール", "AID_イシュマールフード", "女", "頭部B", "!Spine", "!AID_フード無し"]], "イシュマール女Bフード", []),
        ("e5", [["AID_イシュマール", "男", "頭部C"]], "イシュマール男C", []),
        ("e6", [["AID_リンド"]], "リンド", []),
        ("e7", [["AID_リンド", "SID_冥界兵"], ["AID_リンド", "AID_リンド冥将"]], "リンド冥闘士", ["SID_冥界兵"]),
        ("e8", [["AID_コン・ムー"]], "コン・ムー", []),                                          # Kumos: living only
    ]
    FACES = [("FID_ブレザ冥闘士", "Bre0", "168", 1), ("FID_リンド", "Lin0", "030", 0), ("FID_リンド冥闘士", "Lin9", "030", 1),
             ("FID_コン・ムー", "Kon0", "155", 0)] + [
        ("FID_イシュマール%s%s" % (s, h), "Ism0" + ("M" if s == "男" else "F"), "%s_%sHoodOff" % ("015" if s == "男" else "016", h), 0)
        for s in "男女" for h in "ABCD"] + [("FID_イシュマール女Bフード", "Ism4F", "016_BHoodOn", 0)]
    read = TestUnderworldForms.read

    def test_assets(self):
        data = asset_table(self.ENTRIES, self.FACES)
        for look, face in ((self.EGG, ("Ism0F", "900_BHoodOn")), (ru.KasumiLook("男", "C", "A", 1, 1), ("Ism0M", "900_CHoodOff"))):
            out, changes = ru.apply_assets(data, [], [], {"AID_リンド": "AID_カスミ"}, [], kasumi=look)
            b, entries, faces = self.read(out)
            # its own portraits: copies of the Eshmel one its draw names, in its own rig, with its sex's voice
            self.assertEqual(faces["FID_カスミ"], face + (0,))
            self.assertEqual(faces["FID_カスミ冥闘士"], face + (1,))
            self.assertEqual(faces["FID_イシュマール女Bフード"], ("Ism4F", "016_BHoodOn", 0))          # Eshmel's stay as they are
            self.assertFalse([k for k in faces if k.startswith("FID_イシュマール") and k.endswith("冥闘士")])
            self.assertEqual({n for _, n in b.labels if n.startswith("FID_")}, set(faces))
            # Eshmel's alternatives stay, the second Eshmel's follow them
            self.assertEqual(entries[1][0], self.ENTRIES[1][1] + ([["JID_闘士", "AID_カスミ"]] if look.hood else []))
            self.assertEqual(entries[2][0], self.ENTRIES[2][1] + ([["AID_カスミ"]] if look.hood else []))
            self.assertEqual(entries[4][0], self.ENTRIES[4][1] + ([["AID_カスミ", "!Spine", "!AID_フード無し"]] if look.hood else []))
            self.assertEqual(entries[5][0], self.ENTRIES[5][1] + ([] if look.hood else [["AID_カスミ"]]))
            self.assertEqual([x[1:] for x in entries[:len(self.ENTRIES)]], [x[2:] for x in self.ENTRIES])   # looks, keywords as were
            self.assertEqual(faces["FID_コン・ムー冥闘士"], ("Kon0", "155", 1))                   # Kumos: an Underworld form
            self.assertEqual(entries[len(self.ENTRIES):], [
                ([["AID_コン・ムー", "SID_冥界兵"]], "コン・ムー冥闘士", ["SID_冥界兵"]),
                ([["AID_カスミ"]], "カスミ", []),                                                # after Eshmel's: her own portrait
                ([["AID_カスミ", "SID_冥界兵"]], "カスミ冥闘士", ["SID_冥界兵"]),
                ([["AID_カスミ", "AID_リンド冥将"]], "カスミ冥闘士", ["SID_冥界兵"])])            # step 4, as for anyone
            self.assertEqual(entries[7][0], [["AID_リンド", "SID_冥界兵"], ["AID_リンド", "SID_冥界兵"]])
            self.assertEqual(ge.Editor(out).build(), out)                                       # a well-formed container
        # Kasumi plays a lord: the portrait the cutscene names shows hers, face and all
        lord = asset_table(self.ENTRIES + [("e9", [["AID_カイ"]], "カイ", []), ("e10", [["AID_カイ", "SID_冥界兵"]], "カイ冥闘士", ["SID_冥界兵"])],
                           self.FACES + [("FID_カイ", "Kai0", "017", 0), ("FID_カイ冥闘士", "Kai9", "017", 1)])
        _, _, faces = self.read(ru.apply_assets(lord, [], [], {"AID_カイ": "AID_カスミ"}, ["カイ冥闘士"], kasumi=self.EGG)[0])
        self.assertEqual(faces["FID_カイ冥闘士"], ("Ism0F", "900_BHoodOn", 1))
        self.assertEqual(faces["FID_カイ冥闘士本人"], ("Kai9", "017", 1))
        self.assertEqual(ru.apply_assets(data, [], [], None, []), (data, []))                  # off: nothing

    def test_colours(self):
        """kasumi_colours: from the "& Spine" colour entries of the look's sex, skin and hair; later entries win."""
        b = Builder()
        conds = []
        rows = [("c0", [["AID_イシュマール", "女", "肌色2", "Spine"]], "11223344", "55667788", "00000000"),
                ("c1", [["AID_イシュマール", "女", "肌色2"]], "aaaaaaaa", "aaaaaaaa", "00000000"),          # not the portrait's
                ("c2", [["AID_イシュマール", "男", "肌色2", "Spine"]], "bbbbbbbb", "bbbbbbbb", "00000000"),  # the other sex
                ("c3", [["AID_イシュマール", "髪色3", "Spine"]], "00000000", "00000000", "9bc0d1ff"),
                ("c4", [["AID_イシュマール", "髪色3", "Spine"]], "00000000", "00000000", "4a505cff")]       # later wins

        def write(at, row):
            conds.append((at + 0x20, row[1]))
            for k, off in enumerate((0x30, 0x34, 0x38)):
                b.data[at + off:at + off + 4] = bytes.fromhex(row[2 + k])
        table(b, "アセットテーブル", [("条件", "string[,]", 16, 0x20), ("肌カラー", "color", 4, 0x30), ("口紅色", "color", 4, 0x34),
                                     ("髪カラー", "color", 4, 0x38)], 0x40, rows, write)
        for at, alts in conds:
            struct.pack_into("<I", b.data, at, len(alts) | 16 << 16)
            arr = len(b.data)
            b.data += bytes(16 * len(alts))
            struct.pack_into("<Q", b.data, at + 8, arr)
            b.rel.append(at + 8)
            for i, alt in enumerate(alts):
                string_list(b, arr + 16 * i, alt)
        b.labels.sort()
        got = ru.kasumi_colours(b.build(), ru.KasumiLook("女", "B", "B", 2, 3))
        self.assertEqual(got, [bytes.fromhex("11223344"), bytes.fromhex("55667788"), bytes.fromhex("4a505cff")])
        self.assertEqual(ru.kasumi_colours(b.build(), ru.KasumiLook("女", "B", "B", 5, 9)), [None, None, None])

    def test_rig_files(self):
        key = ru.ATLAS_KEY
        atlas = bytes(x ^ y for x, y in zip(b"016.png\nsize: 4096", key * 3))
        out, _ = ru.kasumi_rig_file("atlas.zst", atlas, "016")
        self.assertEqual(bytes(x ^ y for x, y in zip(out, key * 3)), b"900.png\nsize: 4096")
        self.assertEqual(ru.kasumi_rig_file("atlas.zst", atlas, "015")[0], None)                  # another rig's
        bntx = b"BNTX....\x03\x00016\x00\x00_DIC"
        self.assertEqual(ru.kasumi_rig_file("bntx.zst", bntx, "016")[0], b"BNTX....\x03\x00900\x00\x00_DIC")
        self.assertEqual(ru.kasumi_rig_file("bntx.zst", bntx * 2, "016")[0], None)               # not there once
        spdb = b"\x0d\x00\x00\x00016/016.atlas\x0c\x00\x00\x00016/016.skel"
        self.assertEqual(ru.kasumi_rig_file("spdb", spdb, "016")[0], b"\x0d\x00\x00\x00900/900.atlas\x0c\x00\x00\x00900/900.skel")
        skel = skeleton(["Body02_Skin", "A_TopHair_Hair", "A_TopHair_Highlight", "Mouth_n00_Lip", "00_Eye"])
        colours = [bytes.fromhex("dcba9eff"), bytes.fromhex("da9681ff"), bytes.fromhex("9bc0d1ff")]
        out, note = ru.kasumi_rig_file("skel.zst", skel, "016", colours)
        slots = dict(ru._skel_slot_colours(out))
        self.assertEqual({n: out[at:at + 8].hex() for n, at in slots.items()},
                         {"Body02_Skin": "dcba9effffffffff", "A_TopHair_Hair": "9bc0d1ffffffffff", "A_TopHair_Highlight": "ffffffffffffffff",
                          "Mouth_n00_Lip": "da9681ffffffffff", "00_Eye": "ffffffffffffffff"})
        self.assertEqual(len(out), len(skel))
        self.assertEqual(note, "slots coloured: skin 1 (dcba9eff), lip 1 (da9681ff), hair 1 (9bc0d1ff)")
        self.assertEqual(ru.kasumi_rig_file("skel.zst", skeleton(["x_Hair"], version="4.2.01"), "016", colours)[0], None)

    def test_look_helpers(self):
        self.assertEqual((self.EGG.portrait(), self.EGG.voice(), self.EGG.rig()), ("イシュマール女Bフード", "Ism0F", "016"))
        self.assertEqual((self.LOOK.portrait(), self.LOOK.voice(), self.LOOK.rig()), ("イシュマール男C", "Ism0M", "015"))


if __name__ == "__main__":
    unittest.main()
