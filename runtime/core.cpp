// CPU-side runtime: memory map, scheduler, IRQ delivery, dispatch, interpreter fallback.
#include "psx.h"
#include <stdarg.h>
#include <set>

extern "C" {
u8 g_ram[RAM_SIZE + 16];
static u32 g_cs_boot[4096];
u32* g_cs = g_cs_boot;
int g_cs_n;
PgxpVal* g_pgxp_mem;
PgxpVal g_pgxp_scratch[0x100];
int g_pgxp_on = 1;
extern const RecompFunc main_funcs[];
extern const unsigned main_func_count;
extern HookFn main_hooks[];
}

Ctx g_ctx;
Config g_cfg;
int g_log_level = 1;
volatile int g_quit = 0;
u32 g_istat, g_imask;
static u8 g_scratch[0x400 + 16];
static u32 g_memctl[16];
static u32 g_ramsize = 0xB88;
static FILE* g_logfile;

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (!g_logfile) g_logfile = fopen("c12.log", "w");
    if (g_logfile) { fputs(buf, g_logfile); fflush(g_logfile); }
}

void psx_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Ctx* c = &g_ctx;
    logf("FATAL: %s\n pc=%08x ra=%08x sp=%08x cyc=%lld frame=%d\n", buf, c->pc, c->r[31], c->r[29], (long long)c->cyc, g_frame);
    for (int i = 0; i < 32; i += 4)
        logf("  r%02d %08x %08x %08x %08x\n", i, c->r[i], c->r[i + 1], c->r[i + 2], c->r[i + 3]);
    logf(" guest call stack (innermost first):");
    for (int i = g_cs_n - 1, k = 0; i >= 0 && k < 24; i--, k++) logf(" %08x", g_cs[i & 4095]);
    logf("\n");
    exit(3);
}

// ---------------------------------------------------------------------
// scheduler
static s64 g_tick_base, g_cyc_base;
static s64 g_bias_q8 = 512;
static s64 g_ev_time[EV_COUNT];
static EventFn g_ev_fn[EV_COUNT];
static bool g_in_events;
#define NEVER 0x7FFFFFFFFFFFFFFFll

s64 sched_now() { return g_tick_base + (((g_ctx.cyc - g_cyc_base) * g_bias_q8) >> 8); }

static void sched_update() {
    s64 next = NEVER;
    for (int i = 0; i < EV_COUNT; i++)
        if (g_ev_time[i] < next) next = g_ev_time[i];
    s64 now = sched_now();
    s64 d = next == NEVER ? 100000 : next - now;
    if (d < 0) d = 0;
    if (d > 100000) d = 100000;
    s64 e = g_ctx.cyc + ((d << 8) + g_bias_q8 - 1) / g_bias_q8;
    g_ctx.evt = e;
}

void core_set_bias(double bias) {
    s64 q = (s64)(bias * 256.0 + 0.5);
    if (q < 16) q = 16;
    if (q == g_bias_q8) return;
    // rebase so already elapsed time is not rescaled
    g_tick_base = sched_now();
    g_cyc_base = g_ctx.cyc;
    g_bias_q8 = q;
    g_ctx.evt = g_ctx.cyc;
}

void sched_set(int id, s64 delay, EventFn fn) {
    g_ev_time[id] = sched_now() + delay;
    g_ev_fn[id] = fn;
    if (!g_in_events) sched_update();
}
void sched_cancel(int id) { g_ev_time[id] = NEVER; }
bool sched_pending(int id) { return g_ev_time[id] != NEVER; }
void sched_force_check() { if (!g_in_events) g_ctx.evt = g_ctx.cyc; }

void irq_raise(int n) {
    g_istat |= 1u << n;
    sched_force_check();
}

static int g_in_exception;

