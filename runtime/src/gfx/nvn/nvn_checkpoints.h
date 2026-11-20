#pragma once
// [gfx] gpu_checkpoints: a marker before every draw and dispatch, named in the device-lost report (nvn_checkpoints.cpp). Render thread.
#include <volk.h>

namespace nxrt::nvn::be::gpu {

// No-ops unless [gfx] gpu_checkpoints is on and the driver has VK_NV_device_diagnostic_checkpoints. Shader hashes may be null.
void ckpt_draw(VkCommandBuffer cb, const char* vs, const char* fs, const char* gs);
void ckpt_dispatch(VkCommandBuffer cb, const char* cs);

}  // namespace nxrt::nvn::be::gpu
