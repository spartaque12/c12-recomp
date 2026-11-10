// Software reference rasterizer (native resolution, PSX-accurate rules).
#include "gpu.h"

u16 g_vram[1024 * 512];

static const s8 dither_tab[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline u16 fetch_texel(const GpuState& s, int u, int v) {
    u = (u & ~s.tw_mask_x) | (s.tw_off_x & s.tw_mask_x);
    v = (v & ~s.tw_mask_y) | (s.tw_off_y & s.tw_mask_y);
    u &= 255; v &= 255;
    int ty = (s.tpy + v) & 511;
    if (s.depth == 0) {
        u16 w = g_vram[ty * 1024 + ((s.tpx + (u >> 2)) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return g_vram[s.cluty * 1024 + ((s.clutx + idx) & 1023)];
    }
    if (s.depth == 1) {
        u16 w = g_vram[ty * 1024 + ((s.tpx + (u >> 1)) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return g_vram[s.cluty * 1024 + ((s.clutx + idx) & 1023)];
    }
    return g_vram[ty * 1024 + ((s.tpx + u) & 1023)];
}

static inline int clamp5(int v) { return v < 0 ? 0 : v > 31 ? 31 : v; }

// r,g,b are 8-bit vertex colours. texel = 0 means "no texture".
static inline void put_pixel(const GpuState& s, int x, int y, int r, int g, int b, bool has_tex, u16 texel) {
    u16* dst = &g_vram[(y & 511) * 1024 + (x & 1023)];
    if (s.check_mask && (*dst & 0x8000)) return;
    int R, G, B;
    u16 mask = 0;
    bool semi = s.semi;
    if (has_tex) {
        if (texel == 0) return;
        mask = texel & 0x8000;
        if (!mask) semi = false;
        int tr = texel & 31, tg = (texel >> 5) & 31, tb = (texel >> 10) & 31;
        if (s.raw) { R = tr; G = tg; B = tb; }
        else {
            int d = s.dither ? dither_tab[y & 3][x & 3] : 0;
            R = clamp5(((tr * r) >> 4) + d >> 3);
            G = clamp5(((tg * g) >> 4) + d >> 3);
            B = clamp5(((tb * b) >> 4) + d >> 3);
        }
    } else {
        int d = s.dither ? dither_tab[y & 3][x & 3] : 0;
        R = clamp5((r + d) >> 3);
        G = clamp5((g + d) >> 3);
        B = clamp5((b + d) >> 3);
    }
    if (semi) {
        int br = *dst & 31, bg = (*dst >> 5) & 31, bb = (*dst >> 10) & 31;
        switch (s.blend) {
        case 0: R = (br + R) >> 1; G = (bg + G) >> 1; B = (bb + B) >> 1; break;
        case 1: R = clamp5(br + R); G = clamp5(bg + G); B = clamp5(bb + B); break;
        case 2: R = clamp5(br - R); G = clamp5(bg - G); B = clamp5(bb - B); break;
        default: R = clamp5(br + (R >> 2)); G = clamp5(bg + (G >> 2)); B = clamp5(bb + (B >> 2)); break;
        }
    }
    *dst = (u16)(R | (G << 5) | (B << 10) | mask | (s.set_mask ? 0x8000 : 0));
}

static void sw_tri(const GpuVtx* v, const GpuState& s) {
    const GpuVtx *a = &v[0], *b = &v[1], *c = &v[2];
    s64 area = (s64)(b->x - a->x) * (c->y - a->y) - (s64)(c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    if (area < 0) { const GpuVtx* t = b; b = c; c = t; area = -area; }
    int minx = a->x < b->x ? a->x : b->x; if (c->x < minx) minx = c->x;
    int maxx = a->x > b->x ? a->x : b->x; if (c->x > maxx) maxx = c->x;
    int miny = a->y < b->y ? a->y : b->y; if (c->y < miny) miny = c->y;
    int maxy = a->y > b->y ? a->y : b->y; if (c->y > maxy) maxy = c->y;
    if (maxx - minx > 1023 || maxy - miny > 511) return;
    if (minx < s.cx1) minx = s.cx1;
    if (miny < s.cy1) miny = s.cy1;
    if (maxx > s.cx2 + 1) maxx = s.cx2 + 1;
    if (maxy > s.cy2 + 1) maxy = s.cy2 + 1;
    // edge functions (a->b, b->c, c->a), top-left fill rule
    auto edge = [](const GpuVtx* p, const GpuVtx* q, int x, int y) -> s64 {
        return (s64)(q->x - p->x) * (y - p->y) - (s64)(q->y - p->y) * (x - p->x);
    };
    auto bias = [](const GpuVtx* p, const GpuVtx* q) -> int {
        int dx = q->x - p->x, dy = q->y - p->y;
        // with positive area (clockwise in y-down), left edges go up (dy<0), top edges go right...
        bool topleft = (dy < 0) || (dy == 0 && dx > 0);
        return topleft ? 0 : -1;
    };
    int b0 = bias(b, c), b1 = bias(c, a), b2 = bias(a, b);
    int ar = a->rgb & 255, ag = (a->rgb >> 8) & 255, ab = (a->rgb >> 16) & 255;
    int br = b->rgb & 255, bg = (b->rgb >> 8) & 255, bb = (b->rgb >> 16) & 255;
    int cr = c->rgb & 255, cg = (c->rgb >> 8) & 255, cb = (c->rgb >> 16) & 255;
    for (int y = miny; y < maxy; y++) {
        for (int x = minx; x < maxx; x++) {
            s64 w0 = edge(b, c, x, y), w1 = edge(c, a, x, y), w2 = edge(a, b, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            int r, g, bl;
            if (s.gouraud) {
                r = (int)((w0 * ar + w1 * br + w2 * cr) / area);
                g = (int)((w0 * ag + w1 * bg + w2 * cg) / area);
                bl = (int)((w0 * ab + w1 * bb + w2 * cb) / area);
            } else { r = v[0].rgb & 255; g = (v[0].rgb >> 8) & 255; bl = (v[0].rgb >> 16) & 255; }
            if (s.textured) {
                int tu = (int)((w0 * a->u + w1 * b->u + w2 * c->u) / area);
                int tv = (int)((w0 * a->v + w1 * b->v + w2 * c->v) / area);
                put_pixel(s, x, y, r, g, bl, true, fetch_texel(s, tu, tv));
            } else put_pixel(s, x, y, r, g, bl, false, 0);
        }
    }
}

static void sw_rect(int x, int y, int w, int h, int u, int v, u32 rgb, const GpuState& s) {
    int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < s.cy1 || py > s.cy2) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < s.cx1 || px > s.cx2) continue;
            if (s.textured) put_pixel(s, px, py, r, g, b, true, fetch_texel(s, u + i, v + j));
            else put_pixel(s, px, py, r, g, b, false, 0);
        }
    }
}

static void sw_line(const GpuVtx& a, const GpuVtx& b, const GpuState& s) {
    int dx = b.x - a.x, dy = b.y - a.y;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx > 1023 || ady > 511) return;
    int n = adx > ady ? adx : ady;
    int ar = a.rgb & 255, ag = (a.rgb >> 8) & 255, ab = (a.rgb >> 16) & 255;
    int br = b.rgb & 255, bg = (b.rgb >> 8) & 255, bb = (b.rgb >> 16) & 255;
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (dx * i + (dx >= 0 ? n / 2 : -n / 2)) / n : a.x;
        int y = n ? a.y + (dy * i + (dy >= 0 ? n / 2 : -n / 2)) / n : a.y;
        if (x < s.cx1 || x > s.cx2 || y < s.cy1 || y > s.cy2) continue;
        int r = ar, g = ag, bl = ab;
        if (s.gouraud && n) { r = ar + (br - ar) * i / n; g = ag + (bg - ag) * i / n; bl = ab + (bb - ab) * i / n; }
        put_pixel(s, x, y, r, g, bl, false, 0);
    }
}

static void sw_fill(int x, int y, int w, int h, u32 rgb) {
    u16 c = (u16)(((rgb >> 3) & 31) | (((rgb >> 11) & 31) << 5) | (((rgb >> 19) & 31) << 10));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)] = c;
}

