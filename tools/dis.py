"""Tiny MIPS disassembler for the main EXE: dis.py <hexaddr> [count] [file base skip]"""
import struct
import sys

R = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
     "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


def dis(w, pc):
    op, rs, rt, rd, sh, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w == 0:
        return "nop"
    if op == 0:
        n = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr", 12: "syscall", 13: "break",
             16: "mfhi", 17: "mthi", 18: "mflo", 19: "mtlo", 24: "mult", 25: "multu", 26: "div", 27: "divu", 32: "add",
             33: "addu", 34: "sub", 35: "subu", 36: "and", 37: "or", 38: "xor", 39: "nor", 42: "slt", 43: "sltu"}.get(fn, "?%d" % fn)
        if fn in (0, 2, 3):
            return "%s %s,%s,%d" % (n, R[rd], R[rt], sh)
        if fn in (8,):
            return "jr %s" % R[rs]
        if fn == 9:
            return "jalr %s" % R[rs]
        if fn in (16, 18):
            return "%s %s" % (n, R[rd])
        if fn in (24, 25, 26, 27):
            return "%s %s,%s" % (n, R[rs], R[rt])
        return "%s %s,%s,%s" % (n, R[rd], R[rs], R[rt])
    if op in (2, 3):
        return "%s %08x" % ("j" if op == 2 else "jal", ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
    if op in (4, 5, 6, 7, 1):
        n = {4: "beq", 5: "bne", 6: "blez", 7: "bgtz"}.get(op) or {0: "bltz", 1: "bgez", 16: "bltzal", 17: "bgezal"}.get(rt, "b?")
        return "%s %s,%s,%08x" % (n, R[rs], R[rt], pc + 4 + simm * 4)
    if op in (8, 9, 10, 11, 12, 13, 14):
        n = {8: "addi", 9: "addiu", 10: "slti", 11: "sltiu", 12: "andi", 13: "ori", 14: "xori"}[op]
        return "%s %s,%s,%s" % (n, R[rt], R[rs], hex(imm) if op >= 12 else simm)
    if op == 15:
        return "lui %s,0x%04x" % (R[rt], imm)
    if op in (32, 33, 34, 35, 36, 37, 38, 40, 41, 42, 43, 46, 50, 58):
        n = {32: "lb", 33: "lh", 34: "lwl", 35: "lw", 36: "lbu", 37: "lhu", 38: "lwr", 40: "sb", 41: "sh", 42: "swl", 43: "sw", 46: "swr",
             50: "lwc2", 58: "swc2"}[op]
        return "%s %s,%d(%s)" % (n, R[rt] if op < 50 else "c%d" % rt, simm, R[rs])
    if op == 16:
        return "cop0 %08x" % w
    if op == 18:
        return "cop2 %08x" % w
    return ".word %08x" % w


if __name__ == "__main__":
    addr = int(sys.argv[1], 16)
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    f = sys.argv[3] if len(sys.argv) > 3 else "disc/SCES_033.64"
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x80010000
    skip = int(sys.argv[5]) if len(sys.argv) > 5 else 0x800
    d = open(f, "rb").read()
    for i in range(n):
        a = addr + i * 4
        w = struct.unpack_from("<I", d, a - base + skip)[0]
        print("%08x: %08x  %s" % (a, w, dis(w, a)))
