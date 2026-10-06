// jpegio's baseline JPEG encoder (jpegenc.c, from stb_image_write).
#ifndef JPEGIO_JPEGENC_H
#define JPEGIO_JPEGENC_H

// Receives the encoded bytes, in order, a few at a time.
typedef void jpegenc_write_fn(void *ctx, void *data, int size);
// Writes the pixel at (row, col) of src as 0..255 floats.
typedef void jpegenc_fetch_fn(const void *src, int row, int col, float *r, float *g, float *b);

// Encodes a width x height image (1..65535 each) at quality 1..100, 4:2:0
// when subsample is nonzero, else 4:4:4. Returns 1, or 0 for bad arguments.
int jpegenc_write(jpegenc_write_fn *func, void *ctx, int width, int height,
                  jpegenc_fetch_fn *fetch, const void *src, int quality, int subsample);

#endif
