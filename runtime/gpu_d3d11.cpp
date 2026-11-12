// Direct3D 11 renderer: VRAM kept on the GPU at N x resolution in RGBA8 (true colour, A = mask bit),
// native 3D triangles with a depth buffer (gpu3d.h), dual-source semi-transparency.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include "gpu.h"
#include "gpu3d.h"
#include "hdtex.h"
#include "ui.h"
#include <algorithm>

extern HWND g_hwnd;
extern bool g_window_resized;

struct Vtx {
    float x, y, z, w;
    u32 color;
    float u, v;
    s32 tp[4];
    s32 fl[4];
    float lim[4];
};

static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static IDXGISwapChain1* swap;
static ID3D11RenderTargetView* bb_rtv;
static ID3D11Texture2D *vram_tex, *vram_copy, *upload_tex, *read_tex;
static ID3D11RenderTargetView* vram_rtv;
static ID3D11Texture2D* itex;            // in-between frame layer (same layout as VRAM)
static ID3D11RenderTargetView* i_rtv;
static ID3D11ShaderResourceView* i_srv;
static int cur_target;                   // 0 = VRAM, 1 = in-between layer, 2.. = extra targets (VR)
// Extra targets (VR, see gpu.h): 2 / 3 = left / right eye, 4 / 5 = the HUD drawn over black / over white, 6 = cinema.
// Each has its own size and depth buffer and shows one display area of the VRAM coordinate space
// (mx, my .. + dw, dh) stretched over the whole texture, so every primitive, scissor and native 3D
// clip-space formula made for VRAM pixels works unchanged.
struct XTarget {
    ID3D11Texture2D* tex; ID3D11RenderTargetView* rtv; ID3D11ShaderResourceView* srv;
    ID3D11Texture2D* dt; ID3D11DepthStencilView* dsv;
    int w, h; float mx, my, sx, sy;
};
static XTarget xt[5];
static ID3D11ShaderResourceView *vram_srv, *copy_srv, *upload_srv;
static ID3D11VertexShader *vs_prim, *vs_full;
static ID3D11PixelShader *ps_prim, *ps_upload, *ps_present, *ps_blit;
static ID3D11InputLayout* layout;
static ID3D11Buffer *vbuf, *cbuf;
static ID3D11BlendState *bs_opaque, *bs_dual, *bs_sub;
static ID3D11RasterizerState *rs_scissor, *rs_plain;
static ID3D11SamplerState *smp_linear, *smp_point;
static int S = 4;          // scale
static bool tearing;
static int bb_w, bb_h;

#define MAXV 49152
static Vtx verts[MAXV];
static int nverts;
// batch kind = blend kind | depth mode << 3
enum { BK_OPAQUE, BK_DUAL, BK_SUB, BK_SUB_TEX, BK_CUT /* opaque texels of a semi-transparent polygon only */ };
enum { DM_OFF = 0, DM_TEST = 1 /* GREATER_EQUAL, no write */, DM_WRITE = 2 /* GREATER + write */ };
static int cur_kind = -1;
// native 3D path: one depth buffer per target (VRAM, in-between layer), reversed depth (0 = far)
static ID3D11Texture2D* dtex[2];
static ID3D11DepthStencilView* dsv[2];
static ID3D11DepthStencilState* ds_state[3];
static int depth_frame[7] = {-1000, -1000, -1000, -1000, -1000, -1000, -1000};   // g_frame at the last n3_begin on that target
int g_n3_depth_ps1 = 1;
float g_prim_depth;   // > 0: view depth of a flat (2D) primitive of the world list, from its sort slot
int g_n3_selftest = 0;
char g_bb_shot[512];   // when set, the next present writes the back buffer here
static void* cur_rep;   // replacement texture bound for the current batch
ID3D11Device* hw_device() { return dev; }
ID3D11DeviceContext* hw_context() { return ctx; }
static int cur_clip[4] = {-1, -1, -1, -1};
// region of vram_tex modified since vram_copy was last synchronised (native coords, inclusive-exclusive)
static int dirty[4] = {0, 0, 1024, 512};
static bool dirty_any = true;

struct CB {
    float scale;
    int iscale;
    int pass_mode;
    int filter;
    float src[4];
};

static const char* SHADER = R"(
cbuffer cb : register(b0) { float scale; int iscale; int pass_mode; int filt; float4 src; };
Texture2D<float4> vram : register(t0);
Texture2D<uint> upl : register(t1);
Texture2D<float4> rep : register(t2);
SamplerState smp : register(s0);
struct VIn { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; int4 tp : TEXCOORD1; int4 fl : TEXCOORD2; float4 lim : TEXCOORD3; };
struct VOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; nointerpolation int4 tp : TEXCOORD1;
              nointerpolation int4 fl : TEXCOORD2; nointerpolation float4 lim : TEXCOORD3; float2 clip : SV_ClipDistance0; };
