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
// GameData/AssetTable.bin is edited too, and grows: living and Underworld forms (edit_assets()), so that every pool character looks
// living in a living slot and Underworld in an Underworld one, as the vanilla lords do in Prologue Ch. 2, cutscenes included (the
// portraits Prologue Ch. 2's cutscene names, and the chapter flags AID_x冥将). That part needs the mapping, so AssetTable.bin is edited
// after the other files (add_runtime_edit's late), once edit_person has made it.
//
// tools/gamedata/randomize_units.py is the same thing in Python: given the logged seed it prints the same mapping lines and checksum.
#include "ObeliskChamber_game.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
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

// AssetTable.bin (randomize_units.py LIVING_LOOKS, UNDERWORLD_FORMS). The engine gives Underworld Army units the keyword SID_冥界兵, and a
// look entry "AID_x & SID_冥界兵" then gives them their Underworld look. The pool characters whose plain look is Underworld only (Breza,
// King Arago, Hanno, Vandahl, Drake, Cohen, Mahadu) get a living look for their plain one and keep the Underworld one:
const char* const kLivingLooks[] = {
    "\xe3\x83\x96\xe3\x83\xac\xe3\x82\xb6\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // ブレザ冥闘士 (Breza)
    "\xe3\x82\xa2\xe3\x83\xa9\xe3\x82\xb4\xe5\xa4\xa7\xe7\x8e\x8b\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // アラゴ大王冥闘士 (Arago)
    "\xe3\x83\x8f\xe3\x83\xb3\xe3\x83\x8e\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // ハンノ冥闘士 (Hanno)
    "\xe3\x83\xb4\xe3\x82\xa1\xe3\x83\xb3\xe3\x83\x80\xe3\x83\xab\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // ヴァンダル冥闘士 (Vandahl)
    "\xe3\x83\x89\xe3\x83\xac\xe3\x82\xa4\xe3\x82\xaf\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // ドレイク冥闘士 (Drake)
    "\xe3\x82\xb3\xe3\x83\xbc\xe3\x82\xa8\xe3\x83\xb3\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // コーエン冥闘士 (Cohen)
    "\xe3\x83\x9e\xe3\x83\x8f\xe3\x83\x9e\xe3\x83\x89\xe3\x82\xa5\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",   // マハマドゥ冥闘士 (Mahadu)
};
// The pool characters with no Underworld form, by asset key (AID_<x>), in Person.bin's order: each gets one
const char* const kUnderworldForms[] = {
    "\xe3\x83\x94\xe3\x83\xbc\xe3\x83\x86\xe3\x83\xab",   // ピーテル
    "\xe3\x82\xa6\xe3\x83\xab\xe3\x82\xbf\xe3\x83\xb3\xe3\x83\x89",   // ウルタンド
    "\xe3\x83\x9e\xe3\x82\xaf\xe3\x82\xb7\xe3\x83\xa0",   // マクシム
    "\xe3\x82\xb4\xe3\x83\xa9\xe3\x82\xa4\xe3\x82\xa2\xe3\x82\xb9",   // ゴライアス
    "\xe3\x83\x80\xe3\x83\xb3\xe3\x83\x86",   // ダンテ
    "\xe3\x82\xa8\xe3\x82\xb9\xe3\x83\xa1\xe3\x83\xa9\xe3\x83\xab\xe3\x83\x80",   // エスメラルダ
    "\xe3\x83\x9f\xe3\x82\xab\xe3\x82\xa8\xe3\x83\xa9",   // ミカエラ
    "\xe3\x82\xaa\xe3\x83\xab\xe3\x83\x98\xe3\x83\xab",   // オルヘル
    "\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xa8\xe3\x82\xb4",   // ディエゴ
    "\xe3\x83\xad\xe3\x83\xac\xe3\x83\x83\xe3\x82\xbf",   // ロレッタ
    "\xe3\x82\xbb\xe3\x83\x86\xe3\x82\xb9",   // セテス
    "\xe3\x83\x8b\xe3\x83\x8d",   // ニネ
    "\xe3\x83\xaa\xe3\x83\xaa\xe3\x82\xa2\xe3\x83\xb3",   // リリアン
    "\xe3\x83\xa9\xe3\x82\xa4\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x80\xe3\x83\xbc",   // ライサンダー
    "\xe3\x83\xab\xe3\x83\xab\xe3\x83\x87\xe3\x82\xa3\xe3\x83\xa4\xe3\x83\xbc",   // ルルディヤー
    "\xe3\x82\xb7\xe3\x83\xa2\xe3\x83\xb3",   // シモン
    "\xe3\x83\x95\xe3\x82\xa3\xe3\x82\xa2\xe3\x83\x8a",   // フィアナ
    "\xe3\x82\xb7\xe3\x83\xad\xe3\x83\x83\xe3\x82\xb3",   // シロッコ
    "\xe3\x82\xaa\xe3\x83\xaa\xe3\x83\xb3\xe3\x83\x94\xe3\x82\xa2",   // オリンピア
    "\xe3\x83\xa0\xe3\x82\xa6",   // ムウ
    "\xe3\x83\x8a\xe3\x82\xb8\xe3\x83\xa3",   // ナジャ
    "\xe3\x83\x80\xe3\x82\xa4\xe3\x83\x88\xe3\x82\xa6",   // ダイトウ
    "\xe3\x83\x8f\xe3\x83\xab\xe3\x83\xb4\xe3\x82\xa3\xe3\x83\xb3",   // ハルヴィン
    "\xe3\x82\xb3\xe3\x82\xa6\xe3\x82\xab",   // コウカ
    "\xe3\x83\x88\xe3\x83\xad\xe3\x82\xa4\xe3\x82\xa2",   // トロイア
    "\xe3\x82\xbd\xe3\x83\x86\xe3\x82\xa3\xe3\x82\xb9",   // ソティス
    "\xe3\x83\x95\xe3\x82\xa9\xe3\x83\x88\xe3\x83\x8a",   // フォトナ
    "\xe3\x83\xa6\xe3\x83\xbb\xe3\x83\x95\xe3\x82\xa1\xe3\x82\xb9",   // ユ・ファス
    "\xe3\x83\xa9\xe3\x82\xa4\xe3\x83\xa9\xe3\x83\xbc",   // ライラー
    "\xe3\x82\xa4\xe3\x82\xb7\xe3\x82\xa8\xe3\x83\x88",   // イシエト
    "\xe3\x82\xbd\xe3\x83\xac\xe3\x83\xab",   // ソレル
    "\xe3\x82\xb8\xe3\x83\xa5\xe3\x83\xa9",   // ジュラ
    "\xe3\x83\x9e\xe3\x83\xbc\xe3\x82\xba",   // マーズ
    "\xe3\x82\xb9\xe3\x83\x9f\xe3\x83\xab\xe3\x83\x8e\xe3\x82\xb9",   // スミルノス
    "\xe3\x82\xaf\xe3\x83\xac\xe3\x83\xbc\xe3\x83\xab",   // クレール
    "\xe3\x82\xab\xe3\x83\xbc\xe3\x83\xa9",   // カーラ
    "\xe3\x82\xa2\xe3\x82\xa6\xe3\x83\xad\xe3\x83\xa9",   // アウロラ
    "\xe3\x82\xb0\xe3\x82\xb6\xe3\x83\xa9\xe3\x83\xb3",   // グザラン
    "\xe3\x83\xa8\xe3\x83\xbc\xe3\x82\xab\xe3\x82\xa4",   // ヨーカイ
    "\xe3\x82\xa4\xe3\x82\xaa",   // イオ
    "\xe3\x83\x9a\xe3\x83\x9a",   // ペペ
    "\xe3\x83\x8e\xe3\x82\xaf\xe3\x83\x81\xe3\x83\xa5\xe3\x83\xa9",   // ノクチュラ
    "\xe3\x82\xbd\xe3\x83\x95\xe3\x82\xa3\xe3\x82\xa2",   // ソフィア
    "\xe3\x82\xab\xe3\x82\xbf\xe3\x83\xbc\xe3\x83\x8b\xe3\x83\xa3",   // カターニャ
    "\xe3\x83\x8c\xe3\x83\x87\xe3\x82\xa3\xe3\x83\xbc\xe3\x83\x8c",   // ヌディーヌ
    "\xe3\x82\xb6\xe3\x83\xbc\xe3\x82\xb3\xe3\x83\x8d",   // ザーコネ
    "\xe3\x83\x9e\xe3\x82\xb8\xe3\x83\xbc\xe3\x83\x87",   // マジーデ
    "\xe3\x83\x99\xe3\x83\xb3\xe3\x83\x87\xe3\x82\xa3\xe3\x83\x83\xe3\x83\x84",   // ベンディッツ
    "\xe3\x82\xa4\xe3\x83\x8b\xe3\x82\xaa\xe3\x83\x8b",   // イニオニ
    "\xe3\x82\xb8\xe3\x83\xa3\xe3\x82\xb9\xe3\x83\x9f\xe3\x83\xb3",   // ジャスミン
    "\xe3\x82\xa2\xe3\x83\xac\xe3\x82\xad\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x89\xe3\x83\xa9",   // アレキサンドラ
    "\xe3\x83\x8c\xe3\x83\x83\xe3\x83\x84\xe3\x82\xa9",   // ヌッツォ
    "\xe3\x82\xbb\xe3\x83\xb3\xe3\x83\x88\xe3\x83\xaa\xe3\x82\xaa\xe3\x83\xb3",   // セントリオン
    "\xe3\x82\xa2\xe3\x82\xb9\xe3\x83\xaf\xe3\x83\xb3",   // アスワン
    "\xe3\x82\xaf\xe3\x83\xac\xe3\x82\xaa\xe3\x83\x96\xe3\x83\xad\xe3\x82\xb9",   // クレオブロス
    "\xe3\x83\x9f\xe3\x83\xa5\xe3\x82\xbd\xe3\x83\xb3",   // ミュソン
    "\xe3\x83\x8d\xe3\x82\xa4\xe3\x82\xb5\xe3\x83\xb3",   // ネイサン
    "\xe3\x82\xad\xe3\x83\xaa\xe3\x83\xbc\xe3\x82\xaf",   // キリーク
    "\xe3\x82\xad\xe3\x83\xad\xe3\x82\xa4\xe3\x82\xab",   // キロイカ
    "\xe3\x82\xa2\xe3\x83\xb3\xe3\x83\x8a",   // アンナ
    "\xe3\x82\xb0\xe3\x83\xa9\xe3\x83\x87\xe3\x82\xa3\xe3\x82\xa6\xe3\x82\xb9",   // グラディウス
    "\xe3\x82\xaf\xe3\x83\xa9\xe3\x83\x97\xe3\x82\xab",   // クラプカ
    "\xe3\x82\xbf\xe3\x83\x9b\xe3\x82\xa6\xe3\x83\x8b\xe3\x82\xa2",   // タホウニア
    "\xe3\x82\xab\xe3\x82\xb9\xe3\x83\x88\xe3\x83\xbc\xe3\x83\xab",   // カストール
    "\xe3\x83\x8e\xe3\x83\xa0\xe3\x83\xb4\xe3\x82\xa7\xe3\x83\x87",   // ノムヴェデ
    "\xe3\x83\xad\xe3\x83\xb3",   // ロン
    "\xe3\x82\xac\xe3\x83\xb3\xe3\x82\xb7\xe3\x83\xb3",   // ガンシン
    "\xe3\x82\xbc\xe3\x83\x8e\xe3\x83\xb3",   // ゼノン
    "\xe3\x82\xab\xe3\x83\xbc\xe3\x83\xab",   // カール
    "\xe3\x83\x93\xe3\x82\xb8\xe3\x83\xa7\xe3\x82\xa6",   // ビジョウ
    "\xe3\x83\x99\xe3\x83\xa9\xe3\x83\x95\xe3\x82\xa3",   // ベラフィ
    "\xe3\x82\xac\xe3\x83\x96\xe3\x83\xaa\xe3\x82\xa8\xe3\x83\xab",   // ガブリエル
    "\xe3\x82\xaa\xe3\x83\xab\xe3\x83\x95\xe3\x82\xa7\xe3\x82\xa6\xe3\x82\xb9",   // オルフェウス
    "\xe3\x82\xab\xe3\x82\xbf\xe3\x83\xaa\xe3\x83\x8a",   // カタリナ
    "\xe3\x83\x8f\xe3\x83\xb3\xe3\x83\x8b\xe3\x83\x90\xe3\x83\xab",   // ハンニバル
};
constexpr const char* kUnderworldSuffix = "\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab";                // 冥闘士
constexpr const char* kTemplateLook = "\xe3\x83\x96\xe3\x83\xac\xe3\x82\xb6\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab";   // ブレザ冥闘士
constexpr const char* kTemplateAid = "AID_\xe3\x83\x96\xe3\x83\xac\xe3\x82\xb6";                  // AID_ブレザ
constexpr const char* kFieldVoice = "\xe3\x83\x9c\xe3\x82\xa4\xe3\x82\xb9";                       // ボイス
constexpr const char* kUnderworld = "SID_\xe5\x86\xa5\xe7\x95\x8c\xe5\x85\xb5";                     // SID_冥界兵
constexpr const char* kTableAssets = "\xe3\x82\xa2\xe3\x82\xbb\xe3\x83\x83\xe3\x83\x88\xe3\x83\x86\xe3\x83\xbc\xe3\x83\x96\xe3\x83\xab";   // アセットテーブル
constexpr const char* kTableFaces = "\xe5\xae\xb9\xe5\xa7\xbf\xe6\x83\x85\xe5\xa0\xb1";                // 容姿情報
constexpr const char* kFieldCond = "\xe6\x9d\xa1\xe4\xbb\xb6";                                       // 条件
constexpr const char* kFieldLook = "\xe5\xae\xb9\xe5\xa7\xbf";                                       // 容姿
constexpr const char* kFieldKeywords = "\xe3\x82\xad\xe3\x83\xbc\xe3\x83\xaf\xe3\x83\xbc\xe3\x83\x89";   // キーワード
constexpr const char* kFieldFace = "\xe3\x83\x95\xe3\x82\xa7\xe3\x82\xa4\xe3\x82\xb9";                       // フェイス
constexpr const char* kFieldSkin = "\xe8\x82\x8c\xe3\x82\xab\xe3\x83\xa9\xe3\x83\xbc";                 // 肌カラー
constexpr const char* kFieldLip = "\xe5\x8f\xa3\xe7\xb4\x85\xe8\x89\xb2";                                 // 口紅色
constexpr const char* kFieldHair = "\xe9\xab\xaa\xe3\x82\xab\xe3\x83\xa9\xe3\x83\xbc";                 // 髪カラー
constexpr const char* kFieldSpecial = "\xe7\x89\xb9\xe6\xae\x8a";                                    // 特殊
// The Underworld looks Prologue Ch. 2's cutscene and death quotes name outright (randomize_units.py CUTSCENE_FACES): カイ冥闘士,
// ディートリヒ冥闘士, セオドラ冥闘士, レダ冥闘士. Their battle looks take a copy, FID_<look>本人.
const char* const kCutsceneFaces[] = {
    "\xe3\x82\xab\xe3\x82\xa4\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",
    "\xe3\x83\x87\xe3\x82\xa3\xe3\x83\xbc\xe3\x83\x88\xe3\x83\xaa\xe3\x83\x92\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",
    "\xe3\x82\xbb\xe3\x82\xaa\xe3\x83\x89\xe3\x83\xa9\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",
    "\xe3\x83\xac\xe3\x83\x80\xe5\x86\xa5\xe9\x97\x98\xe5\xa3\xab",
};
constexpr const char* kOwnCopy = "\xe6\x9c\xac\xe4\xba\xba";        // 本人
constexpr const char* kFlagSuffix = "\xe5\x86\xa5\xe5\xb0\x86";     // 冥将 (a chapter flag: AID_リンド冥将)

