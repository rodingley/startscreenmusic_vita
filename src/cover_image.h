// Covers, made small for the start screen (cover_image.cpp).
#pragma once
#include <stdint.h>

// Bytes a PNG of w x h pixels (comp 3 or 4) takes when written by writePng.
uint32_t pngSize(int w, int h, int comp);
// px as an uncompressed PNG at out; its size, 0 if it doesn't fit in cap.
uint32_t writePng(const uint8_t* px, int w, int h, int comp, uint8_t* out, uint32_t cap);

struct CoverInfo {
    int width, height;  // the picture's
    int side;           // the square made
    bool reduced;       // read at 1/8 size
    int error;          // picojpeg's (37: progressive JPEG); 100: a PNG not read
                        // (interlaced), 101: damaged; 0 for none
};
// Working memory coverToPng needs for a square of side, for this picture.
uint32_t coverWorkSize(int side, const uint8_t* data, uint32_t size);
// A baseline JPEG's or a PNG's middle square, scaled to at most maxSide
// square (smaller pictures keep their size) and written as a PNG at out: the
// PNG's size, 0 when it can't be read.
uint32_t coverToPng(const uint8_t* data, uint32_t size, uint8_t* work, uint32_t workSize, int maxSide, uint8_t* out,
                    uint32_t cap, CoverInfo* info);