u32 g_watch_addr;
static u32 g_watch_val;
void watch_check(const char* where) {
    if (!g_watch_addr) return;
    u32 v;
    memcpy(&v, g_ram + (g_watch_addr & (RAM_SIZE - 4)), 4);
    if (v == g_watch_val) return;
    logf("WATCH %08x: %08x -> %08x at %s frame %d ra=%08x stack:", g_watch_addr, g_watch_val, v, where, g_frame, g_ctx.r[31]);
    for (int i = g_cs_n - 1, k = 0; i >= 0 && k < 16; i--, k++) logf(" %08x", g_cs[i & 4095]);
    logf(" |\n");
    g_watch_val = v;
}

void psx_events(Ctx* c) {
    watch_check("events");
    if (g_in_events) return;
    g_in_events = true;
    // rebase to keep the fixed-point product small
    s64 now = sched_now();
    g_tick_base = now;
    g_cyc_base = c->cyc;
    for (;;) {
        int best = -1;
        s64 bt = now;
        for (int i = 0; i < EV_COUNT; i++)
            if (g_ev_time[i] <= bt) { bt = g_ev_time[i]; best = i; }
        if (best < 0) break;
        g_ev_time[best] = NEVER;
        g_ev_fn[best](best, now - bt);
    }
    g_in_events = false;
    sched_update();
    u32 sr = c->cop0[12];
    if ((g_istat & g_imask) && (sr & 1) && (sr & 0x400) && !g_in_exception) {
        g_in_exception++;
        bios_exception_irq(c);
        g_in_exception--;
        sched_update();
    }
}

// ---------------------------------------------------------------------
// memory slow paths
static u8 g_bios_rom[0x80000];

u32 mem_r32_slow(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p - 0x1F800000u < 0x400) { u32 v; memcpy(&v, g_scratch + (p & 0x3FF), 4); return v; }
    if (p - 0x1F801000u < 0x2000) return io_read(p, 4);
    if (p < 0x800000) { u32 v; memcpy(&v, g_ram + (p & (RAM_SIZE - 1)), 4); return v; }
    if (p - 0x1FC00000u < 0x80000) { u32 v; memcpy(&v, g_bios_rom + (p & 0x7FFFC), 4); return v; }
    if (a == 0xFFFE0130) return 0x1E988;
    LOGV("r32 unmapped %08x pc~%08x\n", a, c->r[31]);
    return 0xFFFFFFFF;
}
u32 mem_r16_slow(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p - 0x1F800000u < 0x400) { u16 v; memcpy(&v, g_scratch + (p & 0x3FF), 2); return v; }
    if (p - 0x1F801000u < 0x2000) return io_read(p, 2) & 0xFFFF;
    if (p < 0x800000) { u16 v; memcpy(&v, g_ram + (p & (RAM_SIZE - 1)), 2); return v; }
    if (p - 0x1FC00000u < 0x80000) { u16 v; memcpy(&v, g_bios_rom + (p & 0x7FFFE), 2); return v; }
    LOGV("r16 unmapped %08x\n", a);
    return 0xFFFF;
}
u32 mem_r8_slow(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p - 0x1F800000u < 0x400) return g_scratch[p & 0x3FF];
    if (p - 0x1F801000u < 0x2000) return io_read(p, 1) & 0xFF;
    if (p < 0x800000) return g_ram[p & (RAM_SIZE - 1)];
    if (p - 0x1FC00000u < 0x80000) return g_bios_rom[p & 0x7FFFF];
    LOGV("r8 unmapped %08x\n", a);
    return 0xFF;
}
void mem_w32_slow(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    if (p - 0x1F800000u < 0x400) { memcpy(g_scratch + (p & 0x3FF), &v, 4); return; }
    if (p - 0x1F801000u < 0x2000) { io_write(p, v, 4); return; }
    if (p < 0x800000) { memcpy(g_ram + (p & (RAM_SIZE - 1)), &v, 4); return; }
    if (a == 0xFFFE0130) return;
    LOGV("w32 unmapped %08x = %08x\n", a, v);
}
void mem_w16_slow(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    u16 t = (u16)v;
    if (p - 0x1F800000u < 0x400) { memcpy(g_scratch + (p & 0x3FF), &t, 2); return; }
    if (p - 0x1F801000u < 0x2000) { io_write(p, v & 0xFFFF, 2); return; }
    if (p < 0x800000) { memcpy(g_ram + (p & (RAM_SIZE - 1)), &t, 2); return; }
    LOGV("w16 unmapped %08x = %04x\n", a, v);
}
void mem_w8_slow(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    if (p - 0x1F800000u < 0x400) { g_scratch[p & 0x3FF] = (u8)v; return; }
    if (p - 0x1F801000u < 0x2000) { io_write(p, v & 0xFF, 1); return; }
    if (p < 0x800000) { g_ram[p & (RAM_SIZE - 1)] = (u8)v; return; }
    LOGV("w8 unmapped %08x = %02x\n", a, v);
}

