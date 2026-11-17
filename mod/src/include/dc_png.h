#ifndef DC_PNG_H
#define DC_PNG_H

#include <stddef.h>

#ifdef __cplusplus
#include <vector>

/* Decode a PNG to four bytes per pixel. bgra=true matches SDL ARGB8888 on
 * little-endian hosts; false returns conventional RGBA for Vulkan uploads. */
bool dc_png_decode_memory(const unsigned char *data, size_t size,
                          std::vector<unsigned char> &pixels,
                          int *width, int *height, bool bgra);
bool dc_png_decode_file(const char *path,
                        std::vector<unsigned char> &pixels,
                        int *width, int *height, bool bgra);
#endif

#endif
