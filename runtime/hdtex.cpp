// HD texture replacement, compatible with DuckStation "texupload-*" replacement packs.
// A replacement is keyed by the XXH3 hash of the VRAM upload that carries the texture, the hash of
// the palette it is drawn with, and a texel sub-rectangle inside that upload.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include "gpu.h"
#include "hdtex.h"
#define XXH_INLINE_ALL
#include "../third_party/xxhash.h"
#include <unordered_map>
#include <algorithm>

struct RepName {
    u64 src_hash, pal_hash;
    int mode, stp;            // mode: 0 = 4bpp, 1 = 8bpp
    int sw, sh;               // upload size in VRAM words
    int ox, oy, w, h;         // texel rect inside the upload
    int pmin, pmax;
    std::wstring path;
    ID3D11ShaderResourceView* srv = nullptr;
    bool failed = false;
};
struct Write { int x, y, w, h; u64 hash; };

static std::unordered_multimap<u64, RepName*> g_names;   // key: src_hash ^ mode
static std::vector<Write> g_writes;
static std::unordered_map<u64, std::vector<HdSub>> g_cache;
static IWICImagingFactory* g_wic;
static int g_stat_uploads, g_stat_hits, g_stat_loaded;
bool g_hd_on;
ID3D11Device* hw_device();
ID3D11DeviceContext* hw_context();

static bool parse_name(const std::wstring& file, RepName& n) {
    // texupload-<P4|P8|STP4|STP8>-<src>-<pal>-<sw>x<sh>-<ox>-<oy>-<w>x<h>-P<min>-<max>.png
    char buf[300];
    size_t len = std::min(file.size(), sizeof buf - 1);
    for (size_t i = 0; i < len; i++) buf[i] = (char)file[i];
    buf[len] = 0;
    char mode[16];
    unsigned long long sh, ph;
    if (sscanf(buf, "texupload-%15[A-Z0-9]-%16llX-%16llX-%dx%d-%d-%d-%dx%d-P%d-%d", mode, &sh, &ph, &n.sw, &n.sh, &n.ox, &n.oy, &n.w, &n.h, &n.pmin, &n.pmax) != 11)
        return false;
    n.src_hash = sh;
    n.pal_hash = ph;
    n.stp = !strncmp(mode, "ST", 2);
    const char* m = n.stp ? mode + 2 : mode;
    if (!strcmp(m, "P4")) n.mode = 0;
    else if (!strcmp(m, "P8")) n.mode = 1;
    else return false;
    return n.w > 0 && n.h > 0;
}

