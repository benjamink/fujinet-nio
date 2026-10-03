// src/lib/image_pipeline.cpp
#include "fujinet/io/devices/image_pipeline.h"

#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <new>

namespace fujinet::io::image {

namespace {

// ---------------------------------------------------------------------------
// Selector
// ---------------------------------------------------------------------------

constexpr int kMaxPens = 32;

enum SelectorKey : int {
    KeyWidth,
    KeyHeight,
    KeyColors,
    KeyBase,
    KeyPar,
    KeyDither,
    KeyMode,
    KeyUpscale,
    KeyFormat,
    KeyBits,
    KeyCount,
};

constexpr const char* kSelectorKeyNames[KeyCount] = {
    "w", "h", "colors", "base", "par", "dither", "mode", "up", "fmt", "bits",
};

int find_selector_key(const std::string& name)
{
    for (int i = 0; i < KeyCount; ++i) {
        if (name == kSelectorKeyNames[i]) {
            return i;
        }
    }
    return -1;
}

// Decimal digits only, at most four of them, within lo..hi.
bool parse_int(const std::string& text, int lo, int hi, int& out)
{
    if (text.empty() || text.size() > 4) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const int value = std::atoi(text.c_str());
    if (value < lo || value > hi) {
        return false;
    }
    out = value;
    return true;
}

bool parse_par(const std::string& text, Options& o)
{
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    if (!parse_int(text.substr(0, colon), 1, 4, o.parX)) {
        return false;
    }
    return parse_int(text.substr(colon + 1), 1, 4, o.parY);
}

bool parse_dither(const std::string& text, Options& o)
{
    if (text == "fs") {
        o.dither = true;
        return true;
    }
    if (text == "none") {
        o.dither = false;
        return true;
    }
    return false;
}

bool parse_mode(const std::string& text, Options& o)
{
    if (text == "auto") {
        o.mode = ColourMode::Auto;
        return true;
    }
    if (text == "gray") {
        o.mode = ColourMode::Gray;
        return true;
    }
    if (text == "color") {
        o.mode = ColourMode::Color;
        return true;
    }
    return false;
}

bool parse_upscale(const std::string& text, Options& o)
{
    if (text == "0") {
        o.upscale = false;
        return true;
    }
    if (text == "1") {
        o.upscale = true;
        return true;
    }
    return false;
}

// Output formats a writer exists for. Adding a writer adds a name here.
bool parse_format(const std::string& text, Options& o)
{
    if (text == "ilbm") {
        o.format = OutputFormat::Ilbm;
        return true;
    }
    return false;
}

bool apply_selector_value(int key, const std::string& value, Options& o)
{
    switch (key) {
        case KeyWidth:
            return parse_int(value, 16, 1024, o.w);
        case KeyHeight:
            return parse_int(value, 16, 1024, o.h);
        case KeyColors:
            return parse_int(value, 2, kMaxPens, o.colors);
        case KeyBase:
            return parse_int(value, 0, kMaxPens - 2, o.base);
        case KeyPar:
            return parse_par(value, o);
        case KeyDither:
            return parse_dither(value, o);
        case KeyMode:
            return parse_mode(value, o);
        case KeyUpscale:
            return parse_upscale(value, o);
        case KeyFormat:
            return parse_format(value, o);
        case KeyBits:
            return parse_int(value, 1, kMaxPaletteBits, o.bits);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

constexpr int kGrayTolerance = 24;

bool is_gray(const std::vector<std::uint8_t>& rgb)
{
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        const int hi = std::max({rgb[i], rgb[i + 1], rgb[i + 2]});
        const int lo = std::min({rgb[i], rgb[i + 1], rgb[i + 2]});
        if (hi - lo > kGrayTolerance) {
            return false;
        }
    }
    return true;
}

std::vector<Rgb> make_gray_palette(const Options& o)
{
    std::vector<Rgb> palette;
    palette.reserve(static_cast<std::size_t>(o.colors));
    for (int i = 0; i < o.colors; ++i) {
        const std::uint8_t v = snap_channel(i * 255 / (o.colors - 1), o.bits);
        palette.push_back({v, v, v});
    }
    return palette;
}

// Median cut runs on a histogram with 4 bits per channel (4096 bins).
constexpr int kHistBits = 4;
constexpr int kHistLevels = 1 << kHistBits;
constexpr int kHistBins = kHistLevels * kHistLevels * kHistLevels;
constexpr int kHistScale = 255 / (kHistLevels - 1);    // 17: bin value to 8-bit

struct Bin {
    int r;
    int g;
    int b;
    long count;
    // Sums of the 8-bit pixel values in the bin; only kept when the palette
    // is finer than the histogram (bits > kHistBits).
    std::uint64_t sumR;
    std::uint64_t sumG;
    std::uint64_t sumB;
};

struct Box {
    std::size_t lo;
    std::size_t hi;
};

int bin_channel(const Bin& bin, int channel)
{
    if (channel == 0) {
        return bin.r;
    }
    if (channel == 1) {
        return bin.g;
    }
    return bin.b;
}

std::size_t bin_key(const std::uint8_t* px)
{
    const int shift = 8 - kHistBits;
    const int r = px[0] >> shift;
    const int g = px[1] >> shift;
    const int b = px[2] >> shift;
    return static_cast<std::size_t>((r << (2 * kHistBits)) | (g << kHistBits) | b);
}

std::vector<Bin> collect_bins(const std::vector<std::uint8_t>& rgb, bool keepSums)
{
    std::vector<long> hist(kHistBins, 0);
    for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
        ++hist[bin_key(&rgb[i])];
    }

    // Per-bin channel sums, three per bin, for the exact box means.
    std::vector<std::uint64_t> sums;
    if (keepSums) {
        sums.assign(static_cast<std::size_t>(kHistBins) * 3, 0);
        for (std::size_t i = 0; i + 2 < rgb.size(); i += 3) {
            const std::size_t at = bin_key(&rgb[i]) * 3;
            sums[at] += rgb[i];
            sums[at + 1] += rgb[i + 1];
            sums[at + 2] += rgb[i + 2];
        }
    }

    std::vector<Bin> bins;
    for (int k = 0; k < kHistBins; ++k) {
        if (hist[static_cast<std::size_t>(k)] == 0) {
            continue;
        }
        Bin bin{};
        bin.r = k >> (2 * kHistBits);
        bin.g = (k >> kHistBits) & (kHistLevels - 1);
        bin.b = k & (kHistLevels - 1);
        bin.count = hist[static_cast<std::size_t>(k)];
        if (keepSums) {
            const std::size_t at = static_cast<std::size_t>(k) * 3;
            bin.sumR = sums[at];
            bin.sumG = sums[at + 1];
            bin.sumB = sums[at + 2];
        }
        bins.push_back(bin);
    }
    return bins;
}

int channel_range(const std::vector<Bin>& bins, const Box& box, int channel)
{
    int lo = kHistLevels - 1;
    int hi = 0;
    for (std::size_t i = box.lo; i < box.hi; ++i) {
        const int v = bin_channel(bins[i], channel);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return hi - lo;
}

// Pick the box and channel with the widest range. Boxes holding a single bin
// cannot be split. Returns false when no box can be split.
bool pick_box_to_split(const std::vector<Bin>& bins,
                       const std::vector<Box>& boxes,
                       std::size_t& boxIndex,
                       int& channel)
{
    bool found = false;
    int bestRange = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].hi - boxes[i].lo < 2) {
            continue;
        }
        for (int ch = 0; ch < 3; ++ch) {
            const int range = channel_range(bins, boxes[i], ch);
            if (range > bestRange) {
                bestRange = range;
                boxIndex = i;
                channel = ch;
                found = true;
            }
        }
    }
    return found;
}

