// SPDX-License-Identifier: GPL-3.0-or-later
// Fire Emblem: Fortune's Weave: the graphics presets (Low, Medium, High, Ultra) and the output sizes the menus offer.
//
// A preset is not a key of its own: picking one writes seven ObeliskChamber.ini keys, and the preset shown is the one whose seven values
// the file holds now ("Custom" when none matches). So a file edited by hand, the launcher, the F1 menu and Recomp Options always agree.
// Output size, the upscaler and frame generation are left alone: they depend on the display and the GPU, not on how much detail is wanted.
//
//            [fefw] render_ratio   [fefw] shadow_size   [fefw] quality
//   Low      0.5 (half per side)   1024                 handheld (the handheld config's lighter shadows and effects)
//   Medium   0 (the game's own)    0 (the game's own)   console       = the defaults: the console's picture
//   High     0.67                  4096                 docked
//   Ultra    1.0 (native, DLAA)    8192                 docked
//
// and the graphics data (ObeliskChamber_detail.cpp, applies at the next start):
//            [fefw] reflections    ambient_occlusion    crowd_detail    scenery_detail
//   Low      reduced (8 rays)      0                    reduced         reduced
//   others   full                  1                    full            full
//
// The launcher has the same table (launcher/src/pages/settings.js PRESETS) and runtime/docs/CONFIG.md describes it: keep the three in step.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include "nxrt/config.h"
#include "nxrt/platform.h"

namespace nxrt::games::ObeliskChamber {

struct GraphicsPreset {
    const char* name;
    const char* render_ratio;
    const char* shadow_size;
    const char* quality;
    const char* reflections;          // the graphics data keys: the game's own files at the next start
    const char* ambient_occlusion;
    const char* crowd_detail;
    const char* scenery_detail;
};
inline constexpr GraphicsPreset kGraphicsPresets[] = {
    {"Low", "0.5", "1024", "handheld", "reduced", "0", "reduced", "reduced"},
    {"Medium", "0", "0", "console", "full", "1", "full", "full"},
    {"High", "0.67", "4096", "docked", "full", "1", "full", "full"},
    {"Ultra", "1.0", "8192", "docked", "full", "1", "full", "full"},
};
// The seven keys a preset writes, in the order of GraphicsPreset's fields after the name.
inline constexpr const char* kPresetKeys[7] = {"render_ratio", "shadow_size", "quality", "reflections", "ambient_occlusion", "crowd_detail",
                                               "scenery_detail"};
inline const char* preset_value(const GraphicsPreset& p, int i) {
    const char* const v[7] = {p.render_ratio, p.shadow_size, p.quality, p.reflections, p.ambient_occlusion, p.crowd_detail, p.scenery_detail};
    return v[i];
}
inline constexpr const char* kPresetDefaults[7] = {"0", "0", "console", "full", "1", "full", "full"};
inline constexpr int kGraphicsPresetCount = static_cast<int>(sizeof kGraphicsPresets / sizeof kGraphicsPresets[0]);

namespace preset_detail {
inline bool same_number(const std::string& a, const char* b) { return std::atof(a.c_str()) == std::atof(b); }
inline bool same_text(std::string a, const char* b) {
    for (char& c : a) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return a == b;
}
inline bool same_bool(std::string a, const char* b) {
    for (char& c : a) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    const bool on = !(a == "0" || a == "false" || a == "off" || a == "no");
    return on == (b[0] == '1');
}
}  // namespace preset_detail

// The preset ObeliskChamber.ini holds now, -1 for none (Custom).
inline int graphics_preset_current() {
    const std::string ratio = config_get_str("fefw", "render_ratio", "0");
    const std::string shadow = config_get_str("fefw", "shadow_size", "0");
    const std::string quality = config_get_str("fefw", "quality", "console");
    const std::string refl = config_get_str("fefw", "reflections", "full"), ao = config_get_str("fefw", "ambient_occlusion", "1");
    const std::string crowd = config_get_str("fefw", "crowd_detail", "full"), scenery = config_get_str("fefw", "scenery_detail", "full");
    for (int i = 0; i < kGraphicsPresetCount; ++i) {
        const GraphicsPreset& p = kGraphicsPresets[i];
        if (preset_detail::same_number(ratio, p.render_ratio) && preset_detail::same_number(shadow, p.shadow_size) &&
            preset_detail::same_text(quality, p.quality) && preset_detail::same_text(refl, p.reflections) &&
            preset_detail::same_bool(ao, p.ambient_occlusion) && preset_detail::same_text(crowd, p.crowd_detail) &&
            preset_detail::same_text(scenery, p.scenery_detail))
            return i;
    }
    return -1;
}

// ---- output sizes ------------------------------------------------------------------------------------------------------------------------
// The [fefw] output values the menus list, grouped by shape. A shape wider than 16:9 needs [fefw] ultrawide = auto (the default) to widen
// the 3D view; above 3840 x 2160 the shape is kept and drawn smaller (ObeliskChamber_res.cpp), and the window scales it up.
struct OutputChoice {
    const char* value;   // [fefw] output
    const char* shape;   // "16:9", "21:9", "32:9", "" (console, window)
    const char* size;    // "2560 x 1080"
    const char* note;    // "" or what it is drawn at
};
inline constexpr OutputChoice kOutputChoices[] = {
    {"console", "", "Console", "the game's choice: 1440p docked"},
    {"720p", "16:9", "1280 x 720", ""},
    {"1080p", "16:9", "1920 x 1080", ""},
    {"1440p", "16:9", "2560 x 1440", ""},
    {"2160p", "16:9", "3840 x 2160", ""},
    {"2560x1080", "21:9", "2560 x 1080", ""},
    {"3440x1440", "21:9", "3440 x 1440", ""},
    {"3840x1600", "21:9", "3840 x 1600", ""},
    {"5120x2160", "21:9", "5120 x 2160", "drawn at 3840 x 1620"},
    {"3840x1080", "32:9", "3840 x 1080", ""},
    {"5120x1440", "32:9", "5120 x 1440", "drawn at 3840 x 1080"},
    {"window", "", "The window's size", "at start, any shape"},
};
inline constexpr int kOutputChoiceCount = static_cast<int>(sizeof kOutputChoices / sizeof kOutputChoices[0]);

// The display the window is on, as an output value ("3440x1440"); empty when unknown.
inline std::string monitor_output_value() {
    int w = 0, h = 0;
    if (!platform::desktop_pixel_size(&w, &h) || w <= 0 || h <= 0) return {};
    char buf[32];
    std::snprintf(buf, sizeof buf, "%dx%d", w & ~1, h & ~1);
    return buf;
}

// A shape name for a size: "16:9", "21:9" (2.2 to 2.5), "32:9" (3.4 and up), "16:10", or "W:H" rounded.
inline std::string shape_of(int w, int h) {
    if (w <= 0 || h <= 0) return {};
    const double a = static_cast<double>(w) / h;
    if (a > 1.76 && a < 1.80) return "16:9";
    if (a > 1.58 && a < 1.62) return "16:10";
    if (a >= 2.2 && a < 2.5) return "21:9";
    if (a >= 3.4 && a < 3.7) return "32:9";
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.2f:1", a);
    return buf;
}

}  // namespace nxrt::games::ObeliskChamber
