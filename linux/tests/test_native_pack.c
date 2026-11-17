#include "dc_pak.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* test-native-pack                  the pack named by DCME_DATA_PAK: lookup and index
 * test-native-pack game             the installed layout: the pack next to this executable, the working directory
 *                                   data/ (as dc_boot.c leaves it), no DCME_DATA_PAK - game.toml with exe must load
 * test-native-pack none             no pack anywhere: nothing loads (and the reason is printed) */
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "env";
    size_t n = 0;
    if (!strcmp(mode, "game")) {
        const unsigned char *t = dc_pak_find("game.toml", &n);
        if (!t || !n || !strstr((const char *)t, "exe")) {
            fprintf(stderr, "game.toml not found next to the executable (working directory data/)\n");
            return 1;
        }
        puts("native pack next to the executable: ok");
        return 0;
    }
    if (!strcmp(mode, "none")) {
        if (dc_pak_count() != 0 || dc_pak_find("game.toml", &n)) {
            fprintf(stderr, "a pack was loaded where none exists\n");
            return 1;
        }
        puts("native pack missing: reported");
        return 0;
    }
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
