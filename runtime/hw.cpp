// Timers, DMA, SIO (pads), MDEC stub.
#include "psx.h"
#include "savestate.h"

// =====================================================================
// Root counters
struct Timer {
    u32 mode, target;
    u32 count;      // value at base
    s64 base;       // sysclk tick of `count`
    bool irq_done;  // one-shot fired
    bool paused;
};
static Timer g_tm[3];
int g_dot_div = 8;  // dotclock divider from GPU hres
#define HBLANK_TICKS 2168

static void timer_rate(int t, s64* num, s64* den) {
    Timer& m = g_tm[t];
    *num = 1; *den = 1;
    if (t == 0 && (m.mode & 0x100)) { *num = 7 * g_dot_div; *den = 11; }
    else if (t == 1 && (m.mode & 0x100)) { *num = HBLANK_TICKS; }
    else if (t == 2 && (m.mode & 0x200)) { *num = 8; }
}
static u32 timer_period(int t) {
    Timer& m = g_tm[t];
    return ((m.mode & 8) && m.target) ? m.target + 1 : 0x10000;
}
static void timer_event(int id, s64 late);

static void timer_sync(int t) {
    Timer& m = g_tm[t];
    s64 now = sched_now();
    if (m.paused) { m.base = now; return; }
    s64 num, den;
    timer_rate(t, &num, &den);
    s64 adv = (now - m.base) * den / num;
    if (adv <= 0) return;
    m.base += adv * num / den;
    u64 v = (u64)m.count + (u64)adv;
    u32 per = timer_period(t);
    if (m.count < m.target && v >= m.target) m.mode |= 0x800;
    if (v >= 0x10000) m.mode |= 0x1000;
    if (v >= per) {
        if (m.mode & 8) m.mode |= 0x800;
        v %= per;
    }
    m.count = (u32)v;
}
static void timer_sched(int t) {
    Timer& m = g_tm[t];
    int ev = EV_TIMER0 + t;
    if (!(m.mode & 0x30) || m.paused || (m.irq_done && !(m.mode & 0x40))) { sched_cancel(ev); return; }
    s64 num, den;
    timer_rate(t, &num, &den);
    u32 per = timer_period(t);
    s64 best = 0x7FFFFFFF;
    if (m.mode & 0x10) {
        s64 d = (s64)m.target - m.count;
        if (d <= 0) d += per;
        if (m.target < per || per == 0x10000) if (d < best) best = d;
    }
    if (m.mode & 0x20) {
        s64 d = 0x10000 - (s64)m.count;
        if (per == 0x10000 && d < best) best = d;
    }
    if (best == 0x7FFFFFFF) { sched_cancel(ev); return; }
    s64 ticks = (best * num + den - 1) / den;
    if (ticks < 1) ticks = 1;
    sched_set(ev, ticks, timer_event);
}
static void timer_event(int id, s64) {
    int t = id - EV_TIMER0;
    Timer& m = g_tm[t];
    timer_sync(t);
    if (!(m.irq_done && !(m.mode & 0x40))) {
        m.irq_done = true;
        m.mode &= ~0x400u;
        irq_raise(IRQ_TMR0 + t);
        if (m.mode & 0x80) m.mode ^= 0;  // toggle mode not modelled
    }
    timer_sched(t);
}
u32 timer_read(u32 addr) {
    int t = (addr >> 4) & 3;
    if (t > 2) return 0;
    Timer& m = g_tm[t];
    timer_sync(t);
    switch (addr & 0xC) {
    case 0: return m.count & 0xFFFF;
    case 4: { u32 v = m.mode; m.mode &= ~0x1800u; return v; }
    case 8: return m.target;
    }
    return 0;
}
void timer_write(u32 addr, u32 v) {
    int t = (addr >> 4) & 3;
    if (t > 2) return;
    Timer& m = g_tm[t];
    timer_sync(t);
    switch (addr & 0xC) {
    case 0: m.count = v & 0xFFFF; break;
    case 4:
        m.mode = (v & 0x3FF) | 0x400;
        m.count = 0;
        m.irq_done = false;
        m.paused = (t == 2 && (v & 1) && ((v >> 1) & 3) != 1 && ((v >> 1) & 3) != 2);
        break;
    case 8: m.target = v & 0xFFFF; break;
    }
    m.base = sched_now();
    timer_sched(t);
}
void timers_hblank() {}
void timers_vblank(bool) {}
void timers_dotclock_changed() {
    timer_sync(0);
    timer_sched(0);
}
void timers_init() {
    for (int i = 0; i < 3; i++) g_tm[i] = Timer{0x400, 0, 0, 0, false, false};
}

