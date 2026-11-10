"""Wrap a PS-X EXE body in a MIPS32 LE ELF so standard RE tools (Ghidra via REA) load it.

usage: exe2elf.py <psx.exe> <out.elf> [func_map.txt]
func_map lines: "<hexaddr> ..." become STT_FUNC symbols f_<addr>.
"""
import struct
import sys

exe = open(sys.argv[1], "rb").read()
pc0, gp0, taddr, tsize = struct.unpack_from("<4I", exe, 0x10)
body = exe[0x800:0x800 + tsize]
funcs = []
if len(sys.argv) > 3:
    for ln in open(sys.argv[3]):
        p = ln.split()
        if p:
            funcs.append((int(p[0], 16), int(p[1]) * 4 if len(p) > 1 else 0))

shstr = b"\0.text\0.symtab\0.strtab\0.shstrtab\0"
strtab = b"\0"
syms = [struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)]
for a, sz in funcs:
    off = len(strtab)
    strtab += b"f_%08x\0" % a
    syms.append(struct.pack("<IIIBBH", off, a, 0, 0x12, 0, 1))  # GLOBAL FUNC, shndx 1
symtab = b"".join(syms)

EH, PH, SH = 52, 32, 40
off_text = 0x1000
off_sym = off_text + len(body)
off_str = off_sym + len(symtab)
off_shstr = off_str + len(strtab)
off_sh = (off_shstr + len(shstr) + 3) & ~3

eh = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
eh += struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, pc0, EH, off_sh, 0x1000, EH, PH, 1, SH, 5, 4)
ph = struct.pack("<IIIIIIII", 1, off_text, taddr, taddr, len(body), len(body), 7, 0x1000)


def sh(name, typ, flags, addr, off, size, link=0, info=0, align=4, ent=0):
    return struct.pack("<IIIIIIIIII", shstr.index(name), typ, flags, addr, off, size, link, info, align, ent)


shs = b"\0" * SH
shs += sh(b".text\0", 1, 7, taddr, off_text, len(body))
shs += sh(b".symtab\0", 2, 0, 0, off_sym, len(symtab), 3, 1, 4, 16)
shs += sh(b".strtab\0", 3, 0, 0, off_str, len(strtab), align=1)
shs += sh(b".shstrtab\0", 3, 0, 0, off_shstr, len(shstr), align=1)

out = bytearray(eh + ph)
out += b"\0" * (off_text - len(out))
out += body + symtab + strtab + shstr
out += b"\0" * (off_sh - len(out))
out += shs
open(sys.argv[2], "wb").write(out)
print("wrote", sys.argv[2], len(out), "entry %08x" % pc0, len(funcs), "symbols")