// The second Eshmel ([fefw] randomize_eshmel, experimental; randomize_units.py KASUMI_*). Eshmel's own slot is never replaced (the game
// hands the player's avatar settings to a look only through Eshmel's own record): a second Eshmel joins the pool as a look only, its
// parts drawn from the seed, under a look key of its own; every look entry with an AID_イシュマール alternative gets one for it beside
// it, the drawn parts already satisfied. Kumos (never placed on a map) joins as a slot only, so the pool stays one-to-one.
constexpr const char* kKasumiIdent = "Ksm";
constexpr const char* kKasumiMpid = "MPID_Kasumi";
constexpr const char* kKasumiAid = "AID_\xe3\x82\xab\xe3\x82\xb9\xe3\x83\x9f";                              // AID_カスミ
constexpr const char* kKasumiSlot = "Kon";
// Kumos's asset key (コン・ムー): he has no Underworld form, and gets one (as kUnderworldForms) while he is in. His own look is never
// handed out, but randomize_force may put it on someone (Lin=Kon).
constexpr const char* kKasumiSlotForm = "\xe3\x82\xb3\xe3\x83\xb3\xe3\x83\xbb\xe3\x83\xa0\xe3\x83\xbc";
constexpr const char* kEshmelAid = "AID_\xe3\x82\xa4\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xab";   // AID_イシュマール
constexpr const char* kEshmel = "\xe3\x82\xa4\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xab";         // イシュマール
constexpr const char* kEshmelHood =
    "AID_\xe3\x82\xa4\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xab\xe3\x83\x95\xe3\x83\xbc\xe3\x83\x89";   // AID_イシュマールフード
const char* const kEshmelStory[] = {
    "AID_\xe3\x82\xa4\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xab\xe6\xb5\xae\xe9\x81\x8a",   // AID_イシュマール浮遊 (floating)
    "!\xe3\x82\xa4\xe3\x82\xb7\xe3\x83\xa5\xe3\x83\x9e\xe3\x83\xbc\xe3\x83\xab\xe6\x94\xaf\xe6\x8f\xb4",       // !イシュマール支援 (no shadow)
};
constexpr const char* kMale = "\xe7\x94\xb7";                // 男
constexpr const char* kFemale = "\xe5\xa5\xb3";              // 女
constexpr const char* kHead = "\xe9\xa0\xad\xe9\x83\xa8";    // 頭部 A-D (the creator's hairstyle)
constexpr const char* kBody = "\xe8\xba\xab\xe4\xbd\x93";    // 身体 A-E (outfit)
constexpr const char* kSkin = "\xe8\x82\x8c\xe8\x89\xb2";    // 肌色 1-12
constexpr const char* kHair = "\xe9\xab\xaa\xe8\x89\xb2";    // 髪色 1-16
constexpr const char* kHeads = "ABCD";
constexpr const char* kBodies = "ABCDE";
constexpr int kSkins = 12, kHairs = 16;
constexpr const char* kHoodWord = "\xe3\x83\x95\xe3\x83\xbc\xe3\x83\x89";                                   // フード
// The easter egg: 1 draw in kKasumiEggOdds is hooded, with the owner's own Eshmel (female, hairstyle B, outfit B, skin 2, hair 3)
constexpr uint64_t kKasumiEggOdds = 50;
constexpr uint64_t kKasumiSalt = 0x4B4153554D49ull;                                                         // "KASUMI"
// Its portraits (randomize_units.py KASUMI_LOOK...). The game tints a portrait's ..._Skin, ..._Lip, ..._Hair slots only in Eshmel's rigs
// (a face name containing 015 or 016) and with the save's avatar colours, so Kasumi has a rig of her own: a copy of 015 or 016 in
// UI/Face/900 (add_runtime_copy) with her colours written into those slots, and portrait entries FID_カスミ, FID_カスミ冥闘士 using it.
constexpr const char* kKasumiLook = "\xe3\x82\xab\xe3\x82\xb9\xe3\x83\x9f";                                 // カスミ
constexpr const char* kKasumiFace = "900";
// The Underworld portrait (FID_カスミ冥闘士) has a rig of its own, UI/Face/901: her colours baked into its texture, the tinted slots white.
// Only Eshmel's rigs clip (Hood_Clipping clips the head to the hood), and the game never draws them with an effect.
constexpr const char* kKasumiFaceUw = "901";
constexpr const char* kClipSlotSuffix = "_Clipping";
// Each clip ends at a slot ..._Clipping_End showing a 1600 x 1600 quad (Clipping_End) with alpha 0; the Underworld effect draws every slot
// opaque but Render, so that quad covers the head. In the Underworld rig its vertices sit at one point (it draws nothing); the clipping
// stays (without it the hair shows through the hood) and still ends at that slot.
constexpr const char* kClipEndSuffix = "_Clipping_End";
const char* const kRigFiles[] = {"atlas.zst", "bntx.zst", "skel.zst", "spdb"};
constexpr uint8_t kAtlasKey[32] = {0x2d, 0x7a, 0x77, 0x7e, 0x3f, 0x50, 0x21, 0x60, 0x39, 0x3a, 0x51, 0x5c, 0x7d, 0x37, 0x3b, 0x2c,
                                   0x24, 0x35, 0x49, 0x7a, 0x64, 0x5a, 0x70, 0x5e, 0x76, 0x34, 0x69, 0x69, 0x31, 0x5c, 0x27, 0x24};   // repeating
constexpr uint32_t kBc7Unorm = 0x2001;   // nn::gfx ImageFormat of the portrait textures
const char* const kTintSlots[] = {"_Skin", "_Lip", "_Hair"};                         // the game's order: skin, lip, hair

struct KasumiLook {
    std::string sex;         // 男 or 女
    char head = 'A', body = 'A';
    int skin = 1, hair = 1;
    bool hood = false;
    std::vector<std::string> terms() const {   // the avatar conditions it satisfies
        std::vector<std::string> t = {sex, std::string(kHead) + head, std::string(kBody) + body, kSkin + std::to_string(skin),
                                      kHair + std::to_string(hair)};
        if (hood) t.push_back(kEshmelHood);
        return t;
    }
    uint8_t gender() const { return sex == kMale ? 1 : 2; }
    std::string portrait() const { return std::string(kEshmel) + sex + head + (hood ? kHoodWord : ""); }   // the entry its own copies
    std::string voice() const { return std::string("Ism0") + (sex == kMale ? "M" : "F"); }
    std::string rig() const { return sex == kMale ? "015" : "016"; }
    // the skin of the rig its portrait shows: the Eshmel entry's face is <rig>_<head>HoodOff, or HoodOn with the hood
    std::string face_skin() const { return std::string(1, head) + (hood ? "HoodOn" : "HoodOff"); }
    std::string describe() const {
        return sex + " " + kHead + head + " " + kBody + body + " " + kSkin + std::to_string(skin) + " " + kHair + std::to_string(hair) +
               (hood ? std::string(" ") + kHoodWord : std::string());
    }
};
KasumiLook kasumi_egg() {
    KasumiLook l;
    l.sex = kFemale, l.head = 'B', l.body = 'B', l.skin = 2, l.hair = 3, l.hood = true;
    return l;
}

// Red eyes (randomize_units.py EYE_RIGS, EYE_SLOTS). The Underworld portrait effect (UI/Face/Hell.sprop) recolours the eye slots named in
// a list in the executable: 00_eye, 00_eye_w, 00_eye_hi, each also with _l / _r, any case. These pool characters' portrait rigs name
// theirs without the 00_ (Cara, Hurtando, Maxim, Fianna, Scirocco, Halvin, Alexandra, Cleobulus, Isheth, Hong Hua, Centlion), and
// Gladius's (119) names the whites 00_EyeWhite, so the slots are renamed (edit_eye_rig): a prefix, or the name the list has.
const char* const kEyeRigs[] = {"007", "027", "028", "037", "047", "054", "099", "100", "103", "107", "112", "119"};
struct EyeSlot {
    const char* lower;   // the slot's name, lower case
    const char* to;      // a prefix (ends in _) or the new name
};
const EyeSlot kEyeSlots[] = {{"eye", "00_"},      {"eye_w", "00_"},   {"eye_hi", "00_"},   {"eye_l", "00_"},   {"eye_r", "00_"},
                             {"eye_w_l", "00_"},  {"eye_w_r", "00_"}, {"eye_hi_l", "00_"}, {"eye_hi_r", "00_"}, {"eye_highlight", "00_Eye_Hi"},
                             {"00_eyewhite", "00_Eye_W"}};

constexpr size_t kHeader = 0x20;
constexpr uint32_t kTableVersion = 0x20251201;

struct Settings {
    bool on = false;
    uint64_t seed = 0;
    std::string force;
    int eshmel = 0;   // randomize_eshmel: 0 off, 1 on, 2 hood (always the easter egg)
};
Settings g_set;
// The second Eshmel's look, drawn from the seed at install when randomize_eshmel is on (edit_person and edit_assets both use it)
KasumiLook g_kasumi;
// Its portrait colours [skin, lip, hair] (RGBA, 4 bytes; empty: none), read by edit_assets for its rig's skeleton (a later late edit)
std::string g_kasumi_colours[3];
bool g_kasumi_coloured = false;
std::vector<uint8_t> g_kasumi_uw_atlas, g_kasumi_uw_skel;   // UI/Face/901's atlas and skeleton as copied (decompressed), for its texture
// The mapping by asset key (AID_リンド -> AID_ソレル), made by edit_person for edit_assets (randomize_units.py asset_swaps). Both run on
// the thread that builds the RomFS image, Person.bin first (edit_assets is a late edit).
std::unordered_map<std::string, std::string> g_swaps;
// Level-up speakers (randomize_units.py speaker_renames): a replacement's lines name the slot that wears its look (クラプカ -> ライラー)
std::unordered_map<std::string, std::string> g_speakers;
bool g_mapped = false;

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
    uint32_t data_size() const { return dsz_; }

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

// A flat table (depth 1) found by its label: its records and its schema
struct Table {
    uint64_t obj = 0, first = 0, schema = 0;   // the table object (its label's offset), its first record, its schema
    uint32_t rec_size = 0, count = 0, nfields = 0;
    uint64_t rec(uint32_t i) const { return first + uint64_t(rec_size) * i; }
};

bool find_table(const Bin& b, const char* name, Table* t) {
    for (const Bin::Label& l : b.labels()) {
        if (std::strcmp(l.name, name) != 0 || l.at < 0x20) continue;
        const uint64_t desc = l.at - 0x20;
        if (b.u32(desc) != kTableVersion || b.u32(desc + 4) != 1) continue;   // a flat table's descriptor sits just before it
        if (!b.is_ptr(l.at + 0x20) || b.u64(l.at + 0x20) != desc || !b.is_ptr(l.at + 0x28)) continue;
        t->rec_size = b.u32(desc + 16);
        t->nfields = b.u32(desc + 20);
        t->schema = b.u64(desc + 24);
        t->count = b.u32(l.at + 0x10) >> 16;
        t->first = b.u64(l.at + 0x28);
        t->obj = l.at;
        return t->rec_size >= 0x20 && b.has(t->first, uint64_t(t->rec_size) * t->count) && b.has(t->schema, 0x20ull * t->nfields);
    }
    return false;
}

// A field's offset in the record, when the schema has it with this size
bool find_field(const Bin& b, const Table& t, const char* name, uint32_t size, uint32_t* off) {
    for (uint32_t i = 0; i < t.nfields; ++i) {
        const uint64_t f = t.schema + 0x20ull * i;
        const char* n = b.string(f);
        if (!n || std::strcmp(n, name) != 0) continue;
        const uint32_t fsize = b.u32(f + 24), foff = b.u32(f + 28);
        if (fsize != size || foff + fsize > t.rec_size) return false;
        *off = foff;
        return true;
    }
    return false;
}

struct PersonTable : Table {
    uint32_t f_name = 0, f_ident = 0, f_gender = 0, f_reuse = 0, f_aid = 0;
};

bool find_person(const Bin& b, PersonTable* t, std::string* why) {
    if (!find_table(b, kTablePerson, t)) {
        *why = "no person table";
        return false;
    }
    if (find_field(b, *t, kFieldName, 8, &t->f_name) && find_field(b, *t, kFieldIdent, 8, &t->f_ident) &&
        find_field(b, *t, kFieldGender, 1, &t->f_gender) && find_field(b, *t, kFieldReuse, 16, &t->f_reuse) &&
        find_field(b, *t, kFieldAid, 8, &t->f_aid))
        return true;
    *why = "its schema lacks a field the randomizer needs";
    return false;
}

// ---- characters and the mapping ---------------------------------------------------------------------------------------------------------
struct Character {
    std::string ident, key, mpid, aid;   // aid: the model's asset key, AID_カイ
    uint64_t name_ptr = 0, aid_ptr = 0, key_ptr = 0;
    uint8_t gender = 0;
    bool kasumi = false;   // the second Eshmel: no record; its name and model are strings the file does not have yet (mpid, aid)
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
        c.aid = b.string(rec + t.f_aid) ? b.string(rec + t.f_aid) : "";
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

// randomize_units.py draw_kasumi(): 1 in kKasumiEggOdds (or always, with hood) the easter egg, else every part at random, in this order
KasumiLook draw_kasumi(uint64_t seed, bool hood) {
    uint64_t rng = seed ^ kKasumiSalt;
    if (splitmix64(rng) % kKasumiEggOdds == 0 || hood) return kasumi_egg();
    KasumiLook l;
    l.sex = splitmix64(rng) % 2 == 0 ? kMale : kFemale;
    l.head = kHeads[splitmix64(rng) % 4];
    l.body = kBodies[splitmix64(rng) % 5];
    l.skin = 1 + static_cast<int>(splitmix64(rng) % kSkins);
    l.hair = 1 + static_cast<int>(splitmix64(rng) % kHairs);
    return l;
}

// A condition the player's avatar settings decide (randomize_units.py is_avatar_term): 男, 女, 頭部A-D, 身体A-E, 肌色N, 髪色N, the hood
bool is_avatar_term(const std::string& t) {
    if (t == kMale || t == kFemale || t == kEshmelHood) return true;
    auto rest = [&](const char* pre, std::string* r) {
        const size_t n = std::strlen(pre);
        if (t.compare(0, n, pre) != 0 || t.size() < n) return false;
        *r = t.substr(n);
        return true;
    };
    std::string r;
    if (rest(kHead, &r)) return r.size() == 1 && std::strchr(kHeads, r[0]);
    if (rest(kBody, &r)) return r.size() == 1 && std::strchr(kBodies, r[0]);
    if (rest(kSkin, &r) || rest(kHair, &r))
        return !r.empty() && std::all_of(r.begin(), r.end(), [](char c) { return c >= '0' && c <= '9'; });
    return false;
}

// randomize_units.py kasumi_alternatives(): the alternatives a look entry gets for the second Eshmel beside Eshmel's own
std::vector<std::vector<std::string>> kasumi_alternatives(const std::vector<std::vector<std::string>>& alts, const KasumiLook& look) {
    std::vector<std::vector<std::string>> out;
    const std::vector<std::string> terms = look.terms();
    for (const auto& alt : alts) {
        if (std::find(alt.begin(), alt.end(), kEshmelAid) == alt.end()) continue;
        if (std::any_of(alt.begin(), alt.end(), [](const std::string& t) {
                return std::any_of(std::begin(kEshmelStory), std::end(kEshmelStory), [&](const char* s) { return t == s; });
            }))
            continue;
        std::vector<std::string> nw;
        bool ok = true;
        for (const std::string& t : alt) {
            if (t == kEshmelAid) nw.push_back(kKasumiAid);
            else if (!is_avatar_term(t)) nw.push_back(t);
            else if (std::find(terms.begin(), terms.end(), t) == terms.end()) {
                ok = false;
                break;
            }
        }
        if (ok && std::find(out.begin(), out.end(), nw) == out.end() && std::find(alts.begin(), alts.end(), nw) == alts.end())
            out.push_back(std::move(nw));
    }
    return out;
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

// The second Eshmel, as randomize_force names it: Ksm, Kasumi or MPID_Kasumi, any case
bool is_kasumi_name(const std::string& name) {
    const std::string n = lower_nospace(name);
    return n == "ksm" || n == "kasumi" || n == "mpid_kasumi";
}

// Forced pairs first, in the order given; then the rest of the pool is shuffled among itself (Fisher-Yates over the candidates, from the
// last, repeated until nobody drew themselves). randomize_units.py make_mapping() is the same, step for step. With `kasumi` (the second
// Eshmel), it is the last candidate and Kumos (kKasumiSlot) an original only; a forced pair may name it as a replacement.
Mapping make_mapping(const std::vector<Character>& cs, uint64_t seed, const std::string& force, const Character* kasumi) {
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
        const Character *ca = find_character(cs, a), *cb = kasumi && is_kasumi_name(b) ? kasumi : find_character(cs, b);
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
        const bool slot = in_pool(c.ident) || (kasumi && c.ident == kKasumiSlot);
        if (slot && !in(done, c.ident)) origs.push_back(&c);
        if (in_pool(c.ident) && !in(used, c.ident)) cands.push_back(&c);
    }
    if (kasumi && !in(used, kasumi->ident)) cands.push_back(kasumi);
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
// The file with these pointer fields pointing at strings, appended to its pool when new (Grow, below)
std::vector<uint8_t> with_strings(const std::vector<uint8_t>& d, const std::vector<std::pair<uint64_t, std::string>>& fields);

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
    Character kasumi;   // the second Eshmel, when randomize_eshmel is on
    kasumi.ident = kKasumiIdent, kasumi.mpid = kKasumiMpid, kasumi.aid = kKasumiAid, kasumi.gender = g_kasumi.gender(), kasumi.kasumi = true;
    const Mapping mapping = make_mapping(cs, g_set.seed, g_set.force, g_set.eshmel ? &kasumi : nullptr);
    g_swaps.clear();
    g_speakers.clear();
    for (const auto& [o, rep] : mapping) {
        if (o->ident != rep->ident && !o->aid.empty() && !rep->aid.empty()) g_swaps[o->aid] = rep->aid;
        if (o->ident != rep->ident && o->key.rfind("PID_", 0) == 0 && rep->key.rfind("PID_", 0) == 0)
            g_speakers[rep->key.substr(4)] = o->key.substr(4);
    }
    g_mapped = true;
    NXLOG_I(kTag, "randomizer: seed %" PRIu64 ": %zu pairs", g_set.seed, mapping.size());
    for (size_t i = 0; i < mapping.size(); i += kPairsPerLine) {
        std::string line;
        for (size_t j = i; j < std::min(i + kPairsPerLine, mapping.size()); ++j)
            line += (j == i ? "" : ", ") + mapping[j].first->label() + " -> " + mapping[j].second->label();
        NXLOG_I(kTag, "randomizer:   %s", line.c_str());
    }
    if (g_set.eshmel) NXLOG_I(kTag, "randomizer: second Eshmel %s: %s", kasumi.label().c_str(), g_kasumi.describe().c_str());

    std::vector<uint8_t> out(file->data, file->data + file->size);
    auto put = [&](uint64_t off, uint64_t v) { std::memcpy(out.data() + kHeader + off, &v, 8); };   // only on fields is_ptr() vouched for
    std::vector<std::pair<uint64_t, std::string>> new_strings;   // the second Eshmel's name and model
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
        if (b.is_ptr(rec + t.f_name) && (r->name_ptr || r->kasumi)) {
            if (r->kasumi) new_strings.push_back({rec + t.f_name, r->mpid});
            else put(rec + t.f_name, r->name_ptr);
        }
        if (aid) {
            if (r->kasumi) new_strings.push_back({rec + t.f_aid, r->aid});
            else put(rec + t.f_aid, r->aid_ptr);
        }
        // A borrowed model points at the character's own base record, which now carries the replacement's model; never at the
        // replacement's base, which is edited too when the replacement is itself replaced (Lin -> Tro, Tro -> Mhm made Lind look
        // like Mahadu). So only a record that borrowed another character's base changes here.
        if (reuse && b.u64(rec + t.f_reuse) != own->key_ptr) put(rec + t.f_reuse, own->key_ptr);
        out[kHeader + rec + t.f_gender] = r->gender;
        ++changed;
    }
    if (!new_strings.empty()) out = with_strings(out, new_strings);
    NXLOG_I(kTag, "randomizer: Person.bin: %d records changed, %d dragon forms left as they are (fnv1a64 %016" PRIx64 ")", changed, dragons,
            fnv1a64(out));
    if (changed) file->replace(file, out.data(), out.size());
}

