// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the unit randomizer ([fefw] randomize_units, randomize_seed, randomize_force), in the style of Ignis for
// FE14. Swap logic only: the 113 characters of the pool (kPool) are shuffled among themselves (seeded, one-to-one, nobody keeps their own
// look), everywhere the story meets them, enemies included. Stats, classes, items, portraits and text are the slot's own.
//
// GameData/Person.bin is edited as the RomFS is set up (services/mods_dll.cpp add_runtime_edit), so it applies at the next start. The file is
// a self-describing table container (tools/gamedata/gamedata_bin.py has the layout). A character is a 識別子 (identifier, e.g. "Kai"): a
// base record (PID_カイ) with its own model (アセットフラグ = AID_カイ) and a record per story appearance (PID_SC00_05_カイ) that borrows the
// base's model (アセット流用 = PID_カイ). Placements, event scripts and AI arguments all name those PIDs, so only Person.bin changes: every
// record of a replaced identifier takes the replacement base record's
//     名前 (name, MPID_*), アセットフラグ (model, AID_*) and 性別 (gender),
// and a story record's アセット流用 (borrowed model) points at its own character's base, which now has the replacement's model.
// Each is a pointer swapped for one already in the file, so the file keeps its size and layout. Records with no model at all are dragon
// forms (their class is the model) and are left alone. The 識別子 stays (the game tells the routes apart by it).
//
// tools/gamedata/randomize_units.py is the same thing in Python: given the logged seed it prints the same mapping lines and checksum.
#include "ObeliskChamber_game.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/mod_api.h"
#include "nxrt/services_api.h"