static void sw_copy(int sx, int sy, int dx, int dy, int w, int h, const GpuState& s) {
    static u16 tmp[1024 * 512];
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            tmp[j * w + i] = g_vram[((sy + j) & 511) * 1024 + ((sx + i) & 1023)];
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u16* d = &g_vram[((dy + j) & 511) * 1024 + ((dx + i) & 1023)];
            if (s.check_mask && (*d & 0x8000)) continue;
            *d = tmp[j * w + i] | (s.set_mask ? 0x8000 : 0);
        }
}

static void sw_upload(int x, int y, int w, int h, const u16* data, const GpuState& s) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u16* d = &g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
            if (s.check_mask && (*d & 0x8000)) continue;
            *d = data[j * w + i] | (s.set_mask ? 0x8000 : 0);
        }
}

static void sw_download(int x, int y, int w, int h, u16* out) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            out[j * w + i] = g_vram[((y + j) & 511) * 1024 + ((x + i) & 1023)];
}

static std::vector<u32> g_frame_px;
static void build_frame(const GpuDisplay& d) {
    g_frame_px.resize((size_t)d.w * d.h);
    for (int y = 0; y < d.h; y++) {
        for (int x = 0; x < d.w; x++) {
            u32 px = 0;
            if (!d.blank) {
                if (d.rgb24) {
                    const u8* p = (const u8*)&g_vram[((d.y + y) & 511) * 1024] + d.x * 2 + x * 3;
                    px = 0xFF000000u | (p[0] << 16) | (p[1] << 8) | p[2];
                } else {
                    u16 c = g_vram[((d.y + y) & 511) * 1024 + ((d.x + x) & 1023)];
                    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                    px = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
                }
            }
            g_frame_px[(size_t)y * d.w + x] = px;
        }
    }
}

static void sw_present(const GpuDisplay& d) {
    build_frame(d);
    plat_present_sw(g_frame_px.data(), d.w, d.h);
}

bool write_bmp(const char* path, const u32* bgra, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    u32 row = (w * 3 + 3) & ~3, size = 54 + row * h;
    u8 hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    u32 off = 54, ih = 40;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<u8> line(row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            u32 p = bgra[(size_t)y * w + x];
            line[x * 3] = (u8)p;
            line[x * 3 + 1] = (u8)(p >> 8);
            line[x * 3 + 2] = (u8)(p >> 16);
        }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

static bool sw_screenshot(const GpuDisplay& d, const char* path) {
    build_frame(d);
    return write_bmp(path, g_frame_px.data(), d.w, d.h);
}

GpuBackend g_sw_backend = {sw_tri, sw_rect, sw_line, sw_fill, sw_copy, sw_upload, sw_download, sw_present, sw_screenshot};
