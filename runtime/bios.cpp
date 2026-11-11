// High-level BIOS: A0/B0/C0 services, events, exception/IRQ dispatch.
#include "psx.h"
#include <setjmp.h>
#include <string>
#include "savestate.h"

#define A0 c->r[4]
#define A1 c->r[5]
#define A2 c->r[6]
#define A3 c->r[7]
#define V0 c->r[2]

// kernel memory layout used by the HLE
#define K_INTRP 0x80000E00u   // 4 chain heads (8 bytes each)
#define K_TCB 0x80000F00u
#define K_PCB 0x80000EF0u
#define K_B0TAB 0x80001000u
#define K_C0TAB 0x80001400u
#define K_A0TAB 0x80000200u
#define K_STACK 0x80007FF0u

static inline u32 rd32(u32 a) { return mem_r32(&g_ctx, a); }
static inline void wr32(u32 a, u32 v) { mem_w32(&g_ctx, a, v); }
static inline u8 rd8(u32 a) { return (u8)mem_r8(&g_ctx, a); }
static inline void wr8(u32 a, u8 v) { mem_w8(&g_ctx, a, v); }

static std::string gstr(u32 a, int max = 1024) {
    std::string s;
    if (!a) return s;
    for (int i = 0; i < max; i++) {
        u8 ch = rd8(a + i);
        if (!ch) break;
        s.push_back((char)ch);
    }
    return s;
}

int g_bios_log_from = 0;
// ---- events ----------------------------------------------------------
struct Event { u32 cls, spec, mode, func, status; };
#define MAX_EV 32
static Event g_ev[MAX_EV];
enum { EvStUNUSED = 0, EvStWAIT = 0x1000, EvStACTIVE = 0x2000, EvStALREADY = 0x4000 };

void bios_deliver_event(u32 cls, u32 spec) {
    Ctx* c = &g_ctx;
    for (int i = 0; i < MAX_EV; i++) {
        Event& e = g_ev[i];
        if (e.status == EvStACTIVE && e.cls == cls && e.spec == spec) {
            if (e.mode == 0x2000) e.status = EvStALREADY;
            else if (e.mode == 0x1000 && e.func) {
                Ctx save = *c;
                guest_call(c, e.func);
                s64 cyc = c->cyc, evt = c->evt;
                u32 sr = c->cop0[12];
                *c = save;
                c->cyc = cyc; c->evt = evt; c->cop0[12] = sr;
            }
        }
    }
}

// ---- exception state -------------------------------------------------
static u32 g_entry_int;       // HookEntryInt jmp_buf (guest)
static jmp_buf g_exc_jmp;
static bool g_exc_active;
static Ctx g_exc_saved;
static int g_rcnt_autoack[4] = {1, 1, 1, 1};
static int g_pad_autoack = 1;
static bool g_pad_started;
static u32 g_pad_buf[2], g_pad_len[2];
static u32 g_pad_old_buf;  // PAD_init style
static u32 g_heap_base, g_heap_size;
u32 g_rand_seed = 0;

static u32 g_intrp_head[4];  // host-side: games scribble over low kernel RAM

static void run_chain_handlers(Ctx* c) {
    for (int prio = 0; prio < 4; prio++) {
        u32 e = g_intrp_head[prio];
        int guard = 0;
        while (e && guard++ < 64) {
            u32 next = rd32(e), handler = rd32(e + 4), verifier = rd32(e + 8);
            if ((verifier && (verifier >> 20) != 0x800 && (verifier >> 20) != 0x801) || (handler && (handler >> 20) != 0x800 && (handler >> 20) != 0x801))
                logf("bad IntRP chain prio %d elem %08x: next=%08x handler=%08x verifier=%08x\n", prio, e, next, handler, verifier);
            if (verifier) {
                guest_call(c, verifier);
                if (V0 && handler) { A0 = V0; guest_call(c, handler); }
            }
            e = next;
        }
    }
}