VOut vs_prim(VIn i) {
    VOut o;
    // PS1 primitives: pos.xy = VRAM pixel position, w = 1; clip space (ndc*w, z, w).
    // A polygon reaching behind the eye (w < 0 on a vertex) is cut at w = 0 instead of folding over.
    // Native triangles (fl & 4096): pos.xy are already clip space, near/far planes in fl.zw (as floats).
    // pos.z is the clip-space depth chosen by the CPU: N3_DEPTH_K, i.e. NDC depth K / w (reversed, infinite far).
    float2 c = (i.fl.x & 4096) ? i.pos.xy : float2(i.pos.x / 512.0 - 1.0, 1.0 - i.pos.y / 256.0) * i.pos.w;
    o.pos = float4(c, i.pos.z, i.pos.w);
    o.clip = float2(1.0, 1.0);
    if (i.fl.x & 4096) {
        float zf = asfloat(i.fl.w);
        o.clip = float2(i.pos.w - asfloat(i.fl.z), zf > 0.0 ? zf - i.pos.w : 1.0);
    }
    o.col = i.col; o.uv = i.uv; o.tp = i.tp; o.fl = i.fl; o.lim = i.lim;
    return o;
}
uint load16(int x, int y) {
    float4 p = vram.Load(int3((x & 1023) * iscale, (y & 511) * iscale, 0));
    uint4 c = uint4(p * float4(31.0, 31.0, 31.0, 1.0) + 0.5);
    return c.r | (c.g << 5) | (c.b << 10) | (c.a << 15);
}
float4 texel_at(int u, int v, int4 tp, int depth, int tw) {
    int mx = tw & 255, my = (tw >> 8) & 255, ox = (tw >> 16) & 255, oy = (tw >> 24) & 255;
    u = ((u & ~mx) | (ox & mx)) & 255;
    v = ((v & ~my) | (oy & my)) & 255;
    uint t;
    if (depth == 0) { uint w = load16(tp.x + (u >> 2), tp.y + v); t = load16(tp.z + int((w >> ((u & 3) * 4)) & 15u), tp.w); }
    else if (depth == 1) { uint w = load16(tp.x + (u >> 1), tp.y + v); t = load16(tp.z + int((w >> ((u & 1) * 8)) & 255u), tp.w); }
    else t = load16(tp.x + u, tp.y + v);
    if (t == 0) return float4(0, 0, 0, -1);
    return float4(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u), float(t >> 15)) / float4(31, 31, 31, 1);
}
struct POut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };
POut ps_prim(VOut i) {
    POut o;
    int fl = i.fl.x;
    float3 col = i.col.rgb;
    float semi_texel = 1.0, mask = 0.0;
    float2 ruv = (i.uv - i.lim.xy) * i.lim.zw;
    float2 rdx = ddx(ruv), rdy = ddy(ruv);
    if (fl & 512) {
        // HD replacement: colour and cut-out from the image, blend flag from the image (STP packs) or the original texel
        float4 t = rep.SampleGrad(smp, saturate(ruv), rdx, rdy);
        if (t.a < 0.25) discard;
        if (fl & 1024) semi_texel = t.a < 0.75 ? 1.0 : 0.0;
        else {
            float4 o = texel_at(int(i.uv.x), int(i.uv.y), i.tp, (fl >> 2) & 3, i.fl.y);
            semi_texel = max(o.a, 0.0);
        }
        mask = semi_texel;
        if (fl & 2) col = t.rgb; else col = saturate(t.rgb * col * (255.0 / 128.0));
    } else if (fl & 1) {
        int depth = (fl >> 2) & 3;
        float2 uvf = clamp(i.uv, i.lim.xy, i.lim.zw + 0.999);
        float4 t;
        if (depth == 2 && i.fl.y == 0) {
            // direct-colour texture: sample the upscaled VRAM at full resolution
            float2 p = (float2(i.tp.xy) + uvf) * scale;
            // wrap with a modulo: a bit mask is only right for power-of-two scales (3x, 5x, 6x... tiled garbage)
            int2 ip = int2(p);
            int2 sz = int2(1024 * iscale, 512 * iscale);
            ip = ((ip % sz) + sz) % sz;
            t = vram.Load(int3(ip, 0));
            if (dot(t, float4(1, 1, 1, 1)) == 0.0) discard;
        } else if (filt != 0) {
            float2 f = uvf - 0.5;
            int2 b = int2(floor(f));
            float2 fr = f - floor(f);
            int2 lo = int2(i.lim.xy), hi = int2(i.lim.zw);
            float4 t00 = texel_at(clamp(b.x, lo.x, hi.x), clamp(b.y, lo.y, hi.y), i.tp, depth, i.fl.y);
            float4 t10 = texel_at(clamp(b.x + 1, lo.x, hi.x), clamp(b.y, lo.y, hi.y), i.tp, depth, i.fl.y);
            float4 t01 = texel_at(clamp(b.x, lo.x, hi.x), clamp(b.y + 1, lo.y, hi.y), i.tp, depth, i.fl.y);
            float4 t11 = texel_at(clamp(b.x + 1, lo.x, hi.x), clamp(b.y + 1, lo.y, hi.y), i.tp, depth, i.fl.y);
            float4 w = float4((1 - fr.x) * (1 - fr.y), fr.x * (1 - fr.y), (1 - fr.x) * fr.y, fr.x * fr.y);
            float4 op = float4(t00.a >= 0, t10.a >= 0, t01.a >= 0, t11.a >= 0) * w;
            float cov = op.x + op.y + op.z + op.w;
            if (cov < 0.5) discard;
            t.rgb = (t00.rgb * op.x + t10.rgb * op.y + t01.rgb * op.z + t11.rgb * op.w) / cov;
            float4 n = texel_at(int(uvf.x), int(uvf.y), i.tp, depth, i.fl.y);
            t.a = max(n.a, 0.0);
        } else {
            t = texel_at(int(uvf.x), int(uvf.y), i.tp, depth, i.fl.y);
            if (t.a < 0.0) discard;
        }
        semi_texel = t.a;
        mask = t.a;
        if (fl & 2) col = t.rgb; else col = saturate(t.rgb * col * (255.0 / 128.0));
    }
    if (fl & 128) mask = 1.0;
    bool semi = (fl & 16) && semi_texel > 0.5;
    if (pass_mode == 1 && semi) discard;
    if (pass_mode == 2 && !semi) discard;
    float3 f1 = float3(0, 0, 0);
    if (semi) {
        int mode = (fl >> 5) & 3;
        if (mode == 0) { col *= 0.5; f1 = float3(0.5, 0.5, 0.5); }
        else if (mode == 3) { col *= 0.25; f1 = float3(1, 1, 1); }
        else f1 = float3(1, 1, 1);
    }
    o.c0 = float4(col, mask);
    o.c1 = float4(f1, 0);
    return o;
}
struct FOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FOut vs_full(uint id : SV_VertexID) {
    FOut o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
float4 ps_upload(FOut i) : SV_Target {
    // src.xy = upload size in texels, src.z = force mask
    int2 p = int2(i.uv * src.xy);
    uint t = upl.Load(int3(p, 0));
    return float4(float(t & 31u) / 31.0, float((t >> 5) & 31u) / 31.0, float((t >> 10) & 31u) / 31.0, max(float(t >> 15), src.z));
}
float4 ps_blit(FOut i) : SV_Target { return vram.SampleLevel(smp, i.uv, 0); }
float4 ps_present(FOut i) : SV_Target {
    float2 uv = src.xy + i.uv * src.zw;
    return float4(vram.SampleLevel(smp, uv, 0).rgb, 1);
}
)";

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static ID3DBlob* compile(const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(SHADER, strlen(SHADER), "c12", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        logf("shader %s: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    if (err) err->Release();
    return code;
}

static void set_cb(int pass_mode, float a = 0, float b = 0, float c2 = 0, float d = 0) {
    CB cb = {(float)S, S, pass_mode, g_cfg.filter, {a, b, c2, d}};
    ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
}

// the bound target's mapping from VRAM pixels: target pixel = (vram - m) * s
static void target_map(float& mx, float& my, float& sx, float& sy) {
    if (cur_target >= 2) { const XTarget& t = xt[cur_target - 2]; mx = t.mx; my = t.my; sx = t.sx; sy = t.sy; return; }
    mx = my = 0; sx = sy = (float)S;
}
static void set_vram_target(bool scissor) {
    if (cur_target >= 2) {
        const XTarget& t = xt[cur_target - 2];
        ctx->OMSetRenderTargets(1, &t.rtv, t.dsv);
        D3D11_VIEWPORT vp = {-t.mx * t.sx, -t.my * t.sy, 1024.0f * t.sx, 512.0f * t.sy, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(scissor ? rs_scissor : rs_plain);
        return;
    }
    bool il = cur_target && i_rtv;
    ctx->OMSetRenderTargets(1, il ? &i_rtv : &vram_rtv, dsv[il ? 1 : 0]);
    D3D11_VIEWPORT vp = {0, 0, 1024.0f * S, 512.0f * S, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(scissor ? rs_scissor : rs_plain);
}

static void add_dirty(int x1, int y1, int x2, int y2) {
    if (cur_target) return;   // the in-between layer is never sampled as a texture
    x1 = std::max(x1, 0); y1 = std::max(y1, 0); x2 = std::min(x2, 1024); y2 = std::min(y2, 512);
    if (x2 <= x1 || y2 <= y1) return;
    if (!dirty_any) { dirty[0] = x1; dirty[1] = y1; dirty[2] = x2; dirty[3] = y2; dirty_any = true; return; }
    dirty[0] = std::min(dirty[0], x1); dirty[1] = std::min(dirty[1], y1);
    dirty[2] = std::max(dirty[2], x2); dirty[3] = std::max(dirty[3], y2);
}

static void flush() {
    if (!nverts) return;
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, verts, nverts * sizeof(Vtx));
    ctx->Unmap(vbuf, 0);
    set_vram_target(true);
    float mx, my, tsx, tsy;
    target_map(mx, my, tsx, tsy);
    D3D11_RECT sc = {(LONG)floorf((cur_clip[0] - mx) * tsx), (LONG)floorf((cur_clip[1] - my) * tsy), (LONG)ceilf((cur_clip[2] + 1 - mx) * tsx), (LONG)ceilf((cur_clip[3] + 1 - my) * tsy)};
    ctx->RSSetScissorRects(1, &sc);
    UINT stride = sizeof(Vtx), off = 0;
    ctx->IASetVertexBuffers(0, 1, &vbuf, &stride, &off);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_prim, nullptr, 0);
    ctx->PSSetShader(ps_prim, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &copy_srv);
    ID3D11ShaderResourceView* rsrv = (ID3D11ShaderResourceView*)cur_rep;
    ctx->PSSetShaderResources(2, 1, &rsrv);
    ctx->PSSetSamplers(0, 1, &smp_linear);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    float bf[4] = {1, 1, 1, 1};
    ctx->OMSetDepthStencilState(ds_state[std::clamp(cur_kind >> 3, 0, 2)], 0);
    switch (cur_kind & 7) {
    case BK_OPAQUE: set_cb(0); ctx->OMSetBlendState(bs_opaque, bf, ~0u); ctx->Draw(nverts, 0); break;
    case BK_CUT: set_cb(1); ctx->OMSetBlendState(bs_opaque, bf, ~0u); ctx->Draw(nverts, 0); break;
    case BK_DUAL: set_cb(0); ctx->OMSetBlendState(bs_dual, bf, ~0u); ctx->Draw(nverts, 0); break;
    case BK_SUB: set_cb(0); ctx->OMSetBlendState(bs_sub, bf, ~0u); ctx->Draw(nverts, 0); break;
    case BK_SUB_TEX:
        set_cb(1); ctx->OMSetBlendState(bs_opaque, bf, ~0u); ctx->Draw(nverts, 0);
        set_cb(2); ctx->OMSetBlendState(bs_sub, bf, ~0u); ctx->Draw(nverts, 0);
        break;
    }
    nverts = 0;
}

static void sync_copy(int x1, int y1, int x2, int y2) {
    // make vram_copy current for the given native rect if it overlaps the dirty area
    if (!dirty_any) return;
    if (x2 <= dirty[0] || x1 >= dirty[2] || y2 <= dirty[1] || y1 >= dirty[3]) return;
    flush();
    D3D11_BOX box = {(UINT)dirty[0] * S, (UINT)dirty[1] * S, 0, (UINT)dirty[2] * S, (UINT)dirty[3] * S, 1};
    ctx->CopySubresourceRegion(vram_copy, 0, box.left, box.top, 0, vram_tex, 0, &box);
    dirty_any = false;
}

static void begin_prim(const GpuState& s, int kind, int need, int bx1, int by1, int bx2, int by2, void* rep_srv = nullptr) {
    if (s.textured) {
        int wdiv = s.depth == 0 ? 64 : s.depth == 1 ? 128 : 256;
        sync_copy(s.tpx, s.tpy, s.tpx + wdiv, s.tpy + 256);
        if (s.depth < 2) sync_copy(s.clutx, s.cluty, s.clutx + (s.depth == 0 ? 16 : 256), s.cluty + 1);
    }
    if (rep_srv != cur_rep) { flush(); cur_rep = rep_srv; }
    // a small clip window (scrolling text inside a box) moves with the widescreen HUD layout
    int cx1 = s.cx1, cx2 = s.cx2, cy1 = s.cy1, cy2 = s.cy2;
    if (s.xs_clip && s.xs_dx != 0.0f) { cx1 += (int)s.xs_dx; cx2 += (int)s.xs_dx; }
    if (s.xs_k != 1.0f && s.xs_clip) {
        cx1 = (int)floorf(s.xs_a + (cx1 - s.xs_a) * s.xs_k);
        cx2 = (int)ceilf(s.xs_a + (cx2 + 1 - s.xs_a) * s.xs_k) - 1;
    }
    if (s.xs_clip && s.ys != 0.0f) { cy1 += (int)s.ys; cy2 += (int)s.ys; }
    if (kind != cur_kind || cx1 != cur_clip[0] || cy1 != cur_clip[1] || cx2 != cur_clip[2] || cy2 != cur_clip[3] || nverts + need > MAXV) {
        flush();
        cur_kind = kind;
        cur_clip[0] = cx1; cur_clip[1] = cy1; cur_clip[2] = cx2; cur_clip[3] = cy2;
    }
    add_dirty(std::max(bx1, cx1), std::max(by1, cy1), std::min(bx2, cx2 + 1), std::min(by2, cy2 + 1));
}

static inline float XS(const GpuState& s, float x) { return s.xs_k == 1.0f ? x : s.xs_a + (x - s.xs_a) * s.xs_k; }

static int kind_of(const GpuState& s) {
    if (!s.semi) return BK_OPAQUE;
    if (s.blend == 2) return s.textured ? BK_SUB_TEX : BK_SUB;
    return BK_DUAL;
}

static void fill_attr(Vtx& o, const GpuState& s, const float* lim) {
    o.tp[0] = s.tpx; o.tp[1] = s.tpy; o.tp[2] = s.clutx; o.tp[3] = s.cluty;
    o.fl[0] = (s.textured ? 1 : 0) | (s.raw ? 2 : 0) | (s.depth << 2) | (s.semi ? 16 : 0) | (s.blend << 5) | (s.set_mask ? 128 : 0);
    o.fl[1] = s.tw_mask_x | (s.tw_mask_y << 8) | (s.tw_off_x << 16) | (s.tw_off_y << 24);
    o.fl[2] = o.fl[3] = 0;
    memcpy(o.lim, lim, sizeof o.lim);
}

static int depth_index() { return cur_target >= 2 ? cur_target : (cur_target && i_rtv) ? 1 : 0; }
static ID3D11DepthStencilView* depth_view(int t) { return t >= 2 ? xt[t - 2].dsv : dsv[t]; }
static bool depth_valid() {
    int t = depth_index();
    return depth_view(t) && g_frame - depth_frame[t] <= 4;
}
static bool selftest_done[2];
static void n3_selftest_draw(const GpuVtx* v, const GpuState& s);

static void hw_tri(const GpuVtx* v, const GpuState& s) {
    static int selftest_frame = -1;
    if (selftest_frame != g_frame) { selftest_frame = g_frame; selftest_done[0] = selftest_done[1] = false; }
    if (g_n3_selftest && g_prim_depth > 0.0f) {
        static GpuVtx tv[3];
        static GpuState ts;
        static bool have_tex;
        if (s.textured && !s.semi && (std::max({v[0].u, v[1].u, v[2].u}) - std::min({v[0].u, v[1].u, v[2].u})) >= 16) {
            memcpy(tv, v, sizeof tv); ts = s; have_tex = true;   // texture for the next frame's test quad
        }
        if (!selftest_done[cur_target ? 1 : 0]) {
            selftest_done[cur_target ? 1 : 0] = true;
            if (have_tex) {
                GpuState t = ts;
                t.cx1 = s.cx1; t.cy1 = s.cy1; t.cx2 = s.cx2; t.cy2 = s.cy2;
                n3_selftest_draw(tv, t);
            } else n3_selftest_draw(v, s);
        }
    }
    int minx = std::min({v[0].x, v[1].x, v[2].x}), maxx = std::max({v[0].x, v[1].x, v[2].x});
    int miny = std::min({v[0].y, v[1].y, v[2].y}), maxy = std::max({v[0].y, v[1].y, v[2].y});
    if (maxx - minx > 1023 || maxy - miny > 511) return;   // the console drops over-sized polygons
    float lim[4] = {0, 0, 255, 255};
    if (s.textured) {
        int u0 = std::min({v[0].u, v[1].u, v[2].u}), u1 = std::max({v[0].u, v[1].u, v[2].u});
        int v0 = std::min({v[0].v, v[1].v, v[2].v}), v1 = std::max({v[0].v, v[1].v, v[2].v});
        if (u1 != u0) u1--;
        if (v1 != v0) v1--;
        lim[0] = (float)u0; lim[1] = (float)v0; lim[2] = (float)u1; lim[3] = (float)v1;
    }
    const HdSub* hd = s.textured ? hdtex_lookup(s, (int)lim[0], (int)lim[1], (int)lim[2], (int)lim[3]) : nullptr;
    if (hd) { lim[0] = (float)hd->u0; lim[1] = (float)hd->v0; lim[2] = 1.0f / hd->w; lim[3] = 1.0f / hd->h; }
    if (s.xs_k != 1.0f) { int a = (int)floorf(XS(s, (float)minx)), b = (int)ceilf(XS(s, (float)maxx)); minx = std::min(a, b); maxx = std::max(a, b); }
    // primitives of the world list are hidden behind native geometry drawn this frame, at the depth of
    // their sort slot
    int dm = (g_n3_depth_ps1 && g_prim_depth > 0.0f && depth_valid()) ? DM_TEST : DM_OFF;
    begin_prim(s, kind_of(s) | dm << 3, 3, minx, miny, maxx + 1, maxy + 1, hd ? hd->srv : nullptr);
    for (int i = 0; i < 3; i++) {
        Vtx& o = verts[nverts++];
        o.x = XS(s, v[i].px);
        o.y = v[i].py;
        o.w = 1.0f;
        o.z = dm ? N3_DEPTH_K / g_prim_depth : 0.0f;   // the native depth mapping, K / z
        u32 col = s.gouraud ? v[i].rgb : v[0].rgb;
        if (s.textured && s.raw) col = 0x808080;
        o.color = col | 0xFF000000u;
        o.u = (float)v[i].u;
        o.v = (float)v[i].v;
        fill_attr(o, s, lim);
        if (hd) o.fl[0] |= 512 | (hd->stp ? 1024 : 0);
    }
}

static void quad(float x1, float y1, float x2, float y2, float u1, float v1, float u2, float v2, u32 rgb, const GpuState& s, const float* lim) {
    const float px[6] = {x1, x2, x1, x2, x2, x1}, py[6] = {y1, y1, y2, y1, y2, y2};
    const float pu[6] = {u1, u2, u1, u2, u2, u1}, pv[6] = {v1, v1, v2, v1, v2, v2};
    for (int i = 0; i < 6; i++) {
        Vtx& o = verts[nverts++];
        o.x = px[i]; o.y = py[i]; o.z = 0; o.w = 1;
        o.color = rgb | 0xFF000000u;
        o.u = pu[i]; o.v = pv[i];
        fill_attr(o, s, lim);
    }
}

static void hw_rect(int x, int y, int w, int h, int u, int v, u32 rgb, const GpuState& s) {
    float lim[4] = {0, 0, 1023, 1023};
    const HdSub* hd = (s.textured && u + w <= 256 && v + h <= 256) ? hdtex_lookup(s, u, v, u + w - 1, v + h - 1) : nullptr;
    if (hd) { lim[0] = (float)hd->u0; lim[1] = (float)hd->v0; lim[2] = 1.0f / hd->w; lim[3] = 1.0f / hd->h; }
    float fx1 = XS(s, (float)x), fx2 = XS(s, (float)(x + w));
    int dm = (g_n3_depth_ps1 && g_prim_depth > 0.0f && depth_valid()) ? DM_TEST : DM_OFF;   // world sprite
    begin_prim(s, kind_of(s) | dm << 3, 6, (int)floorf(fx1), y, (int)ceilf(fx2), y + h, hd ? hd->srv : nullptr);
    if (s.textured && s.raw) rgb = 0x808080;
    int first = nverts;
    // u/v wrap inside the 256 page: split not needed for typical sprites; the shader masks to 8 bits
    quad(fx1, (float)y, fx2, (float)(y + h), (float)u, (float)v, (float)(u + w), (float)(v + h), rgb, s, lim);
    if (hd) for (int i = first; i < nverts; i++) verts[i].fl[0] |= 512 | (hd->stp ? 1024 : 0);
    if (dm) for (int i = first; i < nverts; i++) verts[i].z = N3_DEPTH_K / g_prim_depth;
}

static void hw_line(const GpuVtx& a, const GpuVtx& b, const GpuState& s) {
    float axf = XS(s, (float)a.x), bxf = XS(s, (float)b.x);
    float dx = bxf - axf, dy = (float)(b.y - a.y);
    if (fabsf(dx) > 1023 || fabsf(dy) > 511) return;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny;
    if (len < 0.001f) { dx = 1; dy = 0; len = 1; }
    nx = -dy / len * 0.5f; ny = dx / len * 0.5f;
    float ex = dx / len * 0.5f, ey = dy / len * 0.5f;
    float lim[4] = {0, 0, 255, 255};
    int dm = (g_n3_depth_ps1 && g_prim_depth > 0.0f && depth_valid()) ? DM_TEST : DM_OFF;   // world line
    begin_prim(s, kind_of(s) | dm << 3, 6, (int)std::min(axf, bxf) - 1, std::min(a.y, b.y) - 1, (int)std::max(axf, bxf) + 2, std::max(a.y, b.y) + 2);
    float ax = axf + 0.5f - ex, ay = a.y + 0.5f - ey, bx = bxf + 0.5f + ex, by = b.y + 0.5f + ey;
    const float px[6] = {ax + nx, bx + nx, ax - nx, bx + nx, bx - nx, ax - nx};
    const float py[6] = {ay + ny, by + ny, ay - ny, by + ny, by - ny, ay - ny};
    const u32 pc[6] = {a.rgb, b.rgb, a.rgb, b.rgb, b.rgb, a.rgb};
    for (int i = 0; i < 6; i++) {
        Vtx& o = verts[nverts++];
        o.x = px[i]; o.y = py[i]; o.z = dm ? N3_DEPTH_K / g_prim_depth : 0.0f; o.w = 1;
        o.color = (s.gouraud ? pc[i] : a.rgb) | 0xFF000000u;
        o.u = o.v = 0;
        fill_attr(o, s, lim);
    }
}

// ---------------------------------------------------------------------------------------------
// Native 3D path (gpu3d.h). Triangles in view space are turned into clip space on the CPU:
//   clip = (((off_x + ofx) / 512 - 1) * z + x * h * ws / 512,  (1 - (off_y + ofy) / 256) * z - y * h / 256,  K,  z)
// which divides to exactly the GTE projection on the 1024x512 VRAM viewport. They share the vertex
// format, shader and batching with the PS1 polygons; fl & 4096 marks them for the vertex shader,
// which then clips at znear / zfar with SV_ClipDistance (the rasterizer interpolates perspective-correctly).
static N3View n3v;
static bool n3_on;
struct N3Queued { N3Vtx v[3]; N3Mat m; float z; };
static std::vector<N3Queued> n3_semi;

bool n3_available() { return dev != nullptr && vram_tex != nullptr; }

void n3_invalidate() { for (int& f : depth_frame) f = -1000; }

static GpuState n3_state(const N3Mat& m) {
    GpuState s{};
    s.textured = m.textured != 0; s.raw = m.raw != 0; s.semi = m.semi != 0; s.gouraud = true; s.dither = m.dither != 0;
    s.blend = m.blend & 3; s.depth = std::clamp(m.depth, 0, 2);
    s.tpx = m.tpx; s.tpy = m.tpy; s.clutx = m.clutx; s.cluty = m.cluty;
    s.tw_mask_x = m.tw_mask_x; s.tw_mask_y = m.tw_mask_y; s.tw_off_x = m.tw_off_x; s.tw_off_y = m.tw_off_y;
    s.cx1 = n3v.clip_x1; s.cy1 = n3v.clip_y1; s.cx2 = n3v.clip_x2; s.cy2 = n3v.clip_y2;
    s.set_mask = m.set_mask != 0;
    return s;
}

// kind: blend kind (BK_*), dm: depth mode (DM_*)
static void n3_emit(const N3Vtx* v, const N3Mat& m, int kind, int dm) {
    GpuState s = n3_state(m);
    float lim[4] = {0, 0, 255, 255};
    if (s.textured) {   // same texel limits as hw_tri (whole-texel UV rectangle of the polygon)
        float fu0 = std::min({v[0].u, v[1].u, v[2].u}), fu1 = std::max({v[0].u, v[1].u, v[2].u});
        float fv0 = std::min({v[0].v, v[1].v, v[2].v}), fv1 = std::max({v[0].v, v[1].v, v[2].v});
        int u0 = std::clamp((int)floorf(fu0 + 0.001f), 0, 255), u1 = std::clamp((int)ceilf(fu1 - 0.001f), 0, 256);
        int v0 = std::clamp((int)floorf(fv0 + 0.001f), 0, 255), v1 = std::clamp((int)ceilf(fv1 - 0.001f), 0, 256);
        if (u1 > u0) u1--;
        if (v1 > v0) v1--;
        u1 = std::min(u1, 255); v1 = std::min(v1, 255);
        lim[0] = (float)u0; lim[1] = (float)v0; lim[2] = (float)u1; lim[3] = (float)v1;
    }
    const HdSub* hd = s.textured ? hdtex_lookup(s, (int)lim[0], (int)lim[1], (int)lim[2], (int)lim[3]) : nullptr;
    if (hd) { lim[0] = (float)hd->u0; lim[1] = (float)hd->v0; lim[2] = 1.0f / hd->w; lim[3] = 1.0f / hd->h; }
    // the whole drawing area may change (the exact projected extent is unknown for near-clipped triangles)
    begin_prim(s, kind | dm << 3, 3, n3v.clip_x1, n3v.clip_y1, n3v.clip_x2 + 1, n3v.clip_y2 + 1, hd ? hd->srv : nullptr);
    float cx = (n3v.off_x + n3v.ofx) / 512.0f - 1.0f, cy = 1.0f - (n3v.off_y + n3v.ofy) / 256.0f;
    float kx = n3v.h * n3v.ws_scale / 512.0f, ky = n3v.h / 256.0f;
    s32 zn, zf;
    float fzn = std::max(n3v.znear, 1e-3f), fzf = n3v.zfar;
    memcpy(&zn, &fzn, 4); memcpy(&zf, &fzf, 4);
    for (int i = 0; i < 3; i++) {
        Vtx& o = verts[nverts++];
        o.x = cx * v[i].z + v[i].x * kx;
        o.y = cy * v[i].z - v[i].y * ky;
        o.z = N3_DEPTH_K;
        o.w = v[i].z;
        u32 col = v[i].rgb & 0xFFFFFF;
        if (s.textured && s.raw) col = 0x808080;
        o.color = col | 0xFF000000u;
        o.u = v[i].u;
        o.v = v[i].v;
        fill_attr(o, s, lim);
        o.fl[0] |= 4096;
        if (hd) o.fl[0] |= 512 | (hd->stp ? 1024 : 0);
        o.fl[2] = zn; o.fl[3] = zf;
    }
}

// Call while the target that should receive the 3D view is bound (VRAM, or the in-between layer
// inside the front-end's interpolation replay). Clears that target's depth buffer to far; it then
// stays valid for PS1 polygon depth tests until the next n3_begin on the target, n3_invalidate(),
// or 4 presented frames without an n3_begin.
void n3_begin(const N3View& v) {
    if (!n3_available()) return;
    flush();
    cur_kind = -1;
    n3v = v;
    n3v.clip_x1 = std::clamp(n3v.clip_x1, 0, 1023); n3v.clip_x2 = std::clamp(n3v.clip_x2, 0, 1023);
    n3v.clip_y1 = std::clamp(n3v.clip_y1, 0, 511); n3v.clip_y2 = std::clamp(n3v.clip_y2, 0, 511);
    if (n3v.ws_scale == 0.0f) n3v.ws_scale = 1.0f;
    int t = depth_index();
    if (depth_view(t)) ctx->ClearDepthStencilView(depth_view(t), D3D11_CLEAR_DEPTH, 0.0f, 0);
    depth_frame[t] = g_frame;
    n3_semi.clear();
    n3_on = true;
}

void n3_tri(const N3Vtx v[3], const N3Mat& m) {
    if (!n3_on) return;
    if (v[0].z <= n3v.znear && v[1].z <= n3v.znear && v[2].z <= n3v.znear) return;   // wholly behind the near plane
    if (m.semi) {
        N3Queued q;
        memcpy(q.v, v, sizeof q.v);
        q.m = m;
        q.z = v[0].z + v[1].z + v[2].z;
        n3_semi.push_back(q);
        return;
    }
    n3_emit(v, m, BK_OPAQUE, DM_WRITE);
}

void n3_end() {
    if (!n3_on) return;
    if (!n3_semi.empty()) {
        // 1) the opaque texels of semi-transparent textured triangles (no STP bit) are solid: draw and
        //    depth-write them first, in any order
        for (auto& q : n3_semi)
            if (q.m.textured) n3_emit(q.v, q.m, BK_CUT, DM_WRITE);
        // 2) everything semi-transparent, back to front by centroid depth, tested but not written
        std::stable_sort(n3_semi.begin(), n3_semi.end(), [](const N3Queued& a, const N3Queued& b) { return a.z > b.z; });
        for (auto& q : n3_semi) {
            GpuState s = n3_state(q.m);
            n3_emit(q.v, q.m, kind_of(s), DM_TEST);
        }
        n3_semi.clear();
    }
    flush();
    cur_kind = -1;
    n3_on = false;
}

void n3_option(const char* k, const char* v) {
    if (!strcmp(k, "n3_depth_ps1")) g_n3_depth_ps1 = atoi(v);
    else if (!strcmp(k, "n3_selftest")) g_n3_selftest = atoi(v);
}

// Debug self-test (n3_selftest 1): before the first 3D polygon of each frame (per target), draw a
// slowly turning textured quad (texture borrowed from that polygon) and a red semi-transparent quad in
// front of it. The PS1 scene drawn afterwards is depth-tested against them, so walls nearer than the
// quad cut through it and everything behind it is hidden.
static void n3_selftest_draw(const GpuVtx* pv, const GpuState& ps) {
    extern float g_gte_h;
    extern float g_ws_scale;
    N3View vw{};
    vw.h = g_gte_h > 1.0f ? g_gte_h : 256.0f;
    vw.clip_x1 = ps.cx1; vw.clip_y1 = ps.cy1; vw.clip_x2 = ps.cx2; vw.clip_y2 = ps.cy2;
    vw.off_x = ps.cx1; vw.off_y = ps.cy1;
    vw.ofx = (ps.cx2 - ps.cx1 + 1) * 0.5f; vw.ofy = (ps.cy2 - ps.cy1 + 1) * 0.5f;
    vw.ws_scale = g_ws_scale;
    vw.znear = 16.0f; vw.zfar = 0.0f;
    n3_begin(vw);
    float H = vw.h, a = g_frame * 0.02f, ca = cosf(a), sa = sinf(a);
    if (g_frame % 300 == 1) logf("n3 selftest: H %.1f clip %d,%d..%d,%d tex %d tp %d,%d depth %d\n", H, ps.cx1, ps.cy1, ps.cx2, ps.cy2, (int)ps.textured, ps.tpx, ps.tpy, ps.depth);
    N3Mat m{};
    m.textured = ps.textured && g_n3_selftest == 1;   // 2 = untextured (gouraud colours only) m.depth = ps.depth; m.tpx = ps.tpx; m.tpy = ps.tpy; m.clutx = ps.clutx; m.cluty = ps.cluty;
    m.tw_mask_x = ps.tw_mask_x; m.tw_mask_y = ps.tw_mask_y; m.tw_off_x = ps.tw_off_x; m.tw_off_y = ps.tw_off_y;
    float u0 = (float)std::min({pv[0].u, pv[1].u, pv[2].u}), u1 = (float)std::max({pv[0].u, pv[1].u, pv[2].u});
    float t0 = (float)std::min({pv[0].v, pv[1].v, pv[2].v}), t1 = (float)std::max({pv[0].v, pv[1].v, pv[2].v});
    if (u1 - u0 < 8) { u0 = 0; u1 = 64; }
    if (t1 - t0 < 8) { t0 = 0; t1 = 64; }
    // quad of half size 0.8H centred at (0, 0, 2.5H), turning about the vertical axis
    float hs = 0.8f * H, cz = 2.5f * H;
    const float lx[4] = {-hs, hs, hs, -hs}, ly[4] = {-hs, -hs, hs, hs};
    const float qu[4] = {u0, u1, u1, u0}, qv[4] = {t0, t0, t1, t1};
    const u32 qc[4] = {0x808080, 0x80FF80, 0xFF8080, 0x8080FF};
    N3Vtx q[4];
    for (int i = 0; i < 4; i++) q[i] = N3Vtx{lx[i] * ca, ly[i], cz + lx[i] * sa, qu[i], qv[i], qc[i]};
    N3Vtx t[3] = {q[0], q[1], q[2]};
    n3_tri(t, m);
    t[0] = q[0]; t[1] = q[2]; t[2] = q[3];
    n3_tri(t, m);
    // red semi-transparent (average) quad, partly in front of the textured one
    N3Mat r{};
    r.semi = 1; r.blend = 0;
    float rx = 0.3f * H, rz = 1.2f * H, rs = 0.25f * H;
    N3Vtx w[4] = {{rx - rs, -rs, rz, 0, 0, 0x0000FF}, {rx + rs, -rs, rz, 0, 0, 0x0000FF},
                  {rx + rs, rs, rz, 0, 0, 0x0000C0}, {rx - rs, rs, rz, 0, 0, 0x0000C0}};
    N3Vtx t2[3] = {w[0], w[1], w[2]};
    n3_tri(t2, r);
    t2[0] = w[0]; t2[1] = w[2]; t2[2] = w[3];
    n3_tri(t2, r);
    n3_end();
}

static void hw_fill(int x, int y, int w, int h, u32 rgb) {
    flush();
    if (w >= 256 && h >= 192) selftest_done[cur_target ? 1 : 0] = false;   // frame buffer cleared: redraw the test quads
    // direct clear through a scissored draw (ignores drawing area and mask)
    GpuState s{};
    s.cx1 = x; s.cy1 = y; s.cx2 = std::min(x + w, 1024) - 1; s.cy2 = std::min(y + h, 512) - 1;
    float lim[4] = {0, 0, 255, 255};
    begin_prim(s, BK_OPAQUE, 6, x, y, x + w, y + h);
    u32 q = ((rgb >> 3) & 31) | (((rgb >> 11) & 31) << 5) | (((rgb >> 19) & 31) << 10);
    u32 c = (((q & 31) * 255 / 31)) | ((((q >> 5) & 31) * 255 / 31) << 8) | ((((q >> 10) & 31) * 255 / 31) << 16);
    quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 0, 0, c, s, lim);
    flush();
    cur_kind = -1;
}

static void hw_copy(int sx, int sy, int dx, int dy, int w, int h, const GpuState&) {
    flush();
    w = std::min({w, 1024 - sx, 1024 - dx});
    h = std::min({h, 512 - sy, 512 - dy});
    if (w <= 0 || h <= 0) return;
    add_dirty(sx, sy, sx + w, sy + h);   // force the source into the copy
    sync_copy(sx, sy, sx + w, sy + h);
    D3D11_BOX box = {(UINT)sx * S, (UINT)sy * S, 0, (UINT)(sx + w) * S, (UINT)(sy + h) * S, 1};
    ctx->CopySubresourceRegion(vram_tex, 0, dx * S, dy * S, 0, vram_copy, 0, &box);
    add_dirty(dx, dy, dx + w, dy + h);
    hdtex_invalidate(dx, dy, w, h);
}

void mods_on_upload(int x, int y, int w, int h, const u16* data);

static void hw_upload(int x, int y, int w, int h, const u16* data, const GpuState& s) {
    flush();
    for (int j = 0; j < h; j++) memcpy(&g_vram[((y + j) & 511) * 1024 + (x & 1023)], data + j * w, std::min(w, 1024 - (x & 1023)) * 2);
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    if (w <= 0 || h <= 0) return;
    hdtex_on_upload(x, y, w, h);
    D3D11_MAPPED_SUBRESOURCE m;
    ctx->Map(upload_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    int ow = w;
    for (int j = 0; j < h; j++) memcpy((u8*)m.pData + j * m.RowPitch, &g_vram[(y + j) * 1024 + x], ow * 2);
    ctx->Unmap(upload_tex, 0);
    ctx->OMSetRenderTargets(1, &vram_rtv, nullptr);
    D3D11_VIEWPORT vp = {(float)x * S, (float)y * S, (float)w * S, (float)h * S, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_plain);
    float bf[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(bs_opaque, bf, ~0u);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_full, nullptr, 0);
    ctx->PSSetShader(ps_upload, nullptr, 0);
    ctx->PSSetShaderResources(1, 1, &upload_srv);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    set_cb(0, (float)w, (float)h, s.set_mask ? 1.0f : 0.0f, 0);
    ctx->Draw(3, 0);
    add_dirty(x, y, x + w, y + h);
    cur_kind = -1;
}

static bool read_region(int x, int y, int w, int h, std::vector<u32>& out, int step, ID3D11Texture2D* from = nullptr) {
    // reads [x,y,w,h] native rect at (S/step) x resolution into out (RGBA8)
    flush();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w * S; d.Height = h * S; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return false;
    D3D11_BOX box = {(UINT)x * S, (UINT)y * S, 0, (UINT)(x + w) * S, (UINT)(y + h) * S, 1};
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, from ? from : vram_tex, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
    int ow = w * S / step, oh = h * S / step;
    out.resize((size_t)ow * oh);
    for (int j = 0; j < oh; j++) {
        const u32* row = (const u32*)((const u8*)m.pData + (size_t)j * step * m.RowPitch);
        for (int i = 0; i < ow; i++) out[(size_t)j * ow + i] = row[i * step];
    }
    ctx->Unmap(st, 0);
    st->Release();
    return true;
}

static void hw_download(int x, int y, int w, int h, u16* out) {
    w = std::min(w, 1024 - x);
    h = std::min(h, 512 - y);
    std::vector<u32> px;
    if (w <= 0 || h <= 0 || !read_region(x, y, w, h, px, S)) return;
    for (int i = 0; i < w * h; i++) {
        u32 p = px[i];
        u32 r = ((p & 255) * 31 + 127) / 255, g = (((p >> 8) & 255) * 31 + 127) / 255, b = (((p >> 16) & 255) * 31 + 127) / 255;
        out[i] = (u16)(r | (g << 5) | (b << 10) | ((p >> 31) << 15));
    }
}

static void make_backbuffer() {
    rel(bb_rtv);
    ID3D11Texture2D* bb = nullptr;
    swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    bb_w = d.Width; bb_h = d.Height;
    dev->CreateRenderTargetView(bb, nullptr, &bb_rtv);
    bb->Release();
}

static void hw_present(const GpuDisplay& d) {
    flush();
    cur_kind = -1;
    if (!swap) return;
    if (g_window_resized) {
        g_window_resized = false;
        rel(bb_rtv);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        make_backbuffer();
    }
    float clear[4] = {0, 0, 0, 1};
    ctx->OMSetRenderTargets(1, &bb_rtv, nullptr);
    ctx->ClearRenderTargetView(bb_rtv, clear);
    if (!d.blank && bb_w > 0 && bb_h > 0) {
        double aspect = g_cfg.widescreen ? 16.0 / 9.0 : 4.0 / 3.0;
        float vw = (float)bb_w, vh = (float)(bb_w / aspect);
        if (vh > bb_h) { vh = (float)bb_h; vw = (float)(bb_h * aspect); }
        D3D11_VIEWPORT vp = {(bb_w - vw) / 2, (bb_h - vh) / 2, vw, vh, 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(rs_plain);
        float bf[4] = {1, 1, 1, 1};
        ctx->OMSetBlendState(bs_opaque, bf, ~0u);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_full, nullptr, 0);
        ctx->PSSetShader(ps_present, nullptr, 0);
        ID3D11ShaderResourceView* nul = nullptr;
        ctx->PSSetShaderResources(0, 1, (d.interp && i_srv) ? &i_srv : &vram_srv);
        ctx->PSSetSamplers(0, 1, g_cfg.present_linear ? &smp_linear : &smp_point);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
        set_cb(0, d.x / 1024.0f, d.y / 512.0f, d.w / 1024.0f, d.h / 512.0f);
        ctx->Draw(3, 0);
        ctx->PSSetShaderResources(0, 1, &nul);
    }
    {
        D3D11_VIEWPORT full = {0, 0, (float)bb_w, (float)bb_h, 0, 1};
        ctx->RSSetViewports(1, &full);
        ctx->OMSetRenderTargets(1, &bb_rtv, nullptr);
        ui_render();
    }
    if (g_bb_shot[0]) {   // test hook: capture exactly what the window shows (incl. the menu)
        ID3D11Texture2D* bb = nullptr;
        swap->GetBuffer(0, IID_PPV_ARGS(&bb));
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &st))) {
            ctx->CopyResource(st, bb);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<u32> px((size_t)td.Width * td.Height);
                for (UINT y = 0; y < td.Height; y++) {
                    const u32* row = (const u32*)((const u8*)m.pData + (size_t)y * m.RowPitch);
                    for (UINT x = 0; x < td.Width; x++) { u32 p = row[x]; px[(size_t)y * td.Width + x] = 0xFF000000u | ((p & 255) << 16) | (p & 0xFF00) | ((p >> 16) & 255); }
                }
                ctx->Unmap(st, 0);
                write_bmp(g_bb_shot, px.data(), td.Width, td.Height);
            }
            st->Release();
        }
        bb->Release();
        g_bb_shot[0] = 0;
    }
    // while a VR mod paces the frames the headset sets the rhythm: the window must not wait for its vsync
    extern void (*g_vr_pacer)(double until);
    bool vs = g_cfg.vsync && !g_vr_pacer;
    swap->Present(vs ? 1 : 0, (!vs && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
}

static bool hw_screenshot(const GpuDisplay& d, const char* path) {
    std::vector<u32> px;
    int step = S >= 4 ? S / 2 : 1;   // keep test screenshots at <= 2x
    if (!read_region(d.x, d.y, std::min(d.w, 1024 - d.x), std::min(d.h, 512 - d.y), px, step, (d.interp && itex) ? itex : nullptr)) return false;
    int w = std::min(d.w, 1024 - d.x) * S / step, h = std::min(d.h, 512 - d.y) * S / step;
    for (auto& p : px) p = 0xFF000000u | ((p & 255) << 16) | (p & 0xFF00) | ((p >> 16) & 255);
    // resample to the aspect ratio the window would show
    int ow = (int)(h * (g_cfg.widescreen ? 16.0 / 9.0 : 4.0 / 3.0) + 0.5);
    std::vector<u32> out((size_t)ow * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < ow; x++) out[(size_t)y * ow + x] = px[(size_t)y * w + (size_t)x * w / ow];
    return write_bmp(path, out.data(), ow, h);
}

// Change the internal resolution on the fly: the whole VRAM (frame buffers, textures, palettes)
// is carried over by a point-sampled blit, so nothing has to be re-uploaded by the game.
static void make_interp_layer() {
    rel(i_rtv); rel(i_srv); rel(itex);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1024 * S; td.Height = 512 * S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &itex))) return;
    dev->CreateRenderTargetView(itex, nullptr, &i_rtv);
    dev->CreateShaderResourceView(itex, nullptr, &i_srv);
    float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(i_rtv, zero);
}