// ---------------------------------------------------------------------
// I/O dispatch
u32 io_read(u32 p, int size) {
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) return spu_read(p) | (spu_read(p + 2) << 16);
        return spu_read(p & ~1u);
    }
    if (p >= 0x1F801800 && p < 0x1F801804) return cdrom_read(p & 3);
    if (p >= 0x1F801040 && p < 0x1F801060) return sio_read(p, size);
    u32 v = 0;
    u32 a4 = p & ~3u;
    if (a4 == 0x1F801070) v = g_istat;
    else if (a4 == 0x1F801074) v = g_imask;
    else if (a4 >= 0x1F801080 && a4 < 0x1F801100) v = dma_read(a4);
    else if (a4 >= 0x1F801100 && a4 < 0x1F801130) v = timer_read(a4);
    else if (a4 == 0x1F801810) v = gpu_read_data();
    else if (a4 == 0x1F801814) v = gpu_read_stat();
    else if (a4 == 0x1F801820 || a4 == 0x1F801824) v = mdec_read(a4);
    else if (a4 < 0x1F801040) v = g_memctl[(a4 - 0x1F801000) >> 2 & 15];
    else if (a4 == 0x1F801060) v = g_ramsize;
    else { LOGV("io r%d %08x\n", size, p); v = 0; }
    return v >> ((p & 3) * 8);
}

void io_write(u32 p, u32 v, int size) {
    if (p >= 0x1F801C00 && p < 0x1F802000) {
        if (size == 4) { spu_write(p, v & 0xFFFF); spu_write(p + 2, v >> 16); }
        else if (size == 2) spu_write(p, v);
        else {   // byte write: only that half of the 16-bit register changes
            u32 old = spu_read(p & ~1u);
            spu_write(p & ~1u, (p & 1) ? ((old & 0x00FF) | ((v & 0xFF) << 8)) : ((old & 0xFF00) | (v & 0xFF)));
        }
        return;
    }
    if (p >= 0x1F801800 && p < 0x1F801804) { cdrom_write(p & 3, v & 0xFF); return; }
    if (p >= 0x1F801040 && p < 0x1F801060) { sio_write(p, v, size); return; }
    u32 a4 = p & ~3u;
    if (a4 == 0x1F801070) { g_istat &= (v << ((p & 3) * 8)) | ~((size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << ((p & 3) * 8)); sched_force_check(); return; }
    if (a4 == 0x1F801074) {
        if (size == 4) g_imask = v; else if (size == 2) g_imask = (g_imask & ~(0xFFFFu << ((p & 2) * 8))) | (v << ((p & 2) * 8));
        else g_imask = (g_imask & ~(0xFFu << ((p & 3) * 8))) | (v << ((p & 3) * 8));
        g_imask &= 0x7FF;
        LOGV("I_MASK=%03x (stat %03x) sr=%08x\n", g_imask, g_istat, g_ctx.cop0[12]);
        sched_force_check();
        return;
    }
    if (a4 >= 0x1F801080 && a4 < 0x1F801100) {
        if (size != 4) {  // partial write: merge
            u32 old = dma_read(a4), sh = (p & 3) * 8, m = (size == 2 ? 0xFFFFu : 0xFFu) << sh;
            v = (old & ~m) | ((v << sh) & m);
        }
        dma_write(a4, v);
        return;
    }
    if (a4 >= 0x1F801100 && a4 < 0x1F801130) { timer_write(a4, v); return; }
    if (a4 == 0x1F801810) { gpu_write_gp0(v); return; }
    if (a4 == 0x1F801814) { gpu_write_gp1(v); return; }
    if (a4 == 0x1F801820 || a4 == 0x1F801824) { mdec_write(a4, v); return; }
    if (a4 < 0x1F801040) { g_memctl[(a4 - 0x1F801000) >> 2 & 15] = v; return; }
    if (a4 == 0x1F801060) { g_ramsize = v; return; }
    if (p >= 0x1F802000) return;  // expansion 2 / POST
    LOGV("io w%d %08x = %08x\n", size, p, v);
}

// ---------------------------------------------------------------------
// COP0 / exceptions
u32 cop0_read(Ctx* c, int reg) {
    if (reg == 13) return (c->cop0[13] & ~0x400u) | ((g_istat & g_imask) ? 0x400 : 0);
    if (reg == 15) return 2;
    return c->cop0[reg];
}
void cop0_write(Ctx* c, int reg, u32 v) {
    if (reg == 13) { c->cop0[13] = (c->cop0[13] & ~0x300u) | (v & 0x300); return; }
    c->cop0[reg] = v;
    if (reg == 12) sched_force_check();
}
void cop0_rfe(Ctx* c) {
    u32 sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xF);
    sched_force_check();
}
void psx_syscall(Ctx* c, u32 pc) {
    u32 sr = c->cop0[12];
    switch (c->r[4]) {
    case 0: break;
    case 1:  // EnterCriticalSection
        c->r[2] = (sr & 0x401) == 0x401;
        c->cop0[12] = sr & ~0x401u;
        break;
    case 2:  // ExitCriticalSection
        c->cop0[12] = sr | 0x401;
        sched_force_check();
        break;
    default:
        logf("syscall(%x) at %08x\n", c->r[4], pc);
        break;
    }
}
void psx_break(Ctx* c, u32 pc) { logf("break at %08x (ra=%08x)\n", pc, c->r[31]); }
void psx_invalid(Ctx* c, u32 pc, u32 w) {
    c->pc = pc;
    psx_fatal("invalid instruction %08x at %08x", w, pc);
}