void bios_exception_irq(Ctx* c) {
    g_exc_saved = *c;
    u32 sr = c->cop0[12];
    static int nexc; if (nexc++ < 20) LOGV("IRQ exception istat=%03x imask=%03x entry_int=%08x\n", g_istat, g_imask, g_entry_int);
    c->cop0[12] = (sr & ~0x3Fu) | ((sr & 0xF) << 2);
    c->cop0[13] = 0x400;
    c->cop0[14] = c->pc;
    g_exc_active = true;
    volatile int cs_depth = g_cs_n;
    CS_PUSH(0xEEEEEEEEu);
    if (setjmp(g_exc_jmp) == 0) {
        c->r[29] = K_STACK;
        u32 pend = g_istat & g_imask;
        if ((pend & 1) && g_pad_started) {
            for (int p = 0; p < 2; p++) {
                if (!g_pad_buf[p]) continue;
                u8 buf[34] = {0};
                bios_pad_poll(p, buf, 34);
                for (u32 i = 0; i < g_pad_len[p] && i < 34; i++) wr8(g_pad_buf[p] + i, buf[i]);
            }
            if (g_pad_old_buf) {
                u8 b0[34], b1[34];
                bios_pad_poll(0, b0, 34);
                bios_pad_poll(1, b1, 34);
                u32 v = ~((u32)(b0[2] << 8 | b0[3]) | ((u32)(b1[2] << 8 | b1[3]) << 16));
                wr32(g_pad_old_buf, v);
            }
            if (g_pad_autoack) g_istat &= ~1u;
        }
        static const int bits[4] = {4, 5, 6, 0};
        for (int i = 0; i < 4; i++) {
            if (g_istat & g_imask & (1u << bits[i])) {
                bios_deliver_event(0xF2000000u + i, 2);
                if (g_rcnt_autoack[i]) g_istat &= ~(1u << bits[i]);
            }
        }
        run_chain_handlers(c);
        if (g_entry_int) {
            u32 b = g_entry_int;
            c->r[31] = rd32(b);
            c->r[29] = rd32(b + 4);
            c->r[30] = rd32(b + 8);
            for (int i = 0; i < 8; i++) c->r[16 + i] = rd32(b + 12 + i * 4);
            c->r[28] = rd32(b + 44);
            V0 = 1;
            u32 pc = c->r[31];
            call_indirect(c, pc);   // normally leaves through ReturnFromException
        }
    }
    g_exc_active = false;
    g_cs_n = cs_depth;
    s64 cyc = c->cyc, evt = c->evt;
    u32 sr2 = c->cop0[12];
    *c = g_exc_saved;
    c->cyc = cyc;
    c->evt = evt;
    c->cop0[12] = (sr2 & ~0xFu) | ((sr2 >> 2) & 0xF);
}

// ---- printf ----------------------------------------------------------
static std::string g_tty;
static void tty_putc(char ch) {
    if (ch == '\n') { logf("[tty] %s\n", g_tty.c_str()); g_tty.clear(); }
    else if (ch != '\r') g_tty.push_back(ch);
}
static std::string format_guest(Ctx* c, u32 fmt_addr, int argi) {
    std::string out, fmt = gstr(fmt_addr, 4096);
    auto arg = [&]() -> u32 {
        u32 v = argi < 4 ? c->r[4 + argi] : rd32(c->r[29] + argi * 4);
        argi++;
        return v;
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && strchr("-+ #0123456789.lh*", fmt[i])) {
            if (fmt[i] == '*') spec += std::to_string((int)arg());
            else if (fmt[i] != 'l' && fmt[i] != 'h') spec.push_back(fmt[i]);
            i++;
        }
        if (i >= fmt.size()) break;
        char cv = fmt[i], buf[512];
        spec.push_back(cv);
        switch (cv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': case 'p':
            snprintf(buf, sizeof buf, spec.c_str(), arg());
            out += buf;
            break;
        case 's': {
            std::string s = gstr(arg(), 400);
            snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
            out += buf;
            break;
        }
        case 'f': case 'g': case 'e': argi += 2; out += "<f>"; break;
        case '%': out.push_back('%'); break;
        default: out += spec; break;
        }
    }
    return out;
}

// ---- memory card (file-level HLE) -----------------------------------
bool mc_open(Ctx* c, const std::string& name, u32 mode, u32* fd);
bool bios_file_call(Ctx* c, int fn);  // in memcard.cpp
void mc_card_info(int chan);

// ---- A0 ---------------------------------------------------------------
static void bios_std(Ctx* c, int n);

