"""MIPS R3000A decode helpers shared by the analyser and the C emitter."""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


class Ins:
    __slots__ = ("w", "pc", "op", "rs", "rt", "rd", "sh", "fn", "imm", "simm", "tgt")

    def __init__(self, w, pc):
        self.w = w
        self.pc = pc
        self.op = w >> 26
        self.rs = (w >> 21) & 31
        self.rt = (w >> 16) & 31
        self.rd = (w >> 11) & 31
        self.sh = (w >> 6) & 31
        self.fn = w & 63
        self.imm = w & 0xFFFF
        self.simm = s16(self.imm)
        self.tgt = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)

    # --- classification -------------------------------------------------
    def is_branch(self):
        """conditional/unconditional pc-relative branch"""
        if self.op in (4, 5, 6, 7):
            return True
        if self.op == 1 and self.rt in (0, 1, 16, 17):
            return True
        return False

    def btarget(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def is_uncond_branch(self):
        # beq r,r / bgez zero / blez zero
        if self.op == 4 and self.rs == self.rt:
            return True
        if self.op == 1 and self.rt in (1, 17) and self.rs == 0:
            return True
        if self.op == 6 and self.rs == 0:
            return True
        return False

    def is_link_branch(self):
        return self.op == 1 and self.rt in (16, 17)

    def is_j(self):
        return self.op == 2

    def is_jal(self):
        return self.op == 3

    def is_jr(self):
        return self.op == 0 and self.fn == 8

    def is_jalr(self):
        return self.op == 0 and self.fn == 9

    def has_delay(self):
        return self.is_branch() or self.op in (2, 3) or self.is_jr() or self.is_jalr()

    def valid(self):
        op, fn = self.op, self.fn
        if op == 0:
            if fn in (0, 2, 3, 4, 6, 7, 8, 9, 12, 13, 16, 17, 18, 19, 24, 25, 26, 27,
                      32, 33, 34, 35, 36, 37, 38, 39, 42, 43):
                # stricter checks: unused fields must be zero
                if fn in (0, 2, 3) and self.rs != 0:
                    return False
                if fn in (4, 6, 7, 32, 33, 34, 35, 36, 37, 38, 39, 42, 43) and self.sh != 0:
                    return False
                if fn == 8 and (self.w & 0x001FFFC0):
                    return False
                if fn == 9 and (self.rt != 0 or self.sh != 0):
                    return False
                if fn in (16, 18) and (self.w & 0x03FF07C0):
                    return False
                if fn in (17, 19) and (self.w & 0x001FFFC0):
                    return False
                if fn in (24, 25, 26, 27) and (self.w & 0x0000FFC0):
                    return False
                return True
            return False
        if op == 1:
            return self.rt in (0, 1, 16, 17)
        if op in (2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 14):
            return True
        if op in (6, 7):
            return self.rt == 0
        if op == 15:
            return self.rs == 0
        if op == 16:
            if self.rs in (0, 4):
                return (self.w & 0x7FF) == 0
            return self.w == 0x42000010  # rfe
        if op == 18:
            if self.rs & 0x10:
                return True
            return self.rs in (0, 2, 4, 6) and (self.w & 0x7FF) == 0
        if op in (32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58):
            return True
        return False