namespace nxrt::games::ObeliskChamber {
namespace {

constexpr const char* kTag = "fefw";

// The pool, by 識別子: the characters shuffled among themselves (randomize_units.py POOL, which says how it was chosen). Every one has a
// model and a portrait of its own (numbered under 300; 3xx are the generic class portraits). Of the 140 with a human model: the 82 placed
// on some map, then 31 never placed (14 that look playable, the 8 gods, 8 NPCs with a unique look, Katarina). Left out: Eshmel (Ism),
// Balor and Oktar (no model), and the generic-looking. "Ma\xef\xbd\x84" is Maｄ (Mandalan): the identifier really ends in a full-width ｄ.
const char* const kPool[] = {
    "Kai", "Tia", "Pet", "Hur", "Bel", "Gat", "Max", "Gol", "Dan", "Dit", "Fab", "Esm", "Mic", "Orh", "Die", "Lor", "Set", "Nin", "Seo",
    "Bon", "Tob", "Lil", "Lys", "Tal", "Urs", "Lou", "Sim", "Fia", "Led", "Buc", "Sci", "Oly", "Moo", "Anh", "Naz", "Sar", "Dai", "Hal",
    "Kou", "Tro", "Sot", "Fot", "Juf", "Dag", "Lyr", "Ish", "Mna", "Sol", "Jun", "Mar", "Smi", "Cla", "Car", "Aur", "Guj", "Yan", "Iio",
    "Pep", "Noc", "Sop", "Cat", "Ned", "Zac", "Maj", "Ven", "Ini", "Jas", "Ale", "Nuz", "Cen", "Asw", "Cle", "Mys", "Nat", "Hri", "Kir",
    "Ann", "Gra", "Kra", "Tah", "Cas", "Nom", "Lon", "Cir", "Che", "Shi", "Hyo", "Gan", "Gil", "Zen", "Znz", "Kar", "Bou", "Day", "Baa",
    "Bjo", "Bol", "Lec", "Ma\xef\xbd\x84", "Ara", "Hno", "Lin", "Ban", "Bre", "Qui", "Dra", "Coh", "Mhm", "Ver", "Gab", "Orp", "Kat",
    "Han"};
// The most shuffles make_mapping() tries for a draw where nobody keeps their own look
constexpr int kShuffleTries = 1000;
// How many pairs each log line names: a log line is cut at 4096 bytes.
constexpr size_t kPairsPerLine = 10;

// Table and field names (UTF-8), as Person.bin's schema spells them
constexpr const char* kTablePerson = "\xe5\x80\x8b\xe4\xba\xba";                                          // 個人
constexpr const char* kFieldName = "\xe5\x90\x8d\xe5\x89\x8d";                                            // 名前
constexpr const char* kFieldIdent = "\xe8\xad\x98\xe5\x88\xa5\xe5\xad\x90";                                // 識別子
constexpr const char* kFieldGender = "\xe6\x80\xa7\xe5\x88\xa5";                                          // 性別
constexpr const char* kFieldReuse = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe6\xb5\x81\xe7\x94\xa8";   // アセット流用
constexpr const char* kFieldAid = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x95\xe3\x83\xa9\xe3\x82\xb0";   // アセットフラグ

constexpr size_t kHeader = 0x20;
constexpr uint32_t kTableVersion = 0x20251201;

struct Settings {
    bool on = false;
    uint64_t seed = 0;
    std::string force;
};
Settings g_set;

// ---- the container ----------------------------------------------------------------------------------------------------------------------
// Offsets are data offsets (from 0x20), as the file's pointers are. Every read is bounds-checked: a file that does not look right is left alone.
class Bin {
public:
    bool open(const uint8_t* d, size_t n) {
        d_ = d, n_ = n;
        if (n < kHeader) return false;
        const uint32_t size = rd32(0), dsz = rd32(4), nptr = rd32(8), nlab = rd32(12);
        const uint64_t rel = kHeader + uint64_t(dsz), strings = rel + 8ull * nptr + 8ull * nlab;
        if (size != n || strings > n) return false;
        dsz_ = dsz;
        strings_ = static_cast<size_t>(strings);
        ptrs_.resize(nptr);
        for (uint32_t i = 0; i < nptr; ++i) std::memcpy(&ptrs_[i], d + rel + 8ull * i, 8);
        std::sort(ptrs_.begin(), ptrs_.end());
        labels_.clear();
        for (uint32_t i = 0; i < nlab; ++i) {
            const size_t at = static_cast<size_t>(rel + 8ull * nptr + 8ull * i);
            labels_.push_back({rd32(at), cstr(strings_ + rd32(at + 4))});
        }
        return true;
    }
    bool has(uint64_t off, size_t len) const { return off + len <= dsz_; }   // inside the data
    uint32_t u32(uint64_t off) const { return has(off, 4) ? rd32(kHeader + static_cast<size_t>(off)) : 0; }
    uint64_t u64(uint64_t off) const {
        uint64_t v = 0;
        if (has(off, 8)) std::memcpy(&v, d_ + kHeader + off, 8);
        return v;
    }
    uint8_t u8(uint64_t off) const { return has(off, 1) ? d_[kHeader + off] : 0; }
    bool is_ptr(uint64_t off) const { return std::binary_search(ptrs_.begin(), ptrs_.end(), off); }
    // The string a pointer field points to; nullptr when the field is null or points elsewhere.
    const char* string(uint64_t off) const {
        if (!is_ptr(off)) return nullptr;
        const uint64_t t = u64(off) + kHeader;
        return t >= strings_ && t < n_ ? cstr(static_cast<size_t>(t)) : nullptr;
    }
    struct Label {
        uint32_t at;
        const char* name;
    };
    const std::vector<Label>& labels() const { return labels_; }

private:
    uint32_t rd32(size_t at) const {
        uint32_t v = 0;
        if (at + 4 <= n_) std::memcpy(&v, d_ + at, 4);
        return v;
    }
    // NUL-terminated inside the file, or ""
    const char* cstr(size_t at) const {
        if (at >= n_ || !std::memchr(d_ + at, 0, n_ - at)) return "";
        return reinterpret_cast<const char*>(d_ + at);
    }
    const uint8_t* d_ = nullptr;
    size_t n_ = 0, strings_ = 0;
    uint32_t dsz_ = 0;
    std::vector<uint64_t> ptrs_;
    std::vector<Label> labels_;
};

struct PersonTable {
    uint64_t first = 0;
    uint32_t rec_size = 0, count = 0;
    uint32_t f_name = 0, f_ident = 0, f_gender = 0, f_reuse = 0, f_aid = 0;
};

bool find_person(const Bin& b, PersonTable* t, std::string* why) {
    for (const Bin::Label& l : b.labels()) {
        if (std::strcmp(l.name, kTablePerson) != 0 || l.at < 0x20) continue;
        const uint64_t desc = l.at - 0x20;
        if (b.u32(desc) != kTableVersion || b.u32(desc + 4) != 1) continue;   // a flat table's descriptor sits just before it
        if (!b.is_ptr(l.at + 0x20) || b.u64(l.at + 0x20) != desc || !b.is_ptr(l.at + 0x28)) continue;
        t->rec_size = b.u32(desc + 16);
        const uint32_t nfields = b.u32(desc + 20);
        const uint64_t schema = b.u64(desc + 24);
        t->count = b.u32(l.at + 0x10) >> 16;
        t->first = b.u64(l.at + 0x28);
        if (t->rec_size < 0x20 || !b.has(t->first, uint64_t(t->rec_size) * t->count) || !b.has(schema, 0x20ull * nfields)) break;
        struct Want {
            const char* name;
            uint32_t size;
            uint32_t* off;
        } want[] = {{kFieldName, 8, &t->f_name}, {kFieldIdent, 8, &t->f_ident}, {kFieldGender, 1, &t->f_gender},
                    {kFieldReuse, 16, &t->f_reuse}, {kFieldAid, 8, &t->f_aid}};
        int found = 0;
        for (Want& w : want) {
            for (uint32_t i = 0; i < nfields; ++i) {
                const uint64_t f = schema + 0x20ull * i;
                const char* name = b.string(f);
                if (!name || std::strcmp(name, w.name) != 0) continue;
                const uint32_t size = b.u32(f + 24), off = b.u32(f + 28);
                if (size == w.size && off + size <= t->rec_size) *w.off = off, ++found;
                break;
            }
        }
        if (found == 5) return true;
        *why = "its schema lacks a field the randomizer needs";
        return false;
    }
    if (why->empty()) *why = "no person table";
    return false;
}

// ---- characters and the mapping ---------------------------------------------------------------------------------------------------------
struct Character {
    std::string ident, key, mpid;
    uint64_t name_ptr = 0, aid_ptr = 0, key_ptr = 0;
    uint8_t gender = 0;
    std::string label() const {
        std::string m = mpid;
        if (m.rfind("MPID_", 0) == 0) m.erase(0, 5);
        return ident + "(" + m + ")";
    }
};

std::vector<Character> characters(const Bin& b, const PersonTable& t) {
    std::vector<Character> out;
    for (uint32_t i = 0; i < t.count; ++i) {
        const uint64_t rec = t.first + uint64_t(t.rec_size) * i;
        const char* ident = b.string(rec + t.f_ident);
        if (!ident || !*ident) continue;
        if (!b.is_ptr(rec + t.f_aid) || b.is_ptr(rec + t.f_reuse) || !b.is_ptr(rec + 8)) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Character& c) { return c.ident == ident; })) continue;
        Character c;
        c.ident = ident;
        c.key = b.string(rec + 8) ? b.string(rec + 8) : "";
        c.mpid = b.string(rec + t.f_name) ? b.string(rec + t.f_name) : "";
        c.name_ptr = b.is_ptr(rec + t.f_name) ? b.u64(rec + t.f_name) : 0;
        c.aid_ptr = b.u64(rec + t.f_aid);
        c.key_ptr = b.u64(rec + 8);
        c.gender = b.u8(rec + t.f_gender);
        out.push_back(std::move(c));
    }
    return out;
}