// depth buffers for the native 3D path, one per target, same size as the VRAM render target
static void make_depth() {
    for (int t = 0; t < 2; t++) {
        rel(dsv[t]); rel(dtex[t]);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = 1024 * S; td.Height = 512 * S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &dtex[t]))) { logf("native 3D: depth buffer %d not created\n", t); continue; }
        dev->CreateDepthStencilView(dtex[t], nullptr, &dsv[t]);
        ctx->ClearDepthStencilView(dsv[t], D3D11_CLEAR_DEPTH, 0.0f, 0);
    }
    n3_invalidate();
}

static void hw_target(int which) {
    if (which == cur_target) return;
    flush();
    cur_target = which;
    cur_kind = -1;
}

void hw_set_scale(int ns) {
    ns = std::clamp(ns, 1, 16);
    if (!dev || !vram_tex || ns == S) return;
    flush();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1024 * ns; td.Height = 512 * ns; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *nt = nullptr, *nc = nullptr;
    ID3D11RenderTargetView* nrtv = nullptr;
    ID3D11ShaderResourceView *nsrv = nullptr, *ncsrv = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &nt))) { logf("resolution change to %dx failed (out of video memory?)\n", ns); return; }
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &nc))) { nt->Release(); return; }
    dev->CreateRenderTargetView(nt, nullptr, &nrtv);
    dev->CreateShaderResourceView(nt, nullptr, &nsrv);
    dev->CreateShaderResourceView(nc, nullptr, &ncsrv);
    ctx->OMSetRenderTargets(1, &nrtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, 1024.0f * ns, 512.0f * ns, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_plain);
    float bf[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(bs_opaque, bf, ~0u);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_full, nullptr, 0);
    ctx->PSSetShader(ps_blit, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &vram_srv);
    ctx->PSSetSamplers(0, 1, &smp_point);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul[3] = {nullptr, nullptr, nullptr};
    ctx->PSSetShaderResources(0, 3, nul);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rel(vram_rtv); rel(vram_srv); rel(copy_srv); rel(vram_tex); rel(vram_copy);
    vram_tex = nt; vram_copy = nc; vram_rtv = nrtv; vram_srv = nsrv; copy_srv = ncsrv;
    S = ns;
    make_interp_layer();
    make_depth();
    dirty[0] = 0; dirty[1] = 0; dirty[2] = 1024; dirty[3] = 512;
    dirty_any = true;
    cur_kind = -1;
    cur_rep = nullptr;
}

