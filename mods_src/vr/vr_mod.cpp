// mods\vr.dll - VR for C-12 with an OpenXR headset.
//
// The runtime draws the pictures (c12_mod_api.h version 4, runtime/vr.cpp): the last world frame once per
// eye from the eye's own pose and lens - real stereo, the native 3D scene blended between game frames -,
// the HUD, and a flat copy of the window's picture. This mod runs the OpenXR session (xr.h, the layer
// from the VCS VR mod) and paces the game with the headset: while the game waits for its next frame the
// mod renders headset frames, each with the newest head pose.
//
// What the headset shows:
//   * in play: the stereo view (projection layer) and the HUD on a quad that follows the head,
//   * menus, loading screens, films (no 3D view): the window's picture on a "cinema" quad in front.
// The head looks around the game camera; with the first-person mod the camera itself turns with the head
// (it reads the head pose, see first_person.c), so W/A/S/D and aiming follow where you look.
// Keys: F8 recentre (also on start).
#include "c12_mod_api.h"
#include "xr.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>

namespace
{
    const C12ModApi *g_api = nullptr;
    int g_enabled = 0;
    float g_units_per_m = 183.0f;   // the hero (about 330 units) as a 1.8 m person
    float g_render_scale = 1.0f;
    float g_hud_dist = 1.4f, g_hud_width = 1.5f;
    float g_cinema_dist = 2.5f, g_cinema_width = 2.6f;
    int g_snap_turn = 1;             // right stick: 30 degree snaps (else smooth)
    float g_snap_deg = 30.0f, g_smooth_speed = 120.0f;   // degrees, degrees per second
    int g_aim_controller = 1;        // shots along the right controller
    int g_beam = 1;                  // laser beam from the right controller
    int g_hud_lazy = 1;              // the HUD follows the head's turn lazily (else fixed to the head)
    bool g_snapped;                  // the stick returned to centre since the last snap
    double g_last_frame_t;
    float g_hud_yaw;                 // the lazy HUD's direction (radians about +y)
    bool g_hud_yaw_ok;
    bool g_was_trigger;

    // Motion controllers -> the PSX pad (C-12's controls):
    //   left stick move (also menus), right stick turn (snap / smooth), right trigger = Cross (fire, accept),
    //   left trigger = Circle (crouch), right grip = R1 (lock on), left grip = L1 (next weapon),
    //   A = Square (use), B = Triangle (back), X = R2 (scope view), Y = Select, menu = Start, right stick click = L2
    void controls(const vr::Hand h[2], double dt)
    {
        const vr::Hand &L = h[0], &R = h[1];
        uint32_t b = 0;
        if (R.trigger > 0.6f) b |= 1u << 14;
        if (L.trigger > 0.6f) b |= 1u << 13;
        if (R.squeeze > 0.6f) b |= 1u << 11;
        if (L.squeeze > 0.6f) b |= 1u << 10;
        if (R.primary) b |= 1u << 15;
        if (R.secondary) b |= 1u << 12;
        if (L.primary) b |= 1u << 9;
        if (L.secondary) b |= 1u << 0;
        if (L.menu || R.menu) b |= 1u << 3;
        if (R.stickClick) b |= 1u << 8;
        int lx = L.active ? (int)(L.stick[0] * 127.0f) : 0, ly = L.active ? (int)(-L.stick[1] * 127.0f) : 0;
        g_api->pad_inject(b, lx, ly, 0, 0);
        // turning with the right stick
        float sx = R.active ? R.stick[0] : 0.0f;
        if (g_snap_turn)
        {
            if (fabsf(sx) < 0.3f) g_snapped = false;
            else if (!g_snapped && fabsf(sx) > 0.7f)
            {
                g_snapped = true;
                g_api->turn((sx > 0 ? 1.0f : -1.0f) * g_snap_deg * (4096.0f / 360.0f), 0.0f);
            }
        }
        else if (fabsf(sx) > 0.2f)
            g_api->turn(sx * g_smooth_speed * (float)dt * (4096.0f / 360.0f), 0.0f);
        // a short buzz when the trigger fires
        bool trig = R.trigger > 0.6f;
        if (trig && !g_was_trigger) vr::haptic(1, 0.6f, 0.05f);
        g_was_trigger = trig;
    }

    // the aim ray and beam from the right controller, the hands for the first-person mod
    void hands(const vr::Hand h[2])
    {
        for (int i = 0; i < 2; i++)
        {
            if (!h[i].active) { g_api->vr_set_hand(i, nullptr); continue; }
            // held where the hand grips, pointing where the controller aims (the grip pose's axes follow the
            // tilted handle: a gun held by them pointed up while the shots and the laser went straight ahead)
            C12VrPose p;
            memcpy(p.q, h[i].aim.q, sizeof p.q);
            memcpy(p.p, h[i].grip.p, sizeof p.p);
            g_api->vr_set_hand(i, &p);
        }
        const vr::Hand &R = h[1];
        C12VrPose aim;
        memcpy(aim.q, R.aim.q, sizeof aim.q);
        memcpy(aim.p, R.aim.p, sizeof aim.p);
        float o[3], rot[9];
        if (R.active && g_aim_controller && g_api->vr_to_world(&aim, o, rot)) g_api->set_aim_ray(1, o, rot);
        else g_api->set_aim_ray(0, nullptr, nullptr);
        g_api->vr_set_beam(R.active && g_beam, &aim, 25.0f, 0x1818b0);
    }

