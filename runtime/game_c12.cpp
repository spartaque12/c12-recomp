// Game-specific glue for C-12: Final Resistance (SCES-03364).
//
// Fibres: the engine runs its scripts/entities as stack-copying coroutines with a hand-written
// context switch (start 80035970, resume 80035a0c, yield 80035b64, exit-call 80035ae0). The guest
// side effects (stack copy, register save areas, bookkeeping) are kept exactly as the original;
// only the control transfer is replaced by native Win32 fibers, one per guest fibre.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "psx.h"
#include <unordered_map>

#define G_FIBRE_BASE 0x800cb670u   // sp of the frame that started/resumed the running fibre
#define G_FIBRE_CUR 0x800ef4c4u    // running fibre struct
#define FN_FIBRE_LOCK 0x800358e8u
#define FN_FIBRE_EPILOGUE 0x80035b08u

enum { FS_RUNNING, FS_YIELDED, FS_FINISHED, FS_EXITCALL };
struct HostFibre {
    void* handle = nullptr;
    HostFibre* resumer = nullptr;
    u32 guest = 0, entry = 0, exit_fn = 0, exit_arg = 0;
    int state = FS_RUNNING;
    u32 cs[4096];
    int cs_n = 0;
};
static HostFibre g_main_fibre;
static HostFibre* g_cur_fibre = &g_main_fibre;
static std::unordered_map<u32, HostFibre*> g_fibres;
extern "C" { extern u32* g_cs; extern int g_cs_n; }

static inline u32 rd(u32 a) { return mem_r32(&g_ctx, a); }
static inline void wr(u32 a, u32 v) { mem_w32(&g_ctx, a, v); }

static void switch_to(HostFibre* t) {
    g_cur_fibre->cs_n = g_cs_n;
    g_cur_fibre = t;
    g_cs = t->cs;
    g_cs_n = t->cs_n;
    SwitchToFiber(t->handle);
}

static void CALLBACK fibre_proc(void* p) {
    HostFibre* hf = (HostFibre*)p;
    Ctx* c = &g_ctx;
    c->r[31] = 0x80035a04;
    call_indirect(c, hf->entry);
    hf->state = FS_FINISHED;
    switch_to(hf->resumer);
    psx_fatal("finished fibre %08x resumed", hf->guest);
}

static void destroy(HostFibre* hf) {
    auto it = g_fibres.find(hf->guest);
    if (it != g_fibres.end() && it->second == hf) g_fibres.erase(it);
    DeleteFiber(hf->handle);
    delete hf;
}

static void run_epilogue(Ctx* c) {
    RecompFn ep = lookup_fn(FN_FIBRE_EPILOGUE);
    if (!ep) psx_fatal("fibre epilogue not recompiled (add 80035b08 to seeds)");
    ep(c);
}

// back on the resumer's context after the fibre yielded / finished / exit-called
static void CALLBACK fibre_proc(void* p);

static void after_switch(Ctx* c, HostFibre* hf) {
    for (;;) {
        switch (hf->state) {
        case FS_YIELDED: return;  // yield already restored the resumer's registers
        case FS_EXITCALL: {
            // 80035ae0(func, arg): the running fibre throws its stack away and continues as
            // func(arg) from the stack base. It is still the same fibre and may yield again, so
            // it gets a fresh native stack and keeps running.
            void* old = hf->handle;
            { static int n; if (n++ < 5) logf("fibre %08x restarted as %08x(%08x)\n", hf->guest, hf->exit_fn, hf->exit_arg); }
            hf->entry = hf->exit_fn;
            hf->state = FS_RUNNING;
            hf->cs_n = 0;
            hf->handle = CreateFiberEx(64 * 1024, 1024 * 1024, 0, fibre_proc, hf);
            if (!hf->handle) psx_fatal("CreateFiber failed");
            DeleteFiber(old);
            c->r[4] = hf->exit_arg;
            switch_to(hf);
            continue;   // back here when it yields or finishes
        }
        case FS_FINISHED:
            run_epilogue(c);
            destroy(hf);
            return;
        default: psx_fatal("fibre %08x switched back in state %d", hf->guest, hf->state);
        }
    }
}

static void push_frame(Ctx* c) {
    u32 sp = c->r[29] - 64;
    u32 t1 = rd(G_FIBRE_BASE), t3 = rd(G_FIBRE_CUR);
    wr(G_FIBRE_BASE, sp);
    wr(G_FIBRE_CUR, c->r[4]);
    wr(sp + 20, t1);
    wr(sp + 24, t3);
    wr(sp + 16, c->r[31]);
    for (int i = 0; i < 8; i++) wr(sp + 28 + i * 4, c->r[16 + i]);
    wr(sp + 60, c->r[30]);
    c->r[29] = sp;
}