// =====================================================================
// DMA
static u32 g_dma_madr[7], g_dma_bcr[7], g_dma_chcr[7];
static u32 g_dpcr = 0x07654321, g_dicr;

static void dma_done(int id, s64) {
    int ch = id == EV_DMA_GPU ? 2 : id == EV_DMA_CD ? 3 : id == EV_DMA_SPU ? 4 : id == EV_DMA_OTC ? 6 : 0;
    if (id == EV_DMA_MDEC) {
        for (int k = 0; k < 2; k++) g_dma_chcr[k] &= ~0x01000000u;
        ch = (g_dicr & (1u << 17)) ? 1 : 0;
    }
    g_dma_chcr[ch] &= ~0x01000000u;
    if (g_dicr & (1u << (16 + ch))) {
        g_dicr |= 1u << (24 + ch);
        if (g_dicr & (1u << 23)) { g_dicr |= 0x80000000u; irq_raise(IRQ_DMA); }
    }
}

static void dma_start(int ch) {
    u32 chcr = g_dma_chcr[ch], madr = g_dma_madr[ch], bcr = g_dma_bcr[ch];
    if (!(g_dpcr & (8u << (ch * 4)))) return;
    s64 delay = 64;
    int ev = EV_DMA_GPU;
    u32 words = ((chcr >> 9) & 3) == 1 ? (bcr & 0xFFFF) * (bcr >> 16) : (bcr & 0xFFFF);
    switch (ch) {
    case 0: mdec_dma_in(madr, bcr, chcr); ev = EV_DMA_MDEC; break;
    case 1: mdec_dma_out(madr, bcr, chcr); ev = EV_DMA_MDEC; break;
    case 2: gpu_dma(madr, bcr, chcr); ev = EV_DMA_GPU; delay = 128 + words / 4; break;
    case 3: cdrom_dma(madr, bcr, chcr); ev = EV_DMA_CD; delay = 128 + words * 2; break;
    case 4: spu_dma(madr, bcr, chcr); ev = EV_DMA_SPU; delay = 128 + words; break;
    case 6: {
        u32 n = bcr & 0xFFFF;
        if (!n) n = 0x10000;
        u32 a = madr & 0x1FFFFC;
        while (n--) {
            u32 next = (a - 4) & 0x1FFFFC;
            u32 v = n ? next : 0xFFFFFF;
            memcpy(g_ram + a, &v, 4);
            g_pgxp_mem[a >> 2].val = ~v;
            a = next;
        }
        ev = EV_DMA_OTC;
        delay = 64 + words / 2;
        break;
    }
    default: break;
    }
    if (((chcr >> 9) & 3) == 1) {  // sync blocks: MADR advances
        g_dma_madr[ch] = (madr + words * 4 * ((chcr & 2) ? -1 : 1)) & 0xFFFFFF;
        g_dma_bcr[ch] &= 0xFFFF;
    }
    { extern void watch_check(const char*); static const char* nm[7] = {"dma0", "dma1", "dma2", "dma3", "dma4", "dma5", "dma6"}; watch_check(nm[ch]); }
    if (delay > 20000) delay = 20000;
    sched_set(ev, delay, dma_done);
}

