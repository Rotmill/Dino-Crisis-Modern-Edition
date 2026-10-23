/* ============================================================================
 * savestate.c — emulator-style quicksave/quickload (see savestate.h).
 * ----------------------------------------------------------------------------
 * Why the capture point is a dispatch boundary: the recompiled code never
 * materializes the guest PC mid-function (host call stack mirrors the MIPS
 * call stack), but at psx_dispatch the target IS the guest PC of a function
 * entry about to run. Saving there means restore can re-enter that function
 * natively; everything above it on the MIPS stack resumes by following $ra
 * through the interpreter, which calls back into compiled code at every
 * JAL/JALR — so steady-state speed returns within a frame.
 *
 * File layout (little-endian, fixed order, version-gated):
 *   SsHeader, cpu block, then each module's own field list.
 * ========================================================================== */
#include "savestate.h"
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#endif

extern int  psx_get_in_exception(void);
extern void sw_batch_flush(void);
extern uint64_t psx_cycle_count;
extern int g_psx_dispatch_depth;
extern void dc_osd(const char* fmt, ...);   /* main.cpp: on-screen debug toast */

volatile int g_savestate_pending = 0;
jmp_buf g_savestate_jmpbuf;
int     g_savestate_anchor_armed = 0;
int  g_ss_delivlog = 0;   /* interrupts.c: log next N exception deliveries */
char g_ss_delivtag = '?';
int  g_ss_walkprobe = 0;  /* dirty_ram_interp.c: probe kernel IRQ-chain walk */
/* 17.09.2026 вечер (поиск мусора, тормозящего игру): пробы клина сейва писали в журнал 4000 строк istatrd/walk и
 * 400 строк exc на КАЖДУЮ загрузку и сохранение — около 0.1 с записи подряд сразу после Del. Клин давно починен,
 * пробы выключены. DC_SS_WALKPROBE=4000 / DC_SS_DELIVLOG=400 — как было. */
static int ss_probe_env(const char *name) { const char *e = getenv(name); return e ? atoi(e) : 0; }

#define SS_MAGIC   0x53534344u   /* 'DCSS' */
#define SS_VERSION 1u

/* Post-save / post-load dispatch-stream tracing (DC_SS_TRACE=n). */
#define SS_REQ_TRACE 4
static int  s_dtrace_n = 0;
static char s_dtrace_tag = '?';

typedef struct SsHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t resume_pc;      /* dispatch target captured at save time */
    uint32_t reserved;
} SsHeader;

void ss_bytes(PsxSaveCtx* c, void* p, size_t n) {
    if (c->error) return;
    if (c->loading) {
        if (fread(p, 1, n, c->f) != n) c->error = 1;
    } else {
        if (fwrite(p, 1, n, c->f) != n) c->error = 1;
    }
}

/* 18.10.2026 (разбор кода): безопасная запись файлов игрока - квиксейвов, карты памяти (memcard.c), dc_user.cfg
 * (dc_settings.c). Прежде квиксейв удалял старый файл и только потом переименовывал новый (выход или сбой между ними -
 * слот пуст), не проверял закрытие файла (полный диск всплывает именно там - писалось «Saved», а слот битый), а карта и
 * настройки открывались "w" прямо поверх старого файла и стирали его ещё до записи. Теперь: новый файл рядом, данные
 * доводятся до диска, подмена одной операцией - до неё на месте старый целый файл. */
