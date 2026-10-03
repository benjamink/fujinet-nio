// src/lib/image_convert.cpp
#include "fujinet/io/devices/image_convert.h"
#include <algorithm>
#include <cstdlib>

namespace fujinet::io::image {

static bool to_int(const std::string& s, int lo, int hi, int& out) {
    if (s.empty() || s.size() > 4) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    int v = std::atoi(s.c_str());
    if (v < lo || v > hi) return false;
    out = v; return true;
}

bool parse_selector(const std::string& sel, Options& out) {
    Options o; unsigned seen = 0;
    static const char* keys[] = {"w","h","colors","base","par","dither","mode","up"};
    std::size_t pos = 0;
    while (pos < sel.size()) {
        std::size_t end = sel.find(',', pos);
        if (end == std::string::npos) end = sel.size();
        std::string kv = sel.substr(pos, end - pos);
        pos = end + 1;
        std::size_t eq = kv.find('=');
        if (eq == std::string::npos) return false;
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        int ki = -1;
        for (int i = 0; i < 8; ++i) if (k == keys[i]) ki = i;
        if (ki < 0 || (seen & (1u << ki))) return false;
        seen |= 1u << ki;
        bool ok = true;
        switch (ki) {
            case 0: ok = to_int(v, 16, 1024, o.w); break;
            case 1: ok = to_int(v, 16, 1024, o.h); break;
            case 2: ok = to_int(v, 2, 32, o.colors); break;
            case 3: ok = to_int(v, 0, 30, o.base); break;
            case 4: { std::size_t c = v.find(':');
                      ok = c != std::string::npos && to_int(v.substr(0, c), 1, 4, o.parX)
                           && to_int(v.substr(c + 1), 1, 4, o.parY); } break;
            case 5: if (v == "fs") o.dither = true; else if (v == "none") o.dither = false; else ok = false; break;
            case 6:
                if (v == "auto") o.mode = Options::Auto;
                else if (v == "gray") o.mode = Options::Gray;
                else if (v == "color") o.mode = Options::Color;
                else ok = false;
                break;
            case 7: if (v == "0") o.upscale = false; else if (v == "1") o.upscale = true; else ok = false; break;
        }
        if (!ok) return false;
    }
    if (o.base + o.colors > 32) return false;
    out = o; return true;
}

int planes_for(const Options& o) {
    int pens = o.base + o.colors, p = 1;
    while ((1 << p) < pens) ++p;
    return p;
}

Size fit_size(int srcW, int srcH, const Options& o) {
    // Display height of one output row is parY/parX of its width; source pixels are square.
    // Work in 1/1024 fixed point to stay integer-only.
    long sx = (long)o.w * 1024 / srcW;
    long sy = (long)o.h * o.parY * 1024 / ((long)srcH * o.parX);
    long s = std::min(sx, sy);
    if (!o.upscale) s = std::min(s, 1024L);
    int w = (int)std::max(1L, srcW * s / 1024);
    int h = (int)std::max(1L, (long)srcH * o.parX * s / (1024L * o.parY));
    return {std::min(w, o.w), std::min(h, o.h)};
}

std::vector<std::uint8_t> scale_rgb(const std::uint8_t* rgb, int sw, int sh, Size out) {
    std::vector<std::uint8_t> d((std::size_t)out.w * out.h * 3);
    for (int y = 0; y < out.h; ++y) {
        int y0 = (int)((long)y * sh / out.h), y1 = std::max(y0 + 1, (int)((long)(y + 1) * sh / out.h));
        for (int x = 0; x < out.w; ++x) {
            int x0 = (int)((long)x * sw / out.w), x1 = std::max(x0 + 1, (int)((long)(x + 1) * sw / out.w));
            long acc[3] = {0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx, ++n)
                    for (int c = 0; c < 3; ++c) acc[c] += rgb[((std::size_t)yy * sw + xx) * 3 + c];
            for (int c = 0; c < 3; ++c) d[((std::size_t)y * out.w + x) * 3 + c] = (std::uint8_t)(acc[c] / n);
        }
    }
    return d;
}

static bool is_gray(const std::vector<std::uint8_t>& rgb) {
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        int mx = std::max({rgb[i], rgb[i+1], rgb[i+2]}), mn = std::min({rgb[i], rgb[i+1], rgb[i+2]});
        if (mx - mn > 24) return false;
    }
    return true;
}

