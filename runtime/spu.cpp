// SPU: 24 ADPCM voices, ADSR, pitch modulation, noise, reverb, CD/XA input.
// Mixing is done in float with selectable interpolation ("enhanced audio").
#include "psx.h"
#include <math.h>
#include "savestate.h"

static u8 g_sram[0x80000];
FILE* g_wav;  // raw s16 stereo 44.1k dump (tests)
static u16 g_reg[0x200];
static u32 g_xfer_addr;
static u16 g_spucnt, g_spustat;
static u32 g_kon, g_koff, g_pmon, g_non, g_eon, g_endx;
static u32 g_irq_addr;

enum { PH_ATTACK, PH_DECAY, PH_SUSTAIN, PH_RELEASE, PH_OFF };
struct Voice {
    u32 addr, repeat;
    u32 counter;        // 12.12 within decoded block stream
    s16 buf[28 + 4];    // [0..3] = last 4 of previous block
    int pos;            // sample index in block (0..27), 28 = need decode
    s32 h1, h2;
    int phase;
    s32 env;            // 0..0x7FFF
    u32 env_cnt;
    s32 last;           // last output amplitude (for pitch mod)
    bool on;
    bool repeat_set;
};
static Voice g_v[24];
static u32 g_noise = 1, g_noise_cnt;
static s64 g_spu_time;
static s64 g_spu_frac;

// reverb
static u32 g_rv_base, g_rv_cur;
static bool g_rv_phase;
static float g_rv_out[2];
static float g_rv_in_acc[2];

// CD audio ring (44.1k stereo float)
#define CDRING 65536
static float g_cd[CDRING][2];
static u32 g_cd_r, g_cd_w;
static bool g_cd_started;
static float g_cd_hist[4][2];
static double g_cd_phase;

static inline u16 ram16(u32 a) { a &= 0x7FFFE; return (u16)(g_sram[a] | (g_sram[a + 1] << 8)); }
static inline void ramw16(u32 a, u16 v) { a &= 0x7FFFE; g_sram[a] = (u8)v; g_sram[a + 1] = (u8)(v >> 8); }

static void spu_irq_check(u32 addr) {
    if ((g_spucnt & 0x40) && (addr & ~7u) == (g_irq_addr & ~7u) && !(g_spustat & 0x40)) {
        g_spustat |= 0x40;
        irq_raise(IRQ_SPU);
    }
}

static void decode_block(Voice& v, int idx) {
    static const int f0[5] = {0, 60, 115, 98, 122}, f1[5] = {0, 0, -52, -55, -60};
    u32 a = v.addr & 0x7FFF8;
    spu_irq_check(a);
    spu_irq_check(a + 8);
    const u8* b = g_sram + a;
    int shift = b[0] & 15, flt = (b[0] >> 4) & 7;
    if (flt > 4) flt = 4;
    if (shift > 12) shift = 9;
    u8 flags = b[1];
    memcpy(v.buf, v.buf + 28, 4 * sizeof(s16));
    for (int i = 0; i < 28; i++) {
        int nib = (b[2 + i / 2] >> ((i & 1) * 4)) & 15;
        s32 t = (s16)(nib << 12) >> shift;
        s32 s = t + ((v.h1 * f0[flt] + v.h2 * f1[flt] + 32) >> 6);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v.h2 = v.h1;
        v.h1 = s;
        v.buf[4 + i] = (s16)s;
    }
    if (flags & 4) v.repeat = a;
    if (flags & 1) {
        g_endx |= 1u << idx;
        v.addr = v.repeat;
        if (!(flags & 2)) { v.phase = PH_RELEASE; v.env = 0; }
    } else v.addr = (a + 16) & 0x7FFF8;
}

