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

    def test_left_alone(self):
        for data in (skeleton(["00_Eye", "Eye_W"]),            # already has the names the effect looks for
                     skeleton(["Eye", "00_eye_w", "Eye_W"]),   # a new name is taken
                     skeleton(["Eye"], version="4.2.01"),      # another format
                     skeleton(["Eye"])[:40]):                  # cut short
            self.assertEqual(ru.rename_eye_slots(data), (data, []))


if __name__ == "__main__":
    unittest.main()
