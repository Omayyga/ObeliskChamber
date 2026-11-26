// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: messages of our own in the game's message files, so the game's own dialogs can show our text (the System
// menu's Quit Game prompt, ObeliskChamber_optmenu.cpp optmenu_ask_quit).
//
// The game reads Msg/<language>/<name>.msbt (MsgStdBn, Nintendo's LMS message format) and looks a message up by its label, a C string
// ("MID_Restart_ReturnTitle_Confirm"); a label no file has shows as nothing. As the RomFS is laid out (services/mods_dll.cpp
// add_runtime_edit), each language's Msg/<language>/System.msbt.zst (zstd; the file with the dialogs' own "MID_Sys_Yes") gets our labels
// too: a message in TXT2, a label in LBL1 (the label hash table is laid out again), and a copy of MID_Sys_Yes's attributes (ATR1) and
// style (TSY1); ATO1 (attribute offsets, per attribute) is copied as it is. Another section (AOCM in this game: a 16-bit count, then a byte
// a message) that holds one entry a message gets MID_Sys_Yes's again; otherwise it is copied as it is (logged). A file with an ATR1 string area after its records, or a label
// table with another hash, is left as it is, and our dialog then falls back to the overlay's prompt.
//
// The unit randomizer adds names the same way (msg_names_install) to each language's Msg/<language>/Person.msbt.zst, after MPID_Leda:
// Katarina, who is in its pool, has a name ID (MPID_Katarina) but no text in any language. A name starts with the control tags
// MPID_Leda's text starts with (Spanish and Italian names begin with one).
#include "ObeliskChamber_game.h"

#include <zstd.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/mod_api.h"
#include "nxrt/services_api.h"

namespace nxrt::games::ObeliskChamber {
namespace {
constexpr const char* kTag = "fefw.msg";
enum class Lang { En, Zh, Ja, Ko };
struct Msg {
    const char* label;
    const char* en;
    const char* zh;
    const char* ja = nullptr;   // nullptr: the English text
    const char* ko = nullptr;
    const char* text(Lang l) const { return l == Lang::Zh ? zh : l == Lang::Ja && ja ? ja : l == Lang::Ko && ko ? ko : en; }
};
const Msg kMsgs[] = {
    {kQuitMid, "Quit the game?\nProgress since your last save will be lost.", "要退出游戏吗？\n上次存档之后的进度将会丢失。"},
};
// The randomizer's names (msg_names_install): Katarina is in its pool, and has a name ID but no text in any language
const Msg kNames[] = {
    {"MPID_Katarina", "Katarina", "卡塔琳娜", "カタリナ", "카타리나"},
};

// One kind of edit: messages added after an anchor label (whose attributes and style they copy) in the files its match names
struct Job {
    const char* what;            // for the log
    const char* anchor;
    const Msg* msgs;
    uint32_t count;
    bool lead_tags;              // each text starts with the control tags the anchor's text starts with
    std::atomic<int> files{0}, skipped{0};
};
Job g_system{"our messages", "MID_Sys_Yes", kMsgs, sizeof kMsgs / sizeof kMsgs[0], false};   // the yes / no dialogs' own label
Job g_names{"the randomizer's names", "MPID_Leda", kNames, sizeof kNames / sizeof kNames[0], true};

struct Rd {
    const uint8_t* p;
    size_t n;
    bool be;
    bool ok = true;
    uint32_t u32(size_t at) {
        if (at + 4 > n) { ok = false; return 0; }
        return be ? (uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3])
                  : (uint32_t(p[at + 3]) << 24 | uint32_t(p[at + 2]) << 16 | uint32_t(p[at + 1]) << 8 | p[at]);
    }
    uint16_t u16(size_t at) {
        if (at + 2 > n) { ok = false; return 0; }
        return be ? uint16_t(p[at] << 8 | p[at + 1]) : uint16_t(p[at + 1] << 8 | p[at]);
    }
};
struct Wr {
    std::vector<uint8_t> b;
    bool be;
    void u32(uint32_t v) {
        if (be) for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
        else for (int s = 0; s <= 24; s += 8) b.push_back(uint8_t(v >> s));
    }
    void u16(uint16_t v) {
        if (be) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
        else { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
    }
    void put_u32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + i] = be ? uint8_t(v >> (24 - 8 * i)) : uint8_t(v >> (8 * i));
    }
};