u32 dma_read(u32 a) {
    if (a == 0x1F8010F0) return g_dpcr;
    if (a == 0x1F8010F4) return g_dicr;
    int ch = (a - 0x1F801080) >> 4;
    if (ch > 6) return 0;
    switch (a & 0xC) {
    case 0: return g_dma_madr[ch];
    case 4: return g_dma_bcr[ch];
    case 8: return g_dma_chcr[ch];
    }
    return 0;
}
void dma_write(u32 a, u32 v) {
    if (a == 0x1F8010F0) { g_dpcr = v; return; }
    if (a == 0x1F8010F4) {
        u32 flags = (g_dicr & 0x7F000000) & ~(v & 0x7F000000);
        g_dicr = (v & 0x00FF803F) | flags;
        if ((g_dicr & 0x8000) || ((g_dicr & 0x800000) && (g_dicr & 0x7F000000 & ((g_dicr & 0x7F0000) << 8))))
            g_dicr |= 0x80000000u;
        return;
    }
    int ch = (a - 0x1F801080) >> 4;
    if (ch > 6) return;
    switch (a & 0xC) {
    case 0: g_dma_madr[ch] = v & 0xFFFFFF; break;
    case 4: g_dma_bcr[ch] = v; break;
    case 8:
        if (ch == 6) v = (v & 0x51000000) | 2;
        g_dma_chcr[ch] = v;
        if (v & 0x01000000) dma_start(ch);
        break;
    }
}

// =====================================================================
// MDEC (unused by this game; keeps status sane)
u32 mdec_read(u32 a) { return a == 0x1F801824 ? 0x80040000 : 0; }
void mdec_write(u32, u32) {}
void mdec_dma_in(u32, u32, u32) {}
void mdec_dma_out(u32, u32, u32) {}

