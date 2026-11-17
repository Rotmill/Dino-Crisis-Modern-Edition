#include "dc_png.h"

#ifndef _WIN32
#include <png.h>

#include <cstdio>
#include <limits>

static bool finish_image(png_image *image, std::vector<unsigned char> &pixels,
                         int *width, int *height, bool bgra)
{
    if (!image || !width || !height || image->width == 0 || image->height == 0 ||
        image->width > 8192 || image->height > 8192) {
        if (image) png_image_free(image);
        return false;
    }
    image->format = bgra ? PNG_FORMAT_BGRA : PNG_FORMAT_RGBA;
    const png_alloc_size_t bytes = PNG_IMAGE_SIZE(*image);
    if (bytes > std::numeric_limits<size_t>::max()) {
        png_image_free(image);
        return false;
    }
    pixels.resize((size_t)bytes);
    const bool ok = png_image_finish_read(image, nullptr, pixels.data(), 0, nullptr) != 0;
    if (ok) {
        *width = (int)image->width;
        *height = (int)image->height;
    } else {
        pixels.clear();
    }
    png_image_free(image);
    return ok;
}

bool dc_png_decode_memory(const unsigned char *data, size_t size,
                          std::vector<unsigned char> &pixels,
                          int *width, int *height, bool bgra)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!data || size == 0 || !png_image_begin_read_from_memory(&image, data, size)) return false;
    return finish_image(&image, pixels, width, height, bgra);
}

bool dc_png_decode_file(const char *path, std::vector<unsigned char> &pixels,
                        int *width, int *height, bool bgra)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!path || !png_image_begin_read_from_file(&image, path)) return false;
    return finish_image(&image, pixels, width, height, bgra);
}
#else
bool dc_png_decode_memory(const unsigned char *, size_t, std::vector<unsigned char> &, int *, int *, bool)
{
    return false;
}
bool dc_png_decode_file(const char *, std::vector<unsigned char> &, int *, int *, bool)
{
    return false;
}
#endif