bool match_person(const std::string& key) { return key == "/gamedata/person.bin"; }

// ---- growing a container: tools/gamedata/gamedata_edit.py Editor, step for step (both must give the same bytes) -------------------------
// insert() puts raw bytes into the data at a data offset; every pointer-list entry, pointer value and label at or after it moves.
// set_ptr() makes a u64 a pointer, to a data offset or to a string (reused from the pool when there, else appended in first-use order);
// new pointers go after the file's own in the pointer list (which the files keep unsorted). build() lays it all out again: pointers
// into the strings move with the pool.
class Grow {
public:
    // `d` is a file Bin::open accepted
    Grow(const uint8_t* d, size_t n) {
        uint32_t dsz = 0, nptr = 0, nlab = 0;
        std::memcpy(&dsz, d + 4, 4), std::memcpy(&nptr, d + 8, 4), std::memcpy(&nlab, d + 12, 4);
        head_.assign(d, d + kHeader);
        data_.assign(d + kHeader, d + kHeader + dsz);
        const size_t rel = kHeader + dsz, strings = rel + 8ull * nptr + 8ull * nlab;
        pool_.assign(d + strings, d + n);
        for (uint32_t i = 0; i < nptr; ++i) {
            uint64_t p = 0, v = 0;
            std::memcpy(&p, d + rel + 8ull * i, 8);
            std::memcpy(&v, data_.data() + p, 8);
            Val val;
            if (v < dsz) val.kind = Val::Data, val.v = v;
            else val.kind = Val::Pool, val.v = v + kHeader - strings;
            put(p, val);
        }
        for (uint32_t i = 0; i < nlab; ++i) {
            uint32_t at = 0, name = 0;
            std::memcpy(&at, d + rel + 8ull * nptr + 8ull * i, 4), std::memcpy(&name, d + rel + 8ull * nptr + 8ull * i + 4, 4);
            labels_.push_back({at, name});
        }
        for (size_t i = 0; i < pool_.size();) {
            const size_t end = static_cast<size_t>(std::find(pool_.begin() + static_cast<std::ptrdiff_t>(i), pool_.end(), uint8_t(0)) -
                                                   pool_.begin());
            index_.emplace(std::string(reinterpret_cast<const char*>(pool_.data() + i), end - i), i);
            i = end + 1;
        }
    }
    std::vector<uint8_t>& data() { return data_; }
    uint32_t u32(uint64_t at) const {
        uint32_t v = 0;
        std::memcpy(&v, data_.data() + at, 4);
        return v;
    }
    void set_u32(uint64_t at, uint32_t v) { std::memcpy(data_.data() + at, &v, 4); }

    void insert(uint64_t at, const std::vector<uint8_t>& raw) {
        const uint64_t n = raw.size();
        data_.insert(data_.begin() + static_cast<std::ptrdiff_t>(at), raw.begin(), raw.end());
        auto mv = [&](uint64_t o) { return o >= at ? o + n : o; };
        std::unordered_map<uint64_t, Val> vals;
        for (uint64_t& p : order_) {
            Val v = vals_.at(p);
            if (v.kind == Val::Data) v.v = mv(v.v);
            p = mv(p);
            vals.emplace(p, std::move(v));
        }
        vals_ = std::move(vals);
        for (auto& l : labels_) l.first = static_cast<uint32_t>(mv(l.first));
        for (auto& l : new_labels_) l.first = mv(l.first);
    }
    void set_ptr(uint64_t at, uint64_t target) {
        Val v;
        v.kind = Val::Data, v.v = target;
        put(at, v);
    }
    void set_ptr(uint64_t at, const std::string& text) { put(at, string_ref(text)); }
    void copy_ptr(uint64_t at, uint64_t from) { put(at, vals_.at(from)); }
    // copy_ptr for each (at, from), every source read before any is written
    void copy_ptrs(const std::vector<std::pair<uint64_t, uint64_t>>& pairs) {
        std::vector<Val> vals;
        for (const auto& p : pairs) vals.push_back(vals_.at(p.second));
        for (size_t i = 0; i < pairs.size(); ++i) put(pairs[i].first, vals[i]);
    }
    // the u64 at `at` is no longer a pointer (it leaves the pointer list; its bytes stay)
    void clear_ptr(uint64_t at) {
        if (vals_.erase(at)) order_.erase(std::find(order_.begin(), order_.end(), at));
    }
    void add_label(uint64_t at, const std::string& text) {
        string_ref(text);
        new_labels_.push_back({at, text});
    }

    std::vector<uint8_t> build() {
        std::vector<uint8_t> pool = pool_;
        std::unordered_map<std::string, size_t> new_at;
        for (const std::string& t : new_strings_) {
            new_at.emplace(t, pool.size());
            pool.insert(pool.end(), t.begin(), t.end());
            pool.push_back(0);
        }
        std::vector<std::pair<uint64_t, uint32_t>> labels;
        for (const auto& l : labels_) labels.push_back({l.first, l.second});
        for (const auto& l : new_labels_) {
            const Val v = string_ref(l.second);
            labels.push_back({l.first, static_cast<uint32_t>(v.kind == Val::Pool ? v.v : new_at.at(v.text))});
        }
        std::stable_sort(labels.begin(), labels.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        const uint64_t dsz = data_.size(), strings = kHeader + dsz + 8ull * order_.size() + 8ull * labels.size();
        std::vector<uint8_t> out(head_);
        out.insert(out.end(), data_.begin(), data_.end());
        for (uint64_t p : order_) {
            const Val& v = vals_.at(p);
            const uint64_t value = v.kind == Val::Data ? v.v : v.kind == Val::Pool ? strings - kHeader + v.v : strings - kHeader + new_at.at(v.text);
            std::memcpy(out.data() + kHeader + p, &value, 8);
        }
        for (uint64_t p : order_) out.insert(out.end(), reinterpret_cast<const uint8_t*>(&p), reinterpret_cast<const uint8_t*>(&p) + 8);
        for (const auto& l : labels) {
            const uint32_t at = static_cast<uint32_t>(l.first);
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&at), reinterpret_cast<const uint8_t*>(&at) + 4);
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&l.second), reinterpret_cast<const uint8_t*>(&l.second) + 4);
        }
        out.insert(out.end(), pool.begin(), pool.end());
        const uint32_t hdr[4] = {static_cast<uint32_t>(out.size()), static_cast<uint32_t>(dsz), static_cast<uint32_t>(order_.size()),
                                 static_cast<uint32_t>(labels.size())};
        std::memcpy(out.data(), hdr, sizeof hdr);
        return out;
    }

private:
    struct Val {
        enum Kind { Data, Pool, New } kind = Data;
        uint64_t v = 0;      // a data offset (Data) or an offset into the pool (Pool)
        std::string text;    // New: a string appended to the pool
    };
    void put(uint64_t at, const Val& v) {
        if (vals_.find(at) == vals_.end()) order_.push_back(at);
        vals_[at] = v;
    }
    Val string_ref(const std::string& text) {
        Val v;
        const auto it = index_.find(text);
        if (it != index_.end()) {
            v.kind = Val::Pool, v.v = it->second;
            return v;
        }
        if (std::find(new_strings_.begin(), new_strings_.end(), text) == new_strings_.end()) new_strings_.push_back(text);
        v.kind = Val::New, v.text = text;
        return v;
    }
    std::vector<uint8_t> head_, data_, pool_;
    std::vector<uint64_t> order_;                          // pointer offsets, in pointer-list order
    std::unordered_map<uint64_t, Val> vals_;
    std::vector<std::pair<uint32_t, uint32_t>> labels_;    // the file's: (data offset, name offset in the pool)
    std::vector<std::pair<uint64_t, std::string>> new_labels_;
    std::unordered_map<std::string, size_t> index_;        // text -> its first occurrence in the pool
    std::vector<std::string> new_strings_;
};

std::vector<uint8_t> with_strings(const std::vector<uint8_t>& d, const std::vector<std::pair<uint64_t, std::string>>& fields) {
    Grow e(d.data(), d.size());
    for (const auto& [at, text] : fields) e.set_ptr(at, text);
    return e.build();
}