static void scan_dir(const std::wstring& dir, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 6) scan_dir(dir + L"\\" + name, depth + 1); continue; }
        RepName n;
        if (!parse_name(name, n)) continue;
        RepName* p = new RepName(n);
        p->path = dir + L"\\" + name;
        g_names.emplace(n.src_hash ^ (u64)n.mode, p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void hdtex_init(const std::string& dir) {
    if (dir.empty()) return;
    std::wstring w(dir.begin(), dir.end());
    scan_dir(w, 0);
    g_hd_on = !g_names.empty();
    logf("hd textures: %d replacement images indexed from %s\n", (int)g_names.size(), dir.c_str());
}

static u64 hash_rect(int x, int y, int w, int h) {
    XXH3_state_t st;
    XXH3_64bits_reset(&st);
    for (int j = 0; j < h; j++) XXH3_64bits_update(&st, &g_vram[(y + j) * 1024 + x], (size_t)w * 2);
    return XXH3_64bits_digest(&st);
}

void hdtex_on_upload(int x, int y, int w, int h) {
    if (!g_hd_on) return;
    if (x + w > 1024) w = 1024 - x;
    if (y + h > 512) h = 512 - y;
    if (w <= 0 || h <= 0) return;
    // an upload replaces whatever it overlaps
    g_writes.erase(std::remove_if(g_writes.begin(), g_writes.end(), [&](const Write& o) {
        return !(o.x >= x + w || o.x + o.w <= x || o.y >= y + h || o.y + o.h <= y);
    }), g_writes.end());
    Write wr = {x, y, w, h, hash_rect(x, y, w, h)};
    g_writes.push_back(wr);
    g_cache.clear();
    g_stat_uploads++;
    if (g_names.count(wr.hash) || g_names.count(wr.hash ^ 1)) g_stat_hits++;
}

void hdtex_invalidate(int x, int y, int w, int h) {
    if (!g_hd_on || g_writes.empty()) return;
    size_t before = g_writes.size();
    g_writes.erase(std::remove_if(g_writes.begin(), g_writes.end(), [&](const Write& o) {
        return !(o.x >= x + w || o.x + o.w <= x || o.y >= y + h || o.y + o.h <= y);
    }), g_writes.end());
    if (g_writes.size() != before) g_cache.clear();
}

void hdtex_stats() {
    if (g_hd_on) logf("hd textures: %d uploads seen, %d matched the pack, %d images loaded\n", g_stat_uploads, g_stat_hits, g_stat_loaded);
}

static bool load_png(RepName* n) {
    if (n->srv) return true;
    if (n->failed) return false;
    n->failed = true;
    ID3D11Device* dev = hw_device();
    if (!dev) return false;
    if (!g_wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(n->path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w, h;
        conv->GetSize(&w, &h);
        std::vector<u8> px((size_t)w * h * 4);
        if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex))) {
                hw_context()->UpdateSubresource(tex, 0, nullptr, px.data(), w * 4, 0);
                if (SUCCEEDED(dev->CreateShaderResourceView(tex, nullptr, &n->srv))) {
                    hw_context()->GenerateMips(n->srv);
                    ok = true;
                    g_stat_loaded++;
                }
                tex->Release();
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    n->failed = !ok;
    return ok;
}

static bool palette_ok(const RepName* n, const GpuState& s) {
    int full = n->mode == 0 ? 15 : 255;
    if (s.clutx + n->pmax >= 1024) return false;
    // DuckStation hashes (max - min + 1) entries starting at the first CLUT entry
    int count = (n->pmin == 0 && n->pmax == full) ? full + 1 : n->pmax - n->pmin + 1;
    return XXH3_64bits(&g_vram[s.cluty * 1024 + s.clutx], (size_t)count * 2) == n->pal_hash;
}

// candidates for one (page, palette, mode), expressed in page texel coordinates
static const std::vector<HdSub>& candidates(const GpuState& s) {
    u64 key = (u64)s.tpx | ((u64)s.tpy << 10) | ((u64)s.clutx << 20) | ((u64)s.cluty << 30) | ((u64)s.depth << 40);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    std::vector<HdSub>& out = g_cache[key];
    int shift = s.depth == 0 ? 2 : 1;
    int pw = 256 >> shift;  // page width in VRAM words
    for (const Write& wr : g_writes) {
        if (wr.x >= s.tpx + pw || wr.x + wr.w <= s.tpx || wr.y >= s.tpy + 256 || wr.y + wr.h <= s.tpy) continue;
        auto range = g_names.equal_range(wr.hash ^ (u64)s.depth);
        for (auto r = range.first; r != range.second; ++r) {
            RepName* n = r->second;
            if (n->src_hash != wr.hash || n->mode != s.depth || n->sw != wr.w || n->sh != wr.h) continue;
            if (!palette_ok(n, s)) continue;
            HdSub sub;
            sub.u0 = ((wr.x - s.tpx) << shift) + n->ox;
            sub.v0 = (wr.y - s.tpy) + n->oy;
            sub.w = n->w;
            sub.h = n->h;
            sub.stp = n->stp;
            sub.name = n;
            sub.srv = nullptr;
            out.push_back(sub);
        }
    }
    return out;
}

const HdSub* hdtex_lookup(const GpuState& s, int u0, int v0, int u1, int v1) {
    if (!g_hd_on || !g_cfg.hd_enabled || !s.textured || s.depth > 1 || s.tw_mask_x || s.tw_mask_y) return nullptr;
    const std::vector<HdSub>& c = candidates(s);
    const HdSub* best = nullptr;
    for (const HdSub& sub : c) {
        if (u0 < sub.u0 || v0 < sub.v0 || u1 >= sub.u0 + sub.w || v1 >= sub.v0 + sub.h) continue;
        // prefer the variant matching the draw's transparency mode
        if (!best || (sub.stp != 0) == s.semi) best = &sub;
    }
    if (!best) return nullptr;
    HdSub* b = const_cast<HdSub*>(best);
    if (!b->srv) {
        RepName* n = (RepName*)b->name;
        if (!load_png(n)) return nullptr;
        b->srv = n->srv;
    }
    return best;
}
