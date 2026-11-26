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
import gamedata_edit  # noqa: E402

# The pool, by 識別子, in Person.bin's order: the characters shuffled among themselves. Every one has a model and a portrait of its own: a
# portrait (フェイス in AssetTable.bin 容姿情報, UI/Face/<n>) numbered under 300; 3xx are the generic class portraits. Of the 140 with a human
# model: the 82 placed on some map (Arago, Vandahl and Drake have theirs on their Underworld entries, FID_*冥闘士), then 31 never placed:
# 14 that look playable (Mic Nin Yan Noc Sop Cat Ned Zac Maj Cle Kir Iio Mys Guj), the 8 gods (Sot Fot Juf Ish Jun Smi Cla Car), 8 NPCs
# with a unique look (Gra Nom Gan Kar Ver Gab Orp Han) and Katarina (Kat, no English name). Left out: Eshmel (Ism, the Prologue / Part 3
# protagonist); Balor (Bal, only ever a dragon, no face) and Oktar (Oct), whose models are not in AssetTable.bin; the generic-looking:
# placed on maps, Har Ger Hoo Jan Win Yas; never placed, Lan Tul Sua Lle Miw Has Kon Nas Hed Get Zon Kyo Tsi Sam Cou Ten Crn, and Rox
# (Troy's portrait). Randa (Lan) and Kumos (Kon) have portraits but stock models: left out by choice. "Maｄ" (Mandalan) really ends in a
# full-width ｄ.
POOL = """
    Kai Tia Pet Hur Bel Gat Max Gol Dan Dit Fab Esm Mic Orh Die Lor Set Nin Seo Bon Tob Lil Lys Tal Urs Lou Sim Fia Led Buc
    Sci Oly Moo Anh Naz Sar Dai Hal Kou Tro Sot Fot Juf Dag Lyr Ish Mna Sol Jun Mar Smi Cla Car Aur Guj Yan Iio Pep Noc Sop
    Cat Ned Zac Maj Ven Ini Jas Ale Nuz Cen Asw Cle Mys Nat Hri Kir Ann Gra Kra Tah Cas Nom Lon Cir Che Shi Hyo Gan Gil Zen
    Znz Kar Bou Day Baa Bjo Bol Lec Maｄ Ara Hno Lin Ban Bre Qui Dra Coh Mhm Ver Gab Orp Kat Han
""".split()
# AssetTable.bin: Underworld forms. The engine gives units of the Underworld Army the keyword SID_冥界兵; a look entry whose condition is
# "AID_x & SID_冥界兵" then gives them their Underworld look (the 冥闘士 portrait entry: tinted portrait, Underworld voice) and model.
# The pool characters whose plain look is Underworld only: they get a living portrait entry for their plain look, and keep the
# Underworld one for Underworld slots (as Breza already had). Breza, King Arago, Hanno, Vandahl, Drake, Cohen, Mahadu:
LIVING_LOOKS = ["ブレザ冥闘士", "アラゴ大王冥闘士", "ハンノ冥闘士", "ヴァンダル冥闘士", "ドレイク冥闘士", "コーエン冥闘士", "マハマドゥ冥闘士"]
UNDERWORLD = "SID_冥界兵"
# The pool characters with no Underworld form at all, by asset key (AID_<x>), in Person.bin's order: each gets one, a tinted copy of
# its plain look's portrait entry and an "AID_x & SID_冥界兵" look entry, so that in an Underworld slot it looks the part.
UNDERWORLD_FORMS = """
    ピーテル ウルタンド マクシム ゴライアス ダンテ エスメラルダ ミカエラ オルヘル ディエゴ ロレッタ セテス ニネ リリアン ライサンダー ルルディヤー シモン フィアナ シロッコ オリンピア ムウ ナジャ ダイトウ ハルヴィン コウカ
    トロイア ソティス フォトナ ユ・ファス ライラー イシエト ソレル ジュラ マーズ スミルノス クレール カーラ アウロラ グザラン ヨーカイ イオ ペペ ノクチュラ ソフィア カターニャ ヌディーヌ ザーコネ マジーデ ベンディッツ
    イニオニ ジャスミン アレキサンドラ ヌッツォ セントリオン アスワン クレオブロス ミュソン ネイサン キリーク キロイカ アンナ グラディウス クラプカ タホウニア カストール ノムヴェデ ロン ガンシン ゼノン カール ビジョウ
    ベラフィ ガブリエル オルフェウス カタリナ ハンニバル
""".split()
# Underworld looks the cutscenes name. Prologue Ch. 2's cutscene and death quotes (Msg/<language>/0_ISM_Battle.msbt, Die.msbt) give
# the four lords' lines an explicit portrait: one of these 容姿 names, in each line's attributes (ATR1, the same in every language). No
# other line names an Underworld portrait. The portrait entry FID_<name> is made to show the Underworld portrait of the character who
# now plays that lord, and the look entries that named it, the lord's own Underworld look in battle, take a copy of it, FID_<name>本人.
CUTSCENE_FACES = ["カイ冥闘士", "ディートリヒ冥闘士", "セオドラ冥闘士", "レダ冥闘士"]
OWN_COPY = "本人"
# Chapter flags. A vanilla Underworld look entry may also match "AID_x & AID_x冥将": a flag the story timeline sets while x's slot is
# Underworld (GameData/Timeline_Part0_Ism.bin sets AID_リンド冥将 and the four lords' for Prologue Ch. 2 and clears them after), so that x
# is Underworld everywhere then, cutscenes included, where no unit carries SID_冥界兵. The flag is named after the slot, the look after
# whoever plays it: each such condition is made to need SID_冥界兵 instead, and a new look entry "AID_<replacement> & AID_x冥将" gives
# the replacement's Underworld look.
FLAG_SUFFIX = "冥将"
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

    def __init__(self, ident, rec, key, name_ptr, aid_ptr, key_ptr, gender, mpid, aid=None):
        self.ident, self.rec, self.key = ident, rec, key
        self.name_ptr, self.aid_ptr, self.key_ptr, self.gender, self.mpid = name_ptr, aid_ptr, key_ptr, gender, mpid
        self.aid = aid          # the model's asset key, AID_カイ

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
                          self.b.ptr(rec + 8), self.b.d[gamedata_bin.HEADER + rec + self.f[F_GENDER]], self.b.string(rec + self.f[F_NAME]),
                          self.b.string(rec + self.f[F_AID]))
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