// Sort the box's bins along `channel` and split it where half the pixels lie
// on each side. Returns the index of the first bin of the upper half.
std::size_t split_point(std::vector<Bin>& bins, const Box& box, int channel)
{
    const auto first = bins.begin() + static_cast<std::ptrdiff_t>(box.lo);
    const auto last = bins.begin() + static_cast<std::ptrdiff_t>(box.hi);
    std::sort(first, last, [channel](const Bin& a, const Bin& b) {
        return bin_channel(a, channel) < bin_channel(b, channel);
    });

    long total = 0;
    for (std::size_t i = box.lo; i < box.hi; ++i) {
        total += bins[i].count;
    }

    long below = 0;
    std::size_t mid = box.lo;
    while (mid < box.hi - 1 && below + bins[mid].count <= total / 2) {
        below += bins[mid].count;
        ++mid;
    }
    if (mid == box.lo) {
        ++mid;
    }
    return mid;
}

// The pixel-weighted mean bin of the box, scaled to 8 bits per channel. This
// is already on the 4-bit grid, so it is exact for bits <= kHistBits.
Rgb box_bin_mean(const std::vector<Bin>& bins, const Box& box)
{
    long r = 0;
    long g = 0;
    long b = 0;
    long n = 0;
    for (std::size_t i = box.lo; i < box.hi; ++i) {
        r += bins[i].r * bins[i].count;
        g += bins[i].g * bins[i].count;
        b += bins[i].b * bins[i].count;
        n += bins[i].count;
    }
    return {
        static_cast<std::uint8_t>(r / n * kHistScale),
        static_cast<std::uint8_t>(g / n * kHistScale),
        static_cast<std::uint8_t>(b / n * kHistScale),
    };
}