bool hw_backend_init() {
    S = std::clamp(g_cfg.scale, 1, 16);
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want, 1, D3D11_SDK_VERSION, &dev, &fl, &ctx))) return false;
    if (g_hwnd) {
        IDXGIDevice* dx = nullptr;
        IDXGIAdapter* ad = nullptr;
        IDXGIFactory2* fac = nullptr;
        dev->QueryInterface(IID_PPV_ARGS(&dx));
        dx->GetAdapter(&ad);
        ad->GetParent(IID_PPV_ARGS(&fac));
        IDXGIFactory5* f5 = nullptr;
        BOOL allow = FALSE;
        if (SUCCEEDED(fac->QueryInterface(IID_PPV_ARGS(&f5)))) {
            f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow);
            f5->Release();
        }
        tearing = allow;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        HRESULT hr = fac->CreateSwapChainForHwnd(dev, g_hwnd, &sd, nullptr, nullptr, &swap);
        fac->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
        fac->Release(); ad->Release(); dx->Release();
        if (FAILED(hr)) return false;
        make_backbuffer();
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = 1024 * S; td.Height = 512 * S; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &vram_tex))) return false;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &vram_copy))) return false;
    dev->CreateRenderTargetView(vram_tex, nullptr, &vram_rtv);
    dev->CreateShaderResourceView(vram_tex, nullptr, &vram_srv);
    dev->CreateShaderResourceView(vram_copy, nullptr, &copy_srv);
    float zero[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(vram_rtv, zero);
    td.Width = 1024; td.Height = 512; td.Format = DXGI_FORMAT_R16_UINT; td.Usage = D3D11_USAGE_DYNAMIC;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &upload_tex))) return false;
    dev->CreateShaderResourceView(upload_tex, nullptr, &upload_srv);

    ID3DBlob *b_vs = compile("vs_prim", "vs_5_0"), *b_ps = compile("ps_prim", "ps_5_0"), *b_vf = compile("vs_full", "vs_5_0"),
             *b_pu = compile("ps_upload", "ps_5_0"), *b_pp = compile("ps_present", "ps_5_0"), *b_pb = compile("ps_blit", "ps_5_0");
    if (!b_vs || !b_ps || !b_vf || !b_pu || !b_pp || !b_pb) return false;
    dev->CreatePixelShader(b_pb->GetBufferPointer(), b_pb->GetBufferSize(), nullptr, &ps_blit);
    dev->CreateVertexShader(b_vs->GetBufferPointer(), b_vs->GetBufferSize(), nullptr, &vs_prim);
    dev->CreatePixelShader(b_ps->GetBufferPointer(), b_ps->GetBufferSize(), nullptr, &ps_prim);
    dev->CreateVertexShader(b_vf->GetBufferPointer(), b_vf->GetBufferSize(), nullptr, &vs_full);
    dev->CreatePixelShader(b_pu->GetBufferPointer(), b_pu->GetBufferSize(), nullptr, &ps_upload);
    dev->CreatePixelShader(b_pp->GetBufferPointer(), b_pp->GetBufferSize(), nullptr, &ps_present);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_SINT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_SINT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(il, 6, b_vs->GetBufferPointer(), b_vs->GetBufferSize(), &layout))) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof verts; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &vbuf);
    bd.ByteWidth = sizeof(CB); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = 0;
    dev->CreateBuffer(&bd, nullptr, &cbuf);

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].RenderTargetWriteMask = 15;
    dev->CreateBlendState(&bl, &bs_opaque);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    dev->CreateBlendState(&bl, &bs_dual);
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    dev->CreateBlendState(&bl, &bs_sub);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = TRUE;
    dev->CreateRasterizerState(&rd, &rs_scissor);
    rd.ScissorEnable = FALSE;
    dev->CreateRasterizerState(&rd, &rs_plain);
    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sm, &smp_linear);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sm, &smp_point);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev->CreateDepthStencilState(&dd, &ds_state[DM_OFF]);
    dd.DepthEnable = TRUE; dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    dev->CreateDepthStencilState(&dd, &ds_state[DM_TEST]);
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_GREATER;
    dev->CreateDepthStencilState(&dd, &ds_state[DM_WRITE]);
    if (g_hwnd && swap) ui_init(g_hwnd, dev, ctx);
    make_interp_layer();
    make_depth();
    hdtex_init(g_cfg.hd_textures);
    atexit(hdtex_stats);
    logf("d3d11 renderer: %dx internal resolution (%dx%d VRAM)%s\n", S, 1024 * S, 512 * S, tearing ? ", tearing allowed" : "");
    return true;
}

