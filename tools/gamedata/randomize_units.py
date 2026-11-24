# SPDX-License-Identifier: GPL-3.0-or-later
"""The unit randomizer, in Python: the test oracle for runtime/games/ObeliskChamber/ObeliskChamber_randomizer.cpp ([fefw] randomize_units).

    python -I tools/gamedata/randomize_units.py <romfs> --seed 1234 [--force Anh=Tro,Lin=Kou] [-o Person.bin] [--show SC00_04 SC00_05]

It prints the same mapping line and checksum as the runtime's log, so a run of the game can be checked against it.

How a swap works. A character is a 識別子 (identifier, e.g. Kai) in GameData/Person.bin. Each identifier has a base record (PID_カイ: the
one with its own model, AID_カイ) and a record for each story appearance (PID_SC00_05_カイ, which borrows the base's model through
アセット流用). Placements (GameData/Dispos/*.bin), event scripts and AI arguments all name those PIDs, so nothing outside Person.bin has
to change: for every record of an identifier being replaced, the record is made to look like the replacement's base record:

    名前            the name (MPID_*)                       <- the replacement's
    アセットフラグ   the model (AID_*), base records          <- the replacement's AID
    アセット流用     the model, borrowed, story records       <- the character's own base PID (which now has the replacement's AID)
    性別            gender (the body's animations)           <- the replacement's

A story record never borrows the replacement's base: when the replacement is itself replaced, that base looks like someone else.

Every edit replaces a pointer with another that is already in the file, so the file keeps its size and layout. The PID, class, stats,
items, skills, affiliation and 識別子 (which the game uses to tell the routes apart) stay the slot's own. Records with no model of their
own and none borrowed are dragon forms (JID_ダグザ竜: the class is the model) and are never touched, so a replaced Dagda keeps his dragon.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata_bin  # noqa: E402

# First milestone: the four lords (Cai, Dietrich, Theodora, Leda), Hong Hua, Troy and Lind.
TEST_POOL = ["Kai", "Dit", "Seo", "Led", "Kou", "Tro", "Lin"]
# Never a replacement: Eshmel (the Prologue / Part 3 protagonist), left out for now.
EXCLUDED = {"Ism"}

F_NAME, F_IDENT, F_GENDER, F_REUSE, F_AID = "名前", "識別子", "性別", "アセット流用", "アセットフラグ"
MASK64 = (1 << 64) - 1


class SplitMix64:
    def __init__(self, seed):
        self.s = seed & MASK64

    def next(self):
        self.s = (self.s + 0x9E3779B97F4A7C15) & MASK64
        z = self.s
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
        return z ^ (z >> 31)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & MASK64
    return h


class Character:
    """An identifier and its base record: the values a replacement hands over."""

    def __init__(self, ident, rec, key, name_ptr, aid_ptr, key_ptr, gender, mpid):
        self.ident, self.rec, self.key = ident, rec, key
        self.name_ptr, self.aid_ptr, self.key_ptr, self.gender, self.mpid = name_ptr, aid_ptr, key_ptr, gender, mpid

    def label(self):
        """How the log names a character: identifier and name ID, e.g. Kai(Kai), Tro(Troja)."""
        return "%s(%s)" % (self.ident, (self.mpid or "").replace("MPID_", ""))


class Person:
    def __init__(self, data):
        self.b = gamedata_bin.GameDataBin(bytes(data))
        self.t = self.b.table("個人")
        self.f = {n: self.t.field(n).off for n in (F_NAME, F_IDENT, F_GENDER, F_REUSE, F_AID)}
        self.chars = []          # file order of the base records
        self.by_ident = {}
        for rec in self.t.records():
            ident = self.b.string(rec + self.f[F_IDENT])
            if not ident or ident in self.by_ident:
                continue
            if self.b.ptr(rec + self.f[F_AID]) is None or self.b.ptr(rec + self.f[F_REUSE]) is not None:
                continue
            c = Character(ident, rec, self.b.key(rec), self.b.ptr(rec + self.f[F_NAME]), self.b.ptr(rec + self.f[F_AID]),
                          self.b.ptr(rec + 8), self.b.d[gamedata_bin.HEADER + rec + self.f[F_GENDER]], self.b.string(rec + self.f[F_NAME]))
            self.chars.append(c)
            self.by_ident[ident] = c

    def find(self, name):
        """A character by identifier (Kai), name ID with or without MPID_ (Troja), or PID with or without PID_; any case."""
        n = name.strip().lower().replace(" ", "")
        for c in self.chars:
            keys = {c.ident.lower(), (c.mpid or "").lower(), (c.mpid or "").lower().replace("mpid_", ""), c.key.lower(),
                    c.key.lower().replace("pid_", "")}
            if n in keys:
                return c
        return None


def parse_force(text):
    """[(original, replacement)] and warnings; an entry that is not original=replacement is skipped, as the runtime does."""
    pairs, warn = [], []
    for part in (text or "").split(","):
        if not part.strip():
            continue
        if "=" not in part:
            warn.append('randomize_force: "%s" is not original=replacement' % part.strip())
            continue
        a, b = part.split("=", 1)
        pairs.append((a.strip(), b.strip()))
    return pairs, warn


def make_mapping(person, seed, pool=TEST_POOL, force=()):
    """[(original Character, replacement Character)] and warnings. Forced pairs first (in the order given), then each pool identifier not
    forced, in pool order, draws a replacement from every eligible character not yet used: no two originals share a replacement."""
    warn = []
    cands = [c for c in person.chars if c.ident not in EXCLUDED]
    used, done, out = set(), set(), []
    for a, b in force:
        ca, cb = person.find(a), person.find(b)
        if ca is None or cb is None:
            warn.append("randomize_force: %s=%s: no character named %s" % (a, b, a if ca is None else b))
            continue
        if ca.ident in done or cb.ident in used:
            warn.append("randomize_force: %s=%s: %s already has a replacement or %s is already one" % (a, b, a, b))
            continue
        out.append((ca, cb))
        done.add(ca.ident)
        used.add(cb.ident)
    rng = SplitMix64(seed)
    for ident in pool:
        c = person.by_ident.get(ident)
        if c is None:
            warn.append("pool: no character %s in Person.bin" % ident)
            continue
        if ident in done:
            continue
        free = [x for x in cands if x.ident not in used]
        if not free:
            warn.append("pool: no replacement left for %s" % ident)
            continue
        r = free[rng.next() % len(free)]
        out.append((c, r))
        done.add(ident)
        used.add(r.ident)
    return out, warn


def apply(data, mapping):
    """The edited Person.bin and a report: [(record key, what changed)], [(record key) skipped: no model of its own (dragon forms)]."""
    p = Person(data)
    b, f = p.b, p.f
    out = bytearray(data)
    swap = {o.ident: (o, r) for o, r in mapping if o.ident != r.ident}
    changed, skipped = [], []

    def put_ptr(off, value):
        assert off in b.ptrs, "0x%x is not a pointer field" % off
        struct.pack_into("<Q", out, gamedata_bin.HEADER + off, value)

    for rec in p.t.records():
        pair = swap.get(b.string(rec + f[F_IDENT]))
        if pair is None:
            continue
        o, r = pair
        aid, reuse = b.ptr(rec + f[F_AID]), b.ptr(rec + f[F_REUSE])
        if aid is None and reuse is None:
            skipped.append(b.key(rec))
            continue
        what = []
        if b.ptr(rec + f[F_NAME]) is not None and r.name_ptr is not None:
            put_ptr(rec + f[F_NAME], r.name_ptr)
            what.append("name")
        if aid is not None:
            put_ptr(rec + f[F_AID], r.aid_ptr)
            what.append("model")
        # A borrowed model points at the character's own base record, which now carries the replacement's model. Never at the
        # replacement's base: that one is edited too when the replacement is itself replaced (Lin -> Tro, Tro -> Mhm made Lind's
        # slot look like Mahadu). Only a record that borrowed another character's base changes here.
        if reuse is not None and reuse != o.key_ptr:
            put_ptr(rec + f[F_REUSE], o.key_ptr)
            what.append("borrowed model")
        out[gamedata_bin.HEADER + rec + f[F_GENDER]] = r.gender
        changed.append((b.key(rec), what))
    return bytes(out), changed, skipped


def mapping_line(seed, mapping):
    return "randomizer: seed %d: %s" % (seed, ", ".join("%s -> %s" % (o.label(), r.label()) for o, r in mapping))


def summary_line(changed, skipped, edited):
    return "randomizer: Person.bin: %d records changed, %d dragon forms left as they are (fnv1a64 %016x)" % (
        len(changed), len(skipped), fnv1a64(edited))


def show_placements(romfs, names, person_before, edited):
    after = Person(edited)
    for n in names:
        path = os.path.join(romfs, "GameData", "Dispos", n if n.endswith(".bin") else n + ".bin")
        with open(path, "rb") as fh:
            d = gamedata_bin.GameDataBin(fh.read())
        t = d.table("配置")
        off = t.field("個人").off
        recs = {after.b.key(r): r for r in after.t.records()}
        for group, rs in t.groups:
            for r in rs:
                pid = d.string(r + off)
                rec = recs.get(pid)
                if rec is None:
                    continue
                was = person_before.b.string(rec + person_before.f[F_NAME])
                now = after.b.string(rec + after.f[F_NAME])
                if was != now:
                    print("%s %s: %s: %s -> %s" % (n, group, pid, was, now))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("romfs", help="the RomFS folder (with GameData/Person.bin)")
    ap.add_argument("--seed", type=int, required=True, help="the [fefw] randomize_seed the game logged")
    ap.add_argument("--force", default="", help="as [fefw] randomize_force, e.g. Anh=Tro,Lin=Kou")
    ap.add_argument("--all", action="store_true", help="every eligible character in the pool (the full design), not the test pool")
    ap.add_argument("-o", help="write the edited Person.bin here")
    ap.add_argument("--show", nargs="*", default=[], help="Dispos files (SC00_05) whose placements to list with their new look")
    ap.add_argument("-v", action="store_true", help="list every record changed")
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8")
    with open(os.path.join(a.romfs, "GameData", "Person.bin"), "rb") as fh:
        data = fh.read()
    person = Person(data)
    pool = [c.ident for c in person.chars if c.ident not in EXCLUDED] if a.all else TEST_POOL
    force, warn = parse_force(a.force)
    mapping, warn2 = make_mapping(person, a.seed, pool, force)
    for w in warn + warn2:
        print("warning: " + w, file=sys.stderr)
    edited, changed, skipped = apply(data, mapping)
    print(mapping_line(a.seed, mapping))
    print(summary_line(changed, skipped, edited))
    if a.v:
        for key, what in changed:
            print("  %s: %s" % (key, ", ".join(what)))
        for key in skipped:
            print("  %s: left as it is (no model of its own)" % key)
    if a.show:
        show_placements(a.romfs, a.show, person, edited)
    if a.o:
        with open(a.o, "wb") as fh:
            fh.write(edited)
        print("wrote %s (sha256 %s)" % (a.o, hashlib.sha256(edited).hexdigest()))


if __name__ == "__main__":
    main()
