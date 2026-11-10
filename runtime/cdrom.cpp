// CD-ROM controller + cue/bin disc image + XA-ADPCM decode.
#include "psx.h"
#include <deque>

// ---- disc image ------------------------------------------------------
struct Track {
    FILE* f = nullptr;
    bool audio = false;
    u32 start = 0;      // disc LBA of file offset 0
    u32 index1 = 0;     // disc LBA of INDEX 01
    u32 sectors = 0;
};
static std::vector<Track> g_tracks;
static u32 g_disc_end;

static std::string dirname_of(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? "" : p.substr(0, i + 1);
}

static bool disc_open(const std::string& cue) {
    FILE* f = fopen(cue.c_str(), "r");
    if (!f) return false;
    char line[1024];
    std::string dir = dirname_of(cue);
    u32 pos = 0;
    Track* cur = nullptr;
    FILE* curfile = nullptr;
    u32 filesecs = 0, filestart = 0;
    bool newfile = false;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "FILE", 4)) {
            char* q1 = strchr(p, '"');
            char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
            if (!q1 || !q2) continue;
            std::string name(q1 + 1, q2);
            pos += filesecs;
            curfile = fopen((dir + name).c_str(), "rb");
            if (!curfile) { logf("cue: cannot open %s\n", name.c_str()); fclose(f); return false; }
            _fseeki64(curfile, 0, SEEK_END);
            filesecs = (u32)(_ftelli64(curfile) / 2352);
            filestart = pos;
            newfile = true;
        } else if (!strncmp(p, "TRACK", 5)) {
            g_tracks.emplace_back();
            cur = &g_tracks.back();
            cur->f = curfile;
            cur->audio = strstr(p, "AUDIO") != nullptr;
            cur->start = filestart;
            cur->index1 = filestart;
            cur->sectors = filesecs;
            newfile = false;
        } else if (!strncmp(p, "INDEX", 5) && cur) {
            int n, m, s, fr;
            if (sscanf(p, "INDEX %d %d:%d:%d", &n, &m, &s, &fr) == 4 && n == 1)
                cur->index1 = filestart + (m * 60 + s) * 75 + fr;
        }
    }
    (void)newfile;
    fclose(f);
    g_disc_end = pos + filesecs;
    return !g_tracks.empty();
}

// ---- mod overrides: replace file contents sector by sector ----------------------------------------
struct Override { u32 lba; std::vector<u8> data; };   // data covers whole 2048-byte sectors from lba
static std::vector<Override> g_overrides;
static bool disc_read_raw(u32 lba, u8* out);
static bool disc_read(u32 lba, u8* out) {
    bool ok = disc_read_raw(lba, out);
    for (const Override& o : g_overrides) {
        u32 n = (u32)(o.data.size() / 2048);
        if (lba >= o.lba && lba < o.lba + n) memcpy(out + 24, o.data.data() + (size_t)(lba - o.lba) * 2048, 2048);
    }
    return ok;
}
static bool disc_read_raw(u32 lba, u8* out) {
    for (auto& t : g_tracks) {
        if (lba >= t.start && lba < t.start + t.sectors) {
            _fseeki64(t.f, (s64)(lba - t.start) * 2352, SEEK_SET);
            return fread(out, 1, 2352, t.f) == 2352;
        }
    }
    memset(out, 0, 2352);
    return false;
}
static int track_of(u32 lba) {
    int n = 1;
    for (size_t i = 0; i < g_tracks.size(); i++)
        if (lba >= g_tracks[i].start) n = (int)i + 1;
    return n;
}

static inline u8 bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }
static inline u32 unbcd(u8 v) { return (v >> 4) * 10 + (v & 15); }