static void hook_fibre_start(Ctx* c, RecompFn) {
    u32 a1 = c->r[5], a2 = c->r[6];
    push_frame(c);
    u32 sp = c->r[29];
    wr(sp + 8, a1);
    wr(sp + 12, a2);
    c->r[4] = 0;
    c->r[5] = 0;
    c->r[31] = 0x800359d4;
    call_indirect(c, FN_FIBRE_LOCK);
    u32 f = rd(G_FIBRE_CUR);
    u32 flags = mem_r16(c, f);
    mem_w16(c, f, (flags | 1) & ~0xCu);
    c->r[4] = rd(sp + 12);

    auto old = g_fibres.find(f);
    if (old != g_fibres.end()) {  // struct reused while its previous incarnation was parked
        DeleteFiber(old->second->handle);
        delete old->second;
        g_fibres.erase(old);
    }
    HostFibre* hf = new HostFibre;
    hf->guest = f;
    hf->entry = rd(sp + 8);
    hf->resumer = g_cur_fibre;
    hf->handle = CreateFiberEx(64 * 1024, 1024 * 1024, 0, fibre_proc, hf);
    if (!hf->handle) psx_fatal("CreateFiber failed");
    g_fibres[f] = hf;
    switch_to(hf);
    after_switch(c, hf);
}

static void hook_fibre_resume(Ctx* c, RecompFn) {
    u32 f = c->r[4];
    u32 flags = mem_r16(c, f);
    if ((flags & 0x10) || !(flags & 1)) return;
    auto it = g_fibres.find(f);
    if (it == g_fibres.end()) {
        logf("fibre %08x resumed without a native context (flags %04x) - ignored\n", f, flags);
        return;
    }
    HostFibre* hf = it->second;
    push_frame(c);
    u32 sp = c->r[29] + 16;
    u32 src = rd(f + 0x30);
    s32 n = (s16)mem_r16(c, f + 0x2e);
    do {
        sp -= 4;
        wr(sp, rd(src));
        src += 4;
        n -= 4;
    } while (n != 0);
    sp -= 16;
    for (int i = 0; i < 8; i++) c->r[16 + i] = rd(f + 8 + i * 4);
    c->r[30] = rd(f + 0x28);
    c->r[25] = rd(f + 4);
    c->r[31] = 0x80035ad0;
    c->r[29] = sp;
    hf->resumer = g_cur_fibre;
    hf->state = FS_RUNNING;
    switch_to(hf);
    after_switch(c, hf);
}

static void hook_fibre_yield(Ctx* c, RecompFn orig) {
    HostFibre* hf = g_cur_fibre;
    if (hf == &g_main_fibre) psx_fatal("fibre yield outside a fibre");
    orig(c);  // saves registers + stack into the fibre, restores the resumer's registers
    hf->state = FS_YIELDED;
    switch_to(hf->resumer);
    // resumed: registers and guest stack were re-established by hook_fibre_resume
}

static void hook_fibre_exitcall(Ctx* c, RecompFn) {
    HostFibre* hf = g_cur_fibre;
    if (hf == &g_main_fibre) psx_fatal("fibre exit-call outside a fibre");
    hf->exit_fn = c->r[4];
    hf->exit_arg = c->r[5];
    hf->state = FS_EXITCALL;
    c->r[29] = rd(G_FIBRE_BASE);
    switch_to(hf->resumer);
    psx_fatal("dead fibre resumed");
}

// ---- frame pacing ----------------------------------------------------------------------------
// libetc's VSync wait is a calibrated busy loop that times out on a faster CPU. Replace it by
// skipping emulated time straight to the next event: no timeouts at any CPU speed, no host CPU burn.
#define G_VCOUNT 0x800ee388u
void idle_until_event(Ctx* c) {
    if (c->cyc < c->evt) c->cyc = c->evt;
    psx_events(c);
}
static void hook_vsync_wait(Ctx* c, RecompFn) {
    s32 target = (s32)c->r[4];
    int guard = 0;
    while ((s32)rd(G_VCOUNT) < target) {
        idle_until_event(c);
        if (++guard > 2000000) { logf("VSync wait stuck (target %d, count %d)\n", target, rd(G_VCOUNT)); break; }
    }
}
// The gameplay loop waits VSync(2) => 25 fps. Its logic is scaled by elapsed vblanks, so waiting a
// single vblank doubles the frame rate without changing game speed.
static void hook_vsync(Ctx* c, RecompFn orig) {
    if (g_cfg.fps60 && c->r[31] == 0x80038030 && c->r[4] == 2) c->r[4] = 0;  // VSync(0): next vblank
    orig(c);
}