// UTF-8 -> code points
std::vector<uint32_t> code_points(const char* s) {
    std::vector<uint32_t> out;
    const auto* u = reinterpret_cast<const unsigned char*>(s);
    while (*u) {
        uint32_t c = *u++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        c &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
        while (more-- > 0 && (*u & 0xc0) == 0x80) c = c << 6 | (*u++ & 0x3f);
        out.push_back(c);
    }
    return out;
}
void put_text(Wr& w, int enc, const char* s) {
    for (uint32_t c : code_points(s)) {
        if (enc == 1) {
            if (c >= 0x10000) { c -= 0x10000; w.u16(uint16_t(0xd800 + (c >> 10))); w.u16(uint16_t(0xdc00 + (c & 0x3ff))); }
            else w.u16(uint16_t(c));
        } else if (enc == 2) {
            w.u32(c);
        } else {   // UTF-8
            if (c < 0x80) w.b.push_back(uint8_t(c));
            else if (c < 0x800) { w.b.push_back(uint8_t(0xc0 | c >> 6)); w.b.push_back(uint8_t(0x80 | (c & 0x3f))); }
            else if (c < 0x10000) { w.b.push_back(uint8_t(0xe0 | c >> 12)); w.b.push_back(uint8_t(0x80 | (c >> 6 & 0x3f))); w.b.push_back(uint8_t(0x80 | (c & 0x3f))); }
            else { w.b.push_back(uint8_t(0xf0 | c >> 18)); w.b.push_back(uint8_t(0x80 | (c >> 12 & 0x3f))); w.b.push_back(uint8_t(0x80 | (c >> 6 & 0x3f))); w.b.push_back(uint8_t(0x80 | (c & 0x3f))); }
        }
    }
    if (enc == 1) w.u16(0);
    else if (enc == 2) w.u32(0);
    else w.b.push_back(0);
}

uint32_t label_hash(const std::string& s, uint32_t slots) {
    uint32_t h = 0;
    for (unsigned char c : s) h = h * 0x492 + c;
    return slots ? h % slots : 0;
}

struct Section { char magic[5] = {}; size_t at = 0, size = 0; };   // at: the data (after the 16-byte section header)