// ---- controller state ---------------------------------------------------
struct Resp { u8 type; u8 n; u8 d[16]; bool sector; };
static std::deque<Resp> g_q;
static u8 g_index, g_int_en = 0x1F, g_int_flag;  // the BIOS leaves all CD interrupts enabled
static u8 g_param[16];
static int g_nparam;
static u8 g_resp[16];
static int g_resp_n, g_resp_pos;
static u8 g_mode, g_filter_file, g_filter_chan;
static u32 g_setloc, g_cur;
static bool g_setloc_pending;
static bool g_reading, g_playing, g_seeking, g_motor = true, g_muted;
static u8 g_sector[2352];       // last sector read off the disc
static u8 g_sectorbuf[2352];    // sector latched for the host (at INT1 delivery)
static std::deque<std::vector<u8>> g_sector_q;
static u8 g_fifo[2352];
static int g_fifo_len, g_fifo_pos;
static u8 g_last_hdr[8];
static u8 g_pending_cmd;
static bool g_cmd_busy;
static u8 g_vol[4] = {0x80, 0, 0x80, 0}, g_vol_pending[4] = {0x80, 0, 0x80, 0};
static s32 g_xa_hist[2][2];
int g_cd_fast = 1;

static u8 stat_byte() {
    u8 s = 0;
    if (g_motor) s |= 2;
    if (g_reading) s |= 0x20;
    if (g_seeking) s |= 0x40;
    if (g_playing) s |= 0x80;
    return s;
}

static void deliver(int, s64);
static void try_deliver() {
    if (g_int_flag || g_q.empty()) return;
    Resp r = g_q.front();
    g_q.pop_front();
    memcpy(g_resp, r.d, r.n);
    g_resp_n = r.n;
    g_resp_pos = 0;
    g_int_flag = r.type;
    if (r.sector && !g_sector_q.empty()) {
        memcpy(g_sectorbuf, g_sector_q.front().data(), 2352);
        g_sector_q.pop_front();
    }
    if (g_int_flag & g_int_en) irq_raise(IRQ_CDROM);
}
static void deliver(int, s64) { try_deliver(); }

static void push(u8 type, std::initializer_list<u8> bytes, bool sector = false) {
    Resp r{};
    r.type = type;
    r.sector = sector;
    for (u8 b : bytes) r.d[r.n++] = b;
    g_q.push_back(r);
    try_deliver();
}

// ---- XA ADPCM -----------------------------------------------------------
static void xa_decode(const u8* sec) {
    const u8* sub = sec + 16;
    u8 coding = sub[3];
    bool stereo = coding & 1;
    int rate = (coding & 4) ? 18900 : 37800;
    if (coding & 0x10) return;  // 8-bit XA: not used
    static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
    static s16 pcm[18 * 8 * 28 * 2];
    int nL = 0, nR = 0;
    s16* outL = pcm;       // interleave later
    static s16 L[18 * 4 * 28 * 2], Rr[18 * 4 * 28];
    (void)outL;
    for (int g = 0; g < 18; g++) {
        const u8* grp = sec + 24 + g * 128;
        for (int blk = 0; blk < 8; blk++) {
            u8 h = grp[4 + blk];
            int shift = h & 15;
            if (shift > 12) shift = 9;
            int flt = (h >> 4) & 3;
            int ch = stereo ? (blk & 1) : 0;
            s32* hist = g_xa_hist[ch];
            for (int i = 0; i < 28; i++) {
                u8 b = grp[16 + i * 4 + blk / 2];
                int nib = (blk & 1) ? (b >> 4) : (b & 15);
                s32 t = (s16)(nib << 12) >> shift;
                s32 s = t + ((hist[0] * K0[flt] + hist[1] * K1[flt] + 32) >> 6);
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                hist[1] = hist[0];
                hist[0] = s;
                if (ch == 0) L[nL++] = (s16)s; else Rr[nR++] = (s16)s;
            }
        }
    }
    int frames = nL;
    for (int i = 0; i < frames; i++) {
        s32 l = L[i], r = stereo ? Rr[i] : L[i];
        // CD volume matrix
        s32 ol = (l * g_vol[0] + r * g_vol[3]) >> 7;   // L->L + R->L
        s32 orr = (l * g_vol[1] + r * g_vol[2]) >> 7;  // L->R + R->R
        if (g_muted) ol = orr = 0;
        pcm[i * 2] = (s16)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
        pcm[i * 2 + 1] = (s16)(orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr);
    }
    spu_cd_audio(pcm, frames, rate);
}

