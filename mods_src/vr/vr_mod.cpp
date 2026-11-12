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
#include <string>

namespace
{
    const C12ModApi *g_api = nullptr;
    int g_enabled = 0;
    float g_units_per_m = 183.0f;   // the hero (about 330 units) as a 1.8 m person
    float g_render_scale = 1.0f;
    float g_hud_dist = 1.4f, g_hud_width = 1.1f;
    float g_cinema_dist = 2.5f, g_cinema_width = 2.6f;

    bool g_on;                // session started (or waiting for the headset)
    double g_retry_at;
    bool g_was_f8;
    bool g_need_recentre = true;

    ID3D11Device *g_dev;
    ID3D11DeviceContext *g_ctx;
    ID3D11VertexShader *g_vs;
    ID3D11PixelShader *g_ps_hud;
    ID3D11Buffer *g_cb;
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
float4 ps_hud(V i) : SV_Target {
    float3 b = tb.SampleLevel(smp, i.uv, 0).rgb;
    float3 w = tw.SampleLevel(smp, i.uv, 0).rgb;
    float a = saturate(1.0 - dot(w - b, float3(1, 1, 1)) / 3.0);
    // an sRGB render target encodes what it gets: hand it linear values so the bytes stay the game's
    if (srgb > 0.5) b = pow(max(b, 0), 2.2);
    return float4(b, a);
}
)";

    bool makeShaders()
    {
        if (g_ps_hud)
            return true;
        ID3DBlob *b = nullptr, *e = nullptr;
        if (FAILED(D3DCompile(kShader, strlen(kShader), "vr", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &b, &e)))
            return false;
        g_dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_vs);
        b->Release();
        if (FAILED(D3DCompile(kShader, strlen(kShader), "vr", nullptr, nullptr, "ps_hud", "ps_5_0", 0, 0, &b, &e)))
            return false;
        g_dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_ps_hud);
        b->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &g_cb);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        g_dev->CreateSamplerState(&sd, &g_smp);
        return g_vs && g_ps_hud && g_cb && g_smp;
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
        if (FAILED(g_dev->CreateRenderTargetView(dst, nullptr, &rtv)))
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

    // One headset frame. Returns false when no frame could be made (session not running).
    bool frame()
    {
        vr::poll();
        if (!vr::running())
            return false;
        if (!vr::waitAndBegin())
            return false;
        if (g_need_recentre) { vr::recenter(); g_need_recentre = false; }
        vr::View views[2];
        vr::Pose head;
        bool haveViews = vr::locateViews(views) && vr::locateHead(head);
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
            int scene = g_api->vr_render(ev, g_units_per_m, w, h, &img);
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
                    hudInto(img.hud_black, img.hud_white, img.hud_w, img.hud_h);
                    vr::QuadLayer &q = quads[nq++];
                    q.on = true;
                    q.panel = vr::Panel::Hud;
                    q.pose = ahead(head, g_hud_dist, 0.0f);
                    q.width = g_hud_width;
                    q.height = g_hud_width * 0.75f;   // the game's 4:3 picture
                    q.alpha = true;
                }
            }
            else if (img.cinema)
            {
                copyInto(vr::Panel::Cinema, img.cinema, img.cinema_w, img.cinema_h);
                vr::QuadLayer &q = quads[nq++];
                q.on = true;
                q.panel = vr::Panel::Cinema;
                vr::Pose origin;
                q.pose = ahead(origin, g_cinema_dist, 0.0f);
                q.width = g_cinema_width;
                q.height = g_cinema_width * img.cinema_h / (float)img.cinema_w;
            }
        }
        vr::endFrame(proj, quads, nq);
        return true;
    }

    // the runtime's frame pacing: headset frames until the game's next frame is due
    void waitUntil(double t)
    {
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
    if (!api || api->version < 4)
        return 0;
    g_api = api;
    api->mod_info("VR", "OpenXR headset: stereo 3D view, HUD panel, menus on a screen. F8 recentres.", &g_enabled);
    api->setting_float("units_per_metre", "World scale (game units per metre)", &g_units_per_m, 100.0f, 300.0f);
    api->setting_float("render_scale", "Render scale", &g_render_scale, 0.5f, 1.5f);
    api->setting_float("hud_distance", "HUD distance (m)", &g_hud_dist, 0.6f, 3.0f);
    api->setting_float("hud_width", "HUD width (m)", &g_hud_width, 0.4f, 2.5f);
    api->setting_float("cinema_distance", "Screen distance for menus (m)", &g_cinema_dist, 1.0f, 6.0f);
    api->setting_float("cinema_width", "Screen width for menus (m)", &g_cinema_width, 1.0f, 6.0f);
    vr::setLogSink(gameLog);
    api->on_frame(onFrame);
    std::string info;
    const bool ok = vr::probe(info);
    vr::log("vr.dll loaded (VR %s); %s: %s\n", g_enabled ? "on" : "off", ok ? "OpenXR ready" : "OpenXR not available", info.c_str());
    return 1;
}