// ---------------------------------------------------------------------
// dispatch
#define MAIN_BASE 0x80010000u
#define MAIN_WORDS (0xE0000u / 4)
static RecompFn g_fn_main[MAIN_WORDS];
static std::set<u32> g_interp_entries;

// ---- relocatable overlays ------------------------------------------------------------------
extern "C" {
extern const OverlayDesc g_overlay_descs[];
extern const unsigned g_overlay_desc_count;
}
struct ActiveOverlay {
    const OverlayDesc* d;
    u32 base;
    std::vector<RecompFn> tab;   // by word offset
};
static std::vector<ActiveOverlay> g_active;
int g_overlays_native = 1;

// Called with the image base and its relocation table just before the game relocates it.
void overlay_loaded(u32 base, u32 reloc_ptr) {
    u32 pb = PHYS(base), size = PHYS(reloc_ptr) - pb;
    // whatever lived in this address range is gone
    for (size_t i = 0; i < g_active.size();)
        if (g_active[i].base < base + size && base < g_active[i].base + g_active[i].d->size) g_active.erase(g_active.begin() + i);
        else i++;
    if (!g_overlays_native || pb + size > RAM_SIZE) return;
    for (unsigned k = 0; k < g_overlay_desc_count; k++) {
        const OverlayDesc& d = g_overlay_descs[k];
        if (d.size != size) continue;
        u32 sum = 0;
        for (u32 o = 0; o + 4 <= size; o += 4) { u32 w; memcpy(&w, g_ram + pb + o, 4); sum += w; }
        if (sum != d.checksum) { logf("overlay %s: image differs from the recompiled one (modded?) - interpreting it\n", d.name); return; }
        for (size_t i = 0; i < g_active.size();)   // same file loaded elsewhere before
            if (g_active[i].d == &d) g_active.erase(g_active.begin() + i); else i++;
        ActiveOverlay a;
        a.d = &d;
        a.base = base | 0x80000000u;
        a.tab.assign(size / 4 + 1, nullptr);
        for (unsigned i = 0; i < d.count; i++) a.tab[d.funcs[i].addr >> 2] = d.funcs[i].fn;
        *d.base = a.base;
        g_active.push_back(std::move(a));
        logf("overlay %s: %u native functions at %08x\n", d.name, d.count, base);
        return;
    }
    logf("overlay at %08x (size %x) is not a known image - interpreting it\n", base, size);
}

