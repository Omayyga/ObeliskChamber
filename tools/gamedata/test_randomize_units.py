# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for gamedata_bin.py and randomize_units.py on hand-built files.  python -I tools/gamedata/test_randomize_units.py"""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata_bin as gb  # noqa: E402
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

    def test_one_to_one_and_seeded(self):
        for seed in range(50):
            m, warn = ru.make_mapping(self.p, seed, ["Kai", "Tro", "Lin"])
            self.assertEqual(warn, [])
            self.assertEqual([o.ident for o, _ in m], ["Kai", "Tro", "Lin"])
            reps = [r.ident for _, r in m]
            self.assertEqual(len(set(reps)), 3)
            self.assertNotIn("Ism", reps)
            self.assertEqual(reps, [r.ident for _, r in ru.make_mapping(self.p, seed, ["Kai", "Tro", "Lin"])[0]])
        seen = {tuple(r.ident for _, r in ru.make_mapping(self.p, s, ["Kai", "Tro", "Lin"])[0]) for s in range(50)}
        self.assertGreater(len(seen), 5)

    def test_force(self):
        m, warn = ru.make_mapping(self.p, 7, ["Kai", "Tro", "Lin"], ru.parse_force("Lindo=Troy, Kai=Dagda")[0])
        self.assertEqual(len(warn), 1)                       # "Troy" is not a name the data knows (Troja is)
        m, warn = ru.make_mapping(self.p, 7, ["Kai", "Tro", "Lin"], ru.parse_force("Lindo=Troja, Kai=Dag, Tro=Dag")[0])
        self.assertEqual(len(warn), 1)                       # Dag is taken
        pairs = [(o.ident, r.ident) for o, r in m]
        self.assertEqual(pairs[:2], [("Lin", "Tro"), ("Kai", "Dag")])
        self.assertEqual(len(pairs), 3)
        self.assertNotIn(pairs[2][1], ("Tro", "Dag", "Ism"))
        self.assertEqual(ru.parse_force(" Kai , Lin = Tro,"), ([("Lin", "Tro")], ['randomize_force: "Kai" is not original=replacement']))

    def test_force_outside_pool(self):
        m, _ = ru.make_mapping(self.p, 1, [], ru.parse_force("Dag=Kai")[0])
        self.assertEqual([(o.ident, r.ident) for o, r in m], [("Dag", "Kai")])


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

    def test_swap(self):
        out, changed, skipped = ru.apply(self.data, [self.pair("Kai", "Tro"), self.pair("Tro", "Lin")])
        self.assertEqual(len(out), len(self.data))
        kai = self.rec(out, "PID_カイ")
        self.assertEqual((kai["名前"], kai["アセットフラグ"], kai["性別"], kai["識別子"], kai["兵種"]),
                         ("MPID_Troja", "AID_トロイア", 2, "Kai", "JID_無職"))
        sc = self.rec(out, "PID_SC00_05_カイ")
        self.assertEqual((sc["名前"], sc["アセット流用"], sc["兵種"]), ("MPID_Troja", "PID_トロイア", "JID_オリハルディア"))
        tro = self.rec(out, "PID_トロイア")       # Troy takes Lind's look; Cai still got Troy's own
        self.assertEqual((tro["名前"], tro["アセットフラグ"], tro["性別"]), ("MPID_Lindo", "AID_リンド", 1))
        self.assertEqual(self.rec(out, "PID_リンド")["名前"], "MPID_Lindo")   # not in the mapping
        self.assertEqual([k for k, _ in changed], ["PID_カイ", "PID_トロイア", "PID_SC00_05_カイ"])
        self.assertEqual(skipped, [])

    def test_dragon_left_alone(self):
        out, changed, skipped = ru.apply(self.data, [self.pair("Dag", "Kai")])
        self.assertEqual(skipped, ["PID_SC00_06_ダグザ竜"])
        dragon = self.rec(out, "PID_SC00_06_ダグザ竜")
        self.assertEqual((dragon["名前"], dragon["性別"]), ("MPID_Dagda", 0))
        human = self.rec(out, "PID_SC00_06_ダグザ")
        self.assertEqual((human["名前"], human["アセット流用"], human["アセットフラグ"]), ("MPID_Kai", "PID_カイ", "AID_カイ"))
        self.assertEqual(dict(changed)["PID_SC00_06_ダグザ"], ["name", "model", "borrowed model"])

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
        self.assertEqual(ru.mapping_line(5, m), "randomizer: seed 5: Kai(Kai) -> Tro(Troja)")
        out, changed, skipped = ru.apply(self.data, m)
        self.assertRegex(ru.summary_line(changed, skipped, out), r"^randomizer: Person.bin: 2 records changed, 0 dragon forms left as "
                                                                 r"they are \(fnv1a64 [0-9a-f]{16}\)$")


if __name__ == "__main__":
    unittest.main()