    bool g_on;                // session started (or waiting for the headset)
    double g_retry_at;
    bool g_was_f8;
    bool g_need_recentre = true;

    ID3D11Device *g_dev;
    ID3D11DeviceContext *g_ctx;
    ID3D11VertexShader *g_vs;
    ID3D11PixelShader *g_ps_hud;
    ID3D11Buffer *g_cb;
    ID3D11VertexShader *g_vs_panel;
    ID3D11PixelShader *g_ps_panel;
    ID3D11Buffer *g_cb_panel;
    ID3D11BlendState *g_blend_pm;
    ID3D11SamplerState *g_smp;

    void gameLog(const char *line)
    {
        if (g_api && g_api->log)
            g_api->log(line);
    }

    // HUD over black (t0) and over white (t1) -> premultiplied colour + coverage
    const char *kShader = R"(
cbuffer cb : register(b0) { float srgb; float3 pad; };
Texture2D tb : register(t0);
Texture2D tw : register(t1);
SamplerState smp : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o;
    float2 p = float2((id << 1) & 2, id & 2);
    o.uv = p;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
// a flat panel (HUD, menu screen) drawn into an eye's picture: corners already projected (clip space)
cbuffer pc : register(b1) { float4 cpos[4]; float4 cuv[4]; float4 cmode; };
V vs_panel(uint id : SV_VertexID) {
    V o;
    o.pos = cpos[id];
    o.uv = cuv[id].xy;
    return o;
}
float4 ps_panel(V i) : SV_Target {
    float3 b = tb.SampleLevel(smp, i.uv, 0).rgb;
    float a = 1.0;
    if (cmode.y > 0.5) {   // HUD: over black and over white -> premultiplied colour + coverage
        float3 w = tw.SampleLevel(smp, i.uv, 0).rgb;
        a = saturate(1.0 - dot(w - b, float3(1, 1, 1)) / 3.0);
    }
    if (cmode.x > 0.5) b = pow(max(b, 0), 2.2);
    return float4(b, a);
}
float4 ps_hud(V i) : SV_Target {
    float3 b = tb.SampleLevel(smp, i.uv, 0).rgb;
    float3 w = tw.SampleLevel(smp, i.uv, 0).rgb;
    float a = saturate(1.0 - dot(w - b, float3(1, 1, 1)) / 3.0);
    // an sRGB render target encodes what it gets: hand it linear values so the bytes stay the game's
    if (srgb > 0.5) b = pow(max(b, 0), 2.2);
    return float4(b, a);
}
)";

    // A render target view on a swapchain image. SteamVR (and Oculus) make the images typeless
    // (R8G8B8A8_TYPELESS): a view with no description fails on them, so the typed format is named.
    HRESULT makeRtv(ID3D11Texture2D *tex, bool srgb, ID3D11RenderTargetView **out)
    {
        D3D11_TEXTURE2D_DESC d = {};
        tex->GetDesc(&d);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.ViewDimension = d.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        switch (d.Format)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: rd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: rd.Format = srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM; break;
        default: rd.Format = d.Format; break;
        }
        const HRESULT hr = g_dev->CreateRenderTargetView(tex, &rd, out);
        static bool logged;
        if (FAILED(hr) && !logged)
        {
            logged = true;
            vr::log("panels: CreateRenderTargetView failed (0x%08x) on a %ux%u image of DXGI format %d (view format %d)\n", (unsigned)hr, d.Width, d.Height, (int)d.Format, (int)rd.Format);
        }
        return hr;
    }

    bool compileShader(const char *entry, const char *target, ID3DBlob **blob)
    {
        ID3DBlob *e = nullptr;
        if (SUCCEEDED(D3DCompile(kShader, strlen(kShader), "vr", nullptr, nullptr, entry, target, 0, 0, blob, &e)))
        {
            if (e) e->Release();
            return true;
        }
        vr::log("panels: shader %s did not compile: %s\n", entry, e ? (const char *)e->GetBufferPointer() : "(no message)");
        if (e) e->Release();
        return false;
    }

    bool makeShaders()
    {
        if (g_ps_hud)
            return true;
        ID3DBlob *b = nullptr;
        if (!compileShader("vs", "vs_5_0", &b))
            return false;
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs);
        b->Release();
        if (!compileShader("ps_hud", "ps_5_0", &b))
            return false;
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps_hud);
        b->Release();
        if (!compileShader("vs_panel", "vs_5_0", &b))
            return false;
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs_panel);
        b->Release();
        if (!compileShader("ps_panel", "ps_5_0", &b))
            return false;
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps_panel);
        b->Release();
        {
            D3D11_BUFFER_DESC pd = {};
            pd.ByteWidth = sizeof(float) * 36;
            pd.Usage = D3D11_USAGE_DEFAULT;
            pd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            g_dev->CreateBuffer(&pd, nullptr, &g_cb_panel);
            D3D11_BLEND_DESC bl = {};
            bl.RenderTarget[0].BlendEnable = TRUE;
            bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            g_dev->CreateBlendState(&bl, &g_blend_pm);
        }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &g_cb);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_dev->CreateSamplerState(&sd, &g_smp);
        return g_vs && g_ps_hud && g_cb && g_smp && g_vs_panel && g_ps_panel && g_cb_panel && g_blend_pm;
    }

    void qrot(const float q[4], const float v[3], float o[3])
    {
        float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
        o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
        o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
        o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
    }

    // Draw a flat panel (width x height metres at `pose`, tracking space) into an eye's picture `dst`.
    // The HUD and the menu screen used to be quad layers for the runtime to place; on a PSVR2 with
    // SteamVR none of them showed. tex1 set: the HUD (over black / over white) blended over the scene;
    // else an opaque picture. Returns false when the panel is behind the eye.
    bool drawPanel(ID3D11Texture2D *dst, bool srgb, int w, int h, const vr::View &eye, const vr::Pose &pose,
                   float width, float height, void *tex0, void *tex1)
    {
        if (!dst || !tex0 || !makeShaders())
            return false;
        float iq[4] = {-eye.pose.q[0], -eye.pose.q[1], -eye.pose.q[2], eye.pose.q[3]};
        float tl = tanf(eye.fov.left), tr = tanf(eye.fov.right), tu = tanf(eye.fov.up), td = tanf(eye.fov.down);
        float cb[36] = {};
        static const float cx[4] = {-0.5f, 0.5f, -0.5f, 0.5f}, cy[4] = {0.5f, 0.5f, -0.5f, -0.5f};
        for (int k = 0; k < 4; k++)
        {
            float loc[3] = {cx[k] * width, cy[k] * height, 0.0f}, wv[3], d[3], e[3];
            qrot(pose.q, loc, wv);
            for (int i = 0; i < 3; i++) d[i] = pose.p[i] + wv[i] - eye.pose.p[i];
            qrot(iq, d, e);   // eye space: x right, y up, -z ahead
            float z = -e[2];
            if (z < 0.02f)
                return false;
            // clip space with w = depth: x/w = (x/z - centre) / half width of the lens' tangents
            cb[k * 4 + 0] = (2.0f * e[0] - (tr + tl) * z) / (tr - tl);
            cb[k * 4 + 1] = (2.0f * e[1] - (tu + td) * z) / (tu - td);
            cb[k * 4 + 2] = 0.5f * z;
            cb[k * 4 + 3] = z;
            cb[16 + k * 4 + 0] = cx[k] + 0.5f;
            cb[16 + k * 4 + 1] = 0.5f - cy[k];
        }
        cb[32] = srgb ? 1.0f : 0.0f;
        cb[33] = tex1 ? 1.0f : 0.0f;
        ID3D11RenderTargetView *rtv = nullptr;
        if (FAILED(makeRtv(dst, srgb, &rtv)))
            return false;
        ID3D11ShaderResourceView *srv[2] = {};
        g_dev->CreateShaderResourceView((ID3D11Texture2D *)tex0, nullptr, &srv[0]);
        if (tex1)
            g_dev->CreateShaderResourceView((ID3D11Texture2D *)tex1, nullptr, &srv[1]);
        g_ctx->UpdateSubresource(g_cb_panel, 0, nullptr, cb, 0, 0);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(nullptr);
        g_ctx->OMSetBlendState(tex1 ? g_blend_pm : nullptr, nullptr, ~0u);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->VSSetShader(g_vs_panel, nullptr, 0);
        g_ctx->VSSetConstantBuffers(1, 1, &g_cb_panel);
        g_ctx->PSSetShader(g_ps_panel, nullptr, 0);
        g_ctx->PSSetConstantBuffers(1, 1, &g_cb_panel);
        g_ctx->PSSetShaderResources(0, 2, srv);
        g_ctx->PSSetSamplers(0, 1, &g_smp);
        g_ctx->Draw(4, 0);
        ID3D11ShaderResourceView *nul[2] = {};
        g_ctx->PSSetShaderResources(0, 2, nul);
        ID3D11RenderTargetView *nrt = nullptr;
        g_ctx->OMSetRenderTargets(1, &nrt, nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, ~0u);
        rtv->Release();
        if (srv[0]) srv[0]->Release();
        if (srv[1]) srv[1]->Release();
        return true;
    }

    // an eye's picture cleared to black (the menu screen floats in the dark)
    ID3D11Texture2D *blackEye(vr::Panel panel, int w, int h, bool &srgb)
    {
        ID3D11Texture2D *dst = vr::panelImage(panel, (uint32_t)w, (uint32_t)h, srgb);
        if (!dst)
            return nullptr;
        ID3D11RenderTargetView *rtv = nullptr;
        if (SUCCEEDED(makeRtv(dst, srgb, &rtv)))
        {
            const float black[4] = {0, 0, 0, 1};
            g_ctx->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
        return dst;
    }

    void copyInto(vr::Panel panel, void *src, int w, int h)
    {
        bool srgb = false;
        ID3D11Texture2D *dst = vr::panelImage(panel, (uint32_t)w, (uint32_t)h, srgb);
        if (!dst || !src)
            return;
        // the same 8-bit RGBA family: the bytes are copied as they are (sRGB-encoded either way)
        g_ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, (ID3D11Texture2D *)src, 0, nullptr);
    }

    void hudInto(void *black, void *white, int w, int h)
    {
        bool srgb = false;
        ID3D11Texture2D *dst = vr::panelImage(vr::Panel::Hud, (uint32_t)w, (uint32_t)h, srgb);
        if (!dst || !black || !white || !makeShaders())
            return;
        ID3D11RenderTargetView *rtv = nullptr;
        ID3D11ShaderResourceView *srv[2] = {};
        if (FAILED(makeRtv(dst, srgb, &rtv)))
            return;
        g_dev->CreateShaderResourceView((ID3D11Texture2D *)black, nullptr, &srv[0]);
        g_dev->CreateShaderResourceView((ID3D11Texture2D *)white, nullptr, &srv[1]);
        float cb[4] = {srgb ? 1.0f : 0.0f, 0, 0, 0};
        g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
        g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(nullptr);
        g_ctx->OMSetBlendState(nullptr, nullptr, ~0u);
        g_ctx->OMSetDepthStencilState(nullptr, 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps_hud, nullptr, 0);
        g_ctx->PSSetShaderResources(0, 2, srv);
        g_ctx->PSSetSamplers(0, 1, &g_smp);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->Draw(3, 0);
        ID3D11ShaderResourceView *nul[2] = {};
        g_ctx->PSSetShaderResources(0, 2, nul);
        ID3D11RenderTargetView *nrt = nullptr;
        g_ctx->OMSetRenderTargets(1, &nrt, nullptr);
        rtv->Release();
        if (srv[0]) srv[0]->Release();
        if (srv[1]) srv[1]->Release();
    }

    // a point `dist` metres ahead of pose `p` (OpenXR: -z ahead), facing back at it, `down` metres lower
    vr::Pose ahead(const vr::Pose &p, float dist, float down)
    {
        const float *q = p.q;
        float v[3] = {0.0f, -down, -dist};
        // rotate v by q
        float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
        vr::Pose o = p;
        o.p[0] = p.p[0] + v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
        o.p[1] = p.p[1] + v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
        o.p[2] = p.p[2] + v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
        return o;
    }

    // The headset's predicted display time on the game's clock (time_now): the scene is blended to the
    // moment each picture is seen, so motion is even at any refresh rate. XrTime and the game's clock
    // both run steadily; the offset between them is set once (and again only if they drift apart).
    double g_xr_offset;
    bool g_xr_offset_ok;
    int64_t g_last_display;
    // frame timing for vr.log: headset frames, how many came later than one display period, and the
    // longest stretch the game itself took between two headset frames
    int g_stat_frames, g_stat_late;
    double g_stat_t0, g_stat_game_max, g_frame_end_t;
    void displayTime(double now)
    {
        const int64_t xt = vr::predictedDisplayTime();
        if (!xt || g_api->version < 6 || !g_api->vr_set_display_time)
            return;
        const double xs = xt * 1e-9;
        if (!g_xr_offset_ok || fabs(xs + g_xr_offset - (now + 0.02)) > 0.1)
        {
            g_xr_offset = now + 0.02 - xs;
            g_xr_offset_ok = true;
        }
        g_api->vr_set_display_time(xs + g_xr_offset);
        const double period = vr::refreshHz() > 0 ? 1.0 / vr::refreshHz() : 0.011;
        if (g_last_display && (xt - g_last_display) * 1e-9 > period * 1.5)
            g_stat_late++;
        g_last_display = xt;
        g_stat_frames++;
        if (g_stat_t0 == 0)
            g_stat_t0 = now;
        if (now - g_stat_t0 >= 10.0)
        {
            vr::log("frames: %.1f a second, %d late (skipped a display refresh), game took up to %.1f ms between frames\n",
                    g_stat_frames / (now - g_stat_t0), g_stat_late, g_stat_game_max * 1000.0);
            g_stat_frames = g_stat_late = 0;
            g_stat_game_max = 0;
            g_stat_t0 = now;
        }
    }

    // Precise frame timing for finding stutter: a summary in vr.log every 10 s and, when the environment
    // variable C12_VR_TIMING_CSV is set, one CSV row per headset frame (vr_timing.csv next to the exe). gpu_ms is the GPU's own time for the eye pictures and HUD
    // (D3D11 timestamp queries, read a few frames later). Columns:
    //   t          game clock, seconds
    //   tick       1 = the game ran its own frame just before this headset frame
    //   gap_ms     time since the previous headset frame was handed in (the game's work when tick = 1)
    //   wait_ms    blocked in xrWaitFrame (about 0 = this frame was already late)
    //   locate_ms  head, views, controller input
    //   render_ms  vr_render on the CPU (the runtime drawing both eyes + HUD)
    //   post_ms    HUD / menu screen drawn into the eye pictures, copies
    //   end_ms     xrEndFrame (blocks when the GPU is behind)
    //   work_ms    everything from the end of the wait to the end of xrEndFrame
    //   disp_dt_ms predicted display time minus the previous frame's (one period when no refresh was skipped)
    //   gpu_ms     GPU time of this frame's work (-1 not available)
    //   head_dps   head turn speed in degrees per second, yaw / pitch in degrees
    //   scene      1 stereo view, 2 menu screen, 0 nothing; focused: the app has the headset's input focus
    namespace prof
    {
        struct Row
        {
            double t = 0, gap = 0, wait = 0, loc = 0, render = 0, post = 0, end = 0, work = 0, dispDt = 0, gpu = -1, dps = 0, yaw = 0, pitch = 0;
            int scene = 0, focused = 0, rendered = 0;
            bool tick = false;
        };
        struct Slot
        {
            Row row;
            bool used = false, open = false, q = false;
            ID3D11Query *dj = nullptr, *a = nullptr, *b = nullptr;
        };
        const int kMaxRows = 72000;   // 10 minutes at 120 Hz
        Slot slots[4];
        unsigned n = 0;
        Row cur;
        Slot *curSlot = nullptr;
        FILE *csv = nullptr;
        bool csvTried = false;
        int rows = 0;
        std::vector<double> wWork, wWait, wRender, wPost, wEnd, wGpu, wGap, wDps;
        int wFrames = 0, wTicks = 0, wTickMissed = 0, wMissed = 0;
        double wT0 = 0;
        int64_t lastXt = 0;
        float prevQ[4] = {0, 0, 0, 1};
        bool prevOk = false;

        std::string pct(std::vector<double> &v)
        {
            if (v.empty())
                return "-";
            std::sort(v.begin(), v.end());
            const size_t last = v.size() - 1;
            char b[96];
            snprintf(b, sizeof b, "%.1f/%.1f/%.1f/%.1f", v[last / 2], v[std::min(last, (size_t)(v.size() * 0.95))],
                     v[std::min(last, (size_t)(v.size() * 0.99))], v[last]);
            return b;
        }

        void openCsv()
        {
            csvTried = true;
            if (!GetEnvironmentVariableA("C12_VR_TIMING_CSV", nullptr, 0))   // opt-in: the summaries in vr.log are always written
                return;
            char path[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            char *s = strrchr(path, '\\');
            if (!s)
                return;
            strcpy(s + 1, "vr_timing.csv");
            csv = fopen(path, "w");
            if (csv)
            {
                fprintf(csv, "t,tick,gap_ms,wait_ms,locate_ms,render_ms,post_ms,end_ms,work_ms,disp_dt_ms,gpu_ms,head_dps,yaw,pitch,scene,focused,rendered\n");
                vr::log("timing: writing %s\n", path);
            }
        }

        void summary(double t)
        {
            const float hz = vr::refreshHz();
            vr::log("timing (%d headset frames, %.0f Hz; ms as median/p95/p99/max):\n", wFrames, hz);
            vr::log("  work   %s   (wait end of xrWaitFrame -> end of xrEndFrame; budget %.1f)\n", pct(wWork).c_str(), hz > 0 ? 1000.0 / hz : 0.0);
            vr::log("  wait   %s   render %s   post %s   end %s\n", pct(wWait).c_str(), pct(wRender).c_str(), pct(wPost).c_str(), pct(wEnd).c_str());
            vr::log("  gpu    %s\n", pct(wGpu).c_str());
            vr::log("  game ticks %d, game work between headset frames %s; ticks followed by a skipped refresh: %d; skipped refreshes in all: %d\n",
                    wTicks, pct(wGap).c_str(), wTickMissed, wMissed);
            double avg = 0, mx = 0;
            for (double d : wDps) { avg += d; mx = std::max(mx, d); }
            vr::log("  head speed avg %.0f max %.0f deg/s\n", wDps.empty() ? 0.0 : avg / wDps.size(), mx);
            if (csv)
                fflush(csv);
            wWork.clear(); wWait.clear(); wRender.clear(); wPost.clear(); wEnd.clear(); wGpu.clear(); wGap.clear(); wDps.clear();
            wFrames = wTicks = wTickMissed = wMissed = 0;
            wT0 = t;
        }

        void finalize(Slot &s)
        {
            if (!s.used)
                return;
            s.used = false;
            Row r = s.row;
            if (s.q && g_ctx)
            {
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
                UINT64 a = 0, b = 0;
                if (g_ctx->GetData(s.dj, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    g_ctx->GetData(s.a, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    g_ctx->GetData(s.b, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency)
                    r.gpu = (double)(b - a) * 1000.0 / (double)dj.Frequency;
            }
            if (!csvTried)
                openCsv();
            if (csv && rows < kMaxRows)
            {
                fprintf(csv, "%.4f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%d,%d,%d\n", r.t, r.tick ? 1 : 0, r.gap, r.wait, r.loc,
                        r.render, r.post, r.end, r.work, r.dispDt, r.gpu, r.dps, r.yaw, r.pitch, r.scene, r.focused, r.rendered);
                rows++;
            }
            const float hz = vr::refreshHz();
            const bool missed = hz > 0 && r.dispDt > 1500.0 / hz;
            wFrames++;
            wWork.push_back(r.work); wWait.push_back(r.wait);
            if (r.rendered) { wRender.push_back(r.render); wPost.push_back(r.post); }
            wEnd.push_back(r.end);
            if (r.gpu >= 0) wGpu.push_back(r.gpu);
            if (r.tick) { wTicks++; wGap.push_back(r.gap); if (missed) wTickMissed++; }
            if (missed) wMissed++;
            if (r.dps > 0) wDps.push_back(r.dps);
            if (wT0 == 0)
                wT0 = r.t;
            if (r.t - wT0 >= 10.0)
                summary(r.t);
        }

        void begin(double entryT, double gapMs)
        {
            curSlot = &slots[n % 4];
            finalize(*curSlot);
            cur = Row();
            cur.t = entryT;
            cur.gap = gapMs;
            cur.tick = gapMs > 1.5;
        }

        void gpuBegin()
        {
            Slot &s = *curSlot;
            s.q = false;
            if (!g_dev || !g_ctx)
                return;
            if (!s.dj)
            {
                D3D11_QUERY_DESC d = {};
                d.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                g_dev->CreateQuery(&d, &s.dj);
                d.Query = D3D11_QUERY_TIMESTAMP;
                g_dev->CreateQuery(&d, &s.a);
                g_dev->CreateQuery(&d, &s.b);
            }
            if (!s.dj || !s.a || !s.b)
                return;
            g_ctx->Begin(s.dj);
            g_ctx->End(s.a);
            s.open = true;
        }

        void gpuEnd()
        {
            Slot &s = *curSlot;
            if (!s.open)
                return;
            g_ctx->End(s.b);
            g_ctx->End(s.dj);
            s.open = false;
            s.q = true;
        }

        void head(const vr::Pose &h, int64_t xt)
        {
            const float *q = h.q;
            const float fx = -2.0f * (q[0] * q[2] + q[1] * q[3]), fy = -2.0f * (q[1] * q[2] - q[0] * q[3]), fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
            cur.yaw = atan2f(-fx, -fz) * 57.29578f;
            cur.pitch = atan2f(fy, sqrtf(fx * fx + fz * fz)) * 57.29578f;
            if (xt && lastXt)
                cur.dispDt = (xt - lastXt) * 1e-6;
            if (prevOk && xt > lastXt && lastXt)
            {
                float dot = fabsf(prevQ[0] * q[0] + prevQ[1] * q[1] + prevQ[2] * q[2] + prevQ[3] * q[3]);
                if (dot > 1.0f) dot = 1.0f;
                cur.dps = 2.0 * acos((double)dot) * 57.29578 / ((xt - lastXt) * 1e-9);
            }
            if (xt != lastXt)
            {
                memcpy(prevQ, q, sizeof prevQ);
                prevOk = true;
                lastXt = xt;
            }
        }

        void commit()
        {
            gpuEnd();
            curSlot->row = cur;
            curSlot->used = true;
            n++;
        }

        void close()
        {
            for (int i = 0; i < 4; i++)
                finalize(slots[(n + i) % 4]);
            if (csv)
            {
                fclose(csv);
                csv = nullptr;
            }
            csvTried = false;
            rows = 0;
        }
    }

    // One headset frame. Returns false when no frame could be made (session not running).
    bool frame()
    {
        vr::poll();
        if (!vr::running())
            return false;
        const double tEntry = g_api->time_now();
        if (!vr::waitAndBegin())
            return false;
        const double tWaited = g_api->time_now();
        prof::begin(tEntry, g_frame_end_t > 0 ? (tEntry - g_frame_end_t) * 1000.0 : 0.0);
        prof::cur.wait = (tWaited - tEntry) * 1000.0;
        if (g_need_recentre) { vr::recenter(); g_need_recentre = false; }
        vr::View views[2];
        vr::Pose head;
        bool haveViews = vr::locateViews(views) && vr::locateHead(head);
        double now = g_api->time_now();
        double dt = g_last_frame_t > 0 ? now - g_last_frame_t : 0.011;
        g_last_frame_t = now;
        displayTime(now);
        if (haveViews)
            prof::head(head, vr::predictedDisplayTime());
        vr::Hand hd[2];
        if (vr::syncInput(hd)) { controls(hd, dt); hands(hd); }
        else { g_api->set_aim_ray(0, nullptr, nullptr); g_api->vr_set_beam(0, nullptr, 0, 0); g_api->vr_set_hand(0, nullptr); g_api->vr_set_hand(1, nullptr); }
        prof::cur.focused = vr::focused() ? 1 : 0;
        prof::cur.loc = (g_api->time_now() - tWaited) * 1000.0;
        double tRendered = 0;
        vr::ProjectionLayer proj;
        vr::QuadLayer quads[2];
        int nq = 0;
        if (haveViews && vr::shouldRender())
        {
            C12VrPose hp;
            memcpy(hp.q, head.q, sizeof hp.q);
            memcpy(hp.p, head.p, sizeof hp.p);
            g_api->vr_set_head(&hp);
            C12VrView ev[2];
            for (int e = 0; e < 2; e++)
            {
                memcpy(ev[e].eye.q, views[e].pose.q, sizeof ev[e].eye.q);
                memcpy(ev[e].eye.p, views[e].pose.p, sizeof ev[e].eye.p);
                ev[e].tan_left = tanf(views[e].fov.left);
                ev[e].tan_right = tanf(views[e].fov.right);
                ev[e].tan_up = tanf(views[e].fov.up);
                ev[e].tan_down = tanf(views[e].fov.down);
            }
            uint32_t rw = 0, rh = 0;
            vr::recommendedSize(rw, rh);
            int w = (int)(rw * g_render_scale), h = (int)(rh * g_render_scale);
            if (w < 64) w = 64;
            if (h < 64) h = 64;
            C12VrImages img;
            const double tR0 = g_api->time_now();
            prof::gpuBegin();
            int scene = g_api->vr_render(ev, g_units_per_m, w, h, &img);
            tRendered = g_api->time_now();
            prof::cur.rendered = 1;
            prof::cur.render = (tRendered - tR0) * 1000.0;
            prof::cur.scene = scene ? 1 : img.cinema ? 2 : 0;
            if (scene)
            {
                copyInto(vr::Panel::Scene, img.eye[0], img.eye_w, img.eye_h);
                copyInto(vr::Panel::SceneRight, img.eye[1], img.eye_w, img.eye_h);
                proj.on = true;
                proj.stereo = true;
                proj.views[0] = views[0];
                proj.views[1] = views[1];
                if (img.hud_black && img.hud_white)
                {
                    vr::Pose hb = head;
                    if (g_hud_lazy)
                    {
                        // follow the head's turn only once it looks more than 25 degrees away, smoothly;
                        // the HUD stays level
                        const float *hq = head.q;
                        float fx = -2.0f * (hq[0] * hq[2] + hq[1] * hq[3]), fz = -(1.0f - 2.0f * (hq[0] * hq[0] + hq[1] * hq[1]));
                        float yaw = atan2f(-fx, -fz);
                        if (!g_hud_yaw_ok) { g_hud_yaw = yaw; g_hud_yaw_ok = true; }
                        float d = yaw - g_hud_yaw;
                        while (d > 3.14159265f) d -= 6.28318531f;
                        while (d < -3.14159265f) d += 6.28318531f;
                        if (fabsf(d) > 0.44f) g_hud_yaw += (d - (d > 0 ? 0.44f : -0.44f)) * 0.15f + d * 0.02f;
                        hb.q[0] = 0.0f; hb.q[1] = sinf(g_hud_yaw * 0.5f); hb.q[2] = 0.0f; hb.q[3] = cosf(g_hud_yaw * 0.5f);
                    }
                    vr::Pose hp = ahead(hb, g_hud_dist, g_hud_lazy ? 0.25f : 0.0f);
                    // the shape of the game's picture: 16:9 in widescreen (the menu screen's picture has it)
                    float aspect = img.cinema_w > 0 && img.cinema_h > 0 ? (float)img.cinema_h / (float)img.cinema_w : 0.75f;
                    for (int e = 0; e < 2; e++)
                    {
                        bool srgb = false;
                        ID3D11Texture2D *dst = vr::panelImage(e ? vr::Panel::SceneRight : vr::Panel::Scene, (uint32_t)img.eye_w, (uint32_t)img.eye_h, srgb);
                        drawPanel(dst, srgb, img.eye_w, img.eye_h, views[e], hp, g_hud_width, g_hud_width * aspect, img.hud_black, img.hud_white);
                    }
                }
            }
            else if (img.cinema)
            {
                // menus, loading screens, films: a screen ahead in the dark, drawn into the eyes' pictures
                vr::Pose origin;
                vr::Pose cp = ahead(origin, g_cinema_dist, 0.0f);
                float ch = g_cinema_width * img.cinema_h / (float)img.cinema_w;
                bool ok = true;
                for (int e = 0; e < 2; e++)
                {
                    bool srgb = false;
                    ID3D11Texture2D *dst = blackEye(e ? vr::Panel::SceneRight : vr::Panel::Scene, w, h, srgb);
                    if (!dst) { ok = false; continue; }
                    drawPanel(dst, srgb, w, h, views[e], cp, g_cinema_width, ch, img.cinema, nullptr);
                }
                if (ok)
                {
                    proj.on = true;
                    proj.stereo = true;
                    proj.views[0] = views[0];
                    proj.views[1] = views[1];
                }
            }
        }
        const double tE0 = g_api->time_now();
        if (tRendered > 0)
            prof::cur.post = (tE0 - tRendered) * 1000.0;
        prof::gpuEnd();
        vr::endFrame(proj, quads, nq);
        g_frame_end_t = g_api->time_now();
        prof::cur.end = (g_frame_end_t - tE0) * 1000.0;
        prof::cur.work = (g_frame_end_t - tWaited) * 1000.0;
        prof::commit();
        return true;
    }

    // the runtime's frame pacing: headset frames until the game's next frame is due
    void waitUntil(double t)
    {
        if (g_frame_end_t > 0)
        {
            const double game = g_api->time_now() - g_frame_end_t;   // the game's own work since the last headset frame
            if (game < 0.2 && game > g_stat_game_max)
                g_stat_game_max = game;
        }
        for (int guard = 0; guard < 64; guard++)
        {
            if (g_api->time_now() >= t)
                return;
            bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (f8 && !g_was_f8)
                g_need_recentre = true;
            g_was_f8 = f8;
            if (!frame())
                break;
        }
        while (g_api->time_now() < t)
            Sleep(1);
    }

    void stopVr()
    {
        if (!g_on)
            return;
        g_api->set_pacer(nullptr);
        g_api->vr_set_head(nullptr);
        g_api->set_aim_ray(0, nullptr, nullptr);
        g_api->vr_set_beam(0, nullptr, 0, 0);
        g_api->vr_set_hand(0, nullptr);
        g_api->vr_set_hand(1, nullptr);
        prof::close();
        vr::stop();
        g_on = false;
        vr::log("VR off\n");
    }

    // once per displayed frame: start / stop with the switch, retry a headset that is off
    void onFrame()
    {
        if (!g_enabled) { stopVr(); vr::clearExited(); return; }
        if (vr::exited()) { stopVr(); return; }
        if (vr::started())
        {
            if (!g_on) { g_on = true; g_api->set_pacer(waitUntil); g_need_recentre = true; }
            return;
        }
        double now = g_api->time_now();
        if (now < g_retry_at)
            return;
        g_retry_at = now + 3.0;
        g_dev = (ID3D11Device *)g_api->d3d11_device();
        if (!g_dev) { vr::log("no D3D11 renderer: VR needs the hardware renderer\n"); g_retry_at = now + 1e9; return; }
        if (!g_ctx) g_dev->GetImmediateContext(&g_ctx);
        std::string why;
        if (!vr::start(g_dev, why) && !vr::pending())
            vr::log("VR not started: %s\n", why.c_str());
    }
}

extern "C" __declspec(dllexport) int c12_mod_init(const C12ModApi *api)
{
    if (!api || api->version < 5)
        return 0;
    g_api = api;
    api->mod_info("VR", "OpenXR headset: stereo 3D view, HUD panel, menus on a screen. F8 recentres.", &g_enabled);
    api->setting_float("units_per_metre", "World scale (game units per metre)", &g_units_per_m, 100.0f, 300.0f);
    api->setting_float("render_scale", "Render scale", &g_render_scale, 0.5f, 1.5f);
    api->setting_float("hud_distance", "HUD distance (m)", &g_hud_dist, 0.6f, 3.0f);
    api->setting_float("hud_width", "HUD width (m)", &g_hud_width, 0.4f, 2.5f);
    api->setting_float("cinema_distance", "Screen distance for menus (m)", &g_cinema_dist, 1.0f, 6.0f);
    api->setting_float("cinema_width", "Screen width for menus (m)", &g_cinema_width, 1.0f, 6.0f);
    api->setting_bool("snap_turn", "Snap turn (right stick; off = smooth)", &g_snap_turn);
    api->setting_float("snap_degrees", "Snap turn angle", &g_snap_deg, 15.0f, 90.0f);
    api->setting_float("smooth_speed", "Smooth turn speed (degrees/s)", &g_smooth_speed, 30.0f, 360.0f);
    api->setting_bool("aim_controller", "Aim with the right controller", &g_aim_controller);
    api->setting_bool("beam", "Laser beam from the controller", &g_beam);
    api->setting_bool("hud_lazy", "HUD follows the head lazily (off = fixed to the view)", &g_hud_lazy);
    vr::setLogSink(gameLog);
    api->on_frame(onFrame);
    std::string info;
    const bool ok = vr::probe(info);
    vr::log("vr.dll loaded (VR %s); %s: %s\n", g_enabled ? "on" : "off", ok ? "OpenXR ready" : "OpenXR not available", info.c_str());
    return 1;
}