// =====================================================================
// SIO0: DualShock on port 1/2. Memory cards are served by the BIOS HLE.
struct PadDev {
    bool analog = true, config = false, locked = false;
    u8 rumble_map[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    u8 motor[2] = {0, 0};
};
static PadDev g_pad[2];
static u16 g_sio_ctrl, g_sio_mode, g_sio_baud;
static u32 g_sio_stat = 5;
static u8 g_sio_rx = 0xFF;
static bool g_sio_rx_ready;
static int g_sio_state;      // byte index within the packet, -1 = idle/ignored
static u8 g_sio_cmd;
static u8 g_sio_resp[40];
static int g_sio_len;
static u8 g_sio_in[40];
u8 g_pad_motor[2][2];

static void pad_build(int port, u8 cmd) {
    PadDev& p = g_pad[port];
    u8* r = g_sio_resp;
    memset(r, 0xFF, sizeof g_sio_resp);
    r[0] = 0xFF;
    r[1] = p.config ? 0xF3 : p.analog ? 0x73 : 0x41;
    r[2] = 0x5A;
    g_sio_len = 3 + (p.config || p.analog ? 6 : 2);
    if (cmd == 0x42 || (cmd == 0x43 && !p.config)) {
        plat_input_poll();
        u16 b = pad_buttons(port);
        r[3] = b & 0xFF;
        r[4] = b >> 8;
        if (p.analog || p.config) pad_analog(port, r + 5);
        if (p.config) g_sio_len = 9;
        return;
    }
    if (!p.config) { g_sio_len = 2; r[1] = 0xFF; return; }
    static const u8 zero6[6] = {0, 0, 0, 0, 0, 0};
    memcpy(r + 3, zero6, 6);
    switch (cmd) {
    case 0x45: { const u8 d[6] = {0x01, 0x02, (u8)(p.analog ? 1 : 0), 0x02, 0x01, 0x00}; memcpy(r + 3, d, 6); break; }
    case 0x47: { const u8 d[6] = {0, 0, 2, 0, 1, 0}; memcpy(r + 3, d, 6); break; }
    case 0x4D: memcpy(r + 3, p.rumble_map, 6); break;
    default: break;
    }
}

static void pad_byte_in(int port, int idx, u8 v) {
    PadDev& p = g_pad[port];
    u8 cmd = g_sio_cmd;
    if (cmd == 0x42 && !p.config) {
        if (idx >= 3 && idx < 9 && p.rumble_map[idx - 3] < 2) {
            p.motor[p.rumble_map[idx - 3]] = v;
            g_pad_motor[port][p.rumble_map[idx - 3]] = v;
        }
    } else if (cmd == 0x43) {
        if (idx == 3) p.config = v == 1;
    } else if (p.config) {
        if (cmd == 0x44) {
            if (idx == 3) p.analog = v == 1;
            if (idx == 4) p.locked = v == 3;
        } else if (cmd == 0x46 && idx == 3) {
            const u8 a[6] = {0, 0, 1, 2, 0, 0x0A}, b[6] = {0, 0, 1, 1, 1, 0x14};
            memcpy(g_sio_resp + 3, v ? b : a, 6);
        } else if (cmd == 0x4C && idx == 3) {
            g_sio_resp[6] = v ? 7 : 4;
        } else if (cmd == 0x4D && idx >= 3 && idx < 9) {
            p.rumble_map[idx - 3] = v;
        }
    }
}

static void sio_ack(int, s64) {
    g_sio_stat |= 0x200;
    if (g_sio_ctrl & 0x1000) irq_raise(IRQ_SIO0);
}

u32 sio_read(u32 a, int size) {
    switch (a & 0x1E) {
    case 0x00: { u8 v = g_sio_rx; g_sio_rx_ready = false; g_sio_rx = 0xFF; return v | (0xFFFFFF00u * (size == 4)); }
    case 0x04: return (g_sio_stat & 0x200) | 5 | (g_sio_rx_ready ? 2 : 0);
    case 0x08: return g_sio_mode;
    case 0x0A: return g_sio_ctrl;
    case 0x0E: return g_sio_baud;
    }
    return a >= 0x1F801050 ? 5 : 0;
}

void sio_write(u32 a, u32 v, int) {
    if (a >= 0x1F801050) return;
    switch (a & 0x1E) {
    case 0x00: {
        u8 out = 0xFF;
        bool ack = false;
        int port = (g_sio_ctrl >> 13) & 1;
        if ((g_sio_ctrl & 2) && g_sio_state >= 0) {
            int i = g_sio_state;
            if (i == 0) {
                if (v == 0x01 && port < 2 && (port == 0)) { ack = true; g_sio_state = 1; }
                else g_sio_state = -1;  // memory card (0x81) or absent pad: no ACK
            } else {
                if (i == 1) { g_sio_cmd = (u8)v; pad_build(port, (u8)v); }
                else pad_byte_in(port, i, (u8)v);
                out = i < g_sio_len ? g_sio_resp[i] : 0xFF;
                g_sio_state = i + 1;
                ack = g_sio_state < g_sio_len;
                if (!ack) g_sio_state = -1;
            }
        }
        g_sio_rx = out;
        g_sio_rx_ready = true;
        if (ack) sched_set(EV_SIO, 500, sio_ack);
        break;
    }
    case 0x08: g_sio_mode = (u16)v; break;
    case 0x0A:
        g_sio_ctrl = (u16)v;
        if (v & 0x10) g_sio_stat &= ~0x200u;
        if (v & 0x40) { g_sio_ctrl = 0; g_sio_stat = 5; g_sio_state = 0; g_sio_rx_ready = false; }
        if (!(v & 2)) g_sio_state = 0;
        break;
    case 0x0E: g_sio_baud = (u16)v; break;
    }
}

bool bios_pad_poll(int port, u8* buf, int len) {
    memset(buf, 0xFF, len);
    if (port != 0) return false;
    plat_input_poll();
    u16 b = pad_buttons(port);
    buf[0] = 0;
    buf[1] = 0x41;
    buf[2] = b & 0xFF;
    buf[3] = b >> 8;
    return true;
}

void sio_init() { g_sio_state = 0; }

// ---- save states ---------------------------------------------------------------------------
void hw_state(StateIO& s) {
    s.tag("HWIO");
    s.pod(g_tm); s.pod(g_dot_div);
    s.pod(g_dma_madr); s.pod(g_dma_bcr); s.pod(g_dma_chcr); s.pod(g_dpcr); s.pod(g_dicr);
    s.pod(g_pad);
    s.pod(g_sio_ctrl); s.pod(g_sio_mode); s.pod(g_sio_baud); s.pod(g_sio_stat);
    s.pod(g_sio_rx); s.pod(g_sio_rx_ready); s.pod(g_sio_state); s.pod(g_sio_cmd);
    s.pod(g_sio_resp); s.pod(g_sio_len); s.pod(g_sio_in);
}