// The mean of the box's pixels themselves, rounded to 8 bits per channel.
Rgb box_pixel_mean(const std::vector<Bin>& bins, const Box& box)
{
    std::uint64_t r = 0;
    std::uint64_t g = 0;
    std::uint64_t b = 0;
    std::uint64_t n = 0;
    for (std::size_t i = box.lo; i < box.hi; ++i) {
        r += bins[i].sumR;
        g += bins[i].sumG;
        b += bins[i].sumB;
        n += static_cast<std::uint64_t>(bins[i].count);
    }
    return {
        static_cast<std::uint8_t>((r + n / 2) / n),
        static_cast<std::uint8_t>((g + n / 2) / n),
        static_cast<std::uint8_t>((b + n / 2) / n),
    };
}

// A box's palette entry, on the 2^bits grid. Up to the histogram's own 4 bits
// the mean bin is used (bits=4 is the original OCS palette); above that the
// mean of the actual pixels, so bits=8 gives exact 24-bit colours.
Rgb box_colour(const std::vector<Bin>& bins, const Box& box, int bits)
{
    const Rgb mean = bits > kHistBits ? box_pixel_mean(bins, box) : box_bin_mean(bins, box);
    return {snap_channel(mean.r, bits), snap_channel(mean.g, bits), snap_channel(mean.b, bits)};
}

std::vector<Rgb> make_colour_palette(const std::vector<std::uint8_t>& rgb, const Options& o)
{
    std::vector<Bin> bins = collect_bins(rgb, o.bits > kHistBits);
    std::vector<Box> boxes{{0, bins.size()}};

    while (static_cast<int>(boxes.size()) < o.colors) {
        std::size_t boxIndex = 0;
        int channel = 0;
        if (!pick_box_to_split(bins, boxes, boxIndex, channel)) {
            break;
        }
        const Box box = boxes[boxIndex];
        const std::size_t mid = split_point(bins, box, channel);
        boxes[boxIndex] = {box.lo, mid};
        boxes.push_back({mid, box.hi});
    }

    std::vector<Rgb> palette;
    palette.reserve(static_cast<std::size_t>(o.colors));
    for (const Box& box : boxes) {
        if (box.hi > box.lo) {
            palette.push_back(box_colour(bins, box, o.bits));
        }
    }
    while (static_cast<int>(palette.size()) < o.colors) {
        palette.push_back({0, 0, 0});
    }
    return palette;
}

// ---------------------------------------------------------------------------
// Quantise and dither
// ---------------------------------------------------------------------------

// Nearest palette entry by squared distance weighted 3:6:1 (R:G:B).
int nearest_colour(const std::vector<Rgb>& palette, int r, int g, int b)
{
    int best = 0;
    long bestDistance = 1L << 30;
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const long dr = r - palette[i].r;
        const long dg = g - palette[i].g;
        const long db = b - palette[i].b;
        const long distance = 3 * dr * dr + 6 * dg * dg + db * db;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Floyd-Steinberg keeps two rows of error terms, each with one spare pixel at
// either end, and three channels per pixel. Errors are stored scaled by 16.
class DitherErrors {
public:
    explicit DitherErrors(int width)
        : _stride(static_cast<std::size_t>(width + 2) * 3)
        , _terms(_stride * 2, 0)
    {}

    // Start row y: its errors become current and the next row's are cleared.
    void start_row(int y)
    {
        _current = &_terms[static_cast<std::size_t>(y & 1) * _stride];
        _next = &_terms[static_cast<std::size_t>((y + 1) & 1) * _stride];
        std::fill(_next, _next + _stride, 0);
    }

    int carried(int x, int channel) const
    {
        return _current[slot(x + 1, channel)] / 16;
    }

    void spread(int x, int channel, int error)
    {
        _current[slot(x + 2, channel)] += error * 7;
        _next[slot(x, channel)] += error * 3;
        _next[slot(x + 1, channel)] += error * 5;
        _next[slot(x + 2, channel)] += error * 1;
    }

private:
    static std::size_t slot(int column, int channel)
    {
        return static_cast<std::size_t>(column) * 3 + static_cast<std::size_t>(channel);
    }

    std::size_t _stride;
    std::vector<int> _terms;
    int* _current = nullptr;
    int* _next = nullptr;
};

} // namespace