static void env_step(Voice& v, int idx) {
    u32 lo = g_reg[(idx * 16 + 8) >> 1], hi = g_reg[(idx * 16 + 10) >> 1];
    int shift, step;
    bool expo, dec;
    switch (v.phase) {
    case PH_ATTACK: expo = lo & 0x8000; dec = false; shift = (lo >> 10) & 31; step = 7 - ((lo >> 8) & 3); break;
    case PH_DECAY: expo = true; dec = true; shift = (lo >> 4) & 15; step = -8; break;
    case PH_SUSTAIN: expo = hi & 0x8000; dec = hi & 0x4000; shift = (hi >> 8) & 31; step = dec ? -8 + (int)((hi >> 6) & 3) : 7 - (int)((hi >> 6) & 3); break;
    case PH_RELEASE: expo = hi & 0x20; dec = true; shift = hi & 31; step = -8; break;
    default: return;
    }
    u32 cycles = 1u << (shift > 11 ? shift - 11 : 0);
    s32 st = step << (shift < 11 ? 11 - shift : 0);
    if (expo && !dec && v.env > 0x6000) cycles *= 4;
    if (expo && dec) st = (st * v.env) >> 15;
    if (++v.env_cnt < cycles) return;
    v.env_cnt = 0;
    v.env += st;
    if (v.env > 0x7FFF) v.env = 0x7FFF;
    if (v.env < 0) v.env = 0;
    if (v.phase == PH_ATTACK && v.env >= 0x7FFF) v.phase = PH_DECAY;
    else if (v.phase == PH_DECAY) {
        s32 sl = ((lo & 15) + 1) * 0x800;
        if (v.env <= sl) v.phase = PH_SUSTAIN;
    } else if (v.phase == PH_RELEASE && v.env == 0) { v.phase = PH_OFF; v.on = false; }
}

static inline float interp(const s16* p, u32 frac) {  // p[0..3], between p[1] and p[2]
    float t = (frac & 0xFFF) / 4096.0f;
    switch (g_cfg.audio_interp) {
    case 0: return p[1];
    case 1: return p[1] + (p[2] - p[1]) * t;
    case 2: {  // cubic B-spline: very close to the console's Gaussian kernel
        float t2 = t * t, t3 = t2 * t;
        float w0 = (1 - 3 * t + 3 * t2 - t3) / 6, w1 = (4 - 6 * t2 + 3 * t3) / 6, w2 = (1 + 3 * t + 3 * t2 - 3 * t3) / 6, w3 = t3 / 6;
        return p[0] * w0 + p[1] * w1 + p[2] * w2 + p[3] * w3;
    }
    default: {  // Catmull-Rom: sharper, less high-frequency loss
        float a = -0.5f * p[0] + 1.5f * p[1] - 1.5f * p[2] + 0.5f * p[3];
        float b = p[0] - 2.5f * p[1] + 2 * p[2] - 0.5f * p[3];
        float c = -0.5f * p[0] + 0.5f * p[2];
        return ((a * t + b) * t + c) * t + p[1];
    }
    }
}

static inline float vol_of(u16 r) {
    if (r & 0x8000) return 1.0f;  // sweep modes: treat as full
    return (float)(s16)(r << 1) / 32768.0f;
}

// Address inside the reverb work area [base, 0x80000), relative to the moving write position.
// Offsets can be negative ("previous sample", all-pass taps), so wrap in both directions.
static inline u32 rv_addr(u32 off) {
    s32 size = (s32)(0x80000 - g_rv_base);
    s32 a = (s32)(g_rv_cur - g_rv_base) + (s32)off;
    a %= size;
    if (a < 0) a += size;
    return g_rv_base + (u32)a;
}
static inline s16 rv_rd(u32 off) { return (s16)ram16(rv_addr(off)); }
static inline void rv_wr(u32 off, float v) {
    if (!(g_spucnt & 0x80)) return;
    s32 i = (s32)lrintf(v);
    ramw16(rv_addr(off), (u16)(s16)(i > 32767 ? 32767 : i < -32768 ? -32768 : i));
}
#define RV(n) g_reg[(0x1C0 + (n) * 2) >> 1]
#define RVA(n) ((u32)RV(n) * 8)
#define RVV(n) ((float)(s16)RV(n) / 32768.0f)