// ---- overlays ----------------------------------------------------------------------------------
// 800612b8(image, relocs) relocates a freshly loaded RELOCS\*.LVB image in place. Tell the core
// (before the relocation, so it can checksum the pristine image), then let the game relocate.
void overlay_loaded(u32 base, u32 reloc_ptr);
static void hook_relocate(Ctx* c, RecompFn orig) {
    overlay_loaded(c->r[4], c->r[5]);
    orig(c);
}

// ---- widescreen visibility ---------------------------------------------------------------------
// 80065db8 builds the four corner rays of the view frustum (camera space, s16 x/y/z + pad at
// 800f8b30) from a fixed 160x120 half-screen; the world grid is walked inside their footprint.
// For the 16:9 view the rays are spread horizontally so the extra screen area gets its geometry.
extern float g_ws_scale;
int g_ws_frustum = 1;
static void hook_frustum_corners(Ctx* c, RecompFn orig) {
    orig(c);
    if (!g_ws_frustum || g_ws_scale == 1.0f) return;
    float k = 1.0f / g_ws_scale * 1.06f;   // a little margin: cells are tested by their corners
    for (u32 a = 0x800f8b30; a < 0x800f8b30 + 32; a += 8) {
        int x = (s16)mem_r16(c, a);
        x = (int)(x * k);
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        mem_w16(c, a, (u32)(u16)(s16)x);
    }
}

// ---- third-person camera ----------------------------------------------------------------------
// 80015b50(cam) turns the right stick into this frame's target angles: pitch at cam+0x2c, yaw at
// cam+0x2e (12-bit, 4096 per turn), current angles at cam+0x20/+0x22, flags at cam+0x18
// (0x800 / 0x1000 = pitch / yaw target given). Raw mouse: the same targets, set from mouse counts.
// When the player gives no target and the hero moves, the game swings the camera back behind him;
// holding the targets every frame instead breaks the camera's position follow (tried), so the
// auto-centre is left alone.
extern long g_cam_mx, g_cam_my;
int g_cam_log;
static void hook_cam_stick(Ctx* c, RecompFn orig) {
    u32 cam = c->r[4];
    orig(c);
    static float fx, fy;
    float scale = 0.55f * g_cfg.mouse_sens;
    fx += (float)g_cam_mx * scale;
    fy += (float)g_cam_my * scale;
    g_cam_mx = g_cam_my = 0;
    int ix = (int)fx, iy = (int)fy;
    fx -= ix; fy -= iy;
    if (g_cam_log) logf("cam %08x mode %d pitch %d yaw %d  target %d %d flags %08x\n", cam, mem_r8(c, cam + 0x77), (s16)mem_r16(c, cam + 0x20), (s16)mem_r16(c, cam + 0x22), (s16)mem_r16(c, cam + 0x2c), (s16)mem_r16(c, cam + 0x2e), mem_r32(c, cam + 0x18));
    if (!g_cfg.mouse_direct_cam) ix = iy = 0;
    u32 fl = mem_r32(c, cam + 0x18);
    if (!ix && !iy) return;
    if (iy) {
        int p = (((s16)mem_r16(c, cam + 0x20) + 0x400) & 0xfff) - iy;
        if (p < 0x20) p = 0x20;
        if (p > 0x7e0) p = 0x7e0;
        mem_w16(c, cam + 0x2c, (u32)((p - 0x400) & 0xfff));
        fl |= 0x800;
    }
    if (ix) {
        mem_w16(c, cam + 0x2e, (u32)(((s16)mem_r16(c, cam + 0x22) + ix) & 0xfff));
        fl |= 0x1000;
    }
    mem_w32(c, cam + 0x18, fl & ~4u);
}

void game_init() {
    hook_set(0x80015b50, hook_cam_stick);
    hook_set(0x80065db8, hook_frustum_corners);
    hook_set(0x800612b8, hook_relocate);
    hook_set(0x800a1128, hook_vsync_wait);
    hook_set(0x800a0fb0, hook_vsync);
    g_main_fibre.handle = ConvertThreadToFiber(nullptr);
    g_cs = g_main_fibre.cs;
    hook_set(0x80035970, hook_fibre_start);
    hook_set(0x80035a0c, hook_fibre_resume);
    hook_set(0x80035b64, hook_fibre_yield);
    hook_set(0x80035ae0, hook_fibre_exitcall);
}