// ---- extra targets for VR (gpu.h) ---------------------------------------------------------------
// (Re)make extra target `which` (2..5) at w x h with a depth buffer; false if out of video memory.
bool hw_xtarget_make(int which, int w, int h) {
    if (!dev || which < 2 || which > 6 || w <= 0 || h <= 0) return false;
    XTarget& t = xt[which - 2];
    if (t.tex && t.w == w && t.h == h) return true;
    flush();
    rel(t.rtv); rel(t.srv); rel(t.tex); rel(t.dsv); rel(t.dt);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &t.tex))) { t.w = t.h = 0; return false; }
    dev->CreateRenderTargetView(t.tex, nullptr, &t.rtv);
    dev->CreateShaderResourceView(t.tex, nullptr, &t.srv);
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t.dt))) dev->CreateDepthStencilView(t.dt, nullptr, &t.dsv);
    t.w = w; t.h = h;
    depth_frame[which] = -1000;
    return true;
}

// Show the VRAM display area x, y, w x h on the whole of extra target `which`, and clear it.
void hw_xtarget_begin(int which, int x, int y, int w, int h, const float rgba[4]) {
    if (which < 2 || which > 6) return;
    XTarget& t = xt[which - 2];
    if (!t.rtv || w <= 0 || h <= 0) return;
    flush();
    t.mx = (float)x; t.my = (float)y; t.sx = (float)t.w / w; t.sy = (float)t.h / h;
    ctx->ClearRenderTargetView(t.rtv, rgba);
    if (t.dsv) ctx->ClearDepthStencilView(t.dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    depth_frame[which] = -1000;
    cur_kind = -1;
}

// test: write extra target `which` to a BMP
bool hw_xtarget_save(int which, const char* path) {
    if (which < 2 || which > 6) return false;
    XTarget& t = xt[which - 2];
    if (!t.tex) return false;
    flush();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = t.w; d.Height = t.h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &st))) return false;
    ctx->CopyResource(st, t.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); return false; }
    std::vector<u32> px((size_t)t.w * t.h);
    for (int y = 0; y < t.h; y++) {
        const u32* row = (const u32*)((const u8*)m.pData + (size_t)y * m.RowPitch);
        for (int x = 0; x < t.w; x++) { u32 p = row[x]; px[(size_t)y * t.w + x] = 0xFF000000u | ((p & 255) << 16) | (p & 0xFF00) | ((p >> 16) & 255); }
    }
    ctx->Unmap(st, 0);
    st->Release();
    return write_bmp(path, px.data(), t.w, t.h);
}