int dc_file_finish(FILE* f) {
    int ok = !ferror(f) && fflush(f) == 0;
#ifdef _WIN32
    if (ok && _commit(_fileno(f)) != 0) ok = 0;
#endif
    if (fclose(f) != 0) ok = 0;
    return ok;
}
int dc_replace_file(const char* tmp, const char* dst) {
#ifdef _WIN32
    /* только что закрытый файл антивирус или индексатор держат доли секунды - несколько попыток */
    for (int i = 0; i < 25; i++) {
        if (MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
        Sleep(20);
    }
    return 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

/* QUICKSAVE SLOTS (2026-08-23 night, player: "скажи как переключать все
 * квиксейвы"). Slot 0 keeps the original name so every existing file and the
 * DC_AUTOLOAD harness still work; slots 1..49 are quicksave1.dcss ... The
 * current slot is switched in game with PageUp / PageDown (see main.cpp) and
 * can be preset with DC_SS_SLOT. DC_SS_FILE still overrides everything. */
static int s_ss_slot = -1;
/* 22.09.2026 (игрок): «увеличь количество ячеек для квик сейва до 20» — слоты 0..19 (quicksave10..19.dcss — новые
 * файлы, старые имена те же). DC_SS_SLOTS=10 — как было.
 * 01.10.2026 вечер (игрок): «расширь количество квик сейвов до 50 (в сборке мода будет лимит в 10, пока что для себя
 * столько)» — дев: слоты 0..49 (quicksave20..49.dcss — новые файлы); DC_SS_SLOTS=20 — как было до сегодня. Релиз мода
 * (DCME_RELEASE) — ровно 10 слотов (0..9), переменная там не читается.
 * 13.10.2026 вечер (игрок): «количество квиксейвов сделай 50 штук, в релизной версии тоже» — релиз: 50 (0..49). */
static int ss_slots(void) {
#ifdef DCME_RELEASE
    return 50;
#else
    static int n = 0;
    if (n <= 0) { const char* e = getenv("DC_SS_SLOTS"); n = e ? atoi(e) : 50; if (n < 1) n = 50; if (n > 100) n = 100; }
    return n;
#endif
}
int savestate_slot(void) {
    if (s_ss_slot < 0) { const char* e = getenv("DC_SS_SLOT"); s_ss_slot = e ? atoi(e) : 0;
                         if (s_ss_slot < 0 || s_ss_slot >= ss_slots()) s_ss_slot = 0; }
    return s_ss_slot;
}
void savestate_slot_set(int n) {
    if (n < 0) n = ss_slots() - 1; if (n >= ss_slots()) n = 0;
    (void)savestate_slot();
    s_ss_slot = n;
}
#ifdef DCME_RELEASE
/* 13.10.2026 вечер (игрок: «сами квиксейвы куда сохраняешь? нужна отдельная папка или всё в один файл»): 50 слотов по ~5 МБ
 * лежали прямо в data\ рядом с настройками. Релиз хранит их в data\quicksaves\ (рабочая папка игры - data, dc_boot.c) - по
 * файлу на слот, как раньше: запись идёт во временный файл и подменяет старый, испорченная запись не задевает другие слоты
 * (в одном общем файле задела бы все). Прежние data\quicksave*.dcss и их .story переезжают туда при первом обращении; уже
 * лежащий в папке файл не перезаписывается. */
#include <direct.h>
#include <io.h>
static void ss_dir_ready(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    _mkdir("quicksaves");
    for (int s = 0; s < 100; s++) {
        char a[48], b[80];
        for (int st = 0; st < 2; st++) {
            if (s == 0) snprintf(a, sizeof(a), st ? "quicksave.dcss.story" : "quicksave.dcss");
            else        snprintf(a, sizeof(a), st ? "quicksave%d.dcss.story" : "quicksave%d.dcss", s);
            snprintf(b, sizeof(b), "quicksaves\\%s", a);
            if (_access(a, 0) == 0 && _access(b, 0) != 0) rename(a, b);
        }
    }
}
#endif
const char* savestate_path(void) {
    static char path[512];
    const char* e = getenv("DC_SS_FILE");
    if (e && e[0]) { snprintf(path, sizeof(path), "%s", e); return path; }
#ifdef DCME_RELEASE
    ss_dir_ready();
    if (savestate_slot() == 0) snprintf(path, sizeof(path), "quicksaves\\quicksave.dcss");
    else                       snprintf(path, sizeof(path), "quicksaves\\quicksave%d.dcss", savestate_slot());
#else
    if (savestate_slot() == 0) snprintf(path, sizeof(path), "quicksave.dcss");
    else                       snprintf(path, sizeof(path), "quicksave%d.dcss", savestate_slot());
#endif
    return path;
}

void savestate_request(int op) {
    g_savestate_pending |= op;
}

/* ---- CPU block (explicit fields: CPUState carries host fn pointers) ------ */
static void cpu_savestate(PsxSaveCtx* c, CPUState* cpu) {
    ss_bytes(c, cpu->gpr,      sizeof(cpu->gpr));
    SS_VAR(c, cpu->pc);
    SS_VAR(c, cpu->hi);
    SS_VAR(c, cpu->lo);
    ss_bytes(c, cpu->cop0,     sizeof(cpu->cop0));
    ss_bytes(c, cpu->gte_data, sizeof(cpu->gte_data));
    ss_bytes(c, cpu->gte_ctrl, sizeof(cpu->gte_ctrl));
}

extern void dc_jaw_savestate(int after);   /* gpu.c (dc_faces2.inc): 27.09 — в сейв исходные вершины челюсти */
static void all_modules_savestate(PsxSaveCtx* c, CPUState* cpu) {
    cpu_savestate(c, cpu);
    SS_VAR(c, psx_cycle_count);
    if (!c->loading) dc_jaw_savestate(0);
    memory_savestate(c);
    if (!c->loading) dc_jaw_savestate(1);
    gpu_savestate(c);
    spu_savestate(c);
    cdrom_savestate(c);
    sio_savestate(c);
    mdec_savestate(c);
    dma_savestate(c);
    timers_savestate(c);
    interrupts_savestate(c);
    memcard_savestate(c);
}

/* ---- save ---------------------------------------------------------------- */
static void savestate_do_save(CPUState* cpu, uint32_t target) {
    /* Deferred prims must land in VRAM before the VRAM snapshot. */
    sw_batch_flush();

    const char* path = savestate_path();
    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "savestate: cannot open %s for write\n", tmp);
        return;
    }
    /* 14.09 вечер: номер комнаты — в заголовке самого сейва (room + 1, 0 = неизвестно) */
    uint32_t room_tag = 0;
    { extern int dc_room_current(void); int r = dc_room_current(); if (r >= 0) room_tag = (uint32_t)r + 1u; }
    SsHeader h = { SS_MAGIC, SS_VERSION, target, room_tag };
    PsxSaveCtx c = { f, 0, 0 };
    ss_bytes(&c, &h, sizeof(h));
    all_modules_savestate(&c, cpu);
    { extern void memory_modram_save(FILE *f); if (!c.error) memory_modram_save(f); }   /* 06.10.2026: память модов — хвостом */
    { extern void dc_steer_diag_save(FILE *f); if (!c.error) dc_steer_diag_save(f); }   /* 06.10.2026: наше руление — последним ('DCNS') */
    if (!dc_file_finish(f)) c.error = 1;   /* 18.10: ошибки записи хвоста и закрытия (полный диск) - тоже ошибка */
    if (c.error) {
        fprintf(stderr, "savestate: WRITE ERROR, state not saved\n");
        dc_osd("Quicksave %d: save failed", savestate_slot());   /* 13.10 ночь: коротким статусом */
        remove(tmp);
        return;
    }
    if (!dc_replace_file(tmp, path)) {   /* 18.10: старый слот цел, пока новый не встал на его место */
        fprintf(stderr, "savestate: could not replace %s, the old state is kept\n", path);
        dc_osd("Quicksave %d: save failed", savestate_slot());
        remove(tmp);
        return;
    }
    fprintf(stderr, "savestate: SAVED -> %s (resume pc=0x%08X)\n", path, target);
    { extern void dc_room_story_save(const char *savepath); dc_room_story_save(path); }   /* 20.09: сюжетные подмены комнаты - рядом с сейвом */
    /* 13.10.2026 вечер (игрок): номер слота и что случилось, без пути к файлу; ночь (игрок: «статус типа saved to
     * такой-то квиксейв, такой-то квиксейв loaded, или empty») - одной короткой строкой, и в деве */
    dc_osd("Saved to quicksave %d", savestate_slot());
}

