# Settings guide

This guide covers the settings a player might want to change, says what each one does, and says **who gains from changing it**.
For most people the defaults are right. The launcher's **Settings**, the **F1** menu and **+ > System > Recomp Options** in the
game cover the common ones, so you rarely need to edit a file by hand.

Every setting, including the developer and test keys left out here, is listed in
[runtime/docs/CONFIG.md](../runtime/docs/CONFIG.md).

## Where settings live

- **`ObeliskChamber.ini`** (in the game folder) holds most keys, under sections such as `[fefw]`, `[gfx]` and `[nvn]`.
- **`controls.ini`** (in the run folder) holds the keys the menus write for picture and input: `[hdr]`, `[upscale]`, `[latency]`,
  `[picture]`, `[display]`, `[fps]`, `[pad]` and the key bindings. In play, the game reads these sections from `controls.ini`, not
  from `ObeliskChamber.ini`.
- **Environment variables** win over both: `NXRT_` + section + `_` + key in capitals, for example `NXRT_NVN_REPLAY_THREADS=1`.
- A key you set by hand always wins over an `auto` default. If you copied lines from a guide or an old test build, remove them when
  you no longer need them, or they keep overriding newer defaults.

Most `auto` defaults look at your hardware and write what they chose to `ObeliskChamber.log`, so the log tells you what you got.

## Quick picks: who should change what

| If you have... | Try | Why |
|---|---|---|
| **A slower GPU** (GTX 10 / 16 series, RX 500 / 5000, an integrated GPU, a handheld PC) | The **Low** graphics preset; `[dlss] upscaler` = FSR 3 or DLSS if you have the files; dynamic resolution | Low renders at half size per side, lowers shadows to 1024, uses the handheld quality and turns down reflections, ambient occlusion, crowd and scenery detail |
| **Little video memory** (6 GB or less) | Keep `[gfx] texture_detail`, `astc_bc` and `residency` at `auto`; lower `[fefw] shadow_size` | The `auto` values already halve the largest textures and store opaque ones at half size on small cards |
| **A 4-core CPU, or an older one without FMA3** | Leave the CPU keys at `auto`. If loading still stutters, try `[nvn] compile_pace_ms = 12` | `replay_threads`, `wait_spin`, `gpl_cold` and the compile thread counts already scale down on small CPUs. Set up builds the compatible game code for CPUs without FMA3 by itself |
| **A fast CPU with 8 or more cores** | Nothing: `wait_spin` and `replay_threads = 2` are on for you | |
| **An Intel GPU** (Arc or integrated) | Nothing. Don't copy old test lines into your ini | `[gfx] intel_compat = auto` turns off the paths that lost the GPU on Intel. A line you set by hand undoes that |
| **An AMD GPU** | Nothing. If you see corrupted pictures, report it with your log | The tracked GPU barrier set (`[nvn] dispatch_barriers`, `game_barriers`) has only been tested on NVIDIA |
| **An RTX 20 or 30 card with DLSS** | Leave `[upscale] dlss_preset_auto` on | It picks DLSS model E, which costs these cards less GPU time than the newer default model |
| **An ultrawide monitor** | Pick your output in the 21:9 or 32:9 group; `[fefw] ultrawide_images` = `fill` or `fit` | `fill` crops full-screen pictures to cover the screen, `fit` keeps them whole with bars |
| **An HDR TV or monitor** | `[hdr] mode = native` | The game's own HDR picture, as on a Switch 2 on an HDR TV |
| **A 120 Hz or faster display** | `[fefw] fps120 = 1` (F1 > Frame rate) | `frame_fixes` keeps capes, hair, physics, dialogue typing and the map at their real speed at 60 and 120 fps |
| **Linux / Steam Deck** | The **Low** preset, or **Medium** with a lower render scale | The Deck's GPU is in the integrated-GPU group above |

## Graphics presets

A preset is not a key of its own. Picking one (launcher **Picture** tab, F1 **Graphics**, or Recomp Options **Graphics**) writes
the seven keys below. If you change one of them yourself, the menus show **Custom**.

