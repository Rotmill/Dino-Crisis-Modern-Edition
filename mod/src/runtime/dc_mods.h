/* dc_mods — моды: файл из папки mods подменяет файл игры с тем же именем (04.10.2026, вечер 7).
 * Подробно — в шапке dc_mods.c. Опций в игре нет; выгрузка и просмотр контента — в отдельном приложении для моддинга. */
#ifndef DC_MODS_H
#define DC_MODS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Образ диска открыт (cdrom.c): разобрать ISO9660, найти в mods\<папка>\ файлы с именами файлов игры. */
void dc_mods_init(void *iso_handle);
/* Сектор прочитан из образа: если он принадлежит подменённому файлу — переписать его данные из мода.
 * raw — сырой сектор 2352 байта (заголовок с адресом остаётся родным), cooked — 2048 байт данных. */
void dc_mods_raw(uint32_t lba, uint8_t *raw2352);
void dc_mods_cooked(uint32_t lba, uint8_t *buf2048);
/* 06.10.2026: a room bigger than its place on the disc is read from virtual sectors past the end of the disc: the sector
 * (raw 2352) made from the mod - 1; the same sector of the file's own place (the room id) */
int  dc_mods_virtual_raw(uint32_t lba, uint8_t *raw2352);
uint32_t dc_mods_virtual_home(uint32_t lba);

/* Звуки WAV из mods (05.10.2026): эффекты и инструменты — по содержимому сэмпла на KEYON голоса, речь XA — по сектору. */
int  dc_mods_snd_count(void);
int  dc_mods_snd_match(const uint8_t *spu_ram, uint32_t ram_size, uint32_t addr);   /* −1 — не подменён */
const int16_t *dc_mods_snd(int id, uint32_t *n, double *ratio, int32_t *loop);    /* WAV моно; ratio — шагов WAV на шаг сэмпла */
int  dc_mods_xa(int lba, int channel, int16_t *native_stereo, int frames);          /* 1 — сектор переписан из WAV */
/* мелодии: узнать секвенцию в RAM (ключ по содержимому), WAV стерео 44.1 кГц и начало петли */
int  dc_mods_song_count(void);
int  dc_mods_song_find(const uint8_t *seq, uint32_t maxlen);
const int16_t *dc_mods_song(int id, uint32_t *frames, int32_t *loop_start);

/* Данные программы игры из mods\other\SLUS_009.22 (05.10.2026): поверх тела EXE при загрузке; после быстрой загрузки —
 * те же байты снова (dc_mods_exe_run по порядку, пока не вернёт 0). */
int  dc_mods_exe_patch(uint32_t t_addr, uint8_t *body, uint32_t size);
int  dc_mods_exe_run(int i, uint32_t *addr, const uint8_t **b, uint32_t *n);
/* 05.10: папка mods с '\' в конце (текстуры PNG — растр); NULL — моды выключены */
const char *dc_mods_dir(void);
/* 05.10: сектор данных прочитан (cdrom.c) — видеосектор ролика с MP4 из mods задаёт время кадра (dc_movie.cpp) */
void dc_mods_movie_sector(uint32_t lba, const uint8_t *user2048);
/* 06.10.2026 (player: "the mods must be picked up by themselves - no restart, no cost to the game"): a thread waits for
 * Windows to say the mods folder changed (no work while it is quiet); after the changes settle the drive's idle moment
 * (cdrom.c) takes them: dc_mods_changed() is one read, dc_mods_reload() rereads only what changed. DC_MODS_LIVE=0 - off. */
int  dc_mods_changed(void);
void dc_mods_reload(void);
void dc_mods_note_read(uint32_t lba);   /* сектор прочитан: файл данных держит подхват, пока игра его читает */

/* 06.10.2026 (player: "the added dinosaurs have the sounds of other dinosaurs"): a sound effect of an enemy of another stage
 * plays its own place's sound (the entry and bank of its stage and room; samples from an extended sound page, spu.c).
 * dc_fk_sound - at the entry of func_8005C9A4 (a hook): 1 - played here, the game's call is skipped. dc_fk_sound_tick - once
 * a pad read: banks of another stage that appeared get their sounds read while the room loads. DC_FK_SOUND=0 - off. */
struct CPUState;
int  dc_fk_sound(struct CPUState *cpu);
void dc_fk_sound_tick(void);

#ifdef __cplusplus
}
#endif
#endif /* DC_MODS_H */