static bool bios_a0_(Ctx* c, u32 n);
static bool bios_a0(Ctx* c, u32 n) { return bios_a0_(c, n); }
static bool bios_a0_(Ctx* c, u32 n) {
    switch (n) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
        return bios_file_call(c, n);
    case 0x13:  // setjmp
        wr32(A0, c->r[31]); wr32(A0 + 4, c->r[29]); wr32(A0 + 8, c->r[30]);
        for (int i = 0; i < 8; i++) wr32(A0 + 12 + i * 4, c->r[16 + i]);
        wr32(A0 + 44, c->r[28]);
        V0 = 0;
        return true;
    case 0x14:
        psx_fatal("BIOS longjmp(%08x) not supported", A0);
        return true;
    case 0x2F: g_rand_seed = g_rand_seed * 0x41C64E6D + 0x3039; V0 = (g_rand_seed >> 16) & 0x7FFF; return true;
    case 0x30: g_rand_seed = A0; return true;
    case 0x39: g_heap_base = A0; g_heap_size = A1; return true;
    case 0x3B: V0 = (u32)-1; return true;
    case 0x3C: tty_putc((char)A0); return true;
    case 0x3E: { std::string s = gstr(A0); for (char ch : s) tty_putc(ch); tty_putc('\n'); return true; }
    case 0x3F: { std::string s = format_guest(c, A0, 1); for (char ch : s) tty_putc(ch); return true; }
    case 0x44: return true;  // FlushCache
    case 0x49: gpu_write_gp0(A0); return true;
    case 0x54: case 0x71: case 0x56: case 0x72: return true;  // CdInit/CdRemove
    case 0x55: case 0x70: return true;  // _bu_init
    case 0x96: case 0x97: case 0x99: case 0x98: return true;
    case 0x9F: return true;  // SetMem
    case 0xA1: psx_fatal("SystemError(%x,%x)", A0, A1); return true;
    case 0xAB: mc_card_info(A0); V0 = 1; return true;
    case 0xAC: mc_card_info(A0); V0 = 1; return true;   // _card_load
    case 0xAD: return true;
    case 0xA2: case 0xA3: return true;
    case 0xB4: V0 = 0; return true;
    default:
        if (n >= 0x0A && n <= 0x38) { bios_std(c, n); return true; }
        return false;
    }
}

// libc subset
static void bios_std(Ctx* c, int n) {
    u32 a0 = A0, a1 = A1, a2 = A2;
    switch (n) {
    case 0x0E: case 0x0F: V0 = (u32)abs((s32)a0); break;
    case 0x10: case 0x11: V0 = (u32)atoi(gstr(a0).c_str()); break;
    case 0x15: {  // strcat
        u32 d = a0;
        while (rd8(d)) d++;
        u8 ch;
        do { ch = rd8(a1++); wr8(d++, ch); } while (ch);
        V0 = a0;
        break;
    }
    case 0x17: case 0x18: {  // strcmp / strncmp
        u32 lim = n == 0x18 ? a2 : 0xFFFFFFFF;
        s32 res = 0;
        for (u32 i = 0; i < lim; i++) {
            u8 x = rd8(a0 + i), y = rd8(a1 + i);
            if (x != y) { res = (s32)x - (s32)y; break; }
            if (!x) break;
        }
        V0 = (u32)res;
        break;
    }
    case 0x19: { u32 d = a0; u8 ch; do { ch = rd8(a1++); wr8(d++, ch); } while (ch); V0 = a0; break; }
    case 0x1A: {
        u32 i = 0;
        for (; i < a2; i++) { u8 ch = rd8(a1 + i); wr8(a0 + i, ch); if (!ch) break; }
        for (; i < a2; i++) wr8(a0 + i, 0);
        V0 = a0;
        break;
    }
    case 0x1B: { u32 l = 0; if (a0) while (rd8(a0 + l)) l++; V0 = l; break; }
    case 0x1C: case 0x1E: {  // index / strchr
        V0 = 0;
        for (u32 p = a0;; p++) { u8 ch = rd8(p); if (ch == (u8)a1) { V0 = p; break; } if (!ch) break; }
        break;
    }
    case 0x1D: case 0x1F: {
        V0 = 0;
        for (u32 p = a0;; p++) { u8 ch = rd8(p); if (ch == (u8)a1) V0 = p; if (!ch) break; }
        break;
    }
    case 0x25: V0 = (u32)toupper((int)a0); break;
    case 0x26: V0 = (u32)tolower((int)a0); break;
    case 0x27: for (u32 i = 0; i < a2; i++) wr8(a1 + i, rd8(a0 + i)); break;  // bcopy(src,dst,len)
    case 0x28: for (u32 i = 0; i < a1; i++) wr8(a0 + i, 0); break;
    case 0x29: { V0 = 0; for (u32 i = 0; i < a2; i++) { int d = rd8(a0 + i) - rd8(a1 + i); if (d) { V0 = (u32)d; break; } } break; }
    case 0x2A: for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i)); V0 = a0; break;
    case 0x2B: for (u32 i = 0; i < a2; i++) wr8(a0 + i, (u8)a1); V0 = a0; break;
    case 0x2C:
        if (a0 <= a1) for (u32 i = 0; i < a2; i++) wr8(a0 + i, rd8(a1 + i));
        else for (u32 i = a2; i-- > 0;) wr8(a0 + i, rd8(a1 + i));
        V0 = a0;
        break;
    case 0x2D: { V0 = 0; for (u32 i = 0; i < a2; i++) { int d = rd8(a0 + i) - rd8(a1 + i); if (d) { V0 = (u32)d; break; } } break; }
    case 0x2E: { V0 = 0; for (u32 i = 0; i < a2; i++) if (rd8(a0 + i) == (u8)a1) { V0 = a0 + i; break; } break; }
    default:
        psx_fatal("BIOS A0:%02x (libc) not implemented", n);
    }
}