// ---- living and Underworld forms: AssetTable.bin ---------------------------------------------------------------------------------------
// randomize_units.py apply_assets(), step for step (its docstring says what and why):
//   1. each look U of kLivingLooks, L = U without 冥闘士, x its character: a new portrait entry FID_L (a copy of FID_U, 特殊 0; a voice
//      code ending in 9 ends in 0 instead); the plain look entry "AID_x" gets 容姿 L and loses a keyword list of just SID_冥界兵; and,
//      unless there is one, a new look entry "AID_x & SID_冥界兵" -> U with that keyword.
//   2. each AID_x of kUnderworldForms, L its plain look, U = L + 冥闘士: a new portrait entry FID_U (a copy of FID_L, 特殊 1) and a new
//      look entry "AID_x & SID_冥界兵" -> U with the keyword.
//   3. each look U of kCutsceneFaces whose character x is replaced by r, V = r's Underworld look: a new portrait entry FID_U本人 (a copy
//      of FID_U), every look entry showing U shows U本人, and FID_U takes the content of FID_V (all but its key; no voice when V has none).
//   4. each condition "AID_x & F" (F a chapter flag, AID_…冥将) of an entry that also has "AID_x & SID_冥界兵", x replaced by r: F
//      becomes SID_冥界兵 there, and, once per F, a new look entry "AID_r & F" -> r's Underworld look (U本人 for a U of step 3).
// New portrait entries follow the last of 容姿情報 (labelled), new look entries the last of アセットテーブル (copies of the vanilla
// "AID_ブレザ & SID_冥界兵" entry), their condition lists the end of the data. Steps 3 and 4 need the mapping (g_swaps).
void edit_assets(const NxModFile* file, void*) {
    Bin b;
    Table assets, faces;
    uint32_t f_cond = 0, f_look = 0, f_kw = 0, f_voice = 0, f_special = 0, f_face = 0;
    if (!b.open(file->data, file->size) || !find_table(b, kTableAssets, &assets) || !find_table(b, kTableFaces, &faces) || !faces.count ||
        !assets.count || !find_field(b, assets, kFieldCond, 16, &f_cond) || !find_field(b, assets, kFieldLook, 16, &f_look) ||
        !find_field(b, assets, kFieldKeywords, 16, &f_kw) || !find_field(b, faces, kFieldVoice, 8, &f_voice) ||
        !find_field(b, faces, kFieldSpecial, 1, &f_special) || !find_field(b, faces, kFieldFace, 8, &f_face)) {
        NXLOG_W(kTag, "randomizer: %s: not the asset table the randomizer knows: left as it is", file->path);
        return;
    }
    if (!g_mapped)
        NXLOG_W(kTag, "randomizer: %s: edited before Person.bin, so without the mapping: the cutscenes' Underworld portraits and the "
                      "chapter flags are left as they are", file->path);
    const std::unordered_map<std::string, std::string> swaps = g_mapped ? g_swaps : std::unordered_map<std::string, std::string>();
    using Strings = std::vector<std::string>;
    auto str = [&](uint64_t at) -> std::string {
        const char* s = b.string(at);
        return s ? s : "";
    };
    auto strings = [&](uint64_t at) {
        Strings v;
        if (!b.is_ptr(at + 8)) return v;
        const uint64_t arr = b.u64(at + 8);
        for (uint32_t i = 0, n = b.u32(at) & 0xFFFF; i < n; ++i) v.push_back(str(arr + 8ull * i));
        return v;
    };
    std::vector<uint64_t> entries;
    std::vector<std::vector<Strings>> conds;
    for (uint32_t i = 0; i < assets.count; ++i) {
        const uint64_t rec = assets.rec(i);
        entries.push_back(rec);
        std::vector<Strings> alts;
        if (b.is_ptr(rec + f_cond + 8)) {
            const uint32_t word = b.u32(rec + f_cond);
            const uint64_t arr = b.u64(rec + f_cond + 8);
            for (uint32_t k = 0; k < (word & 0xFFFF); ++k) alts.push_back(strings(arr + uint64_t(word >> 16) * k));
        }
        conds.push_back(std::move(alts));
    }
    auto has_alt = [&](size_t i, const Strings& alt) { return std::find(conds[i].begin(), conds[i].end(), alt) != conds[i].end(); };
    std::unordered_map<std::string, uint64_t> fids;
    for (uint32_t i = 0; i < faces.count; ++i)
        if (const char* k = b.string(faces.rec(i) + 8)) fids.emplace(k, faces.rec(i));
    const std::string underworld = kUnderworld, suffix = kUnderworldSuffix, own = kOwnCopy, flag_suffix = kFlagSuffix;
    auto ends_with = [](const std::string& s, const std::string& end) {
        return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    };
    int64_t tpl = -1;
    for (size_t i = 0; i < entries.size() && tpl < 0; ++i)
        if (str(entries[i] + f_look) == kTemplateLook && has_alt(i, {kTemplateAid, underworld})) tpl = static_cast<int64_t>(i);
    auto plain_of = [&](const std::string& aid, bool need_look) -> int64_t {
        for (size_t i = 0; i < entries.size(); ++i)
            if (has_alt(i, {aid}) && (!need_look || !str(entries[i] + f_look).empty())) return static_cast<int64_t>(i);
        return -1;
    };
    auto has_underworld = [&](const std::string& aid) {
        for (size_t i = 0; i < entries.size(); ++i)
            if (has_alt(i, {aid, underworld})) return true;
        return false;
    };

    Grow e(file->data, file->size);
    size_t changes = 0;
    struct NewFace {
        uint64_t src;
        std::string key;
        int special;         // -1: keep the source's
        std::string voice;   // empty: keep the source's
        std::string face;    // empty: keep the source's
    };
    struct NewLook {
        Strings terms;      // the condition (one alternative)
        std::string look;
        bool keyword;       // with the keyword SID_冥界兵
    };
    std::vector<NewFace> new_faces;
    std::vector<NewLook> new_looks;
    for (const char* look_c : kLivingLooks) {
        const std::string look = look_c, living = look.substr(0, look.size() - suffix.size());
        const auto src = fids.find("FID_" + look);
        std::string aid;
        for (size_t i = 0; i < entries.size() && aid.empty(); ++i)
            if (str(entries[i] + f_look) == look)
                for (const Strings& alt : conds[i])
                    if (!alt.empty() && alt[0].rfind("AID_", 0) == 0) {
                        aid = alt[0];
                        break;
                    }
        const int64_t plain = aid.empty() ? -1 : plain_of(aid, false);
        if (src == fids.end() || plain < 0 || fids.count("FID_" + living)) {
            ++changes;   // skipped
            continue;
        }
        const std::string voice = str(src->second + f_voice);
        new_faces.push_back({src->second, "FID_" + living, 0,
                             !voice.empty() && voice.back() == '9' ? voice.substr(0, voice.size() - 1) + "0" : std::string(),
                             std::string()});
        const uint64_t prec = entries[static_cast<size_t>(plain)];
        e.set_ptr(prec + f_look, living);
        ++changes;
        if (strings(prec + f_kw) == Strings{underworld}) {
            e.set_u32(prec + f_kw, e.u32(prec + f_kw) & 0xFFFF0000u);
            ++changes;
        }
        if (!has_underworld(aid)) new_looks.push_back({{aid, underworld}, look, true});
    }
    std::vector<const char*> forms(std::begin(kUnderworldForms), std::end(kUnderworldForms));
    if (g_set.eshmel) forms.push_back(kKasumiSlotForm);
    for (const char* stem : forms) {
        const std::string aid = std::string("AID_") + stem;
        const int64_t plain = plain_of(aid, true);
        const std::string living = plain < 0 ? std::string() : str(entries[static_cast<size_t>(plain)] + f_look);
        if (living.empty() || !fids.count("FID_" + living) || fids.count("FID_" + living + suffix) || has_underworld(aid)) {
            ++changes;   // skipped
            continue;
        }
        new_faces.push_back({fids.at("FID_" + living), "FID_" + living + suffix, 1, std::string(), std::string()});
        new_looks.push_back({{aid, underworld}, living + suffix, true});
    }
    // 5. The second Eshmel (randomize_units.py apply_assets step 5): FID_カスミ and FID_カスミ冥闘士, copies of the Eshmel portrait entry
    // its draw names with the face in its own rig (UI/Face/900) and its sex's voice; look entries "AID_カスミ" -> カスミ and "AID_カスミ &
    // SID_冥界兵" -> カスミ冥闘士; an alternative beside each of Eshmel's in every look entry. And its rig's colours (randomize_units.py
    // kasumi_colours), from the file as it is, for edit_kasumi_rig.
    struct KasumiAlts {
        size_t index;
        uint64_t rec;
        std::vector<Strings> alts;
    };
    std::vector<KasumiAlts> kasumi_alts;
    if (g_set.eshmel) {
        const std::string look = kKasumiLook;
        const auto src = fids.find("FID_" + g_kasumi.portrait());
        std::string face = src == fids.end() ? std::string() : str(src->second + f_face);
        if (face.find('_') == std::string::npos || fids.count("FID_" + look) || fids.count("FID_" + look + suffix)) {
            ++changes;   // skipped
        } else {
            const std::string skin = face.substr(face.find('_'));
            new_faces.push_back({src->second, "FID_" + look, 0, g_kasumi.voice(), kKasumiFace + skin});
            new_faces.push_back({src->second, "FID_" + look + suffix, 1, g_kasumi.voice(), kKasumiFaceUw + skin});
            new_looks.push_back({{kKasumiAid}, look, false});
            new_looks.push_back({{kKasumiAid, underworld}, look + suffix, true});
        }
        uint32_t f_skin = 0, f_lip = 0, f_hair = 0;
        if (find_field(b, assets, kFieldSkin, 4, &f_skin) && find_field(b, assets, kFieldLip, 4, &f_lip) &&
            find_field(b, assets, kFieldHair, 4, &f_hair)) {
            Strings want_skin = {kEshmelAid, g_kasumi.sex, kSkin + std::to_string(g_kasumi.skin), "Spine"};
            Strings want_hair = {kEshmelAid, kHair + std::to_string(g_kasumi.hair), "Spine"};
            std::sort(want_skin.begin(), want_skin.end());
            std::sort(want_hair.begin(), want_hair.end());
            auto colour = [&](uint64_t at) {
                std::string c(4, '\0');
                for (int k = 0; k < 4; ++k) c[k] = static_cast<char>(b.u8(at + k));
                return c;
            };
            for (auto& c : g_kasumi_colours) c.clear();
            for (size_t i = 0; i < entries.size(); ++i)
                for (Strings alt : conds[i]) {
                    std::sort(alt.begin(), alt.end());
                    if (alt == want_skin) g_kasumi_colours[0] = colour(entries[i] + f_skin), g_kasumi_colours[1] = colour(entries[i] + f_lip);
                    else if (alt == want_hair) g_kasumi_colours[2] = colour(entries[i] + f_hair);
                }
            for (auto& c : g_kasumi_colours)
                if (c == std::string(4, '\0')) c.clear();
            g_kasumi_coloured = true;
        } else {
            NXLOG_W(kTag, "randomizer: %s: no colour fields: the second Eshmel's portrait is left uncoloured", file->path);
        }
        for (size_t i = 0; i < entries.size(); ++i) {
            std::vector<Strings> alts = kasumi_alternatives(conds[i], g_kasumi);
            if (!alts.empty() && (b.u32(entries[i] + f_cond) >> 16) == 16) kasumi_alts.push_back({i, entries[i], std::move(alts)});
        }
    }
    // aid's Underworld look after steps 1 and 2: its new look entry's, else that of the last "aid & SID_冥界兵" entry with one
    auto underworld_look = [&](const std::string& aid) {
        for (const NewLook& l : new_looks)
            if (l.terms == Strings{aid, underworld}) return l.look;
        std::string found;
        for (size_t i = 0; i < entries.size(); ++i)
            if (has_alt(i, {aid, underworld}) && !str(entries[i] + f_look).empty()) found = str(entries[i] + f_look);
        return found;
    };
    // the portrait entry FID_<look> is or is made from: (entry, 特殊 or -1, voice or ""); false when there is none
    auto face_source = [&](const std::string& look, NewFace* out) {
        const auto it = fids.find("FID_" + look);
        if (it != fids.end()) {
            *out = {it->second, "", -1, "", ""};
            return true;
        }
        for (const NewFace& f : new_faces)
            if (f.key == "FID_" + look) {
                *out = f;
                return true;
            }
        return false;
    };

    // 3. the portraits the cutscenes name
    std::unordered_map<std::string, std::string> renamed;   // look -> the name of its copy
    struct Overwrite {
        uint64_t dst;          // the portrait entry FID_<look>
        std::string look, shows;
        NewFace src;
    };
    std::vector<Overwrite> overwrite;
    for (const char* u_c : kCutsceneFaces) {
        const std::string u = u_c;
        std::string aid;
        for (size_t i = 0; i < entries.size() && aid.empty(); ++i)
            if (str(entries[i] + f_look) == u)
                for (const Strings& alt : conds[i])
                    if (alt.size() == 2 && alt[1] == underworld) {
                        aid = alt[0];
                        break;
                    }
        const auto sw = swaps.find(aid);
        if (aid.empty() || sw == swaps.end()) continue;
        const std::string v = underworld_look(sw->second);
        NewFace src;
        const auto dst = fids.find("FID_" + u);
        if (v.empty() || !face_source(v, &src) || dst == fids.end() || fids.count("FID_" + u + own)) {
            ++changes;   // skipped
            continue;
        }
        new_faces.push_back({dst->second, "FID_" + u + own, -1, std::string(), std::string()});
        renamed[u] = u + own;
        overwrite.push_back({dst->second, u, v, src});
    }
    for (uint64_t rec : entries) {
        const auto it = renamed.find(str(rec + f_look));
        if (it == renamed.end()) continue;
        e.set_ptr(rec + f_look, it->second);
        ++changes;
    }
    // 4. the chapter flags
    std::vector<std::string> flags;
    for (size_t i = 0; i < entries.size(); ++i) {
        const uint64_t rec = entries[i];
        const uint32_t word = b.u32(rec + f_cond);
        const uint64_t arr = b.u64(rec + f_cond + 8);
        for (size_t k = 0; k < conds[i].size(); ++k) {
            const Strings& alt = conds[i][k];
            if (alt.size() != 2 || alt[0].rfind("AID_", 0) != 0 || ends_with(alt[0], flag_suffix) || !swaps.count(alt[0]) ||
                alt[1].rfind("AID_", 0) != 0 || !ends_with(alt[1], flag_suffix) || !has_alt(i, {alt[0], underworld}))
                continue;
            e.set_ptr(b.u64(arr + uint64_t(word >> 16) * k + 8) + 8, underworld);
            ++changes;
            if (std::find(flags.begin(), flags.end(), alt[1]) != flags.end()) continue;
            flags.push_back(alt[1]);
            const std::string v = underworld_look(swaps.at(alt[0]));
            if (v.empty()) {
                ++changes;   // skipped
                continue;
            }
            const auto rn = renamed.find(v);
            new_looks.push_back({{swaps.at(alt[0]), alt[1]}, rn == renamed.end() ? v : rn->second, true});
        }
    }
    if (!new_looks.empty() && tpl < 0) {
        ++changes;   // no template look entry: no Underworld forms added
        new_looks.clear();
    }

    std::unordered_map<std::string, uint64_t> labels;
    for (const Bin::Label& l : b.labels()) labels[l.name] = l.at;
    const uint64_t face_end = faces.rec(faces.count - 1) + faces.rec_size, look_end = assets.rec(assets.count - 1) + assets.rec_size,
                   data_end = b.data_size();
    // the end of the data: each new look entry's condition (one alternative: its header, then its strings), then each condition list
    // the second Eshmel's alternatives make: all the alternatives' headers (the entry's own copied, then the new), then the new strings
    size_t aux_size = 0;
    for (const NewLook& l : new_looks) aux_size += 16 + 8 * l.terms.size();
    for (const KasumiAlts& k : kasumi_alts) {
        aux_size += 16 * ((b.u32(k.rec + f_cond) & 0xFFFF) + k.alts.size());
        for (const Strings& a : k.alts) aux_size += 8 * a.size();
    }
    std::vector<uint8_t> face_raw, look_raw, aux_raw(aux_size, 0);
    for (const NewFace& f : new_faces) face_raw.insert(face_raw.end(), file->data + kHeader + f.src, file->data + kHeader + f.src + faces.rec_size);
    if (!new_looks.empty()) {
        const uint64_t t = entries[static_cast<size_t>(tpl)];
        for (size_t i = 0; i < new_looks.size(); ++i)
            look_raw.insert(look_raw.end(), file->data + kHeader + t, file->data + kHeader + t + assets.rec_size);
    }
    // in place first: the record counts of the two tables
    e.set_u32(labels.at(kTableFaces) + 0x10, uint32_t(faces.count + new_faces.size()) << 16);
    e.set_u32(labels.at(kTableAssets) + 0x10, uint32_t(assets.count + new_looks.size()) << 16);
    // the inserts, from the end of the file down
    e.insert(data_end, aux_raw);
    e.insert(face_end, face_raw);
    e.insert(look_end, look_raw);
    auto now = [&](uint64_t at) {
        return at + (at >= look_end ? look_raw.size() : 0) + (at >= face_end ? face_raw.size() : 0) + (at >= data_end ? aux_raw.size() : 0);
    };
    uint32_t face_row = 0, look_row = 0;
    for (uint32_t i = 0; i < faces.count; ++i) face_row = std::max(face_row, b.u32(faces.rec(i) + 0x14) >> 16);
    for (uint32_t i = 0; i < assets.count; ++i) look_row = std::max(look_row, b.u32(assets.rec(i) + 0x14) >> 16);
    for (size_t i = 0; i < new_faces.size(); ++i) {
        const NewFace& f = new_faces[i];
        const uint64_t rec = now(face_end) - face_raw.size() + uint64_t(faces.rec_size) * i;
        e.set_u32(rec + 0x10, 0x10000u | uint32_t(faces.count + i));
        e.set_u32(rec + 0x14, uint32_t(face_row + 1 + i) << 16);
        for (uint32_t o = 0; o < faces.rec_size; o += 8)
            if (b.is_ptr(f.src + o)) e.copy_ptr(rec + o, now(f.src + o));
        e.set_ptr(rec + 8, f.key);
        if (!f.voice.empty()) e.set_ptr(rec + f_voice, f.voice);
        if (!f.face.empty()) e.set_ptr(rec + f_face, f.face);
        if (f.special >= 0) e.data()[rec + f_special] = static_cast<uint8_t>(f.special);
        e.add_label(rec, f.key);
        ++changes;
    }
    // 3, the overwrites: every source is read before any is written (a lord may play another lord). A field the source has no pointer
    // in (Katarina has no voice) stops being one.
    std::vector<std::pair<uint64_t, uint64_t>> copies;
    std::vector<uint64_t> clears;
    for (const Overwrite& w : overwrite) {
        std::memcpy(e.data().data() + now(w.dst) + 0x20, file->data + kHeader + w.src.src + 0x20, faces.rec_size - 0x20);
        for (uint32_t o = 0x20; o < faces.rec_size; o += 8)
            if (b.is_ptr(w.src.src + o)) copies.push_back({now(w.dst) + o, now(w.src.src + o)});
            else if (b.is_ptr(w.dst + o)) clears.push_back(now(w.dst) + o);
    }
    e.copy_ptrs(copies);
    for (uint64_t at : clears) e.clear_ptr(at);
    for (const Overwrite& w : overwrite) {
        if (!w.src.voice.empty()) e.set_ptr(now(w.dst) + f_voice, w.src.voice);
        if (!w.src.face.empty()) e.set_ptr(now(w.dst) + f_face, w.src.face);
        if (w.src.special >= 0) e.data()[now(w.dst) + f_special] = static_cast<uint8_t>(w.src.special);
        ++changes;
        NXLOG_I(kTag, "randomizer: AssetTable.bin: FID_%s shows %s", w.look.c_str(), w.shows.c_str());
    }
    uint64_t lists = now(data_end) - aux_raw.size();
    for (size_t i = 0; i < new_looks.size(); ++i) {
        const NewLook& l = new_looks[i];
        const uint64_t rec = look_end + uint64_t(assets.rec_size) * i;
        e.set_u32(rec + 0x10, 0x10000u | uint32_t(assets.count + i));
        e.set_u32(rec + 0x14, uint32_t(look_row + 1 + i) << 16);
        e.set_u32(rec + f_cond, 1u | 16u << 16);
        e.set_ptr(rec + f_cond + 8, lists);
        e.set_ptr(rec + f_look, l.look);
        if (l.keyword) {
            e.set_u32(rec + f_kw, 1u | 8u << 16);
            e.copy_ptr(rec + f_kw + 8, now(entries[static_cast<size_t>(tpl)] + f_kw + 8));
        } else {   // an empty list, as the game's are
            e.set_u32(rec + f_kw, 8u << 16);
            std::memset(e.data().data() + rec + f_kw + 8, 0, 8);
        }
        e.set_u32(lists, uint32_t(l.terms.size()) | 8u << 16);
        e.set_ptr(lists + 8, lists + 16);
        for (size_t k = 0; k < l.terms.size(); ++k) e.set_ptr(lists + 16 + 8 * k, l.terms[k]);
        lists += 16 + 8 * l.terms.size();
        ++changes;
        if (!(l.terms.size() == 2 && l.terms[1] == underworld)) {
            std::string cond;
            for (const std::string& t : l.terms) cond += (cond.empty() ? "" : " & ") + t;
            NXLOG_I(kTag, "randomizer: AssetTable.bin: %s -> %s", cond.c_str(), l.look.c_str());
        }
    }
    // the second Eshmel's alternatives: the entry's condition list moves to the end of the data, its own alternatives first (their
    // headers copied: their strings stay where they are), then the new ones
    for (const KasumiAlts& k : kasumi_alts) {
        const uint32_t word = b.u32(k.rec + f_cond), n = word & 0xFFFF;
        const uint64_t arr = b.u64(k.rec + f_cond + 8);
        uint64_t strs = lists + 16ull * (n + k.alts.size());
        e.set_u32(now(k.rec) + f_cond, uint32_t(n + k.alts.size()) | 16u << 16);
        e.set_ptr(now(k.rec) + f_cond + 8, lists);
        for (uint32_t a = 0; a < n; ++a) {
            const uint64_t src = arr + 16ull * a;
            std::memcpy(e.data().data() + lists + 16ull * a, file->data + kHeader + src, 16);
            if (b.is_ptr(src + 8)) e.copy_ptr(lists + 16ull * a + 8, now(src + 8));
        }
        for (size_t j = 0; j < k.alts.size(); ++j) {
            const uint64_t at = lists + 16ull * (n + j);
            e.set_u32(at, uint32_t(k.alts[j].size()) | 8u << 16);
            e.set_ptr(at + 8, strs);
            for (size_t m = 0; m < k.alts[j].size(); ++m) e.set_ptr(strs + 8 * m, k.alts[j][m]);
            strs += 8 * k.alts[j].size();
        }
        lists = strs;
        ++changes;
    }
    if (g_set.eshmel)
        NXLOG_I(kTag, "randomizer: AssetTable.bin: second Eshmel (%s): %zu look entries", g_kasumi.describe().c_str(), kasumi_alts.size());
    const std::vector<uint8_t> out = e.build();
    NXLOG_I(kTag, "randomizer: AssetTable.bin: %zu changes (fnv1a64 %016" PRIx64 ")", changes, fnv1a64(out));
    if (changes) file->replace(file, out.data(), out.size());
}