// ---- reading -------------------------------------------------------------
static s64 sector_period(bool data) {
    s64 p = SYSCLK / ((g_mode & 0x80) ? 150 : 75);
    if (data && g_cd_fast > 1 && !(g_mode & 0x40)) p /= g_cd_fast;
    return p;
}

static void read_event(int, s64 late) {
    if (late < 0 || late > 100000) late = 0;   // keep the long-term sector rate exact: no drift against the sound clock
    if (!g_reading && !g_playing) return;
    if (g_playing) {
        u8 raw[2352];
        disc_read(g_cur, raw);
        s16 pcm[588 * 2];
        for (int i = 0; i < 588; i++) {
            s32 l = (s16)(raw[i * 4] | (raw[i * 4 + 1] << 8)), r = (s16)(raw[i * 4 + 2] | (raw[i * 4 + 3] << 8));
            s32 ol = (l * g_vol[0] + r * g_vol[3]) >> 7, orr = (l * g_vol[1] + r * g_vol[2]) >> 7;
            if (g_muted) ol = orr = 0;
            pcm[i * 2] = (s16)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
            pcm[i * 2 + 1] = (s16)(orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr);
        }
        spu_cd_audio(pcm, 588, 44100);
        g_cur++;
        if (g_cur >= g_disc_end) { g_playing = false; push(4, {stat_byte()}); return; }
        sched_set(EV_CDROM2, SYSCLK / 75 - late, read_event);
        return;
    }
    // back-pressure: never outrun the game
    if (g_sector_q.size() >= 2) { sched_set(EV_CDROM2, 2000, read_event); return; }
    disc_read(g_cur, g_sector);
    memcpy(g_last_hdr, g_sector + 12, 8);
    g_cur++;
    const u8* sub = g_sector + 16;
    bool is_audio = (g_sector[15] == 2) && (sub[2] & 0x04) && (sub[2] & 0x40);
    bool deliver_data = true;
    if ((g_mode & 0x40) && is_audio) {
        deliver_data = false;
        if (!(g_mode & 0x08) || (sub[0] == g_filter_file && sub[1] == g_filter_chan)) xa_decode(g_sector);
    } else if ((g_mode & 0x08) && is_audio) {
        deliver_data = false;
    }
    if (deliver_data) {
        g_sector_q.emplace_back(g_sector, g_sector + 2352);
        push(1, {stat_byte()}, true);
    }
    sched_set(EV_CDROM2, sector_period(deliver_data) - late, read_event);
}

static void second_response(int, s64);
static u8 g_second_cmd;

static void do_seek() {
    if (g_setloc_pending) { g_cur = g_setloc; g_setloc_pending = false; }
}