// ---- B0 ---------------------------------------------------------------
static bool bios_b0(Ctx* c, u32 n) {
    switch (n) {
    case 0x00: V0 = 0; return true;
    case 0x02: {  // init_timer(t, reload, flags)
        u32 t = A0 & 3;
        if (t < 3) {
            io_write(0x1F801108 + t * 16, A1 & 0xFFFF, 4);
            u32 mode = 0x48 | ((A2 & 0x1000) ? 0x10 : 0) | ((A2 & 0x10) ? 0 : 0x100) | ((A2 & 1) ? 0 : 0x200);
            // PCSX mapping: flags 0x1000 irq, 0x0100 repeat, 0x0010/0x0001 clock source
            mode = 0x48;
            if (A2 & 0x1000) mode |= 0x10;
            if (A2 & 0x0100) mode |= 0x40; else mode |= 0x80;
            if (A2 & 0x0010) mode |= 0x001;
            if (t == 2) { if (A2 & 0x0001) mode |= 0x200; } else { if (A2 & 0x0001) mode |= 0x100; }
            io_write(0x1F801104 + t * 16, mode, 4);
            V0 = 1;
        } else V0 = 0;
        return true;
    }
    case 0x03: V0 = (A0 & 3) < 3 ? io_read(0x1F801100 + (A0 & 3) * 16, 4) & 0xFFFF : 0; return true;
    case 0x04: { u32 t = A0 & 3; g_imask |= t == 3 ? 1 : (1u << (4 + t)); V0 = 1; sched_force_check(); return true; }
    case 0x05: { u32 t = A0 & 3; g_imask &= ~(t == 3 ? 1u : (1u << (4 + t))); V0 = 1; return true; }
    case 0x06: { u32 t = A0 & 3; if (t < 3) io_write(0x1F801100 + t * 16, 0, 4); V0 = 1; return true; }
    case 0x07: bios_deliver_event(A0, A1); return true;
    case 0x08: {
        for (int i = 0; i < MAX_EV; i++)
            if (g_ev[i].status == EvStUNUSED) {
                g_ev[i] = {A0, A1, A2, A3, EvStWAIT};
                V0 = 0xF1000000u | i;
                LOGV("OpenEvent(%08x,%x,%x,%08x) = %d\n", A0, A1, A2, A3, i);
                return true;
            }
        V0 = (u32)-1;
        return true;
    }
    case 0x09: g_ev[A0 & (MAX_EV - 1)].status = EvStUNUSED; V0 = 1; return true;
    case 0x0A: {  // WaitEvent
        Event& e = g_ev[A0 & (MAX_EV - 1)];
        if (e.status == EvStUNUSED || e.status == EvStWAIT) { V0 = 0; return true; }
        int guard = 0;
        while (e.status != EvStALREADY && e.mode == 0x2000) {
            c->cyc += 256;
            psx_events(c);
            if (++guard > 4000000) { logf("WaitEvent(%x) timeout\n", A0); break; }
        }
        if (e.status == EvStALREADY) e.status = EvStACTIVE;
        V0 = 1;
        return true;
    }
    case 0x0B: {
        Event& e = g_ev[A0 & (MAX_EV - 1)];
        if (e.status == EvStALREADY) { e.status = EvStACTIVE; V0 = 1; } else V0 = 0;
        return true;
    }
    case 0x0C: { Event& e = g_ev[A0 & (MAX_EV - 1)]; if (e.status != EvStUNUSED) e.status = EvStACTIVE; V0 = 1; return true; }
    case 0x0D: { Event& e = g_ev[A0 & (MAX_EV - 1)]; if (e.status != EvStUNUSED) e.status = EvStWAIT; V0 = 1; return true; }
    case 0x0E: psx_fatal("OpenTh not supported"); return true;
    case 0x0F: V0 = 1; return true;
    case 0x10: psx_fatal("ChangeTh not supported"); return true;
    case 0x12:  // InitPAD
        g_pad_buf[0] = A0; g_pad_len[0] = A1; g_pad_buf[1] = A2; g_pad_len[1] = A3;
        V0 = 2;
        return true;
    case 0x13: g_pad_started = true; g_imask |= 1; V0 = 2; sched_force_check(); return true;
    case 0x14: g_pad_started = false; return true;
    case 0x15: g_pad_old_buf = A1; g_pad_started = true; g_imask |= 1; V0 = 2; return true;  // PAD_init
    case 0x16: { u8 b[34]; bios_pad_poll(0, b, 34); V0 = (u32)(b[2] | (b[3] << 8)); return true; }
    case 0x17:  // ReturnFromException
        if (!g_exc_active) psx_fatal("ReturnFromException outside exception");
        longjmp(g_exc_jmp, 1);
    case 0x18: g_entry_int = 0; return true;
    case 0x19: g_entry_int = A0; LOGV("HookEntryInt(%08x) ra=%08x\n", A0, rd32(A0)); return true;
    case 0x20: {
        for (int i = 0; i < MAX_EV; i++)
            if (g_ev[i].status == EvStALREADY && g_ev[i].cls == A0 && g_ev[i].spec == A1 && g_ev[i].mode == 0x2000)
                g_ev[i].status = EvStACTIVE;
        return true;
    }
    case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
    case 0x42: case 0x43: case 0x44: case 0x45: case 0x41:
        return bios_file_call(c, 0x100 + n);
    case 0x3D: tty_putc((char)A0); return true;
    case 0x3F: { std::string s = gstr(A0); for (char ch : s) tty_putc(ch); return true; }
    case 0x4A: case 0x4B: case 0x4C: V0 = 1; return true;  // Init/Start/StopCARD
    case 0x4E: case 0x4F: {   // _card_write / _card_read(chan, sector, buf)
        void mc_sector(int chan, u32 sector, u32 buf, bool write);
        mc_sector((int)A0, A1, A2, n == 0x4E);
        V0 = 1;
        return true;
    }
    case 0x50: V0 = 0; return true;
    case 0x51: V0 = (u32)-1; return true;
    case 0x54: V0 = 0; return true;
    case 0x56: V0 = K_C0TAB; return true;
    case 0x57: V0 = K_B0TAB; return true;
    case 0x58: V0 = 0; return true;
    case 0x5B: g_pad_autoack = A0; return true;  // ChangeClearPad(int)
    case 0x5C: V0 = 1; return true;   // _card_status: ready
    case 0x5D: V0 = 1; return true;
    default: return false;
    }
}