Stopwatch::Stopwatch()
    : _last(std::chrono::steady_clock::now())
{}

std::uint32_t Stopwatch::lap_us()
{
    const auto now = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - _last).count();
    _last = now;
    return static_cast<std::uint32_t>(us);
}

int default_palette_bits(OutputFormat format)
{
    switch (format) {
        case OutputFormat::Ilbm:
            return 4;       // Amiga OCS: 12-bit colour
    }
    return 4;
}

std::uint8_t snap_channel(int value, int bits)
{
    const int levels = (1 << bits) - 1;
    const int clamped = std::clamp(value, 0, 255);
    const int step = (clamped * levels + 127) / 255;
    return static_cast<std::uint8_t>((step * 255 + levels / 2) / levels);
}

bool parse_selector(const std::string& selector, Options& out)
{
    Options o;
    unsigned seen = 0;
    std::size_t pos = 0;
    while (pos < selector.size()) {
        std::size_t end = selector.find(',', pos);
        if (end == std::string::npos) {
            end = selector.size();
        }
        const std::string pair = selector.substr(pos, end - pos);
        pos = end + 1;

        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            return false;
        }
        const int key = find_selector_key(pair.substr(0, eq));
        if (key < 0) {
            return false;
        }
        const unsigned bit = 1u << key;
        if ((seen & bit) != 0) {
            return false;
        }
        seen |= bit;
        if (!apply_selector_value(key, pair.substr(eq + 1), o)) {
            return false;
        }
    }

    if (o.base + o.colors > kMaxPens) {
        return false;
    }
    if ((seen & (1u << KeyBits)) == 0) {
        o.bits = default_palette_bits(o.format);
    }
    out = o;
    return true;
}

Size fit_size(int srcW, int srcH, const Options& o)
{
    // An output row is displayed parY/parX times as tall as it is wide; source
    // pixels are square. Scale factors are 1/1024 fixed point.
    const std::int64_t scaleX = static_cast<std::int64_t>(o.w) * 1024 / srcW;
    const std::int64_t scaleY = static_cast<std::int64_t>(o.h) * o.parY * 1024
                              / (static_cast<std::int64_t>(srcH) * o.parX);
    std::int64_t scale = std::min(scaleX, scaleY);
    if (!o.upscale) {
        scale = std::min<std::int64_t>(scale, 1024);
    }
    const std::int64_t w = std::max<std::int64_t>(1, srcW * scale / 1024);
    const std::int64_t h = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(srcH) * o.parX * scale / (1024 * static_cast<std::int64_t>(o.parY)));
    return {
        static_cast<int>(std::min<std::int64_t>(w, o.w)),
        static_cast<int>(std::min<std::int64_t>(h, o.h)),
    };
}

std::vector<std::uint8_t> scale_rgb(const std::uint8_t* rgb, int srcW, int srcH, Size out)
{
    // Box filter: each output pixel is the mean of the source pixels it covers.
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h) * 3);
    for (int y = 0; y < out.h; ++y) {
        const int y0 = static_cast<int>(static_cast<std::int64_t>(y) * srcH / out.h);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<std::int64_t>(y + 1) * srcH / out.h));
        for (int x = 0; x < out.w; ++x) {
            const int x0 = static_cast<int>(static_cast<std::int64_t>(x) * srcW / out.w);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<std::int64_t>(x + 1) * srcW / out.w));
            std::int64_t sum[3] = {0, 0, 0};
            std::int64_t n = 0;
            for (int sy = y0; sy < y1; ++sy) {
                const std::uint8_t* row = rgb + static_cast<std::size_t>(sy) * static_cast<std::size_t>(srcW) * 3;
                for (int sx = x0; sx < x1; ++sx) {
                    const std::uint8_t* px = row + static_cast<std::size_t>(sx) * 3;
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    ++n;
                }
            }
            std::uint8_t* d = &dst[(static_cast<std::size_t>(y) * static_cast<std::size_t>(out.w)
                                    + static_cast<std::size_t>(x)) * 3];
            d[0] = static_cast<std::uint8_t>(sum[0] / n);
            d[1] = static_cast<std::uint8_t>(sum[1] / n);
            d[2] = static_cast<std::uint8_t>(sum[2] / n);
        }
    }
    return dst;
}