/* ---- load ---------------------------------------------------------------- */
static int savestate_validate_file(void) {
    FILE* f = fopen(savestate_path(), "rb");
    if (!f) {
        fprintf(stderr, "savestate: no file %s\n", savestate_path());
        dc_osd("Quicksave %d is empty", savestate_slot());
        return 0;
    }
    SsHeader h;
    int ok = fread(&h, sizeof(h), 1, f) == 1 &&
             h.magic == SS_MAGIC && h.version == SS_VERSION &&
             h.resume_pc != 0;
    fclose(f);
    if (!ok) {
        fprintf(stderr, "savestate: %s is not a valid v%u state\n",
                        savestate_path(), (unsigned)SS_VERSION);
        dc_osd("Quicksave %d can't be loaded", savestate_slot());
    }
    return ok;
}

/* Сколько раз грузили савстейт (2026-09-02): загрузка кладёт в машину
 * состояние ИГРЫ, минуя точку входа, — признак «игра запущена» в gpu.c
 * (dc_game_loaded) иначе не встаёт, и атлас шрифта / MODERN OPTIONS / широкий
 * кадр не включаются после Del во время BIOS. */
int g_savestate_loads = 0;

uint32_t savestate_do_load(CPUState* cpu) {
    FILE* f = fopen(savestate_path(), "rb");
    if (!f) return 0;
    SsHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1 ||
        h.magic != SS_MAGIC || h.version != SS_VERSION) {
        fclose(f);
        return 0;
    }
    g_savestate_loads++;
    PsxSaveCtx c = { f, 1, 0 };
    all_modules_savestate(&c, cpu);
    { extern void memory_modram_load(FILE *f); if (!c.error) memory_modram_load(f); }   /* 06.10.2026: память модов, если есть */
    fclose(f);
    if (c.error) {
        fprintf(stderr, "savestate: READ ERROR mid-load - state is torn, aborting\n");
        dc_osd("Quicksave %d: load failed", savestate_slot());
        return 0;
    }

    /* 14.09 вечер: комната из заголовка этого сейва (старые сейвы: 0 → неизвестно) */
    { extern void dc_room_set_id(int id); dc_room_set_id(h.reserved ? (int)h.reserved - 1 : -1); }
    { extern void dc_room_story_load(const char *savepath); dc_room_story_load(savestate_path()); }   /* 20.09: сюжетные подмены комнаты */
    {   /* 05.10: данные программы из mods (сейв мог быть сделан без мода) — снова поверх */
        extern int dc_mods_exe_run(int i, uint32_t *addr, const uint8_t **b, uint32_t *n);
        uint32_t a, n; const uint8_t *b;
        for (int i = 0; dc_mods_exe_run(i, &a, &b, &n); i++)
            for (uint32_t k = 0; k < n; k++) cpu->write_byte(a + k, b[k]);
    }
    /* Host-side teardown/fixups the module loaders can't do themselves. */
    psx_threads_reset();        /* stale BIOS-thread fibers are gone */
    pgxp_addr_reset();          /* float shadows from the pre-load timeline */
    {   /* value cache too (DS clears equivalent state on state load) */
        extern void pgxp_clear_c(void);
        pgxp_clear_c();
    }
    g_interp_anywhere = 1;      /* the restored call chain resumes via interp */
    {   extern uint64_t s_frame_count;          /* debug_server.c vblank count */
        extern uint64_t g_interp_anywhere_until;
        g_interp_anywhere_until = s_frame_count + 120;   /* ~2 s resume window */
    }
    g_psx_dispatch_depth = 0;
    {   /* DC_SS_TRACE=n: log the first n resume-climb transfers and the
         * post-load dispatch stream (compare against the post-save one). */
        extern int g_ss_trace;
        const char* e = getenv("DC_SS_TRACE");
        g_ss_trace = e ? atoi(e) : 0;
        if (g_ss_trace > 0) {
            s_dtrace_n = g_ss_trace; s_dtrace_tag = 'L';
            g_savestate_pending |= SS_REQ_TRACE;
        }
    }

    {   /* Bisect experiment: DC_SS_RESET_SIO=1 forces a clean idle SIO bus
         * after load (cards/DualShock config kept). */
        const char* e = getenv("DC_SS_RESET_SIO");
        if (e && *e == '1') {
            extern void sio_savestate_bus_reset(void);
            sio_savestate_bus_reset();
            fprintf(stderr, "savestate: SIO bus reset to idle (DC_SS_RESET_SIO)\n");
        }
    }
    {   /* DC_SS_PAD_MUTE=n: present the pad as unplugged for n frames after
         * load. Default OFF — the mute itself wedged DC: 60 frames of libpad
         * -2 timeouts made the game kill its scene tasks (a state it never
         * leaves), while the SIO ACK-storm the mute once guarded against no
         * longer reproduces with the SELECT-quiescence capture gate and the
         * thread-fiber resume climb (traps.c) in place. */
        const char* e = getenv("DC_SS_PAD_MUTE");
        int frames = e ? atoi(e) : 0;
        if (frames > 0) {
            extern void sio_savestate_pad_mute(int frames);
            sio_savestate_pad_mute(frames);
        }
    }
    g_ss_delivlog = ss_probe_env("DC_SS_DELIVLOG"); g_ss_delivtag = 'L';
    g_ss_walkprobe = ss_probe_env("DC_SS_WALKPROBE");
    fprintf(stderr, "savestate: LOADED <- %s, resuming at 0x%08X\n",
            savestate_path(), h.resume_pc);
    {   extern int memcard_ss_rewind(void);        /* 22.09: карта при загрузке не откатывается (memcard.c) */
        fprintf(stderr, "savestate: memory cards %s\n", memcard_ss_rewind()
                ? "REWOUND to the image inside the quicksave (DC_SS_CARD_REWIND=1)"
                : "kept as they are on disk - the card image inside the quicksave is skipped (DC_SS_CARD_REWIND=1 - rewind as before)");
    }
    dc_osd("Quicksave %d loaded", savestate_slot());
    return h.resume_pc;
}

