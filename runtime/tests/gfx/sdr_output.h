// SPDX-License-Identifier: GPL-3.0-or-later
// The SDR picture for a test that compares pixels with an SDR expectation. [hdr] mode is off for the life of the object, whatever
// NXRT_HDR_MODE, the config or Windows' HDR switch says; the settings before it are put back. Without it a forced HDR mode (on or
// native) makes even an offscreen renderer HDR10 (vk_renderer.cpp decide_output), the picture drawn there is SDR values in a PQ
// image, and the capture decodes it as PQ BT.2020: every colour comes back brighter and over-saturated.
// Make it before the renderer (the output is chosen when the renderer is created) and keep it until the renderer is gone.
#pragma once
#include "nxrt/extras.h"

namespace gfxtest {

class SdrOutput {
public:
    SdrOutput() : prev_(nxrt::extras::settings()) {
        nxrt::extras::Settings s = prev_;
        s.hdr.mode = nxrt::extras::HdrMode::Off;
        nxrt::extras::set_settings(s);
    }
    ~SdrOutput() { nxrt::extras::set_settings(prev_); }
    SdrOutput(const SdrOutput&) = delete;
    SdrOutput& operator=(const SdrOutput&) = delete;

private:
    nxrt::extras::Settings prev_;
};

}  // namespace gfxtest
