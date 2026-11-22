# Obelisk Chamber 0.3.1.1t (Test Build)

Test build for:
- **memory problems in long sessions** on PCs with 8–16 GB of RAM or a 4–8 GB graphics card (#126, #155, #162);
- **the crash before the first frame on AMD** (#157).

Not for normal play: stay on 0.3.1 if you don't have one of these problems.

> [!NOTE]
> **New numbering for test builds.** 0.3.1.1t is test build **1** of version **0.3**, update **1**:
> - the first number is 0 until the project is done;
> - the second is the version (3);
> - the third is the update within it (1);
> - the fourth is the test build (1).
>
> Test builds no longer take numbers that a regular release uses later.

## Download
- **Windows:** `ObeliskChamber-0.3.1.1t-win64.zip`
- **Linux:** `ObeliskChamber-0.3.1.1t-linux-x64.tar.gz` (runs the game through Proton or Wine, as 0.3.1 does)

Unpack into a **new folder** and press **Set up**. Point it at the same game dump as before; your saves carry over.

## What's being tested

### 1. Memory limits sized to your PC (#126, #155, #162)
In long sessions memory kept growing until the game slowed down, froze for a while, or showed a black-and-white static picture until a restart. We traced it to three causes:

- **The game keeps a large cache of the files it has loaded**: up to 4.7 GB, the same as on the console. Every texture in a cached file also stays in RAM and in video memory.
- **Our own buffers were sized for big PCs**: 256 MB for texture uploads, up to 1 GB for texture preparation.
- **Texture residency** (which shrinks textures nothing has drawn for a while) **only looked at our own count of video memory.** On a 4 GB card our count stayed at 70% while Windows counted 96%, so it never acted.

A new switch sizes all of this to your PC. **It is off unless you turn it on** (see below).

| | 8 GB PC | 16 GB PC | switch off |
|---|---|---|---|
| The game's file cache | 2 GB | 4 GB | 4.7 GB |
| Texture upload buffer | 64 MB | 128 MB | 256 MB |
| Texture preparation | 256 MB | 512 MB | 1 GB |
| Texture residency | also watches Windows' figure on cards with 8 GB or less | same | our own count only |

On our test PC it saved about 0.5 GB of RAM and 0.6 GB of video memory on a two-minute route, with the same frame rate. The difference should be larger in long sessions, which is what we need you to check.

The log also shows more now:
- `RAM:` lines at every load: where the RAM goes;
- `file cache:` lines every 30 s: how full the game's file cache is.

### 2. GPU markers on AMD (#157)
`[gfx] gpu_checkpoints = true` now works on AMD cards too (through `VK_AMD_buffer_marker`). When the GPU stops, the crash file names the draw it was on.

## How to test

**Where the settings go:** `ObeliskChamber.ini`, in the game folder. If Windows hides file extensions, it is the file whose type says *Configuration settings*.

**Memory (#126, #155, #162):**
1. Add these two lines at the end of `ObeliskChamber.ini`:
   ```
   [memory]
   scale_limits = true
   ```
2. Play the way the problem showed up before: a long session, an hour or more if you can.
3. Close the game normally, then attach `ObeliskChamber.log` to your issue. Also attach `ObeliskChamber_freeze.txt` or `ObeliskChamber_crash.txt` if there is one.

**Please send the log even if everything went fine.** It tells us how far the limits went on your PC. If the game behaves worse with the switch on, set `scale_limits = false`, play again, and tell us what you saw.

**AMD crash at start (#157):**
1. In the `[gfx]` section of `ObeliskChamber.ini`, set `gpu_checkpoints = true`.
2. Remove the test lines from earlier rounds (`intel_compat`, `pool_slabs`, `replay_threads`, `float_atomic_add`, `pageable_memory`, `amd_overallocation`).
3. Start the game. When it crashes, attach `ObeliskChamber.log` and `ObeliskChamber_crash.txt`.

## What's Changed

* Memory: a RAM line at every load, the game's file cache line, `[fefw] file_cache_mb`, and `[memory] scale_limits` (#172). Not merged yet: this build is made from PR #172's branch.
* GPU markers on AMD: `[gfx] gpu_checkpoints` uses `VK_AMD_buffer_marker` where NVIDIA's extension is missing (#171). Not merged yet.

**Full Changelog**: https://github.com/xinthefreefallxai/ObeliskChamber/compare/v0.3.1...v0.3.1.1t