bool match_assets(const std::string& key) { return key == "/gamedata/assettable.bin"; }

// ---- red eyes: the portrait rigs -------------------------------------------------------------------------------------------------------
// randomize_units.py rename_eye_slots(), step for step: a Spine 4.1 binary skeleton (big-endian, variable-length integers, strings with
// their length + 1 first) read as far as its slots, and the eye slots' names replaced. Empty when it is not one or a new name is taken.
std::vector<uint8_t> rename_eye_slots(const std::vector<uint8_t>& d, std::vector<std::pair<std::string, std::string>>* renames) {
    size_t p = 0;
    bool ok = true;
    auto take = [&](size_t n) {
        if (!ok || p + n > d.size()) ok = false;
        else p += n;
    };
    auto varint = [&]() {
        uint32_t v = 0;
        for (int shift = 0; ok; shift += 7) {
            if (p >= d.size()) {
                ok = false;
                break;
            }
            const uint8_t b = d[p++];
            v |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80) || shift >= 28) break;
        }
        return v;
    };
    auto string = [&]() {
        const uint32_t n = varint();
        if (!ok || n <= 1) return std::string();
        const size_t at = p;
        take(n - 1);
        return ok ? std::string(reinterpret_cast<const char*>(d.data() + at), n - 1) : std::string();
    };
    auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return t;
    };
    take(8);   // hash
    if (string().rfind("4.1.", 0) != 0) return {};
    take(16);  // x, y, width, height
    const bool nonessential = ok && p < d.size() && d[p] != 0;
    take(1);
    if (nonessential) {
        take(4);
        string();
        string();
    }
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) string();   // the string table
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {           // bones
        string();
        if (i) varint();
        take(32);   // rotation, x, y, scale x, y, shear x, y, length
        varint();   // transform mode
        take(1);    // skin required
        if (nonessential) take(4);
    }
    struct Slot {
        std::string name;
        size_t at, end;
    };
    std::vector<Slot> slots;
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
        const size_t at = p;
        std::string name = string();
        slots.push_back({std::move(name), at, p});
        varint();   // bone
        take(8);    // colour, dark colour
        varint();   // attachment
        varint();   // blend mode
    }
    if (!ok) return {};
    std::vector<std::string> names;
    for (const Slot& s : slots) names.push_back(lower(s.name));
    struct Rename {
        size_t at, end;
        std::string from, to;
    };
    std::vector<Rename> rs;
    for (const Slot& s : slots) {
        const std::string l = lower(s.name);
        for (const EyeSlot& e : kEyeSlots) {
            if (l != e.lower) continue;
            const std::string to = std::string(e.to).back() == '_' ? e.to + s.name : e.to;
            if (std::find(names.begin(), names.end(), lower(to)) != names.end()) return {};
            rs.push_back({s.at, s.end, s.name, to});
        }
    }
    std::vector<uint8_t> out(d);
    for (size_t i = rs.size(); i-- > 0;) {
        std::vector<uint8_t> enc;
        for (uint32_t n = static_cast<uint32_t>(rs[i].to.size()) + 1;;) {
            enc.push_back(static_cast<uint8_t>((n & 0x7F) | (n > 0x7F ? 0x80 : 0)));
            n >>= 7;
            if (!n) break;
        }
        enc.insert(enc.end(), rs[i].to.begin(), rs[i].to.end());
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(rs[i].at), out.begin() + static_cast<std::ptrdiff_t>(rs[i].end));
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(rs[i].at), enc.begin(), enc.end());
    }
    for (const Rename& r : rs) renames->push_back({r.from, r.to});
    return out;
}

// "/ui/face/099/099.skel.zst" for a folder of kEyeRigs: that folder
const char* eye_rig(const std::string& key) {
    for (const char* n : kEyeRigs)
        if (key == std::string("/ui/face/") + n + "/" + n + ".skel.zst") return n;
    return nullptr;
}
bool match_eye_rig(const std::string& key) { return eye_rig(key) != nullptr; }

void edit_eye_rig(const NxModFile* file, void*) {
    std::string key = file->path ? file->path : "";
    for (char& c : key) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    const char* n = eye_rig(key);
    const unsigned long long size = ZSTD_getFrameContentSize(file->data, static_cast<size_t>(file->size));
    if (!n || size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64ull << 20)) {
        NXLOG_W(kTag, "randomizer: %s: not a zstd frame with its size: left as it is", file->path);
        return;
    }
    std::vector<uint8_t> skel(static_cast<size_t>(size));
    const size_t got = ZSTD_decompress(skel.data(), skel.size(), file->data, static_cast<size_t>(file->size));
    if (ZSTD_isError(got) || got != size) {
        NXLOG_W(kTag, "randomizer: %s: does not decompress: left as it is", file->path);
        return;
    }
    std::vector<std::pair<std::string, std::string>> renames;
    const std::vector<uint8_t> out = rename_eye_slots(skel, &renames);
    std::string what;
    for (const auto& [from, to] : renames) what += (what.empty() ? "" : ", ") + from + " -> " + to;
    NXLOG_I(kTag, "randomizer: UI/Face/%s/%s.skel.zst: %s", n, n, what.empty() ? "left as it is" : what.c_str());
    if (out.empty() || renames.empty()) return;
    std::vector<uint8_t> comp(ZSTD_compressBound(out.size()));
    const size_t csize = ZSTD_compress(comp.data(), comp.size(), out.data(), out.size(), 3);
    if (ZSTD_isError(csize)) {
        NXLOG_W(kTag, "randomizer: %s: does not compress again: left as it is", file->path);
        return;
    }
    file->replace(file, comp.data(), csize);
}

// ---- the second Eshmel's portrait rig: UI/Face/900 ------------------------------------------------------------------------------------
// randomize_units.py kasumi_rig_file(), step for step. The four files are copies of rig 015's or 016's (add_runtime_copy, registered at
// install) and are edited as they load: the folder name in the atlas's first line (XOR-scrambled: bytes change by src ^ dst), the BNTX
// texture name and the .spdb paths; the skeleton's _Skin, _Lip, _Hair slots take the colours edit_assets read (so it is a late edit).
// Empty with *note saying why when the file is not laid out as expected.

// [(slot name, offset of its colour)] of a Spine 4.1 binary skeleton; false when it is not one
bool skel_slot_colours(const std::vector<uint8_t>& d, std::vector<std::pair<std::string, size_t>>* slots) {
    size_t p = 0;
    bool ok = true;
    auto take = [&](size_t n) {
        if (!ok || p + n > d.size()) ok = false;
        else p += n;
    };
    auto varint = [&]() {
        uint32_t v = 0;
        for (int shift = 0; ok; shift += 7) {
            if (p >= d.size()) {
                ok = false;
                break;
            }
            const uint8_t b = d[p++];
            v |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80) || shift >= 28) break;
        }
        return v;
    };
    auto string = [&]() {
        const uint32_t n = varint();
        if (!ok || n <= 1) return std::string();
        const size_t at = p;
        take(n - 1);
        return ok ? std::string(reinterpret_cast<const char*>(d.data() + at), n - 1) : std::string();
    };
    take(8);
    if (string().rfind("4.1.", 0) != 0) return false;
    take(16);
    const bool nonessential = ok && p < d.size() && d[p] != 0;
    take(1);
    if (nonessential) {
        take(4);
        string();
        string();
    }
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) string();
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
        string();
        if (i) varint();
        take(32);
        varint();
        take(1);
        if (nonessential) take(4);
    }
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
        std::string name = string();
        varint();
        slots->push_back({std::move(name), p});
        take(8);    // colour, dark colour
        varint();
        varint();
    }
    return ok;
}

bool ends_with(const std::string& name, const std::string& end) {
    return name.size() >= end.size() && name.compare(name.size() - end.size(), end.size(), end) == 0;
}
bool ends_with_clip(const std::string& name) { return ends_with(name, kClipSlotSuffix); }