RecompFn lookup_fn(u32 addr) {
    u32 a = addr | 0x80000000u;
    u32 o = a - MAIN_BASE;
    if (o < 0xE0000u) return (o & 3) ? nullptr : g_fn_main[o >> 2];
    for (const ActiveOverlay& ov : g_active) {
        u32 r = a - ov.base;
        if (r < ov.d->size) return (r & 3) ? nullptr : ov.tab[r >> 2];
    }
    return nullptr;
}

bool hook_set(u32 addr, HookFn fn) {
    // main_funcs is sorted by address
    int lo = 0, hi = (int)main_func_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (main_funcs[mid].addr == addr) { main_hooks[mid] = fn; return true; }
        if (main_funcs[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    logf("hook_set: %08x is not a recompiled function\n", addr);
    return false;
}

static void note_interp(u32 addr) {
    if (g_interp_entries.insert(addr).second) {
        LOGV("interp entry %08x\n", addr);
        FILE* f = fopen("interp_entries.txt", "a");
        if (f) {
            const ActiveOverlay* in = nullptr;
            for (const ActiveOverlay& ov : g_active)
                if ((addr | 0x80000000u) - ov.base < ov.d->size) in = &ov;
            if (in) fprintf(f, "%s %x\n", in->d->name, (addr | 0x80000000u) - in->base);
            else fprintf(f, "%08x\n", addr);
            fclose(f);
        }
    }
}

void call_indirect(Ctx* c, u32 addr) {
    RecompFn fn = lookup_fn(addr);
    if (fn) { fn(c); return; }
    u32 p = PHYS(addr);
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) { bios_call(c, p); return; }
    if (p >= RAM_SIZE || (addr & 3)) psx_fatal("call to bad address %08x", addr);
    note_interp(addr);
    interp_run(c, addr, 0, 0);
}

void jump_out(Ctx* c) { call_indirect(c, c->pc); }

void guest_call(Ctx* c, u32 addr) {
    c->r[31] = 0x1FC0DEAD;  // never jumped to: `jr ra` just returns to the host
    call_indirect(c, addr);
}

// ---------------------------------------------------------------------
// interpreter fallback (same semantics as generated code: jal = host call, jr ra = host return)
static inline void exec_simple(Ctx* c, u32 w, u32 pc) {
    u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sh = (w >> 6) & 31, fn = w & 63;
    u32 imm = w & 0xFFFF;
    u32 simm = (u32)(s32)(s16)imm;
    u32* r = c->r;
    switch (op) {
    case 0:
        switch (fn) {
        case 0: r[rd] = r[rt] << sh; break;
        case 2: r[rd] = r[rt] >> sh; break;
        case 3: r[rd] = (u32)((s32)r[rt] >> sh); break;
        case 4: r[rd] = r[rt] << (r[rs] & 31); break;
        case 6: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 7: r[rd] = (u32)((s32)r[rt] >> (r[rs] & 31)); break;
        case 12: psx_syscall(c, pc); break;
        case 13: psx_break(c, pc); break;
        case 16: r[rd] = c->hi; break;
        case 17: c->hi = r[rs]; break;
        case 18: r[rd] = c->lo; break;
        case 19: c->lo = r[rs]; break;
        case 24: MULT(r[rs], r[rt]); break;
        case 25: MULTU(r[rs], r[rt]); break;
        case 26: op_div(c, r[rs], r[rt]); break;
        case 27: op_divu(c, r[rs], r[rt]); break;
        case 32: case 33: r[rd] = r[rs] + r[rt]; break;
        case 34: case 35: r[rd] = r[rs] - r[rt]; break;
        case 36: r[rd] = r[rs] & r[rt]; break;
        case 37: r[rd] = r[rs] | r[rt]; break;
        case 38: r[rd] = r[rs] ^ r[rt]; break;
        case 39: r[rd] = ~(r[rs] | r[rt]); break;
        case 42: r[rd] = (s32)r[rs] < (s32)r[rt]; break;
        case 43: r[rd] = r[rs] < r[rt]; break;
        default: psx_invalid(c, pc, w);
        }
        break;
    case 8: case 9: r[rt] = r[rs] + simm; break;
    case 10: r[rt] = (s32)r[rs] < (s32)simm; break;
    case 11: r[rt] = r[rs] < simm; break;
    case 12: r[rt] = r[rs] & imm; break;
    case 13: r[rt] = r[rs] | imm; break;
    case 14: r[rt] = r[rs] ^ imm; break;
    case 15: r[rt] = imm << 16; break;
    case 16:
        if (w == 0x42000010) cop0_rfe(c);
        else if (rs == 0) r[rt] = cop0_read(c, rd);
        else if (rs == 4) cop0_write(c, rd, r[rt]);
        else psx_invalid(c, pc, w);
        break;
    case 18:
        if (rs & 0x10) gte_op(c, w & 0x1FFFFFF);
        else if (rs == 0) { r[rt] = gte_mfc2(c, rd); pgxp_mfc2(c, rt, rd); }
        else if (rs == 2) r[rt] = gte_cfc2(c, rd);
        else if (rs == 4) { gte_mtc2(c, rd, r[rt]); pgxp_mtc2(c, rt, rd); }
        else if (rs == 6) gte_ctc2(c, rd, r[rt]);
        else psx_invalid(c, pc, w);
        break;
    case 32: r[rt] = (u32)(s32)(s8)mem_r8(c, r[rs] + simm); break;
    case 33: r[rt] = (u32)(s32)(s16)mem_r16(c, r[rs] + simm); break;
    case 34: op_lwl(c, rt, r[rs] + simm); break;
    case 35: { u32 a = r[rs] + simm; r[rt] = mem_r32(c, a); pgxp_lw(c, rt, a); break; }
    case 36: r[rt] = mem_r8(c, r[rs] + simm); break;
    case 37: r[rt] = mem_r16(c, r[rs] + simm); break;
    case 38: op_lwr(c, rt, r[rs] + simm); break;
    case 40: mem_w8(c, r[rs] + simm, r[rt]); break;
    case 41: mem_w16(c, r[rs] + simm, r[rt]); break;
    case 42: op_swl(c, rt, r[rs] + simm); break;
    case 43: { u32 a = r[rs] + simm; mem_w32(c, a, r[rt]); pgxp_sw(c, rt, a); break; }
    case 46: op_swr(c, rt, r[rs] + simm); break;
    case 50: { u32 a = r[rs] + simm; gte_mtc2(c, rt, mem_r32(c, a)); pgxp_lwc2(c, rt, a); break; }
    case 58: { u32 a = r[rs] + simm; mem_w32(c, a, gte_mfc2(c, rt)); pgxp_swc2(c, rt, a); break; }
    default: psx_invalid(c, pc, w);
    }
    r[0] = 0;
}

void interp_run(Ctx* c, u32 pc, u32, u32) {
    int cs_slot = g_cs_n;
    CS_PUSH(pc | 1);
    struct Pop { int n; ~Pop() { g_cs_n = n; } } pop{cs_slot};
    for (;;) {
        g_cs[cs_slot & 4095] = pc | 1;
        u32 p = PHYS(pc);
        if (p >= RAM_SIZE || (pc & 3)) { c->pc = pc; psx_fatal("interp: bad pc %08x", pc); }
        u32 w;
        memcpy(&w, g_ram + p, 4);
        u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        bool ctl = op == 2 || op == 3 || (op >= 4 && op <= 7) || op == 1 || (op == 0 && ((w & 63) == 8 || (w & 63) == 9));
        if (!ctl) {
            exec_simple(c, w, pc);
            c->cyc++;
            pc += 4;
            continue;
        }
        u32 dw;
        memcpy(&dw, g_ram + ((p + 4) & (RAM_SIZE - 1)), 4);
        u32 bt = pc + 4 + ((u32)(s32)(s16)(w & 0xFFFF) << 2);
        u32 jt = ((pc + 4) & 0xF0000000u) | ((w & 0x3FFFFFF) << 2);
        c->cyc += 2;
        if (op == 0) {
            u32 t = c->r[rs];
            if ((w & 63) == 9) {  // jalr
                u32 rd = (w >> 11) & 31;
                if (rd) c->r[rd] = pc + 8;
                exec_simple(c, dw, pc + 4);
                call_indirect(c, t);
                pc += 8;
                continue;
            }
            exec_simple(c, dw, pc + 4);
            if (rs == 31) return;
            if (lookup_fn(t) || PHYS(t) == 0xA0 || PHYS(t) == 0xB0 || PHYS(t) == 0xC0) { call_indirect(c, t); return; }
            pc = t;
            CHK();
            continue;
        }
        if (op == 3) {
            c->r[31] = pc + 8;
            exec_simple(c, dw, pc + 4);
            call_indirect(c, jt);
            pc += 8;
            continue;
        }
        if (op == 2) {
            exec_simple(c, dw, pc + 4);
            if (lookup_fn(jt)) { call_indirect(c, jt); return; }
            if (jt <= pc) CHK();
            pc = jt;
            continue;
        }
        bool b;
        bool link = false;
        if (op == 4) b = c->r[rs] == c->r[rt];
        else if (op == 5) b = c->r[rs] != c->r[rt];
        else if (op == 6) b = (s32)c->r[rs] <= 0;
        else if (op == 7) b = (s32)c->r[rs] > 0;
        else {
            b = (rt & 1) ? (s32)c->r[rs] >= 0 : (s32)c->r[rs] < 0;
            link = (rt & 0x1E) == 0x10;
        }
        if (link) c->r[31] = pc + 8;
        exec_simple(c, dw, pc + 4);
        if (link) {
            if (b) call_indirect(c, bt);
            pc += 8;
            continue;
        }
        if (b) {
            if (bt <= pc) CHK();
            pc = bt;
        } else pc += 8;
    }
}

// ---------------------------------------------------------------------
void core_init() {
    g_pgxp_mem = (PgxpVal*)calloc(RAM_SIZE / 4, sizeof(PgxpVal));
    for (unsigned i = 0; i < main_func_count; i++) {
        u32 o = main_funcs[i].addr - MAIN_BASE;
        if (o < 0xE0000u) g_fn_main[o >> 2] = main_funcs[i].fn;
    }
    for (int i = 0; i < EV_COUNT; i++) g_ev_time[i] = NEVER;
    g_bias_q8 = (s64)(g_cfg.cpu_bias * 256.0 + 0.5);
    if (g_bias_q8 < 1) g_bias_q8 = 1;
    g_ctx.cop0[12] = 0x10900000;
    g_ctx.cop0[15] = 2;
    remove("interp_entries.txt");
}

bool load_exe_mem(const u8* d, size_t len) {
    if (len < 0x800 || memcmp(d, "PS-X EXE", 8)) return false;
    u32 pc0, gp0, taddr, tsize, saddr;
    memcpy(&pc0, d + 0x10, 4);
    memcpy(&gp0, d + 0x14, 4);
    memcpy(&taddr, d + 0x18, 4);
    memcpy(&tsize, d + 0x1C, 4);
    memcpy(&saddr, d + 0x30, 4);
    if (tsize > len - 0x800) tsize = (u32)(len - 0x800);
    memcpy(g_ram + PHYS(taddr), d + 0x800, tsize);
    g_ctx.pc = pc0;
    g_ctx.r[28] = gp0;
    g_ctx.r[29] = saddr ? saddr : 0x801FFFF0;
    g_ctx.r[30] = g_ctx.r[29];
    return true;
}

bool load_exe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<u8> buf;
    fseek(f, 0, SEEK_END);
    buf.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    return load_exe_mem(buf.data(), buf.size());
}
