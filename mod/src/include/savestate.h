/* ============================================================================
 * savestate.h — emulator-style quicksave/quickload for the recompiled runtime.
 * ----------------------------------------------------------------------------
 * Capture happens at a psx_dispatch boundary (see savestate_dispatch_hook):
 * the only moments when the precise guest PC is known (the entry about to be
 * dispatched) and the whole machine lives in guest RAM + device modules.
 * Restore longjmps to an anchor in main(), reloads every module and re-enters
 * at the saved entry; the saved MIPS call chain above it resumes by following
 * $ra through the interpreter (g_interp_anywhere in dirty_ram_interp.c).
 * ========================================================================== */
#ifndef PSXRECOMP_SAVESTATE_H
#define PSXRECOMP_SAVESTATE_H

#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Symmetric field-list serializer: each module writes its state once and the
 * same function both saves and loads (no save/load drift). */
typedef struct PsxSaveCtx {
    FILE* f;
    int   loading;   /* 0 = writing, 1 = reading */
    int   error;     /* sticky I/O failure flag */
} PsxSaveCtx;

void ss_bytes(PsxSaveCtx* c, void* p, size_t n);
#define SS_VAR(c, v) ss_bytes((c), &(v), sizeof(v))

/* Request bits (set from the SDL key handler, consumed at dispatch level). */
#define SS_REQ_SAVE 1
#define SS_REQ_LOAD 2
extern volatile int g_savestate_pending;
void savestate_request(int op);

/* Called from fntrace_record on every dispatch when g_savestate_pending. */
void savestate_dispatch_hook(CPUState* cpu, uint32_t target);

/* Anchor support (main.cpp): setjmp target for the load unwind. */
extern jmp_buf g_savestate_jmpbuf;
extern int     g_savestate_anchor_armed;

/* After the longjmp lands at the anchor: reload everything.
 * Returns the resume PC (0 on failure — treat as fatal). */
uint32_t savestate_do_load(CPUState* cpu);

const char* savestate_path(void);

/* Per-module state functions (defined in each module). */
void memory_savestate(PsxSaveCtx* c);
void gpu_savestate(PsxSaveCtx* c);
void spu_savestate(PsxSaveCtx* c);
void cdrom_savestate(PsxSaveCtx* c);
void sio_savestate(PsxSaveCtx* c);
void mdec_savestate(PsxSaveCtx* c);
void dma_savestate(PsxSaveCtx* c);
void timers_savestate(PsxSaveCtx* c);
void interrupts_savestate(PsxSaveCtx* c);
void memcard_savestate(PsxSaveCtx* c);

/* Support hooks used by the restore path. */
void psx_threads_reset(void);            /* traps.c: drop all BIOS-thread fibers */
int  psx_on_main_fiber(void);            /* traps.c */
void pgxp_addr_reset(void);              /* pgxp_addr.c: drop stale float shadows */
extern int g_interp_anywhere;            /* dirty_ram_interp.c */
int  interp_anywhere_dispatch(CPUState* cpu, uint32_t addr);

/* Safe file writes (savestate.c): a file is written next to its target (name + ".tmp"), finished with
 * dc_file_finish (stream error, flush, the data on disk, close: 1 = all went fine) and put in place by
 * dc_replace_file in one operation (1 = done), so the old file stays whole until the new one is complete. */
int  dc_file_finish(FILE* f);
int  dc_replace_file(const char* tmp, const char* dst);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_SAVESTATE_H */