std::vector<Rgb> make_palette(const std::vector<std::uint8_t>& rgb, const Options& o)
{
    const bool gray = o.mode == ColourMode::Gray || (o.mode == ColourMode::Auto && is_gray(rgb));
    if (gray) {
        return make_gray_palette(o);
    }
    return make_colour_palette(rgb, o);
}

std::vector<std::uint8_t> map_pixels(const std::vector<std::uint8_t>& rgb,
                                     Size size,
                                     const std::vector<Rgb>& palette,
                                     const Options& o)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size.w) * static_cast<std::size_t>(size.h));
    DitherErrors errors(size.w);
    for (int y = 0; y < size.h; ++y) {
        errors.start_row(y);
        for (int x = 0; x < size.w; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(size.w)
                                 + static_cast<std::size_t>(x);
            const std::uint8_t* px = &rgb[at * 3];
            int c[3];
            for (int ch = 0; ch < 3; ++ch) {
                const int carried = o.dither ? errors.carried(x, ch) : 0;
                c[ch] = std::clamp(px[ch] + carried, 0, 255);
            }
            const int index = nearest_colour(palette, c[0], c[1], c[2]);
            pixels[at] = static_cast<std::uint8_t>(index);
            if (!o.dither) {
                continue;
            }
            const Rgb& chosen = palette[static_cast<std::size_t>(index)];
            errors.spread(x, 0, c[0] - chosen.r);
            errors.spread(x, 1, c[1] - chosen.g);
            errors.spread(x, 2, c[2] - chosen.b);
        }
    }
    return pixels;
}

StatusCode decode_to_indexed(const std::uint8_t* data,
                             std::size_t len,
                             std::uint32_t maxPixels,
                             const Options& o,
                             IndexedImage& out,
                             PipelineReport& report)
{
    report = PipelineReport{};
    if (data == nullptr || len == 0) {
        return StatusCode::InvalidRequest;
    }
    if (len > static_cast<std::size_t>(INT_MAX)) {
        return StatusCode::Unsupported;
    }
    const int dataLen = static_cast<int>(len);

    int srcW = 0;
    int srcH = 0;
    int comp = 0;
    if (!stbi_info_from_memory(data, dataLen, &srcW, &srcH, &comp)) {
        return StatusCode::InvalidRequest;
    }
    report.source = {srcW, srcH};
    if (static_cast<std::uint64_t>(srcW) * static_cast<std::uint64_t>(srcH) > maxPixels) {
        return StatusCode::Unsupported;
    }

    Stopwatch stage;
    std::uint8_t* decoded = stbi_load_from_memory(data, dataLen, &srcW, &srcH, &comp, 3);
    report.timings.decodeUs = stage.lap_us();
    // The header parsed and the size is within the cap, so a null here is an
    // allocation failure (or a stream corrupt late in the data): report it as
    // too large.
    if (decoded == nullptr) {
        return StatusCode::Unsupported;
    }

    try {
        const Size size = fit_size(srcW, srcH, o);
        std::vector<std::uint8_t> rgb = scale_rgb(decoded, srcW, srcH, size);
        stbi_image_free(decoded);
        decoded = nullptr;
        report.timings.scaleUs = stage.lap_us();

        IndexedImage image;
        image.size = size;
        image.palette = make_palette(rgb, o);
        report.timings.paletteUs = stage.lap_us();
        image.pixels = map_pixels(rgb, size, image.palette, o);
        report.timings.mapUs = stage.lap_us();
        out = std::move(image);
    } catch (const std::bad_alloc&) {
        // C++ exceptions are enabled in the ESP32 build (sdkconfig.defaults).
        if (decoded != nullptr) {
            stbi_image_free(decoded);
        }
        return StatusCode::Unsupported;
    }
    return StatusCode::Ok;
}

} // namespace fujinet::io::image
