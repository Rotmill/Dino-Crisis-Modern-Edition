/* dc_pak.h - Dino Crisis ME: the game's data built into the program.
 *
 * 17.09.2026, player: "keep dc_user.cfg, input.ini and SLUS_009.22 in data,
 * build everything else into the exe - this is a release, no debug and no
 * debug files". What used to lie in data\ as loose files - the startup
 * settings (dc_env.cfg), the tuning of items, decals and z-fighting, the hips
 * camera rooms, game.toml with the path to the disc image, the licence texts,
 * the camera collision and the rooms exported from the player's disc image -
 * is packed by setup into dcme_data.pak (mod/tools/pack_data.py) and linked
 * into DinoCrisisME.exe as the resource DCME_DATA.
 *
 * The loaders keep their old file names: a path is looked up as it was
 * spelled relative to the data folder ("dc_env.cfg",
 * "exported_rooms\\library/room_000.dcroom"), with case and slash direction
 * ignored. */

#ifndef DC_PAK_H
#define DC_PAK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes of a packed file, or NULL when it is not in the program. Valid for
 * the life of the process. Every entry is followed by a zero byte that *size
 * does not count, so a text file can be read as a C string. */
const unsigned char *dc_pak_find(const char *path, size_t *size);

/* Entries in the order they were packed - sorted by name, the order
 * FindFirstFile gave for a folder of these files. Names are normalised. */
int         dc_pak_count(void);
const char *dc_pak_name(int i);

/* A path the way entries are named: lower case, '/' as the separator, no
 * leading "./". 0 when it does not fit into cap. */
int dc_pak_norm(const char *path, char *out, size_t cap);

/* A read-only stream over a packed file, standing in for fopen / fgets /
 * fread / fclose in the loaders that used to read files. dc_mf_gets drops the
 * '\r' of a CRLF pair, as fgets did on a file opened in text mode; dc_mf_read
 * is binary, as fread on a file opened with "rb".
 * 04.10.2026 (V3): a mod file of the same name wins over the packed one -
 * <game folder>\mods\rooms\ for exported_rooms/library/, mods\collision\ for
 * exported_rooms/collision/ (dc_pak.c). */
typedef struct DcMemFile DcMemFile;
DcMemFile *dc_mf_open(const char *path);
char      *dc_mf_gets(char *buf, int n, DcMemFile *f);
size_t     dc_mf_read(void *dst, size_t size, size_t count, DcMemFile *f);
void       dc_mf_close(DcMemFile *f);

/* V3 (04.10.2026): our own data (shaders, aiming poses, screen pictures) under embed/<name>. */
const unsigned char *dc_embedded(const char *name, unsigned *size);

#ifdef __cplusplus
}
#endif

/* V3: stand-ins for the file calls of the loaders that read our data in a release (release_edits.py,
 * v3_data_inside): GetFileAttributesExA, _stat64, _findfirst/_findnext/_findclose over the packed names. */
#ifdef _WIN32
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <io.h>
#ifdef __cplusplus
extern "C" {
#endif
int      dcme_file_attr(const char *path, void *wfad);
int      dcme_stat64(const char *path, struct _stat64 *st);
intptr_t dcme_findfirst(const char *pattern, struct _finddata_t *fd);
int      dcme_findnext(intptr_t h, struct _finddata_t *fd);
int      dcme_findclose(intptr_t h);
#ifdef __cplusplus
}
#endif
#endif

#endif