static void exec_command(int, s64) {
    u8 cmd = g_pending_cmd;
    g_cmd_busy = false;
    u8* p = g_param;
    int np = g_nparam;
    g_nparam = 0;
    LOGV("CD cmd %02x (%02x %02x %02x) np=%d\n", cmd, p[0], p[1], p[2], np);
    switch (cmd) {
    case 0x01: push(3, {stat_byte()}); break;
    case 0x02:
        g_setloc = (unbcd(p[0]) * 60 + unbcd(p[1])) * 75 + unbcd(p[2]) - 150;
        g_setloc_pending = true;
        push(3, {stat_byte()});
        break;
    case 0x03: {
        if (np && p[0]) {
            u32 t = unbcd(p[0]);
            if (t >= 1 && t <= g_tracks.size()) g_cur = g_tracks[t - 1].index1;
        } else do_seek();
        g_reading = false;
        g_playing = true;
        push(3, {stat_byte()});
        sched_set(EV_CDROM2, SYSCLK / 75, read_event);
        break;
    }
    case 0x06: case 0x1B:
        do_seek();
        g_playing = false;
        g_reading = true;
        g_sector_q.clear();
        g_xa_hist[0][0] = g_xa_hist[0][1] = g_xa_hist[1][0] = g_xa_hist[1][1] = 0;
        push(3, {stat_byte()});
        sched_set(EV_CDROM2, sector_period(true) + (g_cd_fast > 1 ? 20000 : 300000), read_event);
        break;
    case 0x07: g_motor = true; push(3, {stat_byte()}); g_second_cmd = cmd; sched_set(EV_CD_SECOND, 100000, second_response); break;
    case 0x08:
        push(3, {stat_byte()});
        g_reading = g_playing = false;
        g_second_cmd = cmd;
        sched_set(EV_CD_SECOND, 100000, second_response);
        break;
    case 0x09:
        push(3, {stat_byte()});
        g_reading = g_playing = false;
        sched_cancel(EV_CDROM2);
        g_second_cmd = cmd;
        sched_set(EV_CD_SECOND, g_cd_fast > 1 ? 30000 : 1000000, second_response);
        break;
    case 0x0A:
        g_mode = 0x20;
        g_motor = true;
        g_reading = g_playing = false;
        sched_cancel(EV_CDROM2);
        push(3, {stat_byte()});
        g_second_cmd = cmd;
        sched_set(EV_CD_SECOND, 100000, second_response);
        break;
    case 0x0B: g_muted = true; push(3, {stat_byte()}); break;
    case 0x0C: g_muted = false; push(3, {stat_byte()}); break;
    case 0x0D: g_filter_file = p[0]; g_filter_chan = p[1]; push(3, {stat_byte()}); break;
    case 0x0E: g_mode = p[0]; push(3, {stat_byte()}); break;
    case 0x0F: push(3, {stat_byte(), g_mode, 0, g_filter_file, g_filter_chan}); break;
    case 0x10: push(3, {g_last_hdr[0], g_last_hdr[1], g_last_hdr[2], g_last_hdr[3], g_last_hdr[4], g_last_hdr[5], g_last_hdr[6], g_last_hdr[7]}); break;
    case 0x11: {
        int t = track_of(g_cur);
        u32 abs = g_cur + 150;
        s32 rel = (s32)g_cur - (s32)g_tracks[t - 1].index1;
        if (rel < 0) rel = -rel;
        push(3, {bcd(t), 1, bcd(rel / 75 / 60), bcd(rel / 75 % 60), bcd(rel % 75), bcd(abs / 75 / 60), bcd(abs / 75 % 60), bcd(abs % 75)});
        break;
    }
    case 0x12: push(3, {stat_byte()}); g_second_cmd = cmd; sched_set(EV_CD_SECOND, 100000, second_response); break;
    case 0x13: push(3, {stat_byte(), 1, bcd((u32)g_tracks.size())}); break;
    case 0x14: {
        u32 t = unbcd(p[0]);
        u32 lba = (t == 0 || t > g_tracks.size()) ? g_disc_end : g_tracks[t - 1].index1;
        lba += 150;
        push(3, {stat_byte(), bcd(lba / 75 / 60), bcd(lba / 75 % 60)});
        break;
    }
    case 0x15: case 0x16:
        g_reading = g_playing = false;
        sched_cancel(EV_CDROM2);
        g_seeking = true;
        push(3, {stat_byte()});
        do_seek();
        g_second_cmd = cmd;
        sched_set(EV_CD_SECOND, g_cd_fast > 1 ? 20000 : 400000, second_response);
        break;
    case 0x19:
        if (p[0] == 0x20) push(3, {0x97, 0x01, 0x10, 0xC2});
        else push(3, {stat_byte()});
        break;
    case 0x1A: push(3, {stat_byte()}); g_second_cmd = cmd; sched_set(EV_CD_SECOND, 20000, second_response); break;
    case 0x1E: push(3, {stat_byte()}); g_second_cmd = cmd; sched_set(EV_CD_SECOND, 100000, second_response); break;
    default:
        logf("CD: unknown command %02x\n", cmd);
        push(5, {(u8)(stat_byte() | 1), 0x40});
        break;
    }
}