// The file with our messages added, or empty when it is not one to change (no anchor, ours already there, or a layout not handled).
std::vector<uint8_t> add_messages(const uint8_t* p, size_t n, Lang lang, const Job& job, std::string* why, std::string* notes = nullptr) {
    if (n < 0x20 || std::memcmp(p, "MsgStdBn", 8) != 0) { *why = "not an MSBT file"; return {}; }
    Rd r{p, n, p[8] == 0xfe && p[9] == 0xff};
    const int enc = p[0x0c];
    const uint16_t nsec = r.u16(0x0e);
    std::vector<Section> secs;
    size_t at = 0x20;
    for (uint16_t i = 0; i < nsec; ++i) {
        if (at + 16 > n) { *why = "section past the end"; return {}; }
        Section s;
        std::memcpy(s.magic, p + at, 4);
        s.size = r.u32(at + 4);
        s.at = at + 16;
        if (s.at + s.size > n) { *why = "section past the end"; return {}; }
        secs.push_back(s);
        at = (s.at + s.size + 15) & ~size_t(15);
    }
    const Section *lbl = nullptr, *atr = nullptr, *tsy = nullptr, *txt = nullptr;
    std::string other;   // a section this does not handle (checked once the file is known to be the dialogs' one)
    for (const Section& s : secs) {
        if (!std::strcmp(s.magic, "LBL1")) lbl = &s;
        else if (!std::strcmp(s.magic, "ATR1")) atr = &s;
        else if (!std::strcmp(s.magic, "TSY1")) tsy = &s;
        else if (!std::strcmp(s.magic, "TXT2")) txt = &s;
        else if (!std::strcmp(s.magic, "ATO1")) {}   // attribute offsets: indexed by attribute, not by message (copied as it is)
        else other += std::string(other.empty() ? "" : ", ") + s.magic;
    }
    if (!lbl || !txt) { *why = "no LBL1 or TXT2"; return {}; }
    // labels
    struct Label { std::string name; uint32_t index; };
    std::vector<Label> labels;
    bool hash_other = false;
    const uint32_t slots = r.u32(lbl->at);
    for (uint32_t i = 0; i < slots && r.ok; ++i) {
        const uint32_t count = r.u32(lbl->at + 4 + i * 8), off = r.u32(lbl->at + 8 + i * 8);
        size_t q = lbl->at + off;
        for (uint32_t k = 0; k < count && r.ok; ++k) {
            if (q >= lbl->at + lbl->size) { r.ok = false; break; }
            const uint8_t len = p[q];
            if (q + 1 + len + 4 > lbl->at + lbl->size) { r.ok = false; break; }
            labels.push_back({std::string(reinterpret_cast<const char*>(p + q + 1), len), r.u32(q + 1 + len)});
            if (label_hash(labels.back().name, slots) != i) hash_other = true;   // another hash: ours would be misplaced
            q += 1 + len + 4;
        }
    }
    const uint32_t msgs = r.u32(txt->at);
    if (!r.ok || slots == 0 || msgs == 0) return {};   // not a file we can read: left alone (the dialogs' file is checked below)
    int anchor = -1;
    for (const Label& l : labels) {
        if (l.name == job.anchor) anchor = static_cast<int>(l.index);
        for (uint32_t i = 0; i < job.count; ++i)
            if (l.name == job.msgs[i].label) { *why = "ours already there"; return {}; }
    }
    if (anchor < 0) return {};   // not the file with the anchor: nothing to say
    if (hash_other) { *why = "the label table uses another hash"; return {}; }
    if (static_cast<uint32_t>(anchor) >= msgs) { *why = "the anchor's index is out of range"; return {}; }
    uint32_t attr_size = 0;
    std::vector<size_t> attr_offs;   // the record fields that are offsets of strings after the records, from ATR1's start (shifted below)
    if (atr) {
        const uint32_t count = r.u32(atr->at);
        attr_size = r.u32(atr->at + 4);
        const size_t recs_end = 8 + size_t(count) * attr_size;
        if (count != msgs || atr->size < recs_end || attr_size > 256) { *why = "ATR1 does not have one record a message"; return {}; }
        if (atr->size > recs_end) {
            // A string area after the records (string attributes): fields whose value, in every record, is an offset from ATR1's start into
            // that area at the start of a string (or 0 / ~0 for none) are offsets; they move by the records added. An offset from the area's
            // own start would not move: such fields are left alone. Without any field like that the area cannot be kept right: left alone.
            const size_t unit = enc == 1 ? 2 : enc == 2 ? 4 : 1;
            auto string_start = [&](size_t off) {   // off: from ATR1's start
                if (off < recs_end || off >= atr->size) return false;
                if (off == recs_end) return true;
                for (size_t k = 0; k < unit; ++k)
                    if (p[atr->at + off - unit + k] != 0) return false;
                return true;
            };
            const size_t step = attr_size % 4 == 0 ? 4 : 1;
            for (size_t f = 0; f + 4 <= attr_size; f += step) {
                bool all = true, any = false;
                for (uint32_t m = 0; m < count && all; ++m) {
                    const uint32_t v = r.u32(atr->at + 8 + size_t(m) * attr_size + f);
                    if (v == 0 || v == 0xffffffffu) continue;
                    any = true;
                    all = string_start(v);
                }
                if (all && any) {
                    attr_offs.push_back(f);
                    if (step == 1) f += 3;
                }
            }
            if (attr_offs.empty()) {   // what the log needs to see the layout
                char b[160];
                std::snprintf(b, sizeof b, "ATR1's string area: no record field points into it (%u records of %u bytes, area %zu bytes;",
                              count, attr_size, atr->size - recs_end);
                *why = b;
                for (int m = 0; m < 3 && m < static_cast<int>(count); ++m) {
                    *why += " record";
                    for (size_t k = 0; k < attr_size && k < 24; ++k) { std::snprintf(b, sizeof b, " %02x", p[atr->at + 8 + size_t(m) * attr_size + k]); *why += b; }
                    *why += ";";
                }
                *why += " area";
                for (size_t k = 0; k < 32 && recs_end + k < atr->size; ++k) { std::snprintf(b, sizeof b, " %02x", p[atr->at + recs_end + k]); *why += b; }
                *why += ")";
                return {};
            }
            if (notes) *notes += " ATR1: " + std::to_string(attr_size) + "-byte records, string area of " + std::to_string(atr->size - recs_end) +
                                 " bytes, " + std::to_string(attr_offs.size()) + " offset field(s) moved;";
        }
    }
    if (tsy && tsy->size != size_t(msgs) * 4) { *why = "TSY1 is not one entry a message"; return {}; }
    if (txt->size < 4 + size_t(msgs) * 4) { *why = "TXT2 too short"; return {}; }

    const uint32_t add = job.count;
    for (uint32_t i = 0; i < add; ++i) labels.push_back({job.msgs[i].label, msgs + i});
    // the control tags the anchor's text starts with (0x0E, group, type, parameter size in bytes, each a code unit; then the parameters)
    std::vector<uint8_t> lead;
    if (job.lead_tags) {
        const size_t unit = enc == 1 ? 2 : enc == 2 ? 4 : 1;
        const size_t a = txt->at + r.u32(txt->at + 4 + size_t(anchor) * 4);
        const size_t z = uint32_t(anchor) + 1 < msgs ? txt->at + r.u32(txt->at + 8 + size_t(anchor) * 4) : txt->at + txt->size;
        Rd t{p, z, r.be};
        auto unit_at = [&](size_t at) -> uint32_t { return unit == 1 ? (at < z ? p[at] : 0u) : unit == 2 ? t.u16(at) : t.u32(at); };
        size_t q = a;
        while (q + 4 * unit <= z && unit_at(q) == 0x0e) {
            const size_t len = 4 * unit + unit_at(q + 3 * unit);
            if (q + len > z) break;
            q += len;
        }
        if (a <= q && q <= n) lead.assign(p + a, p + q);
    }
    Wr w{{}, r.be};
    w.b.assign(p, p + 0x20);
    auto section = [&](const char* magic, const std::vector<uint8_t>& data) {
        w.b.insert(w.b.end(), magic, magic + 4);
        w.u32(static_cast<uint32_t>(data.size()));
        for (int i = 0; i < 8; ++i) w.b.push_back(0);
        w.b.insert(w.b.end(), data.begin(), data.end());
        while (w.b.size() % 16) w.b.push_back(0xab);
    };
    for (const Section& s : secs) {
        Wr d{{}, r.be};
        if (&s == lbl) {
            std::vector<std::vector<const Label*>> by(slots);
            for (const Label& l : labels) by[label_hash(l.name, slots)].push_back(&l);
            d.u32(slots);
            uint32_t off = 4 + slots * 8;
            for (uint32_t i = 0; i < slots; ++i) {
                d.u32(static_cast<uint32_t>(by[i].size()));
                d.u32(off);
                for (const Label* l : by[i]) off += 1 + static_cast<uint32_t>(l->name.size()) + 4;
            }
            for (uint32_t i = 0; i < slots; ++i)
                for (const Label* l : by[i]) {
                    if (l->name.size() > 255) { *why = "label too long"; return {}; }
                    d.b.push_back(static_cast<uint8_t>(l->name.size()));
                    d.b.insert(d.b.end(), l->name.begin(), l->name.end());
                    d.u32(l->index);
                }
        } else if (&s == atr) {
            const size_t recs_end = 8 + size_t(msgs) * attr_size;
            const uint32_t shift = add * attr_size;
            d.u32(msgs + add);
            d.u32(attr_size);
            const size_t recs_at = d.b.size();
            d.b.insert(d.b.end(), p + s.at + 8, p + s.at + recs_end);
            for (uint32_t i = 0; i < add; ++i)
                d.b.insert(d.b.end(), p + s.at + 8 + size_t(anchor) * attr_size, p + s.at + 8 + size_t(anchor + 1) * attr_size);
            for (uint32_t m = 0; m < msgs + add; ++m)   // the string offsets move past the records added
                for (size_t f : attr_offs) {
                    const size_t at_f = recs_at + size_t(m) * attr_size + f;
                    Rd rr{d.b.data(), d.b.size(), r.be};
                    const uint32_t v = rr.u32(at_f);
                    if (v != 0 && v != 0xffffffffu) d.put_u32(at_f, v + shift);
                }
            d.b.insert(d.b.end(), p + s.at + recs_end, p + s.at + s.size);
        } else if (&s == tsy) {
            d.b.assign(p + s.at, p + s.at + s.size);
            for (uint32_t i = 0; i < add; ++i) d.u32(r.u32(s.at + size_t(anchor) * 4));
        } else if (&s == txt) {
            const uint32_t grow = add * 4;
            d.u32(msgs + add);
            for (uint32_t i = 0; i < msgs; ++i) d.u32(r.u32(s.at + 4 + i * 4) + grow);
            const size_t offs_at = d.b.size();
            for (uint32_t i = 0; i < add; ++i) d.u32(0);
            d.b.insert(d.b.end(), p + s.at + 4 + size_t(msgs) * 4, p + s.at + s.size);
            for (uint32_t i = 0; i < add; ++i) {
                d.put_u32(offs_at + i * 4, static_cast<uint32_t>(d.b.size()));
                d.b.insert(d.b.end(), lead.begin(), lead.end());
                put_text(d, enc, job.msgs[i].text(lang));
            }
        } else if (std::strcmp(s.magic, "ATO1") != 0) {
            // A section this does not know (Fortune's Weave's System files have AOCM): one entry a message, with or without a count first,
            // gets MID_Sys_Yes's entry again for ours; anything else is copied as it is. What was done is logged with the first file.
            const uint32_t first = s.size >= 4 ? r.u32(s.at) : 0;
            if (first == msgs && (s.size - 4) % msgs == 0 && (s.size - 4) / msgs >= 1 && (s.size - 4) / msgs <= 64) {
                const size_t stride = (s.size - 4) / msgs;
                d.u32(msgs + add);
                d.b.insert(d.b.end(), p + s.at + 4, p + s.at + s.size);
                for (uint32_t i = 0; i < add; ++i) d.b.insert(d.b.end(), p + s.at + 4 + anchor * stride, p + s.at + 4 + (anchor + 1) * stride);
                if (notes) *notes += std::string(" ") + s.magic + ": a count and " + std::to_string(stride) + " bytes a message, the anchor's copied;";
            } else if (s.size >= 2 && r.u16(s.at) == msgs && (s.size - 2) % msgs == 0 && (s.size - 2) / msgs >= 1 && (s.size - 2) / msgs <= 64) {
                // a 16-bit count first (Fortune's Weave's AOCM: 1148 messages, one byte each)
                const size_t stride = (s.size - 2) / msgs;
                d.u16(static_cast<uint16_t>(msgs + add));
                d.b.insert(d.b.end(), p + s.at + 2, p + s.at + s.size);
                for (uint32_t i = 0; i < add; ++i) d.b.insert(d.b.end(), p + s.at + 2 + anchor * stride, p + s.at + 2 + (anchor + 1) * stride);
                if (notes) *notes += std::string(" ") + s.magic + ": a 16-bit count and " + std::to_string(stride) + " bytes a message, the anchor's copied;";
            } else if (s.size % msgs == 0 && s.size / msgs >= 1 && s.size / msgs <= 64) {
                const size_t stride = s.size / msgs;
                d.b.assign(p + s.at, p + s.at + s.size);
                for (uint32_t i = 0; i < add; ++i) d.b.insert(d.b.end(), p + s.at + anchor * stride, p + s.at + (anchor + 1) * stride);
                if (notes) *notes += std::string(" ") + s.magic + ": " + std::to_string(stride) + " bytes a message, the anchor's copied;";
            } else {
                d.b.assign(p + s.at, p + s.at + s.size);
                if (notes) {
                    char b[160];
                    std::snprintf(b, sizeof b, " %s: %zu bytes, not one entry a message (%u messages), copied as it is; it starts", s.magic, s.size, msgs);
                    *notes += b;
                    for (size_t k = 0; k < 16 && k < s.size; ++k) { std::snprintf(b, sizeof b, " %02x", p[s.at + k]); *notes += b; }
                    *notes += ";";
                }
            }
        } else {
            d.b.assign(p + s.at, p + s.at + s.size);
        }
        section(s.magic, d.b);
    }
    w.put_u32(0x12, static_cast<uint32_t>(w.b.size()));
    if (!r.ok) { *why = "a read went past the end"; return {}; }
    return w.b;
}