std::string lower_nospace(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == ' ' || ch == '\t') continue;
        o += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    return o;
}

// By identifier (Kai), name ID with or without MPID_ (Troja), or PID with or without PID_; any case.
const Character* find_character(const std::vector<Character>& cs, const std::string& name) {
    const std::string n = lower_nospace(name);
    for (const Character& c : cs) {
        const std::string id = lower_nospace(c.ident), mp = lower_nospace(c.mpid), key = lower_nospace(c.key);
        const std::string mp2 = mp.rfind("mpid_", 0) == 0 ? mp.substr(5) : mp, key2 = key.rfind("pid_", 0) == 0 ? key.substr(4) : key;
        if (n == id || n == mp || n == mp2 || n == key || n == key2) return &c;
    }
    return nullptr;
}

uint64_t splitmix64(uint64_t& s) {
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t fnv1a64(const std::vector<uint8_t>& d) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001B3ull;
    return h;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t"), z = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, z - a + 1);
}

using Mapping = std::vector<std::pair<const Character*, const Character*>>;

// Forced pairs first, in the order given; then the rest of the pool is shuffled among itself (Fisher-Yates over the candidates, from the
// last, repeated until nobody drew themselves). randomize_units.py make_mapping() is the same, step for step.
Mapping make_mapping(const std::vector<Character>& cs, uint64_t seed, const std::string& force) {
    Mapping out;
    std::vector<std::string> used, done;
    auto in = [](const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); };
    size_t at = 0;
    while (at <= force.size()) {
        const size_t comma = std::min(force.find(',', at), force.size());
        const std::string part = trim(force.substr(at, comma - at));
        at = comma + 1;
        if (part.empty()) continue;
        const size_t eq = part.find('=');
        if (eq == std::string::npos) {
            NXLOG_W(kTag, "randomizer: randomize_force: \"%s\" is not original=replacement", part.c_str());
            continue;
        }
        const std::string a = trim(part.substr(0, eq)), b = trim(part.substr(eq + 1));
        const Character *ca = find_character(cs, a), *cb = find_character(cs, b);
        if (!ca || !cb) {
            NXLOG_W(kTag, "randomizer: randomize_force: %s=%s: no character named %s", a.c_str(), b.c_str(), (!ca ? a : b).c_str());
            continue;
        }
        if (in(done, ca->ident) || in(used, cb->ident)) {
            NXLOG_W(kTag, "randomizer: randomize_force: %s=%s: %s already has a replacement or %s is already one", a.c_str(), b.c_str(),
                    a.c_str(), b.c_str());
            continue;
        }
        out.push_back({ca, cb});
        done.push_back(ca->ident);
        used.push_back(cb->ident);
    }
    // The pool characters not forced (originals) and those not yet used as a replacement (candidates), each in Person.bin's order
    auto in_pool = [](const std::string& ident) {
        return std::any_of(std::begin(kPool), std::end(kPool), [&](const char* x) { return ident == x; });
    };
    for (const char* ident : kPool)
        if (std::none_of(cs.begin(), cs.end(), [&](const Character& c) { return c.ident == ident; }))
            NXLOG_W(kTag, "randomizer: pool: no character %s in Person.bin", ident);
    std::vector<const Character*> origs, cands;
    for (const Character& c : cs) {
        if (!in_pool(c.ident)) continue;
        if (!in(done, c.ident)) origs.push_back(&c);
        if (!in(used, c.ident)) cands.push_back(&c);
    }
    // Original i takes candidate i. Nobody keeps their own look: while an original drew itself, shuffle again (about 2.7 shuffles for the
    // full pool). Only a lone candidate, or kShuffleTries failed shuffles (which a real pool cannot do), leaves someone as themselves.
    uint64_t rng = seed;
    const size_t n = std::min(origs.size(), cands.size());
    for (int t = 0; t < kShuffleTries; ++t) {
        for (size_t i = cands.size(); i-- > 1;) std::swap(cands[i], cands[splitmix64(rng) % (i + 1)]);
        bool self = false;
        for (size_t i = 0; i < n; ++i) self = self || origs[i] == cands[i];
        if (cands.size() < 2 || !self) break;
    }
    for (size_t i = 0; i < n; ++i) out.push_back({origs[i], cands[i]});
    for (size_t i = n; i < origs.size(); ++i) NXLOG_W(kTag, "randomizer: pool: no replacement left for %s", origs[i]->ident.c_str());
    return out;
}