// ---- C0 ---------------------------------------------------------------
static bool bios_c0(Ctx* c, u32 n) {
    switch (n) {
    case 0x00: case 0x01: case 0x0C: case 0x07: case 0x08: case 0x09: case 0x12: case 0x1C: case 0x0D:
        return true;
    case 0x02: {  // SysEnqIntRP(prio, struct)
        u32& head = g_intrp_head[A0 & 3];
        wr32(A1, head);
        head = A1;
        V0 = 0;
        LOGV("SysEnqIntRP(%d, %08x) verifier=%08x handler=%08x\n", A0, A1, rd32(A1 + 8), rd32(A1 + 4));
        return true;
    }
    case 0x03: {  // SysDeqIntRP
        u32& head = g_intrp_head[A0 & 3];
        V0 = 0;
        if (head == A1) { head = rd32(A1); V0 = A1; return true; }
        u32 e = head;
        int guard = 0;
        while (e && guard++ < 64) {
            u32 next = rd32(e);
            if (next == A1) { wr32(e, rd32(A1)); V0 = A1; break; }
            e = next;
        }
        return true;
    }
    case 0x0A: { u32 t = A0 & 3; V0 = g_rcnt_autoack[t]; g_rcnt_autoack[t] = A1; return true; }
    default: return false;
    }
}