static void reverb_tick(float inl, float inr) {
    if (g_rv_base >= 0x7FFF8) { g_rv_out[0] = g_rv_out[1] = 0; return; }
    enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2,
           dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };
    float lin = inl * RVV(vLIN), rin = inr * RVV(vRIN);
    float iir = RVV(vIIR), wall = RVV(vWALL);
    float t;
    t = rv_rd(RVA(mLSAME) - 2); rv_wr(RVA(mLSAME), (lin + rv_rd(RVA(dLSAME)) * wall - t) * iir + t);
    t = rv_rd(RVA(mRSAME) - 2); rv_wr(RVA(mRSAME), (rin + rv_rd(RVA(dRSAME)) * wall - t) * iir + t);
    t = rv_rd(RVA(mLDIFF) - 2); rv_wr(RVA(mLDIFF), (lin + rv_rd(RVA(dRDIFF)) * wall - t) * iir + t);
    t = rv_rd(RVA(mRDIFF) - 2); rv_wr(RVA(mRDIFF), (rin + rv_rd(RVA(dLDIFF)) * wall - t) * iir + t);
    float lo = RVV(vCOMB1) * rv_rd(RVA(mLCOMB1)) + RVV(vCOMB2) * rv_rd(RVA(mLCOMB2)) + RVV(vCOMB3) * rv_rd(RVA(mLCOMB3)) + RVV(vCOMB4) * rv_rd(RVA(mLCOMB4));
    float ro = RVV(vCOMB1) * rv_rd(RVA(mRCOMB1)) + RVV(vCOMB2) * rv_rd(RVA(mRCOMB2)) + RVV(vCOMB3) * rv_rd(RVA(mRCOMB3)) + RVV(vCOMB4) * rv_rd(RVA(mRCOMB4));
    float a1 = RVV(vAPF1), a2 = RVV(vAPF2);
    lo -= a1 * rv_rd(RVA(mLAPF1) - RVA(dAPF1)); rv_wr(RVA(mLAPF1), lo); lo = lo * a1 + rv_rd(RVA(mLAPF1) - RVA(dAPF1));
    ro -= a1 * rv_rd(RVA(mRAPF1) - RVA(dAPF1)); rv_wr(RVA(mRAPF1), ro); ro = ro * a1 + rv_rd(RVA(mRAPF1) - RVA(dAPF1));
    lo -= a2 * rv_rd(RVA(mLAPF2) - RVA(dAPF2)); rv_wr(RVA(mLAPF2), lo); lo = lo * a2 + rv_rd(RVA(mLAPF2) - RVA(dAPF2));
    ro -= a2 * rv_rd(RVA(mRAPF2) - RVA(dAPF2)); rv_wr(RVA(mRAPF2), ro); ro = ro * a2 + rv_rd(RVA(mRAPF2) - RVA(dAPF2));
    g_rv_out[0] = lo * ((float)(s16)g_reg[0x184 >> 1] / 32768.0f);
    g_rv_out[1] = ro * ((float)(s16)g_reg[0x186 >> 1] / 32768.0f);
    g_rv_cur += 2;
    if (g_rv_cur >= 0x80000) g_rv_cur = g_rv_base;
}