// ---- the edit ---------------------------------------------------------------------------------------------------------------------------
void edit_person(const NxModFile* file, void*) {
    Bin b;
    PersonTable t;
    std::string why;
    if (!b.open(file->data, file->size)) why = "not a GameData table file";
    else find_person(b, &t, &why);
    if (!why.empty()) {
        NXLOG_W(kTag, "randomizer: %s: %s: left as it is, units are not randomized", file->path, why.c_str());
        return;
    }
    const std::vector<Character> cs = characters(b, t);
    const Mapping mapping = make_mapping(cs, g_set.seed, g_set.force);
    NXLOG_I(kTag, "randomizer: seed %" PRIu64 ": %zu pairs", g_set.seed, mapping.size());
    for (size_t i = 0; i < mapping.size(); i += kPairsPerLine) {
        std::string line;
        for (size_t j = i; j < std::min(i + kPairsPerLine, mapping.size()); ++j)
            line += (j == i ? "" : ", ") + mapping[j].first->label() + " -> " + mapping[j].second->label();
        NXLOG_I(kTag, "randomizer:   %s", line.c_str());
    }

    std::vector<uint8_t> out(file->data, file->data + file->size);
    auto put = [&](uint64_t off, uint64_t v) { std::memcpy(out.data() + kHeader + off, &v, 8); };   // only on fields is_ptr() vouched for
    int changed = 0, dragons = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const uint64_t rec = t.first + uint64_t(t.rec_size) * i;
        const char* ident = b.string(rec + t.f_ident);
        if (!ident) continue;
        const Character *own = nullptr, *r = nullptr;
        for (const auto& [o, rep] : mapping)
            if (o->ident == ident && o->ident != rep->ident) own = o, r = rep;
        if (!r) continue;
        const bool aid = b.is_ptr(rec + t.f_aid), reuse = b.is_ptr(rec + t.f_reuse);
        if (!aid && !reuse) {
            ++dragons;   // no model of its own and none borrowed: a dragon form
            continue;
        }
        if (b.is_ptr(rec + t.f_name) && r->name_ptr) put(rec + t.f_name, r->name_ptr);
        if (aid) put(rec + t.f_aid, r->aid_ptr);
        // A borrowed model points at the character's own base record, which now carries the replacement's model; never at the
        // replacement's base, which is edited too when the replacement is itself replaced (Lin -> Tro, Tro -> Mhm made Lind look
        // like Mahadu). So only a record that borrowed another character's base changes here.
        if (reuse && b.u64(rec + t.f_reuse) != own->key_ptr) put(rec + t.f_reuse, own->key_ptr);
        out[kHeader + rec + t.f_gender] = r->gender;
        ++changed;
    }
    NXLOG_I(kTag, "randomizer: Person.bin: %d records changed, %d dragon forms left as they are (fnv1a64 %016" PRIx64 ")", changed, dragons,
            fnv1a64(out));
    if (changed) file->replace(file, out.data(), out.size());
}

