import struct, sys, re, collections

p = sys.argv[1]
d = open(p, "rb").read()
print("magic", d[:8])
pc0, gp0, taddr, tsize, daddr, dsize, baddr, bsize, saddr, ssize = struct.unpack_from("<10I", d, 0x10)
print(f"pc0={pc0:08x} gp0={gp0:08x} t_addr={taddr:08x} t_size={tsize:x} b_addr={baddr:08x} b_size={bsize:x} s_addr={saddr:08x} s_size={ssize:x}")
print("region:", d[0x4c:0x4c + 60].split(b"\0")[0])
body = d[0x800:]
print("end addr %08x" % (taddr + len(body)))
# opcode histogram / jal targets
n = len(body) // 4
words = struct.unpack_from("<%dI" % n, body)
jal = collections.Counter()
jr_ra = 0
for w in words:
    if w >> 26 == 3:
        jal[((w & 0x3ffffff) << 2) | 0x80000000] += 1
    if w == 0x03e00008:
        jr_ra += 1
inr = [t for t in jal if taddr <= t < taddr + len(body)]
print("jal sites", sum(jal.values()), "unique targets", len(jal), "in-range", len(inr), "jr ra", jr_ra)
outr = sorted(t for t in jal if not (taddr <= t < taddr + len(body)))
print("out-of-range jal targets:", [hex(x) for x in outr[:40]], len(outr))
# library version strings
for m in set(re.findall(rb"[ -~]{6,}", body)):
    s = m.decode()
    if re.search(r"(?i)\$Id|lib[a-z]+\.|Library|Ver\.|version|\.c\b|\.LVB|\.MWD|\.STR|RELOC|cdrom:|bu00", s):
        print("  str:", s[:100])