// show another VRAM area on the bound extra target, without clearing it
void hw_xtarget_remap(int x, int y, int w, int h) {
    if (cur_target < 2 || w <= 0 || h <= 0) return;
    XTarget& t = xt[cur_target - 2];
    flush();
    t.mx = (float)x; t.my = (float)y; t.sx = (float)t.w / w; t.sy = (float)t.h / h;
    cur_kind = -1;
}

void* hw_xtarget_texture(int which) { return (which >= 2 && which <= 6) ? (void*)xt[which - 2].tex : nullptr; }

// the picture the window shows (the display area of VRAM or of the in-between layer), for a flat
// "cinema" screen: copied into extra target `which`, stretched over it
void hw_xtarget_copy_display(int which, const GpuDisplay& d) {
    if (which < 2 || which > 6) return;
    XTarget& t = xt[which - 2];
    if (!t.rtv) return;
    flush();
    ctx->OMSetRenderTargets(1, &t.rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)t.w, (float)t.h, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_plain);
    float bf[4] = {1, 1, 1, 1};
    ctx->OMSetBlendState(bs_opaque, bf, ~0u);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_full, nullptr, 0);
    ctx->PSSetShader(ps_present, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, (d.interp && i_srv) ? &i_srv : &vram_srv);
    ctx->PSSetSamplers(0, 1, &smp_linear);
    ctx->PSSetConstantBuffers(0, 1, &cbuf);
    set_cb(0, d.x / 1024.0f, d.y / 512.0f, d.w / 1024.0f, d.h / 512.0f);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    ctx->PSSetShaderResources(0, 1, &nul);
    cur_kind = -1;
}

GpuBackend g_hw_backend = {hw_tri, hw_rect, hw_line, hw_fill, hw_copy, hw_upload, hw_download, hw_present, hw_screenshot, hw_target};