bool match_person(const std::string& key) { return key == "/gamedata/person.bin"; }

uint64_t new_seed() {
    std::random_device rd;
    uint64_t s = (uint64_t(rd()) << 32) ^ rd() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
    return s ? s : 1;
}

}  // namespace

void randomizer_install() {
    g_set.on = config_get_bool("fefw", "randomize_units", false);
    if (!g_set.on) return;
    const std::string seed = trim(config_get_str("fefw", "randomize_seed", "0"));
    char* end = nullptr;
    g_set.seed = std::strtoull(seed.c_str(), &end, 10);
    if (seed.empty() || (end && *end)) {
        NXLOG_W(kTag, "randomizer: randomize_seed = \"%s\" is not a number: a new seed is picked", seed.c_str());
        g_set.seed = 0;
    }
    if (g_set.seed == 0) {
        g_set.seed = new_seed();
        std::string err;
        if (config_write("fefw", "randomize_seed", std::to_string(g_set.seed), &err))
            NXLOG_I(kTag, "randomizer: new seed %" PRIu64 " written to the ini (randomize_seed)", g_set.seed);
        else
            NXLOG_W(kTag, "randomizer: new seed %" PRIu64 " could not be written to the ini (%s): the next start picks another", g_set.seed,
                    err.c_str());
    }
    g_set.force = config_get_str("fefw", "randomize_force", "");
    const bool ok = services::modapi::add_runtime_edit("randomizer", &match_person, &edit_person, nullptr);
    NXLOG_I(kTag, "randomizer: on, seed %" PRIu64 "%s%s%s", g_set.seed, g_set.force.empty() ? "" : ", forced ", g_set.force.c_str(),
            ok ? " (GameData/Person.bin is edited as it loads)" : ": too late to edit the files, applies at the next start");
}

}  // namespace nxrt::games::ObeliskChamber