static std::uint8_t ocs(int v) { return (std::uint8_t)(((v + 8) / 17) * 17); }  // snap to 4-bit/channel

std::vector<Rgb> make_palette(const std::vector<std::uint8_t>& rgb, const Options& o) {
    std::vector<Rgb> pal;
    bool gray = o.mode == Options::Gray || (o.mode == Options::Auto && is_gray(rgb));
    if (gray) {
        for (int i = 0; i < o.colors; ++i) { auto v = ocs(i * 255 / (o.colors - 1)); pal.push_back({v, v, v}); }
        return pal;
    }
    // Median cut over the 4096-entry OCS histogram.
    struct Bin { int r, g, b; long n; };
    std::vector<long> hist(4096, 0);
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3)
        ++hist[((rgb[i] >> 4) << 8) | ((rgb[i+1] >> 4) << 4) | (rgb[i+2] >> 4)];
    std::vector<Bin> bins;
    for (int k = 0; k < 4096; ++k) if (hist[k]) bins.push_back({k >> 8, (k >> 4) & 15, k & 15, hist[k]});
    struct Box { std::size_t lo, hi; };
    std::vector<Box> boxes{{0, bins.size()}};
    auto range = [&](const Box& b, int ch) {
        int mn = 15, mx = 0;
        for (std::size_t i = b.lo; i < b.hi; ++i) { int v = ch == 0 ? bins[i].r : ch == 1 ? bins[i].g : bins[i].b; mn = std::min(mn, v); mx = std::max(mx, v); }
        return mx - mn;
    };
    while ((int)boxes.size() < o.colors) {
        int best = -1, bestCh = 0, bestR = 0;
        for (int i = 0; i < (int)boxes.size(); ++i) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            for (int ch = 0; ch < 3; ++ch) { int r = range(boxes[i], ch); if (r > bestR) { bestR = r; best = i; bestCh = ch; } }
        }
        if (best < 0) break;
        Box b = boxes[best];
        std::sort(bins.begin() + b.lo, bins.begin() + b.hi, [bestCh](const Bin& a, const Bin& c) {
            return (bestCh == 0 ? a.r : bestCh == 1 ? a.g : a.b) < (bestCh == 0 ? c.r : bestCh == 1 ? c.g : c.b); });
        long total = 0; for (std::size_t i = b.lo; i < b.hi; ++i) total += bins[i].n;
        long acc = 0; std::size_t mid = b.lo;
        while (mid < b.hi - 1 && acc + bins[mid].n <= total / 2) acc += bins[mid++].n;
        if (mid == b.lo) ++mid;
        boxes[best] = {b.lo, mid}; boxes.push_back({mid, b.hi});
    }
    for (auto& b : boxes) {
        long r = 0, g = 0, bl = 0, n = 0;
        for (std::size_t i = b.lo; i < b.hi; ++i) { r += bins[i].r * bins[i].n; g += bins[i].g * bins[i].n; bl += bins[i].b * bins[i].n; n += bins[i].n; }
        if (n) pal.push_back({(std::uint8_t)(r / n * 17), (std::uint8_t)(g / n * 17), (std::uint8_t)(bl / n * 17)});
    }
    while ((int)pal.size() < o.colors) pal.push_back({0, 0, 0});
    return pal;
}