bool ends_with(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
// Only Msg/<language>/System.msbt(.zst), the file with the dialogs' labels: every message file of the folder (thousands: one per scene
// and language) read, unpacked and parsed at start-up took 19 s on a PC.
bool match_msbt(const std::string& key) {
    return key.rfind("/msg/", 0) == 0 && (ends_with(key, "/system.msbt") || ends_with(key, "/system.msbt.zst"));
}
bool match_person(const std::string& key) {
    return key.rfind("/msg/", 0) == 0 && (ends_with(key, "/person.msbt") || ends_with(key, "/person.msbt.zst"));
}
void edit_msbt(const NxModFile* f, void* user) {
    Job& job = *static_cast<Job*>(user);
    std::string path = f->path ? f->path : "", why;
    std::string lower = path;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const Lang lang = lower.find("zh") != std::string::npos     ? Lang::Zh   // Msg/CNzh, Msg/TWzh
                      : lower.find("/jpja/") != std::string::npos ? Lang::Ja
                      : lower.find("/krko/") != std::string::npos ? Lang::Ko
                                                                  : Lang::En;
    // zstd-compressed files (.msbt.zst): edited inside, compressed again
    const bool zst = f->size >= 4 && f->data[0] == 0x28 && f->data[1] == 0xb5 && f->data[2] == 0x2f && f->data[3] == 0xfd;
    std::vector<uint8_t> plain;
    if (zst) {
        const unsigned long long n = ZSTD_getFrameContentSize(f->data, static_cast<size_t>(f->size));
        if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > (64ull << 20)) {
            NXLOG_W(kTag, "%s: zstd frame without a size: left alone", path.c_str());
            return;
        }
        plain.resize(static_cast<size_t>(n));
        const size_t r = ZSTD_decompress(plain.data(), plain.size(), f->data, static_cast<size_t>(f->size));
        if (ZSTD_isError(r) || r != n) {
            NXLOG_W(kTag, "%s: does not decompress: left alone", path.c_str());
            return;
        }
    }
    const uint8_t* src = zst ? plain.data() : f->data;
    const size_t src_n = zst ? plain.size() : static_cast<size_t>(f->size);
    std::string notes;
    std::vector<uint8_t> out = add_messages(src, src_n, lang, job, &why, &notes);
    if (out.empty()) {
        if (!why.empty() && why != "not an MSBT file") {
            job.skipped.fetch_add(1);
            NXLOG_W(kTag, "%s: %s not added (%s)", path.c_str(), job.what, why.c_str());
        }
        return;
    }
    if (zst) {
        std::vector<uint8_t> comp(ZSTD_compressBound(out.size()));
        const size_t r = ZSTD_compress(comp.data(), comp.size(), out.data(), out.size(), 3);
        if (ZSTD_isError(r)) {
            NXLOG_W(kTag, "%s: does not compress again: left alone", path.c_str());
            return;
        }
        comp.resize(r);
        out.swap(comp);
    }
    f->replace(f, out.data(), out.size());
    if (job.files.fetch_add(1) == 0)
        NXLOG_I(kTag, "%s: %s added (%llu -> %zu bytes; other sections:%s); the other languages' files get them too", path.c_str(), job.what,
                static_cast<unsigned long long>(f->size), out.size(), notes.empty() ? " none" : notes.c_str());
}
}  // namespace

void msg_install() {
    if (!services::modapi::add_runtime_edit("messages", &match_msbt, &edit_msbt, &g_system))
        NXLOG_W(kTag, "too late to add our messages: the System menu's Quit Game uses the overlay's prompt");
}

void msg_names_install() {
    if (!services::modapi::add_runtime_edit("randomizer", &match_person, &edit_msbt, &g_names))
        NXLOG_W(kTag, "too late to add the randomizer's names: Katarina has none");
}

bool msg_ready() { return g_system.files.load() > 0; }

}  // namespace nxrt::games::ObeliskChamber
