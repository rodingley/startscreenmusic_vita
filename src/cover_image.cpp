/*
 * Covers, made small for the start screen.
 *
 * SceShell's texture pool takes no full-size cover (in testing, a 500 x 500
 * picture would not load; the pool takes 64 KB but not 256 KB), and SceShell
 * has about 1 MB of memory free, too little to decode a cover whole (stb_image
 * needed 4.5 MB). So a JPEG is decoded here a block at
 * a time with picojpeg, each pixel added straight into its cell of a small
 * square, and the square written as a plain PNG, which PAF loads from memory.
 * Pictures 1024 pixels or bigger are read at 1/8 size (picojpeg's reduce
 * mode, one pixel per block), which is still more than the square needs.
 * A PNG is unpacked a row at a time the same way (uzlib's streaming inflate,
 * 32 KB of window). Progressive JPEGs and interlaced PNGs are not read (the
 * jacket shows instead).
 *
 * All memory comes from the caller: nothing here allocates.
 */
#include <stdint.h>
#include <string.h>

#include "cover_image.h"
#include "picojpeg.h"
#include "uzlib.h"

namespace {

// ── A PNG, uncompressed (zlib's stored blocks) ─────────────────────────────
uint32_t g_crcTable[256];

uint32_t crc32(const uint8_t* p, uint32_t n) {
    if (!g_crcTable[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            g_crcTable[i] = v;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

// A chunk at p with n bytes of data already in place at p + 8.
uint32_t chunk(uint8_t* p, const char* type, uint32_t n) {
    put32(p, n);
    memcpy(p + 4, type, 4);
    put32(p + 8 + n, crc32(p + 4, n + 4));
    return 12 + n;
}

// ── JPEG input, from memory ────────────────────────────────────────────────
struct Source {
    const uint8_t* data;
    uint32_t size, pos;
};

unsigned char feed(unsigned char* buf, unsigned char want, unsigned char* got, void* ctx) {
    Source* s = (Source*)ctx;
    uint32_t n = s->size - s->pos;
    if (n > want) n = want;
    memcpy(buf, s->data + s->pos, n);
    s->pos += n;
    *got = (unsigned char)n;
    return 0;
}

// The square's cells: each pixel of the picture's middle square adds to one
// (every step-th pixel of a huge picture, so a cell's sum fits 16 bits:
// at most 16 x 16 pixels of 255).
struct Cells {
    uint16_t* sum;    // side * side * 3
    uint16_t* count;  // side * side
    int side, square, ox, oy, step;

    void init(uint8_t* work, int w, int h, int maxSide) {
        square = w < h ? w : h;
        side = square < maxSide ? square : maxSide;
        ox = (w - square) / 2;
        oy = (h - square) / 2;
        step = (square + side * 16 - 1) / (side * 16);
        sum = (uint16_t*)work;
        count = sum + side * side * 3;
        memset(work, 0, (size_t)side * side * 8);
    }

    void add(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        x -= ox;
        y -= oy;
        if (x < 0 || y < 0 || x >= square || y >= square || x % step || y % step) return;
        const int i = (y * side / square) * side + x * side / square;
        sum[i * 3] += r;
        sum[i * 3 + 1] += g;
        sum[i * 3 + 2] += b;
        count[i]++;
    }
};

}  // namespace

uint32_t pngSize(int w, int h, int comp) {
    const uint32_t raw = (uint32_t)h * (1 + (uint32_t)w * comp);
    return 8 + 25 + 12 + 2 + (raw / 65535 + 1) * 5 + raw + 4 + 12;
}

uint32_t writePng(const uint8_t* px, int w, int h, int comp, uint8_t* out, uint32_t cap) {
    if (pngSize(w, h, comp) > cap) return 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    memcpy(out, sig, 8);
    uint8_t* p = out + 8;
    put32(p + 8, (uint32_t)w);
    put32(p + 12, (uint32_t)h);
    p[16] = 8;                  // 8 bits
    p[17] = comp == 4 ? 6 : 2;  // RGBA or RGB
    p[18] = p[19] = p[20] = 0;  // deflate, no filter method, no interlace
    p += chunk(p, "IHDR", 13);
    // IDAT: zlib header, stored blocks of the rows (each with filter 0), Adler-32.
    uint8_t* d = p + 8;
    uint8_t* z = d;
    *z++ = 0x78;
    *z++ = 0x01;
    const uint32_t row = 1 + (uint32_t)w * comp, raw = (uint32_t)h * row;
    uint32_t a = 1, b = 0, done = 0, inBlock = 0;
    for (int y = 0; y < h; y++)
        for (uint32_t i = 0; i < row; i++) {
            if (!inBlock) {
                const uint32_t n = raw - done < 65535 ? raw - done : 65535;
                z[0] = done + n == raw ? 1 : 0;
                z[1] = (uint8_t)n;
                z[2] = (uint8_t)(n >> 8);
                z[3] = (uint8_t)~n;
                z[4] = (uint8_t)(~n >> 8);
                z += 5;
                inBlock = n;
            }
            const uint8_t v = i ? px[(size_t)y * w * comp + i - 1] : 0;
            *z++ = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
            done++;
            inBlock--;
        }
    put32(z, b << 16 | a);
    z += 4;
    p += chunk(p, "IDAT", (uint32_t)(z - d));
    p += chunk(p, "IEND", 0);
    return (uint32_t)(p - out);
}

namespace {

// Each cell's mean, written over the sums (the bytes stay behind the reads),
// as a PNG.
uint32_t finish(Cells& cells, uint8_t* work, uint8_t* out, uint32_t cap, CoverInfo* info) {
    uint8_t* px = work;
    const int n = cells.side * cells.side;
    for (int i = 0; i < n; i++) {
        const uint32_t c = cells.count[i] ? cells.count[i] : 1;
        for (int k = 0; k < 3; k++) px[i * 3 + k] = (uint8_t)((cells.sum[i * 3 + k] + c / 2) / c);
    }
    info->side = cells.side;
    return writePng(px, cells.side, cells.side, 3, out, cap);
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool isPng(const uint8_t* d, uint32_t n) { return n >= 33 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G'; }

// A PNG's header: false for one this doesn't read (interlaced, odd depths).
struct PngHead {
    uint32_t w, h, rowBytes, pixelBytes;
    int depth, type, channels;
};
bool pngHead(const uint8_t* d, uint32_t n, PngHead& p) {
    if (!isPng(d, n) || memcmp(d + 12, "IHDR", 4)) return false;
    p.w = be32(d + 16);
    p.h = be32(d + 20);
    p.depth = d[24];
    p.type = d[25];
    p.channels = p.type == 0 || p.type == 3 ? 1 : p.type == 2 ? 3 : p.type == 4 ? 2 : p.type == 6 ? 4 : 0;
    // The PNG standard's depths only: 1, 2, 4, 8 for palettes; 8, 16 for
    // colour; any of those for grey.
    const bool small = p.depth == 1 || p.depth == 2 || p.depth == 4, wide = p.depth == 8 || p.depth == 16;
    const bool depthOk = p.type == 3 ? small || p.depth == 8 : p.type == 0 ? small || wide : wide;
    if (!p.channels || !depthOk || d[28] /* interlaced */ || !p.w || !p.h || p.w > 16384 || p.h > 16384) return false;
    const uint32_t bits = (uint32_t)p.channels * p.depth;
    p.rowBytes = (p.w * bits + 7) / 8;
    p.pixelBytes = bits >= 8 ? bits / 8 : 1;
    return p.rowBytes <= 0x10000;
}

// IDAT chunks, one after another, for uzlib.
struct PngIn {
    const uint8_t* data;
    uint32_t size, next;
};
PngIn g_pngIn;

int pngSource(struct uzlib_uncomp* d) {
    while (g_pngIn.next + 12 <= g_pngIn.size) {
        const uint8_t* c = g_pngIn.data + g_pngIn.next;
        const uint32_t len = be32(c);
        if (len > g_pngIn.size - g_pngIn.next - 12) return -1;
        g_pngIn.next += 12 + len;
        if (!memcmp(c + 4, "IEND", 4)) return -1;
        if (!memcmp(c + 4, "IDAT", 4) && len) {
            d->source = c + 9;
            d->source_limit = c + 8 + len;
            return c[8];
        }
    }
    return -1;
}

uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

const uint32_t kDict = 32768;

uint32_t pngToPng(const uint8_t* data, uint32_t size, uint8_t* work, uint32_t workSize, int maxSide, uint8_t* out,
                  uint32_t cap, CoverInfo* info) {
    PngHead ph;
    if (!pngHead(data, size, ph)) {
        info->error = 100;  // not a PNG this reads
        return 0;
    }
    info->width = (int)ph.w;
    info->height = (int)ph.h;
    const uint32_t cells = (uint32_t)maxSide * maxSide * 8, row = (ph.rowBytes + 1 + 15) & ~15u;
    if (workSize < cells + kDict + row * 2) return 0;
    // The palette, if any (before the first IDAT).
    uint8_t pal[256 * 3];
    memset(pal, 0, sizeof(pal));
    for (uint32_t at = 8; at + 12 <= size;) {
        const uint32_t len = be32(data + at);
        if (len > size - at - 12 || !memcmp(data + at + 4, "IDAT", 4)) break;
        if (!memcmp(data + at + 4, "PLTE", 4)) memcpy(pal, data + at + 8, len < sizeof(pal) ? len : sizeof(pal));
        at += 12 + len;
    }
    Cells c;
    c.init(work, (int)ph.w, (int)ph.h, maxSide);
    uint8_t *dict = work + cells, *cur = dict + kDict, *prev = cur + row;
    memset(prev, 0, row);
    struct uzlib_uncomp z;
    memset(&z, 0, sizeof(z));
    uzlib_uncompress_init(&z, dict, kDict);
    z.source_read_cb = pngSource;
    g_pngIn.data = data;
    g_pngIn.size = size;
    g_pngIn.next = 8;
    uzlib_get_byte(&z);  // the zlib header (no preset dictionary in a PNG)
    if (uzlib_get_byte(&z) & 0x20) return 0;
    const uint32_t bpp = ph.pixelBytes, mask = (1u << (ph.depth < 8 ? ph.depth : 8)) - 1;
    for (uint32_t y = 0; y < ph.h; y++) {
        // The row: its filter byte, then its bytes.
        z.dest_start = z.dest = cur;
        z.dest_limit = cur + 1 + ph.rowBytes;
        while (z.dest < z.dest_limit) {
            const int res = uzlib_uncompress(&z);
            if (res < 0 || (res == TINF_DONE && z.dest < z.dest_limit)) {
                info->error = 101;  // the picture's data ends early or is damaged
                return 0;
            }
        }
        uint8_t* r = cur + 1;
        const uint8_t* p = prev + 1;
        const int f = cur[0];
        for (uint32_t i = 0; i < ph.rowBytes; i++) {
            const int a = i >= bpp ? r[i - bpp] : 0, b = p[i], cc = i >= bpp ? p[i - bpp] : 0;
            r[i] = (uint8_t)(r[i] + (f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, cc) : 0));
        }
        if (y % c.step == 0 && (int)y >= c.oy && (int)y < c.oy + c.square) {
            for (uint32_t x = 0; x < ph.w; x++) {
                uint8_t cr, cg, cb;
                if (ph.depth < 8) {  // grey or palette, packed
                    const uint32_t bit = x * ph.depth;
                    const uint32_t v = (r[bit / 8] >> (8 - ph.depth - bit % 8)) & mask;
                    if (ph.type == 3) {
                        cr = pal[v * 3], cg = pal[v * 3 + 1], cb = pal[v * 3 + 2];
                    } else {
                        cr = cg = cb = (uint8_t)(v * 255 / mask);
                    }
                } else {
                    const uint8_t* q = r + x * bpp;  // 16-bit samples: their high byte
                    const int s = ph.depth == 16 ? 2 : 1;
                    if (ph.type == 3) cr = pal[q[0] * 3], cg = pal[q[0] * 3 + 1], cb = pal[q[0] * 3 + 2];
                    else if (ph.channels >= 3) cr = q[0], cg = q[s], cb = q[2 * s];
                    else cr = cg = cb = q[0];
                }
                c.add((int)x, (int)y, cr, cg, cb);
            }
        }
        uint8_t* t = cur;
        cur = prev;
        prev = t;
    }
    return finish(c, work, out, cap, info);
}

}  // namespace

uint32_t coverWorkSize(int side, const uint8_t* data, uint32_t size) {
    const uint32_t cells = (uint32_t)side * side * 8;
    PngHead ph;
    if (pngHead(data, size, ph)) return cells + kDict + ((ph.rowBytes + 1 + 15) & ~15u) * 2;
    return cells;
}

uint32_t coverToPng(const uint8_t* data, uint32_t size, uint8_t* work, uint32_t workSize, int maxSide, uint8_t* out,
                    uint32_t cap, CoverInfo* info) {
    memset(info, 0, sizeof(*info));
    if (isPng(data, size)) return pngToPng(data, size, work, workSize, maxSide, out, cap, info);
    if (workSize < (uint32_t)maxSide * maxSide * 8) return 0;
    Source src = {data, size, 0};
    pjpeg_image_info_t jpg;
    int rc = pjpeg_decode_init(&jpg, feed, &src, 0);
    info->error = rc;
    if (rc) return 0;
    info->width = jpg.m_width;
    info->height = jpg.m_height;
    // Big pictures at 1/8 size: one pixel for each 8 x 8 block.
    const bool reduce = (jpg.m_width < jpg.m_height ? jpg.m_width : jpg.m_height) >= 1024;
    if (reduce) {
        src.pos = 0;
        rc = pjpeg_decode_init(&jpg, feed, &src, 1);
        info->error = rc;
        if (rc) return 0;
    }
    info->reduced = reduce;
    const int scale = reduce ? 8 : 1, w = (jpg.m_width + scale - 1) / scale, h = (jpg.m_height + scale - 1) / scale;
    Cells cells;
    cells.init(work, w, h, maxSide);
    const bool grey = jpg.m_scanType == PJPG_GRAYSCALE;
    for (int my = 0; my < jpg.m_MCUSPerCol; my++)
        for (int mx = 0; mx < jpg.m_MCUSPerRow; mx++) {
            rc = pjpeg_decode_mcu();
            if (rc) {
                info->error = rc;
                return 0;
            }
            const uint8_t *r = jpg.m_pMCUBufR, *g = grey ? r : jpg.m_pMCUBufG, *b = grey ? r : jpg.m_pMCUBufB;
            for (int y = 0; y < jpg.m_MCUHeight; y += 8)
                for (int x = 0; x < jpg.m_MCUWidth; x += 8) {
                    const int at = x * 8 + y * 16;  // this 8 x 8 block in the MCU's buffers
                    const int px = mx * jpg.m_MCUWidth + x, py = my * jpg.m_MCUHeight + y;
                    if (reduce) {
                        cells.add(px / 8, py / 8, r[at], g[at], b[at]);
                        continue;
                    }
                    for (int by = 0; by < 8 && py + by < jpg.m_height; by++)
                        for (int bx = 0; bx < 8 && px + bx < jpg.m_width; bx++) {
                            const int i = at + by * 8 + bx;
                            cells.add(px + bx, py + by, r[i], g[i], b[i]);
                        }
                }
        }
    return finish(cells, work, out, cap, info);
}
