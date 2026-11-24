# Unit randomizer: handoff note

Status as of 2026-11-24, branch `claude/epic-curie-oyae0z`. Read this first when picking the work up.

## The goal

An Ignis-style (FE14, https://github.com/thane98/ignis) unit randomizer for Fortune's Weave, built into the runtime.

- **Who is swapped:** characters with human models (playable units, and story characters such as Dagda and Lyrah in their
  human forms). Dragon forms are never in the pool.
- **Where it applies:** everywhere a character is met in the story, including as an enemy. Example: in Dietrich Ch. 3 you
  fight Anatolia's squad, so whoever replaces Anatolia leads that squad.

**First milestone: swap logic only.** No stats, classes, items, portraits or dialogue.

- **Test pool:** the 4 lords, Hong Hua, Troy and Lind. The owner will say who the lords are once the data is readable.
- **Replacements:** each pool unit is replaced by a unit picked at random (seeded) from every eligible human-model unit.
  No two pool units get the same replacement: the full design is a one-to-one swap.
- **Duplicates in this test:** units outside the test pool keep their defaults, so a replacement can still meet the original
  of itself in the story. That is fine for now.
- **Testing:** stops at Prologue Battle 2. It only has to show that playable units and enemy placements are both swapped.

## Decided

- **Packaging:** built into the game module as a `[fefw]` option, not as an external DLL mod. A new
  `runtime/games/ObeliskChamber/ObeliskChamber_randomizer.cpp` modelled on `ObeliskChamber_detail.cpp`, also added to that
  folder's `CMakeLists.txt`.
- **ini keys:**
  - `randomize_units`: off/on, default off.
  - `randomize_seed`: 0 means pick one and write it back with `config_write()`.
  - `randomize_force`: a debug list such as `Anatolia=Troy,Lind=Hong Hua` that overrides the random picks, so a specific
    fight can be checked.
  - Add rows for all three to `runtime/docs/CONFIG.md`.
- **Mapping:**
  - A seeded splitmix64 of our own; the repo has no RNG to reuse.
  - `original -> replacement`, drawn without repeats.
  - The whole mapping is logged at startup.
- **Out of scope for now:**
  - stats, classes, items, portraits, name text
  - event scripts that name a character directly (only if testing shows they matter)
  - F1, Recomp Options and launcher rows
  - DLC (runtime edits do not reach the DLC images)

## How the runtime can change the data

`services::modapi::add_runtime_edit(owner, match, fn, user)` lets the runtime replace a RomFS file's bytes as the RomFS image is
built.

- **Where it lives:** `runtime/src/services/mods_dll.cpp:589`, declared in `runtime/include/nxrt/services_api.h:93`.
- **When it applies:** register it in `play_install` (`runtime/games/ObeliskChamber/ObeliskChamber_play.cpp`, next to
  `detail_install()`), before the game's first file access. Edits apply at the next start.

Two working examples to copy from:

- `ObeliskChamber_detail.cpp`: a MessagePack walker (`Reader`, `Scalar{path, at, type}`) that edits values in place.
- `ObeliskChamber_msg.cpp`: handles zstd (detect, decompress, compress again) and rebuilds an MSBT. Keep `match` functions narrow:
  matching a whole folder once cost 19 s at startup.

The runtime builds on Windows only. A Linux session can write the code and the Python tools; building and in-game tests are on
the owner's PC.

## The game's data: what is known

The owner's dump is `131072/romfs` (update 131072). Character and map data sit in `romfs/GameData/*.bin`.

Candidates to examine (formats not yet confirmed):

| File | Size | Probably |
|---|---|---|
| `GameData/Person.bin` | 2.4 MB | the character table |
| `GameData/Legion.bin` | 42 KB | enemy squads / armies |
| `GameData/Replacement.bin` | 8 KB | unit replacement rules (worth checking first) |
| `GameData/Chapter.bin` | 6 KB | chapters |
| `GameData/Job.bin` | 154 KB | classes |
| `GameData/AssetTable.bin`, `FaceSet.bin` | 3.5 MB, 243 KB | models and portraits per character (human vs dragon form) |
| `GameData/Dispos/*.bin` | 3-94 KB each | map placements, one per battle: `SC00_00`..`SC00_16` (probably the Prologue), `SC11_*`..`SC14_*`, `SC30_*`, `SCXX_*`, `M000`, `M001`, `DB_*`, `KL_*`, `Tr*E_*` |
| `GameData/HubDispos/` | | hub placements (not needed for battles) |
| `Msg/<language>/*.msbt(.zst)` | | names and text |

Also seen:

- Route tags: Dit (Dietrich), Kai, Led, Seo and Ism, from `Timeline_Part1_<route>.bin` and the save-file names.
- `patches/README.md` warns about a crash when a unit has a mount but its class has no dismounted class (`unitinfo_animal_crashfix`).
  Keep this in mind when classes are swapped later.

**ReWeaver** (by wicsm, https://gamebanana.com/tools/24806) is a Windows editor for these tables, and the owner has v0.1.1. It is
useful for checking IDs and names by eye. A Linux session cannot run it.

## Done so far

`tools/gamedata/inspect_romfs.py`, with tests in `tools/gamedata/test_inspect_romfs.py`. Run the tests with
`python -I tools/gamedata/test_inspect_romfs.py`. The tool has three commands:

- `list <romfs>`: every file, its size and its format.
- `find <romfs> words...`: which files contain the words, in UTF-8 or UTF-16, after zstd.
- `dump <file>`: decodes MessagePack, MSBT or SARC to JSON. Anything else comes out as hex.

zstd needs `pip install zstandard`, or the `zstd` program.

## Next steps

1. Get the files above into the session as files. A connector that pastes text cannot carry a 2.4 MB binary. Never commit
   game files to this repo; see LEGAL.md.
2. Run `inspect_romfs.py dump` on `Person.bin`, `Replacement.bin`, `Legion.bin` and one `Dispos/SC00_*.bin` to learn their
   formats. Extend the tool if a `.bin` is not MessagePack.
3. Work out:
   - the character ID field
   - how a placement refers to a character
   - how dragon forms differ from human forms
   - which `Dispos` files are Prologue Battles 1 and 2
   - where the test-pool units are fought (Dietrich Ch. 3 for Anatolia)
4. Ask the owner to confirm the 4 lords' IDs.
5. Write the mapping and the edits in Python against the real files first, as a test oracle. Then port them to
   `ObeliskChamber_randomizer.cpp`.
6. In-game check up to Prologue Battle 2. Use `randomize_force` to put a known unit in a known prologue slot, and check the log's
   mapping line and its `randomizer: <file> edited` lines.