/* ---- frame-boundary save trigger (debug_server_log_call_entry) ----------
 * Fires at every recompiled function ENTRY while a save is pending; accepts
 * only the VSync-wait entry (frame fully submitted, GPU/DMA drained) with
 * every peripheral quiescent. DC-specific anchor, DC_SS_ANCHOR overrides. */
void savestate_entry_hook(CPUState* cpu, uint32_t func_addr) {
    static uint32_t s_anchor = 0;
    if (!s_anchor) {
        const char* e = getenv("DC_SS_ANCHOR");
        s_anchor = e ? (uint32_t)strtoul(e, NULL, 0) : 0x8008786Cu;
    }
    if (func_addr != s_anchor) return;
    if (!(g_savestate_pending & SS_REQ_SAVE)) return;
    if (psx_get_in_exception()) return;

    extern int sio_savestate_quiescent(void);
    extern int dma_savestate_quiescent(void);
    static unsigned long s_defer = 0;
    if ((!sio_savestate_quiescent() || !dma_savestate_quiescent()) &&
        ++s_defer < 4000ul) {
        return;   /* try again at a later frame boundary */
    }
    if (s_defer >= 4000ul)
        fprintf(stderr, "savestate: bus never went idle - saving anyway (state may be torn)\n");
    s_defer = 0;
    g_savestate_pending &= ~SS_REQ_SAVE;
    savestate_do_save(cpu, func_addr);
    /* Remember which room this state belongs to: a quickload does not stream
     * the disc, so the sector-based room identity would be lost across it. */
    { extern void dc_room_save_id(void); dc_room_save_id(); }
    g_ss_delivlog = ss_probe_env("DC_SS_DELIVLOG"); g_ss_delivtag = 'S';
    {   const char* e = getenv("DC_SS_TRACE");
        if (e && atoi(e) > 0) {
            s_dtrace_n = atoi(e); s_dtrace_tag = 'S';
            g_savestate_pending |= SS_REQ_TRACE;
        }
    }
}