static void second_response(int, s64) {
    switch (g_second_cmd) {
    case 0x08: g_motor = false; push(2, {stat_byte()}); break;
    case 0x15: case 0x16: g_seeking = false; push(2, {stat_byte()}); break;
    case 0x1A: push(2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'E'}); break;
    default: push(2, {stat_byte()}); break;
    }
}

u32 cdrom_read(u32 reg) {
    switch (reg) {
    case 0: {
        u8 s = g_index & 3;
        if (g_nparam == 0) s |= 0x08;
        s |= 0x10;
        if (g_resp_pos < g_resp_n) s |= 0x20;
        if (g_fifo_pos < g_fifo_len) s |= 0x40;
        if (g_cmd_busy) s |= 0x80;
        return s;
    }
    case 1:
        if (g_resp_pos < g_resp_n) return g_resp[g_resp_pos++];
        return 0;
    case 2:
        if (g_fifo_pos < g_fifo_len) return g_fifo[g_fifo_pos++];
        return 0;
    case 3:
        return (g_index & 1) ? (g_int_flag | 0xE0) : (g_int_en | 0xE0);
    }
    return 0;
}

void cdrom_write(u32 reg, u32 v) {
    switch (reg) {
    case 0: g_index = v & 3; return;
    case 1:
        if (g_index == 0) {
            g_pending_cmd = (u8)v;
            g_cmd_busy = true;
            sched_set(EV_CDROM, v == 0x0A ? 60000 : (g_cd_fast > 1 ? 6000 : 25000), exec_command);
        } else if (g_index == 3) g_vol_pending[2] = (u8)v;  // R->R
        return;
    case 2:
        if (g_index == 0) { if (g_nparam < 16) g_param[g_nparam++] = (u8)v; }
        else if (g_index == 1) { g_int_en = v & 0x1F; if (g_int_flag & g_int_en) irq_raise(IRQ_CDROM); }
        else if (g_index == 2) g_vol_pending[0] = (u8)v;    // L->L
        else g_vol_pending[3] = (u8)v;                      // R->L
        return;
    case 3:
        if (g_index == 0) {
            if (v & 0x80) {
                if (g_fifo_pos >= g_fifo_len) {
                    if (g_mode & 0x20) { memcpy(g_fifo, g_sectorbuf + 12, 2340); g_fifo_len = 2340; }
                    else { memcpy(g_fifo, g_sectorbuf + 24, 2048); g_fifo_len = 2048; }
                    g_fifo_pos = 0;
                }
            } else {
                g_fifo_len = g_fifo_pos = 0;
            }
        } else if (g_index == 1) {
            g_int_flag &= ~(v & 0x1F);
            if (v & 0x40) g_nparam = 0;
            if (!g_int_flag) {
                g_resp_n = g_resp_pos = 0;
                if (!g_q.empty()) sched_set(EV_CD_DELIVER, g_cd_fast > 1 ? 500 : 3000, deliver);
            }
        } else if (g_index == 2) g_vol_pending[1] = (u8)v;  // L->R
        else if (v & 0x20) memcpy(g_vol, g_vol_pending, 4);
        return;
    }
}

