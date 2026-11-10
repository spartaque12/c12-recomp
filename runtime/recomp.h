/* Shared between generated code (C) and the runtime (C++). */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#define RAM_SIZE 0x200000u

/* PGXP shadow value: a precise screen-space vertex travelling with a 32-bit word */
typedef struct PgxpVal {
    float x, y, w;
    u32 val;   /* the integer word this precision belongs to; mismatch = stale */
    float ox, oy;  /* where the same model vertex was projected in the previous game frame */
    u32 has_old;
    u32 pad;
} PgxpVal;

typedef struct Ctx {
    u32 r[32];
    u32 hi, lo;
    u32 pc;
    u32 pad0;
    s64 cyc;          /* executed instructions */
    s64 evt;          /* instruction count at which psx_events must run */
    u32 cop0[32];
    /* GTE */
    s32 gd[32];       /* data regs (raw storage) */
    s32 gc[32];       /* control regs */
    /* PGXP */
    PgxpVal preg[32]; /* per CPU register */
    PgxpVal pgte[4];  /* SXY0..2 + spare */
    /* frame interpolation: where each CPU register / GTE input vector was loaded from */
    u32 rsrc[32], rsrc_val[32];
    u32 vsrc[3];
} Ctx;

typedef void (*RecompFn)(Ctx*);
typedef void (*HookFn)(Ctx*, RecompFn orig);
typedef struct RecompFunc {
    u32 addr;
    RecompFn fn;
} RecompFunc;

/* relocatable code overlay recompiled at link address 0; `base` is set when the game loads it */
typedef struct OverlayDesc {
    const char* name;
    u32 size;
    u32 checksum;            /* sum of the image words before relocation */
    const RecompFunc* funcs; /* addr = offset inside the image */
    unsigned count;
    u32* base;
} OverlayDesc;

extern u8 g_ram[RAM_SIZE + 16];
extern PgxpVal* g_pgxp_mem;  /* RAM_SIZE/4 entries */
extern int g_pgxp_on;

u32 mem_r8_slow(Ctx* c, u32 a);
u32 mem_r16_slow(Ctx* c, u32 a);
u32 mem_r32_slow(Ctx* c, u32 a);
void mem_w8_slow(Ctx* c, u32 a, u32 v);
void mem_w16_slow(Ctx* c, u32 a, u32 v);
void mem_w32_slow(Ctx* c, u32 a, u32 v);

void psx_events(Ctx* c);
void call_indirect(Ctx* c, u32 addr);  /* guest call: ra already set */
void jump_out(Ctx* c);                 /* tail-jump to c->pc, returns when that code returns */
void psx_syscall(Ctx* c, u32 pc);
void psx_break(Ctx* c, u32 pc);
void psx_invalid(Ctx* c, u32 pc, u32 w);
u32 cop0_read(Ctx* c, int reg);
void cop0_write(Ctx* c, int reg, u32 v);
void cop0_rfe(Ctx* c);
u32 gte_mfc2(Ctx* c, int reg);
u32 gte_cfc2(Ctx* c, int reg);
void gte_mtc2(Ctx* c, int reg, u32 v);
void gte_ctc2(Ctx* c, int reg, u32 v);
void gte_op(Ctx* c, u32 op);

#define PHYS(a) ((a) & 0x1FFFFFFFu)

static inline u32 mem_r8(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) return g_ram[p];
    return mem_r8_slow(c, a);
}
static inline u32 mem_r16(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) { u16 v; memcpy(&v, g_ram + p, 2); return v; }
    return mem_r16_slow(c, a);
}
static inline u32 mem_r32(Ctx* c, u32 a) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) { u32 v; memcpy(&v, g_ram + p, 4); return v; }
    return mem_r32_slow(c, a);
}
static inline void mem_w8(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) { g_ram[p] = (u8)v; return; }
    mem_w8_slow(c, a, v);
}
static inline void mem_w16(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) { u16 t = (u16)v; memcpy(g_ram + p, &t, 2); return; }
    mem_w16_slow(c, a, v);
}
static inline void mem_w32(Ctx* c, u32 a, u32 v) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) { memcpy(g_ram + p, &v, 4); return; }
    mem_w32_slow(c, a, v);
}

