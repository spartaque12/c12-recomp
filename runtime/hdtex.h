#pragma once
#include "gpu.h"

struct HdSub {
    int u0, v0, w, h;   // rect in texture-page texel coordinates
    int stp;            // image encodes semi-transparent texels as alpha ~0.5
    void* name;
    void* srv;          // ID3D11ShaderResourceView*
};

extern bool g_hd_on;
void hdtex_init(const std::string& dir);
void hdtex_on_upload(int x, int y, int w, int h);        // after g_vram has been updated
void hdtex_invalidate(int x, int y, int w, int h);       // VRAM words changed by something else
const HdSub* hdtex_lookup(const GpuState& s, int u0, int v0, int u1, int v1);
void hdtex_stats();
