# SPDX-License-Identifier: GPL-3.0-or-later
"""The unit randomizer, in Python: the test oracle for runtime/games/ObeliskChamber/ObeliskChamber_randomizer.cpp ([fefw] randomize_units).

    python -I tools/gamedata/randomize_units.py <romfs> --seed 1234 [--force Anh=Tro,Lin=Kou] [-o Person.bin] [--show SC00_04 SC00_05]

It prints the same mapping lines and checksum as the runtime's log, so a run of the game can be checked against it.

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

# The pool, by 識別子, in Person.bin's order: the characters shuffled among themselves. Every one has a model and a portrait of its own
# (UI/Face/<n> for its head HD_<n>). Of the 140 with a human model: the 79 placed on some map, then 30 never placed: 14 that look
# playable (Mic Nin Yan Noc Sop Cat Ned Zac Maj Cle Kir Iio Mys Guj), the 8 gods (Sot Fot Juf Ish Jun Smi Cla Car) and 8
# NPCs with a unique look (Gra Nom Gan Kar Ver Gab Orp Han). Left out: Eshmel (Ism, the Prologue / Part 3 protagonist); Balor (Bal) and
# Oktar (Oct), whose models are not in AssetTable.bin (Balor only ever appears as a dragon); and the 28 built from stock heads (HD_Std*,
# HD_Eld*, HD_Glm*, ...) with the generic portrait: placed on maps, Har Ger Hoo Ara Ban Jan Win Dra Yas; never placed, Lan Tul Sua Lle
# Miw Has Kon Nas Hed Get Zon Kyo Kat Tsi Sam Cou Ten Rox Crn. "Maｄ" (Mandalan) really ends in a full-width ｄ.
POOL = """
    Kai Tia Pet Hur Bel Gat Max Gol Dan Dit Fab Esm Mic Orh Die Lor Set Nin Seo Bon Tob Lil Lys Tal Urs Lou Sim Fia Led Buc
    Sci Oly Moo Anh Naz Sar Dai Hal Kou Tro Sot Fot Juf Dag Lyr Ish Mna Sol Jun Mar Smi Cla Car Aur Guj Yan Iio Pep Noc Sop
    Cat Ned Zac Maj Ven Ini Jas Ale Nuz Cen Asw Cle Mys Nat Hri Kir Ann Gra Kra Tah Cas Nom Lon Cir Che Shi Hyo Gan Gil Zen
    Znz Kar Bou Day Baa Bjo Bol Lec Maｄ Hno Lin Bre Qui Coh Mhm Ver Gab Orp Han
""".split()
# The most shuffles make_mapping() tries for a draw where nobody keeps their own look.
SHUFFLE_TRIES = 1000
# How many pairs each log line names: the runtime's log cuts a line at 4096 bytes.
PAIRS_PER_LINE = 10

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


def make_mapping(person, seed, pool=POOL, force=()):
    """[(original Character, replacement Character)] and warnings. Forced pairs first, in the order given. Then the pool characters not
    forced (originals) and those not yet used as a replacement (candidates), each in Person.bin's order: the candidates are shuffled
    (Fisher-Yates, from the last), and original i takes candidate i. Nobody keeps their own look: while an original drew itself, the
    candidates are shuffled again, from where they are, with the same generator (about 2.7 shuffles for the full pool). Only a lone
    candidate, or SHUFFLE_TRIES shuffles that all failed (which a real pool cannot do), can leave someone as themselves."""
    warn = []
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
    for ident in pool:
        if ident not in person.by_ident:
            warn.append("pool: no character %s in Person.bin" % ident)
    pool = set(pool)
    origs = [c for c in person.chars if c.ident in pool and c.ident not in done]
    cands = [c for c in person.chars if c.ident in pool and c.ident not in used]
    rng = SplitMix64(seed)
    n = min(len(origs), len(cands))
    for _ in range(SHUFFLE_TRIES):
        for i in range(len(cands) - 1, 0, -1):
            j = rng.next() % (i + 1)
            cands[i], cands[j] = cands[j], cands[i]
        if len(cands) < 2 or all(origs[i] is not cands[i] for i in range(n)):
            break
    out += zip(origs[:n], cands[:n])
    for c in origs[n:]:
        warn.append("pool: no replacement left for %s" % c.ident)
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


def mapping_lines(seed, mapping):
    """The log's mapping: a line with the seed and the number of pairs, then the pairs, PAIRS_PER_LINE to a line."""
    pairs = ["%s -> %s" % (o.label(), r.label()) for o, r in mapping]
    return ["randomizer: seed %d: %d pairs" % (seed, len(pairs))] + [
        "randomizer:   " + ", ".join(pairs[i:i + PAIRS_PER_LINE]) for i in range(0, len(pairs), PAIRS_PER_LINE)]


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
    ap.add_argument("-o", help="write the edited Person.bin here")
    ap.add_argument("--show", nargs="*", default=[], help="Dispos files (SC00_05) whose placements to list with their new look")
    ap.add_argument("-v", action="store_true", help="list every record changed")
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding="utf-8")
    with open(os.path.join(a.romfs, "GameData", "Person.bin"), "rb") as fh:
        data = fh.read()
    person = Person(data)
    force, warn = parse_force(a.force)
    mapping, warn2 = make_mapping(person, a.seed, POOL, force)
    for w in warn + warn2:
        print("warning: " + w, file=sys.stderr)
    edited, changed, skipped = apply(data, mapping)
    print("\n".join(mapping_lines(a.seed, mapping)))
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