static inline void op_lwl(Ctx* c, int rt, u32 a) {
    u32 m = mem_r32(c, a & ~3u);
    u32 sh = (a & 3) * 8;
    c->r[rt] = (c->r[rt] & (0x00FFFFFFu >> sh)) | (m << (24 - sh));
}
static inline void op_lwr(Ctx* c, int rt, u32 a) {
    u32 m = mem_r32(c, a & ~3u);
    u32 sh = (a & 3) * 8;
    u32 mask = sh ? (0xFFFFFFFFu << (32 - sh)) : 0;
    c->r[rt] = (c->r[rt] & mask) | (m >> sh);
}
static inline void op_swl(Ctx* c, int rt, u32 a) {
    u32 m = mem_r32(c, a & ~3u);
    u32 sh = (a & 3) * 8;
    u32 mask = sh == 24 ? 0 : (0xFFFFFF00u << sh);
    mem_w32(c, a & ~3u, (m & mask) | (c->r[rt] >> (24 - sh)));
}
static inline void op_swr(Ctx* c, int rt, u32 a) {
    u32 m = mem_r32(c, a & ~3u);
    u32 sh = (a & 3) * 8;
    u32 mask = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
    mem_w32(c, a & ~3u, (m & mask) | (c->r[rt] << sh));
}
static inline void op_div(Ctx* c, u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) { c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; c->hi = a; }
    else if (a == 0x80000000u && d == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (u32)(n / d); c->hi = (u32)(n % d); }
}
static inline void op_divu(Ctx* c, u32 a, u32 b) {
    if (b == 0) { c->lo = 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}

/* PGXP propagation: precision rides along with whole-word loads/stores. */
extern PgxpVal g_pgxp_scratch[0x100];  /* scratchpad 1F800000..1F8003FF */
static inline PgxpVal* pgxp_slot(u32 a) {
    u32 p = PHYS(a);
    if (p < RAM_SIZE) return &g_pgxp_mem[p >> 2];
    if (p - 0x1F800000u < 0x400u) return &g_pgxp_scratch[(p >> 2) & 0xFF];
    return 0;
}
static inline void pgxp_lw(Ctx* c, int rt, u32 a) {
    PgxpVal* s = pgxp_slot(a);
    if (s) c->preg[rt] = *s;
    else c->preg[rt].val = ~c->r[rt];
    c->rsrc[rt] = a;
    c->rsrc_val[rt] = c->r[rt];
}
static inline void pgxp_sw(Ctx* c, int rt, u32 a) {
    PgxpVal* s = pgxp_slot(a);
    if (s) *s = c->preg[rt];
}
void pgxp_mfc2(Ctx* c, int rt, int reg);
void pgxp_mtc2(Ctx* c, int rt, int reg);
void pgxp_lwc2(Ctx* c, int reg, u32 a);
void pgxp_swc2(Ctx* c, int reg, u32 a);

extern u32* g_cs;
extern int g_cs_n;
#define CS_PUSH(a) (g_cs[g_cs_n++ & 4095] = (a))
#define CS_POP() (g_cs_n--)

#define R(n) (c->r[n])
#define ADV(n) (c->cyc += (n))
#define CHK() do { if (c->cyc >= c->evt) psx_events(c); } while (0)
#define CHKB() CHK()
#define MULT(a, b) do { s64 t_ = (s64)(s32)(a) * (s64)(s32)(b); c->lo = (u32)t_; c->hi = (u32)((u64)t_ >> 32); } while (0)
#define MULTU(a, b) do { u64 t_ = (u64)(a) * (u64)(b); c->lo = (u32)t_; c->hi = (u32)(t_ >> 32); } while (0)
#define DIV(a, b) op_div(c, (a), (b))
#define DIVU(a, b) op_divu(c, (a), (b))
#define LB(rt, a) (R(rt) = (u32)(s32)(s8)mem_r8(c, (a)))
#define LBU(rt, a) (R(rt) = mem_r8(c, (a)))
#define LH(rt, a) (R(rt) = (u32)(s32)(s16)mem_r16(c, (a)))
#define LHU(rt, a) (R(rt) = mem_r16(c, (a)))
#define LW(rt, a) do { u32 a_ = (a); R(rt) = mem_r32(c, a_); pgxp_lw(c, rt, a_); } while (0)
#define SB(rt, a) mem_w8(c, (a), c->r[rt])
#define SH(rt, a) mem_w16(c, (a), c->r[rt])
#define SW(rt, a) do { u32 a_ = (a); mem_w32(c, a_, c->r[rt]); pgxp_sw(c, rt, a_); } while (0)
#define LWL(rt, a) op_lwl(c, rt, (a))
#define LWR(rt, a) op_lwr(c, rt, (a))
#define SWL(rt, a) op_swl(c, rt, (a))
#define SWR(rt, a) op_swr(c, rt, (a))
#define MFC2(rt, rd) do { R(rt) = gte_mfc2(c, rd); pgxp_mfc2(c, rt, rd); } while (0)
#define MTC2(rt, rd) do { gte_mtc2(c, rd, c->r[rt]); pgxp_mtc2(c, rt, rd); } while (0)
#define LWC2(rt, a) do { u32 a_ = (a); gte_mtc2(c, rt, mem_r32(c, a_)); pgxp_lwc2(c, rt, a_); } while (0)
#define SWC2(rt, a) do { u32 a_ = (a); mem_w32(c, a_, gte_mfc2(c, rt)); pgxp_swc2(c, rt, a_); } while (0)

#ifdef __cplusplus
}
#endif