// randomize_units.py _clipping_entries(): the clipping attachments and their end markers in the skins, [(offset of its slot group's count,
// entry start, entry end)], in file order. Each is alone in its slot's group "<slot> 01 <key ref>"; found by those bytes and checked field
// by field: for a ..._Clipping slot, name ref none or a ..._Clipping string, type 6, end slot "<name>_End", 3 to 4096 vertices (weighted
// or not), the colour (nonessential); for a ..._Clipping_End slot, a ...Clipping_End string, type 2 (mesh: path, colour, vertices, uvs,
// triangles, vertices, hull, sequence, and with nonessential data edges, width, height).
std::vector<std::array<size_t, 3>> clipping_entries(const std::vector<uint8_t>& d, const std::vector<std::pair<std::string, size_t>>& slots) {
    size_t p = 0;
    bool ok = true;
    auto varint_at = [&](size_t& q, uint32_t* v) {
        *v = 0;
        for (int shift = 0;; shift += 7) {
            if (q >= d.size()) return false;
            const uint8_t b = d[q++];
            *v |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80) || shift >= 28) return true;
        }
    };
    auto take = [&](size_t n) {
        if (!ok || p + n > d.size()) ok = false;
        else p += n;
    };
    auto string = [&]() {
        uint32_t n = 0;
        if (!ok || !varint_at(p, &n)) {
            ok = false;
            return std::string();
        }
        if (n <= 1) return std::string();
        const size_t at = p;
        take(n - 1);
        return ok ? std::string(reinterpret_cast<const char*>(d.data() + at), n - 1) : std::string();
    };
    take(8);
    string();
    take(16);
    const bool nonessential = ok && p < d.size() && d[p] != 0;
    take(1);
    if (nonessential) {
        take(4);
        string();
        string();
    }
    uint32_t nstrings = 0;
    if (!ok || !varint_at(p, &nstrings)) return {};
    std::vector<uint32_t> clip_refs, end_refs;
    for (uint32_t i = 0; ok && i < nstrings; ++i) {
        const std::string t = string();
        if (ends_with_clip(t)) clip_refs.push_back(i + 1);
        if (ends_with(t, "Clipping_End")) end_refs.push_back(i + 1);
    }
    if (!ok) return {};
    // past the vertices at q (weighted: per vertex a bone count, then index, x, y, weight per bone; or x, y); false when out of the file
    auto vertices = [&](size_t& q, uint32_t vc) {
        if (q >= d.size()) return false;
        const bool weighted = d[q++] != 0;
        for (uint32_t v = 0; weighted && v < vc; ++v) {
            uint32_t bones = 0;
            if (!varint_at(q, &bones)) return false;
            for (uint32_t b = 0; b < bones; ++b) {
                uint32_t idx = 0;
                if (!varint_at(q, &idx)) return false;
                q += 12;
            }
        }
        if (!weighted) q += 8ull * vc;
        return true;
    };
    auto encode = [](uint32_t v) {
        std::vector<uint8_t> out;
        for (;;) {
            out.push_back(static_cast<uint8_t>((v & 0x7F) | (v > 0x7F ? 0x80 : 0)));
            v >>= 7;
            if (!v) return out;
        }
    };
    std::vector<std::array<size_t, 3>> found;
    for (size_t k = 0; k < slots.size(); ++k) {
        const std::string& name = slots[k].first;
        const bool mesh = ends_with(name, kClipEndSuffix);
        if (!mesh && !ends_with_clip(name)) continue;
        const std::vector<uint32_t>& refs = mesh ? end_refs : clip_refs;
        for (uint32_t ref : refs) {
            std::vector<uint8_t> head = encode(static_cast<uint32_t>(k));
            const size_t slot_len = head.size();
            head.push_back(1);
            const std::vector<uint8_t> refb = encode(ref);
            head.insert(head.end(), refb.begin(), refb.end());
            for (auto it = std::search(d.begin(), d.end(), head.begin(), head.end()); it != d.end();
                 it = std::search(it + 1, d.end(), head.begin(), head.end())) {
                const size_t count_at = static_cast<size_t>(it - d.begin()) + slot_len, at = count_at + 1;
                size_t q = at + refb.size();
                uint32_t nref = 0, endslot = 0, vc = 0;
                if (!varint_at(q, &nref) || !(nref == 0 || std::find(refs.begin(), refs.end(), nref) != refs.end())) continue;
                if (q >= d.size() || d[q] != (mesh ? 2 : 6)) continue;
                ++q;
                if (mesh) {
                    uint32_t path = 0, vc = 0, tris = 0, hull = 0, edges = 0;
                    if (!varint_at(q, &path)) continue;
                    q += 4;   // colour
                    if (!varint_at(q, &vc) || vc < 3 || vc > 4096) continue;
                    q += 8ull * vc;   // uvs
                    if (!varint_at(q, &tris)) continue;
                    q += 2ull * tris;
                    if (!vertices(q, vc) || !varint_at(q, &hull) || q >= d.size()) continue;
                    bool good = true;
                    if (d[q++]) {   // a sequence
                        for (int s = 0; good && s < 4; ++s) {
                            uint32_t x = 0;
                            good = varint_at(q, &x);
                        }
                    }
                    if (good && nonessential) {
                        good = varint_at(q, &edges);
                        q += 2ull * edges + 8;
                    }
                    if (good && q <= d.size()) found.push_back({count_at, at, q});
                    continue;
                }
                if (!varint_at(q, &endslot) || !varint_at(q, &vc)) continue;
                if (endslot >= slots.size() || slots[endslot].first != name + "_End" || vc < 3 || vc > 4096 || q >= d.size() || d[q] > 1)
                    continue;
                if (!vertices(q, vc)) continue;
                if (nonessential) q += 4;
                if (q <= d.size()) found.push_back({count_at, at, q});
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

// ---- the Underworld rig's texture: her colours baked in (randomize_units.py bake_texture) ----------------------------------------------
// The Underworld effect sets every slot's colour to one lavender grey, so in UI/Face/901 the 4x4 BC7 blocks the _Skin, _Lip, _Hair meshes
// of the portrait's skins draw get their endpoint colours times hers, and those slots are white. The atlas packs by polygon, so the pixels
// are the ones the mesh triangles cover, mapped through the atlas as Spine 4.1 maps them.

struct AtlasRegion {
    int64_t x = 0, y = 0, w = 0, h = 0, ox = 0, oy = 0, ow = 0, oh = 0, deg = 0;
};

// randomize_units.py atlas_regions(): the page's size and its regions (width, height unrotated); false when it is not one page of text.
// A region named twice is left out.
bool atlas_regions(const std::vector<uint8_t>& d, int64_t* width, int64_t* height, std::map<std::string, AtlasRegion>* out) {
    std::string text(d.size(), '\0');
    for (size_t i = 0; i < d.size(); ++i) {
        text[i] = static_cast<char>(d[i] ^ kAtlasKey[i % sizeof kAtlasKey]);
        if (static_cast<uint8_t>(text[i]) >= 0x80) return false;
    }
    auto strip = [](const std::string& s) {
        const char* ws = " \t\n\r\x0b\x0c\x1c\x1d\x1e\x1f";
        const size_t a = s.find_first_not_of(ws);
        if (a == std::string::npos) return std::string();
        return s.substr(a, s.find_last_not_of(ws) - a + 1);
    };
    auto to_int = [](const std::string& s, int64_t* v) {   // Python int(): optional sign, digits (spaces already stripped)
        size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
        if (i >= s.size() || s.size() - i > 15) return false;
        int64_t n = 0;
        for (size_t j = i; j < s.size(); ++j) {
            if (s[j] < '0' || s[j] > '9') return false;
            n = n * 10 + (s[j] - '0');
        }
        *v = s[0] == '-' ? -n : n;
        return true;
    };
    std::vector<std::string> lines;
    for (size_t a = 0;;) {
        const size_t b = text.find('\n', a);
        lines.push_back(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    struct Raw {
        std::array<int64_t, 9> v{};
        std::array<bool, 8> set{};
    };
    std::map<std::string, Raw> regions;
    std::set<std::string> twice;
    Raw* cur = nullptr;
    bool gap = false, have_size = false;
    for (size_t li = 1; li < lines.size(); ++li) {
        const std::string line = strip(lines[li]);
        if (line.empty()) {
            gap = cur != nullptr;
            continue;
        }
        if (gap) return false;   // a second page
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            if (regions.count(line)) twice.insert(line);
            cur = &(regions[line] = Raw());
            continue;
        }
        const std::string key = line.substr(0, colon), value = line.substr(colon + 1);
        std::vector<std::string> values;
        for (size_t a = 0;;) {
            const size_t b = value.find(',', a);
            values.push_back(strip(value.substr(a, b == std::string::npos ? std::string::npos : b - a)));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (!cur) {
            if (key == "size") {
                if (values.size() != 2 || !to_int(values[0], width) || !to_int(values[1], height)) return false;
                have_size = true;
            }
        } else if (key == "bounds" || key == "offsets") {
            if (values.size() != 4) return false;
            const size_t at = key == "bounds" ? 0 : 4;
            for (size_t i = 0; i < 4; ++i) {
                if (!to_int(values[i], &cur->v[at + i])) return false;
                cur->set[at + i] = true;
            }
        } else if (key == "rotate") {
            const std::string v = strip(value);
            if (v == "true") cur->v[8] = 90;
            else if (v == "false") cur->v[8] = 0;
            else if (!to_int(v, &cur->v[8])) return false;
        }
    }
    if (!have_size) return false;
    for (auto& [name, r] : regions) {
        const int64_t deg = r.v[8];
        if (twice.count(name) || !r.set[0] || !r.set[1] || !r.set[2] || !r.set[3] || (deg != 0 && deg != 90 && deg != 180 && deg != 270))
            continue;
        if (!r.set[4]) r.v[4] = 0, r.v[5] = 0, r.v[6] = r.v[2], r.v[7] = r.v[3];
        (*out)[name] = {r.v[0], r.v[1], r.v[2], r.v[3], r.v[4], r.v[5], r.v[6], r.v[7], deg};
    }
    return true;
}

struct SkelMesh {
    int kind = 3;   // the index of the slot's kTintSlots suffix, or 3
    std::string region;
    std::vector<float> uvs;
    std::vector<uint16_t> tris;
};

// randomize_units.py _skel_meshes(): the meshes and linked meshes of the skins named in skins (all when null; a linked mesh with its
// parent's, from the skin it names or the default one, any skin); one with a sequence is left out; false when the file does not read.
bool skel_meshes(const std::vector<uint8_t>& d, const std::vector<std::string>* skins, std::vector<SkelMesh>* out) {
    size_t p = 0;
    bool ok = true;
    auto take = [&](size_t n) {
        if (!ok || n > d.size() - p) ok = false;
        else p += n;
        return ok ? d.data() + p - n : nullptr;
    };
    auto byte = [&]() {
        const uint8_t* b = take(1);
        return b ? int(*b) : 0;
    };
    auto varint = [&]() {
        uint32_t v = 0;
        for (int shift = 0; ok; shift += 7) {
            const int b = byte();
            v |= uint32_t(b & 0x7F) << shift;
            if (!(b & 0x80) || shift >= 28) break;
        }
        return v;
    };
    using Opt = std::optional<std::string>;
    auto string = [&]() -> Opt {
        const uint32_t n = varint();
        if (!ok || n == 0) return std::nullopt;
        const uint8_t* s = take(n - 1);
        return s ? Opt(std::string(reinterpret_cast<const char*>(s), n - 1)) : std::nullopt;
    };
    std::vector<Opt> strings;
    auto ref = [&]() -> Opt {
        const uint32_t n = varint();
        if (ok && n > strings.size()) ok = false;
        return ok && n ? strings[n - 1] : std::nullopt;
    };
    auto truthy = [](const Opt& s) { return s && !s->empty(); };
    take(8);
    const Opt version = string();
    if (!ok || !version || version->rfind("4.1.", 0) != 0) return false;
    take(16);
    const bool nonessential = byte() != 0;
    if (nonessential) {
        take(4);
        string();
        string();
    }
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) strings.push_back(string());
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {   // bones
        string();
        if (i) varint();
        take(32);
        varint();
        take(1);
        if (nonessential) take(4);
    }
    std::vector<std::string> slots;
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
        const Opt name = string();
        slots.push_back(name ? *name : std::string());
        varint();
        take(8);
        varint();
        varint();
    }
    const std::pair<int, size_t> constraints[3] = {{1, 12}, {1, 50}, {4, 24}};   // ik, transform, path constraints
    for (const auto& [varints, size] : constraints)
        for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
            string();
            varint();
            take(1);
            for (uint32_t j = 0, m = varint(); ok && j < m; ++j) varint();
            for (int j = 0; j < varints; ++j) varint();
            take(size);
        }
    auto vertices = [&](uint32_t n) {
        if (byte()) {
            for (uint32_t v = 0; ok && v < n; ++v)
                for (uint32_t b = 0, bones = varint(); ok && b < bones; ++b) {
                    varint();
                    take(12);
                }
        } else {
            take(8ull * n);
        }
    };
    auto sequence = [&]() {
        if (!byte()) return false;
        for (int i = 0; i < 4; ++i) varint();
        return true;
    };
    auto kind = [&](uint32_t slot) {
        for (int k = 0; k < 3; ++k)
            if (ends_with(slots[slot], kTintSlots[k])) return k;
        return 3;
    };
    auto be32 = [](const uint8_t* b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; };
    struct Mesh {
        SkelMesh m;
        std::string skin;
    };
    struct Linked {
        int kind;
        std::string region, of;
        uint32_t slot;
        Opt parent;
        std::string skin;
    };
    std::vector<Mesh> meshes;   // in the order first seen (a Python dict), the key's later value replacing an earlier
    std::map<std::tuple<std::string, uint32_t, Opt>, size_t> by_key;
    std::vector<Linked> linked;
    auto skin = [&](const std::string& name) {
        for (uint32_t g = 0, groups = varint(); ok && g < groups; ++g) {
            const uint32_t k = varint();
            if (ok && k >= slots.size()) ok = false;
            for (uint32_t a = 0, n = varint(); ok && a < n; ++a) {
                const Opt key = ref();
                const Opt nm = ref();
                std::string region = truthy(nm) ? *nm : key ? *key : std::string();
                const bool region_none = !truthy(nm) && !key;
                switch (byte()) {
                    case 0:   // region
                        ref();
                        take(32);
                        sequence();
                        break;
                    case 1:   // bounding box
                        vertices(varint());
                        take(nonessential ? 4 : 0);
                        break;
                    case 2: {   // mesh: path, colour, uvs, triangles, vertices, hull, sequence
                        const Opt path = ref();
                        bool none = region_none;
                        if (truthy(path)) region = *path, none = false;
                        take(4);
                        const uint32_t vc = varint();
                        const uint8_t* uv = take(8ull * vc);
                        SkelMesh m;
                        m.kind = ok ? kind(k) : 3;
                        for (uint32_t i = 0; uv && i < 2 * vc; ++i) {
                            const uint32_t bits = be32(uv + 4 * i);
                            float f;
                            std::memcpy(&f, &bits, 4);
                            m.uvs.push_back(f);
                        }
                        const uint32_t nt = varint();
                        const uint8_t* tri = take(2ull * nt);
                        for (uint32_t i = 0; tri && i < nt; ++i) m.tris.push_back(static_cast<uint16_t>(tri[2 * i] << 8 | tri[2 * i + 1]));
                        vertices(vc);
                        varint();
                        if (!sequence() && ok) {
                            m.region = none ? std::string() : region;
                            const auto id = std::make_tuple(name, k, key);
                            const auto at = by_key.find(id);
                            if (at == by_key.end()) {
                                by_key[id] = meshes.size();
                                meshes.push_back({std::move(m), name});
                            } else {
                                meshes[at->second] = {std::move(m), name};
                            }
                        }
                        if (nonessential) {
                            take(2ull * varint());
                            take(8);
                        }
                        break;
                    }
                    case 3: {   // linked mesh: path, colour, skin, parent, timelines, sequence
                        const Opt path = ref();
                        if (truthy(path)) region = *path;
                        take(4);
                        const Opt of = ref();
                        const Opt parent = ref();
                        take(1);
                        if (!sequence() && ok) linked.push_back({kind(k), region, truthy(of) ? *of : "default", k, parent, name});
                        take(nonessential ? 8 : 0);
                        break;
                    }
                    case 4: {   // path
                        take(2);
                        const uint32_t vc = varint();
                        vertices(vc);
                        take(4ull * (vc / 3));
                        take(nonessential ? 4 : 0);
                        break;
                    }
                    case 5:   // point
                        take(12 + (nonessential ? 4 : 0));
                        break;
                    case 6:   // clipping
                        varint();
                        vertices(varint());
                        take(nonessential ? 4 : 0);
                        break;
                    default:
                        ok = false;
                }
            }
        }
    };
    skin("default");
    for (uint32_t i = 0, n = varint(); ok && i < n; ++i) {
        const Opt name = ref();
        for (int list = 0; ok && list < 4; ++list)
            for (uint32_t j = 0, m = varint(); ok && j < m; ++j) varint();
        skin(truthy(name) ? *name : std::string());
    }
    if (!ok) return false;
    auto wanted = [&](const std::string& s) { return !skins || std::find(skins->begin(), skins->end(), s) != skins->end(); };
    for (const Mesh& m : meshes)
        if (wanted(m.skin)) out->push_back(m.m);
    for (const Linked& l : linked) {
        const auto at = by_key.find(std::make_tuple(l.of, l.slot, l.parent));
        if (at == by_key.end() || !wanted(l.skin)) continue;
        SkelMesh m = meshes[at->second].m;
        m.kind = l.kind;
        m.region = l.region;
        out->push_back(std::move(m));
    }
    return true;
}

int64_t floordiv(int64_t a, int64_t b) {
    const int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// randomize_units.py _cover(): mark in maps[k] the pixels whose centres the mesh's triangles cover, inside the region's packed rectangle;
// for a tint kind, add the 4x4 blocks touched. Positions as Spine 4.1 has them (exact in doubles), in 1/256 pixel rounded half up.
void cover(std::vector<uint8_t> (&maps)[4], std::set<int64_t>* touched, int64_t width, int64_t height, int k, const AtlasRegion& r,
           const SkelMesh& m) {
    const bool turned = r.deg == 90 || r.deg == 270;
    const int64_t pw = turned ? r.h : r.w, ph = turned ? r.w : r.h;
    const int64_t rx0 = std::max<int64_t>(r.x, 0), ry0 = std::max<int64_t>(r.y, 0);
    const int64_t rx1 = std::min<int64_t>(r.x + pw, width) - 1, ry1 = std::min<int64_t>(r.y + ph, height) - 1;
    std::vector<std::pair<int64_t, int64_t>> pts;
    for (size_t i = 0; i + 1 < m.uvs.size(); i += 2) {
        const double u = m.uvs[i], v = m.uvs[i + 1];
        double px, py;
        if (r.deg == 90) {
            const double a = v * double(r.oh), b = (1 - u) * double(r.ow);
            px = double(r.x - (r.oh - r.oy - r.h)) + a, py = double(r.y - (r.ow - r.ox - r.w)) + b;
        } else if (r.deg == 180) {
            const double a = (1 - u) * double(r.ow), b = (1 - v) * double(r.oh);
            px = double(r.x - (r.ow - r.ox - r.w)) + a, py = double(r.y - r.oy) + b;
        } else if (r.deg == 270) {
            const double a = (1 - v) * double(r.oh), b = u * double(r.ow);
            px = double(r.x - r.oy) + a, py = double(r.y - r.ox) + b;
        } else {
            const double a = u * double(r.ow), b = v * double(r.oh);
            px = double(r.x - r.ox) + a, py = double(r.y - (r.oh - r.oy - r.h)) + b;
        }
        if (!(std::fabs(px) < double(1 << 20) && std::fabs(py) < double(1 << 20))) return;   // far off the page, or not a number
        const double sx = px * 256, sy = py * 256;
        pts.push_back({static_cast<int64_t>(std::floor(sx + 0.5)), static_cast<int64_t>(std::floor(sy + 0.5))});
    }
    std::vector<uint8_t>& map = maps[k];
    const int64_t bw = (width + 3) / 4;
    for (size_t t = 0; t + 2 < m.tris.size(); t += 3) {
        const size_t a = m.tris[t], b = m.tris[t + 1], c = m.tris[t + 2];
        if (std::max({a, b, c}) >= pts.size()) continue;
        int64_t x0 = pts[a].first, y0 = pts[a].second, x1 = pts[b].first, y1 = pts[b].second, x2 = pts[c].first, y2 = pts[c].second;
        const int64_t area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
        if (area == 0) continue;
        if (area < 0) std::swap(x1, x2), std::swap(y1, y2);
        const int64_t edges[3][4] = {{x0, y0, x1, y1}, {x1, y1, x2, y2}, {x2, y2, x0, y0}};
        const int64_t lo_y = std::max(ry0, -floordiv(128 - std::min({y0, y1, y2}), 256));
        const int64_t hi_y = std::min(ry1, floordiv(std::max({y0, y1, y2}) - 128, 256));
        for (int64_t row = lo_y; row <= hi_y; ++row) {
            const int64_t yc = row * 256 + 128;
            int64_t lo = rx0, hi = rx1;
            for (const auto& e : edges) {   // (xb - xa)(yc - ya) - (yb - ya)(px - xa) >= 0, px = 256 x + 128
                const int64_t xa = e[0], ya = e[1], xb = e[2], yb = e[3];
                const int64_t den = 256 * (ya - yb);
                const int64_t num = -((xb - xa) * (yc - ya) + (yb - ya) * xa) - 128 * (ya - yb);
                if (den > 0) lo = std::max(lo, -floordiv(-num, den));
                else if (den < 0) hi = std::min(hi, floordiv(num, den));
                else if (num > 0) hi = lo - 1;
            }
            if (lo > hi) continue;
            std::fill(map.begin() + row * width + lo, map.begin() + row * width + hi + 1, uint8_t(1));
            if (k < 3)
                for (int64_t blk = (row / 4) * bw + lo / 4; blk <= (row / 4) * bw + hi / 4; ++blk) touched->insert(blk);
        }
    }
}

// randomize_units.py bake_blocks(): {block index: kind} for the blocks a tint kind touches: the kind covering most of the block's pixels
// (ties: kTintSlots order), unless the other attachments cover more
std::map<int64_t, int> bake_blocks(int64_t width, int64_t height, const std::map<std::string, AtlasRegion>& regions,
                                   const std::vector<SkelMesh>& meshes) {
    std::vector<uint8_t> maps[4];
    for (auto& m : maps) m.assign(static_cast<size_t>(width * height), 0);
    std::set<int64_t> touched;
    for (const SkelMesh& m : meshes) {
        const auto r = regions.find(m.region);
        if (r != regions.end()) cover(maps, &touched, width, height, m.kind, r->second, m);
    }
    const int64_t bw = (width + 3) / 4;
    std::map<int64_t, int> out;
    for (const int64_t blk : touched) {
        const int64_t by = blk / bw, bx = blk % bw;
        int64_t cnt[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k)
            for (int64_t row = 4 * by; row < std::min(4 * by + 4, height); ++row) {
                const int64_t s = row * width + 4 * bx, e = std::min(s + 4, (row + 1) * width);
                cnt[k] += std::count(maps[k].begin() + s, maps[k].begin() + e, uint8_t(1));
            }
        int best = 0;
        for (int k = 1; k < 3; ++k)
            if (cnt[k] > cnt[best]) best = k;
        if (cnt[best] && cnt[best] >= cnt[3]) out[blk] = best;
    }
    return out;
}

// randomize_units.py bntx_texture(): the one texture of a BNTX file; false when it is not one block-linear 2D texture with its data
struct BntxTexture {
    uint32_t format = 0, width = 0, height = 0, gob_y = 0;
    uint64_t at = 0, size = 0;
};
bool bntx_texture(const std::vector<uint8_t>& d, BntxTexture* t) {
    static const char brti[4] = {'B', 'R', 'T', 'I'};
    const auto first = std::search(d.begin(), d.end(), brti, brti + 4);
    if (first == d.end() || std::search(first + 4, d.end(), brti, brti + 4) != d.end()) return false;
    const size_t i = static_cast<size_t>(first - d.begin());
    if (i + 0x78 > d.size()) return false;
    auto le = [&](size_t at, int n) {
        uint64_t v = 0;
        for (int b = n - 1; b >= 0; --b) v = v << 8 | d[at + b];
        return v;
    };
    const uint64_t tile = le(i + 0x12, 2), mips = le(i + 0x16, 2), table = le(i + 0x70, 8);
    t->format = static_cast<uint32_t>(le(i + 0x1C, 4));
    t->width = static_cast<uint32_t>(le(i + 0x24, 4)), t->height = static_cast<uint32_t>(le(i + 0x28, 4));
    t->gob_y = 1u << (le(i + 0x34, 4) & 7);
    t->size = le(i + 0x50, 4);
    if (tile != 0 || mips < 1 || table > d.size() || d.size() - table < 8) return false;
    t->at = le(static_cast<size_t>(table), 8);
    return t->at <= d.size() && t->size <= d.size() - t->at;
}

// randomize_units.py block_linear(): where block (bx, by) of a block-linear surface bw blocks wide sits, in bytes; inside a GOB the Switch
// 2's order (tegra_tiling.h gob_switch2: offset bits 4..8 = row bit 0, row bit 1, byte column bit 4, row bit 2, byte column bit 5)
uint64_t block_linear(uint64_t bx, uint64_t by, uint64_t bw, uint64_t gob_y, uint64_t bpb = 16) {
    const uint64_t x = bx * bpb, row_gobs = (bw * bpb + 63) / 64;
    return (by / (8 * gob_y)) * row_gobs * gob_y * 512 + (x / 64) * gob_y * 512 + ((by % (8 * gob_y)) / 8) * 512 + ((x % 64) / 32) * 256 +
           ((by % 8) / 4) * 128 + ((x % 32) / 16) * 64 + ((by % 4) / 2) * 32 + (by % 2) * 16 + (x % 16);
}

// randomize_units.py bc7_tint(): a BC7 block with each endpoint's red, green and blue times rgb / 255 (rounded, then the nearest the mode
// holds with the same p-bit; modes 4 and 5 with a rotation keep that channel in the alpha field); alpha, p-bits and indices kept
void bc7_tint(uint8_t* blk, const uint8_t rgb[3]) {
    // subsets, partition bits, rotation bits, index selection bits, colour bits, alpha bits, p-bit per endpoint, p-bit per subset
    static const int kModes[8][8] = {{3, 4, 0, 0, 4, 0, 1, 0}, {2, 6, 0, 0, 6, 0, 0, 1}, {3, 6, 0, 0, 5, 0, 0, 0}, {2, 6, 0, 0, 7, 0, 1, 0},
                                     {1, 0, 2, 1, 5, 6, 0, 0}, {1, 0, 2, 0, 7, 8, 0, 0}, {1, 0, 0, 0, 7, 7, 1, 0}, {2, 6, 0, 0, 5, 5, 1, 0}};
    auto get = [&](int pos, int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= uint32_t((blk[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
        return v;
    };
    auto put = [&](int pos, int n, uint32_t v) {
        for (int i = 0; i < n; ++i) {
            const uint8_t bit = uint8_t(1u << ((pos + i) & 7));
            blk[(pos + i) >> 3] = static_cast<uint8_t>((v >> i & 1) ? blk[(pos + i) >> 3] | bit : blk[(pos + i) >> 3] & ~bit);
        }
    };
    auto value = [](uint32_t q, int n) {
        q <<= 8 - n;
        return int(q | (q >> n));
    };
    if (blk[0] == 0) return;   // mode 8 (reserved)
    int mode = 0;
    while (!(blk[0] >> mode & 1)) ++mode;
    const int ns = kModes[mode][0], cb = kModes[mode][4], ab = kModes[mode][5], epb = kModes[mode][6], spb = kModes[mode][7];
    int pos = mode + 1 + kModes[mode][1];
    const uint32_t rot = get(pos, kModes[mode][2]);
    pos += kModes[mode][2] + kModes[mode][3];
    const int colour = pos, alpha = pos + 6 * ns * cb, pbits = alpha + 2 * ns * ab;
    for (int ch = 0; ch < 3; ++ch) {
        const bool swapped = rot == uint32_t(ch + 1);
        const int bits = swapped ? ab : cb;
        for (int s = 0; s < ns; ++s)
            for (int e = 0; e < 2; ++e) {
                const int at = swapped ? alpha + (s * 2 + e) * ab : colour + ((ch * ns + s) * 2 + e) * cb;
                const int p = epb ? int(get(pbits + 2 * s + e, 1)) : spb ? int(get(pbits + s, 1)) : -1;
                const int n = bits + (p >= 0);
                auto full = [&](uint32_t q) { return p >= 0 ? (q << 1 | uint32_t(p)) : q; };
                const int target = (value(full(get(at, bits)), n) * rgb[ch] + 127) / 255;
                uint32_t best = 0;
                for (uint32_t q = 1; q < (1u << bits); ++q)
                    if (std::abs(value(full(q), n) - target) < std::abs(value(full(best), n) - target)) best = q;
                put(at, bits, best);
            }
    }
}

// randomize_units.py bake_texture(): the texture (decompressed BNTX) with colours[skin, lip, hair] (4 bytes RGBA; empty: that kind left)
// baked into the blocks bake_blocks() picks from the rig's atlas and skeleton (decompressed), the meshes of skins (null: all); empty with
// *why when one of them does not read as expected
std::vector<uint8_t> bake_texture(const std::vector<uint8_t>& d, const std::vector<uint8_t>& atlas, const std::vector<uint8_t>& skel,
                                  const std::string (&colours)[3], const std::vector<std::string>* skins, std::string* why) {
    BntxTexture t;
    if (!bntx_texture(d, &t) || t.format != kBc7Unorm) {
        *why = "not one BC7 block-linear texture";
        return {};
    }
    int64_t width = 0, height = 0;
    std::map<std::string, AtlasRegion> regions;
    if (!atlas_regions(atlas, &width, &height, &regions) || width != t.width || height != t.height) {
        *why = "the atlas does not read as one page the texture's size";
        return {};
    }
    std::vector<SkelMesh> meshes;
    if (!skel_meshes(skel, skins, &meshes)) {
        *why = "the skeleton's skins do not read";
        return {};
    }
    const uint64_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
    if (bw == 0 || bh == 0 || block_linear(bw - 1, bh - 1, bw, t.gob_y) + 16 > t.size) {
        *why = "the texture data is too short";
        return {};
    }
    std::vector<uint8_t> out = d;
    int counts[3] = {0, 0, 0};
    for (const auto& [blk, k] : bake_blocks(width, height, regions, meshes)) {
        if (colours[k].size() != 4) continue;
        const uint8_t rgb[3] = {uint8_t(colours[k][0]), uint8_t(colours[k][1]), uint8_t(colours[k][2])};
        bc7_tint(out.data() + t.at + block_linear(uint64_t(blk) % bw, uint64_t(blk) / bw, bw, t.gob_y), rgb);
        ++counts[k];
    }
    *why = "blocks tinted: skin " + std::to_string(counts[0]) + ", lip " + std::to_string(counts[1]) + ", hair " + std::to_string(counts[2]) +
           " (" + std::to_string(meshes.size()) + " meshes)";
    return out;
}

// With bake (the source rig's atlas and skeleton, decompressed, and the skins to take), the texture gets the colours baked in.
std::vector<uint8_t> kasumi_rig_file(const std::string& kind, const std::vector<uint8_t>& d, const std::string& src, const std::string& dst,
                                     bool collapse_ends, const std::string (&colours)[3], const std::vector<uint8_t>* atlas,
                                     const std::vector<uint8_t>* skel, const std::vector<std::string>* skins, std::string* note) {
    std::vector<uint8_t> out = d;
    auto count = [](const std::vector<uint8_t>& hay, const std::string& needle, size_t* first) {
        size_t n = 0;
        for (auto it = hay.begin();;) {
            it = std::search(it, hay.end(), needle.begin(), needle.end());
            if (it == hay.end()) return n;
            if (!n) *first = static_cast<size_t>(it - hay.begin());
            ++n;
            it += static_cast<std::ptrdiff_t>(needle.size());
        }
    };
    if (kind == "atlas.zst") {
        const std::string want = src + ".png\n";
        for (size_t i = 0; i < want.size(); ++i)
            if (i >= d.size() || i >= sizeof kAtlasKey || (d[i] ^ kAtlasKey[i]) != static_cast<uint8_t>(want[i])) {
                *note = "the first line is not " + src + ".png";
                return {};
            }
        for (size_t i = 0; i < src.size(); ++i) out[i] ^= static_cast<uint8_t>(src[i] ^ dst[i]);
        *note = "page " + dst + ".png";
        return out;
    }
    if (kind == "bntx.zst") {
        const std::string old = std::string("\x03", 1) + std::string(1, '\0') + src + std::string(1, '\0');
        size_t at = 0;
        if (count(d, old, &at) != 1) {
            *note = "the texture name " + src + " is not there once";
            return {};
        }
        std::copy(dst.begin(), dst.end(), out.begin() + static_cast<std::ptrdiff_t>(at + 2));
        *note = "texture " + dst;
        if (atlas && skel) {
            std::string why;
            std::vector<uint8_t> baked = bake_texture(out, *atlas, *skel, colours, skins, &why);
            *note += (baked.empty() ? "; not baked: " : "; ") + why;
            if (!baked.empty()) out.swap(baked);
        }
        return out;
    }
    if (kind == "spdb") {
        for (const char* ext : {".atlas", ".skel"}) {
            const std::string old = src + "/" + src + ext, nw = dst + "/" + dst + ext;
            size_t at = 0;
            if (count(out, old, &at) != 1) {
                *note = old + " is not there once";
                return {};
            }
            std::copy(nw.begin(), nw.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
        }
        *note = dst + "/" + dst + ".atlas, " + dst + "/" + dst + ".skel";
        return out;
    }
    if (kind == "skel.zst") {
        std::vector<std::pair<std::string, size_t>> slots;
        if (!skel_slot_colours(d, &slots)) {
            *note = "not a Spine 4.1 skeleton";
            return {};
        }
        int counts[3] = {0, 0, 0};
        for (const auto& [name, at] : slots)
            for (int k = 0; k < 3; ++k) {
                const std::string suffix = kTintSlots[k];
                const std::string& c = colours[k];
                if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0 && !c.empty()) {
                    std::copy(c.begin(), c.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
                    ++counts[k];
                }
            }
        auto hex = [](const std::string& c) {
            if (c.empty()) return std::string("none");
            char buf[9];
            std::snprintf(buf, sizeof buf, "%02x%02x%02x%02x", uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]), uint8_t(c[3]));
            return std::string(buf);
        };
        *note = "slots coloured: skin " + std::to_string(counts[0]) + " (" + hex(colours[0]) + "), lip " + std::to_string(counts[1]) + " (" +
                hex(colours[1]) + "), hair " + std::to_string(counts[2]) + " (" + hex(colours[2]) + ")";
        if (collapse_ends) {   // each clip's end marker (an unweighted Clipping_End mesh) with every vertex at 0, 0: it draws nothing
            int collapsed = 0, kept = 0;
            for (const auto& entry : clipping_entries(out, slots)) {
                size_t q = entry[1];
                uint32_t v = 0, vc = 0, tris = 0;
                auto varint = [&](uint32_t* x) {
                    *x = 0;
                    for (int shift = 0; q < out.size(); shift += 7) {
                        const uint8_t b = out[q++];
                        *x |= uint32_t(b & 0x7F) << shift;
                        if (!(b & 0x80) || shift >= 28) break;
                    }
                };
                varint(&v);   // key
                varint(&v);   // name
                if (q >= out.size() || out[q] != 2) {   // a clipping attachment: kept
                    ++kept;
                    continue;
                }
                ++q;
                varint(&v);   // path
                q += 4;       // colour
                varint(&vc);
                q += 8ull * vc;   // uvs
                varint(&tris);
                q += 2ull * tris;
                if (q < out.size() && out[q] == 0 && out.size() - q - 1 >= 8ull * vc) {   // unweighted: x, y for each vertex
                    const auto from = out.begin() + static_cast<std::ptrdiff_t>(q + 1);
                    std::fill(from, from + static_cast<std::ptrdiff_t>(8ull * vc), uint8_t(0));
                    ++collapsed;
                }
            }
            *note += "; clip end markers collapsed: " + std::to_string(collapsed) + " (" + std::to_string(kept) + " clipping attachments kept)";
        }
        return out;
    }
    *note = "not a rig file";
    return {};
}

// "/ui/face/900/900.<kind>" or 901: the kind, and *dst the folder; empty for another file
std::string kasumi_rig_kind(const std::string& key, std::string* dst = nullptr) {
    for (const char* folder : {kKasumiFace, kKasumiFaceUw}) {
        const std::string pre = std::string("/ui/face/") + folder + "/" + folder + ".";
        if (key.rfind(pre, 0) != 0) continue;
        const std::string kind = key.substr(pre.size());
        if (std::none_of(std::begin(kRigFiles), std::end(kRigFiles), [&](const char* k) { return kind == k; })) return std::string();
        if (dst) *dst = folder;
        return kind;
    }
    return std::string();
}
// the files edited after AssetTable.bin, whose edit reads the colours: 900's skeleton (the slots) and 901's texture (baked in); 901's atlas
// and skeleton go before them (they keep the source files for the texture)
bool kasumi_rig_late(const std::string& kind, const std::string& dst) {
    return (dst == kKasumiFace && kind == "skel.zst") || (dst == kKasumiFaceUw && kind == "bntx.zst");
}
bool match_kasumi_rig(const std::string& key) {
    std::string dst;
    const std::string kind = kasumi_rig_kind(key, &dst);
    return !kind.empty() && !kasumi_rig_late(kind, dst);
}
bool match_kasumi_late(const std::string& key) {
    std::string dst;
    const std::string kind = kasumi_rig_kind(key, &dst);
    return !kind.empty() && kasumi_rig_late(kind, dst);
}

void edit_kasumi_rig(const NxModFile* file, void*) {
    std::string key = file->path ? file->path : "";
    for (char& c : key) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    std::string dst;
    const std::string kind = kasumi_rig_kind(key, &dst);
    const bool zst = kind.size() > 4 && kind.compare(kind.size() - 4, 4, ".zst") == 0;
    std::vector<uint8_t> data(file->data, file->data + file->size);
    if (zst) {
        const unsigned long long size = ZSTD_getFrameContentSize(file->data, static_cast<size_t>(file->size));
        if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64ull << 20)) {
            NXLOG_W(kTag, "randomizer: %s: not a zstd frame with its size: left as it is", file->path);
            return;
        }
        data.assign(static_cast<size_t>(size), 0);
        const size_t got = ZSTD_decompress(data.data(), data.size(), file->data, static_cast<size_t>(file->size));
        if (ZSTD_isError(got) || got != size) {
            NXLOG_W(kTag, "randomizer: %s: does not decompress: left as it is", file->path);
            return;
        }
    }
    if (kasumi_rig_late(kind, dst) && !g_kasumi_coloured)
        NXLOG_W(kTag, "randomizer: %s: edited before AssetTable.bin, so without the colours", file->path);
    const bool uw = dst == kKasumiFaceUw;
    if (uw && kind == "atlas.zst") g_kasumi_uw_atlas = data;
    if (uw && kind == "skel.zst") g_kasumi_uw_skel = data;
    // the Underworld rig: her colours in the texture, the tinted slots white
    static const std::string kWhite[3] = {std::string(4, '\xff'), std::string(4, '\xff'), std::string(4, '\xff')};
    const bool bake = uw && kind == "bntx.zst";
    if (bake && (g_kasumi_uw_atlas.empty() || g_kasumi_uw_skel.empty()))
        NXLOG_W(kTag, "randomizer: %s: its atlas or skeleton was not seen first: not baked", file->path);
    const bool baked = bake && !g_kasumi_uw_atlas.empty() && !g_kasumi_uw_skel.empty();
    const std::vector<std::string> skins = {"default", g_kasumi.face_skin()};
    std::string note;
    const std::string(*colours)[3] = uw && kind == "skel.zst" ? &kWhite : &g_kasumi_colours;
    std::vector<uint8_t> out = kasumi_rig_file(kind, data, g_kasumi.rig(), dst, uw && kind == "skel.zst", *colours,
                                               baked ? &g_kasumi_uw_atlas : nullptr, baked ? &g_kasumi_uw_skel : nullptr, &skins, &note);
    NXLOG_I(kTag, "randomizer: UI/Face/%s/%s.%s: from %s: %s", dst.c_str(), dst.c_str(), kind.c_str(), g_kasumi.rig().c_str(), note.c_str());
    if (out.empty()) return;
    if (zst) {
        std::vector<uint8_t> comp(ZSTD_compressBound(out.size()));
        const size_t csize = ZSTD_compress(comp.data(), comp.size(), out.data(), out.size(), 3);
        if (ZSTD_isError(csize)) {
            NXLOG_W(kTag, "randomizer: %s: does not compress again: left as it is", file->path);
            return;
        }
        comp.resize(csize);
        out.swap(comp);
    }
    file->replace(file, out.data(), out.size());
}

// ---- quote speakers: Msg/<language>/Levelup.msbt and Die.msbt ---------------------------------------------------------------------------
// randomize_units.py rewrite_speakers(), step for step. A level-up, death or retreat quote (MID_LevelUp_[Min_|Max_]<voice>_NN,
// MID_Skill_LevelUp_[Max_]<voice>_NN, MID_DIE_<voice>_NN, MID_RETREAT_<voice>_NN) is picked by the unit's voice code, which follows its
// look, but names its speaker in attribute field 3 (クラプカ), and the game shows that speaker's slot, which wears someone else. Each such
// line (by its LBL1 label: the story's own deaths, MID_EV_DIE_* and MID_DIE_SCXX_*, name a role and stay) naming a speaker of g_speakers
// names the slot that wears its look instead: a string the ATR1 string area already has (its first), else one appended to it; ATR1 grows
// and the sections after it move. Empty when nothing changes or the file is not laid out so.
constexpr size_t kSpeakerField = 12;   // the speaker's string offset in an ATR1 record
const char* const kQuotePrefixes[] = {"MID_LevelUp_",       "MID_LevelUp_Min_", "MID_LevelUp_Max_", "MID_Skill_LevelUp_",
                                      "MID_Skill_LevelUp_Max_", "MID_DIE_",      "MID_RETREAT_"};

// A voice code (letters, a digit, at most one letter: Kra0, Ism0F), _, digits
bool voice_and_number(const std::string& rest) {
    auto alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t i = 0;
    while (i < rest.size() && alpha(rest[i])) ++i;
    if (i == 0 || i >= rest.size() || !digit(rest[i])) return false;
    ++i;
    if (i < rest.size() && alpha(rest[i])) ++i;
    if (i >= rest.size() || rest[i] != '_' || i + 1 >= rest.size()) return false;
    for (size_t k = i + 1; k < rest.size(); ++k)
        if (!digit(rest[k])) return false;
    return true;
}
bool is_quote_label(const std::string& label) {
    for (const char* pre : kQuotePrefixes)
        if (label.rfind(pre, 0) == 0 && voice_and_number(label.substr(std::strlen(pre)))) return true;
    return false;
}

std::vector<uint8_t> rewrite_speakers(const std::vector<uint8_t>& d, const std::unordered_map<std::string, std::string>& renames, int* changed) {
    *changed = 0;
    auto u32 = [&](const std::vector<uint8_t>& b, size_t at) {
        uint32_t v = 0;
        std::memcpy(&v, b.data() + at, 4);
        return v;
    };
    if (d.size() < 0x20 || std::memcmp(d.data(), "MsgStdBn\xff\xfe", 10) != 0 || d[0xC] != 0) return {};
    uint16_t nsec = 0;
    std::memcpy(&nsec, d.data() + 0xE, 2);
    size_t p = 0x20, at = 0, size = 0, lat = 0, lsize = 0;
    bool found = false, lfound = false;
    for (uint16_t i = 0; i < nsec; ++i) {
        if (p + 16 > d.size()) return {};
        const uint32_t n = u32(d, p + 4);
        if (std::memcmp(d.data() + p, "ATR1", 4) == 0) at = p, size = n, found = true;
        else if (std::memcmp(d.data() + p, "LBL1", 4) == 0) lat = p, lsize = n, lfound = true;
        p = (p + 16 + n + 15) & ~size_t(15);
    }
    if (!found || !lfound || at + 16 + size > d.size() || lat + 16 + lsize > d.size()) return {};
    std::vector<uint32_t> quotes;   // the indices of the lines whose label is a quote's
    {
        const std::vector<uint8_t> lb(d.begin() + static_cast<std::ptrdiff_t>(lat + 16), d.begin() + static_cast<std::ptrdiff_t>(lat + 16 + lsize));
        if (lb.size() < 4) return {};
        const uint64_t slots = u32(lb, 0);
        if (4 + 8 * slots > lb.size()) return {};
        for (uint64_t i = 0; i < slots; ++i) {
            const uint32_t n = u32(lb, static_cast<size_t>(4 + 8 * i));
            size_t q = u32(lb, static_cast<size_t>(8 + 8 * i));
            for (uint32_t k = 0; k < n; ++k) {
                if (q >= lb.size() || q + 1 + lb[q] + 4 > lb.size()) return {};
                const std::string name(reinterpret_cast<const char*>(lb.data() + q + 1), lb[q]);
                if (is_quote_label(name)) quotes.push_back(u32(lb, q + 1 + lb[q]));
                q += 1 + lb[q] + 4;
            }
        }
        std::sort(quotes.begin(), quotes.end());
    }
    std::vector<uint8_t> area(d.begin() + static_cast<std::ptrdiff_t>(at + 16), d.begin() + static_cast<std::ptrdiff_t>(at + 16 + size));
    if (area.size() < 8) return {};
    const uint64_t count = u32(area, 0), rec = u32(area, 4), recs_end = 8 + count * rec;
    if (rec < kSpeakerField + 4 || recs_end > area.size()) return {};
    std::unordered_map<std::string, size_t> starts;   // text -> its first offset in the string area
    for (size_t i = recs_end; i < area.size();) {
        const auto z = std::find(area.begin() + static_cast<std::ptrdiff_t>(i), area.end(), uint8_t(0));
        if (z == area.end()) return {};
        const size_t end = static_cast<size_t>(z - area.begin());
        starts.emplace(std::string(reinterpret_cast<const char*>(area.data() + i), end - i), i);
        i = end + 1;
    }
    for (uint64_t k = 0; k < count; ++k) {
        const size_t f = static_cast<size_t>(8 + k * rec + kSpeakerField);
        const uint32_t v = u32(area, f);
        if (v == 0 || v == 0xFFFFFFFFu || !std::binary_search(quotes.begin(), quotes.end(), static_cast<uint32_t>(k))) continue;
        if (v < recs_end || v >= area.size() || (v > recs_end && area[v - 1] != 0)) {
            *changed = 0;
            return {};
        }
        const auto z = std::find(area.begin() + v, area.end(), uint8_t(0));
        const std::string old(reinterpret_cast<const char*>(area.data() + v), static_cast<size_t>(z - area.begin()) - v);
        const auto it = renames.find(old);
        if (it == renames.end() || it->second == old) continue;
        auto s = starts.find(it->second);
        if (s == starts.end()) {
            s = starts.emplace(it->second, area.size()).first;
            area.insert(area.end(), it->second.begin(), it->second.end());
            area.push_back(0);
        }
        const uint32_t nv = static_cast<uint32_t>(s->second);
        std::memcpy(area.data() + f, &nv, 4);
        ++*changed;
    }
    if (!*changed) return {};
    std::vector<uint8_t> out(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(at + 16));
    const uint32_t asz = static_cast<uint32_t>(area.size());
    std::memcpy(out.data() + at + 4, &asz, 4);
    out.insert(out.end(), area.begin(), area.end());
    while (out.size() % 16) out.push_back(0xAB);
    out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(std::min(d.size(), (at + 16 + size + 15) & ~size_t(15))), d.end());
    const uint32_t total = static_cast<uint32_t>(out.size());
    std::memcpy(out.data() + 0x12, &total, 4);
    return out;
}

// "/msg/<language>/levelup.msbt.zst" and "/msg/<language>/die.msbt.zst"
bool match_quotes(const std::string& key) {
    for (const std::string tail : {"/levelup.msbt.zst", "/die.msbt.zst"})
        if (key.rfind("/msg/", 0) == 0 && key.size() > tail.size() + 5 && key.compare(key.size() - tail.size(), tail.size(), tail) == 0 &&
            key.find('/', 5) == key.size() - tail.size())
            return true;
    return false;
}

void edit_quotes(const NxModFile* file, void*) {
    std::string path = file->path ? file->path : "";
    if (!path.empty() && path[0] == '/') path.erase(0, 1);
    if (!g_mapped) {
        NXLOG_W(kTag, "randomizer: %s: edited before Person.bin, so without the mapping: quote speakers left as they are", path.c_str());
        return;
    }
    const unsigned long long size = ZSTD_getFrameContentSize(file->data, static_cast<size_t>(file->size));
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64ull << 20)) {
        NXLOG_W(kTag, "randomizer: %s: not a zstd frame with its size: left as it is", path.c_str());
        return;
    }
    std::vector<uint8_t> msbt(static_cast<size_t>(size));
    const size_t got = ZSTD_decompress(msbt.data(), msbt.size(), file->data, static_cast<size_t>(file->size));
    if (ZSTD_isError(got) || got != size) {
        NXLOG_W(kTag, "randomizer: %s: does not decompress: left as it is", path.c_str());
        return;
    }
    int changed = 0;
    const std::vector<uint8_t> out = rewrite_speakers(msbt, g_speakers, &changed);
    NXLOG_I(kTag, "randomizer: %s: %d quote lines name the unit that wears their look", path.c_str(), changed);
    if (out.empty()) return;
    std::vector<uint8_t> comp(ZSTD_compressBound(out.size()));
    const size_t csize = ZSTD_compress(comp.data(), comp.size(), out.data(), out.size(), 3);
    if (ZSTD_isError(csize)) {
        NXLOG_W(kTag, "randomizer: %s: does not compress again: left as it is", path.c_str());
        return;
    }
    file->replace(file, comp.data(), csize);
}

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
    // randomize_eshmel (experimental): off, on, or hood (debug: always the easter egg)
    const std::string eshmel = lower_nospace(config_get_str("fefw", "randomize_eshmel", "off"));
    if (eshmel == "on" || eshmel == "true" || eshmel == "1" || eshmel == "yes") g_set.eshmel = 1;
    else if (eshmel == "hood") g_set.eshmel = 2;
    else if (!(eshmel.empty() || eshmel == "off" || eshmel == "false" || eshmel == "0" || eshmel == "no"))
        NXLOG_W(kTag, "randomizer: randomize_eshmel = \"%s\" is not off, on or hood: off", eshmel.c_str());
    if (g_set.eshmel) g_kasumi = draw_kasumi(g_set.seed, g_set.eshmel == 2);
    msg_names_install();   // Katarina's name (a name ID but no text) and Kasumi's (ObeliskChamber_msg.cpp)
    if (!services::modapi::add_runtime_edit("randomizer", &match_eye_rig, &edit_eye_rig, nullptr))
        NXLOG_W(kTag, "randomizer: too late to rename the portrait rigs' eye slots");
    if (g_set.eshmel) {
        // the second Eshmel's portrait rigs: copies of 015's or 016's four files in UI/Face/900 and 901 (Underworld),
        // edited as they load (900's skeleton and 901's texture after AssetTable.bin, whose edit reads the colours)
        const std::string src = g_kasumi.rig();
        bool ok = true;
        for (const std::string dst : {kKasumiFace, kKasumiFaceUw})
            for (const char* kind : kRigFiles)
                ok = services::modapi::add_runtime_copy("randomizer", "/UI/Face/" + src + "/" + src + "." + kind,
                                                        "/UI/Face/" + dst + "/" + dst + "." + kind) && ok;
        ok = services::modapi::add_runtime_edit("randomizer", &match_kasumi_rig, &edit_kasumi_rig, nullptr) && ok;
        ok = services::modapi::add_runtime_edit("randomizer", &match_kasumi_late, &edit_kasumi_rig, nullptr, true) && ok;
        if (!ok) NXLOG_W(kTag, "randomizer: too late to add the second Eshmel's portrait rig");
    }
    if (!services::modapi::add_runtime_edit("randomizer", &match_quotes, &edit_quotes, nullptr, true))
        NXLOG_W(kTag, "randomizer: too late to rewrite the level-up, death and retreat speakers");
    const bool ok = services::modapi::add_runtime_edit("randomizer", &match_person, &edit_person, nullptr) &&
                    services::modapi::add_runtime_edit("randomizer", &match_assets, &edit_assets, nullptr, true);
    NXLOG_I(kTag, "randomizer: on, seed %" PRIu64 "%s%s%s%s", g_set.seed, g_set.force.empty() ? "" : ", forced ", g_set.force.c_str(),
            g_set.eshmel == 2 ? ", second Eshmel (hood)" : g_set.eshmel ? ", second Eshmel" : "",
            ok ? " (GameData/Person.bin and AssetTable.bin are edited as they load)" : ": too late to edit the files, applies at the next start");
}

}  // namespace nxrt::games::ObeliskChamber
