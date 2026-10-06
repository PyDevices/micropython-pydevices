import sys
S = sys.argv[1]
src = open(S + "/stb_image_write.h").read()
a = src.index("/* ***************************************************************************\n *\n * JPEG writer")
b = src.index("STBIWDEF int stbi_write_jpg_to_func", a)
body = src[a:b]
lic = src[src.index("This software is available under 2 licenses"):]
lic = lic[:lic.index("*/")].rstrip()
def sub(old, new, count=1):
    global body
    assert body.count(old) == count, (old, body.count(old))
    body = body.replace(old, new)
sub("static int stbi_write_jpg_core(stbi__write_context *s, int width, int height, int comp, const void* data, int quality) {",
    "int jpegenc_write(jpegenc_write_fn *func, void *ctx, int width, int height,\n                  jpegenc_fetch_fn *fetch, const void *src, int quality, int subsample) {\n   stbi__write_context sctx = { func, ctx }, *s = &sctx;")
sub("   int row, col, i, k, subsample;", "   int row, col, i, k;")
sub("   if(!data || !width || !height || comp > 4 || comp < 1) {", "   if(!fetch || width < 1 || height < 1 || width > 65535 || height > 65535) {")
sub("   subsample = quality <= 90 ? 1 : 0;\n", "   subsample = subsample ? 1 : 0;\n")
sub("      // comp == 2 is grey+alpha (alpha is ignored)\n      int ofsG = comp > 2 ? 1 : 0, ofsB = comp > 2 ? 2 : 0;\n      const unsigned char *dataR = (const unsigned char *)data;\n      const unsigned char *dataG = dataR + ofsG;\n      const unsigned char *dataB = dataR + ofsB;\n", "")
sub("                  int base_p = (stbi__flip_vertically_on_write ? (height-1-clamped_row) : clamped_row)*width*comp;\n", "", count=2)
sub("                     int p = base_p + ((col < width) ? col : (width-1))*comp;\n                     float r = dataR[p], g = dataG[p], b = dataB[p];",
    "                     float r, g, b;\n                     fetch(src, clamped_row, (col < width) ? col : (width-1), &r, &g, &b);", count=2)
head = """// A baseline JPEG encoder: the JPEG writer from stb_image_write v1.16
// (https://github.com/nothings/stb, commit 2c980bb59875), itself Jon Olick's
// public-domain jo_jpeg.cpp. The encoding is stb's, verbatim; jpegio's
// changes are only how pixels come in and bytes go out:
//
// - pixels are read through a fetch callback (jpegenc.h), so RGB565 and
//   grayscale buffers of any stride encode without an RGB888 copy, where stb
//   reads an 8-bit RGB array;
// - 4:2:0 subsampling is an argument, where stb picks it from quality <= 90;
// - output goes through a write callback and a two-field context, without
//   stb's file and buffering machinery; no vertical flip.
//
// stb's licence, which covers this file:
//
""" + "\n".join("// " + l if l.strip() else "//" for l in lic.splitlines()) + """

#include <math.h>
#include <stddef.h>

#include "jpegenc.h"

#define STBIW_UCHAR(x) (unsigned char) ((x) & 0xff)

typedef struct {
   jpegenc_write_fn *func;
   void *context;
} stbi__write_context;

static void stbiw__putc(stbi__write_context *s, unsigned char c) {
   s->func(s->context, &c, 1);
}

"""
open(sys.argv[2], "w").write(head + body.rstrip() + "\n")
print("ok", len(body.splitlines()), "lines of stb")
