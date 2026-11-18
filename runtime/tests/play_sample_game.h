// SPDX-License-Identifier: GPL-3.0-or-later
// A sample game for the play-mode tests: a first-person game's names for the generic action and context slots, its factory controls.ini
// and the revisions that changed it. The binding engine, the settings reader and the launcher's Keyboard tab are the runtime's; only the
// game names the slots and writes the factory file (play_types.h GameVocabulary, GameDefaults). Fortune's Weave names no slots and binds
// only Switch buttons, so the tests of fire, beams, the alternate body, contexts and default revisions use this game instead.
//
// Use: a SampleGame on the stack registers it, the destructor puts the previous game back:
//   NXTEST(x) { SampleGame g; ... nxrt::play::default_settings() ... }
// The launcher's key_rows() keeps the rows it read first, so a test process that reads them must have this game registered at that time.
#pragma once
#include <string>

#include "nxrt/game.h"
#include "play/play_types.h"

namespace nxtest_play {

namespace sample_detail {

using nxrt::play::Act;
using nxrt::play::ActInfo;
using nxrt::play::Ctx;

// One row per Act, in enum order: the sample game's settings-file names and factory Switch outputs.
inline const ActInfo* acts() {
    static const ActInfo a[] = {
        {Act::None, "none", ""},
        {Act::MoveForward, "move_forward", "LS_UP"},
        {Act::MoveBack, "move_back", "LS_DOWN"},
        {Act::MoveLeft, "move_left", "LS_LEFT"},
        {Act::MoveRight, "move_right", "LS_RIGHT"},
        {Act::LookUp, "look_up", "RS_UP"},
        {Act::LookDown, "look_down", "RS_DOWN"},
        {Act::LookLeft, "look_left", "RS_LEFT"},
        {Act::LookRight, "look_right", "RS_RIGHT"},
        {Act::Primary, "fire", "ZR"},
        {Act::Target, "lock_on", "ZL"},
        {Act::Secondary, "missile", "R"},
        {Act::Jump, "jump", "B"},
        {Act::AltForm, "morph", "Y"},
        {Act::LookHold, "free_look", "L"},
        {Act::Map, "map", "PLUS"},
        {Act::Pause, "pause", "MINUS"},
        {Act::Confirm, "confirm", "A"},
        {Act::Back, "back", "B"},
        {Act::DpadUp, "dpad_up", "UP"},
        {Act::DpadDown, "dpad_down", "DOWN"},
        {Act::DpadLeft, "dpad_left", "LEFT"},
        {Act::DpadRight, "dpad_right", "RIGHT"},
        {Act::SetA1, "beam_1", "X+UP"},
        {Act::SetA2, "beam_2", "X+RIGHT"},
        {Act::SetA3, "beam_3", "X+DOWN"},
        {Act::SetA4, "beam_4", "X+LEFT"},
        {Act::SetB1, "visor_1", "L+UP"},
        {Act::SetB2, "visor_2", "L+RIGHT"},
        {Act::SetB3, "visor_3", "L+DOWN"},
        {Act::SetB4, "visor_4", "L+LEFT"},
        {Act::SetANext, "beam_next", ""},
        {Act::SetAPrev, "beam_prev", ""},
        {Act::BtnA, "btn_a", "A"},
        {Act::BtnB, "btn_b", "B"},
        {Act::BtnX, "btn_x", "X"},
        {Act::BtnY, "btn_y", "Y"},
        {Act::BtnL, "btn_l", "L"},
        {Act::BtnR, "btn_r", "R"},
        {Act::BtnZL, "btn_zl", "ZL"},
        {Act::BtnZR, "btn_zr", "ZR"},
        {Act::BtnPlus, "btn_plus", "PLUS"},
        {Act::BtnMinus, "btn_minus", "MINUS"},
        {Act::BtnLStick, "btn_lstick", "LSTICK"},
        {Act::BtnRStick, "btn_rstick", "RSTICK"},
        {Act::MouseMode, "mouse_mode", ""},
    };
    static_assert(sizeof(a) / sizeof(a[0]) == nxrt::play::kActCount, "one row per Act");
    return a;
}

inline const char* const* ctx_names() {
    static const char* const n[] = {"all", "gameplay", "morph", "map", "menu", "scan"};
    static_assert(sizeof(n) / sizeof(n[0]) == nxrt::play::kCtxCount, "one name per Ctx");
    return n;
}

// The launcher's Keyboard tab rows, in display order.
inline const nxrt::play::GameActLabel* labels(unsigned* count) {
    static const nxrt::play::GameActLabel l[] = {
        {Act::MoveForward, "Move forward", "Movement"}, {Act::MoveBack, "Move back", "Movement"},
        {Act::MoveLeft, "Move left", "Movement"},       {Act::MoveRight, "Move right", "Movement"},
        {Act::LookUp, "Look up", "Movement"},           {Act::LookDown, "Look down", "Movement"},
        {Act::LookLeft, "Look left", "Movement"},       {Act::LookRight, "Look right", "Movement"},
        {Act::Jump, "Jump", "Movement"},                {Act::AltForm, "Morph ball", "Movement"},
        {Act::Primary, "Fire", "Combat"},               {Act::Target, "Lock on", "Combat"},
        {Act::Secondary, "Missile", "Combat"},          {Act::LookHold, "Free look", "Combat"},
        {Act::SetA1, "Power beam", "Beams"},            {Act::SetA2, "Wave beam", "Beams"},
        {Act::SetA3, "Ice beam", "Beams"},              {Act::SetA4, "Plasma beam", "Beams"},
        {Act::SetANext, "Next beam", "Beams"},          {Act::SetAPrev, "Previous beam", "Beams"},
        {Act::SetB1, "Combat visor", "Visors"},         {Act::SetB2, "Scan visor", "Visors"},
        {Act::SetB3, "Thermal visor", "Visors"},        {Act::SetB4, "X-ray visor", "Visors"},
        {Act::Map, "Map", "Menus"},                     {Act::Pause, "Pause", "Menus"},
    };
    *count = static_cast<unsigned>(sizeof(l) / sizeof(l[0]));
    return l;
}

// The current revision. Revision 2 added X on the map (btn_x); revision 3 moved the factory lock_mode from classic to free.
constexpr int kRev = 3;

inline const char* factory_text() {
    return "# Sample game, play mode: controls and display.\n"
           "\n[meta]\ndefaults_rev = 3\n"
           "\n[aim]\nlock_mode = free\n"
           "\n[prompts]\nstyle = auto\n"
           "\n[tweaks]\nglow_color = off\nsuit_glow = auto\nball_glow = auto\ngun_glow = auto\n"
           "\n[bindings.all]\n"
           "W = move_forward\nS = move_back\nA = move_left\nD = move_right\n"
           "Up = look_up\nDown = look_down\nLeft = look_left\nRight = look_right\n"
           "Mouse1 = fire\nMouse3 = lock_on\nF = missile\nSpace = jump\nLeft Ctrl = morph\nLeft Shift = free_look\n"
           "WheelUp = beam_prev\nWheelDown = beam_next\n1 = beam_1\n2 = beam_2\n3 = beam_3\n4 = beam_4\n"
           "Tab = map\nEscape = pause\nQ = btn_l\n"
           "\n[bindings.gameplay]\n"
           "\n[bindings.morph]\n"
           "\n[bindings.map]\nX = btn_x\n"
           "\n[bindings.menu]\nMouse1 = confirm\nReturn = confirm\nEscape = back\nBackspace = back\n"
           "\n[bindings.scan]\n";
}

inline const nxrt::play::GameDefaults& defaults() {
    static const nxrt::play::GameDefaultAddition adds[] = {{2, Ctx::Map, "X", Act::BtnX}};
    static const nxrt::play::GameDefaultChange changes[] = {{3, "aim", "lock_mode", "classic", "free"}};
    static const nxrt::play::GameVocabulary vocab = [] {
        nxrt::play::GameVocabulary v;
        v.acts = acts();
        v.ctx_names = ctx_names();
        v.labels = labels(&v.label_count);
        v.key_alt_yaw = "morph_yaw";
        v.key_alt_pitch = "morph_pitch";
        v.key_wheel_cycles = "wheel_beams";
        v.key_alt_orbit = "ball_orbit";
        v.key_alt_camera = "ball_camera";
        v.key_alt_sens = "ball_sensitivity";
        v.label_alt_camera = "Morph ball camera";
        v.label_wheel_cycles = "Mouse wheel changes the beam";
        v.label_alt_camera_on = "Mouse and right stick turn the morph ball camera";
        return v;
    }();
    static const nxrt::play::GameDefaults d = [] {
        nxrt::play::GameDefaults g;
        g.settings_text = factory_text();
        g.defaults_rev = kRev;
        g.additions = adds;
        g.addition_count = 1;
        g.vocabulary = &vocab;
        g.changes = changes;
        g.change_count = 1;
        return g;
    }();
    return d;
}

}  // namespace sample_detail

// Registers the sample game for the life of the object: the registered game's Info (or an empty one) with the sample's play defaults.
class SampleGame {
public:
    SampleGame() : prev_(nxrt::game::current()) {
        info_ = prev_ ? *prev_ : nxrt::game::Info{};
        info_.play_defaults = &sample_detail::defaults();
        nxrt::game::set_current(&info_);
    }
    ~SampleGame() { nxrt::game::set_current(prev_); }
    SampleGame(const SampleGame&) = delete;
    SampleGame& operator=(const SampleGame&) = delete;

    static unsigned label_count() {
        unsigned n = 0;
        sample_detail::labels(&n);
        return n;
    }

private:
    const nxrt::game::Info* prev_;
    nxrt::game::Info info_;
};

}  // namespace nxtest_play
