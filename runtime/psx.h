// Runtime-internal interfaces (C++).
#pragma once
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

// ---- logging ---------------------------------------------------------
void logf(const char* fmt, ...);
extern int g_log_level;  // 0 quiet, 1 info, 2 verbose
#define LOGV(...) do { if (g_log_level >= 2) logf(__VA_ARGS__); } while (0)

// ---- config ----------------------------------------------------------
struct Config {
    std::string cue;            // path to disc .cue
    std::string moddir = "mods";
    std::string memcard = "memcard1.mcd";
    std::string hd_textures;    // DuckStation-style replacement pack directory
    int scale = 4;              // internal resolution multiplier
    int widescreen = 1;         // 0 = 4:3, 1 = 16:9 (game hack)
    int filter = 0;             // texture filtering
    int vsync = 1;
    int fullscreen = 0;
    int win_w = 1280, win_h = 720;
    double cpu_bias = 2.0;      // sysclk cycles per instruction (lower = faster CPU)
    int audio_interp = 2;       // 0 nearest 1 linear 2 gaussian 3 cubic
    int audio_latency_ms = 40;
    int reverb = 1;
    int headless = 0;
    int max_frames = 0;         // headless: stop after N vblanks
    std::string shot_dir;       // headless screenshot dir
    int shot_every = 0;
    int dump_textures = 0;
    std::string input_script;   // scripted pad input for tests
    int hd_enabled = 1;         // use the HD texture pack
    int interp = 1;             // motion-interpolated in-between frames (smooth 50 fps)
    int present_linear = 1;     // smooth scaling to the window
    float volume = 1.0f;
    int mouse = 1;              // mouse look
    float mouse_sens = 1.0f;
    int mouse_invert_y = 0;
    int mouse_invert_x = 0;
    int cam_autocenter = 1;     // 0 = the camera keeps the direction the player chose
    int mouse_direct_cam = 1;   // third-person view: mouse turns the game's camera directly (no stick emulation)
    int mouse_direct_aim = 1;   // first-person view: mouse writes the game's view angles directly
    int mouse_aim_invert = 0;   // invert vertical aim in first-person view
    int pause_in_menu = 1;
    int binds[16][2] = {};      // virtual-key codes per PSX button (bit order of the pad word)
};
extern Config g_cfg;

// ---- core / scheduler ------------------------------------------------
extern Ctx g_ctx;
typedef void (*EventFn)(int id, s64 late);
enum { EV_GPU_LINE, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_CDROM, EV_CDROM2, EV_CD_SECOND, EV_CD_DELIVER, EV_SIO, EV_SPU, EV_DMA_GPU, EV_DMA_CD, EV_DMA_SPU, EV_DMA_OTC, EV_DMA_MDEC, EV_COUNT };
s64 sched_now();                         // current time in sysclk ticks
void sched_set(int id, s64 delay, EventFn fn);  // relative
void sched_cancel(int id);
bool sched_pending(int id);
void sched_force_check();                // re-evaluate IRQs soon

#define SYSCLK 33868800
enum { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6, IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_SPU = 9, IRQ_PIO = 10 };
void irq_raise(int n);
extern u32 g_istat, g_imask;

u32 io_read(u32 addr, int size);
void io_write(u32 addr, u32 v, int size);

void guest_call(Ctx* c, u32 addr);       // call guest function from host and return
void interp_run(Ctx* c, u32 pc, u32 stop_pc, u32 stop_sp);
RecompFn lookup_fn(u32 addr);
bool hook_set(u32 addr, HookFn fn);
void idle_until_event(Ctx* c);
void core_init();
bool load_exe(const char* path);
bool load_exe_mem(const u8* data, size_t len);
void psx_fatal(const char* fmt, ...);
extern volatile int g_quit;

// ---- bios ------------------------------------------------------------
void bios_init();
bool bios_call(Ctx* c, u32 addr);        // A0/B0/C0 dispatch; true if handled
void bios_exception_irq(Ctx* c);
void bios_vblank_hook();
void bios_deliver_event(u32 cls, u32 spec);

// ---- timers ----------------------------------------------------------
u32 timer_read(u32 addr);
void timer_write(u32 addr, u32 v);
void timers_hblank();
void timers_vblank(bool in_vblank);
void timers_init();

// ---- dma -------------------------------------------------------------
u32 dma_read(u32 addr);
void dma_write(u32 addr, u32 v);

// ---- gpu -------------------------------------------------------------
void gpu_init();
u32 gpu_read_data();
u32 gpu_read_stat();
void gpu_write_gp0(u32 v);
void gpu_write_gp1(u32 v);
void gpu_dma(u32 madr, u32 bcr, u32 chcr);
void gpu_vblank();                       // present
extern int g_frame;

// ---- spu -------------------------------------------------------------
void spu_init();
u32 spu_read(u32 addr);
void spu_write(u32 addr, u32 v);
void spu_dma(u32 madr, u32 bcr, u32 chcr);
void spu_run(s64 ticks);                 // advance to current time
void spu_cd_audio(const s16* lr, int frames, int rate);   // XA / CDDA feed

// ---- cdrom -----------------------------------------------------------
bool cdrom_init(const std::string& cue);
u32 cdrom_read(u32 reg);
void cdrom_write(u32 reg, u32 v);
void cdrom_dma(u32 madr, u32 bcr, u32 chcr);
bool cdrom_find_file(const char* path, u32* lba, u32* size);
bool cdrom_add_override(const char* disc_path, u32 offset, const u8* data, u32 len);
bool cdrom_read_file(const char* path, std::vector<u8>& out);

// ---- sio / pads / memcard -------------------------------------------
void sio_init();
u32 sio_read(u32 addr, int size);
void sio_write(u32 addr, u32 v, int size);
u16 pad_buttons(int port);               // active-low PSX button word, polled now
void pad_analog(int port, u8 out[4]);
bool bios_pad_poll(int port, u8* buf, int len);

// ---- mdec ------------------------------------------------------------
u32 mdec_read(u32 addr);
void mdec_write(u32 addr, u32 v);
void mdec_dma_in(u32 madr, u32 bcr, u32 chcr);
void mdec_dma_out(u32 madr, u32 bcr, u32 chcr);

// ---- platform (window / audio / input) ------------------------------
bool plat_init();
void plat_pump();
void plat_shutdown();
void plat_audio_push(const float* lr, int frames);
int plat_audio_queued_ms();
double plat_time();
void plat_sleep_until(double t);
void plat_input_poll();

// ---- mods ------------------------------------------------------------
void mods_init();