static int nearest(const std::vector<Rgb>& pal, int r, int g, int b) {
    int best = 0; long bd = 1L << 30;
    for (int i = 0; i < (int)pal.size(); ++i) {
        long dr = r - pal[i].r, dg = g - pal[i].g, db = b - pal[i].b;
        long d = 3 * dr * dr + 6 * dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

std::vector<std::uint8_t> map_pixels(const std::vector<std::uint8_t>& rgb, Size s,
                                     const std::vector<Rgb>& pal, const Options& o) {
    std::vector<std::uint8_t> idx((std::size_t)s.w * s.h);
    std::vector<int> err((std::size_t)(s.w + 2) * 2 * 3, 0);   // two error rows, 1px border each side
    for (int y = 0; y < s.h; ++y) {
        int* cur = &err[(std::size_t)(y & 1) * (s.w + 2) * 3];
        int* nxt = &err[(std::size_t)((y + 1) & 1) * (s.w + 2) * 3];
        std::fill(nxt, nxt + (s.w + 2) * 3, 0);
        for (int x = 0; x < s.w; ++x) {
            const std::uint8_t* p = &rgb[((std::size_t)y * s.w + x) * 3];
            int c[3];
            for (int k = 0; k < 3; ++k) c[k] = std::clamp(p[k] + (o.dither ? cur[(x + 1) * 3 + k] / 16 : 0), 0, 255);
            int i = nearest(pal, c[0], c[1], c[2]);
            idx[(std::size_t)y * s.w + x] = (std::uint8_t)(o.base + i);
            if (!o.dither) continue;
            int e[3] = {c[0] - pal[i].r, c[1] - pal[i].g, c[2] - pal[i].b};
            for (int k = 0; k < 3; ++k) {
                cur[(x + 2) * 3 + k] += e[k] * 7;
                nxt[(x)     * 3 + k] += e[k] * 3;
                nxt[(x + 1) * 3 + k] += e[k] * 5;
                nxt[(x + 2) * 3 + k] += e[k] * 1;
            }
        }
    }
    return idx;
}

std::vector<std::uint8_t> byterun1(const std::uint8_t* row, std::size_t n) {
    std::vector<std::uint8_t> out;
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) ++run;
        if (run >= 2) { out.push_back((std::uint8_t)(257 - run)); out.push_back(row[i]); i += run; continue; }
        std::size_t lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && row[i + lit] == row[i + lit + 1])) ++lit;
        out.push_back((std::uint8_t)(lit - 1));
        out.insert(out.end(), row + i, row + i + lit);
        i += lit;
    }
    return out;
}

static void put32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((std::uint8_t)(x >> s)); }
static void put16(std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back((std::uint8_t)(x >> 8)); v.push_back((std::uint8_t)x); }
static void tag(std::vector<std::uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); }

std::vector<std::uint8_t> write_ilbm(const std::vector<std::uint8_t>& idx, Size s,
                                     const std::vector<Rgb>& pal, const Options& o) {
    const int planes = planes_for(o), bpr = ((s.w + 15) / 16) * 2;
    std::vector<std::uint8_t> f;
    tag(f, "FORM"); put32(f, 0); tag(f, "ILBM");
    tag(f, "BMHD"); put32(f, 20);
    put16(f, (std::uint16_t)s.w); put16(f, (std::uint16_t)s.h); put16(f, 0); put16(f, 0);
    f.push_back((std::uint8_t)planes); f.push_back(0); f.push_back(1); f.push_back(0);
    put16(f, 0); f.push_back((std::uint8_t)o.parX); f.push_back((std::uint8_t)o.parY);
    put16(f, (std::uint16_t)s.w); put16(f, (std::uint16_t)s.h);
    tag(f, "CMAP"); put32(f, 3u << planes);
    for (int i = 0; i < (1 << planes); ++i) {
        int k = i - o.base;
        if (k >= 0 && k < (int)pal.size()) { f.push_back(pal[k].r); f.push_back(pal[k].g); f.push_back(pal[k].b); }
        else { f.push_back(0); f.push_back(0); f.push_back(0); }
    }
    tag(f, "BODY"); std::size_t bodyLenAt = f.size(); put32(f, 0);
    std::vector<std::uint8_t> row(bpr);
    for (int y = 0; y < s.h; ++y)
        for (int p = 0; p < planes; ++p) {
            std::fill(row.begin(), row.end(), 0);
            for (int x = 0; x < s.w; ++x)
                if (idx[(std::size_t)y * s.w + x] & (1 << p)) row[x >> 3] |= (std::uint8_t)(0x80 >> (x & 7));
            auto e = byterun1(row.data(), row.size());
            f.insert(f.end(), e.begin(), e.end());
        }
    std::uint32_t bodyLen = (std::uint32_t)(f.size() - bodyLenAt - 4);
    for (int k = 0; k < 4; ++k) f[bodyLenAt + k] = (std::uint8_t)(bodyLen >> (24 - 8 * k));
    if (bodyLen & 1) f.push_back(0);
    std::uint32_t formLen = (std::uint32_t)(f.size() - 8);
    for (int k = 0; k < 4; ++k) f[4 + k] = (std::uint8_t)(formLen >> (24 - 8 * k));
    return f;
}
}