def asset_swaps(mapping):
    """{original's asset key: replacement's}, for apply_assets(): AID_リンド -> AID_ソレル when Lind's slot wears Solel."""
    return {o.aid: r.aid for o, r in mapping if o.ident != r.ident and o.aid and r.aid}


def mapping_lines(seed, mapping):
    """The log's mapping: a line with the seed and the number of pairs, then the pairs, PAIRS_PER_LINE to a line."""
    pairs = ["%s -> %s" % (o.label(), r.label()) for o, r in mapping]
    return ["randomizer: seed %d: %d pairs" % (seed, len(pairs))] + [
        "randomizer:   " + ", ".join(pairs[i:i + PAIRS_PER_LINE]) for i in range(0, len(pairs), PAIRS_PER_LINE)]


def summary_line(changed, skipped, edited):
    return "randomizer: Person.bin: %d records changed, %d dragon forms left as they are (fnv1a64 %016x)" % (
        len(changed), len(skipped), fnv1a64(edited))


def apply_assets(data, looks=LIVING_LOOKS, forms=UNDERWORLD_FORMS, swaps=None, cutscene_faces=CUTSCENE_FACES):
    """AssetTable.bin with living and Underworld forms, and a list of what changed. The file grows (gamedata_edit.Editor); the runtime
    does the same in edit_assets(), step for step, so the bytes must match. `swaps` is asset_swaps(mapping): steps 3 and 4 need it.

    1. Each look U in `looks` (e.g. ドレイク冥闘士), L = U without 冥闘士 (ドレイク), x its character's asset key (AID_ドレイク):
         a new portrait entry FID_L, a copy of FID_U with 特殊 0 (no Underworld tint; a voice code ending in 9, the Underworld set,
         becomes 0: King Arago's generic Mme9 -> Mme0); the plain look entry ("AID_x" alone) gets 容姿 L, and a キーワード list of just
         SID_冥界兵 (the undead model) becomes empty; and, unless one exists, a new look entry "AID_x & SID_冥界兵" -> U with the
         keyword, so that an Underworld slot still shows him as the vanilla game does.
    2. Each AID_x of `forms`: L = the 容姿 of its plain look entry, U = L + 冥闘士: a new portrait entry FID_U, a copy of FID_L with
       特殊 1 (the tint), and a new look entry "AID_x & SID_冥界兵" -> U with the keyword.
    3. Each look U of `cutscene_faces` (カイ冥闘士), x the character whose "AID_x & SID_冥界兵" entry shows it, when x's slot is replaced
       by r: V = r's Underworld look (after 1 and 2). A new portrait entry FID_U本人, a copy of FID_U; every look entry showing U
       shows U本人 instead; and FID_U takes the content of FID_V (all but its key: voice, portrait, tint, eye and face centres), so the
       cutscene line that names U shows r.
    4. Each condition "AID_x & F" (F = AID_…冥将, a chapter flag) of an entry that also has "AID_x & SID_冥界兵", when x's slot is
       replaced by r: F in the condition becomes SID_冥界兵 (the condition now repeats the other), and, once per F, a new look entry
       "AID_r & F" -> r's Underworld look (as in 3, U本人 for a U of step 3) with the keyword.

    New portrait entries go after the last of 容姿情報 (with a label each, as every entry has), new look entries after the last of
    アセットテーブル: later entries win, so an Underworld entry overrides the plain one, as Cai's does. A new look entry is a copy of
    the vanilla "AID_ブレザ & SID_冥界兵" entry (only its condition, look and keyword set). Its condition lists go at the end of the data."""
    b = gamedata_bin.GameDataBin(bytes(data))
    e = gamedata_edit.Editor(data)
    assets, faces = b.table("アセットテーブル"), b.table("容姿情報")
    f_cond, f_look, f_kw = (assets.field(n).off for n in ("条件", "容姿", "キーワード"))
    f_voice, f_special = faces.field("ボイス").off, faces.field("特殊").off
    swaps = swaps or {}
    changes = []

    def strings(at):
        n, arr = b.u32(at) & 0xFFFF, b.ptr(at + 8)
        return [] if arr is None else [b.string(arr + 8 * i) for i in range(n)]

    def alts(rec):
        word, arr = b.u32(rec + f_cond), b.ptr(rec + f_cond + 8)
        return [] if arr is None else [strings(arr + (word >> 16) * i) for i in range(word & 0xFFFF)]

    entries = list(assets.records())
    conds = {rec: alts(rec) for rec in entries}
    fids = {}
    for rec in faces.records():
        fids.setdefault(b.key(rec), rec)
    template = next((r for r in entries if b.string(r + f_look) == "ブレザ冥闘士" and ["AID_ブレザ", UNDERWORLD] in conds[r]), None)

    def plain_of(aid):
        return next((r for r in entries if [aid] in conds[r]), None)

    def has_underworld(aid):
        return any([aid, UNDERWORLD] in conds[r] for r in entries)

    new_faces = []      # (source portrait entry, new key, 特殊 or None to keep, voice or None to keep)
    new_looks = []      # (AID_x, second condition, look)
    for look in looks:
        living = look[:-len("冥闘士")]
        src = fids.get("FID_" + look)
        aid = next((alt[0] for r in entries if b.string(r + f_look) == look for alt in conds[r] if alt and alt[0].startswith("AID_")),
                   None)
        plain = plain_of(aid) if aid else None
        if src is None or plain is None or "FID_" + living in fids:
            changes.append("%s: skipped" % look)
            continue
        voice = b.string(src + f_voice) or ""
        new_faces.append((src, "FID_" + living, 0, voice[:-1] + "0" if voice.endswith("9") else None))
        e.set_ptr(plain + f_look, living)
        changes.append("%s: living look %s" % (aid, living))
        if strings(plain + f_kw) == [UNDERWORLD]:
            e.u32(plain + f_kw, e.u32(plain + f_kw) & 0xFFFF0000)
            changes.append("%s: living model" % aid)
        if not has_underworld(aid):
            new_looks.append((aid, UNDERWORLD, look))
    for stem in forms:
        aid = "AID_" + stem
        plain = next((r for r in entries if [aid] in conds[r] and b.string(r + f_look)), None)
        living = plain is not None and b.string(plain + f_look)
        if not living or "FID_" + living not in fids or "FID_" + living + "冥闘士" in fids or has_underworld(aid):
            changes.append("%s: skipped" % aid)
            continue
        new_faces.append((fids["FID_" + living], "FID_" + living + "冥闘士", 1, None))
        new_looks.append((aid, UNDERWORLD, living + "冥闘士"))

    def underworld_look(aid):
        """aid's Underworld look after steps 1 and 2: its new look entry's, else that of the last "aid & SID_冥界兵" entry with one."""
        for a, second, look in new_looks:
            if a == aid and second == UNDERWORLD:
                return look
        found = None
        for r in entries:
            if [aid, UNDERWORLD] in conds[r] and b.string(r + f_look):
                found = b.string(r + f_look)
        return found

    def face_source(look):
        """(portrait entry, 特殊, voice) that FID_<look> is or is made from (None: keep the entry's), or None when there is none."""
        if "FID_" + look in fids:
            return fids["FID_" + look], None, None
        return next(((src, special, voice) for src, key, special, voice in new_faces if key == "FID_" + look), None)

    # 3. the portraits the cutscenes name
    renamed = {}        # look -> the name of its copy
    overwrite = []      # (portrait entry to overwrite, its look, the look it now shows, (source, 特殊, voice))
    for u in cutscene_faces:
        aid = next((alt[0] for r in entries if b.string(r + f_look) == u for alt in conds[r] if len(alt) == 2 and alt[1] == UNDERWORLD),
                   None)
        if aid not in swaps:
            continue
        v = underworld_look(swaps[aid])
        src = face_source(v) if v else None
        dst = fids.get("FID_" + u)
        if src is None or dst is None or "FID_" + u + OWN_COPY in fids or any(
                (src[0] + o in b.ptrs) != (dst + o in b.ptrs) for o in range(0x20, faces.rec_size, 8)):
            changes.append("%s: skipped" % u)
            continue
        new_faces.append((dst, "FID_" + u + OWN_COPY, None, None))
        renamed[u] = u + OWN_COPY
        overwrite.append((dst, u, v, src))
    for r in entries:
        look = b.string(r + f_look)
        if look in renamed:
            e.set_ptr(r + f_look, renamed[look])
            changes.append("%s -> %s" % (look, renamed[look]))
    # 4. the chapter flags
    flags = set()
    for r in entries:
        word, arr = b.u32(r + f_cond), b.ptr(r + f_cond + 8)
        for k, alt in enumerate(conds[r]):
            if not (len(alt) == 2 and alt[0].startswith("AID_") and not alt[0].endswith(FLAG_SUFFIX) and alt[0] in swaps
                    and alt[1].startswith("AID_") and alt[1].endswith(FLAG_SUFFIX) and [alt[0], UNDERWORLD] in conds[r]):
                continue
            e.set_ptr(b.ptr(arr + (word >> 16) * k + 8) + 8, UNDERWORLD)
            changes.append("%s & %s: needs %s" % (alt[0], alt[1], UNDERWORLD))
            if alt[1] in flags:
                continue
            flags.add(alt[1])
            v = underworld_look(swaps[alt[0]])
            if v is None:
                changes.append("%s: skipped" % alt[1])
                continue
            new_looks.append((swaps[alt[0]], alt[1], renamed.get(v, v)))
    if new_looks and template is None:
        changes.append("no template look entry: no Underworld forms added")
        new_looks = []

    labels = {name: at for at, name in b.labels}
    H = gamedata_bin.HEADER
    face_recs = list(faces.records())
    face_end, look_end, data_end = face_recs[-1] + faces.rec_size, entries[-1] + assets.rec_size, b.data_size
    face_raw = b"".join(bytes(b.d[H + src:H + src + faces.rec_size]) for src, _, _, _ in new_faces)
    look_raw = bytes(b.d[H + template:H + template + assets.rec_size]) * len(new_looks) if new_looks else b""
    aux_raw = bytes(32 * len(new_looks))
    # in place, at the offsets of the file as it is: the record counts of the two tables
    e.u32(labels["容姿情報"] + 0x10, (len(face_recs) + len(new_faces)) << 16)
    e.u32(labels["アセットテーブル"] + 0x10, (len(entries) + len(new_looks)) << 16)
    # the inserts, from the end of the file down, so that each lands where the file as it is says
    e.insert(data_end, aux_raw)
    e.insert(face_end, face_raw)
    e.insert(look_end, look_raw)

    def now(at):
        """Where an offset of the file as it was is now."""
        return at + (len(look_raw) if at >= look_end else 0) + (len(face_raw) if at >= face_end else 0) + (
            len(aux_raw) if at >= data_end else 0)

    face_row = max(b.u32(r + 0x14) >> 16 for r in face_recs)
    for i, (src, key, special, voice) in enumerate(new_faces):
        rec = now(face_end) - len(face_raw) + faces.rec_size * i   # the inserted block starts where face_end was, moved by look_raw
        e.u32(rec + 0x10, 0x10000 | (len(face_recs) + i))
        e.u32(rec + 0x14, (face_row + 1 + i) << 16)
        for o in range(0, faces.rec_size, 8):
            if src + o in b.ptrs:
                e.copy_ptr(rec + o, now(src + o))
        e.set_ptr(rec + 8, key)
        if voice is not None:
            e.set_ptr(rec + f_voice, voice)
        if special is not None:
            e.data[rec + f_special] = special
        e.add_label(rec, key)
        changes.append("%s (%s)" % (key, "copy" if special is None else "tint" if special else "no tint"))
    # 3, the overwrites: every source is read before any is written (a lord may play another lord)
    copies = []
    for dst, u, v, (src, special, voice) in overwrite:
        e.data[now(dst) + 0x20:now(dst) + faces.rec_size] = b.d[H + src + 0x20:H + src + faces.rec_size]
        copies += [(now(dst) + o, now(src + o)) for o in range(0x20, faces.rec_size, 8) if src + o in b.ptrs]
    e.copy_ptrs(copies)
    for dst, u, v, (src, special, voice) in overwrite:
        if voice is not None:
            e.set_ptr(now(dst) + f_voice, voice)
        if special is not None:
            e.data[now(dst) + f_special] = special
        changes.append("FID_%s shows %s" % (u, v))
    look_row = max(b.u32(r + 0x14) >> 16 for r in entries)
    aux = now(data_end) - len(aux_raw)
    for i, (aid, second, look) in enumerate(new_looks):
        rec = look_end + assets.rec_size * i
        lists = aux + 32 * i
        e.u32(rec + 0x10, 0x10000 | (len(entries) + i))
        e.u32(rec + 0x14, (look_row + 1 + i) << 16)
        e.u32(rec + f_cond, 1 | 16 << 16)
        e.set_ptr(rec + f_cond + 8, lists)
        e.set_ptr(rec + f_look, look)
        e.u32(rec + f_kw, 1 | 8 << 16)
        e.copy_ptr(rec + f_kw + 8, now(template + f_kw + 8))
        e.u32(lists, 2 | 8 << 16)
        e.set_ptr(lists + 8, lists + 16)
        e.set_ptr(lists + 16, aid)
        e.set_ptr(lists + 24, second)
        changes.append("%s & %s -> %s" % (aid, second, look))
    return e.build(), changes


def assets_line(changes, edited):
    return "randomizer: AssetTable.bin: %d changes (fnv1a64 %016x)" % (len(changes), fnv1a64(edited))


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
    ap.add_argument("--o-assets", help="write the edited AssetTable.bin here")
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
    assets = os.path.join(a.romfs, "GameData", "AssetTable.bin")
    if os.path.exists(assets):
        with open(assets, "rb") as fh:
            edited_assets, changes = apply_assets(fh.read(), swaps=asset_swaps(mapping))
        print(assets_line(changes, edited_assets))
        if a.o_assets:
            with open(a.o_assets, "wb") as fh:
                fh.write(edited_assets)
    if a.o:
        with open(a.o, "wb") as fh:
            fh.write(edited)
        print("wrote %s (sha256 %s)" % (a.o, hashlib.sha256(edited).hexdigest()))


if __name__ == "__main__":
    main()