| Key (`[fefw]`) | Low | Medium (the console's picture) | High | Ultra |
|---|---|---|---|---|
| `render_ratio` | 0.5 | 0 (the game's own, 0.5) | 0.67 | 1.0 (DLAA) |
| `shadow_size` | 1024 | 0 (2048) | 4096 | 8192 |
| `quality` | handheld | console | docked | docked |
| `reflections` | reduced | full | full | full |
| `ambient_occlusion` | 0 | 1 | 1 | 1 |
| `crowd_detail` | reduced | full | full | full |
| `scenery_detail` | reduced | full | full | full |

## Picture and graphics detail (`[fefw]`, `ObeliskChamber.ini`)

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `output` | `console` | The game's output size: `console` (2560x1440 docked), `720p` to `2160p`, `window`, or a `WxH` | You want 4K output or an ultrawide size. The size sets the picture's shape, not the window |
| `render_ratio` | `0` | Render size as a share of the output per side (0.25 to 1.0). `0` is the game's own 0.5 | Lower it on a slow GPU. `1.0` on a fast one: the upscaler then only anti-aliases |
| `render` | empty | A fixed render size `WxH`; wins over `render_ratio` | You want an exact size |
| `shadow_size` | `0` | Shadow map size, 512 to 8192. `0` is the game's (2048 docked) | Lower it on a slow GPU or with little video memory; 4096 or 8192 look sharper on a fast one |
| `quality` | `console` | `docked` keeps docked shadows and detail everywhere; `handheld` uses the console's handheld cuts | `handheld` on a slow GPU |
| `reflections` | `full` | Screen-space reflections: `full`, `reduced` (8 rays instead of 32, a little grainier) or `off`. Next start | Slow GPUs. `off` saved about 0.15 ms of GPU a frame on a 3090 Ti, more on slower cards |
| `ambient_occlusion` | `1` | `0` turns off the game's SSAO (flatter corners). Next start | Slow GPUs |
| `crowd_detail` | `full` | `reduced`: crowds switch to simpler models twice as soon. Next start | Busy towns on a slow CPU or GPU |
| `scenery_detail` | `full` | `reduced`: scenery detail steps down at 60 % of the distance. Next start | Slow GPUs |
| `ultrawide` | `auto` | On an output wider than 16:9, shows more of the world while menus and HUD stay 16:9 in the centre | `off` if you want the 16:9 picture stretched across the screen |
| `ultrawide_images` | `fill` | Full-screen pictures (illustrations, loading and title screens) on a wide output: `fill` (cover the screen, crop top and bottom), `fit` (whole picture, bars at the sides) or `stretch` (the old look) | You prefer to see the whole picture: `fit` |
| `rt_pool_overflow_mb` | `3072` | Room past the game's render-target pool for large outputs | Leave it. Only lower it if memory is very tight and you play at 1080p |

### Texture memory (`[gfx]`)

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `texture_detail` | `auto` | `half` keeps big textures without their largest level (about 1.9 GB less video memory). `auto` picks `half` under 9 GiB of video memory | You have an 8 GB card that runs out: force `half` |
| `astc_bc` | `auto` | `bc1` stores textures proven opaque at half the size of BC7. `auto` uses it on integrated GPUs and small cards | You want to save video memory on any card: `bc1`. Smooth gradients lose a little precision |
| `astc_quality` | `high` | `high` sharpens textures in the background after first use and caches them | `fast` on a weak CPU to skip the background re-encode |
| `astc_gpu` | `auto` | Textures are converted on the GPU, except on integrated GPUs (on the CPU there) | Leave it. `off` only to troubleshoot |
| `astc_gpu_load_ms` / `astc_gpu_load_s` | `8` / `3` | For 3 s after a load, the texture queue gets at least 8 ms of GPU a frame, so textures pop in sooner | A weak GPU and the seconds after a load feel slow: `astc_gpu_load_ms = 0` restores the old behaviour |
| `residency` | `auto` | Gives back video memory of textures no longer drawn when near the budget. Off on Intel | Leave it |
| `amd_overallocation` | `true` | On AMD, lets the game allocate past the video memory size (Windows moves the rest to system memory) | Leave it |

## Frame rate and timing

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `[fefw] fps60` / `fps120` | `0` / `0` | Gameplay at 60 or 120 fps at the right game speed (`fps120` wins) | Your PC and display can hold 60 or 120 |
| `[fefw] fps240` | `0` | Testing only, in the ini (no menu shows it): a 240 fps target, so at low resolutions the frame rate is not stopped at 120 before the CPU is | Measuring how far your CPU goes; not for normal play |
| `[fefw] frame_fixes` | `1` | At 60 and 120 fps, systems made for 30 fps keep their real speed: capes and hair, physics, cutscene camera shake, animation eases, map markers, menu timers, dialogue typing. Live in F1 | Set `0` only to compare. Without it those systems run 2x fast at 60 and 4x at 120 |
| `[fefw] frame_fixes_skip` | empty | Groups to leave alone: `dt`, `shader`, `physics`, `cloth`, `quake`, `anim`, `map`, `ui`, `text` | You're narrowing down a problem for a report |
| `[fefw] cloth_gravity` | `match` | How gravity and wind on capes and hair are scaled at 60 and 120 fps. `match` keeps hair and capes hanging where they hang at 30 fps. `accel` is the rule up to 0.3.1, where a high ponytail or braid stood up (#160) | Set `accel` only to compare |
| `[fefw] smooth_step` | on with fps60 / fps120 | Spreads the game's catch-up time over frames | Leave it |
| `[fefw] cursor_real_time` | `1` | The map cursor keeps its speed when the game runs below its target | Leave it |
| `[nvn] dynres`, `dynres_target`, `dynres_min` | `false`, `0`, `0.7` | Dynamic resolution: lowers the render size in 10 % steps when the frame rate drops below the target. Needs an output size other than Console. F1 > Display | Slower GPUs that hold the target most of the time but dip in busy areas |
| `[latency] mode` (`controls.ini`) | `off` | NVIDIA Reflex or AMD Anti-Lag 2 | You want lower input delay. Needs a restart |

## Upscaling and HDR (`controls.ini` unless noted)

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `[dlss] upscaler` (`ObeliskChamber.ini`) | `taau` | The upscaler behind the game's own DLSS calls: `taau` (built in, no files), `dlss`, `fsr3`, `xess`, `bilinear`, or `auto` (the best one whose files are there). F1 > Upscaler | You have the DLSS, FSR 3 or XeSS files (see the README) |
| `[dlss] preset` (`ObeliskChamber.ini`) | unset | DLSS model: `e`, `f` (lighter), `j`, `k`, `l`, `m` (newer, heavier) | You want a specific look or speed |
| `[upscale] dlss_preset_auto` | `true` | With no model chosen, RTX 20 / 30 cards use model E | You'd rather have the library's default model on an RTX 20 / 30 |
| `[upscale] mode` | `off` | Spatial upscaling of the final picture to the window: `fsr1` or `nis`. Fortune's Weave upscales its own scene, so the temporal modes (`fsr3`, `dlss`, `dlaa`, `xess`) are ignored here | You render below your window size and want it sharper |
| `[upscale] sharpness` | `0.8` | FSR 1 / NIS / FSR 3 sharpening, 0 to 1 | Taste |
| `[dlss] frame_gen` (`ObeliskChamber.ini`) | `0` | Frame generation: `dlss` (RTX 40+) or `fsr3` | You have a 120 Hz+ display and the files |
| `[hdr] mode` | `auto` | `off`; `auto` (the game's SDR picture made into HDR when Windows has HDR on); `on`; `native`, the game's own HDR picture as on a Switch 2 with an HDR TV | You have an HDR display: `native`. The game's own Brightness options then work |
| `[hdr] peak_nits` / `paper_white_nits` | `0` / `0` | `0` takes your display's peak and Windows' SDR brightness | Your display reports wrong levels. With `native` the game may need a restart to see a change |

## Loading and texture streaming (`[nvn]`, `ObeliskChamber.ini`)

These came from Stephen Jayakar's performance fork and later ports. They decide how textures arrive when an area loads. The defaults
are tuned; change them only if you know what you're testing.

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `tex_async` | `standin` | A texture without its data yet is drawn with a quick stand-in while a background job makes the real one. The render thread never reads the disk cache | Leave it. `off` only to troubleshoot |
| `pool_upload_ms` / `pool_upload_burst_ms` | `6` / `24` | Milliseconds a frame spent on an area's first texture uploads, more while a backlog streams in | A slow CPU and long load-in stutters: try lower values (smoother, slower pop-in) |
| `bound_upload_defer` | `false` | `true` also spreads out the uploads a model on screen needs, which made models show black for a split second after loads | A weak PC with a long single frame after loads, if you don't mind the black flash |
| `staging_ring_mb` | `256` | One persistent upload buffer instead of a new one per texture | Leave it. Lower it only if RAM is very tight |
| `tex_prep_mb` | `1024` | Memory that finished background texture results may hold | Lower it on a 16 GB PC that runs short of RAM |
| `standin_keep`, `standin_copy`, `standin_rows`, `tex_prep_keep_promised` | `true` | Cheaper stand-ins that stay until the real texture is ready | Leave them |
| `mirror_spare` | `true` | The next 64 MiB GPU memory chunk is made ahead on a worker, not on the render thread at area arrivals | Leave it |
| `untile_import` | `false` | `true` would import a whole 64 MiB chunk on the render thread for a GPU untile (16 to 90 ms) | Leave it |
| `pool_mirror` | `on` | GPU copies of memory pools that draws read (launcher / F1 "Faster GPU memory") | `off` only to troubleshoot |
| `[memory] freeze_table` | `true` | Finds the pages to protect during a memory import from the heap's page table (the old way caused 140 to 500 ms hitches) | Leave it |

## Shaders (`[nvn]`)

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `prewarm` | `true` (F1 "Prepare shaders ahead") | Records and builds shaders ahead of their first use | Leave it on |
| `shader_pack_all` / `shader_pack_resident` | `true` / `true` | The whole shader pack stays in memory (about 175 MB), so no shader file is read during play | A 16 GB PC short of RAM: `shader_pack_resident = false` (shader loads then read the disk) |
| `compute_async` | `prebuild` | Compute pipelines are built ahead on background threads | Leave it |
| `prewarm_wait` | `false` | A draw whose shader is still building is skipped for a frame instead of waited for | `true` if you see a missing object for a frame and would rather have a stall |
| `gpl_cold` | `auto` | Compiles shaders the game has set but not drawn yet. `auto`: on with more than 8 hardware threads | A 6 to 8 thread CPU with first-use stutter: try `true` |
| `gpl_threads` / `prewarm_threads` | a quarter of the CPU's threads, 1 to 6 | Background shader build threads (below normal priority) | Leave them |
| `gpl_burst_threads` | `2` | Extra threads that only take urgent shader jobs | Leave it |
| `compile_eco` | `auto` | Shader build threads go to the efficiency cores on Intel 12th gen+ style CPUs | Leave it |
| `compile_pace_ms` / `compile_pace_after_ms` | `8` / `20` | When a frame runs late, background shader work steps aside so the game gets the cores. Never for work a draw waits for | A small CPU with load stutter: raise `compile_pace_ms`. `0` turns it off |

**NVIDIA: the driver's shader cache size.** Besides the game's own caches (`pipeline_cache.bin` and the shader pack), NVIDIA's driver keeps
compiled shaders on disk, up to a size you choose in the **NVIDIA app > Graphics > Global Settings > Shader Cache Size** (older drivers:
NVIDIA Control Panel > Manage 3D settings). Leave it at **Driver Default** or larger. **Disabled** or a small size makes the driver throw
compiled shaders away, so the stutter of first use comes back on the next start; Disabled is only a step in NVIDIA's procedure for clearing
the cache, not a setting to keep. A driver update clears this cache by itself, so the first session after one may stutter a little more.

## CPU and threads

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `[nvn] replay_threads` | `auto` | `2`: a worker thread makes the render thread's Vulkan calls. `auto` picks 2 with 6 or more physical cores, 1 below | A 4-core CPU stutters with `2` forced: set `1`. A 4-core CPU with a slow GPU might gain from `2`: try it |
| `[nvn] replay_batch` | `16` | Calls handed to that worker at a time | Leave it |
| `[kernel] wait_spin` | `auto` | Game threads `main` and `Basic_Worker` spin 30 µs before sleeping, cutting wake-up delay. `auto`: on with 8 or more physical cores | A laptop on battery: `false` saves a little power. 6 physical cores: try `true` |
| `[kernel] spin_us`, `lock_spin_us`, `spin_threads`, `lock_spin_threads` | set by `wait_spin` | The spin times and the threads they apply to | Only for testing |
| `[sched] starvation_lean` | `true` | A cheaper guard for starved threads | Leave it |
| `[gfx] texture_eco` | `true` | Texture conversions off the render thread run in efficiency mode | Leave it |

## GPU work (`[nvn]`)

All of these are on by default on NVIDIA and AMD. On Intel, `[gfx] intel_compat` sets the first five to their safe value.

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `dispatch_barriers` | `tracked` | Compute work that shares nothing runs side by side instead of behind a full barrier | Corrupted pictures on AMD: try `full` and report it |
| `game_barriers` | `tracked` | The game's own barriers are kept only where they protect something | Same as above |
| `batch_ubo_updates` | `true` | A run of uniform-buffer updates gets two barriers instead of two each | Same as above: `false` |
| `feedback_levels` | `true` | Feedback copies (bloom chains) copy only the levels they sample | Same as above: `false` |
| `depth_ro_sticky` | `true` | Draws that don't touch depth stay in a read-only depth pass | Same as above: `false` |
| `lean_draws` | `true` (`false` on Intel) | A lighter per-draw path | Leave it |
| `out_memo_dirty`, `out_prog_dirty`, `out_memo_held` | `true` | A draw re-checks only the buffer slots that changed since the last check (about 0.2 ms a frame of render thread) | A corrupted picture: `out_memo_dirty = false` and report it |
| `reserved_sampler` | `true` | Texture fetches the game samples with filtering get a smooth sampler (fixes blocky clouds) | Leave it |
| `float_atomic_add` | `true` | The lighting compute shader uses the GPU's own float add instead of a retry loop | Leave it |
| `int_view_nearest` | `true` | Integer textures get the unfiltered sampler Vulkan requires | Leave it |
| `camera_patch_cached` | `true` | Faster camera block patching with an upscaler | Leave it |
| `reaper` | `true` | Vulkan objects are destroyed on a low-priority thread instead of the render thread | Leave it |
| `split_submit_ms` | `0` (off) | Long submissions go to the GPU in parts, so the GPU starts sooner | A CPU-bound PC with waits on the GPU: try `2` |
| `narrow_pass_conflicts`, `narrow_copy_barriers` | `true` | Barriers between passes on the same target and after texture copies wait only for that kind of work (GPU time -2% at 4K on an RTX 3090 Ti) | Corrupted pictures: `false` and report it |
| `narrow_transfer_pass_ends`, `narrow_shadow_barriers`, `narrow_update_barriers` | `false` | The same for barriers around copies, clears, compute shadow copies and uniform updates. No gain measured on NVIDIA | AMD or Intel testers: try `true` and compare |
| `stat_counters` | `always` | The game's statistics counters are written every time (`once` saved 30 µs a frame but could hang) | Leave it |
| `shading_rate` | unset | Coarser shading for chosen draws | Expert use; see CONFIG.md |

## Intel (`[gfx]` and `[nvn]`)

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `[gfx] intel_compat` | `auto` | On every Intel GPU: turns off `lean_draws`, `residency`, `batch_ubo_updates`, `feedback_levels` and `depth_ro_sticky`, and uses full barriers. These lost the GPU at the title screen on Arc (issue #81) | Only to test on Intel: `0`. Any of those keys set by hand still wins |
| `[nvn] pool_slabs` | `auto` | Small memory pools share one 16 MiB import. Off on Intel | Leave it |
| `[setup] intel_shaders` | `false` | Set up translates the shaders the way Ryujinx does for Intel GPUs: 64-bit float math done in 32-bit, no barriers inside branches (issue #81) | Intel Arc or iGPU testers only: set `true` in `ObeliskChamber.ini`, then press **Set up** (it translates every shader again). Report whether the game gets further than before |

## AMD testing (`[gfx]`)

Both keys are off by default and meant for testing on AMD Radeon cards. Turn on one at a time and compare the frame rate and the picture
(an A/B with `tools/perf/ab_run.ps1` if you can), then report it.

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `rt_no_storage` | `false` | Render targets the game marked compressible are made without storage usage, so AMD's colour compression can stay on | An AMD tester: try `true`. A missing effect or black area means a shader did store to one: report it with the log |
| `image_format_lists` | `false` | Images that are viewed in a second format name those formats when they are made, which also lets the driver keep compression | An AMD tester: try `true` |

## Menus, sound and quality of life

The `[fefw]` keys are in `ObeliskChamber.ini`. The F1 menu saves the `[fps]` and `[audio]` choices in `controls.ini`, which is used
when `ObeliskChamber.ini` doesn't set the same key.

| Key | Default | What it does | Change it if... |
|---|---|---|---|
| `[fefw] system_menu_entry` | `true` | **Recomp Options** under **+ > System** | `false` to hide it |
| `[fefw] native_options` | `true` | Recomp Options is a screen in the game's own style. `false` opens the F1 menu instead | You prefer the F1 menu |
| `[fefw] system_menu_quit` | `true` | **Quit Game** under **+ > System**, after the game's own yes / no prompt (cursor on No) | `false` to hide it |
| `[fefw] save_slots` / `save_slot` | `true` / `000` | Several save slots, picked on the title screen | `false` for the game's single save |
| `[fefw] mod_menu` | `true` | **DLC / Mods** on the title screen | `false` for the game's own DLC entry |
| `[fefw] prompt_glyphs` | `1` | Button glyphs in the game's text follow your controller (PlayStation, Xbox, keyboard) | `0` for the Switch glyphs |
| `[fefw] pager_dpad` | `small` | A D-pad for the tutorial's page-turn glyphs with a PlayStation or Xbox pad | Taste |
| `[fefw] mouse_switch_now` | `1` | The game's mouse controls switch on and off at once | Leave it |
| `[fps] show` | `0` | Frame rate counter (F2) | You want it on at start |
| `[audio] volume` | `1.0` | Master volume, up to `2.0` (200 %) | Taste |
| `[audio] channels` | `2` | `6` for 5.1 speakers | You have 5.1 |
| `[audio] mute_unfocused` | `0` | `1` silences the game when its window isn't focused | Taste |

## Diagnostics: for bug reports and testing

Leave these at their defaults unless you're testing or someone asked for a log with them.

| Key | Default | What it does |
|---|---|---|
| `[fefw] clock_log` | `10` | Logs how fast the game's clock runs against real time (`1.000x` is right) |
| `[prof] gpu_stats` | `0` | `N`: GPU time per frame, draws and barriers in the log every N frames |
| `[prof] hitch_ms` | `0` | Logs a breakdown of every frame slower than N ms |
| `[nvn] replay_stats` | `0` | Render-thread time per frame in the log |
| `[nvn] rt_check` | `0` | Checks every skipped slot of the held-result short path. Costs 15 to 25 fps: testing only |
| `[nvn] out_prog_ab` | `0` | Switches the held-result short path on and off in alternate stats windows |
| `[fefw] ultrawide_map_diag` | `0` | Logs world map vertices left wide on an ultrawide output |
| `[dlss] test_fail_after_s` | `0` | Fakes one DLSS failure after N seconds, to watch it recover |
| `[gfx] validation` | `false` | Loads the Vulkan validation layer (needs the Vulkan SDK) |
| `[gfx] log_fps` | `false` | One line a second with the frame rate |
