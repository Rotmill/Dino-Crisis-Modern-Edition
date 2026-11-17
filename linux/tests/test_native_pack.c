#include "dc_pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    size_t n = 0;
    const unsigned char *p = dc_pak_find("Test\\Hello.TXT", &n);
    if (!p || n != 5 || memcmp(p, "hello", 5) != 0) {
        fprintf(stderr, "native pack lookup failed\n");
        return 1;
    }
    if (dc_pak_count() != 1 || strcmp(dc_pak_name(0), "test/hello.txt") != 0) {
        fprintf(stderr, "native pack index failed\n");
        return 1;
    }
    puts("native pack test: ok");
    return 0;
}