void cdrom_dma(u32 madr, u32 bcr, u32 chcr) {
    u32 words = ((chcr >> 9) & 3) == 1 ? (bcr & 0xFFFF) * (bcr >> 16) : (bcr & 0xFFFF);
    if (!words) words = 0x10000;
    u32 bytes = words * 4;
    u32 a = madr & 0x1FFFFC;
    for (u32 i = 0; i < bytes; i++) {
        u8 b = g_fifo_pos < g_fifo_len ? g_fifo[g_fifo_pos++] : 0;
        g_ram[(a + i) & (RAM_SIZE - 1)] = b;
    }
    for (u32 i = 0; i < words; i++) {
        u32 wa = (a + i * 4) & (RAM_SIZE - 4), w;
        memcpy(&w, g_ram + wa, 4);
        g_pgxp_mem[wa >> 2].val = ~w;
    }
}

bool cdrom_init(const std::string& cue) {
    if (!disc_open(cue)) return false;
    logf("disc: %d tracks, %u sectors\n", (int)g_tracks.size(), g_disc_end);
    return true;
}

// ---- ISO9660 helper: read a file (path with '\' or '/' separators) from track 1 ----
static bool user_read(u32 lba, u8* out2048) {
    u8 raw[2352];
    if (!disc_read(lba, raw)) return false;
    memcpy(out2048, raw + 24, 2048);
    return true;
}
bool cdrom_find_file(const char* path, u32* lba, u32* size) {
    u8 sec[2048];
    if (!user_read(16, sec) || memcmp(sec + 1, "CD001", 5)) return false;
    u32 dlba, dsize;
    memcpy(&dlba, sec + 156 + 2, 4);
    memcpy(&dsize, sec + 156 + 10, 4);
    std::string rest = path;
    while (!rest.empty() && (rest[0] == '\\' || rest[0] == '/')) rest.erase(0, 1);
    for (;;) {
        size_t sep = rest.find_first_of("/\\");
        std::string part = rest.substr(0, sep);
        bool last = sep == std::string::npos;
        bool found = false;
        for (u32 off = 0; off < dsize && !found;) {
            if (off % 2048 == 0 && !user_read(dlba + off / 2048, sec)) return false;
            const u8* r = sec + off % 2048;
            if (r[0] == 0) { off = (off / 2048 + 1) * 2048; continue; }
            std::string name((const char*)r + 33, r[32]);
            size_t semi = name.find(';');
            if (semi != std::string::npos) name.erase(semi);
            if (_stricmp(name.c_str(), part.c_str()) == 0) {
                memcpy(&dlba, r + 2, 4);
                memcpy(&dsize, r + 10, 4);
                found = true;
            }
            off += r[0];
        }
        if (!found) return false;
        if (last) break;
        rest.erase(0, sep + 1);
    }
    *lba = dlba;
    *size = dsize;
    return true;
}
bool cdrom_read_file(const char* path, std::vector<u8>& out) {
    u32 lba, size;
    if (!cdrom_find_file(path, &lba, &size)) return false;
    out.resize((size + 2047) & ~2047u);
    for (u32 i = 0; i < out.size() / 2048; i++) user_read(lba + i, out.data() + i * 2048);
    out.resize(size);
    return true;
}

// Replace (part of) a disc file with host data. offset must be a multiple of 2048 unless the
// sector's original contents are merged (done here), so arbitrary byte patches work too.
bool cdrom_add_override(const char* disc_path, u32 offset, const u8* data, u32 len) {
    u32 lba, size;
    if (!cdrom_find_file(disc_path, &lba, &size)) return false;
    u32 span = ((size + 2047) / 2048) * 2048;
    if (offset >= span) return false;
    if (offset + len > span) len = span - offset;
    u32 first = offset / 2048, last = (offset + len + 2047) / 2048;
    Override o;
    o.lba = lba + first;
    o.data.resize((size_t)(last - first) * 2048);
    for (u32 i = first; i < last; i++) {
        u8 raw[2352];
        disc_read(lba + i, raw);   // includes earlier overrides, so patches stack
        memcpy(o.data.data() + (size_t)(i - first) * 2048, raw + 24, 2048);
    }
    memcpy(o.data.data() + (offset - first * 2048), data, len);
    g_overrides.push_back(std::move(o));
    return true;
}