/* ---- dispatch-level trigger ---------------------------------------------- */
void savestate_dispatch_hook(CPUState* cpu, uint32_t target) {
    int op = g_savestate_pending;
    if (!op) return;

    if (op & SS_REQ_TRACE) {
        /* Post-save / post-load dispatch-stream logging: the two streams must
         * match instruction-for-instruction if the restore is faithful. */
        fprintf(stderr, "ssd%c %08X ra=%08X sp=%08X a0=%08X\n",
                s_dtrace_tag, target, cpu->gpr[31], cpu->gpr[29], cpu->gpr[4]);
        if (--s_dtrace_n <= 0) g_savestate_pending &= ~SS_REQ_TRACE;
    }

    /* Only at a clean boundary: no exception handler frames on the host
     * stack, guest state fully in RAM/devices. */
    if (psx_get_in_exception()) return;

    if (g_savestate_pending & SS_REQ_LOAD) {
        if (!g_savestate_anchor_armed) { g_savestate_pending &= ~SS_REQ_LOAD; return; }
        /* ⭐ 13.09.2026: ядро-HLE держит цепочки SysEnqIntRP и хук HookEntryInt
         * у себя (bios_hle.c), в savestate их нет. Del на экране запуска —
         * игра ещё не стартовала и ничего не зарегистрировала; грузить сейв в
         * таком процессе = пад никто не опрашивает («Регина не может ходить»).
         * Ждём, пока игра стартует и зарегистрирует обработчики (не дольше
         * DC_SS_WAIT_KERNEL vblank, 300), и только потом грузим. */
        {
            extern int hle_bios_enabled(void);
            extern int hle_kernel_ready(void);
            extern uint64_t s_frame_count;
            static uint64_t wait0 = 0; static int said = 0; static int lim = -1;
            if (lim < 0) { const char* e = getenv("DC_SS_WAIT_KERNEL"); lim = e ? atoi(e) : 300; if (lim < 0) lim = 0; }
            if (lim > 0 && hle_bios_enabled() && !hle_kernel_ready()) {
                if (!wait0) wait0 = s_frame_count ? s_frame_count : 1;
                if (s_frame_count - wait0 < (uint64_t)lim) {
                    if (!said) { said = 1;
                        fprintf(stderr, "savestate: the kernel has no game handlers yet (SysEnqIntRP/HookEntryInt) - waiting for the game to start, then loading\n");
                        dc_osd("Quicksave %d loads once the game starts", savestate_slot()); }
                    return;
                }
                fprintf(stderr, "savestate: handlers not registered within %d frames - loading as is\n", lim);
            } else if (said) {
                fprintf(stderr, "savestate: the kernel is ready (the game's handlers are in place) - loading\n");
            }
            wait0 = 0; said = 0;
        }
        if (!psx_on_main_fiber()) return;   /* defer until a main-fiber dispatch */
        g_savestate_pending &= ~SS_REQ_LOAD;
        if (!savestate_validate_file()) return;
        /* Unwind the whole guest host-stack; main()'s anchor reloads. */
        longjmp(g_savestate_jmpbuf, 1);
    }
}