static void gen_sample(float* out) {
    float l = 0, r = 0, rl = 0, rr = 0;
    // noise
    {
        u32 shift = (g_spucnt >> 10) & 15, step = 4 + ((g_spucnt >> 8) & 3);
        g_noise_cnt += 0x10000 >> 0;
        u32 period = 0x20000u >> 0;
        (void)period;
        static u32 timer;
        timer += step << shift;
        while (timer >= 0x40000) {  // approx: clock LFSR
            timer -= 0x40000;
            u32 bit = ((g_noise >> 15) ^ (g_noise >> 12) ^ (g_noise >> 11) ^ (g_noise >> 10) ^ 1) & 1;
            g_noise = ((g_noise << 1) | bit) & 0xFFFF;
        }
    }
    for (int i = 0; i < 24; i++) {
        Voice& v = g_v[i];
        if (!v.on) { v.last = 0; continue; }
        u32 step = g_reg[(i * 16 + 4) >> 1];
        if ((g_pmon & (1u << i)) && i > 0) {
            s32 f = g_v[i - 1].last + 0x8000;
            step = (u32)(((s32)(s16)step * f) >> 15) & 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        // advance
        v.counter += step;
        while (v.counter >= 0x1000) {
            v.counter -= 0x1000;
            v.pos++;
            if (v.pos >= 28) { decode_block(v, i); v.pos = 0; }
        }
        float s;
        if (g_non & (1u << i)) s = (float)(s16)g_noise;
        else s = interp(&v.buf[v.pos + 1], v.counter);
        env_step(v, i);
        s = s * (float)v.env / 32768.0f;
        v.last = (s32)s;
        g_reg[(i * 16 + 12) >> 1] = (u16)v.env;
        float vl = s * vol_of(g_reg[(i * 16) >> 1]), vr = s * vol_of(g_reg[(i * 16 + 2) >> 1]);
        l += vl; r += vr;
        if (g_eon & (1u << i)) { rl += vl; rr += vr; }
    }
    // CD input
    if (g_cd_started && g_cd_r != g_cd_w) {
        float cl = g_cd[g_cd_r][0], cr = g_cd[g_cd_r][1];
        g_cd_r = (g_cd_r + 1) & (CDRING - 1);
        if (g_spucnt & 1) {
            float vl = (float)(s16)g_reg[0x1B0 >> 1] / 32768.0f, vr = (float)(s16)g_reg[0x1B2 >> 1] / 32768.0f;
            cl *= vl; cr *= vr;
            l += cl; r += cr;
            if (g_spucnt & 4) { rl += cl; rr += cr; }
        }
    } else g_cd_started = false;
    if (g_cfg.reverb) {
        g_rv_in_acc[0] += rl; g_rv_in_acc[1] += rr;
        g_rv_phase = !g_rv_phase;
        if (g_rv_phase) { reverb_tick(g_rv_in_acc[0] * 0.5f, g_rv_in_acc[1] * 0.5f); g_rv_in_acc[0] = g_rv_in_acc[1] = 0; }
        l += g_rv_out[0]; r += g_rv_out[1];
    }
    float ml = vol_of(g_reg[0x180 >> 1]), mr = vol_of(g_reg[0x182 >> 1]);
    if (!(g_spucnt & 0x4000)) ml = mr = 0;  // muted
    if (g_log_level >= 3) {
        static int n;
        if ((l != l || r != r || (ml == 0 && l != 0)) && n++ < 30)
            logf("SPU zero/NaN: l=%f r=%f rv=%f,%f mainvol=%04x,%04x frame %d\n", l, r, g_rv_out[0], g_rv_out[1], g_reg[0x180 >> 1], g_reg[0x182 >> 1], g_frame);
    }
    // headroom + soft limiter instead of the console's hard 16-bit clip
    float ol = l * ml / 32768.0f * 0.8f, orr = r * mr / 32768.0f * 0.8f;
    out[0] = ol > 0.8f || ol < -0.8f ? (ol > 0 ? 1.0f : -1.0f) * (0.8f + 0.2f * tanhf((fabsf(ol) - 0.8f) * 5.0f)) : ol;
    out[1] = orr > 0.8f || orr < -0.8f ? (orr > 0 ? 1.0f : -1.0f) * (0.8f + 0.2f * tanhf((fabsf(orr) - 0.8f) * 5.0f)) : orr;
}

void spu_run(s64 now) {
    s64 d = now - g_spu_time + g_spu_frac;
    g_spu_time = now;
    int n = (int)(d / 768);
    g_spu_frac = d % 768;
    static float buf[4096 * 2];
    while (n > 0) {
        int k = n > 4096 ? 4096 : n;
        for (int i = 0; i < k; i++) gen_sample(buf + i * 2);
        if (!g_cfg.headless) plat_audio_push(buf, k);
        if (g_wav) {
            static s16 pcm[4096 * 2];
            for (int i = 0; i < k * 2; i++) { float f = buf[i] * 32767.0f; pcm[i] = (s16)(f > 32767 ? 32767 : f < -32768 ? -32768 : f); }
            fwrite(pcm, 2, k * 2, g_wav);
        }
        n -= k;
    }
}

static void spu_event(int, s64) {
    spu_run(sched_now());
    sched_set(EV_SPU, 33868, spu_event);
}
void spu_frame() { spu_run(sched_now()); }

void spu_cd_audio(const s16* lr, int frames, int rate) {
    // resample to 44.1k (Catmull-Rom) into the ring
    double step = (double)rate / 44100.0;
    for (int i = 0; i < frames; i++) {
        for (int k = 0; k < 3; k++) { g_cd_hist[k][0] = g_cd_hist[k + 1][0]; g_cd_hist[k][1] = g_cd_hist[k + 1][1]; }
        g_cd_hist[3][0] = lr[i * 2];
        g_cd_hist[3][1] = lr[i * 2 + 1];
        while (g_cd_phase < 1.0) {
            float t = (float)g_cd_phase;
            for (int ch = 0; ch < 2; ch++) {
                float p0 = g_cd_hist[0][ch], p1 = g_cd_hist[1][ch], p2 = g_cd_hist[2][ch], p3 = g_cd_hist[3][ch];
                float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3, b = p0 - 2.5f * p1 + 2 * p2 - 0.5f * p3, c = -0.5f * p0 + 0.5f * p2;
                g_cd[g_cd_w][ch] = ((a * t + b) * t + c) * t + p1;
            }
            u32 nw = (g_cd_w + 1) & (CDRING - 1);
            if (nw != g_cd_r) g_cd_w = nw;
            g_cd_phase += step;
        }
        g_cd_phase -= 1.0;
    }
    u32 fill = (g_cd_w - g_cd_r) & (CDRING - 1);
    // start (or restart after running dry) only with a cushion of about two sectors, so timing
    // jitter between the disc and the sound clock can never empty the buffer mid-stream
    if (fill > 5200) g_cd_started = true;
}

u32 spu_read(u32 addr) {
    u32 o = addr - 0x1F801C00;
    if (o >= 0x400) return 0;
    switch (o) {
    case 0x188: return g_kon & 0xFFFF;
    case 0x18A: return g_kon >> 16;
    case 0x18C: return g_koff & 0xFFFF;
    case 0x18E: return g_koff >> 16;
    case 0x19C: return g_endx & 0xFFFF;
    case 0x19E: return g_endx >> 16;
    case 0x1AA: return g_spucnt;
    case 0x1AE: return (g_spustat & 0x40) | (g_spucnt & 0x3F);
    case 0x1A6: return (u16)(g_xfer_addr >> 3);
    }
    if (o < 0x180 && (o & 0xF) == 0xE) return (u16)(g_v[o >> 4].repeat >> 3);
    if (o < 0x200) return g_reg[o >> 1];
    return 0;
}

void spu_write(u32 addr, u32 val) {
    u32 o = addr - 0x1F801C00;
    u16 v = (u16)val;
    if (o >= 0x200) return;
    spu_run(sched_now());
    g_reg[o >> 1] = v;
    if (o < 0x180) {
        if ((o & 0xF) == 0xE) { g_v[o >> 4].repeat = (u32)v << 3; g_v[o >> 4].repeat_set = true; }
        return;
    }
    switch (o) {
    case 0x188: case 0x18A: {
        u32 bits = o == 0x188 ? v : (u32)v << 16;
        g_kon = (g_kon & (o == 0x188 ? 0xFFFF0000u : 0xFFFFu)) | bits;
        for (int i = 0; i < 24; i++)
            if (bits & (1u << i)) {
                Voice& vc = g_v[i];
                vc.addr = (u32)g_reg[(i * 16 + 6) >> 1] << 3;
                if (g_log_level >= 4) {
                    logf("spu: key on voice %d sample %05x pitch %04x frame %d stack:", i, vc.addr, g_reg[(i * 16 + 4) >> 1], g_frame);
                    for (int s = g_cs_n - 1, k = 0; s >= 0 && k < 8; s--, k++) logf(" %08x", g_cs[s & 4095]);
                    logf("\n");
                }
                vc.repeat = vc.addr;
                vc.counter = 0;
                vc.pos = 28;
                vc.h1 = vc.h2 = 0;
                memset(vc.buf, 0, sizeof vc.buf);
                vc.phase = PH_ATTACK;
                vc.env = 0;
                vc.env_cnt = 0;
                vc.on = true;
                g_endx &= ~(1u << i);
                decode_block(vc, i);
                vc.pos = 0;
            }
        break;
    }
    case 0x18C: case 0x18E: {
        u32 bits = o == 0x18C ? v : (u32)v << 16;
        g_koff = (g_koff & (o == 0x18C ? 0xFFFF0000u : 0xFFFFu)) | bits;
        for (int i = 0; i < 24; i++)
            if ((bits & (1u << i)) && g_v[i].on) g_v[i].phase = PH_RELEASE;
        break;
    }
    case 0x190: g_pmon = (g_pmon & 0xFFFF0000u) | v; break;
    case 0x192: g_pmon = (g_pmon & 0xFFFF) | ((u32)v << 16); break;
    case 0x194: g_non = (g_non & 0xFFFF0000u) | v; break;
    case 0x196: g_non = (g_non & 0xFFFF) | ((u32)v << 16); break;
    case 0x198: g_eon = (g_eon & 0xFFFF0000u) | v; break;
    case 0x19A: g_eon = (g_eon & 0xFFFF) | ((u32)v << 16); break;
    case 0x19C: case 0x19E: break;
    case 0x1A2: g_rv_base = (u32)v << 3; g_rv_cur = g_rv_base; break;
    case 0x1A4: g_irq_addr = (u32)v << 3; break;
    case 0x1A6: g_xfer_addr = (u32)v << 3; break;
    case 0x1A8:
        ramw16(g_xfer_addr, v);
        spu_irq_check(g_xfer_addr);
        g_xfer_addr = (g_xfer_addr + 2) & 0x7FFFE;
        break;
    case 0x1AA:
        if (g_log_level >= 3 && ((v ^ g_spucnt) & 0xC000)) logf("SPUCNT %04x -> %04x frame %d\n", g_spucnt, v, g_frame);
        g_spucnt = v;
        if (!(v & 0x40)) g_spustat &= ~0x40;
        break;
    }
}

void spu_dma(u32 madr, u32 bcr, u32 chcr) {
    u32 words = (bcr & 0xFFFF) * (((chcr >> 9) & 3) == 1 ? (bcr >> 16) : 1);
    u32 a = madr & 0x1FFFFC;
    if (chcr & 1) {
        for (u32 i = 0; i < words * 4; i++) {
            g_sram[g_xfer_addr] = g_ram[(a + i) & (RAM_SIZE - 1)];
            if (!(g_xfer_addr & 7)) spu_irq_check(g_xfer_addr);
            g_xfer_addr = (g_xfer_addr + 1) & 0x7FFFF;
        }
    } else {
        for (u32 i = 0; i < words * 4; i++) {
            g_ram[(a + i) & (RAM_SIZE - 1)] = g_sram[g_xfer_addr];
            g_xfer_addr = (g_xfer_addr + 1) & 0x7FFFF;
        }
    }
}

void spu_init() {
    for (auto& v : g_v) { v.phase = PH_OFF; v.on = false; v.pos = 28; }
    sched_set(EV_SPU, 33868, spu_event);
}

// ---- save states ---------------------------------------------------------------------------
void spu_state(StateIO& s) {
    s.tag("SPU_");
    s.pod(g_sram); s.pod(g_reg);
    s.pod(g_xfer_addr); s.pod(g_spucnt); s.pod(g_spustat);
    s.pod(g_kon); s.pod(g_koff); s.pod(g_pmon); s.pod(g_non); s.pod(g_eon); s.pod(g_endx);
    s.pod(g_irq_addr);
    s.pod(g_v);
    s.pod(g_noise); s.pod(g_noise_cnt);
    s.pod(g_spu_time); s.pod(g_spu_frac);
    s.pod(g_rv_base); s.pod(g_rv_cur); s.pod(g_rv_phase); s.pod(g_rv_out); s.pod(g_rv_in_acc);
    s.pod(g_cd); s.pod(g_cd_r); s.pod(g_cd_w); s.pod(g_cd_started); s.pod(g_cd_hist); s.pod(g_cd_phase);
}