bool bios_call(Ctx* c, u32 addr) {
    u32 n = c->r[9] & 0xFF;
    {
        static int ncalls;
        if (g_log_level >= 2 && ncalls < 400000 && g_frame > g_bios_log_from && !(addr == 0xA0 && (n == 0x3F || n == 0x3C)) && !(addr == 0xB0 && (n == 0x35 || n == 0x3D || n == 0x17))) {
            ncalls++;
            logf("BIOS %c0:%02x(%08x,%08x,%08x) ra=%08x\n", addr == 0xA0 ? 'A' : addr == 0xB0 ? 'B' : 'C', n, A0, A1, A2, c->r[31]);
        }
    }
    bool ok = addr == 0xA0 ? bios_a0(c, n) : addr == 0xB0 ? bios_b0(c, n) : bios_c0(c, n);
    if (!ok) {
        static u8 warned[3][256];
        int t = addr == 0xA0 ? 0 : addr == 0xB0 ? 1 : 2;
        if (!warned[t][n]) {
            warned[t][n] = 1;
            logf("BIOS %c0:%02x unimplemented (a0=%08x a1=%08x a2=%08x ra=%08x)\n", "ABC"[t], n, A0, A1, A2, c->r[31]);
        }
        V0 = 0;
    }
    return true;
}

void bios_vblank_hook() {}

void bios_init() {
    // jump-table shells so library patchers that peek at kernel tables find plausible memory
    for (u32 i = 0; i < 0x100; i++) {
        wr32(K_A0TAB + i * 4, 0x80000000u + 0x2000 + i * 16);
        wr32(K_B0TAB + i * 4, 0x80000000u + 0x3000 + i * 16);
    }
    for (u32 i = 0; i < 0x20; i++) wr32(K_C0TAB + i * 4, 0x80000000u + 0x4000 + i * 16);
    for (u32 a = 0x2000; a < 0x5000; a += 8) { wr32(0x80000000u + a, 0x03E00008); wr32(0x80000004u + a, 0); }
    // device table: libmcrd looks up the "bu" device and temporarily swaps its firstfile handler
    wr32(0x80000150, 0x80006000);
    wr32(0x80000154, 0x50 * 2);
    wr32(0x80006000, 0x80006100);
    wr8(0x80006100, 'b'); wr8(0x80006101, 'u'); wr8(0x80006102, 0);
    for (u32 o = 0x10; o < 0x50; o += 4) wr32(0x80006000 + o, 0x80002000);
    wr32(0x80000100, K_INTRP);
    wr32(0x80000104, 0x20);
    wr32(0x80000108, K_PCB);
    wr32(K_PCB, K_TCB);
    wr32(0x80000110, K_TCB);
    wr32(0x80000114, 0xC0 * 4);
    wr32(K_TCB, 0x4000);
    wr32(0x80000060, 0x00000002);
    wr32(0x80000064, 0x00000000);
    wr32(0x80000068, 0x000000FF);
}

// ---- save states ---------------------------------------------------------------------------
void bios_state(StateIO& s) {
    s.tag("BIOS");
    s.pod(g_ev);
    s.pod(g_entry_int);
    s.pod(g_exc_jmp); s.pod(g_exc_active); s.pod(g_exc_saved);
    s.pod(g_rcnt_autoack); s.pod(g_pad_autoack); s.pod(g_pad_started);
    s.pod(g_pad_buf); s.pod(g_pad_len); s.pod(g_pad_old_buf);
    s.pod(g_heap_base); s.pod(g_heap_size);
    s.pod(g_rand_seed);
    s.pod(g_intrp_head);
}
