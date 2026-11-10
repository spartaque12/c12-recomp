"""Static recompiler: PSX MIPS R3000A image -> C.

usage: recomp.py <config.json>

Every discovered function becomes `void f_<name>_<addr>(Ctx*)`. A function is
the closure of blocks reachable from its entry through local control flow, so
odd layouts (shared tails, fallthrough) just duplicate code instead of
breaking. Anything not statically reachable is handled at runtime by the
interpreter fallback in the runtime.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mips import Ins  # noqa: E402

JR_RA = 0x03E00008


class Image:
    def __init__(self, name, base, data, relocs=None):
        self.name = name
        self.base = base
        self.data = data
        self.end = base + len(data)
        self.relocs = relocs or {}      # overlay: offset -> (type, addend)
        self.overlay = relocs is not None
        n = len(data) // 4
        self.words = struct.unpack_from("<%dI" % n, data)

    def has(self, a):
        return self.base <= a < self.end and not (a & 3)

    def w(self, a):
        return self.words[(a - self.base) >> 2]

    def ins(self, a):
        i = Ins(self.w(a), a)
        if self.overlay and i.op in (2, 3):
            # relocated jumps stay inside the image (offset space); the rest are absolute
            off = (i.w & 0x3FFFFFF) << 2
            r = self.relocs.get(a)
            i.tgt = off if (r and r[0] == 3) else (0x80000000 | off)
        return i

    def lui_value(self, i):
        r = self.relocs.get(i.pc)
        if r and r[0] == 1:
            return (r[1] + 0x8000) & 0xFFFF0000
        if self.overlay:
            return None  # absolute constant, never an in-image address
        return (i.imm << 16) & 0xFFFFFFFF


class Analyzer:
    def __init__(self, img):
        self.img = img
        self.seeds = set()        # function entries
        self.jt = {}              # jr pc -> list of targets
        self.jt_tables = {}       # table addr -> count
        self.funcs = {}           # entry -> sorted list of pcs (standalone emitted)
        self.cover = set()

    # ---- jump tables ---------------------------------------------------
    def resolve_jr(self, pc):
        if pc in self.jt:
            return self.jt[pc]
        img = self.img
        best = None
        for back in (20, 12, 8):
            st = pc - 4 * back
            if not img.has(st):
                continue
            const = {}
            based = {}
            tload = {}
            bound = None
            a = st
            while a < pc:
                i = img.ins(a)
                op = i.op

                def kill(r):
                    const.pop(r, None)
                    based.pop(r, None)
                    tload.pop(r, None)
                if op == 15:
                    kill(i.rt)
                    lv = img.lui_value(i)
                    if lv is not None:
                        const[i.rt] = lv
                elif op in (8, 9):
                    if i.rs in const:
                        v = (const[i.rs] + i.simm) & 0xFFFFFFFF
                        kill(i.rt)
                        const[i.rt] = v
                    elif i.rs in based:
                        v = (based[i.rs] + i.simm) & 0xFFFFFFFF
                        kill(i.rt)
                        based[i.rt] = v
                    else:
                        kill(i.rt)
                elif op == 13 and i.rs in const:
                    v = const[i.rs] | i.imm
                    kill(i.rt)
                    const[i.rt] = v
                elif op == 0 and i.fn == 33:
                    v = None
                    if i.rs in const and i.rt not in const:
                        v = const[i.rs]
                    elif i.rt in const and i.rs not in const:
                        v = const[i.rt]
                    kill(i.rd)
                    if v is not None:
                        based[i.rd] = v
                elif op == 35:
                    v = None
                    if i.rs in based:
                        v = (based[i.rs] + i.simm) & 0xFFFFFFFF
                    kill(i.rt)
                    if v is not None:
                        tload[i.rt] = v
                elif op == 11:
                    bound = i.imm
                    kill(i.rt)
                elif op == 0 and i.fn in (0, 2, 3) or op == 0 and i.fn >= 32:
                    if i.w != 0:
                        kill(i.rd)
                elif op in (10, 12, 14, 32, 33, 36, 37, 34, 38):
                    kill(i.rt)
                a += 4
            j = img.ins(pc)
            if j.rs in tload:
                best = (tload[j.rs], bound)
                break
        if not best:
            return None
        tbl, bound = best
        if not img.has(tbl):
            return None
        tg = []
        a = tbl
        lim = bound if bound else 512
        while len(tg) < lim and img.has(a):
            v = img.w(a)
            if not img.has(v) or abs(v - pc) > 0x8000:
                break
            if a != tbl and a in self.jt_tables:
                break
            tg.append(v)
            a += 4
        if not tg:
            return None
        self.jt[pc] = tg
        self.jt_tables[tbl] = len(tg)
        return tg

    # ---- CFG closure ---------------------------------------------------
    def closure(self, entry, collect_calls=None, stop_at_seeds=False):
        """returns (set of standalone pcs, set of leader pcs, ok)"""
        img = self.img
        seen = set()
        leaders = {entry}
        work = [entry]
        ok = True
        while work:
            pc = work.pop()
            while True:
                if pc in seen:
                    break
                if not img.has(pc):
                    ok = False
                    break
                i = img.ins(pc)
                seen.add(pc)
                if not i.valid():
                    ok = False
                    break
                if not i.has_delay():
                    pc += 4
                    continue
                # control transfer; delay slot at pc+4 is emitted inline
                if not img.has(pc + 4):
                    ok = False
                    break
                d = img.ins(pc + 4)
                if d.has_delay() or not d.valid():
                    ok = False
                if i.is_branch():
                    t = i.btarget()
                    if i.is_link_branch():
                        if collect_calls is not None and img.has(t):
                            collect_calls.add(t)
                        leaders.add(pc + 8)
                        pc += 8
                        continue
                    leaders.add(t)
                    work.append(t)
                    if i.is_uncond_branch():
                        break
                    leaders.add(pc + 8)
                    pc += 8
                    continue
                if i.is_jal():
                    if collect_calls is not None and img.has(i.tgt):
                        collect_calls.add(i.tgt)
                    leaders.add(pc + 8)
                    pc += 8
                    continue
                if i.is_jalr():
                    leaders.add(pc + 8)
                    pc += 8
                    continue
                if i.is_j():
                    t = i.tgt
                    if img.has(t) and not (stop_at_seeds and t in self.seeds and t != entry):
                        leaders.add(t)
                        work.append(t)
                    break
                if i.is_jr():
                    if i.rs != 31:
                        tg = self.resolve_jr(pc)
                        if tg:
                            for t in tg:
                                leaders.add(t)
                                work.append(t)
                    break
                break
        return seen, leaders, ok

    def looks_like_func(self, a):
        img = self.img
        if not img.has(a) or a in self.seeds:
            return False
        if a in self.cover and a not in self.seeds:
            # inside known code: only accept if previous insn ends a function
            pass
        i = img.ins(a)
        prologue = i.op == 9 and i.rs == 29 and i.rt == 29 and i.simm < 0
        after_ret = img.has(a - 8) and img.w(a - 8) == JR_RA
        if not (prologue or after_ret):
            return False
        seen, _, ok = self.closure(a)
        if not ok or len(seen) < 2:
            return False
        # must contain a return
        return any(img.w(p) == JR_RA for p in seen)

    def discover(self, entries, extra=()):
        img = self.img
        self.seeds = set(e for e in entries if img.has(e))
        self.extra = set(e for e in extra if img.has(e))
        done = set()
        while True:
            todo = (self.seeds | self.extra) - done
            if not todo:
                # pointer scans once code-reachability converged
                new = set()
                for a in range(img.base, img.end - 4, 4):
                    if a in self.cover:
                        i = img.ins(a)
                        if i.op == 15 and img.overlay:
                            r = img.relocs.get(a)
                            if r and r[0] == 1:
                                v = r[1]
                                if img.has(v) and v not in self.cover and self.looks_like_func(v):
                                    new.add(v)
                                elif img.has(v) and v in self.cover and v not in self.seeds                                         and img.has(v - 8) and img.w(v - 8) == JR_RA:
                                    new.add(v)
                        elif i.op == 15:  # lui + addiu/ori within 4 insns
                            for k in range(1, 5):
                                if not img.has(a + 4 * k):
                                    break
                                n = img.ins(a + 4 * k)
                                if n.op in (9, 13) and n.rs == i.rt and n.rt == i.rt:
                                    v = ((i.imm << 16) + (n.simm if n.op == 9 else n.imm)) & 0xFFFFFFFF
                                    if img.has(v) and v not in self.cover and self.looks_like_func(v):
                                        new.add(v)
                                    elif img.has(v) and v in self.cover and v not in self.seeds \
                                            and img.has(v - 8) and img.w(v - 8) == JR_RA:
                                        new.add(v)
                                    break
                                if n.has_delay():
                                    break
                    else:
                        v = img.w(a)
                        if img.overlay and img.relocs.get(a, (9,))[0] != 0:
                            continue
                        if img.has(v) and v not in self.seeds:
                            if v in self.cover:
                                if img.has(v - 8) and img.w(v - 8) == JR_RA:
                                    new.add(v)
                            elif self.looks_like_func(v):
                                new.add(v)
                new -= self.seeds
                if not new:
                    break
                self.seeds |= new
                continue
            for e in sorted(todo):
                calls = set()
                seen, _, _ = self.closure(e, calls)
                self.cover |= seen
                for p in list(seen):
                    if img.ins(p).has_delay():
                        self.cover.add(p + 4)
                self.seeds |= calls
                done.add(e)

    def build(self):
        for e in sorted(self.seeds | self.extra):
            seen, leaders, ok = self.closure(e, None, stop_at_seeds=True)
            self.funcs[e] = (seen, leaders)


# ------------------------------------------------------------------------
class Emitter:
    """C emitter. For overlays every code address is emitted relative to the load base `B`."""

    def __init__(self, an, prefix, main_funcs=None):
        self.an = an
        self.img = an.img
        self.prefix = prefix
        self.ovl = an.img.overlay
        self.main_funcs = main_funcs or set()

    def fname(self, a):
        return "f_%s_%08x" % (self.prefix, a)

    def K(self, a):
        """C expression for a code address inside this image"""
        return "(B + 0x%xu)" % a if self.ovl else "0x%08xu" % a

    def simple(self, i):
        """C statement for a non-control instruction"""
        op, rs, rt, rd, sh, fn = i.op, i.rs, i.rt, i.rd, i.sh, i.fn
        R = lambda n: "0u" if n == 0 else "R(%d)" % n
        rl = self.img.relocs.get(i.pc) if self.ovl else None
        lo_reloc = rl is not None and rl[0] == 2

        def setr(n, expr):
            if n == 0:
                return "(void)(%s);" % expr
            return "R(%d) = %s;" % (n, expr)
        if not i.valid():
            return "psx_invalid(c, %s, 0x%08xu);" % (self.K(i.pc), i.w)
        if op == 0:
            if i.w == 0:
                return ";"
            if fn == 0:
                return setr(rd, "%s << %d" % (R(rt), sh))
            if fn == 2:
                return setr(rd, "%s >> %d" % (R(rt), sh))
            if fn == 3:
                return setr(rd, "(u32)((s32)%s >> %d)" % (R(rt), sh))
            if fn == 4:
                return setr(rd, "%s << (%s & 31)" % (R(rt), R(rs)))
            if fn == 6:
                return setr(rd, "%s >> (%s & 31)" % (R(rt), R(rs)))
            if fn == 7:
                return setr(rd, "(u32)((s32)%s >> (%s & 31))" % (R(rt), R(rs)))
            if fn == 12:
                return "psx_syscall(c, %s);" % self.K(i.pc)
            if fn == 13:
                return "psx_break(c, %s);" % self.K(i.pc)
            if fn == 16:
                return setr(rd, "c->hi")
            if fn == 17:
                return "c->hi = %s;" % R(rs)
            if fn == 18:
                return setr(rd, "c->lo")
            if fn == 19:
                return "c->lo = %s;" % R(rs)
            if fn == 24:
                return "MULT(%s, %s);" % (R(rs), R(rt))
            if fn == 25:
                return "MULTU(%s, %s);" % (R(rs), R(rt))
            if fn == 26:
                return "DIV(%s, %s);" % (R(rs), R(rt))
            if fn == 27:
                return "DIVU(%s, %s);" % (R(rs), R(rt))
            if fn in (32, 33):
                if rt == 0:
                    return setr(rd, R(rs))
                if rs == 0:
                    return setr(rd, R(rt))
                return setr(rd, "%s + %s" % (R(rs), R(rt)))
            if fn in (34, 35):
                return setr(rd, "%s - %s" % (R(rs), R(rt)))
            if fn == 36:
                return setr(rd, "%s & %s" % (R(rs), R(rt)))
            if fn == 37:
                if rt == 0:
                    return setr(rd, R(rs))
                if rs == 0:
                    return setr(rd, R(rt))
                return setr(rd, "%s | %s" % (R(rs), R(rt)))
            if fn == 38:
                return setr(rd, "%s ^ %s" % (R(rs), R(rt)))
            if fn == 39:
                return setr(rd, "~(%s | %s)" % (R(rs), R(rt)))
            if fn == 42:
                return setr(rd, "(s32)%s < (s32)%s" % (R(rs), R(rt)))
            if fn == 43:
                return setr(rd, "%s < %s" % (R(rs), R(rt)))
        # sign-extended immediate; a LO16 relocation makes it depend on the load base
        simm_c = "(u32)(s32)(s16)(u16)(0x%04xu + B)" % i.imm if lo_reloc else "0x%08xu" % (i.simm & 0xFFFFFFFF)
        if op in (8, 9):
            if lo_reloc:
                return setr(rt, "%s + %s" % (R(rs), simm_c))
            if rs == 0:
                return setr(rt, "0x%08xu" % (i.simm & 0xFFFFFFFF))
            if i.simm == 0:
                return setr(rt, R(rs))
            return setr(rt, "%s + %s" % (R(rs), simm_c))
        if op == 10:
            return setr(rt, "(s32)%s < (s32)%s" % (R(rs), simm_c))
        if op == 11:
            return setr(rt, "%s < %s" % (R(rs), simm_c))
        if op == 12:
            return setr(rt, "%s & 0x%04xu" % (R(rs), i.imm))
        if op == 13:
            if lo_reloc:
                return setr(rt, "%s | (u32)(u16)(0x%04xu + B)" % (R(rs), i.imm))
            return setr(rt, "%s | 0x%04xu" % (R(rs), i.imm))
        if op == 14:
            return setr(rt, "%s ^ 0x%04xu" % (R(rs), i.imm))
        if op == 15:
            if rl is not None and rl[0] == 1:
                return setr(rt, "(B + 0x%xu + 0x8000u) & 0xFFFF0000u" % rl[1])
            return setr(rt, "0x%08xu" % (i.imm << 16))
        if op == 16:
            if i.w == 0x42000010:
                return "cop0_rfe(c);"
            if rs == 0:
                return setr(rt, "cop0_read(c, %d)" % rd)
            if rs == 4:
                return "cop0_write(c, %d, %s);" % (rd, R(rt))
        if op == 18:
            if rs & 0x10:
                return "gte_op(c, 0x%07xu);" % (i.w & 0x1FFFFFF)
            if rs == 0:
                return "MFC2(%d, %d);" % (rt, rd) if rt else "(void)gte_mfc2(c, %d);" % rd
            if rs == 2:
                return setr(rt, "gte_cfc2(c, %d)" % rd)
            if rs == 4:
                return "MTC2(%d, %d);" % (rt, rd)
            if rs == 6:
                return "gte_ctc2(c, %d, %s);" % (rd, R(rt))
        addr = "%s + %s" % (R(rs), simm_c) if (i.simm or lo_reloc) else R(rs)
        ld = {32: "LB", 33: "LH", 35: "LW", 36: "LBU", 37: "LHU", 34: "LWL", 38: "LWR"}
        st = {40: "SB", 41: "SH", 43: "SW", 42: "SWL", 46: "SWR"}
        if op in ld:
            if rt == 0:
                return "(void)mem_r32(c, (%s) & ~3u);" % addr if op in (35, 34, 38) else "(void)mem_r8(c, %s);" % addr
            return "%s(%d, %s);" % (ld[op], rt, addr)
        if op in st:
            return "%s(%d, %s);" % (st[op], rt, addr)
        if op == 50:
            return "LWC2(%d, %s);" % (rt, addr)
        if op == 58:
            return "SWC2(%d, %s);" % (rt, addr)
        return "psx_invalid(c, %s, 0x%08xu);" % (self.K(i.pc), i.w)

    def cond(self, i):
        R = lambda n: "0u" if n == 0 else "R(%d)" % n
        if i.op == 4:
            return "%s == %s" % (R(i.rs), R(i.rt))
        if i.op == 5:
            return "%s != %s" % (R(i.rs), R(i.rt))
        if i.op == 6:
            return "(s32)%s <= 0" % R(i.rs)
        if i.op == 7:
            return "(s32)%s > 0" % R(i.rs)
        if i.op == 1:
            if i.rt in (0, 16):
                return "(s32)%s < 0" % R(i.rs)
            return "(s32)%s >= 0" % R(i.rs)
        raise ValueError

    def call(self, t):
        if t in self.an.funcs and self.img.has(t):
            return "%s(c);" % self.fname(t)
        if self.ovl:
            if t >= 0x80000000:
                if t in self.main_funcs:
                    return "f_main_%08x(c);" % t
                return "call_indirect(c, 0x%08xu);" % t
            return "call_indirect(c, %s);" % self.K(t)
        return "call_indirect(c, 0x%08xu);" % t

    def goto_out(self, t):
        """leave the function towards address t (not part of this function)"""
        if self.ovl and t >= 0x80000000:
            if t in self.main_funcs:
                return "f_main_%08x(c); return;" % t
            return "c->pc = 0x%08xu; jump_out(c); return;" % t
        if t in self.an.funcs and self.img.has(t):
            return "%s(c); return;" % self.fname(t)
        return "c->pc = %s; jump_out(c); return;" % self.K(t)

    def emit_func(self, entry, out):
        img = self.img
        seen, leaders = self.an.funcs[entry]
        pcs = sorted(seen)
        inline_ds = set()
        for p in pcs:
            if img.ins(p).has_delay():
                inline_ds.add(p + 4)
        w = out.append
        w("void o_%s_%08x(Ctx* c) {" % (self.prefix, entry))
        w("    CHK();")
        if pcs and pcs[0] != entry:
            w("    goto L_%08x;" % entry)
        pend = 0
        prev_term = True
        prev_pc = None
        pcset = set(pcs)
        K = self.K

        def L(t):
            return "L_%08x" % t

        for p in pcs:
            i = img.ins(p)
            standalone_ds = p in inline_ds and p not in leaders
            if standalone_ds:
                continue
            if not prev_term and prev_pc is not None and p != self._next:
                w("    ADV(%d); goto %s;" % (pend, L(self._next)))
                pend = 0
            if p in leaders:
                if pend and not prev_term:
                    w("    ADV(%d);" % pend)
                pend = 0
                w("%s:" % L(p))
            prev_pc = p
            prev_term = False
            if not i.valid():
                w("    psx_invalid(c, %s, 0x%08xu); return;" % (K(p), i.w))
                prev_term = True
                continue
            if not i.has_delay():
                w("    " + self.simple(i))
                pend += 1
                self._next = p + 4
                if (p + 4) not in pcset:
                    w("    ADV(%d); c->pc = %s; jump_out(c); return;" % (pend, K(p + 4)))
                    pend = 0
                    prev_term = True
                continue
            d = img.ins(p + 4) if img.has(p + 4) else None
            if d is None or d.has_delay():
                w("    psx_invalid(c, %s, 0x%08xu); return;" % (K(p), i.w))
                prev_term = True
                continue
            ds = self.simple(d)
            pend += 2
            adv = "ADV(%d);" % pend
            pend = 0
            self._next = p + 8
            nxt_goto = ""
            if (p + 4) in leaders or (p + 8) not in pcset:
                nxt_goto = " goto %s;" % L(p + 8) if (p + 8) in pcset else \
                    " c->pc = %s; jump_out(c); return;" % K(p + 8)
            if i.is_branch():
                t = i.btarget()
                if i.is_link_branch():
                    w("    { int b = %s; R(31) = %s; %s %s if (b) { %s } }%s" %
                      (self.cond(i), K(p + 8), ds, adv, self.call(t), nxt_goto))
                    prev_term = bool(nxt_goto)
                    continue
                back = "CHKB(); " if t <= p else ""
                if t in pcset:
                    tgt = "%sgoto %s;" % (back, L(t))
                else:
                    tgt = "c->pc = %s; jump_out(c); return;" % K(t)
                if i.is_uncond_branch():
                    w("    %s %s %s" % (ds, adv, tgt))
                    prev_term = True
                else:
                    w("    { int b = %s; %s %s if (b) { %s } }%s" % (self.cond(i), ds, adv, tgt, nxt_goto))
                    prev_term = bool(nxt_goto)
                continue
            if i.is_jal():
                w("    R(31) = %s; %s %s %s%s" % (K(p + 8), ds, adv, self.call(i.tgt), nxt_goto))
                prev_term = bool(nxt_goto)
                continue
            if i.is_jalr():
                lk = "R(%d) = %s; " % (i.rd, K(p + 8)) if i.rd else ""
                w("    { u32 t = R(%d); %s%s %s call_indirect(c, t); }%s" % (i.rs, lk, ds, adv, nxt_goto))
                prev_term = bool(nxt_goto)
                continue
            if i.is_j():
                t = i.tgt
                if t in pcset and img.has(t):
                    back = "CHKB(); " if t <= p else ""
                    w("    %s %s %sgoto %s;" % (ds, adv, back, L(t)))
                else:
                    w("    %s %s %s" % (ds, adv, self.goto_out(t)))
                prev_term = True
                continue
            if i.is_jr():
                if i.rs == 31:
                    w("    %s %s return;" % (ds, adv))
                else:
                    tg = self.an.jt.get(p)
                    if tg:
                        w("    { u32 t = R(%d); %s %s switch (t%s) {" % (i.rs, ds, adv, " - B" if self.ovl else ""))
                        for t in sorted(set(tg)):
                            w("        case 0x%08xu: goto %s;" % (t, L(t)))
                        w("        default: c->pc = t; jump_out(c); return; } }")
                    else:
                        w("    { u32 t = R(%d); %s %s c->pc = t; jump_out(c); return; }" % (i.rs, ds, adv))
                prev_term = True
                continue
        if not prev_term:
            w("    ADV(%d); c->pc = %s; jump_out(c); return;" % (pend, K(self._next)))
        w("}")
        w("void %s(Ctx* c) { HookFn h = %s_hooks[%d]; CS_PUSH(%s); if (h) h(c, o_%s_%08x); else o_%s_%08x(c); CS_POP(); }" %
          (self.fname(entry), self.prefix, self.index[entry], K(entry), self.prefix, entry, self.prefix, entry))
        w("")


def write_if_changed(path, txt):
    if not os.path.exists(path) or open(path).read() != txt:
        open(path, "w").write(txt)


def emit_image(img, an, outdir, per, main_funcs=None):
    em = Emitter(an, img.name, main_funcs)
    ents = sorted(an.funcs)
    em.index = {e: i for i, e in enumerate(ents)}
    files = []
    for k in range(0, len(ents), per):
        out = ['#include "recomp.h"']
        if img.overlay:
            out += ['#include "main_funcs.h"', '#include "%s_funcs.h"' % img.name, "#define B ovl_%s_base" % img.name, ""]
        else:
            out += ['#include "%s_funcs.h"' % img.name, ""]
        for e in ents[k:k + per]:
            em.emit_func(e, out)
        fn = "%s_%03d.c" % (img.name, k // per)
        write_if_changed(os.path.join(outdir, fn), "\n".join(out))
        files.append(fn)
    for f in os.listdir(outdir):
        if f.startswith(img.name + "_") and f.endswith(".c") and f not in files and f != img.name + "_table.c" \
                and f[len(img.name) + 1:-2].isdigit():
            os.remove(os.path.join(outdir, f))
    h = "#pragma once\nextern HookFn %s_hooks[];\n" % img.name
    if img.overlay:
        h += "extern u32 ovl_%s_base;\n" % img.name
    h += "".join("void %s(Ctx* c);\n" % em.fname(e) for e in ents)
    write_if_changed(os.path.join(outdir, "%s_funcs.h" % img.name), h)
    t = '#include "recomp.h"\n#include "%s_funcs.h"\n' % img.name
    if img.overlay:
        t += "u32 ovl_%s_base;\n" % img.name
    t += "const RecompFunc %s_funcs[] = {\n" % img.name
    t += "".join("    {0x%08xu, %s},\n" % (e, em.fname(e)) for e in ents)
    t += "};\nconst unsigned %s_func_count = %d;\nHookFn %s_hooks[%d];\n" % (img.name, len(ents), img.name, len(ents) + 1)
    write_if_changed(os.path.join(outdir, "%s_table.c" % img.name), t)
    with open(os.path.join(outdir, "%s_map.txt" % img.name), "w") as m:
        for e in ents:
            m.write("%08x %d\n" % (e, len(an.funcs[e][0])))
    ninsn = len(an.cover)
    print("%s: %d functions, %d insns covered (%.1f%% of image), %d jump tables, %d files" %
          (img.name, len(ents), ninsn, 400.0 * ninsn / max(1, len(img.data)), len(an.jt), len(files)))
    return ents


def read_seeds(path):
    out = []
    if path and os.path.exists(path):
        for ln in open(path):
            ln = ln.split("#")[0].strip()
            if ln:
                out.append(int(ln, 16))
    return out


def load_lvb(path):
    """returns (image bytes linked at 0, {offset: (type, addend)}, checksum)"""
    d = open(path, "rb").read()
    n, off, size, roff = struct.unpack_from("<4I", d, 0)
    assert n == 1, "multi-section LVB not supported"
    hdr = n * 12 + 4
    image = d[hdr + off: hdr + off + size]
    rel = d[hdr + off + size + roff:]
    relocs = {}
    i = 0
    while True:
        w = struct.unpack_from("<I", rel, i)[0]
        i += 4
        if w == 0xFFFFFFFF:
            break
        t = w & 3
        add = 0
        if t == 1:
            add = struct.unpack_from("<I", rel, i)[0]
            i += 4
        relocs[w & ~3] = (t, add)
    nw = len(image) // 4
    checksum = sum(struct.unpack_from("<%dI" % nw, image)) & 0xFFFFFFFF
    return image, relocs, checksum


def main():
    cfg = json.load(open(sys.argv[1]))
    root = os.path.dirname(os.path.abspath(sys.argv[1]))
    outdir = os.path.join(root, cfg["out"])
    os.makedirs(outdir, exist_ok=True)
    main_funcs = set()
    for im in cfg["images"]:
        raw = open(os.path.join(root, im["file"]), "rb").read()
        raw = raw[im.get("skip", 0):]
        if "size" in im:
            raw = raw[:im["size"]]
        img = Image(im["name"], int(im["base"], 16), raw)
        an = Analyzer(img)
        entries = [int(x, 16) for x in im.get("entries", [])]
        extra = [int(x, 16) for x in im.get("extra", [])] + read_seeds(os.path.join(root, im.get("seeds", "")) if im.get("seeds") else None)
        an.discover(entries, extra)
        an.build()
        ents = emit_image(img, an, outdir, im.get("per_file", 250))
        if im["name"] == "main":
            main_funcs = set(ents)
    ov = cfg.get("overlays")
    descs = []
    if ov:
        import glob
        for path in sorted(glob.glob(os.path.join(root, ov["glob"]))):
            name = os.path.splitext(os.path.basename(path))[0].lower()
            image, relocs, checksum = load_lvb(path)
            img = Image(name, 0, image, relocs)
            an = Analyzer(img)
            # the image starts with a table of relocated function pointers
            entries = []
            for a in sorted(relocs):
                if relocs[a][0] == 0 and a < 0x40 and img.has(img.w(a)):
                    entries.append(img.w(a))
            extra = read_seeds(os.path.join(root, "seeds_%s.txt" % name))
            an.discover(entries, extra)
            an.build()
            emit_image(img, an, outdir, ov.get("per_file", 250), main_funcs)
            descs.append((name, len(image), checksum, len(an.funcs)))
        t = '#include "recomp.h"\n'
        for name, size, cs, n in descs:
            t += "extern const RecompFunc %s_funcs[];\nextern u32 ovl_%s_base;\n" % (name, name)
        t += "const OverlayDesc g_overlay_descs[] = {\n"
        for name, size, cs, n in descs:
            t += '    {"%s", 0x%xu, 0x%08xu, %s_funcs, %d, &ovl_%s_base},\n' % (name, size, cs, name, n, name)
        t += "};\nconst unsigned g_overlay_desc_count = %d;\n" % len(descs)
        write_if_changed(os.path.join(outdir, "overlays.c"), t)


if __name__ == "__main__":
    main()
