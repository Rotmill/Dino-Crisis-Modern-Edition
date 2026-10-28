// gpu_vk_raster.cpp — DC GPU raster backend (stage 2 of the perf arc).
//
// Renders the PS1 primitive stream on the GPU at high internal resolution
// while the CPU keeps the exact 1x master VRAM for game logic, mask bits and
// readbacks (see DC_VULKAN_RASTER_PLAN.md). Device, queue and the R16_UINT
// VRAM image are shared with the PT path tracer via ptvk_core.h; this module
// owns its own command pool, offscreen color/depth targets, vertex stream and
// graphics pipelines (dc_raster.vert/.frag).
//
// Semi-transparency renders per PS1 rules: the SAME primitive carries opaque
// (texel bit15=0) and blended (bit15=1) texels, so a semi prim records two
// draws over one vertex range — pass 0 with the opaque pipeline (blending
// off, depth write) discarding bit15 texels, pass 1 with the mode's blend
// pipeline (no depth write) discarding the rest. Depth is K/w — linear in
// 1/w with GREATER compare, the same quantity the CPU z-buffer stores.

#include "ptvk_core.h"
#include "gpu.h"          /* GpuDisplayInfo: display window for the interface squeeze */
#include <vulkan/vulkan.h>
#include <windows.h>
#include <vulkan/vulkan_win32.h>   /* 09.10: общая с Direct3D 11 текстура (DC_VK_SHARED) */
#include <wincodec.h>
/* 10.10.2026: math.h явно - для Clang (libc++ не тянет его сам, как заголовки MSVC) */
#include <math.h>
#pragma comment(lib, "windowscodecs.lib")   /* 05.10: картинки PNG из mods (TEXREP) */
#pragma comment(lib, "ole32.lib")
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
extern int g_vk_conservative;   /* pt_vulkan.cpp: GPU supports conservative raster */
extern int g_vk_nonuniform_tex; /* pt_vulkan.cpp: 05.10 — массив текстур с номером из вершины (подмена текстур из mods) */
extern int g_vk_d3d11_share;    /* pt_vulkan.cpp: 09.10 — расширения общей с D3D11 текстуры включены */
const unsigned short* sw_vram_base(void);
void ptvk_upload_vram(const unsigned short*);
int dcvk_scale(void);
int dcvk_raster_init(void);
}

// ---------------------------------------------------------------- state ----
static const PtvkCore* C = nullptr;
extern "C" int dc_diag_log(void);   /* dc_boot.c: DC_DIAG_LOG, off in a release */
static FILE* s_log = nullptr;
/* 23.10.2026 (GitHub #1, #2: crashes on players' machines with nothing to go by): a release has no dcvk_raster.txt, so
 * what the game's log needs from here goes to stderr too - everything said while the raster comes up and the shaders
 * are prepared (s_rlogMirror), the shared Direct3D 11 texture and every failure; the per-frame chatter stays here. */
static std::atomic<int> s_rlogMirror{ 0 };
static void rlog(const char* fmt, ...) {
    const bool mirror = s_rlogMirror.load(std::memory_order_relaxed) || strstr(fmt, "FAILED") || strstr(fmt, "hang") ||
                        strstr(fmt, "shared:");
    if (!s_log && !mirror) return;
    char line[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (s_log) { fputs(line, s_log); fflush(s_log); }
    if (mirror) fputs(line, stderr);
}

// ---- 11.10.2026: pipeline cache --------------------------------------------------------------------------------
// Two friends of the player: the first screen, the title menu and the first in-engine cutscene came slowly and in jerks.
// Every graphics pipeline used to be built with no VkPipelineCache - at the first frame that needs it (the MSAA set at the
// title, the SSAO, glow and shadow passes, the set again when the world target grows wider at the first 16:9 frame). On a
// computer that has not built them yet the driver compiles the big raster shader for every one of them - seconds of
// frozen frames exactly at those three places; drivers without their own disk cache do it on every start. All creation
// goes through vkp_create: one cache shared by every pipeline, read at start from dc_pipelines.bin next to dc_user.cfg
// and written back (in the background) once new pipelines have been built. A file of another video card or driver is
// refused by its header (vendor, device, cache UUID) and by our own size/checksum. DC_VK_PIPECACHE=0 - no cache (as
// before); DC_VK_PIPELOG=1 - a line for every pipeline (otherwise only the ones over 5 ms).
static VkPipelineCache s_pcache = VK_NULL_HANDLE;
static std::mutex      s_pcMtx;
static int             s_pcNew = 0;          /* pipelines built since the last write */
static unsigned long long s_pcLastNew = 0;   /* GetTickCount64 of the last build */
static int             s_vkpN = 0;
static double          s_vkpMs = 0.0;
static const char*     PC_FILE = "dc_pipelines.bin";
static size_t          s_pcSize = 0;           /* size of the cache data read or last written - written again only when it grew */
/* 18.10.2026: for the shader preparation screen (main.cpp dc_shader_prep): -1 not read yet (or no cache), 0 the cache came
 * from the file, 1 it started empty (first start, another video card or driver, damaged file); and how many pipelines took
 * a real compile (60 ms and more) - a cache from an older build lacks the new shaders */
static std::atomic<int> s_pcCold{ -1 };
static std::atomic<int> s_vkpSlowN{ 0 };
static uint32_t pc_fnv(const uint8_t* p, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; } return h; }
static int pc_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("DC_VK_PIPECACHE"); on = (e && *e == '0') ? 0 : 1; } return on; }
static void pc_load(void) {
    if (!pc_on() || s_pcache) return;
    std::vector<uint8_t> data;
    const char* why = "no file yet";
    FILE* f = fopen(PC_FILE, "rb");
    if (f) {
        uint32_t hd[4] = { 0, 0, 0, 0 };
        if (fread(hd, 4, 4, f) == 4 && hd[0] == 0x43504344u /* DCPC */ && hd[1] == 1u && hd[2] > 32u && hd[2] < (64u << 20)) {
            data.resize(hd[2]);
            if (fread(data.data(), 1, data.size(), f) != data.size() || pc_fnv(data.data(), data.size()) != hd[3]) { data.clear(); why = "damaged file"; }
        } else why = "not our file";
        fclose(f);
    }
    if (!data.empty()) {                     /* VkPipelineCacheHeaderVersionOne: size, version, vendor, device, UUID */
        VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(C->phys, &pr);
        uint32_t h[4]; memcpy(h, data.data(), 16);
        if (h[0] < 32u || h[1] != 1u || h[2] != pr.vendorID || h[3] != pr.deviceID || memcmp(data.data() + 16, pr.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
            data.clear(); why = "another video card or driver";
        }
    }
    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = data.size(); ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(C->device, &ci, nullptr, &s_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0; ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(C->device, &ci, nullptr, &s_pcache) != VK_SUCCESS) s_pcache = VK_NULL_HANDLE;
    }
    if (data.empty()) fprintf(stderr, "pipecache: started empty (%s) - pipelines are built and kept in %s%c", why, PC_FILE, 10);
    else fprintf(stderr, "pipecache: %u KB from %s%c", (unsigned)(data.size() >> 10), PC_FILE, 10);
    s_pcSize = data.size();
    s_pcCold = data.empty() ? 1 : 0;
}
static void pc_write_now(void) {
    std::lock_guard<std::mutex> lk(s_pcMtx);
    if (!s_pcache || !C) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(C->device, s_pcache, &n, nullptr) != VK_SUCCESS || n < 32 || n == s_pcSize) return;
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(C->device, s_pcache, &n, data.data()) != VK_SUCCESS) return;
    data.resize(n);
    const uint32_t hd[4] = { 0x43504344u, 1u, (uint32_t)n, pc_fnv(data.data(), n) };
    std::string tmp = std::string(PC_FILE) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = fwrite(hd, 4, 4, f) == 4 && fwrite(data.data(), 1, n, f) == n;
    fclose(f);
    if (!ok) { remove(tmp.c_str()); return; }
    if (!MoveFileExA(tmp.c_str(), PC_FILE, MOVEFILE_REPLACE_EXISTING)) { remove(tmp.c_str()); return; }
    s_pcSize = n;
    fprintf(stderr, "pipecache: %u KB written to %s%c", (unsigned)(n >> 10), PC_FILE, 10);
}
/* once a frame: new pipelines and 2 s without another one - write the cache on a thread of its own */
static void pc_tick(void) {
    if (!s_pcache || !s_pcNew || GetTickCount64() - s_pcLastNew < 2000ull) return;
    s_pcNew = 0;
    std::thread([] { pc_write_now(); }).detach();
}
static std::mutex s_vkpMtx;
static VkResult vkp_create(const VkGraphicsPipelineCreateInfo* gp, VkPipeline* out, const char* what) {
    /* 23.10.2026: the name goes into the log BEFORE the driver compiles it - a crash inside the driver's shader compiler
     * (GitHub #2: right after the shader preparation, Intel UHD 630) then has this pipeline among the last lines */
    fprintf(stderr, "vkpipe: building %s%c", what, 10);
    LARGE_INTEGER fq, a, b; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
    const VkResult r = vkCreateGraphicsPipelines(C->device, s_pcache, 1, gp, nullptr, out);
    QueryPerformanceCounter(&b);
    const double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)fq.QuadPart;
    if (ms >= 60.0) s_vkpSlowN++;
    static int lg = -1; if (lg < 0) { const char* e = getenv("DC_VK_PIPELOG"); lg = (e && *e == '1') ? 1 : 0; }
    std::lock_guard<std::mutex> lk(s_vkpMtx);           /* sets of pipelines are built on several threads */
    s_vkpN++; s_vkpMs += ms; s_pcNew++; s_pcLastNew = GetTickCount64();
    if (lg || ms >= 5.0)
        fprintf(stderr, "vkpipe: %s %.1f ms (pipeline %d, %.0f ms in all)%c", what, ms, s_vkpN, s_vkpMs, 10);
    return r;
}
/* The pipelines of one set side by side: the driver compiles each on the calling thread, and one after another the four
 * distinct raster pipelines took 4 x 0.4 s with an empty driver cache (i9-13900KF; a slower processor - two or three times
 * that). DC_VK_PIPE_THREADS=0 - one after another (as before). */
static int vkp_threads_on(void) { static int on = -1; if (on < 0) { const char* e = getenv("DC_VK_PIPE_THREADS"); on = (e && *e == '0') ? 0 : 1; } return on; }
template <class F> static void vkp_par(int n, F f) {
    if (!vkp_threads_on() || n <= 1) { for (int i = 0; i < n; i++) f(i); return; }
    std::vector<std::thread> th; th.reserve((size_t)n);
    for (int i = 0; i < n; i++) th.emplace_back([&f, i] { f(i); });
    for (auto& t : th) t.join();
}

static VkCommandPool   s_pool  = VK_NULL_HANDLE;
static VkCommandBuffer s_cmd   = VK_NULL_HANDLE;
static VkFence         s_fence = VK_NULL_HANDLE;

static VkRenderPass          s_rp    = VK_NULL_HANDLE;
/* 21.09 вечер: кадр с захватом сцены (см. Scap) рисуется отрезками: первый — как s_rp, но глубина сохраняется; продолжение —
 * цвет и глубина загружаются. Совместимы с s_rp (форматы и число выборок те же) — конвейеры и кадровый буфер общие. */
static VkRenderPass          s_rpFirst  = VK_NULL_HANDLE;
static VkRenderPass          s_rpResume = VK_NULL_HANDLE;
/* ===== КАРТА ТЕНЕЙ ОТ ЛАМП ИГРЫ (18.09.2026) =============================
 * У растра есть буфер глубины и PGXP w, значит видовая точка фрагмента
 * восстановима — на этом и строится честная тень: глубина сцены снимается из
 * точки лампы (только подвижное: свет комнаты запечён в её вершинах), а в
 * основном проходе фрагмент проверяется по этой карте. */
/* 18.09 вечер: ламп в кадре несколько — слой карты на каждую, а их данные
 * переехали из констант в блок ShadowUBO. Константы вернулись к 128 байтам
 * (208 не даёт добрая половина устройств, и там тени просто не включались). */
#define SH_MAX 4
static VkImage         s_shImg  = VK_NULL_HANDLE;
static VkDeviceMemory  s_shMem  = VK_NULL_HANDLE;
static VkImageView     s_shView = VK_NULL_HANDLE;          /* массив слоёв — для чтения */
static VkImageView     s_shLayer[SH_MAX] = { VK_NULL_HANDLE };  /* по слою — цель рисования */
static VkSampler       s_shSamp = VK_NULL_HANDLE;
static VkRenderPass    s_shRp   = VK_NULL_HANDLE;
static VkFramebuffer   s_shFb[SH_MAX] = { VK_NULL_HANDLE };
static VkPipeline      s_shPipe = VK_NULL_HANDLE;
static int             s_shSize = 1024;
static int             s_shOk   = 0;          /* ресурсы созданы */
static int             s_shN    = 0;          /* ламп в этом кадре */
/* 09.10.2026: рамка заслонивших на слое карты (sh_occ_rects перед dcvk_render) — проходы луны и пирамид только в ней */
enum { SHR_FULL = 0, SHR_EMPTY = 1, SHR_RECT = 2 };
static int             s_shrSt[SH_MAX] = {};      /* слой в этом кадре: весь / пустой / рамка */
static int             s_shrRc[SH_MAX][4] = {};   /* рамка в текселях карты: x0, y0, x1, y1 (x1, y1 — не включая) */
/* рамка приёмника по рамке источника, крупнее в k раз; m — запас (тексели приёмника) на окно фильтра */
static void shr_down(const int* src, int k, int m, int R, int* dst) {
    dst[0] = src[0] / k - m;                 dst[1] = src[1] / k - m;
    dst[2] = (src[2] + k - 1) / k + m;       dst[3] = (src[3] + k - 1) / k + m;
    if (dst[0] < 0) dst[0] = 0; if (dst[1] < 0) dst[1] = 0; if (dst[2] > R) dst[2] = R; if (dst[3] > R) dst[3] = R;
}
static VkBuffer        s_shUbo    = VK_NULL_HANDLE;
static VkDeviceMemory  s_shUboMem = VK_NULL_HANDLE;
static float*          s_shUboMap = nullptr;
/* раскладка блока (float): 0..63 матрицы ламп, 64..79 параметры,
 * 80..95 позиции ламп, 96..99 общее (число ламп, смещение, 1/размер, отладка),
 * 100..103 прочее (самозатенение персонажа и его множитель смещения),
 * 104..111 (25.09) вспышка выстрела/взрыва как свет: flp = точка в видовом пространстве + радиус (0 — нет),
 * flc = цвет × яркость + «обтекание» (pt_renderer.c dc_flash_frame),
 * 112..119 (25.09 вечер) луна: mdir = направление на неё в видовом пространстве + её слой карты (-1 — нет), mcol = цвет × сила +
 * «обтекание» (dcvk_moon_set), 120..127 отражения на полу: rfp = плоскость пола (нормаль + d), rfc = (-, -, допуск, вкл),
 * 128..135 (25.09 ночь) материалы пола: rfm0 = кафель сила/размытие, металл сила/размытие; rfm1 = глянец сила/размытие, оттенок
 * металла, (27.09) металл светлеет только от отражённого; 136..139 луна гладкой тенью: mpf = вкл, зазор до полного размытия, смещение, ширина перехода; 140 — число
 * источников света от светящихся текстур (25.09 поздно), 141..143 — запас; 144..175 glp[8] = их точки в видовом пространстве
 * кадра A + радиус, 176..207 glc[8] = цвет × сила + «обтекание»;
 * 25.09 ночь: 141 — блики материалов вкл, 142 — ламп для них; 208..211 msp = кафель степень/сила, металл степень/сила,
 * 212..215 msp2 = глянец степень/сила, металл в свой цвет, доля бликов от светящихся текстур; 216..231 spp[4] = лампы игры
 * для бликов (точка в видовом пространстве кадра A + радиус), 232..247 spc[4] = их цвет × сила (pt_renderer.c dc_spec_frame);
 * 248..251 senv = «вверх» комнаты в видовом пространстве + есть ли; 252..255 msp3 = отблеск окружения кафель/металл/глянец;
 * 27.09 ОБЪЁМНЫЙ СВЕТ (dc_vol_common.glsl): 256..259 vcfg = ламп игры для дымки, -, -, матрицы годны; 260..271 v2r = вид -> комната
 * (3 строки: xyz + перенос), 272..283 r2v = комната -> вид; 284..287 запас; 288..319 vlp[8] = лампы (точка вида кадра A + радиус),
 * 320..351 vlc[8] = цвет + слой карты теней (-1 — нет);
 * 27.09 ТЕНИ ОТ ЛАМП-ТЕКСТУР: 352..367 lsh[4] = эллипс мягкости слоя карты (столбцы — оси лампы в плоскости карты; 1 0 0 1 — круг);
 * 27.09 (2) ТЕНИ БЕЗ ЗЕРНА И ДОЛЯ СВЕТА (dcvk_shadow_v2): 368..371 spf = новый путь (ставит кадр, когда пирамида построена), доля
 * общего света комнаты, смещение, переход (ед.); 372..375 spw = сила света лампы слоя; 376..379 sps = ламп «только свет», обтекание;
 * 380..395 skA[4] = полуоси полутени слоя (UV · расстояние); 396..427 slp[8] = лампы без своей тени (точка вида + радиус);
 * 428..435 slw[2] = их сила;
 * 27.09 вечер ШЕРОХОВАТЫЙ МЕТАЛЛ (dc_raster.frag): 436..439 mrg = шероховатость, сила рельефа из текстуры, матовость
 * ржавчины и грязи, зерно (все 0 — металл как был);
 * 28.09 (6) ПЕЛЕНА ИГРЫ ПОВЕРХ ДЫМКИ (dc_vol_common.glsl, см. «ПЕЛЕНА ИГРЫ» у veil_collect): 284 (vspare.x) = число полос,
 * 440..455 veilV[4] = полоса во всю ширину: строка от, строка до (точки цели), множитель, вычитание; 456..459 glint (блеск предметов), 460..463 - запас
 * 19.10.2026 СВЕТ СВЕТЯЩИХСЯ ТЕКСТУР, ИСТОЧНИКИ 9..16 (dc_raster.frag): 143 (spare.w) = сколько их, 464..495 glp2[8] = точка вида +
 * радиус, 496..527 glc2[8] = цвет × сила + обтекание (первые 8 — по-прежнему 140, 144..207; дымка и пыль берут только их)
 * 19.10.2026 вечер ЕЁ ДОВОРОТ В КАДРАХ МЕЖДУ ТАКТАМИ (warp_pc_make, dc_raster.vert): 528..531 hq = кватернион её поворота в виде
 * кадра A, 532..534 hc = точка на её оси, 535 (hc.w) = 1 - доворот есть
 * 20.10.2026 ЕЁ РУКИ, КИСТИ С ОРУЖИЕМ И ГОЛОВА МЕЖДУ ТАКТАМИ (arm_warp_make, dc_raster.vert): 536..567 bq[8] = кватернион
 * вершин кости в виде кадра A, 568..599 bt[8] = перенос + (w) есть
 * 25.10.2026 ВЕСЬ ЕЁ СКЕЛЕТ (15 костей, arm_warp_make): 536..599 bq[16], 600..663 bt[16]
 * 26.10.2026 ПАРЫ КУСКОВ (pair_pc_make, dc_raster.vert / dc_fx.vert): 664..667 pq = (x > 0.5 - пары вместо камеры, y - проход
 * кадра A & 3, z - число пар), 668.. pr[PAIR_MAX * 3] = строки матрицы куска на эту долю пути (xyz) + сдвиг (w)
 * 28.10.2026 ВСЕ ЛАМПЫ КОМНАТЫ СРАЗУ (gl_room_out; dc_raster.frag, dc_vol_common.glsl): SH_UBO_GL3.. glp3[16] = источники
 * «светится» 17..32 (точка вида + радиус), +64.. glc3[16] = цвет × сила + обтекание, +128.. ghz[8] = доля дымки места 0..31
 * (прежние пути: 1 у мест 0..7, 0 у прочих - дымка у первых 8, как было); 143 (spare.w) - сколько источников за первыми 8 (до 24) */
#define PAIR_MAX 256
#define SH_UBO_GL3 (668 + PAIR_MAX * 12)
#define SH_UBO_FLOATS (SH_UBO_GL3 + 160)

static float s_shWp[4] = { 0.f, 0.f, 0.f, 0.f };   /* ox, oy, H, H * сжатие */

extern "C" void dcvk_shadow_set(const float* vp, const float* par, const float* lpos,
                                int n, float bias, float dbg, float self_shadow, float rsel_k,
                                float spread_k) {
    if (!s_shOk || !s_shUboMap) return;
    if (n > SH_MAX) n = SH_MAX;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 16; k++) s_shUboMap[i * 16 + k] = vp[i * 16 + k];
        for (int k = 0; k < 4; k++) {
            s_shUboMap[64 + i * 4 + k] = par[i * 4 + k];
            s_shUboMap[80 + i * 4 + k] = lpos[i * 4 + k];
        }
    }
    s_shUboMap[96] = (float)n;
    s_shUboMap[97] = bias;
    s_shUboMap[98] = 1.0f / (float)s_shSize;
    s_shUboMap[99] = dbg;
    s_shUboMap[100] = self_shadow;      /* тень на самом персонаже: 0 — выкл */
    s_shUboMap[101] = 4.0f;             /* ей нужно смещение побольше: рёбра модели грубые */
    s_shUboMap[102] = rsel_k;           /* доля от lpos.w = радиус, в котором лампа его освещает */
    s_shUboMap[103] = spread_k;         /* доля от lpos.w = на чём тень распыляется к своему концу */
    s_shN = n;
}
/* 27.09 (2): тени без зерна — хочет ли кадр новый путь и каким слоям строить пирамиду (dcvk_shadow_v2) */
static int s_spfWant = 0, s_spfMask = 0;
extern "C" void dcvk_shadow_clear(void) {
    s_shN = 0; s_spfWant = 0; s_spfMask = 0;
    if (s_shUboMap) { s_shUboMap[96] = 0.f; s_shUboMap[115] = -1.f; s_shUboMap[368] = 0.f; }
}
/* 27.09 (2): ТЕНИ БЕЗ ЗЕРНА И ДОЛЯ СВЕТА (pt_renderer.c dc_shadow_draw, новый путь). amb — доля общего (запечённого) света
 * комнаты, bias/tw — смещение и ширина перехода по глубине (ед.), wrap — обтекание граней; на слой — сила света лампы (pow4) и
 * полуоси полутени в плоскости карты (ax16: длинная xy, короткая zw; UV · расстояние); лампы без своей тени, только в долю света:
 * lp32 — точка вида + радиус, lw8 — сила, nlo <= 8; mask — слои ламп, которым строить пирамиду. on = 0 — прежний путь. Сам
 * новый путь (spf.x) включает кадр — когда пирамида этого кадра построена (см. «ТЕНИ БЕЗ ЗЕРНА»). */
extern "C" void dcvk_shadow_v2(int on, float amb, float bias, float tw, float wrap, const float* pow4, const float* ax16,
                               int mask, const float* lp32, const float* lw8, int nlo) {
    if (!s_shUboMap) return;
    s_spfWant = on ? 1 : 0;
    s_spfMask = on ? (mask & 15) : 0;
    if (nlo < 0) nlo = 0;
    if (nlo > 8) nlo = 8;
    s_shUboMap[368] = 0.f;
    s_shUboMap[369] = amb; s_shUboMap[370] = bias; s_shUboMap[371] = tw;
    for (int i = 0; i < 4; i++) s_shUboMap[372 + i] = pow4 ? pow4[i] : 0.f;
    s_shUboMap[376] = (float)nlo; s_shUboMap[377] = wrap; s_shUboMap[378] = 0.f; s_shUboMap[379] = 0.f;
    for (int i = 0; i < 16; i++) s_shUboMap[380 + i] = ax16 ? ax16[i] : 0.f;
    for (int i = 0; i < 32; i++) s_shUboMap[396 + i] = (lp32 && i < nlo * 4) ? lp32[i] : 0.f;
    for (int i = 0; i < 8; i++)  s_shUboMap[428 + i] = (lw8 && i < nlo) ? lw8[i] : 0.f;
}
/* 27.09: форма мягкости тени на слой карты (pt_renderer.c, лампы-текстуры): e16 — по 4 числа на слой (столбцы эллипса),
 * слоям >= n и без e16 — круг */
extern "C" void dcvk_shadow_shape(const float* e16, int n) {
    if (!s_shUboMap) return;
    for (int i = 0; i < SH_MAX; i++) {
        const bool ok = e16 && i < n;
        s_shUboMap[352 + i * 4 + 0] = ok ? e16[i * 4 + 0] : 1.f;
        s_shUboMap[352 + i * 4 + 1] = ok ? e16[i * 4 + 1] : 0.f;
        s_shUboMap[352 + i * 4 + 2] = ok ? e16[i * 4 + 2] : 0.f;
        s_shUboMap[352 + i * 4 + 3] = ok ? e16[i * 4 + 3] : 1.f;
    }
}
/* 25.09 ночь: лампы игры для бликов материалов (pt_renderer.c dc_spec_frame, кадр A): pos4 — точка в видовом пространстве +
 * радиус, col4 — цвет × сила; n <= 4 */
extern "C" void dcvk_spec_lamps(const float* pos4, const float* col4, int n) {
    if (!s_shUboMap) return;
    if (n < 0 || !pos4 || !col4) n = 0;
    if (n > 4) n = 4;
    for (int i = 0; i < n * 4; i++) { s_shUboMap[216 + i] = pos4[i]; s_shUboMap[232 + i] = col4[i]; }
    s_shUboMap[142] = (float)n;
}
/* 27.09: ОБЪЁМНЫЙ СВЕТ У ЛАМП (pt_renderer.c dc_vol_frame, кадр A): лампы игры для дымки — pos4 = точка вида + радиус, col4 =
 * цвет + слой карты теней (-1 — нет), n <= 8; v2r12/r2v12 — «вид -> комната» и «комната -> вид» (3 строки: xyz + перенос).
 * ok = 0 — ничего нет (ламп и матриц) */
extern "C" void dcvk_vol_set(const float* pos4, const float* col4, int n, const float* v2r12, const float* r2v12, int ok) {
    if (!s_shUboMap) return;
    if (!ok || !v2r12 || !r2v12) { s_shUboMap[256] = 0.f; s_shUboMap[259] = 0.f; return; }
    if (n < 0 || !pos4 || !col4) n = 0;
    if (n > 8) n = 8;
    s_shUboMap[256] = (float)n; s_shUboMap[257] = 0.f; s_shUboMap[258] = 0.f; s_shUboMap[259] = 1.f;
    for (int i = 0; i < 12; i++) { s_shUboMap[260 + i] = v2r12[i]; s_shUboMap[272 + i] = r2v12[i]; }
    for (int i = 0; i < n * 4; i++) { s_shUboMap[288 + i] = pos4[i]; s_shUboMap[320 + i] = col4[i]; }
}
/* 25.09 ночь: «вверх» комнаты (к потолку) в видовом пространстве кадра A — для отблеска окружения у материалов */
extern "C" void dcvk_spec_up(const float* up, int ok) {
    if (!s_shUboMap) return;
    if (!ok || !up) { s_shUboMap[251] = 0.f; return; }
    s_shUboMap[248] = up[0]; s_shUboMap[249] = up[1]; s_shUboMap[250] = up[2]; s_shUboMap[251] = 1.f;
}
/* ⭐ 25.09 ночь — ОБЩИЙ ВЫКЛЮЧАТЕЛЬ МАТЕРИАЛОВ (меню GRAPHICS -> Materials, main.cpp; F8). 0 — ничего из выбора в
 * приложении: ни отражений пола, ни бликов, ни свечения и света текстур «светится» (двусторонние — не материал, остаются). */
extern "C" { int g_dc_gfx_materials = 1; }
/* 25.09 вечер: лунный свет — направление на луну в видовом пространстве кадра, её слой карты теней (layer < 0 — луны нет) */
extern "C" void dcvk_moon_set(const float* dir, const float* col, float layer, float wrap) {
    if (!s_shUboMap) return;
    if (!dir || !col || layer < 0.f) { s_shUboMap[115] = -1.f; return; }
    s_shUboMap[112] = dir[0]; s_shUboMap[113] = dir[1]; s_shUboMap[114] = dir[2]; s_shUboMap[115] = layer;
    s_shUboMap[116] = col[0]; s_shUboMap[117] = col[1]; s_shUboMap[118] = col[2]; s_shUboMap[119] = wrap;
}
/* 25.09 ночь: ЛУНА ГЛАДКОЙ ТЕНЬЮ (см. «ЛУНА ГЛАДКОЙ ТЕНЬЮ» ниже) — от pt_renderer.c: вкл, шаг размытия в текселях ужатой
 * карты (σ = 3 шага), зазор «заслонивший -> поверхность», на котором тень размыта полностью, смещение и ширина перехода
 * по глубине (единицы комнаты). on = 0 — прежний путь (PCSS с поворотом на пиксель). */
static float s_mpfPar[4] = { 1.f, 1500.f, 40.f, 80.f };
static int   s_mpfWant = 0;
static int   s_mpfRes  = 1024;
extern "C" void dcvk_moon_pf(int on, float step, float gapFull, float bias, float softZ) {
    s_mpfWant = on ? 1 : 0;
    s_mpfPar[0] = step; s_mpfPar[1] = gapFull; s_mpfPar[2] = bias; s_mpfPar[3] = softZ;
}
extern "C" int dcvk_moon_pf_res(void) { return s_mpfRes; }
/* 25.09: вспышка как свет — одна на кадр; rad <= 0 или pos == nullptr — нет */
static int s_flOn = 0;
static int gl_chr_smooth(void);   /* 28.09: персонажи — свет без граней (параметр glow_light_chr, ниже таблицы) */
extern "C" void dcvk_flash_set(const float* pos, float rad, const float* col, float wrap) {
    if (!s_shUboMap) { s_flOn = 0; return; }
    if (!pos || !col || rad <= 0.f) {
        s_flOn = 0;
        for (int i = 104; i < 112; i++) s_shUboMap[i] = 0.f;
        return;
    }
    s_shUboMap[104] = pos[0]; s_shUboMap[105] = pos[1]; s_shUboMap[106] = pos[2]; s_shUboMap[107] = rad;
    s_shUboMap[108] = col[0]; s_shUboMap[109] = col[1]; s_shUboMap[110] = col[2];
    s_shUboMap[111] = gl_chr_smooth() ? wrap : -1.f - wrap;   /* знак: < 0 — персонажи по граням, как было */
    s_flOn = 1;
}
extern "C" int  dcvk_shadow_ready(void) { return s_shOk; }

static VkDescriptorSetLayout s_dsl   = VK_NULL_HANDLE;
static VkPipelineLayout      s_plLay = VK_NULL_HANDLE;
static VkDescriptorPool      s_dpool = VK_NULL_HANDLE;
static VkDescriptorSet       s_dset  = VK_NULL_HANDLE;

// pipeline kinds: 0 = opaque+depth(test+write), 1 = opaque no-depth,
// 2..5 = blend mode 0..3 with depth test (no write),
// 6..9 = blend mode 0..3 without depth.
/* Set while the room we draw ourselves is being submitted. Those triangles
 * are produced at the END of the frame, after everything the game drew - the
 * subtitles and HUD included - so with depth writes on they painted over the
 * text. They belong UNDER the frame, not on top of it: the draw loop runs them
 * first. */
extern "C" int g_dcvk_room_pass = 0;
/* Draw the next triangles as EDGES over the finished picture, whatever the
 * pipeline they would otherwise use. The camera-collision overlay rides this:
 * the shapes are invisible in the world, so the only way to check they sit
 * where the walls are is to see them drawn on top of the walls. */
extern "C" int g_dcvk_wire_pass = 0;
static VkPipeline s_pipes[13] = {};   /* 06.10.2026: 12 — меш из mods (dc_mesh.vert, см. dcvk_mesh_add) */   /* 10 = wireframe overlay; 11 = стекло (01.10 вечер: альфа по Френелю, предумноженная) */
static VkPipeline s_bgPipe = VK_NULL_HANDLE;   // fullscreen VRAM background
static int s_flat_frame = 0;   /* плоский экран (нет 3D): 2D-примитивы сжаты к 4:3,
                                * фоновый проход VRAM сжимаем так же — иначе ролик/меню
                                * из загрузок в VRAM двоится (блиты сжаты, фон — нет) */

static VkImage        s_colImg = VK_NULL_HANDLE;
static VkDeviceMemory s_colMem = VK_NULL_HANDLE;
static VkImageView    s_colView = VK_NULL_HANDLE;
static VkImage        s_depImg = VK_NULL_HANDLE;
static VkDeviceMemory s_depMem = VK_NULL_HANDLE;
static VkImageView    s_depView = VK_NULL_HANDLE;
/* ПРОШЛЫЙ HI-RES КАДР ДЛЯ «КОПИИ КАДРА» (15.09.2026, игрок: «доктор в брифинге какой-то
 * двойной»). Терминал брифинга каждый кадр кладёт поверх нового кадра ПОЛУПРОЗРАЧНУЮ копию
 * прошлого (два квада 15 bpp, tp 256/258, B/2 + F/2 — послесвечение экрана). Текстуры растр
 * берёт из VRAM 1x, а там кадр софт-растра: бюст в нём уже ужат широкоэкранным GTE по X
 * (замер кадрами: голова 100 px против 130 у hi-res) — поверх hi-res бюста ложился второй,
 * узкий и зернистый. Консольный смысл копии — «прошлый кадр», поэтому на экранах вне мира
 * копия кадра читает цвет из нашего прошлого hi-res кадра, а прозрачность и бит 15 — из
 * VRAM, как раньше. DC_COPY_HIRES=0 — прежнее поведение (VRAM 1x). */
static VkImage        s_prevImg = VK_NULL_HANDLE;
static VkDeviceMemory s_prevMem = VK_NULL_HANDLE;
static VkImageView    s_prevView = VK_NULL_HANDLE;
static VkSampler      s_prevSamp = VK_NULL_HANDLE;
static int            s_prevOnGpu = 0;           /* образ уже переведён в SHADER_READ_ONLY */
static int            s_prevValid = 0;           /* в образе лежит отрисованный кадр */
static int            s_prevRect[4] = { 0, 0, 0, 0 };   /* его прямоугольник дисплея в draw-space */
static int            s_prevActW = 0, s_prevActH = 0;   /* его размер в пикселях образа */
static VkFramebuffer  s_fb = VK_NULL_HANDLE;
static int s_fbW = 0, s_fbH = 0;

static VkBuffer       s_vbuf = VK_NULL_HANDLE;
static VkDeviceMemory s_vmem = VK_NULL_HANDLE;
static float*         s_vmap = nullptr;
static VkBuffer       s_rbBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_rbMem = VK_NULL_HANDLE;
static void*          s_rbMap = nullptr;
static size_t         s_rbSize = 0;
/* 05.10.2026: ТЕКСТУРЫ PNG ИЗ MODS (TEXREP, реализация — у dcvk_raster_init): второй буфер вершин — 4 float на вершину
 * {номер картинки замены (−1 — нет), u, v в картинке, 0}; шейдер берёт его как location 5, массив картинок — биндинг 8 */
#define TREP_MAX 512
/* 24.10.2026: мест в массиве на ЭТОЙ видеокарте (buildFixed по её лимитам; DC_TREP_CAP=<n> - меньше для проверки). У Intel
 * в фрагментном шейдере до 64 сэмплеров, 7 + 512 мест драйвер принимал и падал на первом кадре (igvk64.dll, два игрока).
 * Размер массива в шейдере - константа специализации 0 (dc_raster.frag DC_REP_N), её берут все конвейеры с этим шейдером. */
static int32_t s_trepCap = TREP_MAX;
static const VkSpecializationMapEntry s_repSpecMap = { 0, 0, sizeof(int32_t) };
static const VkSpecializationInfo s_repSpec = { 1, &s_repSpecMap, sizeof(int32_t), &s_trepCap };
/* 25.10.2026: ЛЁГКИЕ КОНВЕЙЕРЫ ПО ОПЦИЯМ (dc_raster.frag DC_F_SHADOW, DC_F_MATER - константы специализации 1, 2). Конвейеры
 * растра (обычные, MSAA, меш из mods) собираются с опциями меню: что опция выключила, того в шейдере нет вовсе - меньше
 * регистров, основной проход быстрее (замер DC_BENCH_WARP, 4K, всё выкл, RTX 4080: 0.58 -> 0.42 мс). Картинка та же: без
 * опции эти ветки и так не шли. Опция сменилась - raster_feat_sync (ниже): новый набор строится в фоне в кэш конвейеров,
 * кадры идут прежним, потом он из кэша. Маска: 1 - Shadows, 2 - Materials. DC_RASTER_FEAT=0 - всё в шейдере, как было. */
/* 28.10.2026: msFull - константа 3 (DC_MS_FULL): полная маска выборок MSAA «по выборкам» (dc_raster_sl) - внутри полигона
 * тексель читается раз на пиксель, а не на каждую выборку (там все выборки в одной точке текстуры). 0 - как было. */
struct RasterSpec { int32_t repN; VkBool32 shadow, mater; int32_t msFull; };
static const VkSpecializationMapEntry s_rspecMap[4] = {
    { 0, (uint32_t)offsetof(RasterSpec, repN), sizeof(int32_t) },
    { 1, (uint32_t)offsetof(RasterSpec, shadow), sizeof(VkBool32) },
    { 2, (uint32_t)offsetof(RasterSpec, mater), sizeof(VkBool32) },
    { 3, (uint32_t)offsetof(RasterSpec, msFull), sizeof(int32_t) } };
extern "C" int g_dc_gfx_shadows;                       /* pt_renderer.c: GRAPHICS -> Shadows */
static int raster_feat_want(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_RASTER_FEAT"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return 3;
    return (g_dc_gfx_shadows ? 1 : 0) | (g_dc_gfx_materials ? 2 : 0);
}
static int s_featPipes = 3, s_featMS = 3;               /* с какими опциями собраны s_pipes и s_pipesMS */
static int s_featGoal = -1;                             /* с какими должны быть сейчас (raster_feat_sync), -1 - ещё не решено */
static int raster_feat_goal(void) { return s_featGoal >= 0 ? s_featGoal : raster_feat_want(); }
/* 25.10.2026: БЕЗОПАСНЫЕ ШЕЙДЕРЫ. Игрок с Intel Iris Xe на v1.04: «The modern graphics could not start ... the renderer could not
 * start» (конвейеры растра не собрались, игра ушла в картинку PS1); у UHD 630 и Iris Plus в журнале «[VKR] mesh pipeline ...
 * FAILED», потом падение на первом кадре. Конвейеры растра не собрались - buildFixed поднимает их ещё раз: 1 - шейдеры без
 * выбора картинки из массива по пикселю (dc_raster*_safe, dc_fx_safe: DC_SAFE в dc_raster.frag), одно место в массиве,
 * текстуры из mods выключены; 2 - ещё и без света, теней и материалов (dc_raster_min: DC_LEAN), без MSAA и консервативной
 * растеризации. Устройство без shaderSampledImageArrayNonUniformIndexing начинает сразу с 1 (обычный шейдер объявляет эту
 * возможность). DC_RASTER_SAFE=<1|2> - начать с этого уровня (проверка). Причина отказа - dcvk_last_error (окно, main.cpp). */
static int  s_rasterSafe = 0;
static char s_rasterErr[160] = "";
extern "C" const char* dcvk_last_error(void) { return s_rasterErr; }
static const char* raster_fs_name(int sshade) {
    if (s_rasterSafe >= 2) return "shaders\\dc_raster_min.frag.spv";
    if (s_rasterSafe) return sshade == 2 ? "shaders\\dc_raster_sl_safe.frag.spv"
                           : sshade ? "shaders\\dc_raster_ss_safe.frag.spv" : "shaders\\dc_raster_safe.frag.spv";
    return sshade == 2 ? "shaders\\dc_raster_sl.frag.spv" : sshade ? "shaders\\dc_raster_ss.frag.spv" : "shaders\\dc_raster.frag.spv";
}
static const char* fx_fs_name(void) { return s_rasterSafe ? "shaders\\dc_fx_safe.frag.spv" : "shaders\\dc_fx.frag.spv"; }
static VkBuffer       s_rvBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_rvMem = VK_NULL_HANDLE;
static float*         s_rvMap = nullptr;
/* ⭐ 26.10.2026 ПАРЫ КУСКОВ (кадры между тактами у дверей, см. «ПАРЫ КУСКОВ» у pair_build): третий поток вершин - метка куска
 * модели (gte.cpp pair_note: (проход & 3) << 13 | номер куска + 1; 0 - нет), привязка 2, location 6 */
static VkBuffer       s_pvBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_pvMem = VK_NULL_HANDLE;
static uint32_t*      s_pvMap = nullptr;
static bool trep_gpu_init(void);
static void trep_tri(const float* v, float* rv);
static void trep_frame_prepare(void);
static void trep_frame_upload(VkCommandBuffer cmd);

// vertex stream: 13 floats per vertex (pos2 uv2 color4 info4 w1)
#define VERT_FLOATS 13
static void fuse_note2(const float* v, int semi, int ztest);   /* flicker fuser, defined below */
static int s_fuse_replaying;                                   /* nonzero while the fuser replays */
#define MAX_VERTS   (256 * 1024)
static int s_nverts = 0;
/* 09.10.2026: треугольники заслонивших (персонажи, правило dc_shadow.vert) — копия в памяти процессора для рамки на картах
 * теней (sh_occ_rects). s_vmap процессору читать нельзя: это память видеокарты, чтение идёт мимо кэша (13 мс на кадр).
 * На вершину 5 чисел: x, y, u (метка точки вида -1000), w, флаги. */
static std::vector<float> s_occCpu;
static void occ_note(const float* v, int nv) {
    for (int t = 0; t + 2 < nv; t += 3) {
        const float* tv = v + (size_t)t * 13;
        int hit = 0;
        for (int k = 0; k < 3 && !hit; k++) {
            const float* q = tv + k * 13;
            const int fl = (int)lrintf(q[11]);
            hit = (fl & (16 | 8388608)) != 0 && (fl & 2) == 0 && (q[2] < -500.f || q[12] >= 1.5f);
        }
        if (!hit) continue;
        for (int k = 0; k < 3; k++) { const float* q = tv + k * 13; s_occCpu.insert(s_occCpu.end(), { q[0], q[1], q[2], q[12], q[11] }); }
    }
}
static int s_prim_seq = 0;   /* per-triangle submit order (painter tiebreak) */
/* THE ROOM WE INJECT MUST NEVER WIN A TIE.
 * The tiebreaker nudges a primitive toward the camera in proportion to its
 * submit order, and our room is pushed after the whole game stream - so it got
 * the highest numbers and quietly won against everything the game lays flat on
 * our surfaces. That is where the character's shadow and the blood went
 * (present in one room, gone in the next, depending on how the depths happened
 * to land). Our triangles are numbered from far below zero instead: under the
 * entire game stream, while keeping their own relative order, so a panel still
 * beats the wall it hangs on. */
/* The room's band sits just under the game's stream (which starts at 0), far
 * enough to lose every tie and no further: -4000..-3100 is at most 8e-4 of
 * relative depth, an order below the 0.5% z-bias the room body already has. */
#define ROOM_SEQ_BASE (-4000)
static int s_room_seq = 0;
extern "C" int g_dcvk_room_seq_mode = 0;   /* 16.09: замер Z-fight, см. штамп порядка ниже */

struct Draw { int first, count, kind, pass; int room; int wire;
              int late; /* translucent-last pair: composited after the world */
              int ui;   /* 2D по построению (g_dcvk_ui2d): интерфейс игры — поверх мира (12.09) */
              int clip = 0;               /* 15.09: область рисования игры (scissor при записи) */
              float cr[4] = { 0, 0, 0, 0 };   /* ...x0,y0 — x1,y1 в draw-space, правый/нижний край ИСКЛЮЧИТЕЛЬНО */
              int sqz = 0;                /* 21.09: кандидат сжатия интерфейса: 1 — вершины сжаты, 2 — как есть (dcvk_sqz_resolve) */
              float sqc = 0.f, crx0 = 0.f, crx1 = 0.f;   /* ...центр сжатия и родные x-границы клипа */
              int scap = 0;               /* 21.09 вечер: индекс+1 захвата сцены (s_scaps), из которого берётся текстура */
              int mesh = -1, inst = 0, half = 0;   /* 06.10.2026: меш из mods на видеокарте (dcvk_mesh_draw): пул, экземпляр, половина буферов */
              int mocc = 0;               /* ...и он отбрасывает тень (персонаж) */
              int pdim = 0;               /* 10.10: наша ровная плита паузы - между миром и надписями (dcvk_render) */
              int mref = -1;              /* 25.10: в снимке кадра A (WarpSnap) - номер меша в его mrefs */ };
/* 25.10.2026: меш снимка кадра A - буферы блока пула и диапазоны на момент снимка. Кадры между тактами рисует поток показа
 * (main.cpp DC_PRESENT_THREAD), пока эмуляция может дописать модель в s_mesh / s_mblk (векторы переезжают в памяти), - меш
 * он берёт отсюда. Буферы блоков пула не освобождаются. */
struct MeshRef { VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE; uint32_t icount = 0, ibase = 0; int32_t vbase = 0; };
static const MeshRef* mesh_ref_of(const Draw& d, const std::vector<MeshRef>& mrefs) {
    static const MeshRef none{};
    return (d.mref >= 0 && d.mref < (int)mrefs.size()) ? &mrefs[(size_t)d.mref] : &none;
}
static void medx_record(VkCommandBuffer cmd, VkPipeline pipe, const Draw& d, const MeshRef* mr = nullptr);   /* 06.10.2026: draw вида «меш» (ниже, у dcvk_mesh_add); 25.10: mr - меш снимка */
static VkPipeline mesh_fx_pipe(void);
static std::vector<Draw> s_draws;
static std::vector<Draw> s_semiLate;   /* world semis, composited after all opaque */
static int s_lastUiDraws = -1;         /* 27.09 вечер: примитивов интерфейса в последнем кадре (dcvk_last_ui_draws) */
/* 23.09: надпись DANGER (страница эффектов 896,256, см. dc_tex_is_danger в gpu.c) — треугольник рисуется без проверки
 * глубины и помечается интерфейсом: порядок вывода кладёт такие draw в верхнюю группу кадра. Место на экране — как было. */
extern "C" int dc_tex_is_danger(int tpage, const float* v, int stride);
static int s_topNow = 0;
extern "C" int dc_room_owed(void);     /* 23.09: pt_renderer.c — таблица в потоке из прохода, где комнату игры забрали мы (+1) */
/* ⭐ 21.09.2026 вечер — ЗАХВАТ СЦЕНЫ ПОСРЕДИ КАДРА (игрок, слот 6, сцена в Experiment Simulation Room: «шейдер "третья
 * энергия" сломан. Совершенно не такой, как в оригинале — там это как дисторшн, искажение пространства»).
 * Замер (кольцо GP0 кадра сцены): игра 8 раз за кадр делает одно и то же — GP0 0x80 копирует кусок 64x64 ТЕКУЩЕГО кадра
 * (из буфера, в который рисует) в служебную текстуру VRAM (512,0) и тут же кладёт его обратно на то же место четырьмя
 * непрозрачными квадами 15 bpp (цвет 8A8A8A), у которых общая ЦЕНТРАЛЬНАЯ вершина сдвинута на ~15 точек, а UV ровные: фон
 * под ячейкой втягивается/выпучивается, ячейки перекрываются и искажают уже искажённое — вот и «дисторшн». У нас в VRAM 1x
 * комнаты нет (её рисует наш эмиттер сразу в hi-res), поэтому копия несла чёрное с голубым пятном, и квады закрывали кадр
 * чёрными блоками. Теперь: копия куска буфера рисования в область вне буфера и дисплея запоминается (Scap), примитив с
 * 15-битной текстурой целиком из этой области помечается (Draw.scap), а при записи кадра перед каждой такой ячейкой проход
 * прерывается, кусок НАШЕЙ цветной цели копируется в образ binding 2 (s_prevImg — в конце кадра он всё равно
 * перезаписывается), проход продолжается, и шейдер берёт цвет оттуда (prevMap.z = 2: прозрачность из VRAM не читается —
 * там чёрное софт-растра). В 16:9 ячейка ужимается по X вокруг своего центра на 3/4 — как мир у GTE, чтобы остаться
 * квадратной (DC_SCENE_CAPTURE_SQ=0 — не ужимать). DC_SCENE_CAPTURE=0 — как было. */
struct Scap { int sx, sy, dx, dy, w, h;
              int mode = 0;               /* 0 — ещё не решено, 1 — гладкая ячейка (наш квад + купол в шейдере), 2 — квады игры как есть */
              int bx = 0, by = 0;         /* страница текстуры примитивов ячейки (для гладкой: UV квада — от угла ячейки) */
              float Dx = 0.f, Dy = 0.f;   /* сдвиг центральной вершины, родные точки */
              float fx = 0.f, fy = 0.f;   /* 01.10 вечер: место ячейки для показа (сглаженное; без сглаживания = sx, sy) */
              int ax = 0, ay = 0;         /* ...начало области рисования кадра (буфер y 0 / 256): след ячейки — в координатах кадра */
              int sid = -1;               /* ...номер копии в потоке кадра (порядок ячеек у игры постоянный) */
              int ab = 0;                 /* ...0 — во всех показах, 1 — только кадр A, 2 — только кадр B */
              float uDx = 0.f, uDy = 0.f; /* ...сдвиг центра, с которым нарисован квад (журнал DC_SCENE_CAPTURE_LOG) */ };
static std::vector<Scap> s_scaps;      /* захваты этого кадра, по порядку потока */
static int s_scapNow = 0;              /* индекс+1 для ближайших dcvk_push_tri_raw */
static int s_scapTris = 0;
static uint64_t s_prim_frame_seq = 1;  /* 30.09: номер кадра растра (dcvk_frame_reset) */
static uint64_t s_scapSeenSeq = 0;     /* 30.09: кадр последнего принятого захвата (эффект идёт) */
static int s_scapStream = 0;           /* 01.10 вечер: копий-кандидатов в этом кадре (и отвергнутых) — номер ячейки в потоке */

/* ОБЛАСТЬ РИСОВАНИЯ ИГРЫ (15.09.2026, игрок: «доктор в брифинге не обрезан рамкой окна»).
 * Зеркало софт-растра сообщает область текущего примитива (dcvk_set_prim_clip), вершины
 * потом могут быть сжаты под 16:9 — клип сжимается так же и уходит в Draw; при записи
 * кадра он становится scissor. Полная область (весь экран) клипом не считается — такие
 * draw сливаются как раньше. DC_DRAWAREA_CLIP=0 — прежнее поведение (без обрезки). */
static int   s_pclipOn = 0;
static int   s_pclip[4] = { 0, 0, 1023, 511 };
static int   s_clipNow = 0;               /* клип для ближайших dcvk_push_tri_raw */
static float s_clipRect[4] = { 0, 0, 0, 0 };
/* ⭐ 21.09.2026 вечер — СЖАТИЕ ИНТЕРФЕЙСА РЕШАЕТСЯ ПРИ ЗАПИСИ КАДРА (игрок, слот 8, режим «16:9, интерфейс 4:3»: «интерфейс на
 * один кадр показывается растянутым и только после этого становится 4 на 3, и когда выходишь из инвентаря, игра сначала в
 * 4 на 3 и только на следующий кадр уже 16 на 9... чтобы сразу»). Сжимать примитив интерфейса или нет, решалось при его
 * ПОСТАНОВКЕ по прошлому кадру («в прошлом кадре не было 3D», dc_flat_screen_hint): на первом кадре после смены экрана
 * догадка неверна — интерфейс поверх мира уходил несжатым, а первый кадр плоского экрана сжимался дважды (примитивы + кадр
 * 4:3). Теперь кандидат запоминается с обоими вариантами X, а в dcvk_render, когда про кадр известно всё (есть ли 3D,
 * какой экран), вершины и клип приводятся к решению ЭТОГО кадра — тому же, по которому вывод выбирает 4:3 или 16:9
 * (dcvk_last_flat_final). DC_ASPECT_NOLAG: бит 2 — это правило, бит 1 — яркость текущего кадра (main.cpp); 0 — как было. */
static int   s_sqzNow = 0;                /* кандидат сжатия для ближайших dcvk_push_tri_raw: 1 — сжат, 2 — как есть */
static float s_sqzC = 0.f, s_clipRawX0 = 0.f, s_clipRawX1 = 0.f;
struct SqzCand { int vi; float altx[3]; int sq_now; int font; };   /* font — буква (27.09: dcvk_font_squeezed_last) */
static std::vector<SqzCand> s_sqzCands;
static int s_pb_frame = 0, s_lastPb = 0;  /* решение «кадр целиком 4:3» этого / последнего отданного кадра */
/* ⭐ 21.09.2026 ночь — ЗАГАДКА НА МОНИТОРЕ: КАДР ЦЕЛИКОМ 4:3 (игрок, слот 8, «Will you operate the circuit panel? — Yes»: «не все
 * элементы на нужных местах; рамки — это монитор, он 4 на 3, значит и сама загадка должна быть в 4 на 3; так со всеми подобными
 * загадками — краны с контейнерами, буквы, компьютеры»). Замер: загадку рисует код оверлея зоны (0x800F4xxx, интерпретатор) на
 * ЭКРАНЕ МИРА (байт экрана 3, режим 1) поверх мира — байта-признака нет; рамка монитора — кольцо спрайтов 16x16 по краю области
 * рисования (124 шт., CLUT 7F3F). Кадр был «мир 16:9»: интерфейс сжат к центру, 3D-панели ужаты GTE под 16:9, фон во всю ширину,
 * а то, что сжатие обходит (полупрозрачное, в полный экран), — на родных местах. Вход в загадку — прямая смена кадра, без
 * чёрного, поэтому решение — по СОДЕРЖИМОМУ КАДРА при его записи: есть кольцо рамки со всех четырёх сторон — кадр целиком 4:3,
 * как у консоли: кандидаты сжатия интерфейса -> как есть, 3D, которое GTE ужал (x' = OFX + (x - OFX)·3/4), -> вторым запомненным
 * X обратно (M43Cand), вывод кладёт кадр 4:3 (в режиме «16:9» тоже — dcvk_last_mon_final). Кадр B такой кадр не перепроецирует.
 * DC_MONITOR_43=0 — как было; DC_MONITOR_43_TILES — сколько треугольников рамки нужно (48). */
static int dc_mon43_env(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_MONITOR_43"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int dc_mon43_tiles_min(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_MONITOR_43_TILES"); v = e ? atoi(e) : 48; if (v < 8) v = 8; }
    return v;
}
/* 28.09 (2) (игрок, слот 0 — загадка крана с контейнерами: «сделай те же исправления для загадки с монитором: 4 на 3»). Рамка
 * этого монитора — не плитки 16x16, а спрайты 0x64 CLUT 7C79: 32x32 сверху и снизу, 32x31 по бокам (x = −1 и 288), углы 64x32
 * (кольцо GP0 skipq/pz_gp0.py) — строка лога была «border tiles … 0 triangles». Такие спрайты у края области тоже считаются
 * плитками рамки; порог и «все четыре стороны» — те же. DC_MONITOR_43_WIDE=0 — как было. */
static int dc_mon43_wide(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_MONITOR_43_WIDE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
struct M43Cand { int vi; float altx[3]; };
struct M43Tile { int vi, side; };           /* side: 1 верх, 2 низ, 4 лево, 8 право */
static std::vector<M43Cand> s_m43Cands;
static std::vector<M43Tile> s_monTiles;
static int s_mon_frame = 0, s_lastMon = 0;
static int s_mon_ring = 0, s_lastMonRing = 0;   /* 01.10 вечер: кольцо рамки монитора в кадре — в любом режиме соотношения */
static int dc_monitor_stream_on(void) {        /* DC_MONITOR_STREAM=0 — загадка на мониторе как было (порядок групп, глубина соседей) */
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_MONITOR_STREAM"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* gpu_sw_renderer.c: прошлый кадр — загадка на мониторе (рамка найдена): полупрозрачному без своей глубины соседскую не давать */
extern "C" int dcvk_last_mon_ring(void) { return dc_monitor_stream_on() && s_lastMonRing; }
static int dc_aspect_nolag(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_ASPECT_NOLAG"); v = e ? atoi(e) : 3; if (v < 0) v = 0; }
    return v;
}
static int dc_aspdbg(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_ASPDBG"); v = (e && *e == '1') ? 1 : 0; }
    return v;
}
extern "C" void dcvk_set_prim_clip(int on, int x1, int y1, int x2, int y2) {
    s_pclipOn = on; s_pclip[0] = x1; s_pclip[1] = y1; s_pclip[2] = x2; s_pclip[3] = y2;
}
static int dc_drawarea_clip_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_DRAWAREA_CLIP"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}

// ------------------------------------------------------------- helpers ----
/* 24.10.2026: строка vram: журнала (и релиза) - сколько видеопамяти занято после запуска и после смены целей (MSAA, разрешение):
 * у встроенной видеокарты своей памяти бывает 512 МБ, сверх неё всё встаёт (pt_vulkan.cpp ptvk_vram_usage) */
extern "C" int ptvk_vram_usage(double* used_mb, double* budget_mb, double* heap_mb);
static void vram_note(const char* when) {
    double u = 0.0, b = 0.0, hs = 0.0;
    if (ptvk_vram_usage(&u, &b, &hs))
        std::fprintf(stderr, "vram: %s - %.0f MB in use, the system gives %.0f MB (the card's own memory %.0f MB)%c", when, u, b, hs, 10);
    else
        std::fprintf(stderr, "vram: %s - use not known (no VK_EXT_memory_budget), the card's own memory %.0f MB%c", when, hs, 10);
}
static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(C->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return ~0u;
}

static bool makeBuf(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags props,
                    VkBuffer* buf, VkDeviceMemory* mem, void** map) {
    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(C->device, &bci, nullptr, buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(C->device, *buf, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType(mr.memoryTypeBits, props);
    if (mai.memoryTypeIndex == ~0u ||
        vkAllocateMemory(C->device, &mai, nullptr, mem) != VK_SUCCESS) return false;
    vkBindBufferMemory(C->device, *buf, *mem, 0);
    if (map) vkMapMemory(C->device, *mem, 0, mr.size, 0, map);
    return true;
}

/* ⭐ 06.10.2026 — ПОТОК ВЕРШИН В КЭШИРУЕМОЙ ПАМЯТИ. Процессор читает поток обратно: перенос комнаты в кадр без неё
 * (keepV/keepR в dcvk_render), кандидаты загадки на мониторе 4:3 (m43reg в dcvk_push_tri — на каждом треугольнике мира),
 * подъём декалей, свет «светится», хеш статичных экранов. Память HOST_VISIBLE|HOST_COHERENT без HOST_CACHED у драйвера —
 * write-combined: каждое чтение идёт мимо кэша процессора. Замер профилем 06.10 (слот 44): перенос комнаты 10 % и
 * m43reg 12 % всей работы главного потока. Данные и порядок те же — меняется только тип памяти; нет такого типа —
 * прежний. DC_VSTREAM_CACHED=0 — как было. */
static int vstream_cached_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_VSTREAM_CACHED"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static bool makeBufStream(VkDeviceSize size, VkBuffer* buf, VkDeviceMemory* mem, void** map, const char* what) {
    const VkMemoryPropertyFlags plain = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (vstream_cached_on()) {
        if (makeBuf(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, plain | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                    buf, mem, map) && *map) {
            rlog("[VKR] %s: cached host memory (HOST_CACHED)\n", what);
            return true;
        }
        if (*buf) { vkDestroyBuffer(C->device, *buf, nullptr); *buf = VK_NULL_HANDLE; }
        if (*mem) { vkFreeMemory(C->device, *mem, nullptr); *mem = VK_NULL_HANDLE; }
        *map = nullptr;
        rlog("[VKR] %s: no cached memory - as before\n", what);
    }
    return makeBuf(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, plain, buf, mem, map);   /* 25.10: и источник копии (s_wsVb) */
}

extern "C" const unsigned char *dc_embedded(const char *name, unsigned *size);

/* The shaders are built into the exe (dc_embedded.c). A file shaders/<name>
 * in the data folder the game works from (dc_boot.c) is still accepted, so a
 * shader can be swapped without a rebuild; the release does not ship it. */
static VkShaderModule makeModule(const void* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes; ci.pCode = (const uint32_t*)code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(C->device, &ci, nullptr, &m);
    return m;
}

static VkShaderModule loadSpv(const char* name) {
    unsigned nb = 0;
    const unsigned char* blob = dc_embedded(name, &nb);
    if (blob && nb) return makeModule(blob, nb);
    char path[256];
    snprintf(path, sizeof path, "shaders/%s", name);
    FILE* f = fopen(path, "rb");
    if (!f) { rlog("[VKR] missing %s\n", path); return VK_NULL_HANDLE; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<char> data(n); fread(data.data(), 1, n, f); fclose(f);
    return makeModule(data.data(), (size_t)n);
}

static void cmdBegin() {
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cmd, &bi);
}

// ------------------------------------------------- hi-res font atlas ----
/* Атлас шрифта игры с плотностью S текселей на родной тексель (DC_FONT_HIRES,
 * по умолчанию 4; 0 — выключить): RGBA8 (256·S)², биндинг 1 фрагментного
 * шейдера. Содержимое кладёт gpu.c через dcvk_font_hires_set() после сборки
 * атласа из TTF; до этого в образе заглушка b=255 («ячейка не наша»), и шейдер
 * идёт обычным путём через VRAM. Примитивы со страницы шрифта (768,0; 4bpp)
 * получают флаг F_HIFONT (64) в dcvk_push_tri. Просьба игрока 2026-09-02:
 * «текст в низком разрешении кривой — увеличь разрешение текста в 2 раза». */
extern "C" int dc_font_hires_scale(void);
static VkImage        s_fontImg = VK_NULL_HANDLE;
static VkDeviceMemory s_fontMem = VK_NULL_HANDLE;
static VkImageView    s_fontView = VK_NULL_HANDLE;
static VkSampler      s_fontSamp = VK_NULL_HANDLE;
static VkBuffer       s_fontStage = VK_NULL_HANDLE;
static VkDeviceMemory s_fontStageMem = VK_NULL_HANDLE;
static void*          s_fontStageMap = nullptr;
static int            s_fontW = 0, s_fontH = 0;
static int            s_fontPending = 0;     /* staging -> image на ближайшем кадре */
static int            s_fontOnGpu = 0;       /* образ уже переводился в SHADER_READ_ONLY */
static int            s_fontReady = 0;       /* настоящий атлас загружен: ставим F_HIFONT */
static unsigned char* s_fontEarly = nullptr; /* атлас пришёл до инициализации растеризатора */
static int            s_fontEarlyW = 0, s_fontEarlyH = 0;

static bool fontHiresInit() {
    int S = dc_font_hires_scale();
    /* две страницы по вертикали: верх — страница шрифта (768,0), низ —
     * страница титульных полосок (704,0), выбирается флагом F_HIFONT2 */
    s_fontW = (S > 1) ? 256 * S : 4;             /* выключено: маленькая заглушка */
    s_fontH = (S > 1) ? 512 * S : 4;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { (uint32_t)s_fontW, (uint32_t)s_fontH, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(C->device, &ici, nullptr, &s_fontImg) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(C->device, s_fontImg, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u ||
        vkAllocateMemory(C->device, &mai, nullptr, &s_fontMem) != VK_SUCCESS) return false;
    vkBindImageMemory(C->device, s_fontImg, s_fontMem, 0);
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = s_fontImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(C->device, &vci, nullptr, &s_fontView) != VK_SUCCESS) return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.f;
    if (vkCreateSampler(C->device, &sci, nullptr, &s_fontSamp) != VK_SUCCESS) return false;
    if (!makeBuf((VkDeviceSize)s_fontW * s_fontH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &s_fontStage, &s_fontStageMem, &s_fontStageMap)) return false;
    uint8_t* p = (uint8_t*)s_fontStageMap;
    for (size_t i = 0; i < (size_t)s_fontW * s_fontH; i++) {   /* заглушка: не наша ячейка */
        p[i * 4 + 0] = 0; p[i * 4 + 1] = 0; p[i * 4 + 2] = 255; p[i * 4 + 3] = 0;
    }
    if (s_fontEarly && s_fontEarlyW == s_fontW && s_fontEarlyH == s_fontW) {
        memcpy(s_fontStageMap, s_fontEarly, (size_t)s_fontW * s_fontW * 4);   /* верхняя страница */
        s_fontReady = 1;
    }
    free(s_fontEarly); s_fontEarly = nullptr;
    s_fontPending = 1;
    rlog("[VKR] hi-res font image %dx%d (scale %d)\n", s_fontW, s_fontH, S);
    return true;
}

extern "C" void dcvk_font_hires_set(const unsigned char* rgba, int w, int h) {
    if (!s_fontStageMap) {                     /* растеризатор ещё не поднят — придержим */
        free(s_fontEarly);
        s_fontEarly = (unsigned char*)malloc((size_t)w * h * 4);
        if (s_fontEarly) { memcpy(s_fontEarly, rgba, (size_t)w * h * 4); s_fontEarlyW = w; s_fontEarlyH = h; }
        return;
    }
    if (w != s_fontW || h != s_fontW) {
        rlog("[VKR] font hires: %dx%d does not match the %dx%d page\n", w, h, s_fontW, s_fontW);
        return;
    }
    memcpy(s_fontStageMap, rgba, (size_t)w * h * 4);      /* верхняя страница образа */
    s_fontPending = 1;
    s_fontReady = 1;
}

/* Прямоугольник hi-res атласа в текселях страницы: page 0 — страница шрифта
 * (768,0), page 1 — страница титульных полосок (704,0), вторая половина
 * образа. rgba — (w·S)×(h·S), кодировка как у атласа. */
extern "C" void dcvk_font_hires_rect_set(int page, int x, int y, int w, int h, const unsigned char* rgba) {
    if (!s_fontStageMap || s_fontW < 8 || !rgba) return;
    const int S = s_fontW / 256;
    const int X = x * S, Y = (page * 256 + y) * S, Wp = w * S, Hp = h * S;
    if (X < 0 || Y < 0 || Wp <= 0 || Hp <= 0 || X + Wp > s_fontW || Y + Hp > s_fontH) return;
    uint8_t* dst = (uint8_t*)s_fontStageMap;
    for (int r = 0; r < Hp; r++)
        memcpy(dst + ((size_t)(Y + r) * (size_t)s_fontW + (size_t)X) * 4,
               rgba + (size_t)r * (size_t)Wp * 4, (size_t)Wp * 4);
    s_fontPending = 1;
}
extern "C" void dcvk_font_hires_rect_clear(int page, int x, int y, int w, int h) {
    if (!s_fontStageMap || s_fontW < 8) return;
    const int S = s_fontW / 256;
    const int X = x * S, Y = (page * 256 + y) * S, Wp = w * S, Hp = h * S;
    if (X < 0 || Y < 0 || Wp <= 0 || Hp <= 0 || X + Wp > s_fontW || Y + Hp > s_fontH) return;
    uint8_t* dst = (uint8_t*)s_fontStageMap;
    for (int r = 0; r < Hp; r++) {
        uint8_t* p = dst + ((size_t)(Y + r) * (size_t)s_fontW + (size_t)X) * 4;
        for (int c = 0; c < Wp; c++) { p[c * 4 + 0] = 0; p[c * 4 + 1] = 0; p[c * 4 + 2] = 255; p[c * 4 + 3] = 0; }
    }
    s_fontPending = 1;
}

/* В командный буфер кадра, до render pass: staging -> образ. */
static void fontHiresUpload(VkCommandBuffer cmd) {
    if (!s_fontPending || !s_fontImg) return;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = s_fontOnGpu ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = s_fontOnGpu ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.image = s_fontImg;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmd, s_fontOnGpu ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy bic{};
    bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bic.imageExtent = { (uint32_t)s_fontW, (uint32_t)s_fontH, 1 };
    vkCmdCopyBufferToImage(cmd, s_fontStage, s_fontImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier r = b;
    r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    r.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    r.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &r);
    s_fontPending = 0; s_fontOnGpu = 1;
}
// ===================================================================================================================
// 09.10.2026 — КАДР В DIRECT3D 11 БЕЗ ЧТЕНИЯ В ПАМЯТЬ ПРОЦЕССОРА (DC_VK_SHARED). Игрок: «максимально оптимизировать».
// Замер DC_VK_STAGES: чтение кадра 1600x1200 в кэшируемую память процессора стоило 1.6–1.75 мс GPU на каждом кадре (A и B),
// потом перестановка R/B и заливка в текстуру D3D11 — ещё ~0.5 мс процессора. Теперь кадр блитом RGBA -> BGRA уходит в
// общую с D3D11 текстуру (её создаёт main.cpp, сюда приходит NT-дескриптор); доступ делит её мьютекс (ключ 0 у обеих
// сторон). Процессору читаются только строки, по которым main.cpp меряет яркость кадра (dc_frame_peak, dc_blackwatch:
// строки h/16 + 17k целиком). Дампы кадра (dcvk_dump_last*, кольцо) дочитывают его целиком по требованию.
// ===================================================================================================================
static VkImage        s_dxImg = VK_NULL_HANDLE;
static VkDeviceMemory s_dxMem = VK_NULL_HANDLE;
static int            s_dxW = 0, s_dxH = 0;
static int            s_dxUse = 0;        /* main.cpp: этот кадр — в общую текстуру (dcvk_shared_use) */
static int            s_kmPending = 0;    /* в буфере команд есть запись в общую текстуру — отправка с её мьютексом */
static int            s_lastShared = 0;   /* последний кадр ушёл в общую текстуру: в s_rbMap только строки яркости */
static int            s_dxMutex = 0;      /* у общей текстуры есть мьютекс (иначе порядок держит main.cpp: забор + метка D3D11) */

static void dx_release(void) {
    if (s_dxImg) { vkDestroyImage(C->device, s_dxImg, nullptr); s_dxImg = VK_NULL_HANDLE; }
    if (s_dxMem) { vkFreeMemory(C->device, s_dxMem, nullptr); s_dxMem = VK_NULL_HANDLE; }
    s_dxW = s_dxH = 0;
}
/* кадр w x h из s_colImg (в TRANSFER_SRC_OPTIMAL): блит в общую текстуру + строки яркости в s_rbBuf */
static bool dx_record(int w, int h) {
    if (!s_dxUse || !s_dxImg || w != s_dxW || h != s_dxH) return false;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;                    /* кадр перезаписывается целиком */
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.image = s_dxImg;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkImageBlit bl{};
    bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bl.srcOffsets[1] = { w, h, 1 };
    bl.dstOffsets[1] = { w, h, 1 };
    vkCmdBlitImage(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_dxImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_NEAREST);
    VkImageMemoryBarrier r = b;
    r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; r.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; r.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &r);
    /* строки яркости: те же, что читают dc_frame_peak и dc_blackwatch (main.cpp) */
    VkBufferImageCopy rows[128];
    int nr = 0;
    for (int y = h / 16; y < h - h / 16 && nr < 128; y += 17) {
        VkBufferImageCopy& c = rows[nr++];
        c = VkBufferImageCopy{};
        c.bufferOffset = (VkDeviceSize)y * (VkDeviceSize)w * 4u;
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageOffset = { 0, y, 0 };
        c.imageExtent = { (uint32_t)w, 1, 1 };
    }
    if (nr) vkCmdCopyImageToBuffer(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rbBuf, (uint32_t)nr, rows);
    s_kmPending = 1;
    return true;
}
/* 24.10.2026: время отправки и ожидания забора за развёртку - для строки ptrace: (main.cpp, DC_PACE_TRACE); main.cpp обнуляет */
extern "C" { double g_vk_submit_ms = 0.0, g_vk_wait_ms = 0.0; }
/* 25.10.2026 вечер: последняя отправка - разбивка кадра между тактами (main.cpp pt_extra, DC_HZ_DBG): запись команд, отправка,
 * ожидание забора, видеокарта по меткам (основной проход, после него) */
extern "C" { double g_vk_last_submit_ms = 0.0, g_vk_last_wait_ms = 0.0, g_vk_last_rec_ms = 0.0, g_vk_last_gpu_ms[2] = { 0.0, 0.0 }; }
static LARGE_INTEGER s_vk_last_tq0;
/* 23.10.2026 (dev only): DC_TEST_GPU_SLOW_MS=N - every picture of the game (frames A, B and the extra ones) waits N ms more,
 * as if the graphics card were slower: the pacing catch-up (main.cpp pace_catchup_skip) is tested on a fast machine.
 * 25.10.2026 (dev only): port test_slow {"ms":M,"count":N} - the next N pictures wait M ms more: a burst of long frames, does the
 * pacing come back after it (main.cpp pace_med3, the probe frame B). 25.10.2026 late: "between":1 - only the frames between ticks
 * (main.cpp hz_est_use, the probe frame between ticks). */
static std::atomic<int> s_test_slow_n(0);
static double s_test_slow_ms = 0.0;
static int s_test_slow_between = 0;
extern "C" void dcvk_test_slow(double ms, int n, int between) { s_test_slow_ms = ms; s_test_slow_between = between; s_test_slow_n = n; }
static void test_gpu_slow(int extra) {
#ifndef PSX_NO_DEBUG_TOOLS
    static double ms = -1.0;
    if (ms < 0.0) { const char* e = getenv("DC_TEST_GPU_SLOW_MS"); ms = e ? atof(e) : 0.0; if (ms < 0.0) ms = 0.0;
                    if (ms > 0.0) std::fprintf(stderr, "test: every picture waits %.1f ms more (DC_TEST_GPU_SLOW_MS)%c", ms, 10); }
    double w = ms;
    if ((extra || !s_test_slow_between) && s_test_slow_n.load() > 0 && s_test_slow_n.fetch_sub(1) > 0) w += s_test_slow_ms;
    if (w <= 0.0) return;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    const LONGLONG n = (LONGLONG)(w * (double)f.QuadPart / 1000.0);
    do { QueryPerformanceCounter(&t); } while (t.QuadPart - t0.QuadPart < n);
#endif
}
static bool cmdSubmitWait() {
    vkEndCommandBuffer(s_cmd);
    vkResetFences(C->device, 1, &s_fence);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    /* 09.10: запись в общую с D3D11 текстуру — под её мьютексом (ключ 0 и на взятии, и на отдаче) */
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{ VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR };
    const uint64_t key0 = 0; const uint32_t kto = 0xFFFFFFFFu;
    if (s_kmPending && s_dxMem && s_dxMutex) {
        km.acquireCount = 1; km.pAcquireSyncs = &s_dxMem; km.pAcquireKeys = &key0; km.pAcquireTimeouts = &kto;
        km.releaseCount = 1; km.pReleaseSyncs = &s_dxMem; km.pReleaseKeys = &key0;
        si.pNext = &km;
    }
    s_kmPending = 0;
    LARGE_INTEGER tq0, tq1, tq2; QueryPerformanceCounter(&tq0);
    const VkResult sr = vkQueueSubmit(C->queue, 1, &si, s_fence);
    if (sr != VK_SUCCESS) { if (sr == VK_ERROR_DEVICE_LOST) ptvk_mark_lost((int)sr); return false; }
    QueryPerformanceCounter(&tq1);
    /* 18.10.2026: до конца, не 4 с - буфер команд незаконченной работы снова не пишется (ptvk_wait_fence, pt_vulkan.cpp) */
    const int ok = ptvk_wait_fence(s_fence) != 0;
    QueryPerformanceCounter(&tq2);
    { static double qf = 0.0; if (qf == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); qf = (double)f.QuadPart / 1000.0; }
      g_vk_submit_ms += (double)(tq1.QuadPart - tq0.QuadPart) / qf;
      g_vk_wait_ms += (double)(tq2.QuadPart - tq1.QuadPart) / qf;
      g_vk_last_submit_ms = (double)(tq1.QuadPart - tq0.QuadPart) / qf;
      g_vk_last_wait_ms = (double)(tq2.QuadPart - tq1.QuadPart) / qf;
      s_vk_last_tq0 = tq0; }
    return ok;
}

// ===================================================================================================================
// 25.09.2026 — ЗАТЕНЕНИЕ УГЛОВ (SSAO) И СГЛАЖИВАНИЕ КРАЁВ (MSAA). Игрок: «сделай 2 (ssao) и 6 из списка. все эти новые
// изменения сделай переключаемыми через дебаг». Обе — надстройка над готовым потоком: порядок и состав отрисовки,
// прозрачность, статика и динамика комнаты не меняются.
//  * MSAA: кадр рисуется в цели на N выборок (цвет + глубина) и в конце прохода сводится (resolve) в прежнюю s_colImg —
//    копии кадра, readback и вывод видят обычную картинку. Сглаживаются края полигонов, текстуры внутри полигона не
//    размываются. Галка «по выборкам» — фрагментная ступень на каждую выборку (dc_raster_ss.frag.spv): сглажены и края
//    прозрачных текселей (решётки, сетки, буквы), шейдер работает в N раз больше.
//  * SSAO: после прохода кадра три полноэкранных прохода — линейная глубина + маска (dc_ssao_z.frag, R32F; глубина
//    MSAA читается здесь один раз), сырое затенение в половинном разрешении (dc_ssao.frag, RG32F) и размытие 4x4
//    половинной сетки с учётом глубины + умножение цветной цели (dc_ssao_comp.frag). Где затенять — маска в альфе цветной
//    цели: непрозрачные draw с глубиной пишут 1, плоские (интерфейс, текст, экраны) — 0, полупрозрачные альфу не трогают
//    (смешение альфы ZERO/ONE). Проход умножения возвращает альфу в 1 — вывод видит непрозрачный кадр, как раньше.
//  Настройки: env (DC_MSAA, DC_MSAA_SSHADE, DC_SSAO, DC_SSAO_*), поверх — dc_post_user.cfg (пишет панель F8);
//  DC_POST_NOCFG=1 — файл не читать (замеры). Откат: DC_MSAA=1, DC_SSAO=0. DC_POST_DBG=1 — время GPU в журнал.
// ===================================================================================================================
enum { PP_RADIUS = 0, PP_STRENGTH, PP_BIAS, PP_SAMPLES, PP_MAXPX, PP_BLUR,
       PP_FL_ON, PP_FL_SSTR, PP_FL_SRAD, PP_FL_SMS, PP_FL_BSTR, PP_FL_BRAD, PP_FL_BMS, PP_FL_WRAP, PP_FL_SYNC,
       PP_GL_ON, PP_GL_STR, PP_GL_WIDE, PP_GL_RAD, PP_GL_ADD, PP_GL_ATHR, PP_GL_TEX, PP_GL_THR,          /* 25.09 вечер: свечение */
       PP_GL_MARK,                                                         /* 25.09 ночь: отмеченные полупрозрачные (лазеры) */
       PP_GL_SELF,                                                         /* 25.09 ночь (3): ореол на самой светящейся текстуре */
       PP_GL_LON, PP_GL_LSTR, PP_GL_LRAD,                                  /* 25.09 поздно: свет от светящихся текстур */
       PP_GL_LHUE,                                                         /* 25.09 ночь (4): прозрачные — прежней силы */
       PP_GL_LSTEADY, PP_GL_LFLICK, PP_GL_LHOLD,                           /* 27.09: ровный свет мигающих ламп */
       PP_GL_LCHR,                                                         /* 28.09: на персонажах — без граней */
       PP_RF_ON, PP_RF_TOL, PP_RF_ALL,                                                                /* ...и отражения */
       PP_RF_T_STR, PP_RF_T_BLUR, PP_RF_M_STR, PP_RF_M_BLUR, PP_RF_M_TINT, PP_RF_G_STR, PP_RF_G_BLUR,  /* 25.09 ночь: материалы */
       PP_SP_ON, PP_SP_T_STR, PP_SP_T_SHIN, PP_SP_M_STR, PP_SP_M_SHIN, PP_SP_G_STR, PP_SP_G_SHIN, PP_SP_GL, /* ...и их блики */
       PP_SP_ENV_T, PP_SP_ENV_M, PP_SP_ENV_G,                                                          /* ...и отблеск окружения */
       PP_SP_OLD,                                                          /* 25.09 ночь (3): прежняя формула (прибавка) */
       PP_SP_SOFT,                                                         /* 27.09: металл светлеет только от отражённого света */
       PP_SP_MROUGH, PP_SP_MMATTE, PP_SP_MBUMP, PP_SP_MRUST, PP_SP_MGRAIN, /* 27.09 вечер: шероховатый металл */
       PP_VL_ON, PP_VL_STR, PP_VL_GLOW, PP_VL_RAD, PP_VL_G, PP_VL_NOISE, PP_VL_STEPS, PP_VL_SH,        /* 27.09: объёмный свет */
       PP_VL_DUST, PP_VL_DUSTN, PP_VL_DUSTSZ, PP_VL_DBG,                                               /* ...и пыль в лучах */
       PP_FX_DBG, PP_COUNT };
/* grp: 0 — затенение углов (SSAO), 1 — вспышки выстрелов и взрывов как свет (25.09, pt_renderer.c dc_flash_*) */
struct PostParDef { const char* key; const char* env; float def, lo, hi; const char* name; const char* hint; const char* fmt; int grp; };
static const PostParDef s_ppDef[PP_COUNT] = {
    { "ao_radius",   "DC_SSAO_RADIUS",  600.f,  50.f,  1500.f, "радиус",   "размер затенения в единицах комнаты (рост Регины ~1900)", "%.0f ед." },
    { "ao_strength", "DC_SSAO_STR",     1.2f,   0.f,   3.0f,   "сила",     "насколько темнеют углы (0 — никак)", "%.2f" },
    { "ao_bias",     "DC_SSAO_BIAS",    6.f,    0.f,   60.f,   "смещение", "допуск глубины: больше — меньше пятен на ровном, но и углы слабее", "%.0f ед." },
    { "ao_samples",  "DC_SSAO_SAMPLES", 16.f,   4.f,   32.f,   "выборок",  "выборок на точку (узор 4x4 умножает их ещё на 16); больше — чище и дороже", "%.0f" },
    { "ao_maxpx",    "DC_SSAO_MAXPX",   0.25f,  0.02f, 0.40f,  "предел",   "наибольший радиус на экране, доля высоты кадра — вблизи камеры", "%.2f кадра" },
    { "ao_blur",     "DC_SSAO_BLUR",    1.f,    0.f,   1.f,    "размытие узора", "сгладить узор выборок (выкл — сырое затенение, для отладки)", "%.0f" },
    { "fl_on",       "DC_FLASH",          1.f,    0.f,    1.f,     "вспышки как свет", "выстрел и взрыв гранаты на миг освещают стены, пол и Регину", "%.0f", 1 },
    { "fl_shot_str", "DC_FLASH_SHOT_STR", 1.6f,   0.f,    4.f,     "выстрел: сила",   "насколько ярко выстрел освещает всё вокруг (дробовик x1.4, дротик x0.45)", "%.2f", 1 },
    { "fl_shot_rad", "DC_FLASH_SHOT_RAD", 2250.f, 500.f,  10000.f,   /* 25.09 вечер (игрок): 4500 -> 2250, «чтобы не било по глазам» */ "выстрел: радиус", "докуда достаёт свет выстрела, единиц комнаты (рост Регины ~1900)", "%.0f ед.", 1 },
    { "fl_shot_ms",  "DC_FLASH_SHOT_MS",  70.f,   16.f,   300.f,   "выстрел: длится", "сколько горит вспышка выстрела (кадр игры — 33 мс)", "%.0f мс", 1 },
    { "fl_boom_str", "DC_FLASH_BOOM_STR", 2.2f,   0.f,    6.f,     "взрыв: сила",     "насколько ярко взрыв гранаты освещает всё вокруг", "%.2f", 1 },
    { "fl_boom_rad", "DC_FLASH_BOOM_RAD", 7000.f, 1000.f, 16000.f, "взрыв: радиус",   "докуда достаёт свет взрыва, единиц комнаты", "%.0f ед.", 1 },
    { "fl_boom_ms",  "DC_FLASH_BOOM_MS",  400.f,  50.f,   1500.f,  "взрыв: длится",   "сколько горит взрыв (с мерцанием)", "%.0f мс", 1 },
    { "fl_wrap",     "DC_FLASH_WRAP",     0.3f,   0.f,    1.f,     "обтекание",       "сколько света заходит на грани, отвёрнутые от вспышки (0 — строго по нормали, 1 — со всех сторон)", "%.2f", 1 },
    { "fl_sync",     "DC_FLASH_SYNC",     1.f,    0.f,    1.f,     "вместе со вспышкой игры", "свет загорается, когда игра рисует звезду у ствола (~0.1 с после ухода патрона); выкл — сразу при выстреле", "%.0f", 1 },
    /* 25.09 вечер: группа 2 — свечение самих источников и отражения на полу (см. «СВЕЧЕНИЕ ИСТОЧНИКОВ И ОТРАЖЕНИЯ» ниже) */
    /* 25.09 ночь (4), игрок: «убери вообще эмиссив со всего — слишком яркий; оставь именно свет от помеченных текстур»:
     * ореол (весь проход свечения) выключен по умолчанию; свет от текстур «светится» (glow_light) от него не зависит.
     * DC_GLOW=1 / галка F8 — ореол как был */
    { "glow",        "DC_GLOW",           0.f,    0.f,    1.f,     "свечение источников", "ореол только у самих источников: лазеры, ореолы ламп, вспышки и текстуры из списка «светится» (выбор в приложении); остальной кадр не трогается", "%.0f", 2 },
    { "glow_str",    "DC_GLOW_STR",       0.9f,   0.f,    4.f,     "свечение: сила",   "насколько ярок ореол вокруг источников", "%.2f", 2 },
    { "glow_wide",   "DC_GLOW_WIDE",      0.6f,   0.f,    3.f,     "свечение: дальний ореол", "сила широкого мягкого ореола (ближний — полной силой)", "%.2f", 2 },
    { "glow_rad",    "DC_GLOW_RAD",       1.0f,   0.3f,   3.f,     "свечение: размер", "во сколько раз шире ореол", "%.2f", 2 },
    { "glow_add",    "DC_GLOW_ADD",       1.0f,   0.f,    3.f,     "свечение: лазеры и ореолы", "вклад прозрачных светящихся примитивов игры (режим «сложение»: лазеры, ореолы ламп, вспышки)", "%.2f", 2 },
    { "glow_athr",   "DC_GLOW_ATHR",      0.55f,  0.f,    0.95f,   "свечение: порог лазеров и ореолов", "светится только ядро ярче порога: лазеры и ореолы ламп проходят, тусклые большие световые конусы — нет (0 — всё)", "%.2f", 2 },
    /* 25.09 ночь (игрок: «уменьши свечение текстур, помеченных светящимися, а сам свет от них не изменяй»): 1.0 -> 0.4.
     * 25.09 ночь (3) (игрок: «эмиссив уменьши в 3 раза, а свет от светящихся текстур верни как было — ты видимо его
     * уменьшил»): ореол вокруг — снова 1.0 (его и видно светом на стене), а на самой текстуре — доля glow_self */
    { "glow_tex",    "DC_GLOW_TEX",       1.0f,   0.f,    3.f,     "свечение: выбранные текстуры", "вклад текстур из списка «светится» (экраны, панели, плафоны) в ореол; свет от них — отдельно (glow_light_str)", "%.2f", 2 },
    { "glow_thr",    "DC_GLOW_THR",       0.2f,   0.f,    0.9f,    "свечение: порог",  "у выбранных текстур светятся только тексели ярче порога (0 — вся текстура)", "%.2f", 2 },
    /* 25.09 ночь (игрок, слот 13: «на текстуру лазеров свечение почему-то не действует»): прозрачное «сложение» из списка
     * «светится» — без порога яркости (слой лазера тусклый, 0.15) и со своим вкладом */
    { "glow_mark",   "DC_GLOW_MARK",      3.0f,   0.f,    8.f,     "свечение: отмеченные прозрачные", "вклад прозрачных текстур из списка «светится» (лазеры, огоньки): порога яркости у них нет — тусклый слой лазера тоже светится", "%.2f", 2 },
    /* 25.09 ночь (3): ореол ложится и на саму светящуюся текстуру — лампа выбеливалась, узор пропадал («эмиссив»). На ней
     * самой — только эта доля (метка в альфе прохода свечения -> G1 -> dc_glow_comp.frag); 0.13 = 0.4 / 3 от прежнего;
     * 1 — как было до 25.09 ночи (3) */
    { "glow_self",   "DC_GLOW_SELF",      0.13f,  0.f,    1.f,     "свечение: на самой текстуре", "какая доля ореола ложится на саму светящуюся текстуру (лампу, экран): меньше — не выбеливается, узор виден; вокруг ореол полной силой (1 — как вокруг)", "%.2f", 2 },
    /* 25.09 поздно (игрок: «от светящихся текстур тоже должен быть эффект — реальный свет, настраиваемый, того цвета, какого
     * текстура»): текстура «светится» — ещё и точечный свет её цвета (см. «СВЕТ ОТ СВЕТЯЩИХСЯ ТЕКСТУР») */
    { "glow_light",     "DC_GLOW_LIGHT",     1.f,    0.f, 1.f,     "свет от светящихся текстур", "текстуры «светится» освещают стены, пол и персонажей рядом своим цветом (до 8 источников в кадре)", "%.0f", 2 },
    { "glow_light_str", "DC_GLOW_LIGHT_STR", 0.6f,   0.f, 4.f,     "свет: сила",   "насколько ярко светящаяся текстура освещает всё вокруг (у плафонов свет уже запечён в стенах — много не надо)", "%.2f", 2 },
    { "glow_light_rad", "DC_GLOW_LIGHT_RAD", 2200.f, 300.f, 8000.f, "свет: радиус", "докуда достаёт её свет, единиц комнаты (рост Регины ~1900; большой экран светит дальше)", "%.0f ед.", 2 },
    /* 25.09 ночь (4), игрок: «свет от светящихся текстур верни как было (ты видимо его уменьшил)»: у прозрачных помеченных
     * (лазеры, огоньки) в ночь (2) свет стал тексель × вершина/128 — лазер (39,0,0) вместо прежних 248, в 6 раз слабее */
    { "glow_light_hue", "DC_GLOW_LIGHT_HUE", 1.f,    0.f, 1.f,     "свет прозрачных — прежней силы", "лазеры и огоньки из списка «светится» светят с прежней силой, цветом своей вершины (лазер — красным); выкл — тусклее, как 25.09 ночью (2)", "%.0f", 2 },
    /* 27.09 (игрок: «есть мигающие лампы и если я указываю что они светящиеся, свет сильно бьет по глазам. Можешь на
     * мигающих текстурах этот свет делать не мигающим почти?»): см. «РОВНЫЙ СВЕТ МИГАЮЩИХ ЛАМП» у gl_lights_build */
    { "glow_light_steady", "DC_GLOW_LIGHT_STEADY", 1.f,  0.f, 1.f,  "мигающие лампы светят ровно", "лампа, которую игра мигает (ореол через кадр, мигающая плашка, меняющаяся текстура), светит ровно: свет держится между вспышками; выкл — свет мигает вместе с лампой, как было", "%.0f", 2 },
    { "glow_light_flick",  "DC_GLOW_LIGHT_FLICK",  0.15f, 0.f, 1.f, "мигающие: доля мигания", "сколько мигания лампы остаётся в её свете: 0 — совсем ровный, 1 — мигает как лампа", "%.2f", 2 },
    { "glow_light_hold",   "DC_GLOW_LIGHT_HOLD",   1.0f, 0.1f, 3.f, "мигающие: держать свет", "сколько свет держится, пока лампы нет в кадре игры, а её место в кадре (между вспышками); лампа за краем кадра и после смены ракурса гаснет сразу", "%.1f с", 2 },
    /* 28.09 (игрок, слот 12: «от дымки света такое же затенение граней, как было от теней»): свет светящихся текстур (и вспышек)
     * брал нормаль из производных — плоскую нормаль полигона, и низкополигональные персонажи гранились; теперь у персонажа
     * средняя нормаль видимой стороны, как у луны 27.09 вечером (dc_raster.frag: знак glc.w / flc.w) */
    { "glow_light_chr",    "DC_GLOW_LIGHT_CHR_SMOOTH", 1.f, 0.f, 1.f, "на персонажах — без граней", "свет светящихся текстур и вспышек ложится на Регину и других персонажей ровно, без граней полигонов; выкл — по граням, как было", "%.0f", 2 },
    { "refl",        "DC_REFL",           1.f,    0.f,    1.f,     "отражения на полу", "пол, которому в приложении назначен материал (кафель, металл, глянец), отражает Регину, персонажей, лазеры и светящееся", "%.0f", 2 },
    { "refl_tol",    "DC_REFL_TOL",       60.f,   5.f,    400.f,   "допуск высоты", "отражает только пол на уровне ног (±, единиц комнаты): ступени и столы не зеркалят", "%.0f ед.", 2 },
    { "refl_all",    "DC_REFL_ALL",       0.f,    0.f,    1.f,     "любой пол как кафель (проверка)", "все горизонтальные поверхности на уровне ног отражают как кафель — посмотреть эффект без выбора материалов", "%.0f", 2 },
    /* 25.09 ночь (игрок: «сделай настройку понятнее и чтобы указывать мог что есть что»): материал пола назначается в
     * приложении tools\twosided_picker (dc_material_user.cfg), у каждого — своя сила и размытие отражения */
    { "refl_tile_str",   "DC_REFL_TILE_STR",   0.35f, 0.f, 1.f, "кафель: сила",     "насколько сильно отражает кафель / плитка", "%.2f", 2 },
    { "refl_tile_blur",  "DC_REFL_TILE_BLUR",  0.8f,  0.f, 8.f, "кафель: размытие", "размытость отражения в кафеле (0 — зеркало)", "%.1f", 2 },
    { "refl_metal_str",  "DC_REFL_METAL_STR",  0.45f, 0.f, 1.f, "металл: сила",     "насколько сильно отражает металлический пол", "%.2f", 2 },
    { "refl_metal_blur", "DC_REFL_METAL_BLUR", 3.0f,  0.f, 8.f, "металл: размытие", "металл отражает мутно (0 — зеркало)", "%.1f", 2 },
    { "refl_metal_tint", "DC_REFL_METAL_TINT", 0.6f,  0.f, 1.f, "металл: оттенок",  "отражение в металле окрашено цветом самого металла (0 — своими цветами)", "%.2f", 2 },
    { "refl_gloss_str",  "DC_REFL_GLOSS_STR",  0.7f,  0.f, 1.f, "глянец: сила",     "насколько сильно отражает глянцевый / мокрый пол", "%.2f", 2 },
    { "refl_gloss_blur", "DC_REFL_GLOSS_BLUR", 0.3f,  0.f, 8.f, "глянец: размытие", "глянец почти зеркальный (0 — зеркало)", "%.1f", 2 },
    /* 25.09 ночь (игрок, слот 13: «материал металла кажется не работает» — металлом отмечена кодовая панель на стене):
     * блики от ламп игры и от светящихся текстур на ЛЮБОЙ поверхности с материалом (dc_raster.frag «БЛИКИ МАТЕРИАЛОВ») */
    { "spec",            "DC_MAT_SPEC",        1.f,   0.f, 1.f,   "блики материалов", "поверхности с материалом — и стены, и предметы, и пол — получают блики от ламп игры и светящихся текстур; блик идёт вместе с камерой", "%.0f", 2 },
    { "spec_tile_str",   "DC_SPEC_TILE_STR",   0.25f, 0.f, 2.f,   "кафель: блик", "сила блика кафеля (узкий, неяркий)", "%.2f", 2 },
    { "spec_tile_shin",  "DC_SPEC_TILE_SHIN",  40.f,  2.f, 200.f, "кафель: узость блика", "больше — меньше и острее пятно блика", "%.0f", 2 },
    /* 27.09 (игрок: «металл допустим слишком светлым становится. Исправь материал»): блик металла был широким (8) и сильным
     * (0.8) — рама сетки у лампы ST10D белела целиком; теперь узкий и слабее (как было — 0.8 и 8) */
    { "spec_metal_str",  "DC_SPEC_METAL_STR",  0.5f,  0.f, 2.f,   "металл: блик", "сила блика металла (в цвет самого металла — оттенок как у отражения)", "%.2f", 2 },
    { "spec_metal_shin", "DC_SPEC_METAL_SHIN", 32.f,  2.f, 200.f, "металл: узость блика", "больше — меньше и острее пятно блика", "%.0f", 2 },
    { "spec_gloss_str",  "DC_SPEC_GLOSS_STR",  0.9f,  0.f, 2.f,   "глянец: блик", "сила блика глянца / мокрого (узкий, яркий)", "%.2f", 2 },
    { "spec_gloss_shin", "DC_SPEC_GLOSS_SHIN", 60.f,  2.f, 200.f, "глянец: узость блика", "больше — меньше и острее пятно блика", "%.0f", 2 },
    { "spec_glow",       "DC_SPEC_GLOW",       0.5f,  0.f, 2.f,   "блики от светящихся текстур", "доля бликов от света светящихся текстур (экраны, лампы, лазеры)", "%.2f", 2 },
    /* отблеск окружения: честный блик лампы на стене виден редко (лампы под потолком) — металл читается по отсвету,
     * который гуляет по нему при движении камеры: отражённый луч к потолку — светлее, под скользящим углом — сильнее */
    { "spec_env_tile",   "DC_SPEC_ENV_TILE",   0.12f, 0.f, 1.5f,  "кафель: отблеск окружения", "насколько кафель светлеет, когда отражает потолок (зависит от угла взгляда)", "%.2f", 2 },
    { "spec_env_metal",  "DC_SPEC_ENV_METAL",  0.8f,  0.f, 1.5f,  "металл: отблеск окружения", "насколько металл светлеет от отражённого окружения — своим цветом, сильнее, когда отражает потолок, и под скользящим углом; светлое почти не трогается — не выбеливается", "%.2f", 2 },
    { "spec_env_gloss",  "DC_SPEC_ENV_GLOSS",  0.35f, 0.f, 1.5f,  "глянец: отблеск окружения", "насколько глянец светлеет, когда отражает потолок", "%.2f", 2 },
    /* 25.09 ночь (3), игрок (слот 1, ST10C): «металл пересвечивает текстуры — сделай его более реалистичным и чтобы не
     * пересвечивал»: металл — множитель своего цвета, прибавки «экраном» (dc_raster.frag); 1 — прежняя прибавка */
    { "spec_old",        "DC_SPEC_OLD",        0.f,   0.f, 1.f,   "материалы: прежняя формула", "как было до 25.09 ночи (3): отблеск металла прибавкой (выбеливал), блики без «экрана»", "%.0f", 2 },
    /* 27.09: отблеск металла без постоянной доли — светлеет только там, где отражённый луч смотрит к потолку, и немного под
     * скользящим углом (было: +24% всегда, светлел весь металл — стеллажи ST10C); 0 — как 25.09 ночью (3) */
    { "spec_metal_soft", "DC_METAL_SOFT",      1.f,   0.f, 1.f,   "металл: светлеет только от отражённого", "металл не светлеет весь: только там, где отражает потолок и лампы; выкл — как 25.09 ночью (светлел весь, +24% всегда)", "%.0f", 2 },
    /* 27.09 вечер (игрок, слот 3: «материал металла как будто пластик а не металл. нет шероховатости, нет матовости»): блик,
     * отблеск и отражение металла шли по гладкой нормали грани. Теперь рельеф из самой текстуры (яркость — высота), ржавчина
     * и грязь матовые, блик шире и мягче, отражение мутнее (dc_raster.frag «ШЕРОХОВАТЫЙ МЕТАЛЛ»); выкл — как было */
    { "spec_metal_rough", "DC_METAL_ROUGH",    1.f,   0.f, 1.f,   "металл шероховатый (не пластик)", "рельеф из самой текстуры, ржавчина и грязь не блестят, блик шире и мягче, отражение на полу мутнее и дрожит по рельефу; выкл — гладкий, как было", "%.0f", 2 },
    { "spec_metal_matte", "DC_METAL_MATTE",    0.7f,  0.f, 1.f,   "металл: матовость", "насколько металл матовый: блик шире и мягче, отблеск и отражение слабее и мутнее (0 — гладкий)", "%.2f", 2 },
    { "spec_metal_bump",  "DC_METAL_BUMP",     1.0f,  0.f, 3.f,   "металл: рельеф", "сила рельефа из текстуры: рифлёнка, царапины, заклёпки ломают блик и отражение (0 — ровная грань)", "%.2f", 2 },
    { "spec_metal_rust",  "DC_METAL_RUST",     0.8f,  0.f, 1.f,   "металл: ржавчина и грязь матовые", "рыжие (ржавчина) и тёмные (грязь) тексели почти не блестят и не отражают; чистый металл — как был", "%.2f", 2 },
    { "spec_metal_grain", "DC_METAL_GRAIN",    0.5f,  0.f, 1.f,   "металл: зерно", "разброс блеска по текселям — блик неровный, зернистый (привязан к текстуре, не плывёт)", "%.2f", 2 },
    /* 27.09: ОБЪЁМНЫЙ СВЕТ У ЛАМП (игрок: «Попробуй объемный свет у ламп сделать (дымка и пыль в лучах)» — №4 списка от 24.09):
     * дымка в воздухе у ламп игры и текстур «светится», тени персонажей прямо в ней, пыль в свете ламп (dc_vol*.frag, dc_dust.*) */
    { "vol",         "DC_VOL",         1.f,   0.f, 1.f,    "объёмный свет у ламп", "дымка в воздухе у светящихся текстур (нарисованных ламп, экранов — выбор в приложении) и пыль в их свете; глубина — из прохода SSAO (без SSAO нет и дымки)", "%.0f", 2 },
    /* игрок: «оставь эту дымку только на местах светящихся текстур (это визуально лампы и т.д. А реальные источники света от
     * разрабов не соответствуют визуальному количеству и расположению нарисованных ламп)» — у ламп игры по умолчанию 0 */
    { "vol_str",     "DC_VOL_STR",     0.f,   0.f, 2.f,    "дымка: у ламп игры", "у невидимых ламп таблицы игры (их число и места не совпадают с нарисованными лампами — по умолчанию 0)", "%.2f", 2 },
    { "vol_glow",    "DC_VOL_GLOW",    2.0f,  0.f, 4.f,    "дымка: у светящихся текстур", "насколько светится воздух у текстур «светится» (плафоны, экраны — выбор в приложении): это и есть нарисованные лампы", "%.2f", 2 },
    { "vol_rad",     "DC_VOL_RAD",     0.7f,  0.1f, 1.5f,  "дымка: размер", "докуда от лампы видна дымка — доля радиуса её света", "%.2f", 2 },
    { "vol_g",       "DC_VOL_G",       0.35f, 0.f, 0.9f,   "дымка: ярче к лампе", "рассеяние вперёд: насколько дымка ярче, когда смотришь в сторону лампы (0 — одинаково со всех сторон)", "%.2f", 2 },
    { "vol_noise",   "DC_VOL_NOISE",   0.5f,  0.f, 1.f,    "дымка: клубы", "неровность воздуха — медленно плывущие клубы (0 — ровная)", "%.2f", 2 },
    { "vol_steps",   "DC_VOL_STEPS",   12.f,  4.f, 32.f,   "дымка: шагов по лучу", "на лампу; больше — точнее тени в дымке, дороже", "%.0f", 2 },
    { "vol_shadow",  "DC_VOL_SHADOW",  1.f,   0.f, 1.f,    "дымка: тени персонажей", "персонаж заслоняет свет лампы прямо в воздухе (у ламп, у которых есть карта теней)", "%.0f", 2 },
    /* игрок: «пыль не надо. оставь только эту дымку» — по умолчанию 0 */
    { "vol_dust",    "DC_VOL_DUST",    0.f,   0.f, 2.f,    "пыль: яркость", "пылинки в свете ламп (0 — нет; игрок пыль не захотел)", "%.2f", 2 },
    { "vol_dust_n",  "DC_VOL_DUST_N",  700.f, 0.f, 4000.f, "пыль: сколько", "пылинок на все лампы кадра", "%.0f", 2 },
    { "vol_dust_sz", "DC_VOL_DUST_SZ", 3.f,   0.5f, 12.f,  "пыль: размер", "размер пылинки, единиц комнаты (на экране — от 1.5 до 4.5 пикселя)", "%.1f", 2 },
    { "vol_dbg",     "DC_VOL_DBG",     0.f,   0.f, 1.f,    "дымка: показать лампы", "кольца на точках ламп: пурпурные — лампы игры, голубые — светящиеся текстуры", "%.0f", 2 },
    { "fx_dbg",      "DC_FX_DBG",         0.f,    0.f,    2.f,     "показать буфер",   "0 — кадр, 1 — только свечение (размытое), 2 — только отражение", "%.0f", 2 },
};
static float  s_pp[PP_COUNT];
static int    s_ppMsaa = 4, s_ppSshade = 0, s_ppSsao = 1, s_ppDbg = 0, s_ppInit = 0;
static int    s_ppMsaaMax = 8;              /* предел устройства (выборки цвета и глубины) */
static int    s_ppSSok = 1;                 /* устройство умеет sampleRateShading */
/* 09.10.2026: «по выборкам» — свет на пиксель, тексель на выборку (dc_raster_sl.frag.spv, DC_SAMPLE_LOOP): тени, луна, свет и
 * блики считаются раз на пиксель, а не на каждую из N выборок. DC_MSAA_SLOOP=0 — прежний шейдер на каждую выборку. */
static int    s_ppSloop = 1;
static int    s_ppMsFast = 1;               /* 28.10.2026: и тексель раз на пиксель внутри полигона (RasterSpec msFull), DC_MSAA_FAST=0 - откат */
static int    s_ppSLfail = 0;               /* вариант со светом на пиксель не собрался — прежний */
static int    s_ppMsaaFail = 0;             /* сборка MSAA не удалась — дальше без неё, до новой настройки */
static int    s_ppAoFrame = 0, s_ppMsFrame = 1;   /* что было в последнем кадре (для панели) */
static double s_gpuMain = 0.0, s_gpuPost = 0.0;   /* мс на GPU: проход кадра и пост-проходы (сглаженные) */
/* 25.10.2026: сглаживание фильтром готовой картинки вместо MSAA (меню GRAPHICS -> Anti-Aliasing): 0 - нет, 1 - FXAA,
 * 2 - SMAA, 3 - CMAA2 («СГЛАЖИВАНИЕ FXAA / SMAA / CMAA2» ниже) */
static int    s_ppAA = 0, s_ppAAFrame = 0, s_ppAAFail = 0;
static double s_gpuAA = 0.0;                      /* мс на GPU: сглаживание (сглаженные) */
static const char* const PP_AA_NAME[4] = { "off", "FXAA", "SMAA", "CMAA2" };
static VkQueryPool s_tq = VK_NULL_HANDLE;
static int    s_tqOk = 0; static uint32_t s_tsBits = 0; static float s_tsPeriod = 0.f;
static const char* const PP_FILE = "dc_post_user.cfg";

static int pp_norm_msaa(int n) { return n <= 1 ? 1 : n <= 2 ? 2 : n <= 4 ? 4 : 8; }
static void pp_defaults(void) {
    for (int i = 0; i < PP_COUNT; i++) {
        const char* e = getenv(s_ppDef[i].env);
        s_pp[i] = e ? (float)atof(e) : s_ppDef[i].def;
    }
    { const char* e = getenv("DC_MSAA");        s_ppMsaa   = e ? atoi(e) : 4; }
    { const char* e = getenv("DC_MSAA_SSHADE"); s_ppSshade = (e && *e == '1') ? 1 : 0; }
    { const char* e = getenv("DC_SSAO");        s_ppSsao   = (e && *e == '0') ? 0 : 1; }
    { const char* e = getenv("DC_MSAA_SLOOP");  s_ppSloop  = (e && *e == '0') ? 0 : 1; }
    { const char* e = getenv("DC_MSAA_FAST");   s_ppMsFast = (e && *e == '0') ? 0 : 1; }
    { const char* e = getenv("DC_POST_AA");     s_ppAA     = e ? atoi(e) : 0; }
}
static void pp_clamp(void) {
    s_ppMsaa = pp_norm_msaa(s_ppMsaa);
    if (s_ppAA < 0 || s_ppAA > 3) s_ppAA = 0;
    for (int i = 0; i < PP_COUNT; i++) {
        if (s_pp[i] < s_ppDef[i].lo) s_pp[i] = s_ppDef[i].lo;
        if (s_pp[i] > s_ppDef[i].hi) s_pp[i] = s_ppDef[i].hi;
    }
}
#ifdef DCME_RELEASE   /* ME V3 post settings: our data from the program (dc_pak.c) */
#include "dc_pak.h"
#pragma push_macro("GetFileAttributesExA")
#undef GetFileAttributesExA
#pragma push_macro("FILE")
#undef FILE
#pragma push_macro("fopen")
#undef fopen
#pragma push_macro("fgets")
#undef fgets
#pragma push_macro("fread")
#undef fread
#pragma push_macro("fclose")
#undef fclose
#pragma push_macro("_stat64")
#undef _stat64
#pragma push_macro("_findfirst")
#undef _findfirst
#pragma push_macro("_findnext")
#undef _findnext
#pragma push_macro("_findclose")
#undef _findclose
#define GetFileAttributesExA(p, lvl, fa) dcme_file_attr((p), (fa))
#define FILE DcMemFile
#define fopen(p, m) dc_mf_open(p)
#define fgets(b, n, f) dc_mf_gets((b), (n), (f))
#define fread(d, s, c, f) dc_mf_read((d), (s), (c), (f))
#define fclose(f) dc_mf_close(f)
#define _stat64(p, st) dcme_stat64((p), (st))
#define _findfirst(p, fd) dcme_findfirst((p), (fd))
#define _findnext(h, fd) dcme_findnext((h), (fd))
#define _findclose(h) dcme_findclose(h)
#endif
static void pp_init(void) {
    if (s_ppInit) return;
    s_ppInit = 1;
    pp_defaults();
    { const char* e = getenv("DC_SSAO_DBG"); s_ppDbg = (e && *e == '1') ? 1 : 0; }
    const char* nc = getenv("DC_POST_NOCFG");
    int fromFile = 0;
    if (!(nc && *nc == '1')) {
        FILE* f = fopen(PP_FILE, "r");
        if (f) {
            char line[160];
            while (fgets(line, sizeof line, f)) {
                char k[64]; float v;
                if (line[0] == '#' || sscanf(line, "%63s %f", k, &v) != 2) continue;
                fromFile = 1;
                if (!strcmp(k, "msaa")) s_ppMsaa = (int)v;
                else if (!strcmp(k, "sshade")) s_ppSshade = v > 0.5f ? 1 : 0;
                else if (!strcmp(k, "ssao")) s_ppSsao = v > 0.5f ? 1 : 0;
                else if (!strcmp(k, "postaa")) s_ppAA = (int)(v + 0.5f);
                else for (int i = 0; i < PP_COUNT; i++) if (!strcmp(k, s_ppDef[i].key)) s_pp[i] = v;
            }
            fclose(f);
        }
    }
    pp_clamp();
    std::fprintf(stderr, "post: SSAO %s (radius %.0f, strength %.2f, samples %.0f), MSAA %dx%s, AA filter %s%s%c",
                 s_ppSsao ? "on" : "off", s_pp[PP_RADIUS], s_pp[PP_STRENGTH], s_pp[PP_SAMPLES], s_ppMsaa,
                 s_ppSshade ? " per sample" : "", PP_AA_NAME[s_ppAA], fromFile ? " - from dc_post_user.cfg" : "", 10);
    std::fprintf(stderr, "post: flashes as light %s (shot strength %.2f radius %.0f %.0f ms, blast strength %.2f radius %.0f %.0f ms, wrap %.2f)%c",
                 s_pp[PP_FL_ON] > 0.5f ? "on" : "off", s_pp[PP_FL_SSTR], s_pp[PP_FL_SRAD], s_pp[PP_FL_SMS],
                 s_pp[PP_FL_BSTR], s_pp[PP_FL_BRAD], s_pp[PP_FL_BMS], s_pp[PP_FL_WRAP], 10);
}
#ifdef DCME_RELEASE   /* ME V3 post settings: end */
#undef GetFileAttributesExA
#pragma pop_macro("GetFileAttributesExA")
#undef FILE
#pragma pop_macro("FILE")
#undef fopen
#pragma pop_macro("fopen")
#undef fgets
#pragma pop_macro("fgets")
#undef fread
#pragma pop_macro("fread")
#undef fclose
#pragma pop_macro("fclose")
#undef _stat64
#pragma pop_macro("_stat64")
#undef _findfirst
#pragma pop_macro("_findfirst")
#undef _findnext
#pragma pop_macro("_findnext")
#undef _findclose
#pragma pop_macro("_findclose")
#endif
static int gl_chr_smooth(void) { pp_init(); return s_pp[PP_GL_LCHR] > 0.5f; }
extern "C" void dcvk_post_save(void) {
#ifdef DCME_RELEASE
    return;   /* ME V3: nothing written next to the game (the F8 window that saves is not in a release) */
#endif
    pp_init();
    char buf[4096]; int n = 0;
    n += snprintf(buf + n, sizeof buf - n, "# F8 -> Kartinka: SSAO i MSAA, vspyshki (fl_*), svechenie i otrazheniya (glow_*, refl_*). Udalit' fayl - znacheniya iz run_dc_hle.bat\n");
    n += snprintf(buf + n, sizeof buf - n, "msaa %d\nsshade %d\nssao %d\npostaa %d\n", s_ppMsaa, s_ppSshade, s_ppSsao, s_ppAA);
    for (int i = 0; i < PP_COUNT && n < (int)sizeof buf - 64; i++)
        n += snprintf(buf + n, sizeof buf - n, "%s %g\n", s_ppDef[i].key, s_pp[i]);
    FILE* f = fopen(PP_FILE, "w");          /* текст собран заранее: открытие на запись обрезает файл */
    if (!f) return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
}
extern "C" int   dcvk_post_msaa(void)          { pp_init(); return s_ppMsaa; }
extern "C" int   dcvk_post_msaa_max(void)      { return s_ppMsaaMax; }
extern "C" void  dcvk_post_msaa_set(int n)     { pp_init(); s_ppMsaa = pp_norm_msaa(n); s_ppMsaaFail = 0; }
/* 25.10.2026: FXAA / SMAA / CMAA2 (0 - нет, 1..3); работает в кадрах без MSAA */
extern "C" int    dcvk_post_aa(void)           { pp_init(); return s_ppAA; }
extern "C" void   dcvk_post_aa_set(int m)      { pp_init(); s_ppAA = (m < 0 || m > 3) ? 0 : m; s_ppAAFail = 0; }
extern "C" double dcvk_post_aa_ms(void)        { return s_ppAAFrame ? s_gpuAA : 0.0; }
extern "C" int   dcvk_post_sshade(void)        { pp_init(); return s_ppSshade; }
extern "C" int   dcvk_post_sshade_ok(void)     { return s_ppSSok; }
extern "C" void  dcvk_post_sshade_set(int on)  { pp_init(); s_ppSshade = on ? 1 : 0; s_ppMsaaFail = 0; }
extern "C" int   dcvk_post_sloop(void)         { pp_init(); return s_ppSloop; }
extern "C" void  dcvk_post_sloop_set(int on)   { pp_init(); s_ppSloop = on ? 1 : 0; s_ppMsaaFail = 0; }
extern "C" int   dcvk_post_ssao(void)          { pp_init(); return s_ppSsao; }
extern "C" void  dcvk_post_ssao_set(int on)    { pp_init(); s_ppSsao = on ? 1 : 0; }
extern "C" int   dcvk_post_dbg(void)           { pp_init(); return s_ppDbg; }
extern "C" void  dcvk_post_dbg_set(int on)     { pp_init(); s_ppDbg = on ? 1 : 0; }
extern "C" int   dcvk_post_par_count(void)     { return PP_COUNT; }
extern "C" float dcvk_post_par(int i)          { pp_init(); return (i >= 0 && i < PP_COUNT) ? s_pp[i] : 0.f; }
extern "C" void  dcvk_post_par_set(int i, float v) {
    pp_init();
    if (i < 0 || i >= PP_COUNT) return;
    s_pp[i] = v < s_ppDef[i].lo ? s_ppDef[i].lo : v > s_ppDef[i].hi ? s_ppDef[i].hi : v;
}
extern "C" const char* dcvk_post_par_name(int i) { return (i >= 0 && i < PP_COUNT) ? s_ppDef[i].name : ""; }
extern "C" const char* dcvk_post_par_key(int i)  { return (i >= 0 && i < PP_COUNT) ? s_ppDef[i].key : ""; }
extern "C" const char* dcvk_post_par_hint(int i) { return (i >= 0 && i < PP_COUNT) ? s_ppDef[i].hint : ""; }
extern "C" const char* dcvk_post_par_fmt(int i)  { return (i >= 0 && i < PP_COUNT) ? s_ppDef[i].fmt : "%.2f"; }
extern "C" void  dcvk_post_par_range(int i, float* lo, float* hi) {
    if (i >= 0 && i < PP_COUNT) { *lo = s_ppDef[i].lo; *hi = s_ppDef[i].hi; }
}
extern "C" int   dcvk_post_par_is_toggle(int i) { return i == PP_BLUR || i == PP_FL_ON || i == PP_FL_SYNC ||
                                                         i == PP_GL_ON || i == PP_RF_ON || i == PP_RF_ALL || i == PP_GL_LON ||
                                                         i == PP_SP_ON || i == PP_SP_OLD || i == PP_GL_LHUE || i == PP_SP_SOFT ||
                                                         i == PP_VL_ON || i == PP_VL_SH || i == PP_VL_DBG || i == PP_GL_LSTEADY ||
                                                         i == PP_SP_MROUGH || i == PP_GL_LCHR; }   /* в панели — галка, а не ползунок */
extern "C" int   dcvk_post_par_group(int i)     { return (i >= 0 && i < PP_COUNT) ? s_ppDef[i].grp : -1; }
extern "C" int   dcvk_post_par_find(const char* key) {
    for (int i = 0; i < PP_COUNT; i++) if (!strcmp(s_ppDef[i].key, key)) return i;
    return -1;
}
extern "C" void  dcvk_post_defaults(void) {    /* кнопка раздела «Картинка»: SSAO/MSAA; вспышки (группа 1) не трогает */
    pp_init();
    float keep[PP_COUNT];
    for (int i = 0; i < PP_COUNT; i++) keep[i] = s_pp[i];
    pp_defaults();
    for (int i = 0; i < PP_COUNT; i++) if (s_ppDef[i].grp != 0) s_pp[i] = keep[i];
    pp_clamp(); s_ppMsaaFail = 0;
}
/* 25.09: «по умолчанию» одной группы (панель вспышек не трогает SSAO/MSAA и наоборот) */
extern "C" void  dcvk_post_defaults_grp(int g) {
    pp_init();
    for (int i = 0; i < PP_COUNT; i++) {
        if (s_ppDef[i].grp != g) continue;
        const char* e = getenv(s_ppDef[i].env);
        s_pp[i] = e ? (float)atof(e) : s_ppDef[i].def;
    }
    pp_clamp();
}
extern "C" void  dcvk_post_stats(double* gpuMain, double* gpuPost, int* aoFrame, int* msFrame, int* msFail) {
    *gpuMain = s_gpuMain; *gpuPost = s_gpuPost; *aoFrame = s_ppAoFrame; *msFrame = s_ppMsFrame; *msFail = s_ppMsaaFail;
}

/* пределы устройства и пул меток времени (зовётся из buildFixed) */
static void pp_caps(void) {
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(C->phys, &p);
    const VkSampleCountFlags sc = p.limits.framebufferColorSampleCounts & p.limits.framebufferDepthSampleCounts;
    s_ppMsaaMax = (sc & VK_SAMPLE_COUNT_8_BIT) ? 8 : (sc & VK_SAMPLE_COUNT_4_BIT) ? 4 : (sc & VK_SAMPLE_COUNT_2_BIT) ? 2 : 1;
    s_tsPeriod = p.limits.timestampPeriod;
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(C->phys, &f);
    s_ppSSok = f.sampleRateShading ? 1 : 0;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(C->phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qp(nq);
    if (nq) vkGetPhysicalDeviceQueueFamilyProperties(C->phys, &nq, qp.data());
    s_tsBits = (C->qfam < nq) ? qp[C->qfam].timestampValidBits : 0u;
    if (s_tsBits) {
        VkQueryPoolCreateInfo q{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        q.queryType = VK_QUERY_TYPE_TIMESTAMP; q.queryCount = 16;  /* 09.10: 3..12 - этапы кадра (DC_VK_STAGES) */
        if (vkCreateQueryPool(C->device, &q, nullptr, &s_tq) == VK_SUCCESS) s_tqOk = 1;
    }
    rlog("[VKR] post: MSAA up to %dx, sampleRateShading %d, timestamps %u bits (%.2f ns)\n",
         s_ppMsaaMax, s_ppSSok, s_tsBits, (double)s_tsPeriod);
}
static void pp_ts_reset(void) { if (s_tqOk) vkCmdResetQueryPool(s_cmd, s_tq, 0, 14); }   /* 25.10: 13 - после сглаживания */
static void pp_ts(int i)      { if (s_tqOk) vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_tq, (uint32_t)i); }
/* 09.10.2026: этапы кадра на GPU для DC_VK_STAGES (метки 3 - начало, 4 - после теней, 5 - после копий; 0..2 - как были).
 * out: тени+заливки, отражение/свечение-геометрия, основной проход, после него (SSAO, свечение, дымка), копии. */
static int pp_ts_stages(double out[6]) {
    if (!s_tqOk) return 0;
    uint64_t q[7] = { 0, 0, 0, 0, 0, 0, 0 };
    if (vkGetQueryPoolResults(C->device, s_tq, 0, 7, sizeof q, q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    const uint64_t m = (s_tsBits >= 64) ? ~0ull : ((1ull << s_tsBits) - 1ull);
    const double k = (double)s_tsPeriod * 1e-6;
    out[0] = (double)((q[4] - q[3]) & m) * k; out[1] = (double)((q[0] - q[4]) & m) * k;
    out[2] = (double)((q[1] - q[0]) & m) * k; out[3] = (double)((q[2] - q[1]) & m) * k;
    out[4] = (double)((q[6] - q[2]) & m) * k; out[5] = (double)((q[5] - q[6]) & m) * k;   /* чтение в память / копия прошлого */
    return 1;
}
/* 09.10 (2): подробнее — заливки (3..7), карты ламп (7..8), луна (8..9), пирамиды (9..4); SSAO: глубина (13..10; 25.10:
 * от метки после сглаживания), затенение (10..11), размытие с умножением (11..12) */
static int pp_ts_detail(double out[7]) {
    if (!s_tqOk) return 0;
    uint64_t q[14] = {};
    if (vkGetQueryPoolResults(C->device, s_tq, 0, 14, sizeof q, q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return 0;
    const uint64_t m = (s_tsBits >= 64) ? ~0ull : ((1ull << s_tsBits) - 1ull);
    const double k = (double)s_tsPeriod * 1e-6;
    out[0] = (double)((q[7] - q[3]) & m) * k;  out[1] = (double)((q[8] - q[7]) & m) * k;
    out[2] = (double)((q[9] - q[8]) & m) * k;  out[3] = (double)((q[4] - q[9]) & m) * k;
    out[4] = (double)((q[10] - q[13]) & m) * k; out[5] = (double)((q[11] - q[10]) & m) * k;
    out[6] = (double)((q[12] - q[11]) & m) * k;
    return 1;
}
static void pp_ts_read(void) {
    if (!s_tqOk) return;
    uint64_t q[3] = { 0, 0, 0 };
    if (vkGetQueryPoolResults(C->device, s_tq, 0, 3, sizeof q, q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    const uint64_t m = (s_tsBits >= 64) ? ~0ull : ((1ull << s_tsBits) - 1ull);
    const double k = (double)s_tsPeriod * 1e-6;
    const double a = (double)((q[1] - q[0]) & m) * k, b = (double)((q[2] - q[1]) & m) * k;
    g_vk_last_gpu_ms[0] = a; g_vk_last_gpu_ms[1] = b;
    s_gpuMain = s_gpuMain * 0.9 + a * 0.1;
    s_gpuPost = s_gpuPost * 0.9 + b * 0.1;
    double c = 0.0;                                   /* 25.10.2026: сглаживание - от конца основного прохода до метки 13 */
    if (s_ppAAFrame) {
        uint64_t q13 = 0;
        if (vkGetQueryPoolResults(C->device, s_tq, 13, 1, sizeof q13, &q13, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            c = (double)((q13 - q[1]) & m) * k;
            s_gpuAA = s_gpuAA * 0.9 + c * 0.1;
        }
    }
    static int dbg = -1; static long n = 0;
    if (dbg < 0) { const char* e = getenv("DC_POST_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
    if (dbg && (++n % 120) == 0)
        std::fprintf(stderr, "post: GPU frame %.3f ms, SSAO %.3f ms (now %.3f / %.3f), MSAA %dx%s, SSAO %s, AA filter %s %.3f ms (now %.3f)%c",
                     s_gpuMain, s_gpuPost, a, b, s_ppMsFrame, s_ppSshade ? " per sample" : "", s_ppAoFrame ? "in the frame" : "no",
                     s_ppAAFrame ? PP_AA_NAME[s_ppAA] : "no", s_gpuAA, c, 10);
}

static bool pp_mkImage(VkFormat fmt, VkSampleCountFlagBits ns, VkImageUsageFlags use, VkImageAspectFlags asp,
                       int w, int h, VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = ns; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = use; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(C->device, &ici, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(C->device, *img, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(C->device, &mai, nullptr, mem) != VK_SUCCESS) return false;
    vkBindImageMemory(C->device, *img, *mem, 0);
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = { asp, 0, 1, 0, 1 };
    return vkCreateImageView(C->device, &vci, nullptr, view) == VK_SUCCESS;
}
static void pp_freeImage(VkImage* img, VkDeviceMemory* mem, VkImageView* view) {
    if (*view) vkDestroyImageView(C->device, *view, nullptr);
    if (*img) vkDestroyImage(C->device, *img, nullptr);
    if (*mem) vkFreeMemory(C->device, *mem, nullptr);
    *view = VK_NULL_HANDLE; *img = VK_NULL_HANDLE; *mem = VK_NULL_HANDLE;
}

/* 24.10.2026: конец основного прохода (1x и MSAA, и отрезки захвата сцены): цель цвета уходит в TRANSFER_SRC и сразу читается
 * копиями (кадр наружу, строки яркости, захват сцены), глубину берёт SSAO. Без своей зависимости в конце прохода эти чтения
 * не ждали смены раскладки (проверка синхронизации Vulkan: READ_AFTER_WRITE) - на другом драйвере кадр мог уйти недорисованным.
 * Одна и та же зависимость во всех проходах, совместимых между собой (конвейеры собраны под один, рисуют и в других).
 * DC_PASS_DEP=0 - как было. */
static uint32_t main_pass_dep(const VkSubpassDependency** out) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_PASS_DEP"); on = (e && *e == '0') ? 0 : 1; }
    static VkSubpassDependency d{};
    d.srcSubpass = 0; d.dstSubpass = VK_SUBPASS_EXTERNAL;
    d.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    d.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    d.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    d.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    *out = on ? &d : nullptr;
    return on ? 1u : 0u;
}

/* ---- конвейеры растра: 11 вариантов + фон под проход rp и число выборок ns. Текст — тот же, что был в buildFixed ---- */
static bool mkRasterPipesImpl(VkRenderPass rp, VkSampleCountFlagBits ns, VkShaderModule vs, VkShaderModule fs,
                              VkShaderModule bvs, VkShaderModule bfs, VkPipeline* pipes, VkPipeline* bgOut,
                              const VkSpecializationInfo* spec) {

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    st[1].pSpecializationInfo = spec;                  /* 25.10.2026: размер массива и опции (RasterSpec) */

    /* 05.10: привязка 1 — второй буфер вершин s_rvBuf (текстура PNG из mods: номер и место в картинке, TEXREP) */
    /* 26.10.2026: привязка 2 - метка куска s_pvBuf (пары кусков) */
    VkVertexInputBindingDescription vb[3]{ { 0, VERT_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
                                           { 1, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
                                           { 2, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX } };
    VkVertexInputAttributeDescription va[7]{
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       8 },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
        { 4, 0, VK_FORMAT_R32_SFLOAT,          48 },
        { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 6, 2, VK_FORMAT_R32_UINT,            0 },
    };
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 3; vin.pVertexBindingDescriptions = vb;
    vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    /* Overestimated (conservative) rasterization: a polygon fills every pixel
     * it TOUCHES, instead of only those whose centre it covers. Sub-pixel body
     * polygons — the ones that vanish and let the room show through, more and
     * more with distance — are drawn again. Vertices are untouched, so nothing
     * shifts; only coverage changes. DC_CONSERVATIVE=0 disables. */
    VkPipelineRasterizationConservativeStateCreateInfoEXT cons{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT };
    cons.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    cons.extraPrimitiveOverestimationSize = 0.0f;
    if (g_vk_conservative) rs.pNext = &cons;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = ns;                  /* 25.09: 1 — как было, N — MSAA */
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;

    /* 11.10.2026: the descriptions of all 12 variants and the background first, then the 13 pipelines are built side by side
     * (vkp_par) - the same states as before, one array cell per variant */
    VkPipelineRasterizationStateCreateInfo rswA[12];
    VkPipelineDepthStencilStateCreateInfo  dsA[12];
    VkPipelineColorBlendAttachmentState    baA[12];
    VkPipelineColorBlendStateCreateInfo    cbA[12];
    VkGraphicsPipelineCreateInfo           gpA[13];
    for (int k = 0; k < 12; k++) {
        /* k == 10 is the wireframe overlay: lines, no depth at all (edges
         * behind other surfaces are exactly what one wants to see), no blend. */
        const int wire = (k == 10);
        /* 01.10 вечер: k == 11 — стекло (флаг F_GLASS, dc_raster.frag): проверка глубины без записи, смешивание с
         * предумноженной альфой (шейдер выдаёт цвет × прозрачность + блик и саму прозрачность) */
        const int glass = (k == 11);
        int depth = !wire && ((k == 0) || (k >= 2 && k <= 5) || glass);
        int zwrite = !wire && (k == 0);
        int blend = (!wire && !glass && k >= 2) ? ((k - 2) & 3) : -1;
        VkPipelineRasterizationStateCreateInfo& rsw = rswA[k];
        rsw = rs;
        if (wire) { rsw.polygonMode = VK_POLYGON_MODE_LINE; rsw.pNext = nullptr; }

        VkPipelineDepthStencilStateCreateInfo& ds = dsA[k];
        ds = VkPipelineDepthStencilStateCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState& ba = baA[k];
        ba = VkPipelineColorBlendAttachmentState{};
        ba.colorWriteMask = 0xF;
        if (blend >= 0) {
            ba.blendEnable = VK_TRUE;
            ba.colorBlendOp = VK_BLEND_OP_ADD;
            ba.alphaBlendOp = VK_BLEND_OP_ADD;
            /* 25.09: альфа цели — маска SSAO (dc_raster.frag aoMaskOut): полупрозрачное её не меняет. Цвет — как был;
             * без SSAO альфа везде 1 и так и остаётся. */
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            switch (blend) {
            case 0: /* 0.5D + 0.5S */
                ba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
                ba.dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
                break;
            case 1: /* D + S */
                ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            case 2: /* D - S */
                ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                ba.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
                break;
            case 3: /* D + 0.25S */
                ba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
                ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            }
        }
        if (glass) {                                   /* S + D·(1 − αS); альфа цели (маска SSAO) не трогается */
            ba.blendEnable = VK_TRUE;
            ba.colorBlendOp = VK_BLEND_OP_ADD; ba.alphaBlendOp = VK_BLEND_OP_ADD;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        }
        VkPipelineColorBlendStateCreateInfo& cb = cbA[k];
        cb = VkPipelineColorBlendStateCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &ba;
        /* constants: 0.5 for mode 0, 0.25 for mode 3 — distinct pipelines,
         * so bake the right constant per variant */
        float cc = (blend == 3) ? 0.25f : 0.5f;
        cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] = cc;

        VkGraphicsPipelineCreateInfo& gp = gpA[k];
        gp = VkGraphicsPipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = wire ? &rsw : &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
        gp.layout = s_plLay; gp.renderPass = rp; gp.subpass = 0;
    }
    /* Fullscreen VRAM background pipeline: no vertex input, no depth, no
     * blend — draws the display region as the base image before the prims. */
    VkPipelineShaderStageCreateInfo bst[2]{};
    bst[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    bst[0].stage = VK_SHADER_STAGE_VERTEX_BIT; bst[0].module = bvs; bst[0].pName = "main";
    bst[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    bst[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; bst[1].module = bfs; bst[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo bvin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo bds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState bba{}; bba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo bcb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bcb.attachmentCount = 1; bcb.pAttachments = &bba;
    VkGraphicsPipelineCreateInfo& bgp = gpA[12];
    bgp = VkGraphicsPipelineCreateInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    bgp.stageCount = 2; bgp.pStages = bst;
    bgp.pVertexInputState = &bvin; bgp.pInputAssemblyState = &ia;
    bgp.pViewportState = &vp; bgp.pRasterizationState = &rs;
    bgp.pMultisampleState = &ms; bgp.pDepthStencilState = &bds;
    bgp.pColorBlendState = &bcb; bgp.pDynamicState = &dst;
    bgp.layout = s_plLay; bgp.renderPass = rp; bgp.subpass = 0;

    VkPipeline outA[13] = {};
    VkResult resA[13];
    /* two waves: the driver compiles the shader of k = 0..3 (and the background) and gives the other 8 variants out of
     * what it already compiled (measured with an empty driver cache: k = 4..11 took 0.0 ms one after another; all 13 at
     * once compiled every one of them - three times the work for a processor with few cores) */
    static const int wave1[5] = { 0, 1, 2, 3, 12 }, wave2[8] = { 4, 5, 6, 7, 8, 9, 10, 11 };
    vkp_par(5, [&](int j) { const int i = wave1[j]; resA[i] = vkp_create(&gpA[i], &outA[i], i < 12 ? "raster" : "raster bg"); });
    vkp_par(8, [&](int j) { const int i = wave2[j]; resA[i] = vkp_create(&gpA[i], &outA[i], "raster"); });
    bool ok = true;
    for (int k = 0; k < 12; k++) {
        if (resA[k] != VK_SUCCESS) {
            rlog("[VKR] pipeline %d FAILED\n", k);
            outA[k] = VK_NULL_HANDLE;
            if (k != 10) ok = false;                    /* k == 10, the wireframe: diagnostic only */
        }
        pipes[k] = outA[k];
    }
    if (resA[12] != VK_SUCCESS) { rlog("[VKR] bg pipeline FAILED\n"); outA[12] = VK_NULL_HANDLE; ok = false; }
    *bgOut = outA[12];
    return ok;                                          /* on a failure the caller destroys what was built */
}

static VkPipeline mesh_main_pipe(VkRenderPass rp, VkSampleCountFlagBits ns, VkShaderModule fs,
                                 const VkSpecializationInfo* spec);   /* 06.10.2026, ниже */
/* sshade: 0 — шейдер на пиксель, 1 — на каждую выборку (dc_raster_ss), 2 — 09.10: свет на пиксель, тексель на выборку (dc_raster_sl);
 * feat - 25.10.2026: опции в шейдере (raster_feat_want) */
static bool mkRasterPipes(VkRenderPass rp, VkSampleCountFlagBits ns, int sshade, VkPipeline* pipes, VkPipeline* bgOut, int feat) {
    const RasterSpec rsd = { s_trepCap, (feat & 1) ? VK_TRUE : VK_FALSE, (feat & 2) ? VK_TRUE : VK_FALSE,
                             (sshade == 2 && s_ppMsFast && ns > VK_SAMPLE_COUNT_1_BIT) ? (1 << (int)ns) - 1 : 0 };
    const VkSpecializationInfo rsi = { 4, s_rspecMap, sizeof(RasterSpec), &rsd };
    VkShaderModule vs  = loadSpv("dc_raster.vert.spv");
    VkShaderModule fs  = loadSpv(raster_fs_name(sshade));   /* 25.10.2026: безопасные шейдеры (s_rasterSafe) */
    VkShaderModule bvs = loadSpv("dc_bg.vert.spv");
    VkShaderModule bfs = loadSpv("dc_bg.frag.spv");
    for (int k = 0; k < 13; k++) pipes[k] = VK_NULL_HANDLE;
    *bgOut = VK_NULL_HANDLE;
    bool ok = vs && fs && bvs && bfs && !(s_rasterSafe >= 2 && ns != VK_SAMPLE_COUNT_1_BIT);   /* уровень 2 - без MSAA */
    if (ok) {
        /* 06.10.2026: меш из mods (нет — прежний путь процессора); 11.10: собирается рядом с набором растра */
        VkPipeline meshp = VK_NULL_HANDLE;
        std::thread mt;
        if (vkp_threads_on()) mt = std::thread([&meshp, &rsi, rp, ns, fs] { meshp = mesh_main_pipe(rp, ns, fs, &rsi); });
        ok = mkRasterPipesImpl(rp, ns, vs, fs, bvs, bfs, pipes, bgOut, &rsi);
        if (mt.joinable()) mt.join();
        else if (ok) meshp = mesh_main_pipe(rp, ns, fs, &rsi);
        if (ok) pipes[12] = meshp;
        else if (meshp) vkDestroyPipeline(C->device, meshp, nullptr);
    }
    if (!ok) {
        for (int k = 0; k < 13; k++) if (pipes[k]) { vkDestroyPipeline(C->device, pipes[k], nullptr); pipes[k] = VK_NULL_HANDLE; }
        if (*bgOut) { vkDestroyPipeline(C->device, *bgOut, nullptr); *bgOut = VK_NULL_HANDLE; }
    }
    if (vs)  vkDestroyShaderModule(C->device, vs, nullptr);
    if (fs)  vkDestroyShaderModule(C->device, fs, nullptr);
    if (bvs) vkDestroyShaderModule(C->device, bvs, nullptr);
    if (bfs) vkDestroyShaderModule(C->device, bfs, nullptr);
    return ok;
}

/* ---- MSAA: цели на N выборок, проходы (обычный / с сохранением / продолжение), кадровый буфер, конвейеры ---- */
static int            s_msN = 1, s_msSS = 0;      /* выборок у собранных объектов (1 — не собраны) и «по выборкам» */
static VkRenderPass   s_rpMS = VK_NULL_HANDLE, s_rpMSFirst = VK_NULL_HANDLE, s_rpMSResume = VK_NULL_HANDLE;
static VkPipeline     s_pipesMS[13] = {};
static VkPipeline     s_bgPipeMS = VK_NULL_HANDLE;
static VkImage        s_msColImg = VK_NULL_HANDLE, s_msDepImg = VK_NULL_HANDLE;
static VkDeviceMemory s_msColMem = VK_NULL_HANDLE, s_msDepMem = VK_NULL_HANDLE;
static VkImageView    s_msColView = VK_NULL_HANDLE, s_msDepView = VK_NULL_HANDLE;
static VkFramebuffer  s_fbMS = VK_NULL_HANDLE;
static int            s_msW = 0, s_msH = 0;
static VkImageView    s_msResolveOf = VK_NULL_HANDLE;  /* s_colView, под который собран кадровый буфер */
static VkRenderPass   s_rpResumeCur = VK_NULL_HANDLE;  /* продолжение прохода в текущем кадре (scap_switch) */

static void msaaFree(void) {
    for (int k = 0; k < 13; k++) if (s_pipesMS[k]) { vkDestroyPipeline(C->device, s_pipesMS[k], nullptr); s_pipesMS[k] = VK_NULL_HANDLE; }
    if (s_bgPipeMS) { vkDestroyPipeline(C->device, s_bgPipeMS, nullptr); s_bgPipeMS = VK_NULL_HANDLE; }
    if (s_fbMS) { vkDestroyFramebuffer(C->device, s_fbMS, nullptr); s_fbMS = VK_NULL_HANDLE; }
    pp_freeImage(&s_msColImg, &s_msColMem, &s_msColView);
    pp_freeImage(&s_msDepImg, &s_msDepMem, &s_msDepView);
    if (s_rpMS)       { vkDestroyRenderPass(C->device, s_rpMS, nullptr);       s_rpMS = VK_NULL_HANDLE; }
    if (s_rpMSFirst)  { vkDestroyRenderPass(C->device, s_rpMSFirst, nullptr);  s_rpMSFirst = VK_NULL_HANDLE; }
    if (s_rpMSResume) { vkDestroyRenderPass(C->device, s_rpMSResume, nullptr); s_rpMSResume = VK_NULL_HANDLE; }
    s_msN = 1; s_msSS = 0; s_msW = s_msH = 0; s_msResolveOf = VK_NULL_HANDLE;
}
/* 11.10.2026: images and framebuffer of the MSAA pass for the current target size (s_rpMS must exist).
 * 24.10.2026: W x H - by the frames with 3D (msaaEnsure), not the whole target (resolve into s_colImg - it is bigger or the same) */
static bool msaaTargets(VkSampleCountFlagBits ns, int W, int H) {
    bool ok = pp_mkImage(VK_FORMAT_R8G8B8A8_UNORM, ns, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                         W, H, &s_msColImg, &s_msColMem, &s_msColView);
    ok = ok && pp_mkImage(VK_FORMAT_D32_SFLOAT, ns, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT, W, H, &s_msDepImg, &s_msDepMem, &s_msDepView);
    if (ok) {
        VkImageView atts[3] = { s_msColView, s_msDepView, s_colView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = s_rpMS; fci.attachmentCount = 3; fci.pAttachments = atts;
        fci.width = (uint32_t)W; fci.height = (uint32_t)H; fci.layers = 1;
        ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbMS) == VK_SUCCESS;
    }
    return ok;
}
static bool msaaBuild(int n, int ss, int W, int H) {
    vkDeviceWaitIdle(C->device);
    const int feat = raster_feat_goal();               /* 25.10.2026: опции в шейдере (новые - когда их набор в кэше) */
    /* 11.10.2026: only the size of the target (or the view it resolves into) changed - the render passes and the 13 pipelines
     * do not depend on it and stay; the images and the framebuffer are made again. Before, every growth of the target (the
     * first 16:9 frame of the world - the first in-engine cutscene) built the big raster shader 13 times over.
     * DC_MSAA_KEEP_PIPES=0 - everything again (as before). */
    { static int keep = -1;
      if (keep < 0) { const char* e = getenv("DC_MSAA_KEEP_PIPES"); keep = (e && *e == '0') ? 0 : 1; }
      if (keep && n > 1 && n == s_msN && ss == s_msSS && feat == s_featMS && s_rpMS && s_pipesMS[0] && s_bgPipeMS) {
          if (s_fbMS) { vkDestroyFramebuffer(C->device, s_fbMS, nullptr); s_fbMS = VK_NULL_HANDLE; }
          pp_freeImage(&s_msColImg, &s_msColMem, &s_msColView);
          pp_freeImage(&s_msDepImg, &s_msDepMem, &s_msDepView);
          if (msaaTargets((VkSampleCountFlagBits)n, W, H)) {
              s_msW = W; s_msH = H; s_msResolveOf = s_colView;
              std::fprintf(stderr, "post: MSAA %dx - new targets %dx%d, pipelines kept%c", n, W, H, 10);
              { char w[48]; std::snprintf(w, sizeof w, "MSAA %dx at %dx%d", n, W, H); vram_note(w); }
              return true;
          }
      } }
    msaaFree();
    if (n <= 1) { std::fprintf(stderr, "post: MSAA edge smoothing off%c", 10); vram_note("MSAA off"); return true; }
    const VkSampleCountFlagBits ns = (VkSampleCountFlagBits)n;
    VkAttachmentDescription a[3]{};
    a[0].format = VK_FORMAT_R8G8B8A8_UNORM; a[0].samples = ns;                 /* цвет N выборок */
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].format = VK_FORMAT_D32_SFLOAT; a[1].samples = ns;                     /* глубина N выборок */
    a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a[2].format = VK_FORMAT_R8G8B8A8_UNORM; a[2].samples = VK_SAMPLE_COUNT_1_BIT;   /* сведение — прежняя s_colImg */
    a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for (int i = 0; i < 3; i++) { a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    VkAttachmentReference colRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resRef{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &colRef; sp.pResolveAttachments = &resRef;
    sp.pDepthStencilAttachment = &depRef;
    VkRenderPassCreateInfo rci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rci.attachmentCount = 3; rci.pAttachments = a; rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = main_pass_dep(&rci.pDependencies);
    bool ok = vkCreateRenderPass(C->device, &rci, nullptr, &s_rpMS) == VK_SUCCESS;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE; a[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;   /* отрезки / SSAO */
    ok = ok && vkCreateRenderPass(C->device, &rci, nullptr, &s_rpMSFirst) == VK_SUCCESS;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    ok = ok && vkCreateRenderPass(C->device, &rci, nullptr, &s_rpMSResume) == VK_SUCCESS;
    ok = ok && pp_mkImage(VK_FORMAT_R8G8B8A8_UNORM, ns, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          W, H, &s_msColImg, &s_msColMem, &s_msColView);
    ok = ok && pp_mkImage(VK_FORMAT_D32_SFLOAT, ns, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT, W, H, &s_msDepImg, &s_msDepMem, &s_msDepView);
    if (ok) {
        VkImageView atts[3] = { s_msColView, s_msDepView, s_colView };
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = s_rpMS; fci.attachmentCount = 3; fci.pAttachments = atts;
        fci.width = (uint32_t)W; fci.height = (uint32_t)H; fci.layers = 1;
        ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbMS) == VK_SUCCESS;
    }
    if (!s_ppSSok) ss = 0;
    if (ok && !mkRasterPipes(s_rpMS, ns, ss, s_pipesMS, &s_bgPipeMS, feat)) {
        if (ss == 2) {   /* 09.10: нет dc_raster_sl.frag.spv — прежний «по выборкам» (шейдер на каждую выборку) */
            ss = 1; s_ppSLfail = 1;
            std::fprintf(stderr, "post: per-sample MSAA with lighting per pixel failed to build - the shader runs for every sample%c", 10);
            ok = mkRasterPipes(s_rpMS, ns, 1, s_pipesMS, &s_bgPipeMS, feat);
        } else ok = false;
        if (!ok && ss) { ss = 0; ok = mkRasterPipes(s_rpMS, ns, 0, s_pipesMS, &s_bgPipeMS, feat);   /* нет dc_raster_ss.frag.spv — без «по выборкам» */
                         std::fprintf(stderr, "post: per-sample MSAA failed to build - without it%c", 10); }
    }
    if (!ok) {
        rlog("[VKR] MSAA %dx: build failed\n", n);
        std::fprintf(stderr, "post: MSAA %dx failed to build - the frame goes without it%c", n, 10);
        msaaFree();
        return false;
    }
    s_msN = n; s_msSS = ss; s_msW = W; s_msH = H; s_msResolveOf = s_colView; s_featMS = feat;
    rlog("[VKR] MSAA %dx ready (%dx%d, per sample %d)\n", n, W, H, s_msSS);
    std::fprintf(stderr, "post: MSAA %dx edge smoothing%s (targets %dx%d)%c", n,
                 s_msSS == 2 ? ", per sample (lighting per pixel)" : s_msSS ? ", per sample" : "", W, H, 10);
    { char w[48]; std::snprintf(w, sizeof w, "MSAA %dx at %dx%d", n, W, H); vram_note(w); }
    return true;
}
/* 11.10.2026: the MSAA set is built in the background right after the start (msaaPrewarm, into the pipeline cache) - the
 * first frame that wants MSAA (the boot screens, the title) froze for the whole set (2.1 s with an empty driver cache on
 * an i9). Until the background build is done those frames go without MSAA (on the 2D screens it is not seen); then the
 * set comes out of the cache in milliseconds. DC_MSAA_PREWARM=0 - built at the first frame (as before). */
static std::atomic<int> s_msPrewarm{ 0 };            /* 0 none, 1 building, 2 done */
static void msaaPrewarm(int n, int ss, int feat) {   /* feat - 25.10.2026: опции в шейдере (берёт вызывающий поток) */
    const VkSampleCountFlagBits ns = (VkSampleCountFlagBits)n;
    VkAttachmentDescription a[3]{};                   /* the description of s_rpMS (msaaBuild) - a compatible pass */
    a[0].format = VK_FORMAT_R8G8B8A8_UNORM; a[0].samples = ns;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].format = VK_FORMAT_D32_SFLOAT; a[1].samples = ns;
    a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a[2].format = VK_FORMAT_R8G8B8A8_UNORM; a[2].samples = VK_SAMPLE_COUNT_1_BIT;
    a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    for (int i = 0; i < 3; i++) { a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    VkAttachmentReference colRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resRef{ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &colRef; sp.pResolveAttachments = &resRef;
    sp.pDepthStencilAttachment = &depRef;
    VkRenderPassCreateInfo rci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rci.attachmentCount = 3; rci.pAttachments = a; rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = main_pass_dep(&rci.pDependencies);
    VkRenderPass rp = VK_NULL_HANDLE;
    LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    if (vkCreateRenderPass(C->device, &rci, nullptr, &rp) == VK_SUCCESS) {
        VkPipeline p[13] = {}, bg = VK_NULL_HANDLE;
        if (mkRasterPipes(rp, ns, ss, p, &bg, feat)) {
            for (int k = 0; k < 13; k++) if (p[k]) vkDestroyPipeline(C->device, p[k], nullptr);
            if (bg) vkDestroyPipeline(C->device, bg, nullptr);
        }
        vkDestroyRenderPass(C->device, rp, nullptr);
    }
    QueryPerformanceCounter(&t1);
    std::fprintf(stderr, "post: MSAA %dx pipelines built in the background in %.0f ms%c", n,
                 (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, 10);
    s_msPrewarm = 2;
}
/* 25.10.2026: ОПЦИЯ СМЕНИЛАСЬ (лёгкие конвейеры, RasterSpec выше). Набор под новые опции - сначала в фоне в кэш конвейеров
 * (обычный и, если MSAA собран, его): кадры идут прежним набором - выключенное опцией там есть, но его ветки не идут,
 * включённое появится, когда набор готов (как MSAA при старте). Потом s_featGoal = новые опции: raster_feat_sync пересобирает
 * обычный набор из кэша, msaaEnsure - набор MSAA. Без кэша - сразу (сборка посреди кадра, как было бы). */
static std::atomic<int> s_featPrep{ 0 };             /* 0 нет, 1 строится, 2 готово (опции s_featPrepMask) */
static int s_featPrepMask = -1, s_featBad = -1;
static void feat_prewarm(int feat, int n, int ss) {
    LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    VkPipeline p[13] = {}, bg = VK_NULL_HANDLE;
    if (mkRasterPipes(s_rp, VK_SAMPLE_COUNT_1_BIT, 0, p, &bg, feat)) {
        for (int k = 0; k < 13; k++) if (p[k]) vkDestroyPipeline(C->device, p[k], nullptr);
        if (bg) vkDestroyPipeline(C->device, bg, nullptr);
    }
    if (n > 1) msaaPrewarm(n, ss, feat);
    QueryPerformanceCounter(&t1);
    std::fprintf(stderr, "raster: pipelines for shadows %s, materials %s built in the background in %.0f ms%c", (feat & 1) ? "on" : "off",
                 (feat & 2) ? "on" : "off", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, 10);
    s_featPrepMask = feat;
    s_featPrep = 2;
}
/* перед кадром (A, B, дополнительные), до msaaEnsure; GPU простаивает (рендер синхронный) */
static void raster_feat_sync(void) {
    if (s_featGoal < 0) s_featGoal = s_featPipes;
    const int want = raster_feat_want();
    if (want != s_featGoal && want != s_featBad) {
        const int st = s_featPrep.load();
        if (!s_pcache) s_featGoal = want;
        else if (st == 2 && s_featPrepMask == want) { s_featGoal = want; s_featPrep = 0; }
        else if (st != 1) {
            s_featPrep = 1;
            const int n = s_msN, ss = s_msSS;
            std::thread([want, n, ss] { feat_prewarm(want, n, ss); }).detach();
        }
    }
    if (s_featPipes == s_featGoal) return;
    vkDeviceWaitIdle(C->device);
    LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    VkPipeline np[13] = {}, nbg = VK_NULL_HANDLE;
    if (!mkRasterPipes(s_rp, VK_SAMPLE_COUNT_1_BIT, 0, np, &nbg, s_featGoal)) {
        std::fprintf(stderr, "raster: pipelines for the new options failed to build - the previous ones stay%c", 10);
        s_featBad = s_featGoal; s_featGoal = s_featPipes;
        return;
    }
    for (int k = 0; k < 13; k++) { if (s_pipes[k]) vkDestroyPipeline(C->device, s_pipes[k], nullptr); s_pipes[k] = np[k]; }
    if (s_bgPipe) vkDestroyPipeline(C->device, s_bgPipe, nullptr);
    s_bgPipe = nbg;
    s_featPipes = s_featGoal;
    QueryPerformanceCounter(&t1);
    std::fprintf(stderr, "raster: options in the shader - shadows %s, materials %s (pipelines again, %.1f ms)%c", (s_featPipes & 1) ? "on" : "off",
                 (s_featPipes & 2) ? "on" : "off", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, 10);
}
/* выборок в этом кадре (1 — без MSAA); собирает/пересобирает объекты по настройке. До записи команд кадра.
 * ⭐ 24.10.2026: ЦЕЛИ MSAA - ПО КАДРАМ С 3D (игроки: «сглаживание на встроенной видеокарте - игра и звук на 20 % скорости»).
 * Цели кадра выделены под наибольший режим PS1 (640x512 x масштаб: 3200x2560 при 1200p), а кадр мира - 320x240 (2133x1200);
 * цели MSAA были того же размера: 4x брал 250 МБ видеопамяти, 8x - 500 (замер vram:, RTX 4080). У встроенной своей памяти
 * бывает 512 МБ, а игра и без сглаживания около 430 - сверх неё система гоняет образы туда-обратно. Теперь цели MSAA - размера
 * кадров с 3D (fw x fh этого кадра; больше - растут до него), плоский экран крупнее целей (480 строк: загрузка, переходы) идёт
 * без MSAA и их не растит - на 2D-экране сглаживания не видно. DC_MSAA_FIT=0 - во всю цель, как было. */
static int msaaEnsure(int fw, int fh, int has3d) {
    pp_init();
    int want = s_ppMsaa > s_ppMsaaMax ? s_ppMsaaMax : s_ppMsaa;
    if (s_ppMsaaFail) want = 1;
    if (want <= 1) { if (s_msN > 1) msaaBuild(1, 0, 0, 0); return 1; }
    const int ss = (s_ppSshade && s_ppSSok) ? ((s_ppSloop && !s_ppSLfail) ? 2 : 1) : 0;   /* 09.10: 2 — свет на пиксель */
    /* 11.10: the first frame that wants MSAA starts its set in the background and goes without it, as do the next ones
     * until the set is in the cache (the settings of dc_user.cfg are applied by then - the set is the right one) */
    { static int pw = -1;
      if (pw < 0) { const char* e = getenv("DC_MSAA_PREWARM"); pw = (e && *e == '0') ? 0 : 1; }
      if (pw && s_pcache && s_msN <= 1) {
          if (s_msPrewarm.load() == 0) { s_msPrewarm = 1; const int feat = raster_feat_goal();
                                         std::thread([want, ss, feat] { msaaPrewarm(want, ss, feat); }).detach(); }
          if (s_msPrewarm.load() == 1) return 1;
      } }
    static int fit = -1;
    if (fit < 0) { const char* e = getenv("DC_MSAA_FIT"); fit = (e && *e == '0') ? 0 : 1; }
    const int featOk = s_featGoal < 0 || s_featMS == s_featGoal;   /* 25.10.2026: набор MSAA - под нынешние опции */
    if (!fit) {
        if (s_msN != want || s_msSS != ss || s_msW != s_fbW || s_msH != s_fbH || s_msResolveOf != s_colView || !featOk) {
            if (!msaaBuild(want, ss, s_fbW, s_fbH)) { s_ppMsaaFail = 1; return 1; }
        }
        return s_msN;
    }
    if (fw > s_fbW) fw = s_fbW;
    if (fh > s_fbH) fh = s_fbH;
    const int same = s_msN == want && s_msSS == ss && s_msResolveOf == s_colView && featOk;
    if (same && fw <= s_msW && fh <= s_msH) return s_msN;          /* кадр помещается в цели */
    if (!has3d) return 1;                                           /* плоский экран: без MSAA, цели не трогаем */
    const int tw = same ? std::max(fw, s_msW) : fw, th = same ? std::max(fh, s_msH) : fh;
    if (!msaaBuild(want, ss, tw, th)) { s_ppMsaaFail = 1; return 1; }
    return s_msN;
}

/* ---- SSAO: линейная глубина (R32F, полное разрешение) -> затенение (RG32F, половинное) -> размытие + умножение ---- */
static VkRenderPass          s_rpZ = VK_NULL_HANDLE, s_rpAO = VK_NULL_HANDLE, s_rpComp = VK_NULL_HANDLE;
static VkImage               s_zImg = VK_NULL_HANDLE, s_aoImg = VK_NULL_HANDLE;
static VkDeviceMemory        s_zMem = VK_NULL_HANDLE, s_aoMem = VK_NULL_HANDLE;
static VkImageView           s_zView = VK_NULL_HANDLE, s_aoView = VK_NULL_HANDLE;
static VkFramebuffer         s_fbZ = VK_NULL_HANDLE, s_fbAO = VK_NULL_HANDLE, s_fbComp = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_pdsl = VK_NULL_HANDLE;
static VkPipelineLayout      s_pplLay = VK_NULL_HANDLE;
static VkDescriptorPool      s_pdpool = VK_NULL_HANDLE;
static VkDescriptorSet       s_pdset = VK_NULL_HANDLE;
static VkSampler             s_pSamp = VK_NULL_HANDLE;
static VkPipeline            s_zPipe[2] = {};                     /* [0] глубина 1x, [1] MSAA (выборка 0) */
static VkPipeline            s_aoPipe = VK_NULL_HANDLE, s_compPipe = VK_NULL_HANDLE, s_compDbgPipe = VK_NULL_HANDLE;
static int                   s_ppOk = 0;                          /* 1 — собрано, -1 — не вышло (SSAO не будет) */
static int                   s_ppW = 0, s_ppH = 0;
static VkImageView           s_ppColOf = VK_NULL_HANDLE;           /* цветная цель, под которую собраны буферы */
static VkImageView           s_ppDepBound = VK_NULL_HANDLE;        /* какая глубина сейчас в биндинге 0 */

static bool pp_mkPipe(VkShaderModule vsm, const char* fsPath, int multiply, VkRenderPass rp, VkPipeline* out) {
    VkShaderModule fsm = loadSpv(fsPath);
    if (!fsm) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (multiply) {                               /* цвет = цвет кадра * выход, альфа = 1 */
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
    gp.layout = s_pplLay; gp.renderPass = rp; gp.subpass = 0;
    const bool ok = vkp_create(&gp, out, "post") == VK_SUCCESS;
    vkDestroyShaderModule(C->device, fsm, nullptr);
    if (!ok) rlog("[VKR] post pipeline %s FAILED\n", fsPath);
    return ok;
}
static bool pp_mkPass(VkFormat fmt, int load, VkImageLayout initial, VkImageLayout fin,
                      VkPipelineStageFlags inSrc, VkPipelineStageFlags outDst, VkAccessFlags outAcc, VkRenderPass* out) {
    VkAttachmentDescription a{};
    a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = (load == 2) ? VK_ATTACHMENT_LOAD_OP_CLEAR     /* 09.10: заливка при начале (рамка заслонивших) */
             : load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = initial; a.finalLayout = fin;
    VkAttachmentReference r{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &r;
    VkSubpassDependency d[2]{};
    d[0].srcSubpass = VK_SUBPASS_EXTERNAL; d[0].dstSubpass = 0;
    d[0].srcStageMask = inSrc; d[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    d[0].srcAccessMask = 0; d[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    d[1].srcSubpass = 0; d[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    d[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; d[1].dstStageMask = outDst;
    d[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; d[1].dstAccessMask = outAcc;
    VkRenderPassCreateInfo rci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rci.attachmentCount = 1; rci.pAttachments = &a; rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 2; rci.pDependencies = d;
    return vkCreateRenderPass(C->device, &rci, nullptr, out) == VK_SUCCESS;
}
/* 18.10.2026: объекты SSAO без целей (проходы, раскладка, конвейеры) - отдельно: их заранее строит экран подготовки шейдеров
 * (dcvk_prepare_all); цели зависят от размера кадра и делаются на первом кадре мира, как раньше. 1 - собраны. */
static int postObjects(void) {
    if (s_ppOk) return s_ppOk > 0;
    {
        s_ppOk = -1;
        VkDescriptorSetLayoutBinding b[4]{};
        for (int i = 0; i < 4; i++) {
            b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dci.bindingCount = 4; dci.pBindings = b;
        if (vkCreateDescriptorSetLayout(C->device, &dci, nullptr, &s_pdsl) != VK_SUCCESS) return 0;
        VkPushConstantRange pr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80 };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_pdsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(C->device, &plci, nullptr, &s_pplLay) != VK_SUCCESS) return 0;
        VkDescriptorPoolSize psz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
        if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_pdpool) != VK_SUCCESS) return 0;
        VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsa.descriptorPool = s_pdpool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &s_pdsl;
        if (vkAllocateDescriptorSets(C->device, &dsa, &s_pdset) != VK_SUCCESS) return 0;
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.f;
        if (vkCreateSampler(C->device, &sci, nullptr, &s_pSamp) != VK_SUCCESS) return 0;
        /* 01.10: цель прохода 1 — RG32F: r = ±w, g = доля затенения (ячейка «третьей энергии» гасит его там, где картинка
         * сдвинута; dc_ssao_z.frag / dc_ssao_comp.frag) */
        if (!pp_mkPass(VK_FORMAT_R32G32_SFLOAT, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpZ))
            return 0;
        if (!pp_mkPass(VK_FORMAT_R32G32_SFLOAT, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpAO))
            return 0;
        if (!pp_mkPass(VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, &s_rpComp))
            return 0;
        VkShaderModule pvs = loadSpv("dc_post.vert.spv");
        if (!pvs) { std::fprintf(stderr, "post: no shaders\\dc_post.vert.spv - no SSAO%c", 10); return 0; }
        const bool ok = pp_mkPipe(pvs, "shaders\\dc_ssao_z.frag.spv",     0, s_rpZ,    &s_zPipe[0])
                     && pp_mkPipe(pvs, "shaders\\dc_ssao_z_ms.frag.spv",  0, s_rpZ,    &s_zPipe[1])
                     && pp_mkPipe(pvs, "shaders\\dc_ssao.frag.spv",       0, s_rpAO,   &s_aoPipe)
                     && pp_mkPipe(pvs, "shaders\\dc_ssao_comp.frag.spv",  1, s_rpComp, &s_compPipe)
                     && pp_mkPipe(pvs, "shaders\\dc_ssao_comp.frag.spv",  0, s_rpComp, &s_compDbgPipe);
        vkDestroyShaderModule(C->device, pvs, nullptr);
        if (!ok) { std::fprintf(stderr, "post: SSAO pipelines failed to build - no SSAO%c", 10); return 0; }
        s_ppOk = 1;
        rlog("[VKR] SSAO ready\n");
    }
    return 1;
}
/* ⭐ 24.10.2026: ЦЕЛИ SSAO, СВЕЧЕНИЯ, ОТРАЖЕНИЯ И ДЫМКИ - ПО КАДРАМ С 3D, как цели MSAA (msaaEnsure). Были под всю цель кадра
 * (3200x2560 при 1200p: ~190 МБ видеопамяти), а кадр мира - 2133x1200. Проходы рисуют по размеру кадра (w x h, половина,
 * четверть), шейдеры берут места по gl_FragCoord и textureSize - цель крупнее кадра им не нужна. Размер растёт до самого
 * большого кадра с 3D при этом масштабе (до первого такого кадра - вся цель), новая цель кадра - заново. post_fit_note - в
 * начале кадра A и B, до postEnsure / fxEnsure / volEnsure. DC_POST_FIT=0 - во всю цель, как было. */
static int s_pfW = 0, s_pfH = 0;
static VkImageView s_pfOf = VK_NULL_HANDLE;
static void post_fit_note(int w, int h, int has3d) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_POST_FIT"); on = (e && *e == '0') ? 0 : 1; }
    if (s_pfOf != s_colView) { s_pfOf = s_colView; s_pfW = s_pfH = 0; }
    if (!on || !has3d) return;
    if (w > s_fbW) w = s_fbW;
    if (h > s_fbH) h = s_fbH;
    if (w > s_pfW) s_pfW = w;
    if (h > s_pfH) s_pfH = h;
}
static void post_fit_size(int* w, int* h) {
    const int ok = s_pfOf == s_colView && s_pfW > 0 && s_pfH > 0;
    *w = ok ? s_pfW : s_fbW; *h = ok ? s_pfH : s_fbH;
}
/* 1 — SSAO можно гнать в этом кадре (всё собрано под текущую цветную цель). До записи команд кадра. */
static int postEnsure(void) {
    if (s_ppOk < 0) return 0;
    if (!s_ppOk && !postObjects()) return 0;
    int TW, TH; post_fit_size(&TW, &TH);
    if (s_ppW != TW || s_ppH != TH || s_ppColOf != s_colView) {
        vkDeviceWaitIdle(C->device);
        if (s_fbZ)    { vkDestroyFramebuffer(C->device, s_fbZ, nullptr);    s_fbZ = VK_NULL_HANDLE; }
        if (s_fbAO)   { vkDestroyFramebuffer(C->device, s_fbAO, nullptr);   s_fbAO = VK_NULL_HANDLE; }
        if (s_fbComp) { vkDestroyFramebuffer(C->device, s_fbComp, nullptr); s_fbComp = VK_NULL_HANDLE; }
        pp_freeImage(&s_zImg, &s_zMem, &s_zView);
        pp_freeImage(&s_aoImg, &s_aoMem, &s_aoView);
        const int hw = (TW + 1) / 2, hh = (TH + 1) / 2;
        bool ok = pp_mkImage(VK_FORMAT_R32G32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,     /* 01.10: + доля затенения в g */
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             TW, TH, &s_zImg, &s_zMem, &s_zView)
               && pp_mkImage(VK_FORMAT_R32G32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                             hw, hh, &s_aoImg, &s_aoMem, &s_aoView);
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.attachmentCount = 1; fci.layers = 1;
        if (ok) { fci.renderPass = s_rpZ; fci.pAttachments = &s_zView; fci.width = (uint32_t)TW; fci.height = (uint32_t)TH;
                  ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbZ) == VK_SUCCESS; }
        if (ok) { fci.renderPass = s_rpAO; fci.pAttachments = &s_aoView; fci.width = (uint32_t)hw; fci.height = (uint32_t)hh;
                  ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbAO) == VK_SUCCESS; }
        if (ok) { fci.renderPass = s_rpComp; fci.pAttachments = &s_colView; fci.width = (uint32_t)s_fbW; fci.height = (uint32_t)s_fbH;
                  ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbComp) == VK_SUCCESS; }
        if (!ok) { s_ppOk = -1; std::fprintf(stderr, "post: SSAO buffers not created - no SSAO%c", 10); return 0; }
        VkDescriptorImageInfo ii[3] = { { s_pSamp, s_colView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { s_pSamp, s_aoView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { s_pSamp, s_zView,   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet wd[3]{};
        for (int i = 0; i < 3; i++) {
            wd[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[i].dstSet = s_pdset; wd[i].dstBinding = (uint32_t)(1 + i);
            wd[i].descriptorCount = 1; wd[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(C->device, 3, wd, 0, nullptr);
        s_ppDepBound = VK_NULL_HANDLE;
        s_ppW = TW; s_ppH = TH; s_ppColOf = s_colView;
        { char wn[48]; std::snprintf(wn, sizeof wn, "SSAO at %dx%d", TW, TH); vram_note(wn); }
    }
    return 1;
}
/* после прохода кадра: s_colImg в TRANSFER_SRC, глубина в DEPTH_STENCIL_ATTACHMENT (проход с сохранением глубины) */
static void pp_run(int w, int h, int msN, const float* view, const float* wp) {
    const VkImage     depImg  = (msN > 1) ? s_msDepImg  : s_depImg;
    const VkImageView depView = (msN > 1) ? s_msDepView : s_depView;
    if (s_ppDepBound != depView) {
        VkDescriptorImageInfo di{ s_pSamp, depView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = s_pdset; wd.dstBinding = 0; wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd.pImageInfo = &di;
        vkUpdateDescriptorSets(C->device, 1, &wd, 0, nullptr);
        s_ppDepBound = depView;
    }
    VkImageMemoryBarrier b[2]{};
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[0].image = depImg; b[0].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[1].image = s_colImg; b[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    const float pcs[20] = { view[0], view[1], view[2], view[3], wp[0], wp[1], wp[2], wp[3],
                            (float)w, (float)h, 1.f / (float)w, 1.f / (float)h,
                            s_pp[PP_RADIUS], s_pp[PP_STRENGTH], s_pp[PP_BIAS], s_pp[PP_SAMPLES],
                            s_pp[PP_MAXPX], s_pp[PP_BLUR], (float)s_ppDbg, 0.f };
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    auto pass = [&](VkRenderPass rp, VkFramebuffer fb, int pw, int ph, VkPipeline pipe) {
        const VkViewport vpt{ 0.f, 0.f, (float)pw, (float)ph, 0.f, 1.f };
        const VkRect2D sc{ { 0, 0 }, { (uint32_t)pw, (uint32_t)ph } };
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = rp; rb.framebuffer = fb; rb.renderArea = sc;
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pplLay, 0, 1, &s_pdset, 0, nullptr);
        vkCmdPushConstants(s_cmd, s_pplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
        vkCmdDraw(s_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(s_cmd);
    };
    pass(s_rpZ, s_fbZ, w, h, s_zPipe[(msN > 1) ? 1 : 0]);         /* 1: линейная глубина + маска */
    pp_ts(10);                                                     /* 09.10 (2): DC_VK_STAGES */
    pass(s_rpAO, s_fbAO, hw, hh, s_aoPipe);                        /* 2: затенение, половинная сетка */
    pp_ts(11);
    VkImageMemoryBarrier c{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    c.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; c.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    c.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    c.image = s_colImg; c.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    c.srcQueueFamilyIndex = c.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &c);
    pass(s_rpComp, s_fbComp, w, h, s_ppDbg ? s_compDbgPipe : s_compPipe);   /* 3: размытие + умножение кадра */
    pp_ts(12);
}
/* 27.09: только проход 1 (линейная глубина + маска) — для дымки у ламп, когда SSAO выключен (меню GRAPHICS -> Ambient
 * Occlusion Off). Цветная цель — обратно в TRANSFER_SRC_OPTIMAL (дальше свечение и дымка); альфу = 1 вернёт сложение дымки. */
static void pp_zonly(int w, int h, int msN, const float* view, const float* wp) {
    const VkImage     depImg  = (msN > 1) ? s_msDepImg  : s_depImg;
    const VkImageView depView = (msN > 1) ? s_msDepView : s_depView;
    if (s_ppDepBound != depView) {
        VkDescriptorImageInfo di{ s_pSamp, depView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = s_pdset; wd.dstBinding = 0; wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd.pImageInfo = &di;
        vkUpdateDescriptorSets(C->device, 1, &wd, 0, nullptr);
        s_ppDepBound = depView;
    }
    VkImageMemoryBarrier b[2]{};
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; b[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[0].image = depImg; b[0].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b[1].image = s_colImg; b[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    const float pcs[20] = { view[0], view[1], view[2], view[3], wp[0], wp[1], wp[2], wp[3],
                            (float)w, (float)h, 1.f / (float)w, 1.f / (float)h,
                            s_pp[PP_RADIUS], s_pp[PP_STRENGTH], s_pp[PP_BIAS], s_pp[PP_SAMPLES],
                            s_pp[PP_MAXPX], s_pp[PP_BLUR], 0.f, 0.f };
    const VkViewport vpt{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    const VkRect2D sc{ { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = s_rpZ; rb.framebuffer = s_fbZ; rb.renderArea = sc;
    vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_zPipe[(msN > 1) ? 1 : 0]);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pplLay, 0, 1, &s_pdset, 0, nullptr);
    vkCmdPushConstants(s_cmd, s_pplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s_cmd);
    VkImageMemoryBarrier c{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    c.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; c.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    c.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; c.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    c.image = s_colImg; c.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    c.srcQueueFamilyIndex = c.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &c);
}

/* ===== СГЛАЖИВАНИЕ FXAA / SMAA / CMAA2 (25.10.2026) =====================================================================
 * Игрок: «добавь SMAA, SMAA2, FXAA. Как дополнительные опции» (SMAA2 - это CMAA2): меню GRAPHICS -> Anti-Aliasing, рядом
 * с MSAA 2x/4x/8x. Фильтры готовой картинки, без лишних выборок: в кадре мира с 3D без MSAA, сразу после основного прохода -
 * до SSAO, свечения и дымки. Кадр копируется в s_aaSrcImg; FXAA - один проход в цель кадра (dc_aa_fxaa.frag), SMAA и
 * CMAA2 - рёбра (R8G8), веса вдоль рёбер (RGBA8) и смешение в цель кадра (dc_aa_edge / dc_aa_weight / dc_aa_blend.frag;
 * CMAA2 - те же исходники с -DDC_CMAA=1). Интерфейс, текст и плоские экраны не трогаются: главный проход пишет маску
 * «3D / поверх плоское» в альфу, как для SSAO (dc_raster.frag aoMaskOut). Маска остаётся в альфе, если дальше SSAO или
 * дымка (им она нужна), иначе альфа = 1. Объекты режима строятся на первом его кадре (или на экране подготовки шейдеров),
 * цели - по кадрам с 3D (post_fit_size), рёбра и веса - только у SMAA и CMAA2. Не вышло - кадры без сглаживания
 * (s_ppAAFail, до новой настройки). DC_POST_AA=<0..3> - режим без меню (0 - нет, 1 - FXAA, 2 - SMAA, 3 - CMAA2).
 * 25.10.2026 вечер (замер: SMAA и CMAA2 стоили как MSAA 2x): все проходы пишут только пиксели у рёбер (discard) - рёбра и веса
 * заливаются нулём в начале прохода, смешение и FXAA загружают цель кадра, где тот же кадр. Картинка та же. */
static VkRenderPass     s_aaRpE = VK_NULL_HANDLE, s_aaRpW = VK_NULL_HANDLE, s_aaRpOut = VK_NULL_HANDLE;
static VkSampler        s_aaSamp = VK_NULL_HANDLE;
static VkDescriptorPool s_aaPool = VK_NULL_HANDLE;
static VkDescriptorSet  s_aaSet[3] = {};                  /* [0] копия кадра (FXAA, рёбра), [1] + рёбра, [2] + веса */
static VkPipeline       s_aaPipe[4][3] = {};              /* [режим][проход]: FXAA - [1][0]; SMAA, CMAA2 - рёбра, веса, смешение */
static int              s_aaBase = 0, s_aaMode[4] = {};   /* 1 - собрано, -1 - не вышло */
static VkImage          s_aaSrcImg = VK_NULL_HANDLE, s_aaEImg = VK_NULL_HANDLE, s_aaWImg = VK_NULL_HANDLE;
static VkDeviceMemory   s_aaSrcMem = VK_NULL_HANDLE, s_aaEMem = VK_NULL_HANDLE, s_aaWMem = VK_NULL_HANDLE;
static VkImageView      s_aaSrcView = VK_NULL_HANDLE, s_aaEView = VK_NULL_HANDLE, s_aaWView = VK_NULL_HANDLE;
static VkFramebuffer    s_aaFbE = VK_NULL_HANDLE, s_aaFbW = VK_NULL_HANDLE, s_aaFbOut = VK_NULL_HANDLE;
static int              s_aaTW = 0, s_aaTH = 0, s_aaEW = 0;   /* размер целей; есть ли цели рёбер и весов */
static int              s_aaFW = 0, s_aaFH = 0;               /* размер цели кадра под s_aaFbOut */
static VkImageView      s_aaColOf = VK_NULL_HANDLE;           /* цель кадра, под которую собраны буферы */

static void aa_free_targets(void) {
    if (s_aaFbE)   { vkDestroyFramebuffer(C->device, s_aaFbE, nullptr);   s_aaFbE = VK_NULL_HANDLE; }
    if (s_aaFbW)   { vkDestroyFramebuffer(C->device, s_aaFbW, nullptr);   s_aaFbW = VK_NULL_HANDLE; }
    if (s_aaFbOut) { vkDestroyFramebuffer(C->device, s_aaFbOut, nullptr); s_aaFbOut = VK_NULL_HANDLE; }
    pp_freeImage(&s_aaSrcImg, &s_aaSrcMem, &s_aaSrcView);
    pp_freeImage(&s_aaEImg, &s_aaEMem, &s_aaEView);
    pp_freeImage(&s_aaWImg, &s_aaWMem, &s_aaWView);
    s_aaTW = s_aaTH = 0; s_aaEW = 0; s_aaFW = s_aaFH = 0; s_aaColOf = VK_NULL_HANDLE;
}
/* проходы, наборы и конвейеры режима mode (1..3). 1 - собраны */
static int aaObjects(int mode) {
    if (mode < 1 || mode > 3) return 0;
    if (s_aaMode[mode]) return s_aaMode[mode] > 0;
    if (s_aaBase < 0) return 0;
    if (!s_aaBase) {
        s_aaBase = -1;
        if (!postObjects()) { std::fprintf(stderr, "post: no post-processing objects - no AA filter%c", 10); return 0; }
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;   /* билинейные выборки FXAA и смешения */
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.f;
        if (vkCreateSampler(C->device, &sci, nullptr, &s_aaSamp) != VK_SUCCESS) return 0;
        VkDescriptorPoolSize psz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12 };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 3; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
        if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_aaPool) != VK_SUCCESS) return 0;
        const VkDescriptorSetLayout ls[3] = { s_pdsl, s_pdsl, s_pdsl };
        VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsa.descriptorPool = s_aaPool; dsa.descriptorSetCount = 3; dsa.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(C->device, &dsa, s_aaSet) != VK_SUCCESS) return 0;
        /* рёбра и веса живут один кадр; смешение и FXAA пишут в цель кадра (TRANSFER_SRC до и после - как после прохода).
         * 25.10 вечер: рёбра и веса заливаются нулём в начале прохода, шейдеры пишут только пиксели у рёбер (discard) */
        if (!pp_mkPass(VK_FORMAT_R8G8_UNORM, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_aaRpE))
            return 0;
        if (!pp_mkPass(VK_FORMAT_R8G8B8A8_UNORM, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_aaRpW))
            return 0;
        if (!pp_mkPass(VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_READ_BIT, &s_aaRpOut))
            return 0;
        s_aaBase = 1;
    }
    s_aaMode[mode] = -1;
    VkShaderModule pvs = loadSpv("dc_post.vert.spv");
    if (!pvs) return 0;
    bool ok;
    if (mode == 1)
        ok = pp_mkPipe(pvs, "shaders\\dc_aa_fxaa.frag.spv", 0, s_aaRpOut, &s_aaPipe[1][0]);
    else if (mode == 2)
        ok = pp_mkPipe(pvs, "shaders\\dc_aa_smaa_edge.frag.spv",   0, s_aaRpE,   &s_aaPipe[2][0])
          && pp_mkPipe(pvs, "shaders\\dc_aa_smaa_weight.frag.spv", 0, s_aaRpW,   &s_aaPipe[2][1])
          && pp_mkPipe(pvs, "shaders\\dc_aa_smaa_blend.frag.spv",  0, s_aaRpOut, &s_aaPipe[2][2]);
    else
        ok = pp_mkPipe(pvs, "shaders\\dc_aa_cmaa_edge.frag.spv",   0, s_aaRpE,   &s_aaPipe[3][0])
          && pp_mkPipe(pvs, "shaders\\dc_aa_cmaa_weight.frag.spv", 0, s_aaRpW,   &s_aaPipe[3][1])
          && pp_mkPipe(pvs, "shaders\\dc_aa_cmaa_blend.frag.spv",  0, s_aaRpOut, &s_aaPipe[3][2]);
    vkDestroyShaderModule(C->device, pvs, nullptr);
    if (!ok) { std::fprintf(stderr, "post: %s pipelines failed to build - no AA filter%c", PP_AA_NAME[mode], 10); return 0; }
    s_aaMode[mode] = 1;
    rlog("[VKR] AA filter %s ready\n", PP_AA_NAME[mode]);
    return 1;
}
/* 1 - сглаживание mode можно гнать в кадре w x h (объекты и цели собраны под текущую цель кадра). До записи команд кадра. */
static int aaEnsure(int mode, int w, int h) {
    if (!aaObjects(mode)) return 0;
    int TW, TH; post_fit_size(&TW, &TH);
    const int ew = mode >= 2;
    if (s_aaTW != TW || s_aaTH != TH || s_aaColOf != s_colView || s_aaFW != s_fbW || s_aaFH != s_fbH || s_aaEW < ew) {
        vkDeviceWaitIdle(C->device);
        aa_free_targets();
        const VkImageUsageFlags rt = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        bool ok = pp_mkImage(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, TW, TH, &s_aaSrcImg, &s_aaSrcMem, &s_aaSrcView);
        if (ok && ew)
            ok = pp_mkImage(VK_FORMAT_R8G8_UNORM, VK_SAMPLE_COUNT_1_BIT, rt, VK_IMAGE_ASPECT_COLOR_BIT, TW, TH, &s_aaEImg, &s_aaEMem, &s_aaEView)
              && pp_mkImage(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, rt, VK_IMAGE_ASPECT_COLOR_BIT, TW, TH, &s_aaWImg, &s_aaWMem, &s_aaWView);
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.attachmentCount = 1; fci.layers = 1;
        if (ok) { fci.renderPass = s_aaRpOut; fci.pAttachments = &s_colView; fci.width = (uint32_t)s_fbW; fci.height = (uint32_t)s_fbH;
                  ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_aaFbOut) == VK_SUCCESS; }
        if (ok && ew) { fci.renderPass = s_aaRpE; fci.pAttachments = &s_aaEView; fci.width = (uint32_t)TW; fci.height = (uint32_t)TH;
                        ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_aaFbE) == VK_SUCCESS; }
        if (ok && ew) { fci.renderPass = s_aaRpW; fci.pAttachments = &s_aaWView;
                        ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_aaFbW) == VK_SUCCESS; }
        if (!ok) {
            aa_free_targets(); s_ppAAFail = 1;
            std::fprintf(stderr, "post: AA filter buffers not created - no AA filter%c", 10);
            return 0;
        }
        /* наборы: ни один не ссылается на цель, в которую пишет его проход */
        const VkImageView sv = s_aaSrcView, ev = ew ? s_aaEView : sv, wv = ew ? s_aaWView : sv;
        const VkImageView vs[3][4] = { { sv, sv, sv, sv }, { sv, ev, sv, sv }, { sv, ev, wv, sv } };
        VkDescriptorImageInfo ii[12]; VkWriteDescriptorSet wd[12]{};
        for (int i = 0; i < 12; i++) {
            ii[i] = { s_aaSamp, vs[i / 4][i % 4], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wd[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[i].dstSet = s_aaSet[i / 4]; wd[i].dstBinding = (uint32_t)(i % 4);
            wd[i].descriptorCount = 1; wd[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[i].pImageInfo = &ii[i];
        }
        vkUpdateDescriptorSets(C->device, 12, wd, 0, nullptr);
        s_aaTW = TW; s_aaTH = TH; s_aaEW = ew; s_aaFW = s_fbW; s_aaFH = s_fbH; s_aaColOf = s_colView;
        { char wn[48]; std::snprintf(wn, sizeof wn, "AA filter at %dx%d", TW, TH); vram_note(wn); }
    }
    return w <= s_aaTW && h <= s_aaTH;
}
/* режим сглаживания этого кадра (0 - без него): выбран, кадр мира с 3D без MSAA. Выключили - цели освобождаются.
 * До записи команд кадра; aa_run - с тем же режимом */
static int aa_frame(int msN, int world, int w, int h) {
    pp_init();
    const int mode = s_ppAA;
    if (!mode || s_ppAAFail) {
        if (s_aaTW) { vkDeviceWaitIdle(C->device); aa_free_targets(); }
        return 0;
    }
    return (msN == 1 && world && aaEnsure(mode, w, h)) ? mode : 0;
}
/* после основного прохода: s_colImg в TRANSFER_SRC и остаётся в ней. keepAlpha - маска остаётся в альфе (дальше SSAO, дымка) */
static void aa_run(int mode, int w, int h, int keepAlpha) {
    VkImageMemoryBarrier b[2]{};
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    b[0].oldLayout = b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;            /* записи прохода - до копии */
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].image = s_colImg;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].srcAccessMask = 0; b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].image = s_aaSrcImg;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cp.extent = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyImage(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_aaSrcImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b[1]);
    const float pcs[8] = { (float)w, (float)h, 1.f / (float)s_aaTW, 1.f / (float)s_aaTH, keepAlpha ? 1.f : 0.f, 0.f, 0.f, 0.f };
    const VkViewport vpt{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    const VkRect2D sc{ { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    const VkClearValue zero{};                                   /* заливка рёбер и весов (у прохода в цель кадра - не нужна) */
    auto pass = [&](VkRenderPass rp, VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set) {
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = rp; rb.framebuffer = fb; rb.renderArea = sc;
        rb.clearValueCount = 1; rb.pClearValues = &zero;
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pplLay, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(s_cmd, s_pplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
        vkCmdDraw(s_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(s_cmd);
    };
    if (mode == 1) { pass(s_aaRpOut, s_aaFbOut, s_aaPipe[1][0], s_aaSet[0]); return; }
    pass(s_aaRpE,   s_aaFbE,   s_aaPipe[mode][0], s_aaSet[0]);   /* рёбра */
    pass(s_aaRpW,   s_aaFbW,   s_aaPipe[mode][1], s_aaSet[1]);   /* веса вдоль рёбер */
    pass(s_aaRpOut, s_aaFbOut, s_aaPipe[mode][2], s_aaSet[2]);   /* смешение - в цель кадра */
}


/* ===== СВЕЧЕНИЕ ИСТОЧНИКОВ И ОТРАЖЕНИЯ НА ПОЛУ (25.09.2026 вечер) ========================================================
 * Игрок: «Свечение только самих источников: лазеров, ламп, экранов, зелёных панелей. Не свечение поверх кадра, от которого
 * вы отказались» и «Отражения на полу: на кафеле и металле отражались бы лазеры, лампы и Регина. Какие текстуры блестят,
 * можно выбирать в приложении, как с двусторонними текстурами».
 * Оба эффекта — отдельные проходы по тем же draw кадра в свои цели половинного разрешения (dc_fx.vert + dc_raster.frag,
 * собранный с DC_FX_PASS = dc_fx.frag.spv; вид прохода — pc.prevMap.w):
 *  * СВЕЧЕНИЕ (1): вся непрозрачная геометрия пишет глубину и чёрный цвет, своим цветом — только текстуры «светится» (бит 22
 *    страницы в вершине, gpu.c dc_tex_class) и сложением — полупрозрачные примитивы игры в режимах «сложение» (лазеры,
 *    ореолы ламп, вспышки). Ореол поэтому только у ВИДИМОЙ части источника (за колонной не светит). Потом два размытия
 *    (половинное и четвертное разрешение, dc_glow_blur.frag) и сложение с готовым кадром (dc_glow_comp.frag) — после SSAO.
 *  * ОТРАЖЕНИЕ (2): те же треугольники, отражённые плоскостью пола (на уровне ног, pt_renderer.c dc_refl_frame): персонажи
 *    и «светится» — своим цветом (альфа 1), прочая геометрия выше пола заслоняет (альфа 0, цвет 0), пол и всё ниже него
 *    отбрасываются; сложением — те же светящиеся полупрозрачные. Основной проход на полу из списка «блестит» (бит 21
 *    страницы) берёт эту картинку в своей точке экрана (биндинг 5): размытие, спад к краю допуска по высоте.
 * Кадр B (DC_FRAME_WARP) делает оба прохода своей проекцией: отражение — в пространстве кадра A, потом перепроецирование.
 * Параметры — группа 2 таблицы «Картинки» (F8 «Свечение и отражения», dc_post_user.cfg). В родном режиме (O) — нет. */
static VkRenderPass          s_rpFX = VK_NULL_HANDLE, s_rpGB = VK_NULL_HANDLE, s_rpGC = VK_NULL_HANDLE;
static VkPipeline            s_fxPipe[2] = {};              /* 0 — непрозрачный с записью глубины, 1 — сложение без записи */
static VkPipeline            s_gBlurPipe = VK_NULL_HANDLE, s_gCompPipe = VK_NULL_HANDLE, s_gCompDbgPipe = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_gdsl = VK_NULL_HANDLE;
static VkPipelineLayout      s_gplLay = VK_NULL_HANDLE;
static VkDescriptorPool      s_gdpool = VK_NULL_HANDLE;
static VkDescriptorSet       s_gset[5] = {};                /* размытия: E, T, E, Q; сложение: G1 + G2 + R */
static VkSampler             s_fxSamp = VK_NULL_HANDLE;     /* линейный, с краем */
enum { FXI_E = 0, FXI_R, FXI_T, FXI_G1, FXI_Q, FXI_G2, FXI_N };
static VkImage               s_fxImg[FXI_N] = {};
static VkDeviceMemory        s_fxMem[FXI_N] = {};
static VkImageView           s_fxView[FXI_N] = {};
static VkImage               s_fxDep = VK_NULL_HANDLE;
static VkDeviceMemory        s_fxDepMem = VK_NULL_HANDLE;
static VkImageView           s_fxDepView = VK_NULL_HANDLE;
static VkFramebuffer         s_fbFXe = VK_NULL_HANDLE, s_fbFXr = VK_NULL_HANDLE, s_fbT = VK_NULL_HANDLE, s_fbG1 = VK_NULL_HANDLE,
                             s_fbQ = VK_NULL_HANDLE, s_fbG2 = VK_NULL_HANDLE, s_fbGC = VK_NULL_HANDLE;
static int                   s_fxOk = 0;                    /* 1 — собрано, -1 — не вышло (эффектов не будет) */
static int                   s_fxW = 0, s_fxH = 0;
static VkImageView           s_fxColOf = VK_NULL_HANDLE;
static int                   s_fxFresh = 0;                 /* новые образы — перевести в SHADER_READ_ONLY на ближайшем кадре */
static float                 s_rfPlane[4] = { 0.f, 0.f, 0.f, 0.f };
static int                   s_rfPlaneOk = 0;
static int                   s_fxGlowFrame = 0, s_fxReflFrame = 0;   /* что было в последнем кадре (для панели) */
/* pt_renderer.c dc_refl_frame: плоскость пола в видовом пространстве кадра (нормаль к камере, n·p = d) */
extern "C" void dcvk_refl_plane(const float* n, float d, int ok) {
    s_rfPlaneOk = (ok && n) ? 1 : 0;
    if (s_rfPlaneOk) { s_rfPlane[0] = n[0]; s_rfPlane[1] = n[1]; s_rfPlane[2] = n[2]; s_rfPlane[3] = d; }
}
extern "C" void dcvk_fx_stats(int* glow, int* refl, int* ok) { *glow = s_fxGlowFrame; *refl = s_fxReflFrame; *ok = s_fxOk; }

static bool fx_mkGeomPipe(VkShaderModule vs, VkShaderModule fs, int additive, VkPipeline* out) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    st[1].pSpecializationInfo = &s_repSpec;
    /* 26.10.2026: привязка 1 - метка куска s_pvBuf (пары кусков, dc_fx.vert) */
    VkVertexInputBindingDescription vb[2]{ { 0, VERT_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX },
                                           { 1, sizeof(uint32_t), VK_VERTEX_INPUT_RATE_VERTEX } };
    VkVertexInputAttributeDescription va[6]{
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       8 },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
        { 4, 0, VK_FORMAT_R32_SFLOAT,          48 },
        { 6, 1, VK_FORMAT_R32_UINT,            0 },
    };
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 2; vin.pVertexBindingDescriptions = vb;
    vin.vertexAttributeDescriptionCount = 6; vin.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = additive ? VK_FALSE : VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;     /* глубина K/w: ближе — больше, как в растре */
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (additive) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
    gp.layout = s_plLay; gp.renderPass = s_rpFX; gp.subpass = 0;
    return vkp_create(&gp, out, "fx geometry") == VK_SUCCESS;
}
/* полноэкранный проход свечения: blend 0 — без смешения, 2 — сложение (альфа цели не трогается) */
static bool fx_mkPost(VkShaderModule vsm, const char* fsPath, int blend, VkRenderPass rp, VkPipeline* out) {
    VkShaderModule fsm = loadSpv(fsPath);
    if (!fsm) return false;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (blend == 2) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
    gp.layout = s_gplLay; gp.renderPass = rp; gp.subpass = 0;
    const bool ok = vkp_create(&gp, out, "post") == VK_SUCCESS;
    vkDestroyShaderModule(C->device, fsm, nullptr);
    if (!ok) rlog("[VKR] fx post pipeline %s FAILED\n", fsPath);
    return ok;
}
static void fx_freeTargets(void) {
    VkFramebuffer* fbs[7] = { &s_fbFXe, &s_fbFXr, &s_fbT, &s_fbG1, &s_fbQ, &s_fbG2, &s_fbGC };
    for (int i = 0; i < 7; i++) if (*fbs[i]) { vkDestroyFramebuffer(C->device, *fbs[i], nullptr); *fbs[i] = VK_NULL_HANDLE; }
    for (int i = 0; i < FXI_N; i++) pp_freeImage(&s_fxImg[i], &s_fxMem[i], &s_fxView[i]);
    pp_freeImage(&s_fxDep, &s_fxDepMem, &s_fxDepView);
}
/* 18.10.2026: объекты свечения и отражения без целей (проходы, раскладка, два больших конвейера dc_fx и проходы размытия) -
 * отдельно: их заранее строит экран подготовки шейдеров (dcvk_prepare_all). Прежде первая комната с луной, лампой с тенью
 * или блестящим полом строила их посреди кадра (луна и тени без зерна зовут fxEnsure) - первая сцена на движке. 1 - собраны. */
static int fxObjects(void) {
    if (s_fxOk) return s_fxOk > 0;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    {
        s_fxOk = -1;
        {   VkAttachmentDescription a[2]{};
            a[0].format = HF; a[0].samples = VK_SAMPLE_COUNT_1_BIT;
            a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            a[1].format = VK_FORMAT_D32_SFLOAT; a[1].samples = VK_SAMPLE_COUNT_1_BIT;
            a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            for (int i = 0; i < 2; i++) { a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
            VkAttachmentReference cr{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dr{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription sp{};
            sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sp.colorAttachmentCount = 1; sp.pColorAttachments = &cr; sp.pDepthStencilAttachment = &dr;
            VkSubpassDependency d[2]{};
            d[0].srcSubpass = VK_SUBPASS_EXTERNAL; d[0].dstSubpass = 0;
            d[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            d[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            d[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            d[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            d[1].srcSubpass = 0; d[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            d[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; d[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            d[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; d[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo rci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            rci.attachmentCount = 2; rci.pAttachments = a; rci.subpassCount = 1; rci.pSubpasses = &sp;
            rci.dependencyCount = 2; rci.pDependencies = d;
            if (vkCreateRenderPass(C->device, &rci, nullptr, &s_rpFX) != VK_SUCCESS) return 0; }
        if (!pp_mkPass(HF, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpGB))
            return 0;
        if (!pp_mkPass(VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, &s_rpGC))
            return 0;
        {   VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = 0.f;
            if (vkCreateSampler(C->device, &sci, nullptr, &s_fxSamp) != VK_SUCCESS) return 0; }
        {   VkDescriptorSetLayoutBinding b[3]{};
            for (int i = 0; i < 3; i++) {
                b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dci.bindingCount = 3; dci.pBindings = b;
            if (vkCreateDescriptorSetLayout(C->device, &dci, nullptr, &s_gdsl) != VK_SUCCESS) return 0;
            VkPushConstantRange pr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };   /* 28.09 (6): было 32 — сложению свечения нужны полосы пелены */
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1; plci.pSetLayouts = &s_gdsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pr;
            if (vkCreatePipelineLayout(C->device, &plci, nullptr, &s_gplLay) != VK_SUCCESS) return 0;
            VkDescriptorPoolSize psz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 15 };
            VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpci.maxSets = 5; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
            if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_gdpool) != VK_SUCCESS) return 0;
            VkDescriptorSetLayout ls[5] = { s_gdsl, s_gdsl, s_gdsl, s_gdsl, s_gdsl };
            VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsa.descriptorPool = s_gdpool; dsa.descriptorSetCount = 5; dsa.pSetLayouts = ls;
            if (vkAllocateDescriptorSets(C->device, &dsa, s_gset) != VK_SUCCESS) return 0; }
        {   VkShaderModule fvs = loadSpv("dc_fx.vert.spv"), ffs = loadSpv(fx_fs_name());
            bool r2[2] = { false, false };              /* 11.10: the two side by side (vkp_par) */
            if (fvs && ffs) vkp_par(2, [&](int i) { r2[i] = fx_mkGeomPipe(fvs, ffs, i, &s_fxPipe[i]); });
            const bool ok = fvs && ffs && r2[0] && r2[1];
            if (fvs) vkDestroyShaderModule(C->device, fvs, nullptr);
            if (ffs) vkDestroyShaderModule(C->device, ffs, nullptr);
            if (!ok) { std::fprintf(stderr, "fx: no shaders\\dc_fx.vert.spv / dc_fx.frag.spv or the pipeline failed to build - no glow and no reflections%c", 10); return 0; } }
        {   VkShaderModule pvs = loadSpv("dc_post.vert.spv");
            const bool ok = pvs && fx_mkPost(pvs, "shaders\\dc_glow_blur.frag.spv", 0, s_rpGB, &s_gBlurPipe)
                         && fx_mkPost(pvs, "shaders\\dc_glow_comp.frag.spv", 2, s_rpGC, &s_gCompPipe)
                         && fx_mkPost(pvs, "shaders\\dc_glow_comp.frag.spv", 0, s_rpGC, &s_gCompDbgPipe);
            if (pvs) vkDestroyShaderModule(C->device, pvs, nullptr);
            if (!ok) { std::fprintf(stderr, "fx: glow blur/add pipelines failed to build - no effects%c", 10); return 0; } }
        s_fxOk = 1;
        rlog("[VKR] fx (glow + reflections) ready\n");
    }
    return 1;
}
/* 1 — проходы свечения/отражения можно писать в этом кадре. До записи команд кадра (пересборка ждёт простоя GPU). */
static int fxEnsure(void) {
    if (s_fxOk < 0) return 0;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!s_fxOk && !fxObjects()) return 0;
    int TW, TH; post_fit_size(&TW, &TH);                /* 24.10.2026: по кадрам с 3D (post_fit_note) */
    if (s_fxW != TW || s_fxH != TH || s_fxColOf != s_colView) {
        vkDeviceWaitIdle(C->device);
        fx_freeTargets();
        const int hw = (TW + 1) / 2, hh = (TH + 1) / 2, qw = (hw + 1) / 2, qh = (hh + 1) / 2;
        const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        bool ok = true;
        for (int i = 0; i < FXI_N && ok; i++) {
            const int q = (i == FXI_Q || i == FXI_G2);
            ok = pp_mkImage(HF, VK_SAMPLE_COUNT_1_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, q ? qw : hw, q ? qh : hh,
                            &s_fxImg[i], &s_fxMem[i], &s_fxView[i]);
        }
        ok = ok && pp_mkImage(VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT, hw, hh, &s_fxDep, &s_fxDepMem, &s_fxDepView);
        auto mkfb = [&](VkRenderPass rp, VkImageView a0, VkImageView a1, int fw, int fh, VkFramebuffer* out) {
            VkImageView at[2] = { a0, a1 };
            VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fci.renderPass = rp; fci.attachmentCount = a1 ? 2 : 1; fci.pAttachments = at;
            fci.width = (uint32_t)fw; fci.height = (uint32_t)fh; fci.layers = 1;
            return vkCreateFramebuffer(C->device, &fci, nullptr, out) == VK_SUCCESS;
        };
        ok = ok && mkfb(s_rpFX, s_fxView[FXI_E], s_fxDepView, hw, hh, &s_fbFXe)
                && mkfb(s_rpFX, s_fxView[FXI_R], s_fxDepView, hw, hh, &s_fbFXr)
                && mkfb(s_rpGB, s_fxView[FXI_T], VK_NULL_HANDLE, hw, hh, &s_fbT)
                && mkfb(s_rpGB, s_fxView[FXI_G1], VK_NULL_HANDLE, hw, hh, &s_fbG1)
                && mkfb(s_rpGB, s_fxView[FXI_Q], VK_NULL_HANDLE, qw, qh, &s_fbQ)
                && mkfb(s_rpGB, s_fxView[FXI_G2], VK_NULL_HANDLE, qw, qh, &s_fbG2)
                && mkfb(s_rpGC, s_colView, VK_NULL_HANDLE, s_fbW, s_fbH, &s_fbGC);
        if (!ok) { fx_freeTargets(); s_fxOk = -1; std::fprintf(stderr, "fx: glow/reflection targets not created - no effects%c", 10); return 0; }
        {   const int src[5][3] = { { FXI_E, -1, -1 }, { FXI_T, -1, -1 }, { FXI_E, -1, -1 }, { FXI_Q, -1, -1 }, { FXI_G1, FXI_G2, FXI_R } };
            VkDescriptorImageInfo ii[15]; VkWriteDescriptorSet wd[15]{}; int nw = 0;
            for (int s = 0; s < 5; s++)
                for (int b = 0; b < 3; b++) {
                    if (src[s][b] < 0) continue;
                    ii[nw] = { s_fxSamp, s_fxView[src[s][b]], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    wd[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[nw].dstSet = s_gset[s]; wd[nw].dstBinding = (uint32_t)b;
                    wd[nw].descriptorCount = 1; wd[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[nw].pImageInfo = &ii[nw];
                    nw++;
                }
            ii[nw] = { s_fxSamp, s_fxView[FXI_R], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };      /* растр: биндинг 5 */
            wd[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[nw].dstSet = s_dset; wd[nw].dstBinding = 5;
            wd[nw].descriptorCount = 1; wd[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[nw].pImageInfo = &ii[nw];
            nw++;
            vkUpdateDescriptorSets(C->device, (uint32_t)nw, wd, 0, nullptr); }
        s_fxFresh = 1;
        s_fxW = TW; s_fxH = TH; s_fxColOf = s_colView;
        std::fprintf(stderr, "fx: glow and reflection targets %dx%d / %dx%d%c", hw, hh, qw, qh, 10);
    }
    return 1;
}
/* новые образы — сразу в SHADER_READ_ONLY (их читают дескрипторы, даже если в этом кадре проход не шёл) */
static void fx_init_layouts(void) {
    if (!s_fxFresh) return;
    s_fxFresh = 0;
    VkImageMemoryBarrier b[FXI_N]{};
    for (int i = 0; i < FXI_N; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].srcAccessMask = 0; b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[i].image = s_fxImg[i]; b[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, FXI_N, b);
}
/* флаги растра (истинный цвет, фильтр, округление UV) — те же, что у основного прохода */
static float fx_pc7(void) {
    static int tc = -1, tf = -1, us = -1;
    if (tc < 0) { const char* e = getenv("DC_TRUE_COLOR"); tc = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_TEX_FILTER"); tf = (e && *e == '1') ? 1 : 0;
                  e = getenv("DC_UV_SNAP"); us = (e && *e == '0') ? 0 : 1; }
    return (float)(tc + 2 * tf + 64 * us);
}
/* проход свечения (mode 1) или отражения (mode 2) по draw кадра — до основного прохода.
 * ord == nullptr — draws по порядку с from (кадр A: порядок основного прохода здесь ещё не собран — для непрозрачного с
 * глубиной и для сложения порядок не важен); warp16 — поля wq/wt/wd/wp (кадр A — без сдвига). */
static void fx_geom_pass(int mode, const std::vector<Draw>& draws, const std::vector<size_t>* ord, size_t from,
                         int w, int h, int dispX, int dispY, int dispW, int dispH, const float* warp16,
                         VkBuffer vb = VK_NULL_HANDLE, const std::vector<MeshRef>* mrefs = nullptr,   /* 25.10: снимок - копия вершин и меши */
                         VkBuffer pv = VK_NULL_HANDLE) {                                              /* 26.10: и меток кусков */
    if (vb == VK_NULL_HANDLE) vb = s_vbuf;
    if (pv == VK_NULL_HANDLE) pv = s_pvBuf;
    const VkBuffer vbp[2] = { vb, pv }; const VkDeviceSize vbo[2] = { 0, 0 };
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    VkClearValue cl[2]{}; cl[0].color = { { 0.f, 0.f, 0.f, 0.f } }; cl[1].depthStencil = { 0.f, 0 };
    VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = s_rpFX; rb.framebuffer = (mode == 1) ? s_fbFXe : s_fbFXr;
    rb.renderArea = { { 0, 0 }, { (uint32_t)hw, (uint32_t)hh } };
    rb.clearValueCount = 2; rb.pClearValues = cl;
    vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vpt{ 0.f, 0.f, (float)hw, (float)hh, 0.f, 1.f };
    const VkRect2D scFull{ { 0, 0 }, { (uint32_t)hw, (uint32_t)hh } };
    vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &scFull);
    vkCmdBindVertexBuffers(s_cmd, 0, 2, vbp, vbo);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plLay, 0, 1, &s_dset, 0, nullptr);
    vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 64, 64, warp16);
    float pc[16] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH, 0.f, 1.f, 1.f, fx_pc7(),
                     1.f, s_pp[PP_GL_TEX] * (g_dc_gfx_materials ? 1.f : 0.f), s_pp[PP_GL_THR], s_pp[PP_GL_ATHR],  0.f, 0.f, 0.f, (float)mode };
    int bound = -1; VkRect2D scCur = scFull;
    const int clipEn = dc_drawarea_clip_on();
    long nd = 0;
    const size_t n = ord ? ord->size() : draws.size();
    for (size_t oi = ord ? 0 : from; oi < n; oi++) {
        const size_t di = ord ? (*ord)[oi] : oi;
        if (di >= draws.size()) continue;
        const Draw& d = draws[di];
        if (d.wire || d.ui || d.scap > 0) continue;
        if (d.mesh >= 0) {                                           /* 06.10.2026: меш из mods — отражается и заслоняет свечение */
            VkPipeline mp = mesh_fx_pipe();
            if (mp != VK_NULL_HANDLE && !d.clip) {
                if (scCur.offset.x != scFull.offset.x || scCur.offset.y != scFull.offset.y ||
                    scCur.extent.width != scFull.extent.width || scCur.extent.height != scFull.extent.height) {
                    vkCmdSetScissor(s_cmd, 0, 1, &scFull); scCur = scFull;
                }
                pc[4] = 0.f; pc[8] = mode == 1 ? s_pp[PP_GL_ADD] : 1.f; pc[12] = 0.f;
                vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
                medx_record(s_cmd, mp, d, mrefs ? mesh_ref_of(d, *mrefs) : nullptr);
                bound = -1;
                vkCmdBindVertexBuffers(s_cmd, 0, 2, vbp, vbo);
                nd++;
            }
            continue;
        }
        int pipe; float addk = 1.f;
        if (d.kind == 0) pipe = 0;                                   /* непрозрачное с глубиной */
        else if (d.kind == 3) pipe = 1;                              /* полупрозрачное «сложение» B + F */
        else if (d.kind == 5) { pipe = 1; addk = 0.25f; }            /* B + F/4 */
        else continue;                   /* плоское (интерфейс), «среднее» (стекло, дым) и вычитание — не светятся */
        VkRect2D scWant = scFull;
        if (d.clip && clipEn) {
            const float kx = (float)hw / (float)dispW, ky = (float)hh / (float)dispH;
            int x0 = (int)floorf((d.cr[0] - (float)dispX) * kx), x1 = (int)ceilf((d.cr[2] - (float)dispX) * kx);
            int y0 = (int)floorf((d.cr[1] - (float)dispY) * ky), y1 = (int)ceilf((d.cr[3] - (float)dispY) * ky);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > hw) x1 = hw; if (y1 > hh) y1 = hh;
            if (x1 <= x0 || y1 <= y0) continue;
            scWant.offset = { x0, y0 }; scWant.extent = { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
        }
        if (scWant.offset.x != scCur.offset.x || scWant.offset.y != scCur.offset.y ||
            scWant.extent.width != scCur.extent.width || scWant.extent.height != scCur.extent.height) {
            vkCmdSetScissor(s_cmd, 0, 1, &scWant); scCur = scWant;
        }
        if (pipe != bound) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_fxPipe[pipe]); bound = pipe; }
        pc[4] = (float)d.pass;
        pc[8] = (mode == 1 ? s_pp[PP_GL_ADD] : 1.f) * addk;
        pc[12] = (mode == 1 && g_dc_gfx_materials) ? s_pp[PP_GL_MARK] * addk : 0.f;   /* 25.09 ночь: отмеченные «светится» прозрачные */
        vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
        vkCmdDraw(s_cmd, (uint32_t)d.count, 1, (uint32_t)d.first, 0);
        nd++;
    }
    vkCmdEndRenderPass(s_cmd);
    { static int dbg = -1; static long k = 0;
      if (dbg < 0) { const char* e = getenv("DC_FX_LOG"); dbg = (e && *e == '1') ? 1 : 0; }
      if (dbg && (++k % 120) == 1) std::fprintf(stderr, "fx: %s pass - draw %ld%c", mode == 1 ? "glow" : "reflection", nd, 10); }
}
/* ===== ПЕЛЕНА ИГРЫ ПОВЕРХ ДЫМКИ И СВЕЧЕНИЯ (28.09.2026 (6)) ==============================================================
 * Игрок (слот 14, вход в комнату; слот 16, сцена): «дымка видна всегда и не сразу появляется/затухает… так же видна в сценах
 * в местах, где чёрные линии киношные… чтобы дымку не было видно, когда не видно и остального, и не лезла сквозь киношные
 * линии». Замер dump_prims: затемнение перехода игра делает полноэкранным квадом без текстуры, полупрозрачность «вычитание»
 * (B − F, серый 160 → 216 → 240 → 255, кадр за кадром), полосы кино — такими же квадами во всю ширину (серый 248, 28 строк
 * сверху и снизу). Дымка (dc_vol_comp) и свечение источников (dc_glow_comp) складываются с кадром ПОСЛЕ всего кадра — поверх
 * этих пелен, поэтому светили на чёрном. Теперь полосы во всю ширину (≥ 95 % дисплея), полупрозрачные без глубины, лежащие
 * в порядке вывода после мира, собираются в список (до 4, одинаковые — один раз): «вычитание» гасит добавку на свою серость
 * (почти чёрная ≥ 0.9 — целиком), «среднее» (B/2 + F/2) — вдвое; сложения не гасят. Шейдеры сложения берут список: дымка —
 * из блока ламп (vspare.x, veilR/veilK), свечение — из push-констант. DC_POST_VEIL=0 — как было. */
struct Veil { float y0, y1, mul, sub; };
static Veil s_veil[4];
static int  s_veilN = 0;
static int veil_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_POST_VEIL"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static void veil_collect(const std::vector<Draw>& draws, const std::vector<size_t>& order, int w, int h,
                         int dispX, int dispY, int dispW, int dispH) {
    s_veilN = 0;
    if (!veil_on() || !s_vmap || dispW <= 0 || dispH <= 0 || w <= 0 || h <= 0) return;
    int seen3d = 0, dropped = 0;
    for (size_t oi = 0; oi < order.size(); oi++) {
        const size_t di = order[oi];
        if (di >= draws.size()) continue;
        const Draw& d = draws[di];
        if (d.wire) continue;
        if (d.kind == 0 || (d.kind >= 2 && d.kind <= 5)) { seen3d = 1; continue; }
        if (!seen3d || d.kind < 6 || d.kind > 9 || d.count < 3) continue;
        const int mode = d.kind - 6;                          /* 0 — B/2+F/2, 1 — B+F, 2 — B−F, 3 — B+F/4 */
        if (mode == 1 || mode == 3) continue;                 /* сложение свет не гасит */
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, cs = 0.f;
        for (int k = 0; k < d.count; k++) {
            const float* p = s_vmap + (size_t)(d.first + k) * VERT_FLOATS;
            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
            if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
            cs += (p[4] + p[5] + p[6]) * (1.f / 3.f);
        }
        if (x1 - x0 < 0.95f * (float)dispW) continue;          /* только полосы во всю ширину: затемнение, полосы кино */
        const float* p0 = s_vmap + (size_t)d.first * VERT_FLOATS;
        if (mode == 2 && ((int)p0[11] & 1)) continue;          /* вычитание текстурой — серость неизвестна */
        float mul = 1.f, sub = 0.f;
        if (mode == 0) mul = 0.5f;
        else { sub = cs / (float)d.count / 255.f; if (sub >= 0.9f) { mul = 0.f; sub = 0.f; } }
        const float py0 = ((y0 - (float)dispY) / (float)dispH) * (float)h, py1 = ((y1 - (float)dispY) / (float)dispH) * (float)h;
        int dup = 0;
        for (int q = 0; q < s_veilN; q++)
            if (fabsf(s_veil[q].y0 - py0) < 0.5f && fabsf(s_veil[q].y1 - py1) < 0.5f && s_veil[q].mul == mul && fabsf(s_veil[q].sub - sub) < 1e-3f) { dup = 1; break; }
        if (dup) continue;
        if (s_veilN >= 4) { dropped++; continue; }
        s_veil[s_veilN++] = { py0, py1, mul, sub };
    }
    { static int dbg = -1; static long k = 0; static int lastN = -1;
      if (dbg < 0) { const char* e = getenv("DC_POST_VEIL_LOG"); dbg = (e && *e == '1') ? 1 : 0; }
      if ((dbg && (++k % 30) == 1) || (s_veilN != lastN && (s_veilN == 0 || lastN <= 0))) {
          std::fprintf(stderr, "veil: %d band(s) over the world this frame%s", s_veilN, s_veilN ? ":" : "");
          for (int q = 0; q < s_veilN; q++) std::fprintf(stderr, " [rows %.0f..%.0f of %d, x%.2f -%.2f]", s_veil[q].y0, s_veil[q].y1, h, s_veil[q].mul, s_veil[q].sub);
          if (dropped) std::fprintf(stderr, " (%d more not taken)", dropped);
          std::fprintf(stderr, " - haze and glow go under them%c", 10);
      }
      lastN = s_veilN; }
}
/* список полос -> блок ламп (дымка): vspare.x — число, veilV[q] = (строка от, строка до, множитель, вычитание) */
static void veil_ubo(void) {
    if (!s_shUboMap) return;
    s_shUboMap[284] = (float)s_veilN;
    for (int q = 0; q < 4; q++) {
        const bool ok = q < s_veilN;
        s_shUboMap[440 + q * 4 + 0] = ok ? s_veil[q].y0 : 0.f;
        s_shUboMap[440 + q * 4 + 1] = ok ? s_veil[q].y1 : 0.f;
        s_shUboMap[440 + q * 4 + 2] = ok ? s_veil[q].mul : 1.f;
        s_shUboMap[440 + q * 4 + 3] = ok ? s_veil[q].sub : 0.f;
    }
}
/* после прохода кадра (и SSAO): размытия свечения и сложение с кадром; dbg 1/2 — вместо кадра буфер свечения / отражения.
 * Цветная цель на входе и выходе — TRANSFER_SRC_OPTIMAL. */
static void fx_glow_post(int w, int h, int blur, int dbg) {
    const int hw = (w + 1) / 2, hh = (h + 1) / 2, qw = (hw + 1) / 2, qh = (hh + 1) / 2;
    auto pass = [&](VkRenderPass rp, VkFramebuffer fb, int pw, int ph, VkPipeline pipe, VkDescriptorSet set, const float* p8,
                    uint32_t pbytes = 32) {
        const VkViewport vpt{ 0.f, 0.f, (float)pw, (float)ph, 0.f, 1.f };
        const VkRect2D sc{ { 0, 0 }, { (uint32_t)pw, (uint32_t)ph } };
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = rp; rb.framebuffer = fb; rb.renderArea = sc;
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gplLay, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(s_cmd, s_gplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pbytes, p8);   /* 28.09 (6): сложение — 112 байт */
        vkCmdDraw(s_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(s_cmd);
    };
    if (blur) {
        const float r = s_pp[PP_GL_RAD];
        /* a: шаг выборки в пикселях источника (x, y), во сколько раз источник крупнее цели; b: активная область источника */
        const float b1[8] = { r, 0.f, 1.f, 0.f, (float)hw, (float)hh, 0.f, 0.f };
        const float b2[8] = { 0.f, r, 1.f, 0.f, (float)hw, (float)hh, 0.f, 0.f };
        const float b3[8] = { 2.f * r, 0.f, 2.f, 0.f, (float)hw, (float)hh, 0.f, 0.f };
        const float b4[8] = { 0.f, r, 1.f, 0.f, (float)qw, (float)qh, 0.f, 0.f };
        pass(s_rpGB, s_fbT,  hw, hh, s_gBlurPipe, s_gset[0], b1);   /* E -> T: по горизонтали */
        pass(s_rpGB, s_fbG1, hw, hh, s_gBlurPipe, s_gset[1], b2);   /* T -> G1: по вертикали (ближний ореол) */
        pass(s_rpGB, s_fbQ,  qw, qh, s_gBlurPipe, s_gset[2], b3);   /* E -> Q: вдвое мельче + по горизонтали */
        pass(s_rpGB, s_fbG2, qw, qh, s_gBlurPipe, s_gset[3], b4);   /* Q -> G2: по вертикали (дальний ореол) */
    }
    VkImageMemoryBarrier c{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    c.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; c.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    c.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    c.image = s_colImg; c.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    c.srcQueueFamilyIndex = c.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &c);
    /* a.w — доля ореола на самой светящейся текстуре (25.09 ночь (3), glow_self);
     * 28.09 (6): c.x — число полос пелены игры, vv[4] — полосы во всю ширину: (строка от, строка до — точки цели, множитель,
     * вычитание); dc_glow_comp.frag гасит ими добавку свечения (см. «ПЕЛЕНА ИГРЫ») */
    float cp[28] = { blur ? s_pp[PP_GL_STR] : 0.f, s_pp[PP_GL_WIDE], (float)dbg, g_dc_gfx_materials ? s_pp[PP_GL_SELF] : 1.f,
                     (float)hw, (float)hh, (float)qw, (float)qh };
    cp[8] = (float)s_veilN;
    for (int q = 0; q < 4; q++) {
        const bool ok = q < s_veilN;
        cp[12 + q * 4 + 0] = ok ? s_veil[q].y0 : 0.f;  cp[12 + q * 4 + 1] = ok ? s_veil[q].y1 : 0.f;
        cp[12 + q * 4 + 2] = ok ? s_veil[q].mul : 1.f; cp[12 + q * 4 + 3] = ok ? s_veil[q].sub : 0.f;
    }
    pass(s_rpGC, s_fbGC, w, h, dbg ? s_gCompDbgPipe : s_gCompPipe, s_gset[4], cp, 112);
}

/* ===== ОБЪЁМНЫЙ СВЕТ У ЛАМП (27.09.2026) =================================================================================
 * Игрок: «Попробуй объемный свет у ламп сделать (дымка и пыль в лучах)» (№4 списка улучшений от 24.09). После SSAO и свечения:
 *  1) dc_vol.frag — половинное разрешение (RGBA16F): луч из камеры до поверхности (глубина — цель прохода 1 SSAO, s_zView), по
 *     нему свет ламп игры (pt_renderer.c dc_vol_frame -> vlp/vlc в UBO) и текстур «светится» (glp/glc), рассеянный в воздухе;
 *     у ламп с картой теней персонажей — их тень прямо в дымке (s_shView); клубы — шум в координатах комнаты;
 *  2) dc_vol_comp.frag — окно 4x4 с учётом глубины и сложение с кадром (проход s_rpComp/s_fbComp от SSAO, альфа = 1);
 *  3) dc_dust.vert/.frag — в том же проходе пылинки: точки в координатах комнаты у ламп, свет лампы в своей точке.
 * Без SSAO нет глубины — нет и дымки. Кадр B — своей проекцией, точки возвращаются в кадр A (wq/wt, как у растра). Параметры —
 * группа 2 таблицы «Картинки» (vol*, F8 «Объёмный свет у ламп»). DC_VOL=0 — выкл. */
static VkRenderPass          s_rpV = VK_NULL_HANDLE;
static VkDescriptorSetLayout s_vdsl = VK_NULL_HANDLE;
static VkPipelineLayout      s_vplLay = VK_NULL_HANDLE;
static VkDescriptorPool      s_vdpool = VK_NULL_HANDLE;
static VkDescriptorSet       s_vset = VK_NULL_HANDLE;
static VkPipeline            s_vPipe = VK_NULL_HANDLE, s_vCompPipe = VK_NULL_HANDLE, s_vDustPipe = VK_NULL_HANDLE;
static VkImage               s_vImg = VK_NULL_HANDLE;
static VkDeviceMemory        s_vMem = VK_NULL_HANDLE;
static VkImageView           s_vView = VK_NULL_HANDLE;
static VkFramebuffer         s_fbV = VK_NULL_HANDLE;
static int                   s_vOk = 0;                     /* 1 — собрано, -1 — не вышло (объёмного света не будет) */
static int                   s_vW = 0, s_vH = 0;
static VkImageView           s_vZOf = VK_NULL_HANDLE;        /* под какую глубину SSAO записан набор дескрипторов */

static bool v_mkPipe(VkShaderModule vsm, VkShaderModule fsm, int additive, VkRenderPass rp, VkPipeline* out) {
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    if (additive) {                               /* цвет кадра + выход, альфа = 1 (выход несёт 1) */
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
    gp.layout = s_vplLay; gp.renderPass = rp; gp.subpass = 0;
    const bool ok = vkp_create(&gp, out, "post") == VK_SUCCESS;
    if (!ok) rlog("[VKR] vol pipeline FAILED\n");
    return ok;
}
/* 18.10.2026: объекты дымки без цели (проход, раскладка, три конвейера) - отдельно, их заранее строит экран подготовки
 * шейдеров (dcvk_prepare_all); нужен только проход сложения SSAO (postObjects). 1 - собраны. */
static int volObjects(void) {
    if (s_vOk) return s_vOk > 0;
    if (!s_rpComp) return 0;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    {
        s_vOk = -1;
        if (!pp_mkPass(HF, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpV))
            return 0;
        VkDescriptorSetLayoutBinding b[4]{};
        for (int i = 0; i < 4; i++) {
            b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;          /* блок ламп (тот же, что у растра) */
        VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dci.bindingCount = 4; dci.pBindings = b;
        if (vkCreateDescriptorSetLayout(C->device, &dci, nullptr, &s_vdsl) != VK_SUCCESS) return 0;
        VkPushConstantRange pr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
        VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plci.setLayoutCount = 1; plci.pSetLayouts = &s_vdsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(C->device, &plci, nullptr, &s_vplLay) != VK_SUCCESS) return 0;
        VkDescriptorPoolSize psz[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = psz;
        if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_vdpool) != VK_SUCCESS) return 0;
        VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsa.descriptorPool = s_vdpool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &s_vdsl;
        if (vkAllocateDescriptorSets(C->device, &dsa, &s_vset) != VK_SUCCESS) return 0;
        VkShaderModule pvs = loadSpv("dc_post.vert.spv"), vfs = loadSpv("dc_vol.frag.spv"),
                       cfs = loadSpv("dc_vol_comp.frag.spv"), dvs = loadSpv("dc_dust.vert.spv"),
                       dfs = loadSpv("dc_dust.frag.spv");
        const bool ok = pvs && vfs && cfs && dvs && dfs
                     && v_mkPipe(pvs, vfs, 0, s_rpV, &s_vPipe)
                     && v_mkPipe(pvs, cfs, 1, s_rpComp, &s_vCompPipe)
                     && v_mkPipe(dvs, dfs, 1, s_rpComp, &s_vDustPipe);
        VkShaderModule mods[5] = { pvs, vfs, cfs, dvs, dfs };
        for (int i = 0; i < 5; i++) if (mods[i]) vkDestroyShaderModule(C->device, mods[i], nullptr);
        if (!ok) { std::fprintf(stderr, "vol: no shaders\\dc_vol*.spv / dc_dust.*.spv or the pipeline failed to build - no volumetric light%c", 10); return 0; }
        s_vOk = 1;
        rlog("[VKR] vol (volumetric lamps + dust) ready\n");
    }
    return 1;
}
/* 1 — объёмный свет можно писать в этом кадре (после postEnsure: нужны цель глубины и проход сложения SSAO). До записи команд. */
static int volEnsure(void) {
    if (s_vOk < 0) return 0;
    if (!s_shView || !s_shSamp || !s_shUbo || !s_zView || !s_fbComp || !s_rpComp) return 0;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!s_vOk && !volObjects()) return 0;
    int TW, TH; post_fit_size(&TW, &TH);                /* 24.10.2026: по кадрам с 3D (post_fit_note) */
    if (s_vW != TW || s_vH != TH) {
        vkDeviceWaitIdle(C->device);
        if (s_fbV) { vkDestroyFramebuffer(C->device, s_fbV, nullptr); s_fbV = VK_NULL_HANDLE; }
        pp_freeImage(&s_vImg, &s_vMem, &s_vView);
        const int hw = (TW + 1) / 2, hh = (TH + 1) / 2;
        bool ok = pp_mkImage(HF, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, hw, hh, &s_vImg, &s_vMem, &s_vView);
        if (ok) {
            VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fci.renderPass = s_rpV; fci.attachmentCount = 1; fci.pAttachments = &s_vView;
            fci.width = (uint32_t)hw; fci.height = (uint32_t)hh; fci.layers = 1;
            ok = vkCreateFramebuffer(C->device, &fci, nullptr, &s_fbV) == VK_SUCCESS;
        }
        if (!ok) { s_vOk = -1; std::fprintf(stderr, "vol: haze target not created - no volumetric light%c", 10); return 0; }
        s_vW = TW; s_vH = TH; s_vZOf = VK_NULL_HANDLE;
        std::fprintf(stderr, "vol: haze target %dx%d%c", hw, hh, 10);
    }
    if (s_vZOf != s_zView) {                          /* глубина SSAO пересоздана (размер кадра) — набор заново */
        VkDescriptorImageInfo ii[3] = { { s_pSamp,  s_zView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { s_shSamp, s_shView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { s_pSamp,  s_vView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkDescriptorBufferInfo ub{ s_shUbo, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wd[4]{};
        const int bind[4] = { 0, 1, 2, 3 };
        for (int i = 0; i < 4; i++) {
            wd[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[i].dstSet = s_vset; wd[i].dstBinding = (uint32_t)bind[i];
            wd[i].descriptorCount = 1;
        }
        wd[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[0].pImageInfo = &ii[0];
        wd[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[1].pImageInfo = &ii[1];
        wd[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;         wd[2].pBufferInfo = &ub;
        wd[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[3].pImageInfo = &ii[2];
        vkUpdateDescriptorSets(C->device, 4, wd, 0, nullptr);
        s_vZOf = s_zView;
    }
    return 1;
}
/* после SSAO (и свечения): дымка в половинной сетке, сложение с кадром и пыль. Цветная цель на входе и выходе —
 * TRANSFER_SRC_OPTIMAL; глубина SSAO (s_zView) и карта теней — SHADER_READ_ONLY. warp16 — поля wq/wt кадра B (nullptr — кадр A). */
static void vol_run(int w, int h, const float* view, const float* wp, const float* warp16) {
    static LARGE_INTEGER qf = { 0 }, t0 = { 0 };
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!qf.QuadPart) { QueryPerformanceFrequency(&qf); t0 = now; }
    const float tsec = (float)fmod((double)(now.QuadPart - t0.QuadPart) / (double)qf.QuadPart, 3600.0);
    const float wq[4] = { warp16 ? warp16[0] : 0.f, warp16 ? warp16[1] : 0.f, warp16 ? warp16[2] : 0.f, warp16 ? warp16[3] : 1.f };
    const float wt[4] = { warp16 ? warp16[4] : 0.f, warp16 ? warp16[5] : 0.f, warp16 ? warp16[6] : 0.f, warp16 ? warp16[7] : 0.f };
    const float pcs[32] = { view[0], view[1], view[2], view[3], wp[0], wp[1], wp[2], wp[3],
                            (float)w, (float)h, 1.f / (float)w, 1.f / (float)h,
                            wq[0], wq[1], wq[2], wq[3], wt[0], wt[1], wt[2], wt[3],
                            s_pp[PP_VL_STR], g_dc_gfx_materials ? s_pp[PP_VL_GLOW] : 0.f, s_pp[PP_VL_RAD], s_pp[PP_VL_G],
                            s_pp[PP_VL_NOISE], tsec, s_pp[PP_VL_SH], s_pp[PP_VL_STEPS],
                            s_pp[PP_VL_DBG], s_pp[PP_VL_DUSTSZ], s_pp[PP_VL_DUST], s_pp[PP_VL_DUSTN] };
    const int hw = (w + 1) / 2, hh = (h + 1) / 2;
    auto begin = [&](VkRenderPass rp, VkFramebuffer fb, int pw, int ph) {
        const VkViewport vpt{ 0.f, 0.f, (float)pw, (float)ph, 0.f, 1.f };
        const VkRect2D sc{ { 0, 0 }, { (uint32_t)pw, (uint32_t)ph } };
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = rp; rb.framebuffer = fb; rb.renderArea = sc;
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vplLay, 0, 1, &s_vset, 0, nullptr);
        vkCmdPushConstants(s_cmd, s_vplLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcs, pcs);
    };
    begin(s_rpV, s_fbV, hw, hh);                                    /* 1: дымка, половинная сетка */
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vPipe);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(s_cmd);
    VkImageMemoryBarrier c{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    c.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; c.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    c.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    c.image = s_colImg; c.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    c.srcQueueFamilyIndex = c.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &c);
    begin(s_rpComp, s_fbComp, w, h);                                /* 2: сложение с кадром */
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vCompPipe);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    const int nd = (int)(s_pp[PP_VL_DUSTN] + 0.5f);
    if (s_pp[PP_VL_DUST] > 0.f && nd > 0) {                         /* 3: пыль в лучах */
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_vDustPipe);
        vkCmdDraw(s_cmd, (uint32_t)nd * 6u, 1, 0, 0);
    }
    vkCmdEndRenderPass(s_cmd);
    { static int dbg = -1; static long k = 0;
      if (dbg < 0) { const char* e = getenv("DC_VOL_LOG"); dbg = (e && *e == '1') ? 1 : 0; }
      if (dbg && (++k % 120) == 1 && s_shUboMap)
          std::fprintf(stderr, "vol: frame %s, game lamps %.0f (matrices %s), 'glows' textures %.0f, dust motes %d, t %.1f s%c",
                       warp16 ? "B" : "A", s_shUboMap[256], s_shUboMap[259] > 0.5f ? "yes" : "no", s_shUboMap[140], nd, tsec, 10); }
}

/* ===== СВЕТ ОТ СВЕТЯЩИХСЯ ТЕКСТУР (25.09.2026 поздно) ===============================================================
 * Игрок: «от светящихся текстур тоже должен быть какой-то эффект (реальный свет, настраиваемый, и того цвета, какого
 * текстура)». Каждый кадр A: треугольники потока с текстурой «светится» (бит 23 страницы, выбор в приложении; с глубиной, не
 * рабица) -> видовая точка (обратная перспектива по w, как у карты теней), площадь, цвет самой текстуры (до 7 текселей
 * окна UV из видеопамяти, прозрачные мимо). Соседние (ближе 800 ед. к центру группы) — один источник: центр по площади,
 * цвет по площади. До 8 самых сильных (площадь × яркость) -> UBO 140 (число), glp/glc (144..207): точка + радиус,
 * цвет × сила. dc_raster.frag добавляет свет как у вспышки: альбедо × цвет × (1 − d/R)² × косинус (персонажам обтекание
 * ≥ 0.5); сама светящаяся текстура своим светом не подсвечивается. Тени у этого света нет. Параметры — группа 2 таблицы
 * «Картинки» (glow_light, _str, _rad). Кадр B: точки в пространстве кадра A (фрагмент возвращается туда же). */
extern "C" int g_dc_orig_mode;             /* memory.c: родной режим (клавиша O) */
extern "C" int dc_orig_nogfx(void);        /* memory.c 28.09 (7): режим O и наша графика в нём выключена (DC_ORIG_GFX=0) */
static int s_glN = 0;                      /* источников в последнем кадре (для панели) */
extern "C" void dcvk_glow_light_stats(int* n) { *n = s_glN; }
/* 19.10.2026 (игрок: «увеличь дальность света»): источников в кадре до 16 - прежде 8, и в комнате с дюжиной ламп (сейв 49 игрока:
 * 12 следов) четыре дальних не светили вовсе, а с поворотом камеры выпадали и возвращались. Источник j < 8 - в glp/glc (144/176),
 * дальше - в glp2/glc2 в конце блока (464/496); 140 - сколько первых (до 8), 143 - сколько дальше */
enum { GL_OUT = 16 };
static inline float* gl_slot_p(int j) { return s_shUboMap + (j < 8 ? 144 + j * 4 : j < 16 ? 464 + (j - 8) * 4 : SH_UBO_GL3 + (j - 16) * 4); }
static inline float* gl_slot_c(int j) { return s_shUboMap + (j < 8 ? 176 + j * 4 : j < 16 ? 496 + (j - 8) * 4 : SH_UBO_GL3 + 64 + (j - 16) * 4); }
static inline void gl_set_count(int n) { s_shUboMap[140] = (float)(n < 8 ? n : 8); s_shUboMap[143] = (float)(n > 8 ? n - 8 : 0); }
static void gl_texel_rgb(const unsigned short* vr, int tp, int cx, int cy, int u, int v, float* rgb, int* ok) {
    const int depth = (tp >> 7) & 3, tpx = (tp & 15) * 64, tpy = ((tp >> 4) & 1) * 256;
    u &= 255; v &= 255;
    unsigned short tx;
    if (depth == 0) {
        const unsigned short w = vr[(((tpy + v) & 511) << 10) + ((tpx + (u >> 2)) & 1023)];
        tx = vr[((cy & 511) << 10) + ((cx + ((w >> ((u & 3) * 4)) & 0xF)) & 1023)];
    } else if (depth == 1) {
        const unsigned short w = vr[(((tpy + v) & 511) << 10) + ((tpx + (u >> 1)) & 1023)];
        tx = vr[((cy & 511) << 10) + ((cx + ((w >> ((u & 1) * 8)) & 0xFF)) & 1023)];
    } else {
        tx = vr[(((tpy + v) & 511) << 10) + ((tpx + u) & 1023)];
    }
    if (tx == 0) { *ok = 0; return; }          /* прозрачный тексель */
    rgb[0] = (float)(tx & 31) * 8.f; rgb[1] = (float)((tx >> 5) & 31) * 8.f; rgb[2] = (float)((tx >> 10) & 31) * 8.f;
    *ok = 1;
}
/* ===== РОВНЫЙ СВЕТ МИГАЮЩИХ ЛАМП (27.09.2026) ======================================================================
 * Игрок: «есть мигающие лампы и если я указываю что они светящиеся, свет сильно бьет по глазам. Можешь на мигающих
 * текстурах этот свет делать не мигающим почти?». Игра мигает лампами по-разному: полупрозрачный ореол рисуется через
 * кадр (на ЭЛТ сливался в ровное свечение; FLICKER FUSER повторяет его только при неподвижной камере — на ходу ореол
 * мигает честно), плашка гаснет и загорается, текстура меняется. Свет выше собирается заново каждый кадр A и мигал вместе
 * с лампой — строб до 15 Гц. Теперь у каждого источника есть СЛЕД в комнате (матрица «вид -> комната» кадра из
 * dc_vol_frame; нет её — в виде кадра и коротко): группа кадра находит ближайший свой след (ближе 600 ед.), след держит свет
 * ГОРЯЩЕЙ фазы — пик ровный, пока лампа горела не раньше glow_light_hold с назад (дольше темно — погасла или потускнела:
 * пик спадает за 0.3 с до нового уровня), цвет и размер — только из кадров не темнее 85 % пика. Если лампы в потоке нет, а
 * её место в кадре — свет держится до glow_light_hold с; за краем кадра, после смены ракурса или комнаты (глаз прыгнул
 * > 1200 ед., взгляд повернулся > 18°) — гаснет сразу, как было. В свет идёт доля мигания glow_light_flick (0.15 —
 * «почти»): быстрое среднее мгновенного света (0.15 с). Ровная лампа не меняется: след = её кадр, точка — сглаженное место
 * в комнате (группа «лампа + ореол» и «лампа без ореола» — центры в разных местах). DC_GLOW_LIGHT_STEADY=0 / галка F8 —
 * как было. DC_GLOW_LIGHT_LOG=2 — строка glowlight2: на каждый кадр A (следы, свет, видна/держится). */
/* 27.10.2026 (игрок, слот 1: «свечение от ламп нужно сделать, чтобы оно все-таки было привязано к их источникам ... это
 * свечение, оно остается на месте, а там уже этой лампы, так сказать, нету источника. То есть такие вот, как это,
 * остаточный след остается»): группа видимых треугольников уезжала с поворотом камеры (состав групп по 800 ед. зависел от
 * того, что видно), а осиротевший след держался hold (1 с) там, где лампы нет. Теперь (1) треугольник светильника комнаты
 * (pt_renderer.c dc_glow_fixtures: меш комнаты, неподвижный центр) идёт в группу ЭТОГО светильника, точка света - его
 * центр, след узнаётся по ключу светильника; прочие (не из меша) - группами, как было; (2) свет держится между вспышками
 * только у следа, который уже мигал (лампа пропадала и возвращалась, пока место в кадре); немигавший без лампы гаснет
 * сразу (0.15 с). DC_GLOW_LIGHT_FIX=0 - как было. */
struct GlClus { double p[3], c[3], a, lum; unsigned fix; float fc[3], fa; int fi; };   /* fix - ключ светильника (0 - нет), fc - центр, fa - площадь, fi - его номер (28.10) */
enum { GLT_N = 32 };
struct GlTrack {
    float pos[3];      /* место лампы: в комнате (room = 1) или в виде кадра (room = 0, матриц нет) */
    float view[3];     /* точка вида последнего кадра, где лампу видели (room = 0) */
    float e[3];        /* свет горящей фазы: цвет текстуры × множитель размера / 255 — как glc до силы */
    float kr;          /* множитель радиуса горящей фазы */
    float peak;        /* наибольший канал света с медленным спадом */
    float fast[3];     /* быстрое среднее мгновенного света (кадр без лампы — 0) */
    double seen;       /* когда лампу видели в потоке, с */
    double lastOn;     /* когда лампа последний раз горела (не темнее 85 % пика), с */
    int alive, room, hit, id;
    unsigned fix;      /* 27.10: ключ светильника комнаты (0 - группа видимого, как было) */
    int miss, blinks;  /* 27.10: в прошлом кадре A лампы не было (место в кадре); сколько раз она так пропадала и возвращалась */
    float fade;        /* 27.10: доля света немигавшего следа без лампы (гаснет за 0.15 с) */
};
static GlTrack s_glt[GLT_N];
static double  s_gltPrev = 0.0;
static float   s_gltEye[3], s_gltFwd[3];
static int     s_gltHave = 0, s_gltNextId = 1;
static void gl_steady_reset(void) { for (int i = 0; i < GLT_N; i++) s_glt[i].alive = 0; s_gltHave = 0; }
static inline float gl_max3(const float* v) { return v[0] > v[1] ? (v[0] > v[2] ? v[0] : v[2]) : (v[1] > v[2] ? v[1] : v[2]); }
static inline void gl_xf(const float* M, const float* p, float* o) {        /* матрица 3 строки: xyz + перенос */
    for (int r = 0; r < 3; r++) o[r] = M[r * 4] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2] + M[r * 4 + 3];
}
extern "C" int dc_light_epoch(void);    /* pt_renderer.c: растёт при смене комнаты и быстрой загрузке */
extern "C" int dc_glow_fixtures(const float** cen4, const float** box6, const unsigned** key);   /* pt_renderer.c 27.10: светильники комнаты */
static int gl_fix_on(void) { static int v = -1; if (v < 0) { const char* e = getenv("DC_GLOW_LIGHT_FIX"); v = (e && *e == '0') ? 0 : 1; } return v; }
static int gl_steady_out(const GlClus* cl, int ncl) {
    /* 19.10.2026 (игрок: «в другой комнате уже, а старый свет ещё оставался»): след лампы прежней комнаты в координатах
     * комнаты мог совпасть с лампой новой ближе 600 ед. - и та светила прежним пиком и цветом, пока не прошёл hold. Новая
     * комната или быстрая загрузка - следы с нуля (смена ракурса ловится и так, а переход через дверь - не всегда) */
    { static int ep = -1; const int e = dc_light_epoch();
      if (e != ep) { ep = e; gl_steady_reset(); } }
    LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
    const double now = (double)qc.QuadPart / (double)qf.QuadPart;
    float dt = s_gltPrev > 0.0 ? (float)(now - s_gltPrev) : 0.f;
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.25f) dt = 0.25f;
    s_gltPrev = now;
    const int room = s_shUboMap[259] > 0.5f ? 1 : 0;
    const float* V2R = s_shUboMap + 260;
    const float* R2V = s_shUboMap + 272;
    const float flick = s_pp[PP_GL_LFLICK], hold = s_pp[PP_GL_LHOLD];
    const float holdMax = room ? hold : (hold < 0.12f ? hold : 0.12f);   /* без матриц место в виде верно, пока камера стоит */
    const int fixOn = gl_fix_on();                                       /* 27.10: держать свет только мигавшим */
    const float aFast = 1.f - expf(-dt / 0.15f), kDim = expf(-dt / 0.3f), kGone = expf(-dt / 0.15f);
    int cut = 0;                                                         /* смена ракурса / комнаты */
    if (room) {
        const float eye[3] = { V2R[3], V2R[7], V2R[11] };
        float fwd[3] = { V2R[2], V2R[6], V2R[10] };
        const float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl > 1e-6f) for (int q = 0; q < 3; q++) fwd[q] /= fl;
        if (s_gltHave) {
            const float dx = eye[0] - s_gltEye[0], dy = eye[1] - s_gltEye[1], dz = eye[2] - s_gltEye[2];
            const float dot = fwd[0] * s_gltFwd[0] + fwd[1] * s_gltFwd[1] + fwd[2] * s_gltFwd[2];
            if (dx * dx + dy * dy + dz * dz > 1200.f * 1200.f || dot < 0.95f) cut = 1;
        }
        for (int q = 0; q < 3; q++) { s_gltEye[q] = eye[q]; s_gltFwd[q] = fwd[q]; }
        s_gltHave = 1;
    } else s_gltHave = 0;
    for (int t = 0; t < GLT_N; t++) {
        s_glt[t].hit = 0;
        if (s_glt[t].alive && s_glt[t].room != room) s_glt[t].alive = 0;
    }
    /* группы кадра — от самой сильной: ближайший свободный след ближе 600 ед. или новый */
    int ord[32]; float en[32][3], enm[32], krn[32];
    const int nc = ncl < 32 ? ncl : 32;
    for (int c = 0; c < nc; c++) {
        const double A = cl[c].a;
        /* 27.10: размер - по видимой площади, как было (по всей площади светильника свет стал ярче, чем настроил игрок) */
        const double sz = sqrt(A) / 300.0;
        const float ki = (float)(sz < 0.35 ? 0.35 : sz > 1.5 ? 1.5 : sz);
        krn[c] = (float)(sz < 0.6 ? 0.6 : sz > 1.6 ? 1.6 : sz);
        for (int q = 0; q < 3; q++) en[c][q] = (float)(cl[c].c[q] / A) * ki / 255.f;
        enm[c] = gl_max3(en[c]); ord[c] = c;
    }
    for (int a = 1; a < nc; a++) { const int k = ord[a]; int b = a - 1; while (b >= 0 && enm[ord[b]] < enm[k]) { ord[b + 1] = ord[b]; b--; } ord[b + 1] = k; }
    for (int oi = 0; oi < nc; oi++) {
        const int c = ord[oi];
        const double A = cl[c].a;
        const float vw[3] = { (float)(cl[c].p[0] / A), (float)(cl[c].p[1] / A), (float)(cl[c].p[2] / A) };
        float rp[3];
        const unsigned fx = room ? cl[c].fix : 0u;                       /* 27.10: светильник комнаты - его центр и его след */
        if (fx) { rp[0] = cl[c].fc[0]; rp[1] = cl[c].fc[1]; rp[2] = cl[c].fc[2]; }
        else if (room) gl_xf(V2R, vw, rp); else { rp[0] = vw[0]; rp[1] = vw[1]; rp[2] = vw[2]; }
        int best = -1; float bd = 600.f * 600.f;
        for (int t = 0; t < GLT_N; t++) {
            const GlTrack& T = s_glt[t];
            if (!T.alive || T.hit || T.fix != fx) continue;
            if (fx) { best = t; break; }
            const float dx = T.pos[0] - rp[0], dy = T.pos[1] - rp[1], dz = T.pos[2] - rp[2], d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = t; }
        }
        if (best < 0) {                                                  /* новый след: свободный или самый слабый невидимый */
            for (int t = 0; t < GLT_N && best < 0; t++) if (!s_glt[t].alive) best = t;
            if (best < 0) {
                float wp = 1e30f;
                for (int t = 0; t < GLT_N; t++) if (!s_glt[t].hit && s_glt[t].peak < wp) { wp = s_glt[t].peak; best = t; }
            }
            if (best < 0) continue;
            GlTrack& T = s_glt[best];
            T.alive = 1; T.room = room; T.hit = 1; T.id = s_gltNextId++; T.seen = now; T.lastOn = now;
            for (int q = 0; q < 3; q++) { T.pos[q] = rp[q]; T.view[q] = vw[q]; T.e[q] = en[c][q]; T.fast[q] = en[c][q]; }
            T.kr = krn[c]; T.peak = enm[c];
            T.fix = fx; T.miss = 0; T.blinks = 0; T.fade = 1.f;
            continue;
        }
        GlTrack& T = s_glt[best];
        for (int q = 0; q < 3; q++) {
            T.pos[q] = fx ? rp[q] : room ? T.pos[q] + (rp[q] - T.pos[q]) * 0.35f : rp[q];   /* лампа в комнате стоит: место сглаживается */
            T.view[q] = vw[q];
            T.fast[q] += (en[c][q] - T.fast[q]) * aFast;
        }
        if (T.miss) { T.miss = 0; if (T.blinks < 1000) T.blinks++; }   /* 27.10: пропадала и вернулась - мигает */
        T.fade = 1.f;
        /* пик держится ровно, пока лампа горела не раньше hold с назад (тёмная фаза мигания); дольше темно — лампа
         * погасла или стала тусклее: пик спадает (0.3 с) до её нового уровня */
        float pk = (now - T.lastOn > hold) ? T.peak * kDim : T.peak;
        if (enm[c] > pk) pk = enm[c];
        if (enm[c] >= 0.85f * pk) {                                      /* горящая фаза: её цвет и размер */
            for (int q = 0; q < 3; q++) T.e[q] += (en[c][q] - T.e[q]) * 0.5f;
            T.kr += (krn[c] - T.kr) * 0.5f;
            T.lastOn = now;
        }
        T.peak = pk; T.seen = now; T.hit = 1;
    }
    /* следы без лампы в этом кадре: место в кадре — держим свет, иначе гаснет сразу */
    float sx0 = 1e30f, sx1 = -1e30f, sy0 = 1e30f, sy1 = -1e30f; int haveB = 0;
    for (int t = 0; t < GLT_N; t++) {
        GlTrack& T = s_glt[t];
        if (!T.alive || T.hit) continue;
        const float age = (float)(now - T.seen);
        if (cut || age > holdMax + 0.5f) { T.alive = 0; continue; }
        if (!haveB) {                                                    /* границы 3D-кадра — по вершинам потока */
            haveB = 1;
            for (int i = 0; i < s_nverts; i++) {
                const float* v = s_vmap + (size_t)i * VERT_FLOATS;
                if (v[12] < 1.5f) continue;
                if (v[0] < sx0) sx0 = v[0]; if (v[0] > sx1) sx1 = v[0];
                if (v[1] < sy0) sy0 = v[1]; if (v[1] > sy1) sy1 = v[1];
            }
        }
        float v[3];
        if (room) gl_xf(R2V, T.pos, v); else { v[0] = T.view[0]; v[1] = T.view[1]; v[2] = T.view[2]; }
        int on = 0;
        if (v[2] > 50.f && sx1 > sx0 && sy1 > sy0) {
            const float sx = v[0] * s_shWp[3] / v[2] + s_shWp[0], sy = v[1] * s_shWp[2] / v[2] + s_shWp[1];
            const float mx = (sx1 - sx0) * 0.05f, my = (sy1 - sy0) * 0.05f;
            on = sx >= sx0 - mx && sx <= sx1 + mx && sy >= sy0 - my && sy <= sy1 + my;
        }
        if (!on) { T.alive = 0; continue; }
        for (int q = 0; q < 3; q++) T.fast[q] -= T.fast[q] * aFast;
        /* 27.10: держим только след, который уже мигал; немигавший без лампы гаснет сразу, но помнится до hold + 0.5 с:
         * вернётся лампа - значит мигает, дальше её свет держится */
        T.miss = 1;
        if (fixOn && !T.blinks) T.fade *= kGone;
        else if (age > holdMax) T.peak *= kGone;                         /* пока держим — пик ровный */
        if (T.peak < 0.002f) T.alive = 0;
    }
    /* вывод: до 8 заметных (свет × близость к камере) */
    int tix[GLT_N]; float pri[GLT_N], eo[GLT_N][3], vo[GLT_N][3]; int nt = 0;
    for (int t = 0; t < GLT_N; t++) {
        const GlTrack& T = s_glt[t];
        if (!T.alive) continue;
        const float em = gl_max3(T.e), s = em > 1e-6f ? T.peak / em : 0.f;
        for (int q = 0; q < 3; q++) { const float x = (T.e[q] * s * (1.f - flick) + T.fast[q] * flick) * T.fade; eo[nt][q] = x > 0.f ? x : 0.f; }
        if (room) gl_xf(R2V, T.pos, vo[nt]); else { vo[nt][0] = T.view[0]; vo[nt][1] = T.view[1]; vo[nt][2] = T.view[2]; }
        const float dd = (vo[nt][0] * vo[nt][0] + vo[nt][1] * vo[nt][1] + vo[nt][2] * vo[nt][2]) / (5000.f * 5000.f);
        pri[nt] = gl_max3(eo[nt]) / (1.f + dd);
        if (pri[nt] < 1e-5f) continue;
        tix[nt++] = t;
    }
    int sel[GLT_N];
    for (int i = 0; i < nt; i++) sel[i] = i;
    for (int a = 1; a < nt; a++) { const int k = sel[a]; int b = a - 1; while (b >= 0 && pri[sel[b]] < pri[k]) { sel[b + 1] = sel[b]; b--; } sel[b + 1] = k; }
    const int n = nt < GL_OUT ? nt : GL_OUT;                   /* 19.10: до 16 (было 8) */
    for (int j = 0; j < n; j++) {
        const int i = sel[j];
        const GlTrack& T = s_glt[tix[i]];
        float* gp = gl_slot_p(j);
        float* gc = gl_slot_c(j);
        gp[0] = vo[i][0]; gp[1] = vo[i][1]; gp[2] = vo[i][2];
        gp[3] = s_pp[PP_GL_LRAD] * T.kr;
        for (int q = 0; q < 3; q++) gc[q] = eo[i][q] * s_pp[PP_GL_LSTR];
        gc[3] = gl_chr_smooth() ? 0.25f : -1.25f;              /* 28.09: знак < 0 — персонажи по граням, как было */
    }
    { static int lg = -1;
      if (lg < 0) { const char* e = getenv("DC_GLOW_LIGHT_LOG"); lg = (e && *e >= '1' && *e <= '9') ? *e - '0' : 0; }
      if (lg >= 2) {
          char buf[2400]; int p = 0; float sum = 0.f;
          for (int j = 0; j < n; j++) sum += gl_max3(gl_slot_c(j));
          p += snprintf(buf + p, sizeof buf - p, "glowlight2: t=%.3f steady groups %d traces %d out %d sum %.4f%s%s |", now, ncl, nt, n, sum,
                        room ? "" : " (no matrices)", cut ? " SHOT CHANGE" : "");
          for (int j = 0; j < n && p < (int)sizeof buf - 110; j++) {
              const GlTrack& T = s_glt[tix[sel[j]]];
              p += snprintf(buf + p, sizeof buf - p, " #%d %s %.3f at %.0f,%.0f,%.0f %s b%d f%.2f;", T.id, T.hit ? "visible" : "held",
                            gl_max3(gl_slot_c(j)), T.pos[0], T.pos[1], T.pos[2], T.fix ? "fix" : "grp", T.blinks, T.fade);
          }
          std::fprintf(stderr, "%s%c", buf, 10);
      } }
    return n;
}
/* ===== ВСЕ ЛАМПЫ КОМНАТЫ СРАЗУ (28.10.2026) ==========================================================================
 * Игрок (слот 49): «идешь вперед и прям видно как в глазах включаются эти дымка от света. А может быть и сам свет. А можно
 * сделать, чтобы оно сразу все было включено и на глазах не появлялось?». Свет и дымка брали лампы только из потока кадра:
 * лампа входила в кадр - её свет загорался, выходила - гас; ламп в комнате сейва 49 больше, чем мест (16 света, 8 дымки), и на
 * ходу места переходили от лампы к лампе - свет и дымка вспыхивали на глазах. Теперь, пока комнату рисует обход растра:
 * (1) светильник комнаты (pt_renderer.c dc_glow_fixtures) светит, пока обход его рисует, в кадре он или нет
 *     (dc_glow_fixture_present); цвет и размер - по мешу комнаты (его треугольники, обращённые к камере, тексели - как у
 *     потока), поток кадра даёт только мигание (доля glow_light_flick) и тёмную фазу мигающей лампы, как было;
 * (2) 32 места (прежние пути - до 16, дымка у первых 8; в комнате сейва 49 30 ламп) держатся за своими лампами, лампа
 *     входит в место и уходит из него за 0.4 с; дымка - у каждого места, так же плавно, её доля - ghz (SH_UBO_GL3 + 128,
 *     dc_vol_common.glsl glHazeW); место получает лампа, чей свет достаёт до кадра (шар её радиуса задевает пирамиду вида),
 *     ярче и ближе к камере - раньше;
 * (3) быстрая загрузка, новая комната (8 кадров A) и смена ракурса (3) - всё сразу на месте, без плавного входа.
 * Треугольники «светится» не из меша (группы видимого) - как было, только за краем кадра их свет держится до 10 с, а новый
 * входит за 0.25 с. Комнату рисует игра (обход стоит) - всё как было (gl_steady_out). DC_GLOW_LIGHT_ROOM=0 - как было;
 * DC_GLOW_LIGHT_LOG=2 - строка glowroom2 на каждый кадр A, 3 - ещё glowcal (светильник в кадре: поток против меша). */
enum { GLR_N = 160, GLR_TRI = 2048, GLR_FIX = 128, GLR_OUT = 32 };
struct GlRTrack {
    float pos[3];      /* место в комнате: у светильника - его центр, у группы - сглаженное место */
    float e[3];        /* свет горящей фазы: цвет × множитель силы / 255 */
    float ki, kr;      /* множители силы и радиуса (по площади) */
    float peak;        /* группа: наибольший канал с медленным спадом, как у gl_steady_out */
    float fast[3];     /* быстрое среднее мгновенного света (поток) */
    double seen;       /* когда лампу видели в потоке, с */
    double lastOn;     /* когда горела: в потоке не темнее 85 % пика или вне кадра (судить нельзя) */
    double judged;     /* группа: когда видели в потоке или она была вне кадра */
    int alive, hit, id;
    int fi;            /* светильник этого кадра (-1 - группа или его больше нет) */
    int meshOk;        /* свет светильника уже взят по мешу */
    int miss, blinks;  /* как у gl_steady_out: лампы не было в потоке; сколько раз она так пропадала и возвращалась */
    int slot;          /* место 0..31, -1 - нет */
    int want;          /* в этом кадре ей нужно место */
    int vis;           /* 1 - в потоке, 2 - в кадре, а в потоке нет (тёмная фаза), 0 - судить нельзя (вне кадра) */
    int drop;          /* группа на месте светильника, у которого теперь свой след: уходит за 0.3 с */
    unsigned fix;      /* ключ светильника (0 - группа видимого) */
    float fade;        /* доля света без лампы в потоке (немигавшая гаснет за 0.15 с) */
    float pw;          /* доля присутствия: светильник рисует обход, группа видна (0..1) */
    float lw;          /* доля света в месте (0..1, за 0.4 с), дымка - по ней же */
};
struct GlRTri { float col[3], n[3], p0[3], area; int fix; unsigned char ok, two; };
static GlRTrack s_glr[GLR_N];
static GlRTri   s_glrT[GLR_TRI];
static int      s_glrSlot[GLR_OUT];                /* место -> след, -1 - пусто */
static int      s_glrTn = 0, s_glrOn = 0, s_glrInst = 0, s_glrNextId = 1, s_glrHave = 0;
static unsigned s_glrGen = 0;
static long     s_glrTick = 0, s_glrCal = 0;
static double   s_glrPrev = 0.0;
static float    s_glrEye[3], s_glrFwd[3];
static float    s_glFr[4] = { 0.f, 0.f, 0.f, 0.f };   /* пирамида вида кадра A: x/z слева и справа, y/z сверху и снизу (dcvk_render) */
static int      s_glFrOk = 0;
static void gl_room_reset(void) {
    for (int t = 0; t < GLR_N; t++) { s_glr[t].alive = 0; s_glr[t].slot = -1; }
    for (int j = 0; j < GLR_OUT; j++) s_glrSlot[j] = -1;
    s_glrOn = 0; s_glrHave = 0; s_glrTn = 0;
}
extern "C" int dc_glow_fixture_present(unsigned char* pres, int cap, unsigned char* td, int tcap, long* walk_seq, unsigned* gen);
extern "C" int dc_glow_fixture_tri(int i, int* fix, int* tp, int* cx, int* cy, int uv[6], int rgb[9], int* code, float* area, float v9[9], int* two);
static inline float gl_sstep(float a, float b, float x) {
    float t = b > a ? (x - a) / (b - a) : (x >= b ? 1.f : 0.f);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}
/* 28.10 (2): страница источника - как её читает поток (gl_lights_build): (int)(v[8] + 0.5f) во float, а у «светится» в
 * полосе страницы бит 23 - число не меньше 2^23, шаг float там 1, и +0.5 округляется к чётному: нечётная страница читается
 * как следующая (слот 49: плафоны рисуются со страницы 0x17, свет всегда брал цвет текселей 0x18). Свет по мешу берёт так
 * же - цвет света как был. DC_GLOW_LIGHT_TRUEPAGE=1 - оба пути по настоящей странице (цвет самой лампы) */
static int gl_true_page(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_GLOW_LIGHT_TRUEPAGE"); v = (e && *e == '1') ? 1 : 0; }
    return v;
}
static inline int gl_lane_page(float lane) { return (gl_true_page() ? (int)((double)lane + 0.5) : (int)(lane + 0.5f)) & 0x1FF; }
/* цвет треугольника светильника по мешу - как у потока (gl_lights_build): 7 текселей окна UV, прозрачные мимо;
 * полупрозрачное с цветом вершин - оттенок вершины (glow_light_hue); 0 - не светит */
static int gl_room_tricol(const unsigned short* vr, int tp, int cx, int cy, const int* uv, const int* rgb, int code, float* col) {
    tp = gl_lane_page((float)((tp & 0x1FF) | (1 << 23)));               /* страница - как у потока (выше) */
    const float u0 = (float)uv[0], v0 = (float)uv[1], u1 = (float)uv[2], v1 = (float)uv[3], u2 = (float)uv[4], v2 = (float)uv[5];
    const float uvs[7][2] = { { u0, v0 }, { u1, v1 }, { u2, v2 }, { (u0 + u1) * 0.5f, (v0 + v1) * 0.5f }, { (u1 + u2) * 0.5f, (v1 + v2) * 0.5f },
                              { (u0 + u2) * 0.5f, (v0 + v2) * 0.5f }, { (u0 + u1 + u2) / 3.f, (v0 + v1 + v2) / 3.f } };
    col[0] = col[1] = col[2] = 0.f;
    int nn = 0;
    for (int s = 0; s < 7; s++) {
        float c[3]; int ok = 0;
        gl_texel_rgb(vr, tp, cx, cy, (int)uvs[s][0], (int)uvs[s][1], c, &ok);
        if (ok) { col[0] += c[0]; col[1] += c[1]; col[2] += c[2]; nn++; }
    }
    if (!nn) return 0;
    for (int q = 0; q < 3; q++) col[q] /= (float)nn;
    if ((code & 2) && !(code & 1)) {
        float vcm[3], mx = 0.f;
        for (int q = 0; q < 3; q++) { vcm[q] = (float)(rgb[q] + rgb[3 + q] + rgb[6 + q]) / 3.f; if (vcm[q] > mx) mx = vcm[q]; }
        if (s_pp[PP_GL_LHUE] > 0.5f) {
            if (mx < 2.f) return 0;
            for (int q = 0; q < 3; q++) col[q] = col[q] * vcm[q] / mx;
        } else {
            for (int q = 0; q < 3; q++) { col[q] = col[q] * vcm[q] / 128.f; if (col[q] > 255.f) col[q] = 255.f; }
        }
    }
    return 1;
}
/* треугольники светильников по мешу (pt_renderer.c dc_glow_fixture_tri): новая сборка светильников - всё заново; иначе раз
 * в ~секунду цвет заново (экраны и лампы анимированы), остаётся более яркий - свет горящей фазы */
static void gl_room_mesh(const unsigned short* vr, unsigned gen) {
    const int fresh = gen != s_glrGen || !s_glrTn;
    if (!fresh && (++s_glrTick % 60) != 0) return;
    if (fresh) { s_glrGen = gen; s_glrTn = 0; }
    const int lim = fresh ? GLR_TRI : s_glrTn;
    for (int i = 0; i < lim; i++) {
        int fix = 0, tp = 0, cx = 0, cy = 0, uv[6], rgb[9], code = 0, two = 0; float area = 0.f, v9[9];
        if (!dc_glow_fixture_tri(i, &fix, &tp, &cx, &cy, uv, rgb, &code, &area, v9, &two)) break;
        GlRTri& M = s_glrT[i];
        float col[3];
        const int ok = gl_room_tricol(vr, tp, cx, cy, uv, rgb, code, col);
        if (fresh) {
            const float e1[3] = { v9[3] - v9[0], v9[4] - v9[1], v9[5] - v9[2] }, e2[3] = { v9[6] - v9[0], v9[7] - v9[1], v9[8] - v9[2] };
            M.n[0] = e1[1] * e2[2] - e1[2] * e2[1]; M.n[1] = e1[2] * e2[0] - e1[0] * e2[2]; M.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            for (int q = 0; q < 3; q++) { M.p0[q] = v9[q]; M.col[q] = ok ? col[q] : 0.f; }
            M.area = area; M.fix = fix; M.ok = (unsigned char)(ok ? 1 : 0); M.two = (unsigned char)(two ? 1 : 0);
            s_glrTn = i + 1;
        } else if (ok && (!M.ok || gl_max3(col) > gl_max3(M.col))) {
            M.ok = 1;
            for (int q = 0; q < 3; q++) M.col[q] = col[q];
        }
    }
}
/* насколько точка вида (кадр A) вне пирамиды вида: 0 - внутри; иначе сильнее всего нарушенная грань (ближняя - z = 50) -
 * не больше настоящего расстояния до пирамиды, свет у её края не теряется */
static float gl_room_outside(const float* v) {
    float s = 50.f - v[2];
    if (s_glFrOk) {
        const float kx0 = s_glFr[0], kx1 = s_glFr[1], ky0 = s_glFr[2], ky1 = s_glFr[3];
        const float a = (kx0 * v[2] - v[0]) / sqrtf(1.f + kx0 * kx0), b = (v[0] - kx1 * v[2]) / sqrtf(1.f + kx1 * kx1);
        const float c = (ky0 * v[2] - v[1]) / sqrtf(1.f + ky0 * ky0), d = (v[1] - ky1 * v[2]) / sqrtf(1.f + ky1 * ky1);
        if (a > s) s = a;
        if (b > s) s = b;
        if (c > s) s = c;
        if (d > s) s = d;
    }
    return s > 0.f ? s : 0.f;
}
static void gl_room_kill(int t) {
    GlRTrack& T = s_glr[t];
    if (T.slot >= 0 && T.slot < GLR_OUT && s_glrSlot[T.slot] == t) s_glrSlot[T.slot] = -1;
    T.slot = -1; T.alive = 0;
}
static int gl_room_out(const GlClus* cl, int ncl, const unsigned short* vr, const float* fxC, const float* fxB, const unsigned* fxK,
                       int nf, const unsigned char* pres, const unsigned char* td, unsigned gen, int walkAge) {
    static int lg = -1;
    if (lg < 0) { const char* e = getenv("DC_GLOW_LIGHT_LOG"); lg = (e && *e >= '1' && *e <= '9') ? *e - '0' : 0; }
    LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
    const double now = (double)qc.QuadPart / (double)qf.QuadPart;
    float dt = s_glrPrev > 0.0 ? (float)(now - s_glrPrev) : 0.f;
    const int gap = s_glrPrev > 0.0 && now - s_glrPrev > 0.5;           /* полсекунды без кадра A (меню, экран предметов) */
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.25f) dt = 0.25f;
    s_glrPrev = now;
    const float* V2R = s_shUboMap + 260;
    const float* R2V = s_shUboMap + 272;
    const float flick = s_pp[PP_GL_LFLICK], hold = s_pp[PP_GL_LHOLD];
    const float aFast = 1.f - expf(-dt / 0.15f), aE = 1.f - expf(-dt / 0.3f), kDim = expf(-dt / 0.3f), kGone = expf(-dt / 0.15f);
    const float RAMP = 0.4f;                                             /* вход в место и уход из него, с */
    /* вход в этот режим, новая комната, быстрая загрузка, перерыв: следы с нуля, всё сразу */
    { static int ep = -1; const int e = dc_light_epoch();
      if (!s_glrOn || e != ep || gap) {
          ep = e;
          for (int t = 0; t < GLR_N; t++) { s_glr[t].alive = 0; s_glr[t].slot = -1; }
          for (int j = 0; j < GLR_OUT; j++) s_glrSlot[j] = -1;
          s_glrTn = 0; s_glrHave = 0; s_glrInst = 8;
      } }
    s_glrOn = 1;
    int cut = 0;                                                         /* смена ракурса - как у gl_steady_out */
    {
        const float eye[3] = { V2R[3], V2R[7], V2R[11] };
        float fwd[3] = { V2R[2], V2R[6], V2R[10] };
        const float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
        if (fl > 1e-6f) for (int q = 0; q < 3; q++) fwd[q] /= fl;
        if (s_glrHave) {
            const float dx = eye[0] - s_glrEye[0], dy = eye[1] - s_glrEye[1], dz = eye[2] - s_glrEye[2];
            const float dot = fwd[0] * s_glrFwd[0] + fwd[1] * s_glrFwd[1] + fwd[2] * s_glrFwd[2];
            if (dx * dx + dy * dy + dz * dz > 1200.f * 1200.f || dot < 0.95f) cut = 1;
        }
        for (int q = 0; q < 3; q++) { s_glrEye[q] = eye[q]; s_glrFwd[q] = fwd[q]; }
        s_glrHave = 1;
    }
    if (cut) {                                                           /* группы видимого - с нуля, места - сразу */
        for (int t = 0; t < GLR_N; t++) if (s_glr[t].alive && !s_glr[t].fix) gl_room_kill(t);
        if (s_glrInst < 3) s_glrInst = 3;
    }
    const int inst = s_glrInst > 0;
    /* свет светильников по мешу: треугольники, которые обход рисует (td), лицом к камере или двусторонние
     * (лицо: знак det(V2R) × (n · (p0 - глаз)) >= 0 - как у отсечения изнанки в обходе) */
    gl_room_mesh(vr, gen);
    const float det = V2R[0] * (V2R[5] * V2R[10] - V2R[6] * V2R[9]) - V2R[1] * (V2R[4] * V2R[10] - V2R[6] * V2R[8])
                    + V2R[2] * (V2R[4] * V2R[9] - V2R[5] * V2R[8]);
    const float sg = det < 0.f ? -1.f : 1.f;
    const float eye[3] = { V2R[3], V2R[7], V2R[11] };
    static float fA[GLR_FIX], fC[GLR_FIX][3], fA2[GLR_FIX], fC2[GLR_FIX][3];
    for (int f = 0; f < nf; f++) { fA[f] = fA2[f] = 0.f; for (int q = 0; q < 3; q++) fC[f][q] = fC2[f][q] = 0.f; }
    for (int i = 0; i < s_glrTn; i++) {
        const GlRTri& M = s_glrT[i];
        if (!M.ok || !td[i] || M.fix < 0 || M.fix >= nf) continue;
        fA2[M.fix] += M.area;
        for (int q = 0; q < 3; q++) fC2[M.fix][q] += M.col[q] * M.area;
        const float d = M.n[0] * (M.p0[0] - eye[0]) + M.n[1] * (M.p0[1] - eye[1]) + M.n[2] * (M.p0[2] - eye[2]);
        if (!M.two && sg * d < 0.f) continue;
        fA[M.fix] += M.area;
        for (int q = 0; q < 3; q++) fC[M.fix][q] += M.col[q] * M.area;
    }
    /* следы светильников - по ключу */
    static int f2t[GLR_FIX], fcl[GLR_FIX];
    for (int f = 0; f < nf; f++) { f2t[f] = -1; fcl[f] = -1; }
    for (int t = 0; t < GLR_N; t++) {
        GlRTrack& T = s_glr[t];
        T.hit = 0; T.vis = 0;
        if (!T.alive || !T.fix) continue;
        T.fi = -1;
        for (int f = 0; f < nf; f++) if (fxK[f] == T.fix && f2t[f] < 0) { f2t[f] = t; T.fi = f; break; }
    }
    auto take = [&](void) -> int {                                       /* свободный след; нет - самая слабая невидимая группа */
        for (int t = 0; t < GLR_N; t++) if (!s_glr[t].alive) return t;
        int best = -1; float wp = 1e30f;
        for (int t = 0; t < GLR_N; t++) {
            const GlRTrack& T = s_glr[t];
            if (T.fix || T.hit) continue;
            const float w = (T.slot >= 0 ? 10.f : 0.f) + T.peak * T.pw;
            if (w < wp) { wp = w; best = t; }
        }
        if (best >= 0) gl_room_kill(best);
        return best;
    };
    auto boxD2 = [&](const float* B, const float* p) -> float {          /* квадрат расстояния от точки до рамки */
        float d2 = 0.f;
        for (int q = 0; q < 3; q++) { const float o = p[q] < B[q] ? B[q] - p[q] : p[q] > B[3 + q] ? p[q] - B[3 + q] : 0.f; d2 += o * o; }
        return d2;
    };
    /* новый след - у каждого светильника, которого рисует обход; группа видимого на его месте становится его следом
     * (свет не гаснет и не входит заново), прочие группы там уходят */
    for (int f = 0; f < nf; f++) {
        if (f2t[f] >= 0 || !pres[f]) continue;
        const float* B = fxB + f * 6;
        int t = -1;
        if (!inst) {
            float bd = 250.f * 250.f;
            for (int u = 0; u < GLR_N; u++) {
                const GlRTrack& G = s_glr[u];
                if (!G.alive || G.fix) continue;
                const float d2 = boxD2(B, G.pos);
                if (d2 < bd) { bd = d2; t = u; }
            }
        }
        if (t >= 0) {
            GlRTrack& T = s_glr[t];
            if (lg >= 2) std::fprintf(stderr, "glowroom: group #%d -> fixture F%d%c", T.id, f, 10);
            T.fix = fxK[f]; T.meshOk = 0; T.drop = 0;
        } else {
            t = take();
            if (t < 0) break;
            GlRTrack& T = s_glr[t];
            memset(&T, 0, sizeof T);
            T.alive = 1; T.id = s_glrNextId++; T.fix = fxK[f]; T.slot = -1;
            T.fade = 1.f; T.pw = inst ? 1.f : 0.f; T.seen = T.lastOn = T.judged = now;
            for (int q = 0; q < 3; q++) T.pos[q] = fxC[f * 4 + q];
            if (lg >= 2) std::fprintf(stderr, "glowroom: new #%d F%d at %.0f,%.0f,%.0f area %.0f%s%c", T.id, f, fxC[f * 4], fxC[f * 4 + 1],
                                      fxC[f * 4 + 2], fxC[f * 4 + 3], inst ? " (instant)" : "", 10);
        }
        s_glr[t].fi = f; f2t[f] = t;
        if (!inst)
            for (int u = 0; u < GLR_N; u++) {
                GlRTrack& G = s_glr[u];
                if (G.alive && !G.fix && !G.drop && boxD2(B, G.pos) < 250.f * 250.f) G.drop = 1;
            }
    }
    /* свет светильника - по мешу (лицом к камере; первый взгляд изнанкой - по всем его треугольникам); присутствие - обход */
    for (int f = 0; f < nf; f++) {
        const int t = f2t[f];
        if (t < 0) continue;
        GlRTrack& T = s_glr[t];
        const float* C = fxC + f * 4;
        const float ap = inst ? 1.f : aE;
        for (int q = 0; q < 3; q++) T.pos[q] += (C[q] - T.pos[q]) * ap;
        float A = fA[f]; const float* S = fC[f];
        if (A < 1.f && !T.meshOk) { A = fA2[f]; S = fC2[f]; }
        if (A >= 1.f) {
            const float sz = sqrtf(A) / 300.f;
            const float ki = sz < 0.35f ? 0.35f : (sz > 1.5f ? 1.5f : sz), kr = sz < 0.6f ? 0.6f : (sz > 1.6f ? 1.6f : sz);
            const float a = (inst || !T.meshOk) ? 1.f : aE;
            for (int q = 0; q < 3; q++) T.e[q] += (S[q] / A * ki / 255.f - T.e[q]) * a;
            T.ki += (ki - T.ki) * a; T.kr += (kr - T.kr) * a;
            if (!T.meshOk) { for (int q = 0; q < 3; q++) T.fast[q] = T.e[q]; T.meshOk = 1; }
        }
        if (pres[f]) T.pw = inst ? 1.f : (T.pw + dt / 0.3f > 1.f ? 1.f : T.pw + dt / 0.3f);
        else T.pw = inst ? 0.f : (T.pw - dt / 0.3f < 0.f ? 0.f : T.pw - dt / 0.3f);
    }
    for (int t = 0; t < GLR_N; t++) {                                    /* светильника больше нет в таблице - уходит */
        GlRTrack& T = s_glr[t];
        if (T.alive && T.fix && T.fi < 0) T.pw = inst ? 0.f : (T.pw - dt / 0.3f < 0.f ? 0.f : T.pw - dt / 0.3f);
    }
    /* поток кадра: у светильника - мигание и тёмная фаза; прочее - группы видимого, как у gl_steady_out */
    enum { GLR_C = 160 };
    int ord[GLR_C]; float en[GLR_C][3], enm[GLR_C], kin[GLR_C], krn[GLR_C];
    const int nc = ncl < GLR_C ? ncl : GLR_C;
    for (int c = 0; c < nc; c++) {
        const double A = cl[c].a;
        const double sz = sqrt(A) / 300.0;
        kin[c] = (float)(sz < 0.35 ? 0.35 : sz > 1.5 ? 1.5 : sz);
        krn[c] = (float)(sz < 0.6 ? 0.6 : sz > 1.6 ? 1.6 : sz);
        for (int q = 0; q < 3; q++) en[c][q] = (float)(cl[c].c[q] / A) * kin[c] / 255.f;
        enm[c] = gl_max3(en[c]); ord[c] = c;
    }
    for (int a = 1; a < nc; a++) { const int k = ord[a]; int b = a - 1; while (b >= 0 && enm[ord[b]] < enm[k]) { ord[b + 1] = ord[b]; b--; } ord[b + 1] = k; }
    for (int oi = 0; oi < nc; oi++) {
        const int c = ord[oi];
        const double A = cl[c].a;
        const int f = cl[c].fi;
        if (cl[c].fix && f >= 0 && f < nf && pres[f] && f2t[f] >= 0 && !s_glr[f2t[f]].hit) {
            GlRTrack& T = s_glr[f2t[f]];
            if (!T.meshOk) {                                             /* по мешу цвета нет (тексели не взялись) - по потоку */
                const float a = gl_max3(T.e) > 0.f ? aE : 1.f;
                for (int q = 0; q < 3; q++) T.e[q] += (en[c][q] - T.e[q]) * a;
                T.ki += (kin[c] - T.ki) * a; T.kr += (krn[c] - T.kr) * a;
            }
            for (int q = 0; q < 3; q++) T.fast[q] += ((float)(cl[c].c[q] / A) * T.ki / 255.f - T.fast[q]) * aFast;
            if (T.miss) { T.miss = 0; if (T.blinks < 1000) T.blinks++; }   /* пропадала и вернулась - мигает */
            T.fade = inst ? 1.f : (T.fade + dt / 0.3f > 1.f ? 1.f : T.fade + dt / 0.3f);
            T.seen = T.lastOn = T.judged = now; T.hit = 1; T.vis = 1;
            fcl[f] = c;
            continue;
        }
        const float vw[3] = { (float)(cl[c].p[0] / A), (float)(cl[c].p[1] / A), (float)(cl[c].p[2] / A) };
        float rp[3]; gl_xf(V2R, vw, rp);
        int best = -1; float bd = 600.f * 600.f;
        for (int t = 0; t < GLR_N; t++) {
            const GlRTrack& T = s_glr[t];
            if (!T.alive || T.hit || T.fix) continue;
            const float dx = T.pos[0] - rp[0], dy = T.pos[1] - rp[1], dz = T.pos[2] - rp[2], d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = t; }
        }
        if (best < 0) {
            best = take();
            if (best < 0) continue;
            GlRTrack& T = s_glr[best];
            memset(&T, 0, sizeof T);
            T.alive = 1; T.hit = 1; T.vis = 1; T.id = s_glrNextId++; T.fi = -1; T.slot = -1; T.seen = T.lastOn = T.judged = now;
            for (int q = 0; q < 3; q++) { T.pos[q] = rp[q]; T.e[q] = en[c][q]; T.fast[q] = en[c][q]; }
            T.ki = kin[c]; T.kr = krn[c]; T.peak = enm[c]; T.fade = 1.f; T.pw = inst ? 1.f : 0.f;
            continue;
        }
        GlRTrack& T = s_glr[best];
        for (int q = 0; q < 3; q++) { T.pos[q] += (rp[q] - T.pos[q]) * 0.35f; T.fast[q] += (en[c][q] - T.fast[q]) * aFast; }
        if (T.miss) { T.miss = 0; if (T.blinks < 1000) T.blinks++; }
        T.fade = 1.f;
        float pk = (now - T.lastOn > hold) ? T.peak * kDim : T.peak;
        if (enm[c] > pk) pk = enm[c];
        if (enm[c] >= 0.85f * pk) {
            for (int q = 0; q < 3; q++) T.e[q] += (en[c][q] - T.e[q]) * 0.5f;
            T.kr += (krn[c] - T.kr) * 0.5f;
            T.lastOn = now;
        }
        T.peak = pk; T.seen = T.judged = now; T.hit = 1; T.vis = 1;
        if (!T.drop) T.pw = inst ? 1.f : (T.pw + dt / 0.25f > 1.f ? 1.f : T.pw + dt / 0.25f);
    }
    /* следы без лампы в потоке. Светильник: целиком в кадре, лицом, обход рисует - а в потоке нет: тёмная фаза (немигавший
     * гаснет за 0.15 с, мигавший держится hold); иначе судить нельзя - свет как был. Группа: место в кадре - как у
     * gl_steady_out; за краем кадра свет держится до 10 с */
    float sx0 = 1e30f, sx1 = -1e30f, sy0 = 1e30f, sy1 = -1e30f; int haveB = 0;
    auto centerIn = [&](const float* pos) -> int {                       /* точка в кадре, отступ 5 % внутрь */
        float v[3]; gl_xf(R2V, pos, v);
        if (v[2] <= 50.f) return 0;
        if (s_glFrOk) {
            const float x = v[0] / v[2], y = v[1] / v[2], mx = (s_glFr[1] - s_glFr[0]) * 0.05f, my = (s_glFr[3] - s_glFr[2]) * 0.05f;
            return x >= s_glFr[0] + mx && x <= s_glFr[1] - mx && y >= s_glFr[2] + my && y <= s_glFr[3] - my;
        }
        if (!haveB) {                                                    /* нет пирамиды - рамка 3D-кадра по вершинам потока */
            haveB = 1;
            for (int i = 0; i < s_nverts; i++) {
                const float* w = s_vmap + (size_t)i * VERT_FLOATS;
                if (w[12] < 1.5f) continue;
                if (w[0] < sx0) sx0 = w[0];
                if (w[0] > sx1) sx1 = w[0];
                if (w[1] < sy0) sy0 = w[1];
                if (w[1] > sy1) sy1 = w[1];
            }
        }
        if (sx1 <= sx0 || sy1 <= sy0) return 0;
        const float sx = v[0] * s_shWp[3] / v[2] + s_shWp[0], sy = v[1] * s_shWp[2] / v[2] + s_shWp[1];
        const float mx = (sx1 - sx0) * 0.05f, my = (sy1 - sy0) * 0.05f;
        return sx >= sx0 + mx && sx <= sx1 - mx && sy >= sy0 + my && sy <= sy1 - my;
    };
    auto boxIn = [&](int f) -> int {                                     /* рамка светильника целиком в кадре */
        if (!s_glFrOk) return 0;
        const float* B = fxB + f * 6;
        for (int k = 0; k < 8; k++) {
            const float p[3] = { B[(k & 1) ? 3 : 0], B[(k & 2) ? 4 : 1], B[(k & 4) ? 5 : 2] };
            float v[3]; gl_xf(R2V, p, v);
            if (v[2] <= 50.f) return 0;
            const float x = v[0] / v[2], y = v[1] / v[2];
            if (x < s_glFr[0] || x > s_glFr[1] || y < s_glFr[2] || y > s_glFr[3]) return 0;
        }
        return 1;
    };
    for (int t = 0; t < GLR_N; t++) {
        GlRTrack& T = s_glr[t];
        if (!T.alive || T.hit) continue;
        if (T.fix) {
            const int f = T.fi;
            if (f >= 0 && pres[f] && fA[f] >= 1.f && boxIn(f)) {
                T.vis = 2;
                for (int q = 0; q < 3; q++) T.fast[q] -= T.fast[q] * aFast;
                T.miss = 1;
                if (!T.blinks || now - T.lastOn > hold) T.fade *= kGone;
            } else T.lastOn = now;                                       /* не видно - считаем горящей: вернётся в кадр - не мигнёт */
            if (T.pw <= 0.f && (f < 0 || !pres[f])) gl_room_kill(t);
            continue;
        }
        if (centerIn(T.pos)) {
            const float age = (float)(now - T.judged);
            T.vis = 2;
            for (int q = 0; q < 3; q++) T.fast[q] -= T.fast[q] * aFast;
            T.miss = 1;
            if (gl_fix_on() && !T.blinks) T.fade *= kGone;
            else if (age > hold) T.peak *= kGone;
            if (age > hold + 0.5f || T.peak < 0.002f) T.pw -= dt / 0.15f;
        } else {
            T.judged = now;
            if (now - T.seen > 10.0) T.pw -= dt / 0.3f;
        }
        if (T.drop) T.pw -= dt / 0.3f;
        if (T.pw <= 0.f) gl_room_kill(t);
    }
    /* свет следа, место в виде и важность: свет × близость к камере × доля шара света (или дымки, если он больше), задевающая
     * пирамиду вида; лампе вне кадра - малая доля: место у неё есть, пока мест хватает, - вернётся в кадр уже горящей */
    static float eo[GLR_N][3], vo[GLR_N][3], rr[GLR_N], reach[GLR_N], pri[GLR_N];
    static unsigned char ro[GLR_N];
    int cand[GLR_N], nt = 0;
    for (int t = 0; t < GLR_N; t++) {
        GlRTrack& T = s_glr[t];
        T.want = 0; ro[t] = 0;
        if (!T.alive) continue;
        const float em = gl_max3(T.e), s = T.fix ? 1.f : (em > 1e-6f ? T.peak / em : 0.f);
        for (int q = 0; q < 3; q++) { const float x = (T.e[q] * s * (1.f - flick) + T.fast[q] * flick) * T.fade * T.pw; eo[t][q] = x > 0.f ? x : 0.f; }
        gl_xf(R2V, T.pos, vo[t]);
        const float dd = (vo[t][0] * vo[t][0] + vo[t][1] * vo[t][1] + vo[t][2] * vo[t][2]) / (5000.f * 5000.f);
        const float R = s_pp[PP_GL_LRAD] * T.kr, Rw = R * (s_pp[PP_VL_RAD] > 1.f ? s_pp[PP_VL_RAD] : 1.f), out = gl_room_outside(vo[t]);
        const float m = gl_max3(eo[t]) / (1.f + dd);
        rr[t] = R;
        reach[t] = 1.f - gl_sstep(0.5f * Rw, Rw, out);
        ro[t] = (unsigned char)(out < 1.5f * Rw);                        /* в GPU: свет или дымка достаёт до кадра (запас - кадр B) */
        pri[t] = m * (reach[t] + 0.02f * (1.f - gl_sstep(Rw, 2.f * Rw, out)) + 0.001f);
        if (m > 1e-6f) cand[nt++] = t;
    }
    for (int a = 1; a < nt; a++) {                                       /* по важности; у занявших место - с запасом 1.2 */
        const int k = cand[a]; const float pk = pri[k] * (s_glr[k].slot >= 0 ? 1.2f : 1.f);
        int b = a - 1;
        while (b >= 0 && pri[cand[b]] * (s_glr[cand[b]].slot >= 0 ? 1.2f : 1.f) < pk) { cand[b + 1] = cand[b]; b--; }
        cand[b + 1] = k;
    }
    const int nw = nt < GLR_OUT ? nt : GLR_OUT;
    for (int k = 0; k < nw; k++) s_glr[cand[k]].want = 1;
    /* места: доля света входит и уходит за RAMP; ушедший - место свободно */
    for (int j = 0; j < GLR_OUT; j++) {
        const int t = s_glrSlot[j];
        if (t < 0) continue;
        GlRTrack& T = s_glr[t];
        if (!T.alive || T.slot != j) { s_glrSlot[j] = -1; continue; }
        if (T.want) T.lw = inst ? 1.f : (T.lw + dt / RAMP > 1.f ? 1.f : T.lw + dt / RAMP);
        else {
            T.lw = inst ? 0.f : T.lw - dt / RAMP;
            if (T.lw <= 0.f) { T.lw = 0.f; T.slot = -1; s_glrSlot[j] = -1; }
        }
    }
    for (int k = 0; k < nw; k++) {                                       /* новым - первое свободное место */
        const int t = cand[k];
        GlRTrack& T = s_glr[t];
        if (T.slot >= 0) continue;
        int j = -1;
        for (int s = 0; s < GLR_OUT && j < 0; s++) if (s_glrSlot[s] < 0) j = s;
        if (j < 0) break;
        s_glrSlot[j] = t; T.slot = j; T.lw = inst ? 1.f : 0.f;
    }
    /* вывод в GPU - подряд, по порядку мест, только те, чей свет или дымка достаёт до кадра (прочие держат место, но растр и
     * дымка их не перебирают); дымка - у всех по доле света (цвет уже × lw); пустое - нули (радиус 0 - растр, дымка и пыль его
     * пропускают) */
    int no = 0, nused = 0;
    for (int j = 0; j < GLR_OUT; j++) {
        const int t = s_glrSlot[j];
        if (t < 0) continue;
        nused++;
        if (!ro[t]) continue;
        const GlRTrack& T = s_glr[t];
        float* gp = gl_slot_p(no);
        float* gc = gl_slot_c(no);
        gp[0] = vo[t][0]; gp[1] = vo[t][1]; gp[2] = vo[t][2]; gp[3] = rr[t];
        for (int q = 0; q < 3; q++) gc[q] = eo[t][q] * s_pp[PP_GL_LSTR] * T.lw;
        gc[3] = gl_chr_smooth() ? 0.25f : -1.25f;                       /* 28.09: знак < 0 - персонажи по граням, как было */
        s_shUboMap[SH_UBO_GL3 + 128 + no] = 1.f;
        no++;
    }
    for (int j = no; j < GLR_OUT; j++) {
        float* gp = gl_slot_p(j);
        float* gc = gl_slot_c(j);
        gp[0] = gp[1] = gp[2] = gp[3] = 0.f; gc[0] = gc[1] = gc[2] = gc[3] = 0.f;
        s_shUboMap[SH_UBO_GL3 + 128 + j] = 0.f;
    }
    gl_set_count(no);
    if (lg >= 2) {
        char buf[6000]; int p = 0, npres = 0, ntr = 0;
        for (int f = 0; f < nf; f++) npres += pres[f];
        for (int t = 0; t < GLR_N; t++) ntr += s_glr[t].alive;
        p += snprintf(buf + p, sizeof buf - p, "glowroom2: t=%.3f fixtures %d present %d tracks %d walk %d inst %d out %d gpu %d%s |",
                      now, nf, npres, ntr, walkAge, s_glrInst, nused, no, cut ? " SHOT CHANGE" : "");
        for (int j = 0; j < GLR_OUT && p < (int)sizeof buf - 200; j++) {
            const int t = s_glrSlot[j];
            if (t < 0) continue;
            const GlRTrack& T = s_glr[t];
            char nm[16];
            if (T.fix) snprintf(nm, sizeof nm, "F%d", T.fi); else snprintf(nm, sizeof nm, "grp");
            p += snprintf(buf + p, sizeof buf - p, " s%d #%d %s e %.4f r %.2f lw %.2f g%d pw %.2f f %.2f %s;", j, T.id, nm,
                          gl_max3(eo[t]) * s_pp[PP_GL_LSTR] * T.lw, reach[t], T.lw, (int)ro[t], T.pw, T.fade,
                          T.vis == 1 ? "vis" : T.vis == 2 ? "held" : "out");
        }
        p += snprintf(buf + p, sizeof buf - p, " | no slot:");
        for (int k = 0; k < nt && p < (int)sizeof buf - 40; k++) {
            const GlRTrack& T = s_glr[cand[k]];
            if (T.slot >= 0) continue;
            p += snprintf(buf + p, sizeof buf - p, " #%d %.4f%s", T.id, pri[cand[k]], T.vis == 1 ? "v" : "");
        }
        std::fprintf(stderr, "%s%c", buf, 10);
    }
    if (lg >= 3 && (s_glrCal++ % 30) == 0)                               /* светильник целиком в кадре: поток против меша */
        for (int f = 0; f < nf; f++) {
            const int c = fcl[f];
            if (c < 0 || !boxIn(f)) continue;
            const double A = cl[c].a;
            const float am = fA[f] > 0.f ? fA[f] : 1.f;
            std::fprintf(stderr, "glowcal: F%d stream area %.0f color %.0f,%.0f,%.0f | mesh facing area %.0f color %.0f,%.0f,%.0f | drawn %.0f | fixture %.0f%c",
                         f, A, cl[c].c[0] / A, cl[c].c[1] / A, cl[c].c[2] / A, fA[f], fC[f][0] / am, fC[f][1] / am, fC[f][2] / am,
                         fA2[f], fxC[f * 4 + 3], 10);
        }
    if (s_glrInst > 0) s_glrInst--;
    return no;
}
static void gl_lights_build(void) {
    s_glN = 0;
    if (!s_shUboMap) return;
    gl_set_count(0);
    for (int j = 0; j < 32; j++) s_shUboMap[SH_UBO_GL3 + 128 + j] = j < 8 ? 1.f : 0.f;   /* 28.10: дымка - у первых 8, как было (gl_room_out пишет свою) */
    if (s_pp[PP_GL_LON] < 0.5f || !g_dc_gfx_materials || dc_orig_nogfx() || !s_vmap || s_nverts < 3 || s_shWp[3] == 0.f || s_shWp[2] == 0.f) {
        if (s_glrOn) gl_room_reset();
        return;
    }
    const unsigned short* vr = sw_vram_base();
    if (!vr) { if (s_glrOn) gl_room_reset(); return; }
    enum { GLC = 160 };    /* 28.10: до 160 групп - все светильники кадра (gl_room_out); прежним путям - первые 32, как было */
    GlClus cl[GLC];
    int ncl = 0;
    const float MERGE = 800.f;
    /* 27.09: DC_GLOW_LIGHT_TESTBLINK=N — ПРОВЕРКА ровного света: все «светится» гаснут на вторую половину каждых N кадров A
     * (N = 2 — как плашка игры через кадр, N = 20 — медленное мигание); 0 — нет */
    static int tbN = -1; static long tbK = 0;
    if (tbN < 0) { const char* e = getenv("DC_GLOW_LIGHT_TESTBLINK"); tbN = e ? atoi(e) : 0; if (tbN < 2) tbN = 0;
                   if (tbN) std::fprintf(stderr, "glowlight: TEST - 'glows' lamps go out for %d of every %d frames (DC_GLOW_LIGHT_TESTBLINK)%c", tbN / 2, tbN, 10); }
    const int tbOff = tbN ? (int)((tbK++ % tbN) >= tbN - tbN / 2) : 0;
    /* 27.10: светильники комнаты (pt_renderer.c «СВЕТИЛЬНИКИ ДЛЯ СВЕТА ОТ СВЕТЯЩИХСЯ ТЕКСТУР»): треугольник у рамки светильника
     * (до 250 ед.) - в его группу; нужны матрицы «вид -> комната» этого кадра (dc_vol_frame) и ровный свет */
    const float *fxC = nullptr, *fxB = nullptr; const unsigned* fxK = nullptr;
    const int nfx = (gl_fix_on() && s_pp[PP_GL_LSTEADY] > 0.5f && s_shUboMap[259] > 0.5f) ? dc_glow_fixtures(&fxC, &fxB, &fxK) : 0;
    const float* V2R = s_shUboMap + 260;
    for (int i = 0; i + 2 < s_nverts; i += 3) {
        const float* v0 = s_vmap + (size_t)i * VERT_FLOATS;
        const int fl = (int)(v0[11] + 0.5f);
        if (!(fl & 1) || (fl & 1024)) continue;                         /* текстурный, не рабица */
        const int tpf = (int)(v0[8] + 0.5f);
        if (!((tpf >> 23) & 1)) continue;                               /* не «светится» */
        if (tbOff) continue;                                            /* проверка: лампа «погасла» в этом кадре */
        float P[3][3]; int good = 1;
        for (int k = 0; k < 3; k++) {
            const float* v = v0 + (size_t)k * VERT_FLOATS;
            const float w = v[12];
            if (w < 1.5f) { good = 0; break; }
            P[k][0] = (v[0] - s_shWp[0]) * w / s_shWp[3];
            P[k][1] = (v[1] - s_shWp[1]) * w / s_shWp[2];
            P[k][2] = w;
        }
        if (!good) continue;
        const float e1[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] };
        const float e2[3] = { P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2] };
        const float cx_ = e1[1] * e2[2] - e1[2] * e2[1], cy_ = e1[2] * e2[0] - e1[0] * e2[2], cz_ = e1[0] * e2[1] - e1[1] * e2[0];
        const float area = 0.5f * sqrtf(cx_ * cx_ + cy_ * cy_ + cz_ * cz_);
        if (area < 1.f) continue;
        const float cen[3] = { (P[0][0] + P[1][0] + P[2][0]) / 3.f, (P[0][1] + P[1][1] + P[2][1]) / 3.f, (P[0][2] + P[1][2] + P[2][2]) / 3.f };
        /* цвет самой текстуры: вершины окна UV, середины рёбер и центр (прозрачные тексели мимо) */
        const int tp = gl_lane_page(v0[8]), cxl = (int)(v0[9] + 0.5f) & 0x3FF, cyl = (int)(v0[10] + 0.5f) & 0x3FF;
        const float* va = v0; const float* vb = v0 + VERT_FLOATS; const float* vc_ = v0 + 2 * VERT_FLOATS;
        const float uvs[7][2] = { { va[2], va[3] }, { vb[2], vb[3] }, { vc_[2], vc_[3] },
                                  { (va[2] + vb[2]) * 0.5f, (va[3] + vb[3]) * 0.5f }, { (vb[2] + vc_[2]) * 0.5f, (vb[3] + vc_[3]) * 0.5f },
                                  { (va[2] + vc_[2]) * 0.5f, (va[3] + vc_[3]) * 0.5f },
                                  { (va[2] + vb[2] + vc_[2]) / 3.f, (va[3] + vb[3] + vc_[3]) / 3.f } };
        float col[3] = { 0.f, 0.f, 0.f }; int nn = 0;
        for (int s = 0; s < 7; s++) {
            float rgb[3]; int ok = 0;
            gl_texel_rgb(vr, tp, cxl, cyl, (int)uvs[s][0], (int)uvs[s][1], rgb, &ok);
            if (ok) { col[0] += rgb[0]; col[1] += rgb[1]; col[2] += rgb[2]; nn++; }
        }
        if (!nn) continue;
        col[0] /= nn; col[1] /= nn; col[2] /= nn;
        /* 25.09 ночь: полупрозрачное «сложение» (лазер, огонёк) — белая маска, цвет даёт вершина (лазер: 248 × 20/128 = 39,0,0);
         * непрозрачным текстурам — цвет самой текстуры, как и было (свет в стенах уже запечён в вершинах) */
        if ((fl & 2) && !(fl & 4)) {
            /* 25.09 ночь (4): сила — как до ночи (2) (цвет текстуры), от вершины — только оттенок (лазер 248,0,0);
             * погасший огонёк (вершина почти чёрная) не светит. glow_light_hue 0 — тексель × вершина/128, как в ночь (2) */
            float vcm[3], mx = 0.f;
            for (int q = 0; q < 3; q++) { vcm[q] = (va[4 + q] + vb[4 + q] + vc_[4 + q]) / 3.f; if (vcm[q] > mx) mx = vcm[q]; }
            if (s_pp[PP_GL_LHUE] > 0.5f) {
                if (mx < 2.f) continue;
                for (int q = 0; q < 3; q++) col[q] = col[q] * vcm[q] / mx;
            } else {
                for (int q = 0; q < 3; q++) { col[q] = col[q] * vcm[q] / 128.f; if (col[q] > 255.f) col[q] = 255.f; }
            }
        }
        const float lum = (col[0] > col[1] ? (col[0] > col[2] ? col[0] : col[2]) : (col[1] > col[2] ? col[1] : col[2])) / 255.f;
        int k = -1, fi = -1;
        if (nfx > 0) {                                                  /* 27.10: ближайший по рамке светильник, внутри - ближайший центр */
            float rc[3]; gl_xf(V2R, cen, rc);
            float bdd = 0.f, bcc = 0.f;
            for (int f = 0; f < nfx; f++) {
                const float* B = fxB + f * 6;
                float d2 = 0.f;
                for (int q = 0; q < 3; q++) { const float o = rc[q] < B[q] ? B[q] - rc[q] : rc[q] > B[3 + q] ? rc[q] - B[3 + q] : 0.f; d2 += o * o; }
                if (d2 > 250.f * 250.f) continue;
                const float* C = fxC + f * 4;
                const float cc = (rc[0] - C[0]) * (rc[0] - C[0]) + (rc[1] - C[1]) * (rc[1] - C[1]) + (rc[2] - C[2]) * (rc[2] - C[2]);
                if (fi < 0 || d2 < bdd || (d2 == bdd && cc < bcc)) { bdd = d2; bcc = cc; fi = f; }
            }
        }
        if (fi >= 0) {
            for (int c = 0; c < ncl; c++) if (cl[c].fix == fxK[fi]) { k = c; break; }
        } else {
            for (int c = 0; c < ncl; c++) {
                if (cl[c].fix) continue;                                /* 27.10: группы видимого - без светильников */
                const double cxp = cl[c].p[0] / cl[c].a, cyp = cl[c].p[1] / cl[c].a, czp = cl[c].p[2] / cl[c].a;
                const double dx = cen[0] - cxp, dy = cen[1] - cyp, dz = cen[2] - czp;
                if (dx * dx + dy * dy + dz * dz < (double)MERGE * MERGE) { k = c; break; }
            }
        }
        if (k < 0) {
            if (ncl >= GLC) continue;
            k = ncl++;
            for (int q = 0; q < 3; q++) { cl[k].p[q] = 0.0; cl[k].c[q] = 0.0; cl[k].fc[q] = fi >= 0 ? fxC[fi * 4 + q] : 0.f; }
            cl[k].a = 0.0; cl[k].lum = 0.0;
            cl[k].fix = fi >= 0 ? fxK[fi] : 0u; cl[k].fa = fi >= 0 ? fxC[fi * 4 + 3] : 0.f; cl[k].fi = fi;
        }
        for (int q = 0; q < 3; q++) { cl[k].p[q] += (double)cen[q] * area; cl[k].c[q] += (double)col[q] * area; }
        cl[k].a += area; cl[k].lum += (double)lum * area;
    }
    const int ncl32 = ncl < 32 ? ncl : 32;
    static int lg = -1; static long k2 = 0;
    if (lg < 0) { const char* e = getenv("DC_GLOW_LIGHT_LOG"); lg = (e && *e >= '1' && *e <= '9') ? *e - '0' : 0; }
    /* 27.09: ровный свет мигающих ламп — вывод через следы (выше); выкл — как было */
    if (s_pp[PP_GL_LSTEADY] > 0.5f) {
        if (nfx > 0) {   /* 28.10: пока комнату рисует обход растра - все лампы комнаты сразу (gl_room_out) */
            static unsigned char pres[GLR_FIX], td[GLR_TRI]; static long wsLast = -1; static int wsAge = 1000;
            long ws = 0; unsigned fg = 0;
            const int np = dc_glow_fixture_present(pres, GLR_FIX, td, GLR_TRI, &ws, &fg);
            if (ws != wsLast) { wsLast = ws; wsAge = 0; } else if (wsAge < 1000) wsAge++;
            int npres = 0;
            for (int f = 0; f < np; f++) npres += pres[f];
            if (np > 0 && npres > 0 && wsAge <= 10) {
                if (!s_glrOn) gl_steady_reset();
                const int n = gl_room_out(cl, ncl, vr, fxC, fxB, fxK, np, pres, td, fg, wsAge);
                s_glN = n;
                if (lg && (++k2 % 120) == 1)
                    std::fprintf(stderr, "glowlight: %d sources (room mode: %d fixtures, %d drawn by the walk; %d groups)%c", n, np, npres, ncl, 10);
                return;
            }
        }
        if (s_glrOn) gl_room_reset();
        const int n = gl_steady_out(cl, ncl32);
        gl_set_count(n);
        s_glN = n;
        if (lg && (++k2 % 120) == 1) {
            std::fprintf(stderr, "glowlight: %d sources (%d groups), steady light for blinking ones%c", n, ncl32, 10);
            for (int j = 0; j < n; j++) {
                const float *gp = gl_slot_p(j), *gc = gl_slot_c(j);
                std::fprintf(stderr, "   #%d at %.0f,%.0f,%.0f radius %.0f color %.2f,%.2f,%.2f%c", j, gp[0], gp[1], gp[2], gp[3],
                             gc[0], gc[1], gc[2], 10);
            }
        }
        return;
    }
    if (s_glrOn) gl_room_reset();
    gl_steady_reset();
    /* до 8 самых заметных: средняя яркость × размер (корень площади, как у силы света ниже) и ближе к камере — важнее:
     * мелкая яркая табличка рядом значит больше, чем ряд тусклых плафонов в конце комнаты */
    int ord[GLC]; double pri[GLC];
    for (int c = 0; c < ncl32; c++) {
        ord[c] = c;
        const double A = cl[c].a, mx = cl[c].p[0] / A, my = cl[c].p[1] / A, mz = cl[c].p[2] / A;
        double sz = sqrt(A) / 300.0; if (sz < 0.35) sz = 0.35; if (sz > 1.5) sz = 1.5;
        const double dd = (mx * mx + my * my + mz * mz) / (5000.0 * 5000.0);
        pri[c] = (cl[c].lum / A) * sz / (1.0 + dd);
    }
    for (int a = 0; a < ncl32; a++)
        for (int b2 = a + 1; b2 < ncl32; b2++)
            if (pri[ord[b2]] > pri[ord[a]]) { const int tt = ord[a]; ord[a] = ord[b2]; ord[b2] = tt; }
    const int n = ncl32 < 8 ? ncl32 : 8;
    for (int j = 0; j < n; j++) {
        const int c = ord[j];
        const double A = cl[c].a;
        float sz = (float)(sqrt(A) / 300.0);                        /* размер источника: экран ~300 ед. = 1 */
        const float kr = sz < 0.6f ? 0.6f : (sz > 1.6f ? 1.6f : sz);
        const float ki = sz < 0.35f ? 0.35f : (sz > 1.5f ? 1.5f : sz);
        float* gp = s_shUboMap + 144 + j * 4;
        float* gc = s_shUboMap + 176 + j * 4;
        gp[0] = (float)(cl[c].p[0] / A); gp[1] = (float)(cl[c].p[1] / A); gp[2] = (float)(cl[c].p[2] / A);
        gp[3] = s_pp[PP_GL_LRAD] * kr;
        const float s = s_pp[PP_GL_LSTR] * ki / 255.f;
        gc[0] = (float)(cl[c].c[0] / A) * s; gc[1] = (float)(cl[c].c[1] / A) * s; gc[2] = (float)(cl[c].c[2] / A) * s;
        gc[3] = gl_chr_smooth() ? 0.25f : -1.25f;              /* 28.09: знак < 0 — персонажи по граням, как было */
    }
    gl_set_count(n);
    s_glN = n;
    if (lg && (++k2 % 120) == 1) {
        std::fprintf(stderr, "glowlight: %d sources (%d groups)%c", n, ncl32, 10);
        for (int j = 0; j < n; j++)
            std::fprintf(stderr, "   #%d at %.0f,%.0f,%.0f radius %.0f color %.2f,%.2f,%.2f%c", j, s_shUboMap[144 + j * 4],
                         s_shUboMap[145 + j * 4], s_shUboMap[146 + j * 4], s_shUboMap[147 + j * 4],
                         s_shUboMap[176 + j * 4], s_shUboMap[177 + j * 4], s_shUboMap[178 + j * 4], 10);
    }
    if (lg >= 2) {                                                       /* 27.09: строка на каждый кадр A (замер мигания) */
        LARGE_INTEGER qf, qc; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qc);
        char buf[1024]; int p = 0; float sum = 0.f;
        for (int j = 0; j < n; j++) { const float* g = s_shUboMap + 176 + j * 4; sum += g[0] > g[1] ? (g[0] > g[2] ? g[0] : g[2]) : (g[1] > g[2] ? g[1] : g[2]); }
        p += snprintf(buf + p, sizeof buf - p, "glowlight2: t=%.3f as before groups %d out %d sum %.4f |",
                      (double)qc.QuadPart / (double)qf.QuadPart, ncl32, n, sum);
        for (int j = 0; j < n && p < (int)sizeof buf - 60; j++) {
            const float* g = s_shUboMap + 176 + j * 4;
            p += snprintf(buf + p, sizeof buf - p, " %.3f at %.0f,%.0f,%.0f;", g[0] > g[1] ? (g[0] > g[2] ? g[0] : g[2]) : (g[1] > g[2] ? g[1] : g[2]),
                          s_shUboMap[144 + j * 4], s_shUboMap[145 + j * 4], s_shUboMap[146 + j * 4]);
        }
        std::fprintf(stderr, "%s%c", buf, 10);
    }
}

/* ===== ЛУНА ГЛАДКОЙ ТЕНЬЮ (25.09.2026 ночь) ==========================================================================
 * Игрок: «Луна в 13 комнатах с небом — тень от этого источника света очень шумная». Лунная тень широкая и мягкая, а её
 * полутень считалась 12 выборками с поворотом на пиксель (зерно) после поиска заслонившего по 6 выборкам широкого круга
 * (дыры прямо в тени). Теперь слой луны после прохода карты теней ужимается (dc_moon_pf.frag: доля заслонённых текселей
 * и эта доля × их средняя глубина), размывается Гауссом по горизонтали (dc_glow_blur.frag) и по вертикали с упаковкой
 * резкой версии (dc_moon_pack.frag) — растр берёт две билинейные выборки с биндинга 6 (dc_raster.frag moonLitPF): у ног
 * резкая, к концу тени размытая. Размер ужатой карты — DC_MOON_PFRES (1024: карта 2048 ужимается 2x2). Шаг размытия
 * считает pt_renderer.c из ширины поля луны (moon_blur, единиц комнаты). Нужны объекты свечения (s_rpGB, s_gdsl,
 * s_gplLay, s_fxSamp — fxEnsure); не собралось — прежний путь. DC_MOON_PF=0 (панель теней: moon_pf) — прежний путь. */
enum { MPF_P0 = 0, MPF_PT, MPF_P1, MPF_N };
static VkImage          s_mpfImg[MPF_N] = {};
static VkDeviceMemory   s_mpfMem[MPF_N] = {};
static VkImageView      s_mpfView[MPF_N] = {};
static VkFramebuffer    s_mpfFb[MPF_N] = {};
static VkPipeline       s_mpfDownPipe = VK_NULL_HANDLE, s_mpfPackPipe = VK_NULL_HANDLE;
static VkDescriptorPool s_mpfPool = VK_NULL_HANDLE;
static VkDescriptorSet  s_mpfSetDown[SH_MAX] = {}, s_mpfSetH = VK_NULL_HANDLE, s_mpfSetV = VK_NULL_HANDLE;
static int              s_mpfOk = 0;       /* 1 — собрано, -1 — не вышло (прежний путь) */
static int              s_mpfFresh = 0;    /* новые образы — перевести в SHADER_READ_ONLY на ближайшем кадре */
static int              s_mpfFrame = 0;    /* в последнем кадре A луна шла гладким путём (для панели) */
static VkRenderPass     s_rpGBclr = VK_NULL_HANDLE;   /* 09.10: как s_rpGB, но с заливкой при начале (рамка заслонивших) */
extern "C" void dcvk_moon_pf_stats(int* ok, int* frame, int* res) { *ok = s_mpfOk; *frame = s_mpfFrame; *res = s_mpfRes; }
/* 1 — можно писать проходы гладкой луны в этом кадре. До записи команд кадра (создание ждёт простоя GPU). */
static int mpfEnsure(void) {
    if (s_mpfOk) return s_mpfOk > 0;
    if (!s_shOk || !s_shView) return 0;
    if (!fxEnsure()) return 0;                   /* s_rpGB, s_gdsl, s_gplLay, s_fxSamp */
    s_mpfOk = -1;
    vkDeviceWaitIdle(C->device);
    { const char* e = getenv("DC_MOON_PFRES"); int v = e ? atoi(e) : 1024;
      if (v < 128) v = 128; if (v > s_shSize) v = s_shSize;
      while (s_shSize % v) v--;                  /* ужатие — целое число текселей карты */
      s_mpfRes = v; }
    const int R = s_mpfRes;
    const VkFormat HF = VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < MPF_N; i++) {
        if (!pp_mkImage(HF, VK_SAMPLE_COUNT_1_BIT, cu, VK_IMAGE_ASPECT_COLOR_BIT, R, R, &s_mpfImg[i], &s_mpfMem[i], &s_mpfView[i]))
            { std::fprintf(stderr, "moonpf: images not created - the moon shadow takes the old path%c", 10); return 0; }
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = s_rpGB; fci.attachmentCount = 1; fci.pAttachments = &s_mpfView[i];
        fci.width = (uint32_t)R; fci.height = (uint32_t)R; fci.layers = 1;
        if (vkCreateFramebuffer(C->device, &fci, nullptr, &s_mpfFb[i]) != VK_SUCCESS) return 0;
    }
    {   VkShaderModule pvs = loadSpv("dc_post.vert.spv");
        const bool ok = pvs && fx_mkPost(pvs, "shaders\\dc_moon_pf.frag.spv", 0, s_rpGB, &s_mpfDownPipe)
                            && fx_mkPost(pvs, "shaders\\dc_moon_pack.frag.spv", 0, s_rpGB, &s_mpfPackPipe);
        if (pvs) vkDestroyShaderModule(C->device, pvs, nullptr);
        if (!ok) { std::fprintf(stderr, "moonpf: no shaders\\dc_moon_pf.frag.spv / dc_moon_pack.frag.spv - the moon shadow takes the old path%c", 10); return 0; } }
    if (!pp_mkPass(HF, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   /* нет — проходы по всей карте */
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpGBclr))
        s_rpGBclr = VK_NULL_HANDLE;
    {   VkDescriptorPoolSize psz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * (SH_MAX + 2) };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = SH_MAX + 2; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
        if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_mpfPool) != VK_SUCCESS) return 0;
        VkDescriptorSetLayout ls[SH_MAX + 2]; VkDescriptorSet sets[SH_MAX + 2];
        for (int i = 0; i < SH_MAX + 2; i++) ls[i] = s_gdsl;
        VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsa.descriptorPool = s_mpfPool; dsa.descriptorSetCount = SH_MAX + 2; dsa.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(C->device, &dsa, sets) != VK_SUCCESS) return 0;
        for (int i = 0; i < SH_MAX; i++) s_mpfSetDown[i] = sets[i];
        s_mpfSetH = sets[SH_MAX]; s_mpfSetV = sets[SH_MAX + 1];
        VkDescriptorImageInfo ii[SH_MAX + 4]; VkWriteDescriptorSet wd[SH_MAX + 4]{}; int nw = 0;
        auto put = [&](VkDescriptorSet s, uint32_t b, VkSampler sm, VkImageView v) {
            ii[nw] = { sm, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wd[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[nw].dstSet = s; wd[nw].dstBinding = b;
            wd[nw].descriptorCount = 1; wd[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[nw].pImageInfo = &ii[nw];
            nw++; };
        for (int i = 0; i < SH_MAX; i++) put(s_mpfSetDown[i], 0, s_shSamp, s_shLayer[i]);   /* слой карты теней */
        put(s_mpfSetH, 0, s_fxSamp, s_mpfView[MPF_P0]);
        put(s_mpfSetV, 0, s_fxSamp, s_mpfView[MPF_PT]);
        put(s_mpfSetV, 1, s_fxSamp, s_mpfView[MPF_P0]);
        put(s_dset, 6, s_fxSamp, s_mpfView[MPF_P1]);                                         /* растр: биндинг 6 */
        vkUpdateDescriptorSets(C->device, (uint32_t)nw, wd, 0, nullptr); }
    s_mpfFresh = 1;
    s_mpfOk = 1;
    std::fprintf(stderr, "moonpf: smooth moon shadow ready - map %dx%d (reduced %dx%d)%c", R, R, s_shSize / R, s_shSize / R, 10);
    return 1;
}
/* новые образы — сразу в SHADER_READ_ONLY: биндинг 6 растр читает в любом кадре */
static void mpf_init_layouts(void) {
    if (!s_mpfFresh) return;
    s_mpfFresh = 0;
    VkImageMemoryBarrier b[MPF_N]{};
    for (int i = 0; i < MPF_N; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].srcAccessMask = 0; b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b[i].image = s_mpfImg[i]; b[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, MPF_N, b);
}
/* после прохода карты теней (слой layer — луна): ужать, размыть по горизонтали, по вертикали + упаковать резкую */
static void mpf_run(int layer) {
    const int R = s_mpfRes;
    /* 09.10: rc — рамка рисования (nullptr — вся цель); clr — заливка при начале (nullptr — без неё, как было) */
    auto pass = [&](VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set, const float* p8, const int* rc, const float* clr) {
        const VkViewport vpt{ 0.f, 0.f, (float)R, (float)R, 0.f, 1.f };
        const VkRect2D sc{ { 0, 0 }, { (uint32_t)R, (uint32_t)R } };
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = s_rpGB; rb.framebuffer = fb; rb.renderArea = sc;
        VkClearValue cv{};
        if (clr) { cv.color = { { clr[0], clr[1], clr[2], clr[3] } }; rb.renderPass = s_rpGBclr; rb.clearValueCount = 1; rb.pClearValues = &cv; }
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (pipe != VK_NULL_HANDLE) {
            VkRect2D sr = sc;
            if (rc) { sr.offset = { rc[0], rc[1] }; sr.extent = { (uint32_t)(rc[2] - rc[0]), (uint32_t)(rc[3] - rc[1]) }; }
            vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sr);
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gplLay, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(s_cmd, s_gplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, p8);
            vkCmdDraw(s_cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(s_cmd);
    };
    const float st = s_mpfPar[0];
    const float pd[8] = { (float)(s_shSize / R), 0.f, 0.f, 0.f, (float)R, (float)R, 0.f, 0.f };
    const float ph[8] = { st, 0.f, 1.f, 0.f, (float)R, (float)R, 0.f, 0.f };
    const float pv[8] = { 0.f, st, 0.f, 0.f, (float)R, (float)R, 0.f, 0.f };
    const int shr = (s_rpGBclr != VK_NULL_HANDLE && layer >= 0 && layer < SH_MAX) ? s_shrSt[layer] : SHR_FULL;
    if (shr == SHR_EMPTY) {                       /* 09.10: на слое луны никого — растр читает только P1: нули */
        static const float z4[4] = { 0.f, 0.f, 0.f, 0.f };
        pass(s_mpfFb[MPF_P1], VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, nullptr, z4);
        return;
    }
    if (shr == SHR_RECT) {
        /* 09.10: вне рамки выход прохода — его значение на пустом месте: P0 и PT — (0, 0, 0, 1) (доля 0, альфа 1 из ужатия),
         * P1 — нули. Рамка P0 — заслонившие, ужатые в k раз, + 1 тексель; PT шире по x, P1 — ещё и по y на окно Гаусса
         * (±6 шагов размытия, + 2 на билинейную выборку). */
        static const float e4[4] = { 0.f, 0.f, 0.f, 1.f }, z4[4] = { 0.f, 0.f, 0.f, 0.f };
        const int k = s_shSize / R;
        const int m = (int)ceilf(6.f * fabsf(st)) + 2;
        int r0[4], r1[4], r2[4];
        shr_down(s_shrRc[layer], k, 1, R, r0);
        r1[0] = r0[0] - m; r1[1] = r0[1]; r1[2] = r0[2] + m; r1[3] = r0[3];
        if (r1[0] < 0) r1[0] = 0; if (r1[2] > R) r1[2] = R;
        r2[0] = r1[0]; r2[1] = r1[1] - m; r2[2] = r1[2]; r2[3] = r1[3] + m;
        if (r2[1] < 0) r2[1] = 0; if (r2[3] > R) r2[3] = R;
        pass(s_mpfFb[MPF_P0], s_mpfDownPipe, s_mpfSetDown[layer], pd, r0, e4);
        pass(s_mpfFb[MPF_PT], s_gBlurPipe, s_mpfSetH, ph, r1, e4);
        pass(s_mpfFb[MPF_P1], s_mpfPackPipe, s_mpfSetV, pv, r2, z4);
        return;
    }
    pass(s_mpfFb[MPF_P0], s_mpfDownPipe, s_mpfSetDown[layer], pd, nullptr, nullptr);   /* слой -> P0: доля, доля·глубина */
    pass(s_mpfFb[MPF_PT], s_gBlurPipe, s_mpfSetH, ph, nullptr, nullptr);               /* P0 -> PT: по горизонтали */
    pass(s_mpfFb[MPF_P1], s_mpfPackPipe, s_mpfSetV, pv, nullptr, nullptr);             /* PT -> P1: по вертикали, + резкая из P0 */
}

/* ===== ТЕНИ БЕЗ ЗЕРНА (27.09.2026 (2)) ==============================================================================
 * Игрок: «тени выглядят очень шумными и не совсем точными. От нескольких источников сложение не реалистично работает».
 * Слой карты каждой лампы после прохода карты ужимается (dc_shpf_down.frag: доля заслонённых текселей и доля × расстояние до
 * заслонившего, 2048 -> 1024) и складывается в пирамиду уровней (dc_shpf_pyr.frag: 1024 -> 4, RG16F, массив по слоям). Растр
 * (dc_raster.frag shadowOccPF, биндинг 7) делает по ней PCSS гладкими окнами нужного размера — без зерна и полос. Нужны
 * объекты свечения (s_gdsl, s_gplLay, s_fxSamp — fxEnsure); не собралось — прежний путь (выборки с поворотом). Слои луны
 * сюда не идут — у неё своя гладкая тень (выше). DC_SHADOW_V2=0 (панель теней: v2) — прежние тени целиком. */
#define SPF_LV 9
static VkImage          s_spfImg = VK_NULL_HANDLE;
static VkDeviceMemory   s_spfMem = VK_NULL_HANDLE;
static VkImageView      s_spfArr = VK_NULL_HANDLE;                 /* все слои и уровни — растру */
static VkImageView      s_spfLv[SH_MAX][SPF_LV] = {};              /* слой × уровень — цель и источник проходов */
static VkFramebuffer    s_spfFb[SH_MAX][SPF_LV] = {};
static VkRenderPass     s_rpSPF = VK_NULL_HANDLE;
static VkPipeline       s_spfDownPipe = VK_NULL_HANDLE, s_spfPyrPipe = VK_NULL_HANDLE;
static VkSampler        s_spfSamp = VK_NULL_HANDLE;
static VkDescriptorPool s_spfPool = VK_NULL_HANDLE;
static VkDescriptorSet  s_spfSet[SH_MAX][SPF_LV] = {};             /* [слой][0] читает слой карты, [слой][l] — уровень l-1 */
static int              s_spfOk = 0;       /* 1 — собрано, -1 — не вышло (прежний путь) */
static int              s_spfFresh = 0;    /* новый образ — перевести в SHADER_READ_ONLY на ближайшем кадре */
static int              s_spfRes = 1024, s_spfLevels = SPF_LV;
static VkRenderPass     s_rpSPFclr = VK_NULL_HANDLE;  /* 09.10: как s_rpSPF, но с заливкой при начале (рамка заслонивших) */
/* 1 — можно писать проходы пирамиды в этом кадре. До записи команд кадра (создание ждёт простоя GPU). */
static int spfEnsure(void) {
    if (s_spfOk) return s_spfOk > 0;
    if (!s_shOk || !s_shView || !s_dset) return 0;
    if (!fxEnsure()) return 0;                   /* s_gdsl, s_gplLay, s_fxSamp */
    s_spfOk = -1;
    vkDeviceWaitIdle(C->device);
    s_spfRes = s_shSize / 2; if (s_spfRes < 128) s_spfRes = 128;
    { int lv = 1, r = s_spfRes; while (lv < SPF_LV && r > 4) { r /= 2; lv++; } s_spfLevels = lv; }
    const VkFormat F = VK_FORMAT_R16G16_SFLOAT;
    if (!pp_mkPass(F, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpSPF))
        { std::fprintf(stderr, "shpf: pass not created - shadows take the old path%c", 10); return 0; }
    if (!pp_mkPass(F, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,   /* 09.10; нет — уровни целиком */
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &s_rpSPFclr))
        s_rpSPFclr = VK_NULL_HANDLE;
    {   VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = F;
        ici.extent = { (uint32_t)s_spfRes, (uint32_t)s_spfRes, 1 };
        ici.mipLevels = (uint32_t)s_spfLevels; ici.arrayLayers = SH_MAX; ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(C->device, &ici, nullptr, &s_spfImg) != VK_SUCCESS)
            { std::fprintf(stderr, "shpf: image not created - shadows take the old path%c", 10); return 0; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(C->device, s_spfImg, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == ~0u || vkAllocateMemory(C->device, &mai, nullptr, &s_spfMem) != VK_SUCCESS)
            { std::fprintf(stderr, "shpf: no memory for the pyramid - shadows take the old path%c", 10); return 0; }
        vkBindImageMemory(C->device, s_spfImg, s_spfMem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = s_spfImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vci.format = F;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)s_spfLevels, 0, SH_MAX };
        if (vkCreateImageView(C->device, &vci, nullptr, &s_spfArr) != VK_SUCCESS) return 0;
        for (int li = 0; li < SH_MAX; li++)
            for (int l = 0; l < s_spfLevels; l++) {
                VkImageViewCreateInfo lvi = vci;
                lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                lvi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, (uint32_t)li, 1 };
                if (vkCreateImageView(C->device, &lvi, nullptr, &s_spfLv[li][l]) != VK_SUCCESS) return 0;
                const int R = s_spfRes >> l;
                VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                fci.renderPass = s_rpSPF; fci.attachmentCount = 1; fci.pAttachments = &s_spfLv[li][l];
                fci.width = (uint32_t)R; fci.height = (uint32_t)R; fci.layers = 1;
                if (vkCreateFramebuffer(C->device, &fci, nullptr, &s_spfFb[li][l]) != VK_SUCCESS) return 0;
            }
    }
    {   VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.minLod = 0.f; sci.maxLod = (float)s_spfLevels;
        if (vkCreateSampler(C->device, &sci, nullptr, &s_spfSamp) != VK_SUCCESS) return 0; }
    {   VkShaderModule pvs = loadSpv("dc_post.vert.spv");
        const bool ok = pvs && fx_mkPost(pvs, "shaders\\dc_shpf_down.frag.spv", 0, s_rpSPF, &s_spfDownPipe)
                            && fx_mkPost(pvs, "shaders\\dc_shpf_pyr.frag.spv", 0, s_rpSPF, &s_spfPyrPipe);
        if (pvs) vkDestroyShaderModule(C->device, pvs, nullptr);
        if (!ok) { std::fprintf(stderr, "shpf: no shaders\\dc_shpf_down.frag.spv / dc_shpf_pyr.frag.spv - shadows take the old path%c", 10); return 0; } }
    {   enum { NSMAX = SH_MAX * SPF_LV };
        const int NS = SH_MAX * s_spfLevels;
        VkDescriptorPoolSize psz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(3 * NS) };
        VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpci.maxSets = (uint32_t)NS; dpci.poolSizeCount = 1; dpci.pPoolSizes = &psz;
        if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_spfPool) != VK_SUCCESS) return 0;
        VkDescriptorSetLayout ls[NSMAX]; VkDescriptorSet sets[NSMAX];
        for (int i = 0; i < NS; i++) ls[i] = s_gdsl;
        VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsa.descriptorPool = s_spfPool; dsa.descriptorSetCount = (uint32_t)NS; dsa.pSetLayouts = ls;
        if (vkAllocateDescriptorSets(C->device, &dsa, sets) != VK_SUCCESS) return 0;
        VkDescriptorImageInfo ii[NSMAX + 1]; VkWriteDescriptorSet wd[NSMAX + 1]{}; int nw = 0;
        auto put = [&](VkDescriptorSet s, uint32_t b, VkSampler sm, VkImageView v) {
            ii[nw] = { sm, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            wd[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[nw].dstSet = s; wd[nw].dstBinding = b;
            wd[nw].descriptorCount = 1; wd[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wd[nw].pImageInfo = &ii[nw];
            nw++; };
        for (int li = 0; li < SH_MAX; li++)
            for (int l = 0; l < s_spfLevels; l++) {
                s_spfSet[li][l] = sets[li * s_spfLevels + l];
                if (l == 0) put(s_spfSet[li][0], 0, s_shSamp, s_shLayer[li]);          /* слой карты теней */
                else        put(s_spfSet[li][l], 0, s_fxSamp, s_spfLv[li][l - 1]);     /* уровень выше, вдвое крупнее */
            }
        put(s_dset, 7, s_spfSamp, s_spfArr);                                           /* растр: биндинг 7 */
        vkUpdateDescriptorSets(C->device, (uint32_t)nw, wd, 0, nullptr); }
    s_spfFresh = 1;
    s_spfOk = 1;
    std::fprintf(stderr, "shpf: grain-free shadows ready - pyramid %dx%d, %d levels, %d layers%c", s_spfRes, s_spfRes, s_spfLevels, SH_MAX, 10);
    return 1;
}
/* новый образ — сразу в SHADER_READ_ONLY: биндинг 7 растр держит в любом кадре */
static void spf_init_layouts(void) {
    if (!s_spfFresh) return;
    s_spfFresh = 0;
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.image = s_spfImg; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)s_spfLevels, 0, SH_MAX };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
}
/* после прохода карты теней: слой li -> уровень 0 (ужатие 2x2 с переводом глубины в расстояние), дальше уровень за уровнем */
static void spf_run(int li) {
    /* 09.10: rc — рамка рисования (nullptr — весь уровень); clear — заливка при начале значением пустого места (доля 0) */
    auto pass = [&](int l, VkPipeline pipe, const float* p8, const int* rc, int clear) {
        const int R = s_spfRes >> l;
        const VkViewport vpt{ 0.f, 0.f, (float)R, (float)R, 0.f, 1.f };
        const VkRect2D sc{ { 0, 0 }, { (uint32_t)R, (uint32_t)R } };
        VkRenderPassBeginInfo rb{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = s_rpSPF; rb.framebuffer = s_spfFb[li][l]; rb.renderArea = sc;
        VkClearValue cv{};                                 /* (0, 0): ни доли заслонённого, ни расстояния */
        if (clear) { rb.renderPass = s_rpSPFclr; rb.clearValueCount = 1; rb.pClearValues = &cv; }
        vkCmdBeginRenderPass(s_cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        if (pipe != VK_NULL_HANDLE) {
            VkRect2D sr = sc;
            if (rc) { sr.offset = { rc[0], rc[1] }; sr.extent = { (uint32_t)(rc[2] - rc[0]), (uint32_t)(rc[3] - rc[1]) }; }
            vkCmdSetViewport(s_cmd, 0, 1, &vpt); vkCmdSetScissor(s_cmd, 0, 1, &sr);
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_gplLay, 0, 1, &s_spfSet[li][l], 0, nullptr);
            vkCmdPushConstants(s_cmd, s_gplLay, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, p8);
            vkCmdDraw(s_cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(s_cmd);
    };
    const float A = s_shUboMap[64 + li * 4 + 1], B = s_shUboMap[64 + li * 4 + 2];
    const float pd[8] = { (float)(s_shSize / s_spfRes), A, B, 0.f, (float)s_spfRes, (float)s_spfRes, 0.f, 0.f };
    const int shr = (s_rpSPFclr != VK_NULL_HANDLE && li >= 0 && li < SH_MAX) ? s_shrSt[li] : SHR_FULL;
    if (shr == SHR_EMPTY) {                        /* 09.10: на слое никого — все уровни просто нули */
        for (int l = 0; l < s_spfLevels; l++) pass(l, VK_NULL_HANDLE, nullptr, nullptr, 1);
        return;
    }
    int rc[4] = { 0, 0, 0, 0 };
    if (shr == SHR_RECT) shr_down(s_shrRc[li], s_shSize / s_spfRes, 1, s_spfRes, rc);   /* ужатие 2x2 + 1 тексель */
    pass(0, s_spfDownPipe, pd, shr == SHR_RECT ? rc : nullptr, shr == SHR_RECT);
    for (int l = 1; l < s_spfLevels; l++) {
        const float src = (float)(s_spfRes >> (l - 1));
        const float pp[8] = { 0.f, 0.f, 0.f, 0.f, src, src, 0.f, 0.f };
        if (shr == SHR_RECT) {                     /* 5 билинейных выборок: окно приёмника ±1 тексель */
            int nr[4]; shr_down(rc, 2, 1, s_spfRes >> l, nr);
            rc[0] = nr[0]; rc[1] = nr[1]; rc[2] = nr[2]; rc[3] = nr[3];
        }
        pass(l, s_spfPyrPipe, pp, shr == SHR_RECT ? rc : nullptr, shr == SHR_RECT);
    }
}

// ------------------------------------------------------ pipeline set-up ----
/* 25.10.2026: набор дескрипторов растра (биндинги 0..8: VRAM, атлас шрифта, прошлый кадр, карта теней, блок ламп, отражение,
 * луна, пирамида теней, массив текстур из mods), его раскладка, раскладка конвейеров и раскладка мешей (набор 0 у них общий).
 * Заполняет биндинги 0 и 1; 3..7 - buildFixed дальше, 8 - trep_gpu_init. Безопасные шейдеры (s_rasterSafe) - одно место в
 * массиве. raster_sets_free - всё обратно: buildFixed поднимает конвейеры заново, если они не собрались. */
static void raster_sets_free(void) {
    { void mesh_layouts_free(void); mesh_layouts_free(); }
    if (s_dpool) { vkDestroyDescriptorPool(C->device, s_dpool, nullptr); s_dpool = VK_NULL_HANDLE; s_dset = VK_NULL_HANDLE; }
    if (s_plLay) { vkDestroyPipelineLayout(C->device, s_plLay, nullptr); s_plLay = VK_NULL_HANDLE; }
    if (s_dsl)   { vkDestroyDescriptorSetLayout(C->device, s_dsl, nullptr); s_dsl = VK_NULL_HANDLE; }
}
static void raster_safe_apply(void) {
    if (s_rasterSafe >= 1) g_vk_nonuniform_tex = 0;   /* текстуры из mods - выкл (trep_*: «the video card cannot index...») */
    if (s_rasterSafe >= 2) g_vk_conservative = 0;     /* и без консервативной растеризации */
}
static bool raster_sets(void) {
    // descriptor: binding 0 = VRAM combined image sampler,
    //             binding 1 = hi-res font atlas (see fontHiresInit)
    //             binding 2 = previous hi-res frame (15.09, see s_prevImg; written in ensureTargets)
    VkDescriptorSetLayoutBinding bnd[9]{};
    bnd[0].binding = 0; bnd[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bnd[0].descriptorCount = 1; bnd[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bnd[1] = bnd[0]; bnd[1].binding = 1;
    bnd[2] = bnd[0]; bnd[2].binding = 2;
    bnd[3] = bnd[0]; bnd[3].binding = 3;          /* 18.09: карта теней (слой на лампу) */
    bnd[4].binding = 4; bnd[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bnd[4].descriptorCount = 1;                   /* 18.09 вечер: данные ламп */
    bnd[4].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bnd[5] = bnd[0]; bnd[5].binding = 5;          /* 25.09 вечер: отражение пола (цель прохода отражения, половинная) */
    bnd[6] = bnd[0]; bnd[6].binding = 6;          /* 25.09 ночь: луна гладкой тенью (ужатая и размытая карта луны) */
    bnd[7] = bnd[0]; bnd[7].binding = 7;          /* 27.09 (2): тени без зерна (пирамида карт теней ламп, массив по слоям) */
    bnd[8] = bnd[0]; bnd[8].binding = 8;          /* 05.10: текстуры PNG из mods (s_trepCap картинок, по умолчанию заглушка) */
    {   /* 24.10.2026: мест столько, сколько видеокарта даёт фрагментному шейдеру: 7 одиночных сэмплеров рядом (биндинги 0-3, 5-7),
         * среди ресурсов ещё блок ламп и цели цвета (до 4 с запасом). NVIDIA и AMD - 512, как было; Intel - 57 */
        VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(C->phys, &pr);
        const VkPhysicalDeviceLimits& L = pr.limits;
        int64_t cap = TREP_MAX;
        const int64_t lim[5] = { (int64_t)L.maxPerStageDescriptorSamplers - 7, (int64_t)L.maxPerStageDescriptorSampledImages - 7,
                                 (int64_t)L.maxPerStageResources - 7 - 1 - 4, (int64_t)L.maxDescriptorSetSamplers - 7,
                                 (int64_t)L.maxDescriptorSetSampledImages - 7 };
        for (int64_t v : lim) if (v < cap) cap = v;
        if (const char* e = getenv("DC_TREP_CAP")) { const int64_t v = atoi(e); if (v > 0 && v < cap) cap = v; }
        if (cap < 1) cap = 1;
        if (s_rasterSafe) cap = 1;                    /* 25.10.2026: безопасные шейдеры - одно место (DC_SAFE: repTex[0]) */
        s_trepCap = (int32_t)cap;
        std::fprintf(stderr, "render: textures from mods - %d at once (graphics card limits per shader: %u samplers, %u images, %u resources)%s%c",
                     (int)s_trepCap, L.maxPerStageDescriptorSamplers, L.maxPerStageDescriptorSampledImages, L.maxPerStageResources,
                     s_rasterSafe ? " - safe shaders, textures from mods off" : "", 10);
    }
    bnd[8].descriptorCount = (uint32_t)s_trepCap;
    VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 9; dci.pBindings = bnd;
    if (vkCreateDescriptorSetLayout(C->device, &dci, nullptr, &s_dsl) != VK_SUCCESS) {
        s_dsl = VK_NULL_HANDLE; snprintf(s_rasterErr, sizeof s_rasterErr, "the descriptor set layout could not be created"); return false;
    }

    /* 64: view + params (0..32) и прошлый кадр prevRect + prevMap (32..64, 15.09) */
    /* 17.09: +64 б — перепроецирование кадра B. Данные теней здесь не лежат:
     * они в блоке ShadowUBO, иначе на устройствах со 128 байтами констант
     * теней не было вовсе. */
    uint32_t pcsize = 128;
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcsize };   /* 17.09: +64 б — перепроецирование кадра B */
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &s_dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(C->device, &plci, nullptr, &s_plLay) != VK_SUCCESS) {
        s_plLay = VK_NULL_HANDLE; snprintf(s_rasterErr, sizeof s_rasterErr, "the pipeline layout could not be created"); return false;
    }
    { void mesh_layouts(void); mesh_layouts(); }   /* 06.10.2026: набор 1 мешей из mods (экземпляры, кости) */

    VkDescriptorPoolSize psz[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7 + (uint32_t)s_trepCap },
                                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1; dpci.poolSizeCount = 2; dpci.pPoolSizes = psz;
    if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_dpool) != VK_SUCCESS) {
        s_dpool = VK_NULL_HANDLE; snprintf(s_rasterErr, sizeof s_rasterErr, "the descriptor pool could not be created"); return false;
    }
    VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsa.descriptorPool = s_dpool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &s_dsl;
    if (vkAllocateDescriptorSets(C->device, &dsa, &s_dset) != VK_SUCCESS) {
        s_dset = VK_NULL_HANDLE; snprintf(s_rasterErr, sizeof s_rasterErr, "the descriptor set could not be allocated"); return false;
    }
    VkDescriptorImageInfo dii[2] = {
        { C->vramSamp, C->vramView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { s_fontSamp,  s_fontView,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = s_dset; w[i].dstBinding = (uint32_t)i; w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[i].pImageInfo = &dii[i];
    }
    vkUpdateDescriptorSets(C->device, 2, w, 0, nullptr);
    return true;
}

static bool buildFixed() {
    // command pool / buffer / fence
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = C->qfam;
    if (vkCreateCommandPool(C->device, &pci, nullptr, &s_pool) != VK_SUCCESS) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "Vulkan could not create a command pool"); return false;
    }
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(C->device, &ai, &s_cmd) != VK_SUCCESS) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "Vulkan could not create a command buffer"); return false;
    }
    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(C->device, &fci, nullptr, &s_fence);

    // render pass: RGBA8 color (clear -> TRANSFER_SRC) + D32 depth (clear 0)
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &colRef;
    sp.pDepthStencilAttachment = &depRef;
    VkRenderPassCreateInfo rci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rci.attachmentCount = 2; rci.pAttachments = att;
    rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = main_pass_dep(&rci.pDependencies);
    if (vkCreateRenderPass(C->device, &rci, nullptr, &s_rp) != VK_SUCCESS) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "Vulkan could not create the render pass"); return false;
    }
    {   /* 21.09 вечер: отрезки кадра с захватом сцены (не вышло — захват просто не включится) */
        VkAttachmentDescription a2[2] = { att[0], att[1] };
        a2[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderPassCreateInfo r2 = rci; r2.pAttachments = a2;
        if (vkCreateRenderPass(C->device, &r2, nullptr, &s_rpFirst) != VK_SUCCESS) s_rpFirst = VK_NULL_HANDLE;
        a2[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a2[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        a2[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a2[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (vkCreateRenderPass(C->device, &r2, nullptr, &s_rpResume) != VK_SUCCESS) s_rpResume = VK_NULL_HANDLE;
    }

    /* 25.10.2026: набор дескрипторов - raster_sets (выше); безопасные шейдеры (s_rasterSafe) - с самого начала, если
     * устройство не выбирает картинку из массива по пикселю, или DC_RASTER_SAFE=<1|2> (проверка) */
    { const char* e = getenv("DC_RASTER_SAFE"); if (e) { const int v = atoi(e); s_rasterSafe = v < 0 ? 0 : (v > 2 ? 2 : v); } }
    if (!g_vk_nonuniform_tex && s_rasterSafe < 1) s_rasterSafe = 1;
    if (s_rasterSafe) std::fprintf(stderr, "raster: bring-up with the safe shaders, level %d%c", s_rasterSafe, 10);
    raster_safe_apply();
    if (!fontHiresInit()) {
        rlog("[VKR] hi-res font image failed\n");
        snprintf(s_rasterErr, sizeof s_rasterErr, "the font image could not be created");
        return false;
    }
    if (!raster_sets()) return false;

    /* --- карта теней: цель, проход, конвейер (18.09) ---------------------- */
    /* данные ламп: маленький блок, живёт всегда — даже когда карту создать не
     * удалось, иначе набор дескрипторов невалиден (биндинг 4 пустой) */
    if (makeBuf((VkDeviceSize)SH_UBO_FLOATS * sizeof(float),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &s_shUbo, &s_shUboMem, (void**)&s_shUboMap) && s_shUboMap) {
        for (int i = 0; i < SH_UBO_FLOATS; i++) s_shUboMap[i] = 0.f;
        dcvk_shadow_shape(nullptr, 0);                /* 27.09: мягкость теней — круг, пока лампы-текстуры не зададут форму */
        s_shUboMap[115] = -1.f;                 /* луны нет */
    } else {
        rlog("[VKR] shadow data block not created\n");
    }
    {
        /* размер карты: 2048 даёт пятно на персонажа в полсотни текселей даже
         * при широком поле зрения лампы (DC_SHADOW_MAPSIZE — откатить на 1024) */
        { const char* e = getenv("DC_SHADOW_MAPSIZE"); int v = e ? atoi(e) : 2048;
          if (v < 256) v = 256; if (v > 4096) v = 4096; s_shSize = v; }
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
        ici.extent = { (uint32_t)s_shSize, (uint32_t)s_shSize, 1 };
        ici.mipLevels = 1; ici.arrayLayers = SH_MAX; ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(C->device, &ici, nullptr, &s_shImg) == VK_SUCCESS) {
            VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(C->device, s_shImg, &mr);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(C->device, &mai, nullptr, &s_shMem) == VK_SUCCESS &&
                vkBindImageMemory(C->device, s_shImg, s_shMem, 0) == VK_SUCCESS) {
                VkImageViewCreateInfo ivi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                ivi.image = s_shImg; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                ivi.format = VK_FORMAT_D32_SFLOAT;
                ivi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, SH_MAX };
                vkCreateImageView(C->device, &ivi, nullptr, &s_shView);
                for (int li = 0; li < SH_MAX; li++) {      /* отдельный вид на слой — цель прохода */
                    VkImageViewCreateInfo lvi = ivi;
                    lvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    lvi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, (uint32_t)li, 1 };
                    vkCreateImageView(C->device, &lvi, nullptr, &s_shLayer[li]);
                }
                VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
                sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                vkCreateSampler(C->device, &sci, nullptr, &s_shSamp);

                VkAttachmentDescription sa{};
                sa.format = VK_FORMAT_D32_SFLOAT; sa.samples = VK_SAMPLE_COUNT_1_BIT;
                sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                sa.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                sa.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                sa.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                sa.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                VkAttachmentReference sr{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
                VkSubpassDescription ssp{};
                ssp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
                ssp.pDepthStencilAttachment = &sr;
                VkSubpassDependency sdep[2]{};
                sdep[0].srcSubpass = VK_SUBPASS_EXTERNAL; sdep[0].dstSubpass = 0;
                sdep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                sdep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
                sdep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                sdep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                sdep[1].srcSubpass = 0; sdep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
                sdep[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                sdep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                sdep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                sdep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                VkRenderPassCreateInfo srci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
                srci.attachmentCount = 1; srci.pAttachments = &sa;
                srci.subpassCount = 1; srci.pSubpasses = &ssp;
                srci.dependencyCount = 2; srci.pDependencies = sdep;
                if (vkCreateRenderPass(C->device, &srci, nullptr, &s_shRp) == VK_SUCCESS) {
                    int fbok = 1;
                    for (int li = 0; li < SH_MAX; li++) {
                        VkFramebufferCreateInfo sfi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                        sfi.renderPass = s_shRp; sfi.attachmentCount = 1; sfi.pAttachments = &s_shLayer[li];
                        sfi.width = (uint32_t)s_shSize; sfi.height = (uint32_t)s_shSize; sfi.layers = 1;
                        if (vkCreateFramebuffer(C->device, &sfi, nullptr, &s_shFb[li]) != VK_SUCCESS) fbok = 0;
                    }
                    if (fbok && s_shUboMap) s_shOk = 1;
                }
                /* 24.10.2026: все слои - сразу в SHADER_READ_ONLY. Растр держит карту в наборе (биндинг 3) с первого кадра, а слой
                 * переходит в эту раскладку только своим проходом теней: до первой лампы (и слои сверх числа ламп комнаты) он был
                 * UNDEFINED - проверка Vulkan: VUID-vkCmdDraw-None-09600, другой драйвер вправе отдать мусор */
                cmdBegin();
                VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.image = s_shImg;
                b.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, SH_MAX };
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
                if (!cmdSubmitWait()) s_shOk = 0;
            }
        }
        if (!s_shOk) rlog("[VKR] shadow map not created\n");
    }

    // vertex stream + shaders + 10 pipeline variants
    /* 06.10.2026: оба потока — makeBufStream (кэшируемая память, см. vstream_cached_on) */
    if (!makeBufStream((VkDeviceSize)MAX_VERTS * VERT_FLOATS * sizeof(float),
                       &s_vbuf, &s_vmem, (void**)&s_vmap, "vertex stream")) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "no memory for the vertex stream"); return false;
    }
    /* 05.10: второй поток вершин (текстура PNG из mods) — по 4 числа на вершину, номер картинки −1 = родная текстура */
    if (!makeBufStream((VkDeviceSize)MAX_VERTS * 4 * sizeof(float),
                       &s_rvBuf, &s_rvMem, (void**)&s_rvMap, "second vertex stream") || !s_rvMap) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "no memory for the second vertex stream"); return false;
    }
    for (size_t i = 0; i < (size_t)MAX_VERTS; i++) { s_rvMap[i * 4] = -1.f; s_rvMap[i * 4 + 1] = s_rvMap[i * 4 + 2] = s_rvMap[i * 4 + 3] = 0.f; }
    /* 26.10.2026: третий поток - метка куска (пары кусков), 0 - нет */
    if (!makeBufStream((VkDeviceSize)MAX_VERTS * sizeof(uint32_t),
                       &s_pvBuf, &s_pvMem, (void**)&s_pvMap, "pair tag stream") || !s_pvMap) {
        snprintf(s_rasterErr, sizeof s_rasterErr, "no memory for the pair tag stream"); return false;
    }
    memset(s_pvMap, 0, (size_t)MAX_VERTS * sizeof(uint32_t));
    if (!trep_gpu_init()) {
        rlog("[VKR] texture replacement init failed\n");
        snprintf(s_rasterErr, sizeof s_rasterErr, "the texture array could not be set up"); return false;
    }

    /* 25.09: 11 вариантов растра и фон строит mkRasterPipes (выше) — тем же кодом строятся конвейеры MSAA */
    /* 25.10.2026: не собрались - ещё раз с безопасными шейдерами (s_rasterSafe 1, потом 2), набор дескрипторов - заново */
    const int feat0 = raster_feat_want();             /* 25.10.2026: опции в шейдере */
    while (!mkRasterPipes(s_rp, VK_SAMPLE_COUNT_1_BIT, 0, s_pipes, &s_bgPipe, feat0)) {
        if (s_rasterSafe >= 2) {
            snprintf(s_rasterErr, sizeof s_rasterErr, "the graphics card driver could not build the graphics pipelines");
            return false;
        }
        s_rasterSafe++;
        std::fprintf(stderr, "raster: bring-up again with the safe shaders, level %d (%s) - the pipelines did not build%c", s_rasterSafe,
                     s_rasterSafe == 1 ? "no texture array per pixel, textures from mods off" : "also no lights, shadows, materials and MSAA", 10);
        raster_sets_free();
        raster_safe_apply();
        if (!raster_sets()) return false;
        if (!trep_gpu_init()) { snprintf(s_rasterErr, sizeof s_rasterErr, "the texture array could not be set up"); return false; }
    }
    s_featPipes = feat0;
    std::fprintf(stderr, "raster: options in the shader - shadows %s, materials %s%s%c", (feat0 & 1) ? "on" : "off",
                 (feat0 & 2) ? "on" : "off", feat0 == 3 ? "" : " (what is off is not in the pipelines)", 10);
    /* состояние вершин и прохода — для конвейера карты теней ниже (как у растра) */
    VkVertexInputBindingDescription vb{ 0, VERT_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[5]{
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,       0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       8 },
        { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
        { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
        { 4, 0, VK_FORMAT_R32_SFLOAT,          48 },
    };
    VkPipelineVertexInputStateCreateInfo vin{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 5; vin.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    /* конвейер карты теней: те же вершины, только ступень вершин (18.09) */
    if (s_shOk) {
        VkShaderModule svs = loadSpv("dc_shadow.vert.spv");
        if (!svs) { s_shOk = 0; rlog("[VKR] no dc_shadow.vert.spv - shadows off\n"); }
        else {
            VkPipelineShaderStageCreateInfo sst{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            sst.stage = VK_SHADER_STAGE_VERTEX_BIT; sst.module = svs; sst.pName = "main";
            VkPipelineDepthStencilStateCreateInfo sds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            sds.depthTestEnable = VK_TRUE; sds.depthWriteEnable = VK_TRUE;
            sds.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendStateCreateInfo scb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            scb.attachmentCount = 0;
            VkPipelineRasterizationStateCreateInfo srs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            srs.polygonMode = VK_POLYGON_MODE_FILL; srs.cullMode = VK_CULL_MODE_NONE;
            srs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; srs.lineWidth = 1.0f;
            srs.depthBiasEnable = VK_TRUE;
            srs.depthBiasConstantFactor = 1.25f; srs.depthBiasSlopeFactor = 1.75f;
            VkGraphicsPipelineCreateInfo sgp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            sgp.stageCount = 1; sgp.pStages = &sst;
            sgp.pVertexInputState = &vin; sgp.pInputAssemblyState = &ia;
            sgp.pViewportState = &vp; sgp.pRasterizationState = &srs;
            sgp.pMultisampleState = &ms; sgp.pDepthStencilState = &sds;
            sgp.pColorBlendState = &scb; sgp.pDynamicState = &dst;
            sgp.layout = s_plLay; sgp.renderPass = s_shRp; sgp.subpass = 0;
            if (vkp_create(&sgp, &s_shPipe, "shadow map") != VK_SUCCESS) {
                rlog("[VKR] shadow map pipeline FAILED\n"); s_shOk = 0;
            }
            vkDestroyShaderModule(C->device, svs, nullptr);
        }
    }
    /* биндинг 3 должен быть заполнен всегда — иначе набор дескрипторов
     * невалиден; без карты подставляем атлас шрифта как заглушку */
    { VkDescriptorImageInfo sdi{ s_shSamp ? s_shSamp : s_fontSamp,
                                 s_shView ? s_shView : s_fontView,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      VkDescriptorBufferInfo sbi{ s_shUbo, 0, VK_WHOLE_SIZE };
      VkWriteDescriptorSet sw[2]{};
      sw[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      sw[0].dstSet = s_dset; sw[0].dstBinding = 3; sw[0].descriptorCount = 1;
      sw[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw[0].pImageInfo = &sdi;
      sw[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      sw[1].dstSet = s_dset; sw[1].dstBinding = 4; sw[1].descriptorCount = 1;
      sw[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; sw[1].pBufferInfo = &sbi;
      vkUpdateDescriptorSets(C->device, s_shUbo ? 2 : 1, sw, 0, nullptr); }
    /* 25.09 вечер: биндинг 5 (отражение пола) — до сборки проходов отражения заглушка (атлас шрифта), как у биндинга 3 */
    { VkDescriptorImageInfo rdi{ s_fontSamp, s_fontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      VkWriteDescriptorSet rw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
      rw.dstSet = s_dset; rw.dstBinding = 5; rw.descriptorCount = 1;
      rw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; rw.pImageInfo = &rdi;
      vkUpdateDescriptorSets(C->device, 1, &rw, 0, nullptr);
      rw.dstBinding = 6;                        /* 25.09 ночь: луна гладкой тенью — до сборки её проходов тоже заглушка */
      vkUpdateDescriptorSets(C->device, 1, &rw, 0, nullptr);
      /* 27.09 (2): тени без зерна — до сборки пирамиды заглушка-массив (сама карта теней), шейдер её не читает (spf.x = 0) */
      VkDescriptorImageInfo pdi{ s_shSamp ? s_shSamp : s_fontSamp, s_shView ? s_shView : s_fontView,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
      rw.dstBinding = 7; rw.pImageInfo = &pdi;
      vkUpdateDescriptorSets(C->device, 1, &rw, 0, nullptr); }

    pp_caps();                      /* 25.09: пределы MSAA, метки времени GPU */
    rlog("[VKR] fixed state ready\n");
    return true;
}

/* Targets are allocated ONCE at the maximum PS1 display size times the
 * scale (boot alternates 640x478 / 640x472 / 256x240 every few frames —
 * recreating images per mode change cost a vkDeviceWaitIdle + 30 MB of
 * allocations per present, a stall storm). Rendering uses a w*h viewport
 * and the readback copies only the active region. */
static int s_fbSc = 0;                       /* 10.10: масштаб, под который выделены цели */
static bool ensureTargets(int w, int h) {
    const int sc = dcvk_scale();
    int aw = 640 * sc, ah = 512 * sc;
    if (sc > 5) {                            /* 10.10: широкие экраны - в половинном масштабе (dcvk_scale_for) */
        aw = std::max(640 * ((sc + 1) / 2), 368 * sc);
        ah = std::max(512 * ((sc + 1) / 2), 256 * sc);
    }
    /* 10.10: мир шире кадра PS1 по форме картинки (dcvk_target_w) - цель шире; при том же масштабе только растёт (окно
     * тянут мышью при Auto - без пересоздания на каждом шаге) */
    if (w > aw && w <= 8192) aw = w;
    if (s_fbSc == sc && s_fbW > aw && s_fbH == ah) aw = s_fbW;
    if (w > aw || h > ah) return false;      /* beyond PS1 display limits */
    w = aw; h = ah;                          /* allocate the max once */
    if (s_fbW == w && s_fbH == h) return true;
    vkDeviceWaitIdle(C->device);
    if (s_fb) vkDestroyFramebuffer(C->device, s_fb, nullptr);
    if (s_colView) vkDestroyImageView(C->device, s_colView, nullptr);
    if (s_colImg) vkDestroyImage(C->device, s_colImg, nullptr);
    if (s_colMem) vkFreeMemory(C->device, s_colMem, nullptr);
    if (s_depView) vkDestroyImageView(C->device, s_depView, nullptr);
    if (s_depImg) vkDestroyImage(C->device, s_depImg, nullptr);
    if (s_depMem) vkFreeMemory(C->device, s_depMem, nullptr);
    if (s_prevView) vkDestroyImageView(C->device, s_prevView, nullptr);
    if (s_prevImg) vkDestroyImage(C->device, s_prevImg, nullptr);
    if (s_prevMem) vkFreeMemory(C->device, s_prevMem, nullptr);
    s_fb = VK_NULL_HANDLE; s_colImg = VK_NULL_HANDLE; s_depImg = VK_NULL_HANDLE;
    s_colView = VK_NULL_HANDLE; s_depView = VK_NULL_HANDLE;
    s_colMem = VK_NULL_HANDLE; s_depMem = VK_NULL_HANDLE;
    s_prevImg = VK_NULL_HANDLE; s_prevView = VK_NULL_HANDLE; s_prevMem = VK_NULL_HANDLE;
    s_prevOnGpu = 0; s_prevValid = 0;

    auto mkImg = [&](VkFormat fmt, VkImageUsageFlags use, VkImageAspectFlags asp,
                     VkImage* img, VkDeviceMemory* mem, VkImageView* view) -> bool {
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = use; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(C->device, &ici, nullptr, img) != VK_SUCCESS) return false;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(C->device, *img, &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(C->device, &mai, nullptr, mem) != VK_SUCCESS) return false;
        vkBindImageMemory(C->device, *img, *mem, 0);
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = *img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { asp, 0, 1, 0, 1 };
        return vkCreateImageView(C->device, &vci, nullptr, view) == VK_SUCCESS;
    };
    /* 25.09: SAMPLED — цвет (маска в альфе) и глубину читает SSAO */
    if (!mkImg(VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, &s_colImg, &s_colMem, &s_colView)) return false;
    if (!mkImg(VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, &s_depImg, &s_depMem, &s_depView)) return false;
    /* 15.09: прошлый кадр — того же размера, копия цветной цели в конце каждого кадра */
    if (!mkImg(VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, &s_prevImg, &s_prevMem, &s_prevView)) return false;
    if (!s_prevSamp) {
        VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;   /* пиксель в пиксель */
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.f;
        if (vkCreateSampler(C->device, &sci, nullptr, &s_prevSamp) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorImageInfo pii{ s_prevSamp, s_prevView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet pw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        pw.dstSet = s_dset; pw.dstBinding = 2; pw.descriptorCount = 1;
        pw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; pw.pImageInfo = &pii;
        vkUpdateDescriptorSets(C->device, 1, &pw, 0, nullptr);
    }

    VkImageView atts[2] = { s_colView, s_depView };
    VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = s_rp; fci.attachmentCount = 2; fci.pAttachments = atts;
    fci.width = (uint32_t)w; fci.height = (uint32_t)h; fci.layers = 1;
    if (vkCreateFramebuffer(C->device, &fci, nullptr, &s_fb) != VK_SUCCESS) return false;

    size_t need = (size_t)w * h * 4;
    if (need > s_rbSize) {
        if (s_rbMap) { vkUnmapMemory(C->device, s_rbMem); s_rbMap = nullptr; }
        if (s_rbBuf) vkDestroyBuffer(C->device, s_rbBuf, nullptr);
        if (s_rbMem) vkFreeMemory(C->device, s_rbMem, nullptr);
        s_rbBuf = VK_NULL_HANDLE; s_rbMem = VK_NULL_HANDLE;
        /* HOST_CACHED is essential: SDL_UpdateTexture reads this buffer back
         * on the CPU, and reading write-combined (COHERENT-only) memory is
         * uncached — measured 42 ms per 7.7 MB frame, which alone halved the
         * frame rate. Cached+coherent reads at cache speed (<1 ms). Fall back
         * to coherent-only if the driver lacks a cached host type. */
        if (!makeBuf(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                     | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &s_rbBuf, &s_rbMem, &s_rbMap) &&
            !makeBuf(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     &s_rbBuf, &s_rbMem, &s_rbMap)) return false;
        s_rbSize = need;
    }
    s_fbW = w; s_fbH = h; s_fbSc = sc;
    rlog("[VKR] targets %dx%d\n", w, h);
    { char wn[48]; std::snprintf(wn, sizeof wn, "frame targets %dx%d", w, h); vram_note(wn); }
    return true;
}

// ----------------------------------------------------------- public API ----
/* 09.10.2026 (DC_VK_SHARED, см. блок у cmdSubmitWait): общая с D3D11 текстура кадра. */
extern "C" int dcvk_shared_available(void) { return g_vk_d3d11_share; }
/* handle — дескриптор текстуры D3D11 (B8G8R8A8, keyed mutex): NT или, при kmt, прежний общий; w x h — размер кадра. 0 — снять. */
extern "C" int dcvk_shared_attach(void* handle, int w, int h, int kmt, int mutex) {
    if (!C || !C->device) return 0;
    struct GpuGuard { GpuGuard() { ptvk_gpu_lock(); } ~GpuGuard() { ptvk_gpu_unlock(); } } gpu_guard;
    vkDeviceWaitIdle(C->device);
    dx_release();
    s_lastShared = 0;
    if (!handle || w <= 0 || h <= 0 || !g_vk_d3d11_share) return 0;
    VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    const VkExternalMemoryHandleTypeFlagBits htype = kmt ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT
                                                         : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    emi.handleTypes = htype;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {   /* 24.10.2026: берёт ли драйвер такую текстуру D3D11 (формат, использование, вид дескриптора) - раньше не спрашивали */
        VkPhysicalDeviceExternalImageFormatInfo xi{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
        xi.handleType = htype;
        VkPhysicalDeviceImageFormatInfo2 fi{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
        fi.pNext = &xi; fi.format = ici.format; fi.type = ici.imageType; fi.tiling = ici.tiling; fi.usage = ici.usage;
        VkExternalImageFormatProperties xp{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkImageFormatProperties2 fp{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        fp.pNext = &xp;
        const VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(C->phys, &fi, &fp);
        const VkExternalMemoryFeatureFlags ff = xp.externalMemoryProperties.externalMemoryFeatures;
        if (fr != VK_SUCCESS || !(ff & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
            fp.imageFormatProperties.maxExtent.width < (uint32_t)w || fp.imageFormatProperties.maxExtent.height < (uint32_t)h) {
            rlog("[VKR] shared: the driver cannot take this D3D11 texture (%s handle, query %d, features 0x%X, up to %ux%u)\n",
                 kmt ? "KMT" : "NT", (int)fr, (unsigned)ff, fp.imageFormatProperties.maxExtent.width, fp.imageFormatProperties.maxExtent.height);
            return 0;
        }
    }
    if (vkCreateImage(C->device, &ici, nullptr, &s_dxImg) != VK_SUCCESS) {
        s_dxImg = VK_NULL_HANDLE; rlog("[VKR] shared: image create FAILED\n"); return 0;
    }
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(C->device, s_dxImg, &mr);
    uint32_t bits = mr.memoryTypeBits, hbits = 0;
    int hres = -1000;
    auto pProps = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(C->device, "vkGetMemoryWin32HandlePropertiesKHR");
    if (pProps) {
        VkMemoryWin32HandlePropertiesKHR hp{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
        hres = (int)pProps(C->device, htype, (HANDLE)handle, &hp);
        hbits = hp.memoryTypeBits;
        if (hres == VK_SUCCESS && (hp.memoryTypeBits & bits)) bits &= hp.memoryTypeBits;
    }
    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.image = s_dxImg;
    VkImportMemoryWin32HandleInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    imp.pNext = &ded;
    imp.handleType = htype;
    imp.handle = (HANDLE)handle;
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &imp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memType(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u) { dx_release(); rlog("[VKR] shared: no memory type for the import FAILED\n"); return 0; }   /* 24.10.2026 */
    if (vkAllocateMemory(C->device, &mai, nullptr, &s_dxMem) != VK_SUCCESS) {
        s_dxMem = VK_NULL_HANDLE; dx_release(); rlog("[VKR] shared: memory import FAILED\n"); return 0;
    }
    if (vkBindImageMemory(C->device, s_dxImg, s_dxMem, 0) != VK_SUCCESS) {
        dx_release(); rlog("[VKR] shared: bind FAILED\n"); return 0;
    }
    s_dxW = w; s_dxH = h; s_dxMutex = mutex ? 1 : 0;
    rlog("[VKR] shared: D3D11 texture %dx%d imported (%s handle, %llu bytes, type %u; image types 0x%X, handle types 0x%X, %d)\n",
         w, h, kmt ? "KMT" : "NT", (unsigned long long)mr.size, mai.memoryTypeIndex, mr.memoryTypeBits, hbits, hres);
    return 1;
}
/* main.cpp перед рендером кадра: 1 — кадр в общую текстуру (если она того же размера), 0 — прежнее чтение целиком */
extern "C" void dcvk_shared_use(int on) { s_dxUse = on ? 1 : 0; }
/* последний кадр ушёл в общую текстуру (main.cpp копирует её в текстуру SDL, а не заливает s_rbMap) */
extern "C" int dcvk_shared_last(void) { return s_lastShared; }
extern "C" int dcvk_enabled(void) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("DC_GPU_RASTER"); en = (e && *e == '1') ? 1 : 0; }
    return en;
}
/* ⭐ 10.10.2026 (игрок: «разрешение рендера игры отдельно от окна, до 4K, минимум 320x240, как на PS1»): масштаб растра
 * 1..9 от кадра PS1 (240 строк -> 240p..2160p) меняется из меню на ходу (dcvk_set_scale, main.cpp scr.render);
 * DC_GPU_SCALE задаёт его на старте, как раньше (по умолчанию 5). Широкие экраны PS1 (640 точек - заставки загрузки) при
 * масштабе больше 5 рисуются в половинном (dcvk_scale_for): иначе цели выросли бы до 5760x4608 с MSAA. При 5 и меньше -
 * всё как было. */
static int s_gpu_scale = 0;
extern "C" int dcvk_scale(void) {
    if (!s_gpu_scale) {
        const char* e = getenv("DC_GPU_SCALE");
        int s = e ? atoi(e) : 5;
        if (s < 1) s = 1;
        if (s > 9) s = 9;
        s_gpu_scale = s;
    }
    return s_gpu_scale;
}
extern "C" void dcvk_set_scale(int s) {
    if (s < 1) s = 1;
    if (s > 9) s = 9;
    s_gpu_scale = s;
}
/* ⭐ 19.10.2026 (игрок, слоты 48 и 49: «открываю меню - экран просто чёрный», с Render Resolution 2160p): экран предметов -
 * кадр PS1 320x480, при масштабе 9 цель нужна 4320 строк, а выделено 2560 (ensureTargets: «больше предела PS1») - кадр
 * не рисовался вовсе. Так было при любом масштабе больше 5 (у игрока утром стоял 1200p). Высокие кадры (больше 256 строк)
 * теперь тоже в половинном масштабе, как широкие: 480 строк x 5 = 2400 - не меньше строк мира. DC_RENDER_TALL_HALF=0 - как было. */
extern "C" int dcvk_scale_for(int dispW, int dispH) {
    const int s = dcvk_scale();
    static int tall = -1;
    if (tall < 0) { const char* e = getenv("DC_RENDER_TALL_HALF"); tall = (e && *e == '0') ? 0 : 1; }
    return (s > 5 && (dispW > 400 || (tall && dispH > 256))) ? (s + 1) / 2 : s;
}
/* ⭐ 10.10.2026: ШИРИНА КАДРА РАСТРА ПО ФОРМЕ КАРТИНКИ. Мир ужат GTE по X в sq (memory.c dc_ws_sq) и растягивается выводом в
 * 1/sq: рисованный шириной «кадр PS1 x масштаб», он мылился по горизонтали (16:9 при 1200p - 1600 точек на 2133 показа; на
 * 32:9 было бы втрое). Теперь цель шире в 1/sq: точка кадра квадратная, «1440p» на 16:9 - ровно 2560x1440. Кадры PS1
 * шириной 640 (заставки загрузки - плоские, показываются 4:3) - как были. Предел 7680 (32:9 при 2160p).
 * DC_RENDER_WIDE=0 - как было. */
extern "C" float dc_ws_sq(void);
extern "C" int dcvk_target_w(int dispW, int scale) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("DC_RENDER_WIDE"); en = (e && *e == '0') ? 0 : 1; }
    const float sq = dc_ws_sq();
    if (!en || dispW > 400 || !(sq < 0.999f)) return dispW * scale;
    int w = (int)lroundf((float)dispW * (float)scale / sq);
    if (w > 7680) w = 7680;
    return w;
}

/* Vertical extent of the current stream in draw space — the present gate
 * uses it to tell double-buffered world frames (drawn OUTSIDE the shown
 * area until the flip) from draw-as-you-go single-buffer screens (menus). */
static float s_streamMinY = 1e9f, s_streamMaxY = -1e9f;
static int s_vertsLow = 0, s_vertsHigh = 0;   /* VRAM halves y<248 / y>=248 */
extern "C" int dcvk_stream_count(void) { return s_nverts; }
extern "C" int dcvk_stream_hits(int y0, int y1) {
    return s_nverts > 0 && s_streamMaxY >= (float)y0 && s_streamMinY <= (float)y1;
}

/* Frame-submission tracking (2026-08-16, camera-cut mixed-frame flash): each
 * DMA ch2 linked-list transfer = one DrawOTag = one game frame submission.
 * At a camera cut the game submits the last OLD-view frame AND the first
 * NEW-view frame between two presents (no flip in between) — rendering that
 * concatenated stream mixed two views into one torn image. dma.c marks each
 * submission start; when the stream holds >= 2 submissions, dcvk_render draws
 * only the TAIL (the newest frame). */
static int s_actorVerts = 0;        /* вершин персонажа в текущем кадре */
/* ⏱ Куда девались примитивы ПЕРСОНАЖА (09.09.2026, «часть Регины пропадает»).
 * Замер показал: игра прислала 1938 вершин, до потока дошло 1611 — 327
 * потеряно у нас. Считаем по причинам, чтобы знать, какая ветка их съедает. */
static int s_alost_oob = 0, s_alost_behind = 0, s_alost_short = 0, s_alost_full = 0;
static int s_alost_oob_p = 0, s_alost_behind_p = 0, s_alost_short_p = 0, s_alost_full_p = 0;
static int s_actor_out_all = 0, s_actor_out_all_p = 0;
extern "C" int dcvk_actor_out_all(void) { return s_actor_out_all_p; }

/* ⭐ ПРИЗНАК «У ПЕРСОНАЖА НЕ ХВАТАЕТ ГЕОМЕТРИИ» (09.09.2026).
 * Прежний признак смотрел на ВЕСЬ поток кадра и потому был слеп к главному
 * случаю: замер по сцене двери — кадр f=1255, где игра прислала 1641 вершину
 * Регины вместо 1938 (85 %), а общий поток остался прежним, и фильтр по
 * потоку не сработал. Считаем медиану восьми последних кадров ПО САМОМУ
 * персонажу и сравниваем с текущим. Ноль не считаем провалом: персонажа может
 * законно не быть в кадре. DC_ACTOR_DROP=<проценты> (умолч. 92), 0 — выкл. */
static int s_actorHist[8], s_actorHistN = 0, s_actorMed = 0;
extern "C" int dcvk_actor_drop(void) {
    static int thr = -1;
    if (thr < 0) { const char* e = getenv("DC_ACTOR_DROP"); thr = e ? atoi(e) : 0; }   /* по умолчанию ВЫКЛ: маскировка кадров даёт свой артефакт */
    if (thr <= 0 || s_actorMed <= 300) return 0;
    int cur = s_actor_out_all;                     /* текущий накопленный кадр */
    if (cur <= 0) return 0;
    return (cur * 100 < s_actorMed * thr) ? 1 : 0;
}
static void actor_med_update(int v) {
    if (v <= 0) return;
    if (s_actorHistN < 8) s_actorHist[s_actorHistN++] = v;
    else { for (int i = 1; i < 8; i++) s_actorHist[i-1] = s_actorHist[i]; s_actorHist[7] = v; }
    int t[8]; int n = s_actorHistN;
    for (int i = 0; i < n; i++) t[i] = s_actorHist[i];
    for (int i = 1; i < n; i++) { int k = t[i], j = i - 1;
        while (j >= 0 && t[j] > k) { t[j+1] = t[j]; j--; } t[j+1] = k; }
    s_actorMed = t[n/2];
}
extern "C" void dcvk_actor_lost(int* oob, int* behind, int* shortp, int* full) {
    *oob = s_alost_oob_p; *behind = s_alost_behind_p; *shortp = s_alost_short_p; *full = s_alost_full_p;
}
#define ACTOR_TRI(v) (((int)(v)[11] & 16) != 0)
extern "C" int dcvk_actor_verts(void) { return s_actorVerts; }
static int s_prev_no3d_pub = 0;   /* см. dcvk_no_world_now ниже */
/* ⭐ ПРИЗНАК «КАДР НЕДОСОБРАН» (09.09.2026). Медиана числа вершин за
 * последние восемь кадров против текущего: на переходе через дверь ровно один
 * кадр приходит с 2361 вершиной там, где у соседей 2658 — и именно в нём у
 * Регины нет руки. Порог в процентах задаёт DC_VERT_HOLD (умолч. 88),
 * 0 — выключить признак. Решение «показывать или нет» принимает main.cpp. */
static int s_vertMed = 0;           /* медиана числа вершин за 8 показанных кадров */
extern "C" int dcvk_vert_drop(void) {
    /* Считается ПО ТЕКУЩЕМУ накопленному потоку, а не по прошлому кадру:
     * main.cpp спрашивает это ДО рендера, решая, показывать ли кадр. */
    /* ⭐ ПОРОГ 88 % ПРОПУСКАЛ РЕАЛЬНЫЙ ДЕФЕКТ (09.09.2026, кольцо кадров
     * игрока, кадр 46: «часть Регины отсутствует»). Замер по ring2.txt:
     * соседи 2688 / 2658 / **2364** / 2658 / 2658 — провал ровно на 11.1 %,
     * то есть 88.9 % от медианы, и фильтр «ниже 88 %» его не поймал,
     * промахнувшись на одну десятую процента. Порог поднят до 93 %:
     * пропажа даже одной конечности стоит заметно больше 7 % потока, а
     * ложных срабатываний бояться нечего — правило применяется ТОЛЬКО на
     * переходах и прячет один кадр подряд (см. main.cpp, vhold_run). */
    static int thr = -1;
    if (thr < 0) { const char* e = getenv("DC_VERT_HOLD"); thr = e ? atoi(e) : 88; }
    if (thr <= 0 || s_vertMed <= 600) return 0;
    return (s_nverts * 100 < s_vertMed * thr) ? 1 : 0;
}
static int s_submits = 0;
static int s_submitMarkVerts = 0;   /* s_nverts at the LAST submission start */
static int s_submitMarkDraws = 0;
static int s_submitMarkLow = 0, s_submitMarkHigh = 0;
/* ⭐ 09.09.2026: отметки ВСЕХ сборок кадра, а не только последней.
 * На переходе через дверь замер кольца показал кадры с `submits=4` и вдвое
 * большим числом вершин (20550 против обычных 10275) — в один показанный кадр
 * попадали ДВЕ сборки мира. Прежний сторож смотрел только на последнюю
 * отметку: если игра добавляла после мира ещё одну маленькую сборку (HUD,
 * плашка названия комнаты), условие «голова >= 600 вершин» проверялось уже
 * для неё, и старый мир оставался в кадре вместе с новым. Теперь ищем
 * последнюю СУЩЕСТВЕННУЮ сборку — ту, после которой нарисовано >= 600
 * вершин, — и рисуем от неё. DC_MIXED_TAIL=0 возвращает прежнее поведение. */
#define SUBMIT_MARKS 16
static struct { int verts, draws, low, high; } s_marks[SUBMIT_MARKS];
static int s_markCount = 0;
extern "C" void gpu_get_draw_offset(int *ox, int *oy);   /* gpu.c, GP0 0xE5 */
/* 25.09 ночь: полигоны персонажей, которые игра не рисует (за краем кадра, спиной к камере), — в видовом пространстве
 * для карты теней (pt_renderer.c); число вершин */
extern "C" int pt_shx_take(float* dst, int maxVerts, int vfloats);
static int s_submitDrawY = -1;      /* draw offset Y at the last submission start */
extern "C" void dcvk_note_submission(void) {
    /* translucent-last is PER BUILD (2026-08-30, "тень впереди Регины на
     * кадр"): when two game builds land in one present, deferred semis from
     * build A must draw before build B's opaque - flushing only at render
     * time mixed the frames and the shadow decal from the newer build
     * appeared ahead of the older build's body. */
    if (!s_semiLate.empty()) {
        s_draws.insert(s_draws.end(), s_semiLate.begin(), s_semiLate.end());
        s_semiLate.clear();
    }
    s_submits++;
    { int ox = 0, oy = 0; gpu_get_draw_offset(&ox, &oy); s_submitDrawY = oy; }
    s_submitMarkVerts = s_nverts;
    s_submitMarkDraws = (int)s_draws.size();
    s_submitMarkLow = s_vertsLow;
    s_submitMarkHigh = s_vertsHigh;
    if (s_markCount < SUBMIT_MARKS) {
        s_marks[s_markCount].verts = s_nverts;
        s_marks[s_markCount].draws = (int)s_draws.size();
        s_marks[s_markCount].low   = s_vertsLow;
        s_marks[s_markCount].high  = s_vertsHigh;
        s_markCount++;
    }
}
extern "C" int dcvk_submit_count(void) { return s_submits; }


// Unlocked implementation: callers must hold ptvk_gpu_lock (the PT trace runs
// on its own thread and shares the queue / VRAM image with this backend).
static int raster_init_impl(void) {
    static int state = 0;   // 0 = untried, 1 = ok, -1 = failed
    if (state) return state > 0;
    state = -1;
    s_log = dc_diag_log() ? fopen("dcvk_raster.txt", "w") : nullptr;
    s_rlogMirror = 1;                /* 23.10.2026: the bring-up into the game's log too (rlog) */
    C = ptvk_core_acquire();
    if (!C) {   /* 26.10.2026: the window shows the core's own reason first (ptvk_last_error); this only if it gave none */
        snprintf(s_rasterErr, sizeof s_rasterErr, "the Vulkan core of the renderer could not start");
        rlog("[VKR] core acquire FAILED\n"); s_rlogMirror = 0; return 0;
    }
    pc_load();                       /* 11.10: кэш конвейеров - до первой сборки */
    if (!buildFixed()) { std::fprintf(stderr, "raster: bring-up FAILED%c", 10); s_rlogMirror = 0; return 0; }
    s_draws.reserve(512);
    std::fprintf(stderr, "raster: ready%s%c", s_rasterSafe >= 2 ? " (safe shaders, level 2: no lights, shadows, materials, MSAA)"
                 : s_rasterSafe ? " (safe shaders, level 1: no texture array, textures from mods off)" : "", 10);
    vram_note("raster ready");
    s_rlogMirror = 0;
    state = 1;
    return 1;
}
// ===================================================================================================================
// 05.10.2026 — ТЕКСТУРЫ PNG ИЗ MODS (игрок: «доделай всё»; решение 04.10: «текстуры PNG — любое разрешение, по хешу
// текстуры, подменяем на нашей стороне, игра не знает»). Приложение Dino Crisis ME Studio выгружает текстуры в
// source\textures\<файл>\tpXXXX_cxN_cyN_uA-B_vC-D.png; «Сконвертировать всё» пишет mods\textures\index.txt: страница
// (с режимом цвета), палитра, окно UV, ключ содержимого (FNV-1a 64: индексы текселей окна и все цвета палитры; у 15 бит —
// сами пиксели; цвета без бита 15) и путь картинки. Здесь на каждый текстурный треугольник: замены с той же страницей и палитрой → окно UV
// треугольника внутри окна замены и ключ VRAM совпал → во втором буфере вершин номер картинки и место в ней, шейдер берёт
// цвет из картинки (любого размера, с мипами), прозрачность — её альфа, бит 15 — как у текселя игры. Ключ перепроверяется
// только после записи в VRAM (эпоха gpu.c: заливки, копии, загрузка сейва). Картинки декодирует свой поток (WIC), на GPU
// они попадают в начале кадра. DC_MODS=0 / DC_MODS_TEXTURES=0 — выключить.
// ===================================================================================================================
extern "C" const char* dc_mods_dir(void);           /* dc_mods.c: папка mods с '\' в конце, NULL — моды выключены */
extern "C" unsigned    dc_tex_epoch_now(void);      /* gpu.c: растёт при записи в VRAM вне буферов кадра */

struct TRep {
    uint16_t tp = 0, cx = 0, cy = 0;
    int u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uint64_t key = 0;
    std::wstring path;
    unsigned epoch = 0;
    bool valid = false;
    int state = 0;            /* 0 — не грузили, 1 — в очереди, 2 — декодирована, 4 — заливается, 3 — на GPU, −1 — не читается */
    int slot = -1;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0, mips = 1;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    bool repeat = false;              /* 05.10 ночь (2): картинка меша — край повторяется (сэмплер REPEAT) */
    std::vector<uint8_t> png;         /* картинка меша из glb — байты PNG (декодирует тот же поток) */
    /* 06.10.2026 (подхват модов на лету): отметка файла, каким его декодировали (размер и время), и поколение записи —
     * декод, начатый до перечитывания или снятия, не кладётся. state −2 — снята (место освободит кадр), −3 — освобождена */
    uint64_t stamp = 0;
    unsigned gen = 0;
};
/* 05.10 ночь (2): deque — записи мешей добавляются на ходу, ссылки на прежние должны жить */
static std::deque<TRep> s_trep;
static std::unordered_map<uint64_t, std::vector<int>> s_trepBy;
static int s_trepOn = -1;                       /* −1 — индекс ещё не читали */
static std::mutex s_trepMu;
static std::condition_variable s_trepCv;
static std::vector<int> s_trepQ;
static bool s_trepThr = false;
static int s_trepSlots = 0;
static std::vector<int> s_trepUpload;           /* созданы в этом кадре — записать команды заливки */
static std::vector<std::pair<VkBuffer, VkDeviceMemory>> s_trepFree;   /* промежуточные буферы прошлого кадра */
static VkSampler s_trepSamp = VK_NULL_HANDLE;
static VkSampler s_trepSampRep = VK_NULL_HANDLE;    /* 05.10 ночь (2): для картинок мешей (UV за пределами 0..1) */
static int s_repOvSlot = -1;                       /* dcvk_rep_override: место картинки на вершинах следующих треугольников */
static float s_repOvUV[6];
static VkImage s_trepDummy = VK_NULL_HANDLE;
static VkDeviceMemory s_trepDummyMem = VK_NULL_HANDLE;
static VkImageView s_trepDummyView = VK_NULL_HANDLE;

static uint64_t trep_key_vram(const unsigned short* vram, int tp, int cx, int cy, int u0, int v0, int u1, int v1) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto put = [&](uint8_t b) { h ^= b; h *= 0x100000001B3ull; };
    const int px = (tp & 15) * 64, py = ((tp >> 4) & 1) * 256, mode = (tp >> 7) & 3;
    for (int v = v0; v <= v1; v++)
        for (int u = u0; u <= u1; u++) {
            const int y = (py + v) & 511;
            if (mode == 0) { uint16_t w = vram[y * 1024 + ((px + (u >> 2)) & 1023)]; put((uint8_t)((w >> ((u & 3) * 4)) & 15)); }
            else if (mode == 1) { uint16_t w = vram[y * 1024 + ((px + (u >> 1)) & 1023)]; put((uint8_t)((w >> ((u & 1) * 8)) & 0xFF)); }
            else { uint16_t w = vram[y * 1024 + ((px + u) & 1023)] & 0x7FFF; put((uint8_t)w); put((uint8_t)(w >> 8)); }
        }
    /* цвета — без бита 15: игра ставит его палитрам после загрузки (замер ST103: цвета те же, у всех бит 15) */
    if (mode < 2) {
        const int n = mode == 0 ? 16 : 256;
        for (int i = 0; i < n; i++) { uint16_t c = vram[(cy & 511) * 1024 + ((cx + i) & 1023)] & 0x7FFF; put((uint8_t)c); put((uint8_t)(c >> 8)); }
    }
    return h;
}

static std::wstring trep_wpath(const std::string& base, const char* dir, const char* tail) {
    std::string rel = tail;
    while (!rel.empty() && (rel.back() == '\n' || rel.back() == '\r' || rel.back() == ' ')) rel.pop_back();
    for (auto& ch : rel) if (ch == '/') ch = '\\';
    /* 18.10.2026: корень mods - в кодировке ANSI (dc_mods.c, GetModuleFileNameA), имя из index.txt - UTF-8 (пишет студия);
     * прежде всё шло как UTF-8, и при русских буквах в пути к игре текстуры модов молча не находились */
    auto widen = [](const std::string& s, UINT cp) {
        int wn = MultiByteToWideChar(cp, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(wn > 0 ? wn - 1 : 0, L'\0');
        if (wn > 0) MultiByteToWideChar(cp, 0, s.c_str(), -1, &w[0], wn);
        return w;
    };
    return widen(base + dir, CP_ACP) + widen(rel, CP_UTF8);
}

/* экраны из mods (05.10): картинка 2D-экрана (титул, меню, конец игры) любого размера вместо той, что игра заливает в VRAM
 * целиком (A0 320x240 в кадр): mods\screens\index.txt — «ширина высота ключ путь», ключ — FNV-1a 64 залитых пикселей без
 * бита 15 (пишет «Сконвертировать всё» для screens\<ИМЯ>\image.png не 320x240). Узнаётся в блите заливки (dcvk_mirror_blit). */
static std::unordered_map<uint32_t, std::vector<int>> s_scrBy;   /* (w << 16) | h → записи s_trep */
static int s_scrNow = -1;                        /* запись экрана для треугольников текущего блита (−1 — нет) */
static float s_scrRect[4];

/* строки screens\index.txt и textures\index.txt — записи (ещё не в s_trep) */
static void trep_read_lines(const std::string& base, std::vector<TRep>& out) {
    std::string ip = base + "screens\\index.txt";
    FILE* f = fopen(ip.c_str(), "rb");
    char line[2048];
    if (f) {
        int n = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            int w = 0, h = 0; unsigned long long key; int pos = 0;
            if (sscanf(line, "%d %d %llx %n", &w, &h, &key, &pos) < 3 || pos <= 0 || w <= 0 || h <= 0 || w > 1024 || h > 512) continue;
            TRep r;
            r.tp = 0xFFFF; r.u1 = w - 1; r.v1 = h - 1; r.key = key;
            r.path = trep_wpath(base, "screens\\", line + pos);
            out.push_back(std::move(r));
            n++;
        }
        fclose(f);
        std::fprintf(stderr, "mods: screens from PNG - %d (index %s)%c", n, ip.c_str(), 10);
    }
    ip = base + "textures\\index.txt";
    f = fopen(ip.c_str(), "rb");
    if (!f) return;
    int ntex = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        unsigned tp, cx, cy; int u0, v0, u1, v1; unsigned long long key; int pos = 0;
        if (sscanf(line, "%x %u %u %d %d %d %d %llx %n", &tp, &cx, &cy, &u0, &v0, &u1, &v1, &key, &pos) < 8 || pos <= 0) continue;
        ntex++;
        TRep r;
        r.tp = (uint16_t)(tp & 0x19F); r.cx = (uint16_t)cx; r.cy = (uint16_t)cy;
        r.u0 = u0; r.v0 = v0; r.u1 = u1; r.v1 = v1; r.key = key; r.path = trep_wpath(base, "textures\\", line + pos);
        out.push_back(std::move(r));
    }
    fclose(f);
    std::fprintf(stderr, "mods: textures from PNG - %d (index %s)%c", ntex, ip.c_str(), 10);
}

/* поиск по s_trep: текстуры игры — по странице и палитре, экраны — по размеру (06.10.2026: собирается заново при подхвате) */
static void trep_maps(void) {
    s_trepBy.clear();
    s_scrBy.clear();
    for (size_t i = 0; i < s_trep.size(); i++) {
        const TRep& r = s_trep[i];
        if (r.key == ~0ull || r.state == -2 || r.state == -3) continue;
        if (r.tp == 0xFFFF) { s_scrBy[(uint32_t)(r.u1 + 1) << 16 | (uint32_t)(r.v1 + 1)].push_back((int)i); continue; }
        const int mode = (r.tp >> 7) & 3;
        const uint64_t k = (uint64_t)r.tp << 32 | (uint64_t)(mode < 2 ? r.cx : 0) << 16 | (uint64_t)(mode < 2 ? r.cy : 0);
        s_trepBy[k].push_back((int)i);
    }
    /* окна одной страницы и палитры могут вкладываться (кусок двери в окне комнаты): первым — самое маленькое */
    for (auto& kv : s_trepBy)
        std::stable_sort(kv.second.begin(), kv.second.end(), [](int a, int b) {
            const TRep& x = s_trep[a]; const TRep& y = s_trep[b];
            return (x.u1 - x.u0 + 1) * (x.v1 - x.v0 + 1) < (y.u1 - y.u0 + 1) * (y.v1 - y.v0 + 1);
        });
}

static void trep_load_index(void) {
    s_trepOn = 0;
    const char* e = getenv("DC_MODS_TEXTURES");
    if (e && *e == '0') return;
    const char* base = dc_mods_dir();
    if (!base) return;
    if (!g_vk_nonuniform_tex) { std::fprintf(stderr, "mods: textures from PNG are off - the video card cannot index a texture array%c", 10); return; }
    std::vector<TRep> lines;
    trep_read_lines(base, lines);
    for (TRep& r : lines) s_trep.push_back(std::move(r));
    trep_maps();
    s_trepOn = s_trep.empty() ? 0 : 1;
}

/* 06.10.2026: размер и время файла картинки (0 — нет) */
static uint64_t trep_stamp(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (p.empty() || !GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    const uint64_t t = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    const uint64_t s = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    return (t ^ (s * 0x9E3779B97F4A7C15ull)) | 1u;
}

static void trep_worker(void) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    for (;;) {
        int idx = -1;
        std::wstring path;
        std::vector<uint8_t> png;
        unsigned gen0 = 0;
        {
            std::unique_lock<std::mutex> lk(s_trepMu);
            s_trepCv.wait(lk, [] { return !s_trepQ.empty(); });
            idx = s_trepQ.back();
            s_trepQ.pop_back();
            path = s_trep[idx].path;
            png.swap(s_trep[idx].png);
            gen0 = s_trep[idx].gen;
        }
        const uint64_t stamp = png.empty() ? trep_stamp(path) : 0;   /* 06.10.2026: файл, каким его декодируем */
        std::vector<uint8_t> px;
        UINT w = 0, h = 0;
        bool ok = false;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        IWICStream* stm = nullptr;
        bool opened = false;
        if (fac && !png.empty())                                   /* 05.10 ночь (2): картинка меша из glb — из памяти */
            opened = SUCCEEDED(fac->CreateStream(&stm)) && SUCCEEDED(stm->InitializeFromMemory(png.data(), (DWORD)png.size())) &&
                     SUCCEEDED(fac->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec));
        else if (fac)
            opened = SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec));
        if (opened &&
            SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            px.resize((size_t)w * h * 4);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stm) stm->Release();
        std::vector<uint8_t>().swap(png);
        std::lock_guard<std::mutex> lk(s_trepMu);
        TRep& r = s_trep[idx];
        if (r.gen != gen0 || r.state != 1) continue;   /* 06.10.2026: перечитана или снята, пока декодировалась — не эта картинка */
        r.stamp = stamp;
        if (ok) { r.rgba.swap(px); r.w = (int)w; r.h = (int)h; r.state = 2; }
        else { r.state = -1; std::fprintf(stderr, "mods: texture %ls - cannot read the PNG%c", path.c_str(), 10); }
    }
}

/* DC_TEXREP_LOG=1 — раз в 300 текстурных треугольников из списка: почему замена не взялась (флаги, окно, нет страницы и
 * палитры, окно UV, ключ) — разбор «текстура из mods не подменилась» */
static int s_trepLog = -1;
static long s_trepSt[8];
static void trep_stat(int k, const float* v) {
    if (s_trepLog < 0) { const char* e = getenv("DC_TEXREP_LOG"); s_trepLog = (e && *e == '1') ? 1 : 0; }
    if (!s_trepLog) return;
    s_trepSt[k]++;
    static long n = 0, said = 0;
    if (k >= 3 && said < 400 && (++n % 50) == 1) {
        said++;
        std::fprintf(stderr, "texrep: %s tp %03x clut %d,%d flags %x uv (%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)%c",
                     k == 3 ? "no page+palette" : k == 4 ? "UV window outside" : k == 5 ? "wrong key" : "taken",
                     (int)v[8] & 0x19F, (int)v[9] & 0x3FF, (int)v[10] & 0x1FF, (int)v[11],
                     v[2], v[3], v[VERT_FLOATS + 2], v[VERT_FLOATS + 3], v[2 * VERT_FLOATS + 2], v[2 * VERT_FLOATS + 3], 10);
    }
    long tot = 0; for (int i = 0; i < 8; i++) tot += s_trepSt[i];
    if (tot % 20000 == 0)
        std::fprintf(stderr, "texrep: totals flags %ld window %ld none %ld uv %ld key %ld taken %ld%c",
                     s_trepSt[1], s_trepSt[2], s_trepSt[3], s_trepSt[4], s_trepSt[5], s_trepSt[6], 10);
}

static void trep_queue(int idx) {
    TRep& r = s_trep[idx];
    if (r.state != 0) return;
    std::lock_guard<std::mutex> lk(s_trepMu);
    r.state = 1;
    s_trepQ.push_back(idx);
    if (!s_trepThr) { std::thread(trep_worker).detach(); s_trepThr = true; }
    s_trepCv.notify_one();
}

/* 05.10 ночь (2) — КАРТИНКИ МЕШЕЙ ИЗ MODS (dc_meshrep.cpp): запись в том же списке, место в массиве repTex — когда картинка
 * дойдёт до GPU. Источник: файл (path), байты PNG из glb (png) или готовые пиксели RGBA (цвет материала без картинки).
 * Зовётся только из потока эмуляции (как trep_tri); список меняется под замком — его читает подготовка кадра. */
extern "C" int dcvk_rep_tex_add(const wchar_t* path, const uint8_t* png, size_t png_n, const uint8_t* rgba, int w, int h, int repeat) {
    if (s_trepOn < 0) trep_load_index();
    if (!g_vk_nonuniform_tex) return -1;
    std::lock_guard<std::mutex> lk(s_trepMu);
    TRep r;
    r.repeat = repeat != 0;
    if (path) r.path = path;
    if (png && png_n) r.png.assign(png, png + png_n);
    if (rgba && w > 0 && h > 0) { r.rgba.assign(rgba, rgba + (size_t)w * h * 4); r.w = w; r.h = h; r.state = 2; }
    else if (r.path.empty() && r.png.empty()) return -1;
    r.key = ~0ull;                                                   /* не текстура игры: в s_trepBy не попадает */
    s_trep.push_back(std::move(r));
    s_trepOn = 1;
    return (int)s_trep.size() - 1;
}
/* место картинки в массиве или −1 (ещё грузится / не читается); первая просьба ставит декод в очередь */
extern "C" int dcvk_rep_tex_slot(int idx) {
    if (idx < 0 || idx >= (int)s_trep.size()) return -1;
    TRep& r = s_trep[idx];
    if (r.state == 0) trep_queue(idx);
    return r.state == 3 ? r.slot : -1;
}

/* 06.10.2026 (подхват модов на лету, dc_meshrep.cpp): картинка снятой модели — её образ и место освободит следующий кадр */
extern "C" void dcvk_rep_tex_drop(int idx) {
    std::lock_guard<std::mutex> lk(s_trepMu);
    if (idx < 0 || idx >= (int)s_trep.size()) return;
    TRep& r = s_trep[idx];
    if (r.state == -2 || r.state == -3) return;
    r.gen++;
    r.state = -2;
    std::vector<uint8_t>().swap(r.rgba);
    std::vector<uint8_t>().swap(r.png);
}

/* 06.10.2026 (подхват модов на лету, dc_mods.c — поток эмуляции): screens\index.txt и textures\index.txt заново. Та же строка
 * остаётся своей записью (её PNG поменялся — декодируется снова на то же место в массиве), новая — новая запись, ушедшая —
 * снята. Пока картинка декодируется, виден рисунок игры. */
extern "C" void dcvk_mods_reload(void) {
    if (s_trepOn < 0) return;                    /* ещё не читали: первое обращение прочтёт файлы, как они есть */
    const char* e = getenv("DC_MODS_TEXTURES");
    const char* base = dc_mods_dir();
    if ((e && *e == '0') || !base || !g_vk_nonuniform_tex) return;
    std::vector<TRep> want;
    trep_read_lines(base, want);
    auto line = [](const TRep& r) {
        uint64_t h = 0xCBF29CE484222325ull;
        auto put = [&](const void* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t*)p)[i]; h *= 0x100000001B3ull; } };
        put(&r.tp, 2); put(&r.cx, 2); put(&r.cy, 2); put(&r.u0, 4); put(&r.v0, 4); put(&r.u1, 4); put(&r.v1, 4); put(&r.key, 8);
        put(r.path.data(), r.path.size() * sizeof(wchar_t));
        return h;
    };
    std::lock_guard<std::mutex> lk(s_trepMu);
    std::unordered_map<uint64_t, int> live;
    for (size_t i = 0; i < s_trep.size(); i++) {
        const TRep& r = s_trep[i];
        if (r.key == ~0ull || r.state == -2 || r.state == -3) continue;
        live.emplace(line(r), (int)i);
    }
    std::vector<char> keep(s_trep.size(), 0);
    int added = 0, again = 0, gone = 0;
    for (TRep& w : want) {
        auto it = live.find(line(w));
        if (it != live.end() && !keep[(size_t)it->second]) {
            keep[(size_t)it->second] = 1;
            TRep& r = s_trep[(size_t)it->second];
            if (r.state != 0 && trep_stamp(r.path) != r.stamp) { r.gen++; r.state = 0; again++; }   /* PNG поменялся */
            continue;
        }
        s_trep.push_back(std::move(w));
        added++;
    }
    for (size_t i = 0; i < keep.size(); i++) {
        TRep& r = s_trep[i];
        if (r.key == ~0ull || r.state == -2 || r.state == -3 || keep[i]) continue;
        r.gen++;
        r.state = -2;
        std::vector<uint8_t>().swap(r.rgba);
        gone++;
    }
    trep_maps();
    s_scrNow = -1;
    s_trepOn = s_trep.empty() ? 0 : 1;
    std::fprintf(stderr, "mods: textures and screens picked up - %d new, %d changed, %d gone%c", added, again, gone, 10);
}
/* следующие треугольники — с этой картинкой (slot) и этими местами вершин (uv: 3 × u,v); slot < 0 — обычные */
extern "C" void dcvk_rep_override(int slot, const float* uv) {
    s_repOvSlot = slot;
    if (slot >= 0 && uv) for (int i = 0; i < 6; i++) s_repOvUV[i] = uv[i];
}

/* блит заливки CPU→VRAM (gpu_sw_renderer.c dcvk_mirror_blit) — до его треугольников: залитое совпало с экраном из mods —
 * треугольники блита берут цвет из картинки; dcvk_screen_end — после них */
extern "C" void dcvk_screen_begin(int x, int y, int w, int h) {
    s_scrNow = -1;
    if (s_trepOn < 0) trep_load_index();
    if (s_trepOn <= 0 || s_scrBy.empty()) return;
    auto it = s_scrBy.find((uint32_t)w << 16 | (uint32_t)h);
    if (it == s_scrBy.end()) return;
    const unsigned short* vr = sw_vram_base();
    uint64_t hsh = 0xCBF29CE484222325ull;
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            const uint16_t c = vr[((y + yy) & 511) * 1024 + ((x + xx) & 1023)] & 0x7FFF;
            hsh ^= (uint8_t)c; hsh *= 0x100000001B3ull;
            hsh ^= (uint8_t)(c >> 8); hsh *= 0x100000001B3ull;
        }
    for (int idx : it->second) {
        if (s_trep[idx].key != hsh) continue;
        trep_queue(idx);
        s_scrNow = idx;
        s_scrRect[0] = (float)x; s_scrRect[1] = (float)y; s_scrRect[2] = (float)w; s_scrRect[3] = (float)h;
        static int said = 0;
        if (said < 8) { said++; std::fprintf(stderr, "mods: screen %dx%d at %d,%d - picture from mods%c", w, h, x, y, 10); }
        return;
    }
}
extern "C" void dcvk_screen_end(void) { s_scrNow = -1; }

static void trep_tri(const float* v, float* rv) {
    if (s_repOvSlot >= 0) {                                           /* 05.10 ночь (2): треугольник меша из mods */
        for (int i = 0; i < 3; i++) {
            rv[i * 4] = (float)s_repOvSlot; rv[i * 4 + 1] = s_repOvUV[i * 2]; rv[i * 4 + 2] = s_repOvUV[i * 2 + 1]; rv[i * 4 + 3] = 0.f;
        }
        return;
    }
    for (int i = 0; i < 3; i++) { rv[i * 4] = -1.f; rv[i * 4 + 1] = rv[i * 4 + 2] = rv[i * 4 + 3] = 0.f; }
    if (s_trepOn < 0) trep_load_index();
    if (s_trepOn <= 0) return;
    const int flags = (int)v[11];
    if (flags & 8) {                                                  /* блит заливки: экран из mods (dcvk_screen_begin) */
        if (s_scrNow >= 0 && s_trep[s_scrNow].state == 3)
            for (int i = 0; i < 3; i++) {
                rv[i * 4] = (float)s_trep[s_scrNow].slot;
                rv[i * 4 + 1] = (v[i * VERT_FLOATS + 2] - s_scrRect[0]) / s_scrRect[2];
                rv[i * 4 + 2] = (v[i * VERT_FLOATS + 3] - s_scrRect[1]) / s_scrRect[3];
            }
        return;
    }
    if (!(flags & 1)) return;
    if (flags & (8 | 64 | 128 | 256 | 1024)) { trep_stat(1, v); return; }   /* копия VRAM, шрифт, решётки */
    const int tp = (int)v[8] & 0x19F;
    const int iy = (int)v[9], iz = (int)v[10];
    if (((iy >> 10) & 31) || ((iz >> 10) & 31)) { trep_stat(2, v); return; }  /* окно текстуры (повтор) — как у игры */
    const int mode = (tp >> 7) & 3;
    const int cx = iy & 0x3FF, cy = iz & 0x1FF;
    const uint64_t k = (uint64_t)tp << 32 | (uint64_t)(mode < 2 ? cx : 0) << 16 | (uint64_t)(mode < 2 ? cy : 0);
    auto it = s_trepBy.find(k);
    if (it == s_trepBy.end()) { trep_stat(3, v); return; }
    float umin = v[2], umax = v[2], vmin = v[3], vmax = v[3];
    for (int i = 1; i < 3; i++) {
        const float u = v[i * VERT_FLOATS + 2], vv = v[i * VERT_FLOATS + 3];
        umin = u < umin ? u : umin; umax = u > umax ? u : umax; vmin = vv < vmin ? vv : vmin; vmax = vv > vmax ? vv : vmax;
    }
    int why = 4;
    for (int idx : it->second) {
        TRep& r = s_trep[idx];
        if (umin < r.u0 - 0.01f || umax > r.u1 + 1.01f || vmin < r.v0 - 0.01f || vmax > r.v1 + 1.01f) continue;
        const unsigned ep = dc_tex_epoch_now();
        if (r.epoch != ep || !r.epoch) {
            r.epoch = ep ? ep : 1;
            r.valid = trep_key_vram(sw_vram_base(), r.tp, r.cx, r.cy, r.u0, r.v0, r.u1, r.v1) == r.key;
        }
        if (!r.valid) { why = 5; continue; }
        trep_stat(6, v);
        trep_queue(idx);
        if (r.state != 3) return;                                       /* ещё грузится — пока родная */
        const float W = (float)(r.u1 - r.u0 + 1), H = (float)(r.v1 - r.v0 + 1);
        for (int i = 0; i < 3; i++) {
            rv[i * 4] = (float)r.slot;
            rv[i * 4 + 1] = (v[i * VERT_FLOATS + 2] - (float)r.u0) / W;
            rv[i * 4 + 2] = (v[i * VERT_FLOATS + 3] - (float)r.v0) / H;
        }
        return;
    }
    trep_stat(why, v);
}

static void trep_write_slot(int slot, VkImageView view, bool repeat = false) {
    VkDescriptorImageInfo ii{ repeat && s_trepSampRep ? s_trepSampRep : s_trepSamp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = s_dset; w.dstBinding = 8; w.dstArrayElement = (uint32_t)slot; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(C->device, 1, &w, 0, nullptr);
}

/* сэмплер (мипы, линейно, край — прижать) и заглушка 1x1 во все места массива: набор дескрипторов всегда валиден */
static bool trep_gpu_init(void) {
    if (ptvk_lost()) return false;      /* 18.10: видеокарта потеряна */
    if (s_trepDummyView == VK_NULL_HANDLE) {   /* 25.10.2026: второй вызов (безопасные шейдеры, новый набор) - только записи ниже */
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 16.f;
    if (vkCreateSampler(C->device, &sci, nullptr, &s_trepSamp) != VK_SUCCESS) return false;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;     /* 05.10 ночь (2): меши */
    if (vkCreateSampler(C->device, &sci, nullptr, &s_trepSampRep) != VK_SUCCESS) s_trepSampRep = VK_NULL_HANDLE;
    VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM; ici.extent = { 1, 1, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(C->device, &ici, nullptr, &s_trepDummy) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(C->device, s_trepDummy, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(C->device, &mai, nullptr, &s_trepDummyMem) != VK_SUCCESS) return false;
    vkBindImageMemory(C->device, s_trepDummy, s_trepDummyMem, 0);
    VkImageViewCreateInfo ivi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivi.image = s_trepDummy; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D; ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(C->device, &ivi, nullptr, &s_trepDummyView) != VK_SUCCESS) return false;
    cmdBegin();
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; b.image = s_trepDummy;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    if (!cmdSubmitWait()) return false;
    }
    std::vector<VkDescriptorImageInfo> ii((size_t)s_trepCap, VkDescriptorImageInfo{ s_trepSamp, s_trepDummyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = s_dset; w.dstBinding = 8; w.dstArrayElement = 0; w.descriptorCount = (uint32_t)s_trepCap;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = ii.data();
    vkUpdateDescriptorSets(C->device, 1, &w, 0, nullptr);
    return true;
}

/* 06.10.2026 (подхват модов на лету): образ картинки уходит (перечитана или снята) — GPU здесь простаивает (до записи команд
 * кадра); место снятой картинки в массиве — заглушке и в список свободных (мест всего s_trepCap) */
static std::vector<int> s_trepFreeSlots;
static void trep_release(TRep& r) {
    if (r.view) vkDestroyImageView(C->device, r.view, nullptr);
    if (r.img) vkDestroyImage(C->device, r.img, nullptr);
    if (r.mem) vkFreeMemory(C->device, r.mem, nullptr);
    if (r.stage) vkDestroyBuffer(C->device, r.stage, nullptr);
    if (r.smem) vkFreeMemory(C->device, r.smem, nullptr);
    r.view = VK_NULL_HANDLE; r.img = VK_NULL_HANDLE; r.mem = VK_NULL_HANDLE; r.stage = VK_NULL_HANDLE; r.smem = VK_NULL_HANDLE;
}
static void trep_slot_free(TRep& r) {
    if (r.slot < 0) return;
    trep_write_slot(r.slot, s_trepDummyView);
    s_trepFreeSlots.push_back(r.slot);
    r.slot = -1;
}

/* до записи команд кадра (GPU простаивает): образы для декодированных картинок, их места в массиве дескрипторов */
static void trep_frame_prepare(void) {
    for (auto& f : s_trepFree) { vkDestroyBuffer(C->device, f.first, nullptr); vkFreeMemory(C->device, f.second, nullptr); }
    s_trepFree.clear();
    if (s_trepOn <= 0 || !s_trepSamp) return;
    std::lock_guard<std::mutex> lk(s_trepMu);
    for (size_t i = 0; i < s_trep.size(); i++) {
        TRep& r = s_trep[i];
        if (r.state == -2) { trep_release(r); trep_slot_free(r); r.state = -3; continue; }   /* 06.10.2026: снята */
        if (r.state != 2) continue;
        trep_release(r);                                /* 06.10.2026: перечитана — прежний образ уходит, место остаётся */
        if (r.slot < 0 && s_trepSlots >= s_trepCap && s_trepFreeSlots.empty()) {
            static int said = 0;
            if (!said) { said = 1; std::fprintf(stderr, "mods: textures - all %d places taken, the rest keep the game's own%c", (int)s_trepCap, 10); }
            r.state = -1; continue;
        }
        int mips = 1;
        for (int d = r.w > r.h ? r.w : r.h; d > 1; d >>= 1) mips++;
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = { (uint32_t)r.w, (uint32_t)r.h, 1 };
        ici.mipLevels = (uint32_t)mips; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool ok = vkCreateImage(C->device, &ici, nullptr, &r.img) == VK_SUCCESS;
        if (ok) {
            VkMemoryRequirements mr{};
            vkGetImageMemoryRequirements(C->device, r.img, &mr);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = mr.size; mai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ok = mai.memoryTypeIndex != ~0u && vkAllocateMemory(C->device, &mai, nullptr, &r.mem) == VK_SUCCESS &&
                 vkBindImageMemory(C->device, r.img, r.mem, 0) == VK_SUCCESS;
        }
        if (ok) {
            VkImageViewCreateInfo ivi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            ivi.image = r.img; ivi.viewType = VK_IMAGE_VIEW_TYPE_2D; ivi.format = VK_FORMAT_R8G8B8A8_UNORM;
            ivi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)mips, 0, 1 };
            ok = vkCreateImageView(C->device, &ivi, nullptr, &r.view) == VK_SUCCESS;
        }
        void* map = nullptr;
        if (ok) ok = makeBuf((VkDeviceSize)r.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.stage, &r.smem, &map) && map;
        if (!ok) {
            trep_release(r); trep_slot_free(r);
            r.state = -1; std::fprintf(stderr, "mods: texture %ls - no video memory for it%c", r.path.c_str(), 10); continue;
        }
        memcpy(map, r.rgba.data(), r.rgba.size());
        std::vector<uint8_t>().swap(r.rgba);
        r.mips = mips;
        if (r.slot < 0) {                               /* 06.10.2026: место снятой картинки — первым */
            if (!s_trepFreeSlots.empty()) { r.slot = s_trepFreeSlots.back(); s_trepFreeSlots.pop_back(); }
            else r.slot = s_trepSlots++;
        }
        trep_write_slot(r.slot, r.view, r.repeat);
        s_trepUpload.push_back((int)i);
        r.state = 4;                                    /* образ есть, заливка — в командах этого кадра */
    }
}

/* в командах кадра до прохода: промежуточный буфер → мип 0, остальные мипы — уменьшением, всё — в SHADER_READ_ONLY */
static void trep_frame_upload(VkCommandBuffer cmd) {
    for (int idx : s_trepUpload) {
        TRep& r = s_trep[idx];
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.image = r.img; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)r.mips, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy bic{};
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { (uint32_t)r.w, (uint32_t)r.h, 1 };
        vkCmdCopyBufferToImage(cmd, r.stage, r.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        int mw = r.w, mh = r.h;
        for (int m = 1; m < r.mips; m++) {
            VkImageMemoryBarrier s = b;
            s.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(m - 1), 1, 0, 1 };
            s.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; s.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            s.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; s.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &s);
            const int nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
            VkImageBlit bl{};
            bl.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(m - 1), 0, 1 };
            bl.srcOffsets[1] = { mw, mh, 1 };
            bl.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, 1 };
            bl.dstOffsets[1] = { nw, nh, 1 };
            vkCmdBlitImage(cmd, r.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            mw = nw; mh = nh;
        }
        VkImageMemoryBarrier f[2];
        f[0] = b; f[1] = b;
        f[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)(r.mips - 1), 0, 1 };
        f[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        f[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(r.mips - 1), 1, 0, 1 };
        f[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        for (auto& x : f) {
            x.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            x.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                             r.mips > 1 ? 2 : 1, r.mips > 1 ? f : &f[1]);
        s_trepFree.push_back({ r.stage, r.smem });
        r.stage = VK_NULL_HANDLE; r.smem = VK_NULL_HANDLE;
        r.state = 3;
    }
    s_trepUpload.clear();
}

// ===================================================================================================================
// 06.10.2026: МЕШИ ИЗ MODS НА ВИДЕОКАРТЕ. Игрок: «мод не должен тормозить даже на сильно устаревших ПК — 60 кадров, как
// игра PS1». Прежде каждый треугольник меша из mods шёл путём примитива PS1 (зеркало программного растра: процессор ещё и
// растеризовал его в видеопамять PS1, а вершины считал сам) — Ада Вонг на 113 тысяч треугольников давала 24 кадра, и поток
// растра (256 К вершин) часть модели отбрасывал. Теперь модель лежит в видеопамяти (пул вершин позы покоя и пул индексов —
// один раз), за кадр dc_meshrep.cpp отдаёт экземпляр (кости, свет, камера, ~2 КБ): dcvk_mesh_draw кладёт его в буфер
// экземпляров (своя половина на кадр — кадр B и перенос комнаты прошлого показа читают свою) и draw вида «меш» в общий
// список кадра — порядок, затемнения, полосы кино, интерфейс поверх — как у всего остального. Проходы: основной и кадр B —
// конвейер 12 растра (dc_mesh.vert + тот же dc_raster.frag), карта теней — dc_mesh_sh.vert, свечение и отражения —
// dc_mesh_fx.vert. DC_MESH_GPU=0 — прежний путь процессора (он же — если видеокарта чего-то не дала).
// ===================================================================================================================
struct MeshVtx { float p[3], n[3], uv[2]; uint8_t j[4], w[4], c[4]; float slot; };
static_assert(sizeof(MeshVtx) == 48, "MeshVtx: 48 bytes, as in dc_mesh_common.glsl");
struct MeshGpu { uint32_t vbase, ibase, icount; int blk; uint32_t nv; };
static std::vector<MeshGpu> s_mesh;
/* 06.10.2026 (подхват модов на лету): места снятых моделей в блоках пула — модель, которая влезает (та же модель, сохранённая
 * заново), встаёт туда, а не в конец блока */
struct MeshHole { int blk; VkDeviceSize vo, vn, io, in; };
static std::vector<MeshHole> s_mholes;
/* ⭐ 06.10.2026 (2), игрок: «96 МБ вершин и 48 МБ индексов — ни в какое сравнение с современными играми; поднять предел».
 * Пул — БЛОКИ, которые добавляются по мере загрузки моделей: без модов видеопамять не занята вовсе; блок — не меньше
 * 64 МБ вершин и 32 МБ индексов или под модель целиком. Потолок всех блоков — видеопамять карты минус 2 ГБ игре (кадр,
 * MSAA, тени, текстуры; на карте до 4 ГБ — половина), DC_MESH_POOL_MB — вручную. Не влезло — модель идёт прежним путём
 * процессора, как и раньше при полном пуле. */
struct MeshBlock { VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE; VkDeviceMemory vm = VK_NULL_HANDLE, im = VK_NULL_HANDLE;
                   VkDeviceSize vcap = 0, icap = 0, vused = 0, iused = 0; };
static std::vector<MeshBlock> s_mblk;
static VkDeviceSize s_mTotal = 0;                  /* выделено всеми блоками */
#define MESH_INST_MAX 512                      /* экземпляров за кадр */
#define MESH_BONE_MAX 8192                     /* костей за кадр */
#define MESH_INST_V4  9                        /* vec4 на экземпляр (dc_mesh_common.glsl) */
#define MESH_BONE_V4  10                       /* vec4 на кость */
static VkBuffer       s_mInstBuf = VK_NULL_HANDLE, s_mBoneBuf = VK_NULL_HANDLE;
static VkDeviceMemory s_mInstMem = VK_NULL_HANDLE, s_mBoneMem = VK_NULL_HANDLE;
static float*         s_mInstMap = nullptr;
static float*         s_mBoneMap = nullptr;
static int            s_mInstN = 0, s_mBoneN = 0, s_mHalf = 0;
static VkDescriptorSetLayout s_mdsl = VK_NULL_HANDLE;
static VkDescriptorPool      s_mdpool = VK_NULL_HANDLE;
static VkDescriptorSet       s_mdset[2] = {};
static VkPipelineLayout      s_mplLay = VK_NULL_HANDLE;   /* набор 0 — как у растра, набор 1 — экземпляры и кости */
static VkPipeline            s_shMeshPipe = VK_NULL_HANDLE, s_fxMeshPipe = VK_NULL_HANDLE;
static int                   s_meshState = 0;             /* 0 — не готово, 1 — готово, −1 — нет (прежний путь) */
static int                   s_fxMeshTried = 0;

static int mesh_gpu_env(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_MESH_GPU"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
void mesh_layouts(void) {
    if (!mesh_gpu_env() || s_mplLay) return;
    VkDescriptorSetLayoutBinding b[2]{};
    for (int i = 0; i < 2; i++) {
        b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 2; dci.pBindings = b;
    if (vkCreateDescriptorSetLayout(C->device, &dci, nullptr, &s_mdsl) != VK_SUCCESS) { s_mdsl = VK_NULL_HANDLE; return; }
    const VkDescriptorSetLayout sl[2] = { s_dsl, s_mdsl };
    VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };   /* как у растра: наборы 0 совместимы */
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 2; plci.pSetLayouts = sl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(C->device, &plci, nullptr, &s_mplLay) != VK_SUCCESS) s_mplLay = VK_NULL_HANDLE;
}
/* 25.10.2026: повторный подъём растра (безопасные шейдеры, raster_sets_free) - набор 0 у мешей новый */
void mesh_layouts_free(void) {
    if (s_mplLay) { vkDestroyPipelineLayout(C->device, s_mplLay, nullptr); s_mplLay = VK_NULL_HANDLE; }
    if (s_mdsl) { vkDestroyDescriptorSetLayout(C->device, s_mdsl, nullptr); s_mdsl = VK_NULL_HANDLE; }
}
static void mesh_vin(VkPipelineVertexInputStateCreateInfo& vin, VkVertexInputBindingDescription& vb, VkVertexInputAttributeDescription* va) {
    vb = { 0, (uint32_t)sizeof(MeshVtx), VK_VERTEX_INPUT_RATE_VERTEX };
    va[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    va[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12 };
    va[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, 24 };
    va[3] = { 3, 0, VK_FORMAT_R8G8B8A8_UINT, 32 };
    va[4] = { 4, 0, VK_FORMAT_R8G8B8A8_UNORM, 36 };
    va[5] = { 5, 0, VK_FORMAT_R8G8B8A8_UNORM, 40 };
    va[6] = { 6, 0, VK_FORMAT_R32_SFLOAT, 44 };
    vin = VkPipelineVertexInputStateCreateInfo{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 7; vin.pVertexAttributeDescriptions = va;
}
/* конвейер меша: вершины — свой шейдер (fsm — фрагментный, может отсутствовать: карта теней), состояние — как у собрата */
static VkPipeline mesh_pipe(const char* vsPath, VkShaderModule fsm, VkRenderPass rp, VkSampleCountFlagBits ns, int kind,
                            const VkSpecializationInfo* spec) {
    if (!s_mplLay || rp == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkShaderModule vs = loadSpv(vsPath);
    if (!vs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
    st[1].pSpecializationInfo = spec;                  /* 25.10.2026: растр - с опциями (RasterSpec), dc_fx - s_repSpec */
    VkVertexInputBindingDescription vb; VkVertexInputAttributeDescription va[7];
    VkPipelineVertexInputStateCreateInfo vin; mesh_vin(vin, vb, va);
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineRasterizationConservativeStateCreateInfoEXT cons{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT };
    cons.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    if (kind == 0 && g_vk_conservative) rs.pNext = &cons;                 /* как у растра (основной проход) */
    if (kind == 2) { rs.depthBiasEnable = VK_TRUE; rs.depthBiasConstantFactor = 1.25f; rs.depthBiasSlopeFactor = 1.75f; }
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = ns;
    VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = kind == 2 ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER_OR_EQUAL;
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = kind == 2 ? 0 : 1; cb.pAttachments = &ba;
    cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] = 0.5f;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dst{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = fsm ? 2 : 1; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dst;
    gp.layout = s_mplLay; gp.renderPass = rp; gp.subpass = 0;
    VkPipeline out = VK_NULL_HANDLE;
    if (vkp_create(&gp, &out, "mesh") != VK_SUCCESS) out = VK_NULL_HANDLE;
    vkDestroyShaderModule(C->device, vs, nullptr);
    if (out == VK_NULL_HANDLE) rlog("[VKR] mesh pipeline %s FAILED\n", vsPath);
    return out;
}
static VkPipeline mesh_main_pipe(VkRenderPass rp, VkSampleCountFlagBits ns, VkShaderModule fs, const VkSpecializationInfo* spec) {
    if (!mesh_gpu_env()) return VK_NULL_HANDLE;
    return mesh_pipe("shaders\\dc_mesh.vert.spv", fs, rp, ns, 0, spec);
}
static VkPipeline mesh_fx_pipe(void) {
    if (s_fxMeshPipe != VK_NULL_HANDLE || s_fxMeshTried || s_meshState <= 0 || s_rpFX == VK_NULL_HANDLE) return s_fxMeshPipe;
    s_fxMeshTried = 1;
    VkShaderModule ffs = loadSpv(fx_fs_name());
    if (ffs) { s_fxMeshPipe = mesh_pipe("shaders\\dc_mesh_fx.vert.spv", ffs, s_rpFX, VK_SAMPLE_COUNT_1_BIT, 1, &s_repSpec);
               vkDestroyShaderModule(C->device, ffs, nullptr); }
    return s_fxMeshPipe;
}
/* буферы мешей — при первой модели: пулы вершин и индексов в видеопамяти, экземпляры и кости (видны процессору) */
static bool mesh_buffers(void) {
    if (s_meshState) return s_meshState > 0;
    s_meshState = -1;
    if (!mesh_gpu_env() || !s_mplLay || s_pipes[12] == VK_NULL_HANDLE) return false;
    /* 06.10.2026 (2): пулов вершин и индексов здесь больше нет — блоки по мере загрузки моделей (mesh_block_for) */
    const VkDeviceSize ih = (VkDeviceSize)MESH_INST_MAX * MESH_INST_V4 * 16, bh = (VkDeviceSize)MESH_BONE_MAX * MESH_BONE_V4 * 16;
    if (!makeBuf(ih * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &s_mInstBuf, &s_mInstMem, (void**)&s_mInstMap) || !s_mInstMap) return false;
    if (!makeBuf(bh * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &s_mBoneBuf, &s_mBoneMem, (void**)&s_mBoneMap) || !s_mBoneMap) return false;
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 };
    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 2; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(C->device, &dpci, nullptr, &s_mdpool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout ls[2] = { s_mdsl, s_mdsl };
    VkDescriptorSetAllocateInfo dsa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsa.descriptorPool = s_mdpool; dsa.descriptorSetCount = 2; dsa.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(C->device, &dsa, s_mdset) != VK_SUCCESS) return false;
    for (int h = 0; h < 2; h++) {
        VkDescriptorBufferInfo bi[2] = { { s_mInstBuf, ih * (VkDeviceSize)h, ih }, { s_mBoneBuf, bh * (VkDeviceSize)h, bh } };
        VkWriteDescriptorSet w[2]{};
        for (int k = 0; k < 2; k++) {
            w[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[k].dstSet = s_mdset[h]; w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi[k];
        }
        vkUpdateDescriptorSets(C->device, 2, w, 0, nullptr);
    }
    if (s_shOk && s_shRp != VK_NULL_HANDLE)
        s_shMeshPipe = mesh_pipe("shaders\\dc_mesh_sh.vert.spv", VK_NULL_HANDLE, s_shRp, VK_SAMPLE_COUNT_1_BIT, 2, &s_repSpec);
    s_meshState = 1;
    rlog("[VKR] meshes from mods on the GPU: ready\n");
    return true;
}
/* готов ли путь видеокарты (у dc_meshrep.cpp: нет — прежний путь процессора) */
extern "C" int dcvk_mesh_ok(void) {
    if (!s_vmap && !dcvk_raster_init()) return 0;
    return mesh_buffers() ? 1 : 0;
}
/* 06.10.2026 (2): сколько видеопамяти можно отдать мешам из mods (все блоки вместе) */
static VkDeviceSize mesh_budget(void) {
    static VkDeviceSize b = 0;
    if (b) return b;
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(C->phys, &mp);
    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
        if ((mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[i].size > heap) heap = mp.memoryHeaps[i].size;
    const VkDeviceSize keep = (VkDeviceSize)2 << 30;
    b = heap > 2 * keep ? heap - keep : heap / 2;
    if (b < ((VkDeviceSize)256 << 20)) b = (VkDeviceSize)256 << 20;
    const char* e = getenv("DC_MESH_POOL_MB");
    if (e && atoi(e) > 0) b = (VkDeviceSize)atoi(e) << 20;
    rlog("[VKR] mesh pools: up to %llu MB of video memory (largest device heap %llu MB)\n",
         (unsigned long long)(b >> 20), (unsigned long long)(heap >> 20));
    return b;
}
static void mesh_block_free(MeshBlock& B) {
    if (B.vb) vkDestroyBuffer(C->device, B.vb, nullptr);
    if (B.ib) vkDestroyBuffer(C->device, B.ib, nullptr);
    if (B.vm) vkFreeMemory(C->device, B.vm, nullptr);
    if (B.im) vkFreeMemory(C->device, B.im, nullptr);
    B = MeshBlock();
}
/* блок, где модель поместится: уже выделенный или новый; −1 — видеопамять для мешей кончилась */
static int mesh_block_for(VkDeviceSize vb, VkDeviceSize ib) {
    for (int k = 0; k < (int)s_mblk.size(); k++) {
        const MeshBlock& B = s_mblk[(size_t)k];
        if (B.vused + vb <= B.vcap && B.iused + ib <= B.icap) return k;
    }
    const VkDeviceSize MB = (VkDeviceSize)1 << 20;
    const VkDeviceSize vx = (vb + MB - 1) & ~(MB - 1), ix = (ib + MB - 1) & ~(MB - 1);
    VkDeviceSize vc = std::max<VkDeviceSize>((VkDeviceSize)64 << 20, vx), ic = std::max<VkDeviceSize>((VkDeviceSize)32 << 20, ix);
    if (s_mTotal + vc + ic > mesh_budget()) { vc = vx; ic = ix; }                  /* под конец — блок ровно под модель */
    if (s_mTotal + vc + ic > mesh_budget()) return -1;
    MeshBlock B; B.vcap = vc; B.icap = ic;
    if (!makeBuf(vc, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 &B.vb, &B.vm, nullptr) ||
        !makeBuf(ic, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 &B.ib, &B.im, nullptr)) {
        mesh_block_free(B);
        rlog("[VKR] mesh pools: video memory refused a block of %llu MB\n", (unsigned long long)((vc + ic) >> 20));
        return -1;
    }
    s_mblk.push_back(B);
    s_mTotal += vc + ic;
    rlog("[VKR] mesh pools: block %d - %llu MB vertices, %llu MB indices (all blocks %llu MB)\n", (int)s_mblk.size() - 1,
         (unsigned long long)(vc >> 20), (unsigned long long)(ic >> 20), (unsigned long long)(s_mTotal >> 20));
    return (int)s_mblk.size() - 1;
}
/* копии из промежуточного буфера в видеопамять — один раз, с ожиданием (загрузка модели) */
static bool mesh_copy(VkBuffer st, int n, const VkBuffer* dst, const VkBufferCopy* reg) {
    if (ptvk_lost()) return false;
    ptvk_gpu_lock();
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    bool ok = vkAllocateCommandBuffers(C->device, &ai, &cb) == VK_SUCCESS;
    bool busy = false;                  /* 18.10: работа не закончена (видеокарта потеряна) - забор и буфер не трогать */
    VkFence fe = VK_NULL_HANDLE;
    if (ok) {
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &bi);
        for (int k = 0; k < n; k++) vkCmdCopyBuffer(cb, st, dst[k], 1, &reg[k]);
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vkEndCommandBuffer(cb);
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        ok = vkCreateFence(C->device, &fci, nullptr, &fe) == VK_SUCCESS;
        if (ok) {
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            const VkResult sr = vkQueueSubmit(C->queue, 1, &si, fe);
            if (sr == VK_ERROR_DEVICE_LOST) ptvk_mark_lost((int)sr);
            ok = sr == VK_SUCCESS;
            if (ok && !ptvk_wait_fence(fe)) { ok = false; busy = true; }
            if (!busy) vkDestroyFence(C->device, fe, nullptr);
        }
        if (!busy) vkFreeCommandBuffers(C->device, s_pool, 1, &cb);
    }
    ptvk_gpu_unlock();
    return ok;
}
/* модель в пул (один раз): вершины MeshVtx (48 Б) и индексы; −1 — не влезла / нет пути видеокарты.
 * 06.10.2026 (2): блок по месту (mesh_block_for); через видеопамять — частями до 64 МБ (большой модели не нужно
 * столько же памяти хоста под промежуточный буфер). */
extern "C" int dcvk_mesh_add(const void* verts, int nv, const uint32_t* idx, int ni) {
    if (!dcvk_mesh_ok() || nv <= 0 || ni <= 0) return -1;
    const VkDeviceSize vb = (VkDeviceSize)nv * sizeof(MeshVtx), ib = (VkDeviceSize)ni * 4, ibA = (ib + 15) & ~(VkDeviceSize)15;
    int blk = -1;
    VkDeviceSize vo = 0, io = 0;
    for (size_t h = 0; h < s_mholes.size(); h++) {               /* 06.10.2026: место снятой модели */
        MeshHole& H = s_mholes[h];
        if (H.vn < vb || H.in < ibA) continue;
        blk = H.blk; vo = H.vo; io = H.io;
        H.vo += vb; H.vn -= vb; H.io += ibA; H.in -= ibA;
        if (H.vn < sizeof(MeshVtx) || H.in < 16) s_mholes.erase(s_mholes.begin() + (std::ptrdiff_t)h);
        break;
    }
    const bool hole = blk >= 0;
    if (!hole) blk = mesh_block_for(vb, ibA);
    if (blk < 0) {
        rlog("[VKR] mesh pools: %d vertices, %d indices do not fit into video memory\n", nv, ni);
        return -1;
    }
    MeshBlock& B = s_mblk[(size_t)blk];
    if (!hole) { vo = B.vused; io = B.iused; }
    const VkDeviceSize CH = (VkDeviceSize)64 << 20;
    const VkDeviceSize stSize = std::min<VkDeviceSize>(CH, vb + ib);
    VkBuffer st = VK_NULL_HANDLE; VkDeviceMemory sm = VK_NULL_HANDLE; void* map = nullptr;
    if (!makeBuf(stSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 &st, &sm, &map) || !map) {
        if (st) vkDestroyBuffer(C->device, st, nullptr);
        if (sm) vkFreeMemory(C->device, sm, nullptr);
        return -1;
    }
    bool ok = true;
    if (vb + ib <= CH) {                                           /* как раньше: одна отправка */
        memcpy(map, verts, (size_t)vb);
        memcpy((uint8_t*)map + vb, idx, (size_t)ib);
        const VkBuffer dst[2] = { B.vb, B.ib };
        const VkBufferCopy reg[2] = { { 0, vo, vb }, { vb, io, ib } };
        ok = mesh_copy(st, 2, dst, reg);
    } else {
        for (VkDeviceSize o = 0; ok && o < vb; o += CH) {
            const VkDeviceSize n = std::min<VkDeviceSize>(CH, vb - o);
            memcpy(map, (const uint8_t*)verts + o, (size_t)n);
            const VkBufferCopy reg{ 0, vo + o, n };
            ok = mesh_copy(st, 1, &B.vb, &reg);
        }
        for (VkDeviceSize o = 0; ok && o < ib; o += CH) {
            const VkDeviceSize n = std::min<VkDeviceSize>(CH, ib - o);
            memcpy(map, (const uint8_t*)idx + o, (size_t)n);
            const VkBufferCopy reg{ 0, io + o, n };
            ok = mesh_copy(st, 1, &B.ib, &reg);
        }
    }
    vkDestroyBuffer(C->device, st, nullptr);
    vkFreeMemory(C->device, sm, nullptr);
    if (!ok) {
        if (hole) s_mholes.push_back({ blk, vo, vb, io, ibA });   /* место не потеряно */
        return -1;
    }
    s_mesh.push_back({ (uint32_t)(vo / sizeof(MeshVtx)), (uint32_t)(io / 4), (uint32_t)ni, blk, (uint32_t)nv });
    if (!hole) {
        B.vused += vb;
        B.iused += ibA;
    }
    return (int)s_mesh.size() - 1;
}
/* 06.10.2026 (подхват модов на лету, dc_meshrep.cpp): модель снята через несколько кадров после подмены (её экземпляров
 * в кадре уже нет) — место в блоке пула свободно для следующей */
extern "C" void dcvk_mesh_drop(int mesh) {
    if (mesh < 0 || mesh >= (int)s_mesh.size()) return;
    MeshGpu& g = s_mesh[(size_t)mesh];
    if (g.blk < 0) return;
    const VkDeviceSize ib = ((VkDeviceSize)g.icount * 4 + 15) & ~(VkDeviceSize)15;
    s_mholes.push_back({ g.blk, (VkDeviceSize)g.vbase * sizeof(MeshVtx), (VkDeviceSize)g.nv * sizeof(MeshVtx), (VkDeviceSize)g.ibase * 4, ib });
    g.blk = -1;
}
/* экземпляр в кадр: inst — 9 vec4 (dc_mesh_common.glsl; порядок и первую кость ставит растр), bones — nb × 10 vec4;
 * occ — персонаж (отбрасывает тень). Draw встаёт в список кадра там, где его позвали (в обходе списка игры). */
extern "C" int dcvk_mesh_draw(int mesh, const float* inst, const float* bones, int nb, int occ) {
    if (s_meshState <= 0 || mesh < 0 || mesh >= (int)s_mesh.size() || nb < 1) return 0;
    if (s_mInstN >= MESH_INST_MAX || s_mBoneN + nb > MESH_BONE_MAX) return 0;
    float* I = s_mInstMap + ((size_t)s_mHalf * MESH_INST_MAX + (size_t)s_mInstN) * MESH_INST_V4 * 4;
    memcpy(I, inst, (size_t)MESH_INST_V4 * 16);
    float seq;                                                   /* порядок (painter) — один на весь меш */
    if (g_dcvk_room_pass) {
        int rseq = s_room_seq;
        if (rseq < -ROOM_SEQ_BASE - 100) s_room_seq++;
        seq = (float)(ROOM_SEQ_BASE + rseq);
        if (g_dcvk_room_seq_mode == 1) seq = (float)(-100 - rseq);
        else if (g_dcvk_room_seq_mode == 2) seq = (float)ROOM_SEQ_BASE;
    } else {
        seq = (float)s_prim_seq++;
    }
    I[1 * 4 + 1] = seq;
    I[8 * 4 + 0] = (float)s_mBoneN;
    memcpy(s_mBoneMap + ((size_t)s_mHalf * MESH_BONE_MAX + (size_t)s_mBoneN) * MESH_BONE_V4 * 4, bones, (size_t)nb * MESH_BONE_V4 * 16);
    s_mBoneN += nb;
    Draw d{ s_nverts, 0, 0, 0, g_dcvk_room_pass, 0, 0, 0 };
    d.mesh = mesh; d.inst = s_mInstN; d.half = s_mHalf; d.mocc = occ ? 1 : 0;
    s_draws.push_back(d);
    s_mInstN++;
    return 1;
}
void mesh_frame_reset(void) {
    s_mHalf ^= 1;
    s_mInstN = 0;
    s_mBoneN = 0;
}
static void medx_record(VkCommandBuffer cmd, VkPipeline pipe, const Draw& d, const MeshRef* mr) {
    if (pipe == VK_NULL_HANDLE || s_meshState <= 0) return;
    MeshRef r;
    if (mr) {                                                    /* 25.10: меш снимка кадра A - s_mesh / s_mblk не читаем */
        r = *mr;
    } else {
        if (d.mesh < 0 || d.mesh >= (int)s_mesh.size()) return;
        const MeshGpu& g = s_mesh[(size_t)d.mesh];
        if (g.blk < 0 || g.blk >= (int)s_mblk.size()) return;
        const MeshBlock& B = s_mblk[(size_t)g.blk];              /* 06.10.2026 (2): блок пула этой модели */
        r.vb = B.vb; r.ib = B.ib; r.icount = g.icount; r.ibase = g.ibase; r.vbase = (int32_t)g.vbase;
    }
    if (r.vb == VK_NULL_HANDLE || r.ib == VK_NULL_HANDLE) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_mplLay, 1, 1, &s_mdset[d.half & 1], 0, nullptr);
    const VkDeviceSize z = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &r.vb, &z);
    vkCmdBindIndexBuffer(cmd, r.ib, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, r.icount, 1, r.ibase, r.vbase, (uint32_t)d.inst);
}

extern "C" int dcvk_raster_init(void) {
    ptvk_gpu_lock();
    int ok = raster_init_impl();
    ptvk_gpu_unlock();
    return ok;
}

/* ===== 18.10.2026: ВСЕ КОНВЕЙЕРЫ ДО ПЕРВОГО КАДРА - ЭКРАН «ПОДГОТОВКА ШЕЙДЕРОВ» (main.cpp dc_shader_prep) ================
 * Игрок (у двух друзей с процессорами Ryzen первый экран шёл кадр в секунду, в первой сцене на движке - долгий пролаг):
 * «сделать как в современных AAA, чтобы шейдеры прогружались в самом начале, просто отдельным экраном, ... полоской
 * прогресса, и затем уже начинать игру». Кэш 11.10 (dc_pipelines.bin) спасал только второй запуск: на первом набор MSAA
 * строился в фоне под экранами запуска (их же процессором), а объекты свечения (два больших конвейера dc_fx - их зовут и
 * гладкая луна, и тени без зерна), SSAO и дымки - посреди кадра, где впервые нужны: первая комната с луной или лампой с
 * тенью - это и есть первая сцена на движке. Теперь main.cpp на своём потоке зовёт dcvk_raster_init и следом это, а главный
 * поток рисует экран с полоской. Здесь: набор MSAA под настройки - в кэш (msaaPrewarm, рядом со свечением), объекты свечения,
 * SSAO и дымки - сразу настоящие (без целей: их по размеру кадра делает первый кадр, как раньше), конвейеры гладкой луны и
 * теней без зерна - в кэш (их образы и наборы - тоже первый кадр с ними). Кэш пишется сразу. Фаза для полоски: 0 - растр
 * (dcvk_raster_init), 1 - MSAA и свечение, 2 - лёгкие наборы, 3 - готово. */
static std::atomic<int> s_prepPhase{ 0 };
extern "C" int dcvk_prep_phase(void) { return s_prepPhase.load(); }
/* 1 - конвейеры правда собираются (кэш начат пустым или конвейер шёл 60 мс и дольше), 0 - кэш из файла, -1 - не известно */
extern "C" int dcvk_prep_cold(void) { return (s_pcCold.load() == 1 || s_vkpSlowN.load() > 0) ? 1 : s_pcCold.load(); }
extern "C" void dcvk_prepare_all(void) {
    ptvk_gpu_lock();
    LARGE_INTEGER fq, t0, t1, t2; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    const int n0 = s_vkpN;
    s_prepPhase = 1;
    s_rlogMirror = 1;                                           /* 23.10.2026: into the game's log too (rlog) */
    pp_init();
    const int want = s_ppMsaaFail ? 1 : (s_ppMsaa > s_ppMsaaMax ? s_ppMsaaMax : s_ppMsaa);   /* как msaaEnsure */
    const int ss = (s_ppSshade && s_ppSSok) ? ((s_ppSloop && !s_ppSLfail) ? 2 : 1) : 0;
    std::fprintf(stderr, "shaders: preparing - MSAA %dx (by samples %d), pipeline cache %s%c", want, ss,
                 s_pcache ? (s_pcCold.load() == 1 ? "empty" : "from the file") : "off", 10);
    std::thread mt;
    if (want > 1 && s_pcache && s_msPrewarm.load() == 0) {      /* первый кадр с MSAA возьмёт набор из кэша (msaaEnsure) */
        s_msPrewarm = 1;
        const int feat = raster_feat_goal();
        mt = std::thread([want, ss, feat] { msaaPrewarm(want, ss, feat); });
    }
    const int fxok = fxObjects();
    if (mt.joinable()) mt.join();
    QueryPerformanceCounter(&t1);
    s_prepPhase = 2;
    const int pok = postObjects();
    const int vok = pok ? volObjects() : 0;
    const int aok = (pok && s_ppAA) ? aaObjects(s_ppAA) : 0;   /* 25.10.2026: сглаживание выбранного режима */
    int lok = 0;
    if (fxok) {                                                 /* гладкая луна (mpfEnsure) и тени без зерна (spfEnsure) */
        VkShaderModule pvs = loadSpv("dc_post.vert.spv");
        if (pvs) {
            auto warm = [&](const char* fs, VkRenderPass rp) {
                VkPipeline p = VK_NULL_HANDLE;
                if (fx_mkPost(pvs, fs, 0, rp, &p)) { vkDestroyPipeline(C->device, p, nullptr); lok++; }
            };
            warm("shaders\\dc_moon_pf.frag.spv", s_rpGB);
            warm("shaders\\dc_moon_pack.frag.spv", s_rpGB);
            VkRenderPass rp = VK_NULL_HANDLE;                   /* как s_rpSPF */
            if (pp_mkPass(VK_FORMAT_R16G16_SFLOAT, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &rp)) {
                warm("shaders\\dc_shpf_down.frag.spv", rp);
                warm("shaders\\dc_shpf_pyr.frag.spv", rp);
                vkDestroyRenderPass(C->device, rp, nullptr);
            }
            vkDestroyShaderModule(C->device, pvs, nullptr);
        }
    }
    QueryPerformanceCounter(&t2);
    ptvk_gpu_unlock();
    if (s_pcNew) { s_pcNew = 0; pc_write_now(); }               /* не ждать первых кадров: кэш на диске сразу */
    std::fprintf(stderr, "shaders: prepared - MSAA %dx%s, glow and reflections %s, SSAO %s, haze %s, AA filter %s, moon and shadows %d of 4;"
                         " %d pipelines, %.0f ms (MSAA and glow %.0f ms)%c",
                 want, (want > 1 && s_msPrewarm.load() == 2) ? " into the cache" : (want > 1 ? " (no cache - at its first frame)" : " (off)"),
                 fxok ? "ok" : "FAILED", pok ? "ok" : "FAILED", vok ? "ok" : "FAILED",
                 !s_ppAA ? "off" : aok ? PP_AA_NAME[s_ppAA] : "FAILED", lok, s_vkpN - n0,
                 (double)(t2.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart,
                 (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, 10);
    s_rlogMirror = 0;
    s_prepPhase = 3;
}

extern "C" void sw_uipage_reset(void);   /* gpu_sw_renderer.c: страницы интерфейса живут один кадр */
extern "C" int  dc_sqdbg_left(void);     /* сколько строк лога классификации осталось */

/* ⭐ ТЕКСТ ПОВЕРХ ЖИВОГО МИРА = ЭКРАН ПАУЗЫ (09.09.2026).
 * Игрок: «на экране паузы мы можем крутить камерой». Своего флага у паузы
 * нет — пословное сравнение 12 КБ глобалей в двух циклах «игра ↔ пауза» не
 * дало НИ ОДНОГО устойчиво различающего слова: игра просто крутит опрос
 * пада, ничего не записывая. Зато рендер знает точно: единственный текст,
 * который ложится поверх трёхмерного кадра, — это надпись PAUSE (меню,
 * инвентарь и карта рисуются без 3D и идут другим путём, пилларбоксом).
 * Считаем такие спрайты за кадр; счёт прошлого кадра и есть признак. */
static int s_fontOverWorld = 0, s_fontOverWorldPrev = 0;
extern "C" int dc_text_over_world(void) { return s_fontOverWorldPrev > 0; }
/* ⭐ КОПИЯ КАДРА ИЗ VRAM = ПРИЗНАК ПАУЗЫ (12.09.2026, игрок: «в ингейм UI текст
 * не сжался, а картинки сжались — вразнобой»). Правило 09.09 «текст поверх
 * мира не сжимать» ловило ВЕСЬ текст игры в мире — подписи предметов, окна
 * сообщений, подсказки у дверей — а плашки под ним сжимались: буквы вылезали
 * за рамку. Двоился же текст только на паузе, и только потому, что игра там
 * подкладывает под него КОПИЮ КАДРА — полноэкранные текстурные квады 15 bpp
 * со второй половины VRAM (tp 258/272/274, замер sqdbg 09.09), в которой тот же
 * текст уже нарисован несжатым. Значит и правило должно жить только в кадре,
 * где такая копия ЕСТЬ: увидели двумерный текстурный квад 15 bpp на полэкрана
 * и больше — с этого момента текст не сжимаем (и он же считается признаком
 * паузы для dc_text_over_world). В обычном кадре мира копии нет — текст
 * сжимается вместе с плашками, как до 09.09. DC_FONT_NOSQUEEZE_ANY=1 — правило
 * 09.09 без гейта. */
static int s_frameCopy = 0;
/* ⭐ 10.10.2026 - РОВНОЕ ЗАТЕМНЕНИЕ ПАУЗЫ (игрок: «в нашем режиме на паузе тёмный фон накладывается неравномерно и постоянно
 * расплывается. Убери этот фон или сделай его на весь экран сразу, без расплываний и анимаций»). Снимки паузы: игра каждый кадр
 * кладёт полупрозрачную КОПИЮ КАДРА из видеопамяти (квады 15 bpp на полэкрана и больше, см. s_frameCopy), а там у нас кадр
 * софт-растра 1x, не наш: тёмные полосы у надписи PAUSE и Регины гуляют по экрану, и копия копии расплывается. Теперь в
 * мире (экран 3) такие квады не рисуются, а вместо них - одна чёрная плита B/2 + F/2 на весь кадр, сразу, между миром и
 * надписями (место в порядке вывода - после последнего draw с глубиной, см. dcvk_render; пробовавшаяся 09.09 подмена плит
 * игры зависела от порядка потока и мигала). DC_PAUSE_EVEN=0 - как было. */
static int s_pauseDim = 0;
static int dc_pause_even_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_PAUSE_EVEN"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* 27.09 вечер: буквы кадра сжаты к центральному 4:3 или нет — мышь окна выбора (main.cpp) считает, где варианты на экране */
static int s_fontSqN = 0, s_fontFlatN = 0, s_fontSqPrev = -1;
extern "C" int dcvk_font_squeezed_last(void) { return s_fontSqPrev; }   /* 1 — сжаты, 0 — нет, -1 — букв не было */

/* Был ли в этом кадре хоть один примитив с глубиной, то есть нарисован ли уже
 * мир. Нужен пелене затемнения: та её половина, что идёт ДО мира, всё равно
 * будет закрашена, а поверх мира ложится только вторая. */
static int s_worldDrawn = 0;

extern "C" void dc_actor_in_flip(void);   /* gpu_sw_renderer.c: счётчик пришедших примитивов персонажа */

extern "C" void dcvk_frame_reset(void) {
    sw_uipage_reset();
    dc_actor_in_flip();
    s_alost_oob_p = s_alost_oob; s_alost_behind_p = s_alost_behind;
    s_alost_short_p = s_alost_short; s_alost_full_p = s_alost_full;
    s_alost_oob = s_alost_behind = s_alost_short = s_alost_full = 0;
    s_actor_out_all_p = s_actor_out_all;
    actor_med_update(s_actor_out_all);   /* медиана считается по ПОКАЗАННЫМ кадрам */
    s_actor_out_all = 0;
    s_fontOverWorldPrev = s_fontOverWorld;   /* признак паузы: см. dc_text_over_world */
    s_fontOverWorld = 0;
    s_fontSqN = s_fontFlatN = 0;             /* 27.09: см. dcvk_font_squeezed_last (итог — в dcvk_render после решения кадра) */
    s_frameCopy = 0;
    s_pauseDim = 0;
    s_worldDrawn = 0;
    s_nverts = 0;
    s_occCpu.clear();                                 /* 09.10: заслонившие кадра (рамка на картах теней) */
    s_draws.clear();
    s_semiLate.clear();
    s_sqzCands.clear();
    s_m43Cands.clear(); s_monTiles.clear();   /* 21.09 ночь: загадка на мониторе (M43Cand) */
    s_scaps.clear(); s_scapNow = 0; s_scapTris = 0;   /* 21.09 вечер: захваты сцены живут один кадр */
    s_scapStream = 0;                                 /* 01.10 вечер: номер ячейки в потоке (DC_SCENE_CAPTURE_SMOOTHT) */
    s_prim_frame_seq++;                               /* 30.09: номер кадра растра */
    s_streamMinY = 1e9f; s_streamMaxY = -1e9f;
    s_vertsLow = s_vertsHigh = 0;
    s_submits = 0; s_submitDrawY = -1;
    s_submitMarkVerts = s_submitMarkDraws = 0;
    s_submitMarkLow = s_submitMarkHigh = 0;
    s_markCount = 0;
    s_actorVerts = 0;
    s_prim_seq = 0;
    s_room_seq = 0;
    mesh_frame_reset();   /* 06.10.2026: экземпляры мешей — в другую половину буферов (10.10: без объявления в блоке - оно
                           * внутри extern "C" давало другую языковую связь, Clang это отвергает; функция определена выше) */
}

/* ===== THE NEAR PLANE, SOLVED HERE ========================================
 * The PS1 has no near clipping. A polygon crossing the camera plane gets a
 * vertex whose depth saturates the perspective divider; the GTE raises the
 * divide-overflow flag - psx-spx defines that flag as "the vertex exceeds the
 * near clip plane" - and the game's own polygon loops read the flag summary
 * and drop the whole polygon. With the original fixed cameras nothing ever
 * came that close. With a free camera it happens constantly, and it reads as
 * geometry tearing open along the edges of the frame as the camera swings.
 *
 * Fixing it in the game's loops means patching eight separate routines and
 * still missing whatever else submits geometry. Here there is one funnel that
 * every polygon of every submitter passes through, and none of the console's
 * limits apply: float coordinates, a real z-buffer, per-vertex w from PGXP.
 *
 * So the engine is told to stop discarding (gte.cpp clears the reject bits
 * while the free camera is live, and publishes the TRUE signed depth even for
 * vertices the hardware clamps to zero), and the triangle is clipped properly
 * right here. The projection is exactly invertible, so view space comes back
 * from (x, y, w) without any extra plumbing:
 *      x_view = (x_screen - offset - OFX) * w / (H * squeeze)
 * clip against w >= NEAR, project the pieces back, draw one or two triangles.
 * A polygon is never dropped for crossing the plane - only the part actually
 * behind the camera goes away, which is the part nobody can see. */

extern "C" {
    extern float g_pt_proj_h, g_pt_proj_ofx, g_pt_proj_ofy;
    void gpu_get_draw_offset(int *ox, int *oy);
}
static void dcvk_push_tri_raw(const float* v, int semi_mode, int ztest);

#define NC_NEAR 2.0f     /* view units; our raster has no divider to saturate */

struct NCv { float x, y, z, u, vv, r, g, b; uint32_t tag; };   /* x,y = VIEW space; z = depth; 26.10: метка куска */

/* 26.10.2026: метки кусков следующего треугольника (gpu_sw_renderer.c - вокруг dcvk_push_tri; пишет dcvk_push_tri_raw) */
static uint32_t s_pairTri[3] = { 0u, 0u, 0u };
extern "C" void dcvk_pair_next(const unsigned* pq) { for (int i = 0; i < 3; i++) s_pairTri[i] = pq ? (uint32_t)pq[i] : 0u; }

static void nc_mix(NCv& o, const NCv& a, const NCv& b, float t) {
    o.x = a.x + (b.x - a.x) * t;   o.y  = a.y  + (b.y  - a.y)  * t;
    o.z = a.z + (b.z - a.z) * t;   o.u  = a.u  + (b.u  - a.u)  * t;
    o.vv= a.vv+ (b.vv- a.vv)* t;   o.r  = a.r  + (b.r  - a.r)  * t;
    o.g = a.g + (b.g - a.g) * t;   o.b  = a.b  + (b.b  - a.b)  * t;
    o.tag = a.tag;                 /* точка на ребре - метка той вершины, что перед камерой (вызывающий) */
}

/* ---- INTERFACE SQUEEZE (2026-09-02) ---------------------------------------
 * Under DC_WIDESCREEN the GTE projects the world with X scaled by 3/4 and the
 * presentation stretches the frame to 16:9, so 3D comes out right and
 * everything drawn in plain screen space - menu, inventory, text, dialog
 * boxes - comes out 4/3 too wide. The first cure (2026-09-01: keep such
 * screens as a 4:3 frame with side bars, DC_UI_43) was rolled back at the
 * player's request ("пусть опять будет всё в 16 на 9"): the frame is always
 * wide now and the interface itself is squeezed instead, primitive by
 * primitive, by 3/4 about the centre of the display window. Measured on the
 * ITEM / EQUIP / MAP / MIX screens and the OPTION overlay: every such frame
 * opens with a full-screen black fill, then a backdrop picture in two tiles,
 * panels, tabs and 15x15 glyphs - all of them 2D with w = 1, none of them
 * see-through.
 * Only primitives the mirror marked 2D by construction qualify
 * (g_dcvk_ui2d, gpu_sw_renderer.c). A full-screen fill or backdrop (>= 94%
 * of the window both ways) stays put, so the black behind a menu still
 * covers the whole canvas and a full-screen title image is not left with
 * its stretched twin peeking out from the background pass. Anything outside
 * the display column (texture uploads, the font atlas at x = 768) is not
 * touched either. DC_UI_SQUEEZE=0 turns it off (interface stretched, as
 * before 2026-09-01). */
extern "C" int g_dcvk_ui2d;
extern "C" int g_dcvk_gte_line;             /* 01.10 (k): линия между точками GTE (gpu_sw_renderer.c dcvk_mirror_line) */
extern "C" uint8_t* memory_get_ram_ptr(void);
extern int g_dc_aiming;                     /* 20.09: main.cpp (C++ linkage), aim held (mouse / trigger) */
extern "C" int g_dc_orig_mode;              /* memory.c: родной режим (клавиша O) */
/* 15.09.2026: соотношение сторон из меню SCREEN (memory.c) — мир ужат под 16:9 (режимы 1, 2) и
 * интерфейс держит 4:3 на широком мире (режим 1); спрашивать каждый раз — меняется на лету */
extern "C" int dc_ws_world(void);
extern "C" float dc_ws_sq(void);   /* 10.10: множитель X мира - любая форма картинки (memory.c; было 0.75 / 1) */
extern "C" int dc_ui43_squeeze(void);
extern "C" int dc_game_loaded(void);   /* gpu.c: 0 while the BIOS runs (logo, licence) */
/* ПЛОСКИЙ ЭКРАН (2026-09-02, «начальные заставки, экраны сделай 4:3
 * визуально»): титул, PUSH START, логотип Capcom, инвентарь — кадры без
 * единого примитива с глубиной. На них сжимается ВСЁ, включая полноширинные
 * картинки и заливки, а бока закрываются чёрными полосами (dcvk_render).
 * Иначе фоновая картинка титула (VRAM-блит) оставалась растянутой, а текст
 * поверх неё — сжатым, и надпись показывалась ДВАЖДЫ: блит берёт конечное
 * содержимое VRAM, где та же надпись уже нарисована игрой несжатой. Когда
 * сжато всё одинаково, копия в блите ложится точно под наши сжатые знаки.
 * Признак «в прошлом кадре не было 3D» + байт экрана игры не 3/8 (мир/OPTION
 * поверх мира): на выходе из меню байт становится 3 раньше первого кадра
 * мира, так что заливка мира сжатой не уходит. */
static int s_prev_no3d = 0;
/* 14.09.2026 (игрок: «в брифинге есть часть, где осталось двойное наложение
 * сжатого и несжатого 4:3»): страницы досье с 3D-бюстом (dump_prims page_12:
 * tp 139 с глубиной + две полуэкранные КОПИИ КАДРА tp 256/258 15 bpp semi +
 * текст tp 37..40). Кадр не плоский (есть 3D), интерфейс сжимался, а копия
 * кадра несёт тот же интерфейс несжатым — две копии. Копия кадра вне мира
 * (байт экрана не 3/8) = экран интерфейса, показываем его целиком 4:3 как
 * соседние плоские страницы. DC_COPY_FLAT=0 — прежнее. */
static int s_prev_copyflat = 0;
static int dc_copyflat_now(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_COPY_FLAT"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !s_frameCopy) return 0;
    uint8_t* ram = memory_get_ram_ptr();
    int scr = ram ? ram[0xB7878] : 3;
    return (scr != 3 && scr != 8) ? 1 : 0;
}
static int dc_flat_screen_hint(void) {
    /* 21.09: DC_SQZ_FORCE_HINT=0/1 — проверка отложенного сжатия: догадка заведомо неверна на каждом кадре, картинка
     * обязана выйти той же (решает dcvk_render); только для замера */
    { static int fh = -2;
      if (fh == -2) { const char* e = ::getenv("DC_SQZ_FORCE_HINT"); fh = e ? atoi(e) : -1; }
      if (fh >= 0) return fh ? 1 : 0; }
    if (s_prev_copyflat) return 1;     /* 14.09: копия кадра вне мира — как плоский */
    /* Content only. The game's screen byte was tried as a second witness and
     * dropped the same day: the TITLE MENU (NEW GAME / LOAD GAME / OPTION)
     * runs with byte 3 - the "world" value - and stayed stretched with its
     * text doubled. A frame without a single depth-tested primitive is flat,
     * whatever the byte says; the one-frame cost on leaving a menu (the
     * world's opening fill squeezed once, sides showing the base image of
     * the same world) is invisible. */
    return s_prev_no3d;
}
/* ПЛОСКИЙ КАДР — ПИЛЛАРБОКС ЦЕЛИКОМ (2026-09-02, вечер; игрок: «начальный
 * ролик наложен два раза друг на друга, растянутый и сжатый»). Сжатие по
 * примитивам на плоском экране опиралось на классификацию «2D по
 * построению» (g_dcvk_ui2d): полупрозрачные текстурированные квады из неё
 * исключены (в мире это блики ламп, вспышки), а именно ими начальная сцена
 * с терминалом рисует свою картинку — она оставалась растянутой, пока текст
 * и фон VRAM сжимались. Любая такая классификация даёт две копии, как только
 * один класс примитивов промахивается. Поэтому плоский кадр (без единого
 * примитива с глубиной) растеризатор теперь НЕ сжимает и не закрывает
 * полосами вовсе: он рисует его как есть, а вывод (dc_frame_rect, main.cpp)
 * показывает всё изображение целиком в прямоугольнике 4:3 — один источник,
 * вторых копий быть не может. В мире (есть 3D) правила прежние: сжимаются
 * только плашки/текст интерфейса. DC_FLAT_PILLARBOX=0 — прежний путь
 * (сжатие каждого примитива + фон + полосы). */
static int dc_flat_pillarbox_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_FLAT_PILLARBOX"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static int dc_ui_squeeze_tri(const float* v, float* out, int* would = nullptr) {
    static int en = -1;
    if (en < 0) {
        const char* e = ::getenv("DC_UI_SQUEEZE");
        en = (e && *e == '0') ? 0 : 1;
    }
    if (!en || !g_dcvk_ui2d || !dc_ui43_squeeze()) return 0;   /* 15.09: только режим 1 соотношения */
    /* ⭐ ТЕКСТ ПОВЕРХ МИРА НЕ СЖИМАЕМ (09.09.2026, «на паузе текст двоится»).
     *
     * Разбор командой порта sqdbg на экране паузы: буквы PAUSE — пять
     * спрайтов со страницы шрифта (tp=44, x 136..174), они шли как интерфейс
     * и сжимались. А рядом игра выводит VRAM-копию кадра (полноэкранные
     * блиты tp=258/272/274), в которой тот же текст УЖЕ нарисован — софт-
     * растром, то есть несжатым. Две копии одного слова, расходящиеся к
     * краям: сжатая поверх несжатой.
     *
     * Сжать заодно и копию нельзя: она несёт весь кадр, включая мир, и
     * ужала бы картинку до 4:3. Значит не сжимать надо текст — тогда обе
     * копии совпадают. Экраны, где текст ДОЛЖЕН держать пропорции (меню,
     * инвентарь, карта), рисуются без 3D и сюда не доходят: их показывает
     * пилларбокс целым кадром (dc_flat_pillarbox_on).
     * DC_FONT_NOSQUEEZE=0 — прежнее поведение. */
    {
        static int fs = -1, any = -1;
        if (fs < 0) { const char* e = ::getenv("DC_FONT_NOSQUEEZE"); fs = (e && *e == '0') ? 0 : 1;
                      e = ::getenv("DC_FONT_NOSQUEEZE_ANY"); any = (e && *e == '1') ? 1 : 0; }
        int tp = (int)(v[8] + 0.5f), fl = (int)(v[11] + 0.5f);
        /* копия кадра (s_frameCopy) ловится в dcvk_push_tri: её квады
         * полупрозрачные и сюда как «интерфейс» не доходят */
        if (fs && (any || s_frameCopy)) {
            if ((fl & 1) && (tp & 0xF) == 12 && ((tp >> 4) & 1) == 0
                && ((tp >> 7) & 3) == 0) { s_fontOverWorld++; return 0; }
        }
    }
    /* BIOS (PlayStation logo, licence screen): the FRAME is shown 4:3 by
     * dc_frame_rect (main.cpp), so nothing here may squeeze - the two would
     * stack, and the logo's depth-tested polygons flipping the flat-screen
     * hint from frame to frame made the picture jump between squeezed and
     * not (player 2026-09-02: "первые два экрана переключаются на сжатые /
     * разжатые, на экране PlayStation двойной текст"). */
    if (!dc_game_loaded()) return 0;
    GpuDisplayInfo di; gpu_get_display_info(&di);
    if (di.width < 64 || di.height < 64) return 0;
    const float dx0 = (float)di.display_x, dw = (float)di.width, dh = (float)di.height;
    float x0 = v[0], x1 = v[0], y0 = v[1], y1 = v[1];
    for (int i = 1; i < 3; i++) {
        float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    if (x1 <= dx0 || x0 >= dx0 + dw) return 0;                     /* off the display column */
    int flat = dc_flat_screen_hint();
    /* плоский кадр показывается 4:3 целиком выводом — здесь ничего не сжимать.
     * 21.09: с would — досчитать правила неплоского кадра: догадка «плоский» берётся по ПРОШЛОМУ кадру и на первом
     * кадре после смены экрана неверна; решает dcvk_sqz_resolve при записи кадра */
    int denied_flat = 0;
    if (flat && dc_flat_pillarbox_on()) { if (!would) return 0; denied_flat = 1; flat = 0; }
    /* In the WORLD a primitive spanning the whole display width is a
     * screen-wide band, not a piece of the interface: the frame's black
     * clear, a fade, the letterbox bars of a cutscene. Squeezed, such a band
     * would uncover the sides of the wide canvas, so it keeps its width. On a
     * FLAT screen (title, menus, inventory - no 3D at all) everything is
     * squeezed alike and dcvk_render paints black bars over the sides. */
    if (x1 - x0 >= 0.98f * dw && !flat) return 0;
    /* In the WORLD, on top of the mirror's "2D by construction" mark, the
     * triangle must be half of an axis-aligned rectangle (two distinct x,
     * two distinct y): a glyph, a plate, a sprite. Player 2026-09-02: "у
     * героев пропадают полигоны, у руки с оружием Регины всегда нет пары
     * полигонов" - a character polygon whose vertices carry no PGXP record
     * (GTE origin lost, no depth) passed the mark, was squeezed toward the
     * centre and left a hole in the mesh. Flat screens keep the loose rule:
     * the MAP draws its room outlines as arbitrary flat triangles. */
    if (!flat) {
        const float ax = v[0], bx = v[VERT_FLOATS], cx0 = v[2 * VERT_FLOATS];
        const float ay = v[1], by = v[VERT_FLOATS + 1], cy0 = v[2 * VERT_FLOATS + 1];
        int nx = (ax != bx) + (ax != cx0) + (bx != cx0);
        int ny = (ay != by) + (ay != cy0) + (by != cy0);
        if (nx != 2 || ny != 2) return 0;
    }
    (void)dh;
    const float cx = dx0 + dw * 0.5f;
    memcpy(out, v, sizeof(float) * 3 * VERT_FLOATS);
    for (int i = 0; i < 3; i++) out[i * VERT_FLOATS] = cx + (v[i * VERT_FLOATS] - cx) * dc_ws_sq();   /* 10.10: 16:9 - 0.75, 21:9 - 0.5625 ... */
    if (denied_flat) { *would = 1; return 0; }
    { static int st = -1; static long n = 0;
      if (st < 0) { const char* e = ::getenv("DC_UI_SQUEEZE_LOG"); st = (e && *e == '1') ? 1 : 0; }
      if (st && (++n % 500) == 1)
          std::fprintf(stderr, "uisqueeze: #%ld disp=%u+%u x %u  bbox %.0f..%.0f -> %.1f..%.1f%c",
                       n, di.display_x, di.width, di.height, x0, x1,
                       cx + (x0 - cx) * dc_ws_sq(), cx + (x1 - cx) * dc_ws_sq(), 10); }
    return 1;
}

static int dc_scap_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static int dc_scap_smooth(void) {        /* гладкий купол вместо огранки из четырёх квадов */
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_SMOOTH"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* ⭐ 01.10.2026 (игрок, слот 6: «видны границы квадов», «мигает»). Ячейка — 2D-draw, в маске SSAO (альфа, aoMaskOut) она
 * писала 0: затенение углов под ячейкой пропадало квадратом с резкой кромкой, а ячейки всплывают — квадраты без затенения
 * ползут по стенам и полу. Под ячейкой — картинка мира, снятая ДО прохода SSAO: пусть SSAO ложится на неё, как на мир вокруг
 * (у кромки купола сдвиг нулевой — затенение совпадает). DC_SCENE_CAPTURE_AO=0 — как было. */
static int dc_scap_ao(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_AO"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static float dc_scap_k(void) {           /* множитель X ячейки: мир 16:9 ужат GTE на 3/4 */
    static int sq = -1;
    if (sq < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_SQ"); sq = (e && *e == '0') ? 0 : 1; }
    return sq ? dc_ws_sq() : 1.0f;   /* 10.10: любая форма картинки */
}
/* Зовёт зеркало софт-растра на каждой копии VRAM->VRAM (sw_copy_rect): src/dst в координатах VRAM, a* — область рисования */
extern "C" void dcvk_scene_capture(int sx, int sy, int dx, int dy, int w, int h, int ax1, int ay1, int ax2, int ay2) {
    if (!dcvk_enabled() || !dc_scap_on()) return;
    if (w < 8 || h < 8 || w > 128 || h > 128 || s_scaps.size() >= 120) return;   /* 01.10 вечер: 64 -> 120 (у гладкой ячейки две записи) */
    const int sid = s_scapStream++;                   /* 01.10 вечер: номер ячейки — и у отвергнутых (ниже края кадра) */
    /* ⭐ 30.09 (игрок, слот 6, «третья энергия» в коридоре ST60B: «снизу выплывают необработанные чёрные квады»). Кольцо GP0:
     * игра берёт куски 64x64 и у самого края кадра — (150,504), (90,500)… при низе буфера 495: кусок наполовину за буфером.
     * Такой захват отвергался целиком, и 4 квада его ячейки рисовались из видеопамяти 1x (там комнаты нет) — чёрный квадрат,
     * который «выплывает» снизу вместе с движением ячеек. Теперь достаточно, чтобы кусок ЗАДЕВАЛ буфер рисования: ячейка
     * рисуется из нашего кадра, часть за краем кадра всё равно за экраном. DC_SCENE_CAPTURE_EDGE=0 — как было (целиком внутри). */
    { static int edge = -1; if (edge < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_EDGE"); edge = (e && *e == '0') ? 0 : 1; }
      if (edge) { if (sx + w - 1 < ax1 || sy + h - 1 < ay1 || sx > ax2 || sy > ay2) return; }
      else if (sx < ax1 || sy < ay1 || sx + w - 1 > ax2 || sy + h - 1 > ay2) return; }      /* источник — не буфер рисования */
    auto hits = [&](int rx, int ry, int rw, int rh) {
        return dx < rx + rw && dx + w > rx && dy < ry + rh && dy + h > ry; };
    if (hits(ax1, ay1, ax2 - ax1 + 1, ay2 - ay1 + 1)) return;                          /* цель — в самом буфере рисования */
    GpuDisplayInfo di; gpu_get_display_info(&di);
    if (hits(di.display_x, di.display_y, (int)di.width, (int)di.height)) return;       /* ...или на экране */
    { Scap ns{}; ns.sx = sx; ns.sy = sy; ns.dx = dx; ns.dy = dy; ns.w = w; ns.h = h;
      ns.fx = (float)sx; ns.fy = (float)sy; ns.ax = ax1; ns.ay = ay1; ns.sid = sid;
      s_scaps.push_back(ns); }
    s_scapSeenSeq = s_prim_frame_seq;                 /* 30.09: эффект идёт */
    { static int said = 0;
      if (said < 6) { said++;
          std::fprintf(stderr, "raster: scene capture #%d: frame piece %d,%d %dx%d -> texture %d,%d (drawn back from OUR frame)%c",
                       (int)s_scaps.size(), sx, sy, w, h, dx, dy, 10); } }
}
/* ⭐ 01.10.2026 вечер (игрок, слот 7 — тот же коридор ST60B, комната 84: «шейдер сейчас вызывает дрыжки, дерганья. Надо,
 * чтобы этого не было, были только плавные искажения пространства»). Замер кольца GP0 (s7_seq_an.py, 167 кадров игры): 16
 * ячеек в постоянном порядке; каждая ходит ровно на 1 точку PS1 за кадр игры (30 Гц) треугольной волной с остановкой на кадр
 * у краёв, а сдвиг центра D (до 16 точек = 72 пикселя 1080p) идёт синусом ЦЕЛЫМИ шагами — 0, 0, 1, 0, 1, 1… Каждый такой шаг
 * перекидывает картинку под куполом на 4.5 пикселя вывода, и шаги неравномерные — вот «дерганья». Теперь у ячейки есть след
 * (номер копии в потоке кадра + место в координатах кадра): место и D сглаживаются двойным скользящим средним по 4 кадрам
 * игры (неровность D 0.139 -> 0.010 точки на шаг), а на 60 Гц кадр A показывает середину между прошлым и текущим сглаженным
 * состоянием, кадр B — текущее (у ячейки два квада: «только A» и «только B»; кадр B рисуется и при неподвижной камере, если
 * в снимке такие ячейки есть). След рвётся, если ячейки не было дольше 4 кадров, сменился размер или место прыгнуло больше
 * 8 точек (тогда — как пришло). DC_SCENE_CAPTURE_SMOOTHT=0 — как было; _N — длина окна (1..8, умолч. 4). */
static int dc_scap_smootht(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_SMOOTHT"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* ⭐ 01.10.2026 вечер, второй заход (игрок, слот 7: «всё ещё есть дрыжки — резкие смены искажений»). Сглаживание движения
 * было не главным. Покадровый поиск рывков (s7_pops.py: блоки 16x16, разница соседних показов) нашёл всплески ровно на
 * Регине, а сверка с кадром без ячеек показала: в «скачущих» кадрах Регина почти НЕ искажена, в соседних — искажена.
 * Снимок порядка вывода (DC_ORDER_DBG): у части ячеек draw с ui=0 — они встают в группу «плоское ДО мира», сразу после нашей
 * комнаты, и Регина рисуется ПОВЕРХ них; остальные (ui=1) — в верхней группе, после мира. Признак ui — «2D по построению»
 * (g_dcvk_ui2d) от зеркала софт-растра, а ячейка иногда приходит «с ложной глубиной» (вершины случайно совпали с записью
 * PGXP, строка «arrived with a (false) depth») — какие ячейки такие, меняется каждые 1–3 кадра игры, и Регина под ними то
 * искажалась, то нет. У консоли ячейки — последние команды кадра (после мира и свечения): искажается всё, что нарисовано.
 * Теперь квад ячейки всегда идёт как 2D-интерфейс (g_dcvk_ui2d = 1 на время записи) — один слой, после мира, в каждом кадре.
 * DC_SCENE_CAPTURE_UITOP=0 — как было. */
static int dc_scap_uitop(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_UITOP"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static int dc_scap_smootht_n(void) {
    static int n = -1;
    if (n < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_SMOOTHT_N"); n = e ? atoi(e) : 4; if (n < 1) n = 1; if (n > 8) n = 8; }
    return n;
}
struct ScapTrack { uint64_t seq = 0; int w = 0, h = 0;
                   int nr = 0; float raw[8][4] = {};   /* последние места/сдвиги как пришли (x, y кадра, Dx, Dy) */
                   int nm = 0; float ma[8][4] = {};    /* ...их скользящие средние */
                   int hasP = 0; float Sp[4] = {}; };  /* сглаженное состояние прошлого кадра игры */
static ScapTrack s_scapTrk[64];
/* v: x, y в координатах кадра, Dx, Dy. Выход A (кадр A: середина прошлого и текущего) и B (кадр B: текущее). */
static void scap_track(int sid, int w, int h, const float v[4], float A[4], float B[4]) {
    for (int i = 0; i < 4; i++) A[i] = B[i] = v[i];
    if (sid < 0 || sid >= 64) return;
    ScapTrack& t = s_scapTrk[sid];
    const int N = dc_scap_smootht_n();
    const int same = (t.seq == s_prim_frame_seq);
    int cont = t.seq && s_prim_frame_seq >= t.seq && s_prim_frame_seq - t.seq <= 4 && t.w == w && t.h == h && t.nr > 0 &&
               fabsf(v[0] - t.raw[t.nr - 1][0]) <= 8.f && fabsf(v[1] - t.raw[t.nr - 1][1]) <= 8.f;
    if (!cont) { t.nr = 0; t.nm = 0; t.hasP = 0; }
    t.seq = s_prim_frame_seq; t.w = w; t.h = h;
    auto push = [&](float (*ring)[4], int& n, const float* x) {
        if (cont && same && n > 0) { for (int i = 0; i < 4; i++) ring[n - 1][i] = x[i]; return; }   /* второй раз в том же кадре */
        if (n >= N) { memmove(ring[0], ring[1], sizeof(float) * 4 * (size_t)(N - 1)); n = N - 1; }
        for (int i = 0; i < 4; i++) ring[n][i] = x[i];
        n++;
    };
    push(t.raw, t.nr, v);
    float m[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < t.nr; k++) for (int i = 0; i < 4; i++) m[i] += t.raw[k][i];
    for (int i = 0; i < 4; i++) m[i] /= (float)t.nr;
    push(t.ma, t.nm, m);
    float S[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < t.nm; k++) for (int i = 0; i < 4; i++) S[i] += t.ma[k][i];
    for (int i = 0; i < 4; i++) S[i] /= (float)t.nm;
    for (int i = 0; i < 4; i++) { B[i] = S[i]; A[i] = t.hasP ? 0.5f * (t.Sp[i] + S[i]) : S[i]; }
    if (!(cont && same)) { for (int i = 0; i < 4; i++) t.Sp[i] = S[i]; t.hasP = 1; }
}
/* push-константы 32..64 для ячейки захвата: prevRect подобран так, что q шейдера = место текселя в кадре (0..1) */
/* DC_SCENE_CAPTURE_LOG=1 — строка на каждую нарисованную ячейку (отладка «дрыжек»): кадр A/B, номер, sid, место, сдвиг */
static int dc_scap_log(void) {
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_LOG"); on = (e && *e == '1') ? 1 : 0; }
    return on;
}
static int dc_scap_dbg(void) {             /* DC_SCENE_CAPTURE_DBG=1: кадр без ячеек (сверка) — в кадре A и B */
    static int on = -1;
    if (on < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_DBG"); on = (e && *e == '1') ? 1 : 0; }
    return on;
}
static unsigned long long s_scapLogN = 0;
static void scap_log(char ab, const Scap& c) {
    if (!dc_scap_log()) return;
    std::fprintf(stderr, "scaplog: %c n=%llu sid=%d ab=%d raw=%d,%d f=%.2f,%.2f D=%.2f,%.2f rawD=%.0f,%.0f%c",
                 ab, s_scapLogN, c.sid, c.ab, c.sx - c.ax, c.sy - c.ay, c.fx - (float)c.ax, c.fy - (float)c.ay, c.uDx, c.uDy, c.Dx, c.Dy, 10);
}
static void scap_pcp(const Scap& c, int w, int h, int dispX, int dispY, int dispW, int dispH, float* o) {
    const float k = dc_scap_k();
    /* зеркало кладёт вершину в центр пикселя (x + 0.5): тексель u лежит в кадре на sx + u + 0.5 — иначе содержимое ячейки
     * сдвинуто на полпикселя (3 точки кадра 1080p), в перекрытиях сдвиг копится, и кромка ячейки читается ступенькой */
    const float cxp = c.fx + (float)c.w * 0.5f + 0.5f;                /* 01.10 вечер: fx, fy — место показа (= sx, sy без сглаживания) */
    const float tx0 = (c.mode == 1) ? (float)c.bx : (float)c.dx;      /* угол ячейки в текселях, как его видит шейдер (страница + uv) */
    const float ty0 = (c.mode == 1) ? (float)c.by : (float)c.dy;
    o[0] = tx0 + (float)c.w * 0.5f - (cxp - (float)dispX) / k;
    o[1] = ty0 - (c.fy + 0.5f - (float)dispY);
    o[2] = (float)dispW / k; o[3] = (float)dispH;
    o[4] = s_fbW > 0 ? (float)w / (float)s_fbW : 0.f; o[5] = s_fbH > 0 ? (float)h / (float)s_fbH : 0.f;
    /* ⭐ 01.10 (игрок, слот 6: «должно быть только это прозрачное искажение»): цвет ячейки игры 8A8A8A осветлял купол на 7.8 %,
     * а ячейки перекрываются — светлые пятна ползли по кадру вместе с ячейками. prevMap.w = 1 — без осветления, только сдвиг.
     * DC_SCENE_CAPTURE_TINT=1 — как было (осветление гаснет к кромке купола). */
    static int tint = -1;
    if (tint < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_TINT"); tint = (e && *e == '1') ? 1 : 0; }
    /* ⭐ 01.10 (тот же слот 6, «дрыжки»): затенение углов (SSAO) считается по НЕсдвинутой глубине мира и ложится на ячейку
     * после неё — под куполом его тёмные стыки и ореол вокруг Регины стоят мимо сдвинутой картинки и дёргаются вместе с
     * куполом (30 раз в секунду). Бит 2: в шейдере затенение гаснет там, где сдвиг больше ~2 точек вывода (альфа 0.5..1 =
     * доля, dc_ssao_z/_comp). DC_SCENE_CAPTURE_AOFADE=0 — затенение на всей ячейке, как 01.10 ночью. */
    static int aof = -1;
    if (aof < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_AOFADE"); aof = (e && *e == '0') ? 0 : 1; }
    o[6] = (c.mode == 1) ? 3.f : 2.f; o[7] = (float)((tint ? 0 : 1) + (aof ? 2 : 0));
}
/* Прервать проход, снять кусок цветной цели под ячейкой в образ binding 2, продолжить проход (цвет и глубина загружаются) */
static void scap_switch(const Scap& c, const VkRenderPassBeginInfo& rbi0, int w, int h, int dispX, int dispY, int dispW, int dispH) {
    vkCmdEndRenderPass(s_cmd);                              /* цветная цель теперь TRANSFER_SRC_OPTIMAL */
    const float kx = (float)w / (float)dispW, ky = (float)h / (float)dispH;
    /* ⭐ 01.10.2026 (игрок, слот 6: «горизонтальные полосы — края квадов, видимо, выступают немного», «дрыжки»). Квад ячейки
     * стоит на полпикселя ниже и правее куска (вершины зеркала — x + 0.5): его низ на 2.25 точки 1080p ниже куска, а запас
     * копии был 2 точки. Нижняя строка ячейки (с MSAA по выборкам — и предпоследняя) читала в s_prevImg чужое: кадр ПРОШЛОГО
     * показа (уже с затенением углов — темнее) или кусок более ранней ячейки этого кадра. Замер: тёмная строка в 1 точку
     * (−6 яркости) ровно по низу ячеек ряда y 100..164 (строка 739); ячейки всплывают на пиксель за кадр игры — полоса ползёт
     * и дёргается. Теперь копия покрывает весь квад (+0.5) и опору билинейной выборки. DC_SCENE_CAPTURE_MARGIN=0 — как было. */
    static int marg = -1;
    if (marg < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_MARGIN"); marg = (e && *e == '0') ? 0 : 1; }
    const float ext = marg ? 0.5f : 0.f;
    const int pad = marg ? 3 : 2;
    int x0 = (int)floorf((c.fx - (float)dispX) * kx) - pad, y0 = (int)floorf((c.fy - (float)dispY) * ky) - pad;   /* 01.10 вечер: fx, fy */
    int x1 = (int)ceilf((c.fx + (float)(c.w - dispX) + ext) * kx) + pad, y1 = (int)ceilf((c.fy + (float)(c.h - dispY) + ext) * ky) + pad;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
    if (x1 > x0 && y1 > y0) {
        VkImageMemoryBarrier pb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        pb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; pb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pb.image = s_prevImg;
        pb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &pb);
        VkImageCopy ic{};
        ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.srcOffset = { x0, y0, 0 }; ic.dstOffset = { x0, y0, 0 };
        ic.extent = { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 1 };
        vkCmdCopyImage(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_prevImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        VkImageMemoryBarrier pr = pb;
        pr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; pr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &pr);
    }
    VkRenderPassBeginInfo r2 = rbi0;
    r2.renderPass = s_rpResumeCur ? s_rpResumeCur : s_rpResume;   /* 25.09: с MSAA — продолжение прохода MSAA */
    r2.clearValueCount = 0; r2.pClearValues = nullptr;
    vkCmdBeginRenderPass(s_cmd, &r2, VK_SUBPASS_CONTENTS_INLINE);
}

extern "C" void dcvk_push_tri(const float* v, int semi_mode, int ztest) {
    /* 10.10: копия кадра паузы (полупрозрачный квад 15 bpp из видеопамяти на полэкрана и больше) в мире - не рисуется, вместо
     * неё ровная плита (s_pauseDim, dcvk_render). Приходит и с глубиной (PGXP), поэтому - до разбора по ztest. */
    if (semi_mode >= 0 && dc_pause_even_on()) {
        const int tpq = (int)(v[8] + 0.5f), flq = (int)(v[11] + 0.5f);
        if ((flq & 1) && !(flq & 8) && ((tpq >> 7) & 3) == 2) {
            GpuDisplayInfo dq; gpu_get_display_info(&dq);
            float qx0 = v[0], qx1 = v[0], qy0 = v[1], qy1 = v[1];
            for (int i = 1; i < 3; i++) {
                const float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
                if (x < qx0) qx0 = x; if (x > qx1) qx1 = x;
                if (y < qy0) qy0 = y; if (y > qy1) qy1 = y;
            }
            const uint8_t* rq = memory_get_ram_ptr();
            if (rq && rq[0xB7878] == 3 && dq.width >= 64 && dq.height >= 64 &&
                qx1 - qx0 >= 0.45f * (float)dq.width && qy1 - qy0 >= 0.45f * (float)dq.height) {
                s_frameCopy = 1;
                s_pauseDim = 1;
                return;
            }
        }
    }
    struct TopScope { ~TopScope() { s_topNow = 0; } } topScope;          /* 23.09: DANGER — поверх всего */
    s_topNow = (((int)(v[11] + 0.5f) & 1) && !g_dcvk_room_pass &&
                dc_tex_is_danger((int)(v[8] + 0.5f), v, VERT_FLOATS)) ? 1 : 0;
    /* the flicker fuser stores screen positions for replay - an interface
     * primitive about to be squeezed must not be noted at its unsqueezed
     * place (it never flickers in the fuser's sense anyway) */
    if (!g_dcvk_ui2d && s_repOvSlot < 0) fuse_note2(v, semi_mode, ztest);   /* 05.10 ночь (2): меш из mods не повторяется */
    /* 15.09: ОБЛАСТЬ РИСОВАНИЯ этого примитива. Клип ставится ниже, по той ветке, которой
     * пойдут вершины (как есть / сжатие интерфейса / сжатие GTE под 16:9), и сбрасывается
     * на выходе. Область на весь экран — не клип. */
    struct ClipScope { ~ClipScope() { s_clipNow = 0; s_sqzNow = 0; } } clipScope;
    int areaClip = 0;
    if (s_pclipOn && dc_drawarea_clip_on()) {
        /* только вне мира (байт экрана не 3): брифинг, меню, инвентарь, карта. В мире наш
         * кадр — своя камера и своя комната, область рисования игры там не трогаем;
         * DC_DRAWAREA_CLIP=2 — и в мире тоже */
        static int anyScr = -1;
        if (anyScr < 0) { const char* e = getenv("DC_DRAWAREA_CLIP"); anyScr = (e && *e == '2') ? 1 : 0; }
        uint8_t* rca = memory_get_ram_ptr();
        const int scra = rca ? rca[0xB7878] : 3;
        GpuDisplayInfo dca; gpu_get_display_info(&dca);
        const int aw = s_pclip[2] - s_pclip[0] + 1, ah = s_pclip[3] - s_pclip[1] + 1;
        if ((anyScr || scra != 3) && dca.width >= 64 && dca.height >= 64 &&
            (aw < (int)dca.width || ah < (int)dca.height)) areaClip = 1;
    }
    /* sq: 0 — вершины как есть; иначе x' = c + (x - c) * 0.75 вокруг c */
    auto clipSet = [&](int squeezed, float c) {
        if (!areaClip) return;
        float x0 = (float)s_pclip[0], x1 = (float)(s_pclip[2] + 1);
        s_clipRawX0 = x0; s_clipRawX1 = x1;                 /* 21.09: для dcvk_sqz_resolve */
        if (squeezed) { const float k = dc_ws_sq(); x0 = c + (x0 - c) * k; x1 = c + (x1 - c) * k; }   /* 10.10: любая форма */
        s_clipRect[0] = x0; s_clipRect[1] = (float)s_pclip[1];
        s_clipRect[2] = x1; s_clipRect[3] = (float)(s_pclip[3] + 1);
        s_clipNow = 1;
    };
    /* WHERE THE CHAIN BREAKS (DC_NCSTAT=1). Four numbers answer it:
     *   total   - primitives reaching the raster
     *   nodepth - arrived without per-vertex w (PGXP miss or the guard):
     *             those cannot be clipped, and a 3D one among them is a
     *             polygon whose projection blew up
     *   near    - has a vertex at or behind the camera plane (what we want)
     *   clipped - actually rebuilt here */
    { static int st = -1;
      if (st < 0) { const char* e = ::getenv("DC_NCSTAT"); st = (e && *e == '1') ? 1 : 0; }
      if (st) {
        static long total = 0, nodepth = 0, nearv = 0, last = -1;
        total++;
        if (!ztest) nodepth++;
        else if (v[12] < 2.f || v[VERT_FLOATS + 12] < 2.f || v[2 * VERT_FLOATS + 12] < 2.f) nearv++;
        if (total / 200000 != last) { last = total / 200000;
            std::fprintf(stderr, "raster: total=%ld nodepth=%ld near=%ld%c", total, nodepth, nearv, 10); }
      } }
    /* 21.09 вечер: текстура этого примитива — только что снятый кусок кадра (см. Scap)? Тогда он рисуется из НАШЕГО кадра:
     * без сжатия интерфейса (ячейка стоит на точке мира), отдельным draw со своим номером захвата. Проверка стоит ДО развилки
     * по глубине: вершины ячейки считает процессор, но их экранные координаты иногда совпадают с записью PGXP настоящей
     * вершины мира — зеркало выдаёт такой треугольник «с глубиной», он уходил 3D-путём мимо распознавания и рисовался из
     * VRAM 1x (игрок: «появляются испорченные чёрные прямоугольники»). Ячейка — всегда 2D: w = 1, без теста глубины. */
    if (!s_scaps.empty()) {
        const int tpq = (int)(v[8] + 0.5f), flq = (int)(v[11] + 0.5f);
        if ((flq & 1) && !(flq & 8) && ((tpq >> 7) & 3) == 2) {
            Scap& c = s_scaps.back();
            const float bx = (float)((tpq & 15) * 64), by = (float)(((tpq >> 4) & 1) * 256);
            int inside = 1;
            for (int i = 0; i < 3 && inside; i++) {
                const float U = bx + v[i * VERT_FLOATS + 2], V = by + v[i * VERT_FLOATS + 3];
                const float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
                if (U < (float)c.dx - 0.5f || U > (float)(c.dx + c.w) + 0.5f ||
                    V < (float)c.dy - 0.5f || V > (float)(c.dy + c.h) + 0.5f) inside = 0;
                if (x < -4096.f || x > 8192.f || y < -4096.f || y > 8192.f) inside = 0;
            }
            if (inside) {
                float cv[3 * VERT_FLOATS];
                memcpy(cv, v, sizeof cv);
                if (ztest) {
                    static long nz = 0;
                    if (++nz <= 6 || (nz % 500) == 0)
                        std::fprintf(stderr, "raster: scene-capture cell arrived with a (false) depth, w=%.1f,%.1f,%.1f - drawn flat (#%ld)%c",
                                     v[12], v[VERT_FLOATS + 12], v[2 * VERT_FLOATS + 12], nz, 10);
                    for (int i = 0; i < 3; i++) cv[i * VERT_FLOATS + 12] = 1.f;
                }
                const float k = dc_scap_k();
                const float cxp = (float)c.sx + (float)c.w * 0.5f + 0.5f;   /* центр ячейки там, где лежат вершины (x + 0.5) */
                /* ГЛАДКАЯ ЯЧЕЙКА. Решение — по первому треугольнику ячейки: в нём должна быть центральная вершина (UV = середина
                 * ячейки), а две другие — стоять на своих местах (положение = источник + UV). Тогда сдвиг центра известен, и
                 * вместо квадов игры идёт один наш квад; остальные треугольники этой ячейки проглатываются. Иначе — как есть. */
                if (c.mode == 0) {
                    c.mode = 2; c.bx = (int)bx; c.by = (int)by;
                    if (dc_scap_smooth() && (c.w % 4) == 0 && (c.h % 4) == 0) {
                        int ci = -1, regular = 1;
                        for (int i = 0; i < 3; i++) {
                            const float ur = bx + v[i * VERT_FLOATS + 2] - (float)c.dx, vr = by + v[i * VERT_FLOATS + 3] - (float)c.dy;
                            const float ex = v[i * VERT_FLOATS] - ((float)c.sx + ur + 0.5f), ey = v[i * VERT_FLOATS + 1] - ((float)c.sy + vr + 0.5f);
                            if (fabsf(ur - (float)c.w * 0.5f) < 0.75f && fabsf(vr - (float)c.h * 0.5f) < 0.75f) { ci = i; c.Dx = ex; c.Dy = ey; }
                            else if (fabsf(ex) > 1.f || fabsf(ey) > 1.f) regular = 0;
                        }
                        if (ci >= 0 && regular) c.mode = 1;
                        { static int zr = -1;                    /* 01.10: DC_SCENE_CAPTURE_ZERO=1 — ячейка без сдвига (сверка: кадр как без ячеек) */
                          if (zr < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_ZERO"); zr = (e && *e == '1') ? 1 : 0; }
                          if (zr) { c.Dx = 0.f; c.Dy = 0.f; } }
                    }
                    if (c.mode == 1) {
                        const int cw = c.w, ch = c.h;
                        const float tpv = (float)((tpq & 511) + ((cw / 4) << 9) + ((ch / 4) << 15));   /* размер ячейки — в старших битах страницы */
                        /* один квад ячейки: место fx, fy (угол куска в координатах VRAM), сдвиг центра Dx, Dy; scapIdx — индекс+1 Scap */
                        auto emitCell = [&](int scapIdx, float fx, float fy, float Dx, float Dy) {
                            const float cxe = fx + (float)cw * 0.5f + 0.5f;  /* центр ячейки там, где лежат вершины (x + 0.5) */
                            const float X0 = cxe - (float)cw * 0.5f * k, X1 = cxe + (float)cw * 0.5f * k;
                            const float Y0 = fy + 0.5f, Y1 = fy + (float)ch + 0.5f;
                            const float dxv = floorf(Dx * 16.f + 0.5f) + 32768.f, dyv = floorf(Dy * 16.f + 0.5f) + 32768.f;
                            const float px[4] = { X0, X1, X0, X1 }, py[4] = { Y0, Y0, Y1, Y1 };
                            const float pu[4] = { 0.f, (float)cw, 0.f, (float)cw }, pv[4] = { 0.f, 0.f, (float)ch, (float)ch };
                            static const int idx[6] = { 0, 1, 2, 1, 3, 2 };
                            s_scapNow = scapIdx;
                            clipSet(0, 0.f);
                            const int uiWas = g_dcvk_ui2d;
                            if (dc_scap_uitop()) g_dcvk_ui2d = 1;         /* 01.10 вечер: ячейка — всегда слой после мира */
                            for (int t = 0; t < 2; t++) {
                                float qv[3 * VERT_FLOATS];
                                for (int i = 0; i < 3; i++) {
                                    float* d = qv + i * VERT_FLOATS;
                                    memcpy(d, cv, VERT_FLOATS * sizeof(float));       /* цвет, флаги — как у треугольника игры */
                                    const int q = idx[t * 3 + i];
                                    d[0] = px[q]; d[1] = py[q]; d[2] = pu[q]; d[3] = pv[q];
                                    d[8] = tpv; d[9] = dxv; d[10] = dyv; d[12] = 1.f;
                                }
                                dcvk_push_tri_raw(qv, semi_mode, 0);
                                s_scapTris++;
                            }
                            g_dcvk_ui2d = uiWas;
                            s_scapNow = 0;
                        };
                        /* 01.10 вечер: сглаживание по времени (см. scap_track) — квад A и квад B; без него — как пришло */
                        if (dc_scap_smootht() && c.sid >= 0) {
                            const float v4[4] = { (float)(c.sx - c.ax), (float)(c.sy - c.ay), c.Dx, c.Dy };
                            float A[4], B[4];
                            scap_track(c.sid, cw, ch, v4, A, B);
                            const int ia = (int)s_scaps.size();
                            c.ab = 1; c.fx = A[0] + (float)c.ax; c.fy = A[1] + (float)c.ay; c.uDx = A[2]; c.uDy = A[3];
                            Scap cb = c; cb.ab = 2; cb.fx = B[0] + (float)c.ax; cb.fy = B[1] + (float)c.ay; cb.uDx = B[2]; cb.uDy = B[3];
                            const float aFx = c.fx, aFy = c.fy;
                            emitCell(ia, aFx, aFy, A[2], A[3]);
                            if (s_scaps.size() < 128) {
                                s_scaps.push_back(cb);                   /* ⛔ ссылка c дальше недействительна */
                                emitCell((int)s_scaps.size(), cb.fx, cb.fy, B[2], B[3]);
                            } else s_scaps[(size_t)ia - 1].ab = 0;
                            return;
                        }
                        c.uDx = c.Dx; c.uDy = c.Dy;
                        emitCell((int)s_scaps.size(), (float)c.sx, (float)c.sy, c.Dx, c.Dy);
                        return;
                    }
                }
                if (c.mode == 1) return;                     /* остальные треугольники гладкой ячейки: её квад уже в потоке */
                if (k < 0.999f)
                    for (int i = 0; i < 3; i++) cv[i * VERT_FLOATS] = cxp + (cv[i * VERT_FLOATS] - cxp) * k;
                s_scapNow = (int)s_scaps.size(); s_scapTris++;
                clipSet(0, 0.f);
                { const int uiWas = g_dcvk_ui2d;
                  if (dc_scap_uitop()) g_dcvk_ui2d = 1;           /* 01.10 вечер: и квады игры как есть — слой после мира */
                  dcvk_push_tri_raw(cv, semi_mode, 0);
                  g_dcvk_ui2d = uiWas; }
                s_scapNow = 0;
                return;
            }
        }
    }
    /* ⭐ 30.09 (слот 6, «третья энергия»: «мигает… снизу выплывают необработанные чёрные квады»). Квад со служебной текстуры
     * захвата (15 бит, страница 512,0, цвет ячейки 8A8A8A), НЕ узнанный как ячейка этого кадра (захвата в кадре нет — копия и
     * квады разошлись по нашим кадрам, или захват отвергнут), рисовался из видеопамяти 1x — там комнаты нет, чёрный квадрат на
     * кадр. Если захваты были недавно (эффект идёт), такой квад не рисуется. DC_SCENE_CAPTURE_SKIPMISS=0 — как было. */
    { const int tpm = (int)(v[8] + 0.5f), flm2 = (int)(v[11] + 0.5f);
      if ((flm2 & 1) && !(flm2 & 8) && ((tpm >> 7) & 3) == 2 && (tpm & 15) == 8 && !((tpm >> 4) & 1)) {
          static int skip = -1; if (skip < 0) { const char* e = ::getenv("DC_SCENE_CAPTURE_SKIPMISS"); skip = (e && *e == '0') ? 0 : 1; }
          const int r8 = (int)(v[4] + 0.5f), g8 = (int)(v[5] + 0.5f), b8 = (int)(v[6] + 0.5f);
          const int cellcol = (r8 == g8 && g8 == b8 && r8 >= 0x80 && r8 <= 0x94);
          const int recent = (s_scapSeenSeq && s_prim_frame_seq - s_scapSeenSeq <= 120);
          static long nmiss = 0, nskip = 0; nmiss++;
          if (skip && cellcol && recent) nskip++;
          if (nmiss <= 8 || (nmiss % 300) == 0)
              std::fprintf(stderr, "raster: scene-capture cell NOT recognized (#%ld, skipped %ld, captures this frame %d) - %s%c",
                           nmiss, nskip, (int)s_scaps.size(), (skip && cellcol && recent) ? "not drawn (DC_SCENE_CAPTURE_SKIPMISS)" : "drawn from VRAM 1x", 10);
          if (skip && cellcol && recent) return; } }
    /* Only geometry that came with a real per-vertex depth can be clipped -
     * 2D and HUD primitives arrive with w = 1 and ztest = 0 and are left
     * exactly alone. */
    if (!ztest) {
        /* No per-vertex depth: normal for 2D and HUD work, which lives inside
         * the VRAM page. A "3D" primitive whose vertices landed thousands of
         * pixels away is one whose projection blew up - drop it rather than
         * smear it across the screen. */
        for (int i = 0; i < 3; i++) {
            float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
            if (x < -4096.f || x > 8192.f || y < -4096.f || y > 8192.f) { if (ACTOR_TRI(v)) s_alost_oob++; return; }
        }
        /* 01.10 (k): линия между точками GTE (ось сборки в загадке STABILIZER, gpu.c dc_line_gte_detect) — не интерфейс: без
         * сжатия к центру; в кадре загадки 4:3 растягивается обратно ×4/3 вместе с 3D (M43Cand). DC_LINE_GTE=0 — как было. */
        if (g_dcvk_gte_line) {
            clipSet(0, 0.f);
            const int n0 = s_nverts;
            dcvk_push_tri_raw(v, semi_mode, ztest);
            if (dc_mon43_env() && dc_ws_world() && s_vmap && s_nverts == n0 + 3) {
                int dxm = 0, dym = 0; gpu_get_draw_offset(&dxm, &dym);
                const float ox = g_pt_proj_ofx + (float)dxm;
                M43Cand c; c.vi = n0;
                for (int i = 0; i < 3; i++) c.altx[i] = ox + (s_vmap[(size_t)(n0 + i) * VERT_FLOATS] - ox) / dc_ws_sq();
                s_m43Cands.push_back(c);
            }
            return;
        }
        /* 21.09 ночь: плитка рамки монитора — непрозрачный текстурный прямоугольник 16x16 у края области рисования (M43Cand) */
        if (dc_mon43_env()) {
            const int flm = (int)(v[11] + 0.5f);
            if ((flm & 1) && semi_mode < 0) {
                float mx0 = v[0], mx1 = v[0], my0 = v[1], my1 = v[1];
                for (int i = 1; i < 3; i++) {
                    const float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
                    if (x < mx0) mx0 = x; if (x > mx1) mx1 = x; if (y < my0) my0 = y; if (y > my1) my1 = y;
                }
                const float tw = mx1 - mx0, th = my1 - my0;
                const int t16 = fabsf(tw - 16.f) < 0.75f && fabsf(th - 16.f) < 0.75f;
                const int t32 = dc_mon43_wide() && (fabsf(tw - 32.f) < 0.75f || fabsf(tw - 64.f) < 0.75f)   /* 28.09 (2): рамка крана */
                                && (fabsf(th - 32.f) < 0.75f || fabsf(th - 31.f) < 0.75f);
                if (t16 || t32) {
                    int dxt = 0, dyt = 0; gpu_get_draw_offset(&dxt, &dyt);
                    GpuDisplayInfo dmt; gpu_get_display_info(&dmt);
                    const float ax0 = (float)dxt, ay0 = (float)dyt, ax1 = ax0 + (float)dmt.width, ay1 = ay0 + (float)dmt.height;
                    if (dmt.width >= 64 && dmt.height >= 64 && mx0 >= ax0 - 1.f && mx1 <= ax1 + 1.f && my0 >= ay0 - 1.f && my1 <= ay1 + 1.f) {
                        const int side = (my0 < ay0 + 33.f ? 1 : 0) | (my1 > ay1 - 33.f ? 2 : 0) | (mx0 < ax0 + 33.f ? 4 : 0) | (mx1 > ax1 - 33.f ? 8 : 0);
                        if (side) { M43Tile t; t.vi = s_nverts; t.side = side; s_monTiles.push_back(t); }
                    }
                }
            }
        }
        /* HI-RES ШРИФТ: текстурированный примитив со страницы шрифта игры
         * (tpage x=768, y=0, 4bpp) получает F_HIFONT — шейдер берёт атлас
         * высокого разрешения вместо VRAM-текселей */
        float hv[3 * VERT_FLOATS];
        /* только в игре: BIOS держит текстуру своей надписи «PlayStation» в
         * той же странице (768,0) — с атласом поверх неё на логотипе снова
         * появлялись наши буквы (игрок 2026-09-02, вечер) */
        if (s_fontReady && dc_game_loaded()) {
            int tp = (int)(v[8] + 0.5f), fl = (int)(v[11] + 0.5f);
            /* страница 12 = шрифт (768,0); страница 11 = титульные полоски
             * (704,0) — вторая половина атласа, флаг F_HIFONT2 (128) */
            if ((fl & 1) && !(fl & 8) && ((tp & 0xF) == 12 || (tp & 0xF) == 11) &&
                ((tp >> 4) & 1) == 0 && ((tp >> 7) & 3) == 0) {
                int add = ((tp & 0xF) == 11) ? (64 | 128) : 64;
                memcpy(hv, v, sizeof hv);
                for (int i = 0; i < 3; i++) hv[i * VERT_FLOATS + 11] = (float)(fl | add);
                v = hv;
            }
        }
        /* копия кадра (см. s_frameCopy у dc_text_over_world): ПОЛУПРОЗРАЧНЫЙ
         * текстурный квад 15 bpp (tp 256/258/272/274, замер sqdbg 12.09:
         * quad160x240 ui2d=0 semi=0) на полэкрана и больше — начало экрана
         * паузы; классификатор считает такие квады миром, поэтому ловим их
         * здесь, до сжатия и независимо от g_dcvk_ui2d */
        if (!s_frameCopy) {
            int tpc = (int)(v[8] + 0.5f), flc = (int)(v[11] + 0.5f);
            if ((flc & 1) && !(flc & 8) && ((tpc >> 7) & 3) == 2) {
                GpuDisplayInfo dfc; gpu_get_display_info(&dfc);
                float fx0 = v[0], fx1 = v[0], fy0 = v[1], fy1 = v[1];
                for (int i = 1; i < 3; i++) {
                    float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
                    if (x < fx0) fx0 = x; if (x > fx1) fx1 = x;
                    if (y < fy0) fy0 = y; if (y > fy1) fy1 = y;
                }
                if (dfc.width >= 64 && dfc.height >= 64 &&
                    fx1 - fx0 >= 0.45f * (float)dfc.width && fy1 - fy0 >= 0.45f * (float)dfc.height)
                    s_frameCopy = 1;
            }
        }
        /* the game's 2D interface keeps 4:3 proportions on the wide canvas */
        float sq[3 * VERT_FLOATS];
        {
            const int defer = (dc_aspect_nolag() & 2) && dc_flat_pillarbox_on();
            int would = 0;
            const int sqd = dc_ui_squeeze_tri(v, sq, defer ? &would : nullptr);
            int isfont = 0;
            { const int tpf = (int)(v[8] + 0.5f), flf = (int)(v[11] + 0.5f);          /* 27.09: буква (dcvk_font_squeezed_last) */
              isfont = (flf & 1) && (tpf & 0xF) == 12 && ((tpf >> 4) & 1) == 0 && ((tpf >> 7) & 3) == 0;
              if (isfont && !sqd && !would) s_fontFlatN++; }
            if (sqd || would) {
                GpuDisplayInfo dsq; gpu_get_display_info(&dsq);
                const float csq = (float)dsq.display_x + (float)dsq.width * 0.5f;   /* центр сжатия — как в dc_ui_squeeze_tri */
                const int n0 = s_nverts;
                if (defer) { s_sqzNow = sqd ? 1 : 2; s_sqzC = csq; }
                if (sqd) { clipSet(1, csq); dcvk_push_tri_raw(sq, semi_mode, ztest); }
                else     { clipSet(0, 0.f); dcvk_push_tri_raw(v, semi_mode, ztest); }
                s_sqzNow = 0;
                if (defer && s_nverts == n0 + 3) {     /* 21.09: оба варианта X — решение при записи кадра */
                    SqzCand c; c.vi = n0; c.sq_now = sqd ? 1 : 0; c.font = isfont;
                    const float* alt = sqd ? v : sq;
                    for (int i = 0; i < 3; i++) c.altx[i] = alt[i * VERT_FLOATS];
                    s_sqzCands.push_back(c);
                } else if (isfont) { if (sqd) s_fontSqN++; else s_fontFlatN++; }
                return;
            }
        }
        clipSet(0, 0.f);
        dcvk_push_tri_raw(v, semi_mode, ztest);
        return;
    }
    s_worldDrawn = 1;              /* дальше идёт геометрия с глубиной = мир */
    /* 14.09 (игрок: «в брифинге модель доктора смещена»): плоский кадр с 3D
     * (страницы досье, dc_copyflat_now) показывается 4:3 целиком, а GTE под
     * DC_WIDESCREEN ужал проекцию по X на 3/4 вокруг центра — бюст уезжал к
     * центру. Растягиваем такие вершины обратно (×4/3 вокруг центра проекции)
     * и дальше считаем проекцию неужатой (wsx = 1). */
    float vflat[3 * VERT_FLOATS];
    float wsx_flat = 0.f;                                   /* 0 = как обычно */
    if (s_prev_copyflat) {
        /* 15.09: растягивать обратно — только когда такой кадр идёт пилларбоксом при ужатом мире
         * (режим 1 соотношения); в «16:9» кадр растянут выводом целиком, в «4:3» мир не ужат */
        const int wsf = dc_ui43_squeeze() && dc_ws_world();
        if (wsf) {
            int dox0 = 0, doy0 = 0; gpu_get_draw_offset(&dox0, &doy0);
            const float ox0 = g_pt_proj_ofx + (float)dox0;
            memcpy(vflat, v, sizeof vflat);
            for (int i = 0; i < 3; i++) vflat[i * VERT_FLOATS] = ox0 + (vflat[i * VERT_FLOATS] - ox0) / dc_ws_sq();
            v = vflat; wsx_flat = 1.0f;
        }
    }
    /* 15.09: клип 3D-примитива. В плоском кадре с 3D (досье, растянуто обратно) и в 4:3
     * вершины в родных координатах; в мире 16:9 GTE сжал X на 3/4 вокруг центра проекции —
     * область игры сжимается так же. */
    {
        const int wsc = dc_ws_world();   /* 15.09: соотношение сторон из меню SCREEN — на лету (memory.c) */
        if (wsx_flat > 0.f || !wsc) clipSet(0, 0.f);
        else { int doxc = 0, doyc = 0; gpu_get_draw_offset(&doxc, &doyc);
               clipSet(1, g_pt_proj_ofx + (float)doxc); }
    }
    /* 21.09 ночь: загадка на мониторе — второй X вершин (GTE ужал проекцию под 16:9 вокруг её центра; обратно ×4/3), см. M43Cand */
    const int m43 = dc_mon43_env() && wsx_flat == 0.f && dc_ws_world();
    float m43ox = 0.f;
    if (m43) { int dxm = 0, dym = 0; gpu_get_draw_offset(&dxm, &dym); m43ox = g_pt_proj_ofx + (float)dxm; }
    auto m43reg = [&](int n0) {
        if (!m43 || s_nverts != n0 + 3 || !s_vmap) return;
        M43Cand c; c.vi = n0;
        for (int i = 0; i < 3; i++) c.altx[i] = m43ox + (s_vmap[(size_t)(n0 + i) * VERT_FLOATS] - m43ox) / dc_ws_sq();
        s_m43Cands.push_back(c);
    };
    const float w0 = v[12], w1 = v[VERT_FLOATS + 12], w2 = v[2 * VERT_FLOATS + 12];
    if (w0 >= NC_NEAR && w1 >= NC_NEAR && w2 >= NC_NEAR) {
        const int n0m = s_nverts;
        dcvk_push_tri_raw(v, semi_mode, ztest);      /* nothing crosses */
        m43reg(n0m);
        return;
    }
    if (w0 < NC_NEAR && w1 < NC_NEAR && w2 < NC_NEAR) { if (ACTOR_TRI(v)) s_alost_behind++; return; }   /* wholly behind */
    /* ⭐ ПОЛУПРОЗРАЧНОЕ ЧЕРЕЗ ПЛОСКОСТЬ КАМЕРЫ НЕ РИСУЕТСЯ (16.09.2026 вечер, игрок, слот 7: «проблема
     * растягивания декалей в мусорных координатах вернулась; декаль вне камеры не должна отображаться на
     * экране так и в принципе»). Замер dump_prims на ракурсе игрока: красная полоса через кадр — лазерный
     * луч игры (semi, tpage 30, CLUT 768,503), у квада две вершины за камерой; клип по ближней плоскости
     * оставлял часть от стены до w = 2 (вершины на самой плоскости у глаза, x 38..346), и она расходилась
     * клином на полэкрана. Стены и пол клип по-прежнему достраивает (иначе дыры у края кадра), а декаль, луч,
     * ореол, стекло, у которых есть вершина за камерой, пропускаются целиком — консоль с родных ракурсов их
     * так не видела никогда. DC_SEMI_NEAR_DROP=0 — резать, как раньше. */
    if (semi_mode >= 0) {
        static int snd = -1;
        if (snd < 0) { const char* e = ::getenv("DC_SEMI_NEAR_DROP"); snd = (e && *e == '0') ? 0 : 1; }
        if (snd) {
            static long nd = 0, lastlog = -1;
            ++nd;
            if (nd / 256 != lastlog && lastlog < 64) { lastlog = nd / 256;
                std::fprintf(stderr, "raster: see-through triangle across the camera plane dropped (#%ld, w=%.1f,%.1f,%.1f room=%d)%c",
                             nd, w0, w1, w2, g_dcvk_room_pass, 10); }
            return;
        }
    }

    const float H = g_pt_proj_h;
    if (!(H > 0.0f)) { const int n0m = s_nverts; dcvk_push_tri_raw(v, semi_mode, ztest); m43reg(n0m); return; }
    const int ws = dc_ws_world();   /* 15.09: соотношение сторон из меню SCREEN — на лету (memory.c) */
    const float wsx = (wsx_flat > 0.f) ? wsx_flat : (ws ? dc_ws_sq() : 1.0f);
    int dox = 0, doy = 0;
    gpu_get_draw_offset(&dox, &doy);
    const float ox = g_pt_proj_ofx + (float)dox, oy = g_pt_proj_ofy + (float)doy;

    NCv in[3];
    for (int i = 0; i < 3; i++) {
        const float* s = v + i * VERT_FLOATS;
        float z = s[12];
        in[i].z = z;
        in[i].x = (s[0] - ox) * z / (H * wsx);
        in[i].y = (s[1] - oy) * z / H;
        in[i].u = s[2]; in[i].vv = s[3];
        in[i].r = s[4]; in[i].g = s[5]; in[i].b = s[6];
        in[i].tag = s_pairTri[i];
    }

    NCv poly[4];
    int n = 0;
    for (int i = 0; i < 3 && n < 4; i++) {
        const NCv& a = in[i];
        const NCv& b = in[(i + 1) % 3];
        float da = a.z - NC_NEAR, db = b.z - NC_NEAR;
        if (da >= 0.0f && n < 4) poly[n++] = a;
        if ((da < 0.0f) != (db < 0.0f) && n < 4) { nc_mix(poly[n], a, b, da / (da - db)); poly[n].tag = (da >= 0.0f) ? a.tag : b.tag; n++; }
    }
    if (n < 3) { if (ACTOR_TRI(v)) s_alost_short++; return; }
    const uint32_t pin[3] = { s_pairTri[0], s_pairTri[1], s_pairTri[2] };   /* 26.10: метки - у каждого куска обрезки свои */

    /* per-primitive lanes (texture page, clut, flags) belong to the whole
     * triangle, so they come across untouched */
    float out[3 * VERT_FLOATS];
    for (int k = 1; k + 1 < n; k++) {
        const int idx[3] = { 0, k, k + 1 };
        for (int j = 0; j < 3; j++) {
            const NCv& p = poly[idx[j]];
            float* o = out + j * VERT_FLOATS;
            o[0] = ox + H * p.x / p.z * wsx;
            o[1] = oy + H * p.y / p.z;
            o[2] = p.u;  o[3] = p.vv;
            o[4] = p.r;  o[5] = p.g;  o[6] = p.b;  o[7] = 0.0f;
            o[8] = v[8]; o[9] = v[9]; o[10] = v[10]; o[11] = v[11];
            o[12] = p.z;
            s_pairTri[j] = p.tag;
        }
        { const int n0m = s_nverts; dcvk_push_tri_raw(out, semi_mode, ztest); m43reg(n0m); }
    }
    for (int j = 0; j < 3; j++) s_pairTri[j] = pin[j];
    { static long n_clip = 0, last = -1;
      static int stat = -1;
      if (stat < 0) { const char* e = ::getenv("DC_NCSTAT"); stat = (e && *e == '1') ? 1 : 0; }
      if (stat && ++n_clip / 2048 != last) { last = n_clip / 2048;
          std::fprintf(stderr, "raster near clip: %ld triangles rebuilt\n", n_clip); } }
}

// One triangle: v = 3 x [x,y,u,v,r,g,b,_,tpage,clutx,cluty,flags,w].
// semi_mode -1 = opaque; ztest/zwrite mirror the CPU zt semantics.
static void dcvk_push_tri_raw(const float* v, int semi_mode, int ztest) {
    /* ⏱ Честный счёт вершин персонажа, дошедших до потока: без гейта по Y,
     * который стоит у s_actorVerts (тот считает только видимую полосу кадра
     * и потому даёт мнимую «потерю» на близкой камере). */
    if (ACTOR_TRI(v) && !g_dcvk_room_pass) s_actor_out_all += 3;
    /* ТЕКСТ ИГРЫ — ПОВЕРХ МИРА (2026-09-09, игрок: «в загадке текст лежит
     * слоем ниже полупрозрачного стекла»). Мировые полупрозрачные (стекло,
     * лучи, гель) откладываются до конца кадра — см. translucent-last ниже, —
     * а окно текста игра рисует ВНУТРИ кадра, спрайтами шрифта. Отложенное
     * стекло ложилось поверх букв (замер primdump в той самой загадке: текст
     * = draw 4 kind=1, стекло = draws 5..16 kind=2 ПОСЛЕ него).
     * Как только пошёл примитив, 2D ПО ПОСТРОЕНИЮ (rect/sprite/fill/blit —
     * g_dcvk_ui2d, тот же признак, по которому интерфейс сжимается в 4:3),
     * накопленные semi выкладываем: они остаются НАД всем миром (для чего и
     * откладывались — «лучи поверх Регины») и ПОД интерфейсом, их взаимный
     * порядок сохраняется. DC_SEMI_UNDER_UI=0 — старое поведение. */
    if (g_dcvk_ui2d && !s_semiLate.empty()) {
        static int under = -1;
        if (under < 0) { const char* e = getenv("DC_SEMI_UNDER_UI"); under = (e && *e == '0') ? 0 : 1; }
        if (under) {
            s_draws.insert(s_draws.end(), s_semiLate.begin(), s_semiLate.end());
            s_semiLate.clear();
        }
    }
    /* SANITY NET (2026-08-23). A primitive whose projection blew up - a
     * saturated vertex nobody clipped, a billboard that landed on the eye -
     * must not be drawn as a slab across the screen. The PS1 draw space is
     * 1024x512; eight screens of overhang is already far more than a close
     * camera legitimately needs. The !(fabs < MAX) form also catches NaN.
     * This is the one choke point every submitter goes through (game prims,
     * our room, the billboard rebuilders, blits). DC_XY_MAX=0 disables. */
    { static float xymax = -1.f;
      if (xymax < 0.f) { const char* e = getenv("DC_XY_MAX"); xymax = e ? (float)atof(e) : 32768.f; }
      if (xymax > 0.f)
          for (int i = 0; i < 3; i++)
              if (!(fabsf(v[i * VERT_FLOATS]) < xymax) || !(fabsf(v[i * VERT_FLOATS + 1]) < xymax)) return; }
    if (s_nverts + 3 > MAX_VERTS) { if (ACTOR_TRI(v)) s_alost_full++; return; }   // stream full: drop
    /* DC_BIGSEMI_LOG=1 - forensic aid for "декали лезут мусором в экран":
     * any see-through triangle spanning most of the frame gets logged with
     * enough identity (blend, tpage, submitter, fuse-replay flag) to name
     * its source. Diagnostic only, no behaviour change. */
    { static int bl = -1; static long n = 0;
      if (bl < 0) { const char* e = getenv("DC_BIGSEMI_LOG"); bl = (e && *e == '1') ? 1 : 0; }
      if (bl && semi_mode >= 0) {
          float x0 = v[0], x1 = v[0], y0 = v[1], y1 = v[1];
          for (int i = 1; i < 3; i++) {
              float x = v[i * VERT_FLOATS], y = v[i * VERT_FLOATS + 1];
              if (x < x0) x0 = x; if (x > x1) x1 = x;
              if (y < y0) y0 = y; if (y > y1) y1 = y;
          }
          if (x1 - x0 > 500.f || y1 - y0 > 350.f)
              if ((++n % 8) == 1)
                  rlog("[BIGSEMI] #%ld semi=%d ztest=%d room=%d replay=%d tpage=%.0f "
                       "flags=%.0f bbox=(%.0f,%.0f)-(%.0f,%.0f) w=%.1f,%.1f,%.1f "
                       "xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f)\n",
                       n, semi_mode, ztest, g_dcvk_room_pass, s_fuse_replaying, v[8], v[11],
                       x0, y0, x1, y1, v[12], v[12 + VERT_FLOATS], v[12 + 2 * VERT_FLOATS],
                       v[0], v[1], v[VERT_FLOATS], v[VERT_FLOATS + 1],
                       v[2 * VERT_FLOATS], v[2 * VERT_FLOATS + 1]);
      } }
    if (!s_vmap && !dcvk_raster_init()) return;
    memcpy(s_vmap + (size_t)s_nverts * VERT_FLOATS, v,
           3 * VERT_FLOATS * sizeof(float));
    trep_tri(v, s_rvMap + (size_t)s_nverts * 4);   /* 05.10: текстура PNG из mods (−1 — родная) */
    /* 26.10.2026: метки кусков (пары кусков) - только у треугольников игры с глубиной; повтор мигающих - без меток */
    if (s_pvMap) for (int i = 0; i < 3; i++) s_pvMap[(size_t)s_nverts + (size_t)i] = (ztest && !s_fuse_replaying) ? s_pairTri[i] : 0u;
    /* 12.10.2026: блеск предметов - место вдоль диагонали предмета (pt_renderer.c rl_glint_*: цвет-альфа 16..17) в четвёртое
     * число второго буфера (шейдер - v_glint 1..2); цвет-альфу ниже перепишет номер вывода */
    for (int i = 0; i < 3; i++) { const float gq = v[i * VERT_FLOATS + 7];
        if (gq >= 15.5f && gq <= 17.5f) s_rvMap[((size_t)s_nverts + (size_t)i) * 4 + 3] = gq - 15.0f;
        else if (gq >= 31.5f && gq <= 63.5f) s_rvMap[((size_t)s_nverts + (size_t)i) * 4 + 3] = -(gq - 31.0f); }   /* 20.10: её кость -> -(кость + 1) */
    occ_note(v, 3);                                /* 09.10: заслонивший — и в память процессора (рамка на картах теней) */

    /* LIFT THE GAME'S DECALS OFF THE FLOOR.
     * The shadow blob and the blood are see-through primitives the game lays
     * flat on a surface, and against the floor WE draw they come out on the
     * wrong side of it - visible only from underneath (field: "декали
     * буквально лежат с обратной стороны пола"). Measured, this is not a
     * tie the painter order can settle: their w runs a third larger than our
     * room's over the same part of the frame, so no plausible bias on our
     * floor reaches them - biasing it ten times harder changed nothing.
     * They simply need to come forward a little, which is what the console's
     * ordering table did for them by construction. A small proportional step
     * toward the camera does it, and only see-through primitives of the
     * GAME's are touched: our own room and every opaque thing - characters,
     * creatures, props - keep their depth exactly.
     * DC_DECAL_LIFT is the factor on w (smaller = nearer); 1.0 disables. */
    if (!g_dcvk_room_pass && semi_mode >= 0 && semi_mode != 8) {   /* 01.10 вечер: стекло (8) — не декаль, к камере не тянем */
        static float lift = -1.f, liftcap = 240.f;
        if (lift < 0.f) { const char* e = getenv("DC_DECAL_LIFT"); lift = e ? (float)atof(e) : 0.97f;
                          e = getenv("DC_DECAL_LIFT_CAP"); if (e) liftcap = (float)atof(e); }
        if (lift < 0.9999f) {
            float* d = s_vmap + (size_t)s_nverts * VERT_FLOATS;
            /* FLAT DECALS ONLY, CAPPED (2026-08-30, player: "лазеры и круглые
             * плашки поверх всего"). The lift exists for paper-thin decals
             * lying ON a surface (blood, signs): they share the surface's
             * depth and need a nudge to win the tie. It was applied to EVERY
             * see-through prim of the game's, proportionally — so a laser
             * beam spanning the corridor or a glow right by the camera was
             * dragged tens/hundreds of units toward the eye and popped in
             * front of things it is actually behind. Two guards: a real decal
             * is FLAT in depth (its three w's are near-equal — a beam is not),
             * and the pull is capped in absolute units. */
            /* 2026-08-30 v2: the flatness gate is GONE — a sign on a side
             * wall seen at a glancing angle spans depth and the gate denied
             * it the lift, so the signs clipped into their walls again.
             * Every game semi lifts, proportional with an absolute cap. */
            for (int i = 0; i < 3; i++) {
                float w = d[i * VERT_FLOATS + 12];
                float pull = w * (1.0f - lift);
                if (pull > liftcap) pull = liftcap;
                d[i * VERT_FLOATS + 12] = w - pull;
            }
        }
    }
    {   /* Painter tiebreaker: stamp the triangle's submit order into the
         * unused color.a lane. The vertex shader nudges depth toward the
         * camera by ~2e-7 per prim — later-drawn (OT-nearer) primitives win
         * ties against coplanar earlier ones (opaque wall signs, hatches),
         * matching PS1 painter semantics, while real depth separations stay
         * orders of magnitude larger than the accumulated bias. */
        /* MEASURED, AND THE OTHER WAY ROUND FROM WHAT THIS ONCE ASSUMED.
         * Our room is appended AFTER the whole game stream - the window we
         * push into already holds the game's previous ordering table - so a
         * shared counter hands it the HIGHEST submit order in the frame, and
         * the painter nudge then makes it win every tie. That is where the
         * shadow and the blood went on the pieces carrying no z-bias. Number
         * the room below the game stream instead, keeping its own relative
         * order so a panel still beats the wall it hangs on. */
        int rseq = s_room_seq;
        if (g_dcvk_room_pass && rseq < -ROOM_SEQ_BASE - 100) s_room_seq++;
        float seq = g_dcvk_room_pass ? (float)(ROOM_SEQ_BASE + rseq)
                                     : (float)s_prim_seq++;
        /* 16.09 ЗАМЕР Z-FIGHT (порт zf_var seqmode): 1 — порядок комнаты обратный, 2 — без порядка.
         * Пиксели, которые меняются между 0 и 1, решает только порядок вывода — там полигоны
         * лежат друг на друге. По умолчанию 0 — как было. */
        if (g_dcvk_room_pass && g_dcvk_room_seq_mode == 1) seq = (float)(-100 - rseq);
        else if (g_dcvk_room_pass && g_dcvk_room_seq_mode == 2) seq = (float)ROOM_SEQ_BASE;
        float* dst = s_vmap + (size_t)s_nverts * VERT_FLOATS;
        dst[7]                  = seq;
        dst[7 + VERT_FLOATS]     = seq;
        dst[7 + 2 * VERT_FLOATS] = seq;
    }
    /* DEPTH CENSUS (DC_WPROBE=1): what w do OUR room triangles carry, and what
     * do the game's see-through primitives - the shadow blob and the blood -
     * carry at the same place on screen? If the decals sit at a noticeably
     * larger w than our floor, no amount of biasing the floor will lift them:
     * they are simply being placed further away than the surface they belong
     * to, and that is a different bug. */
    { static int wp = -1;
      if (wp < 0) { const char* e = getenv("DC_WPROBE"); wp = (e && *e == '1') ? 1 : 0; }
      if (wp) {
          static double rsum = 0, gsum = 0; static long rn = 0, gn = 0;
          static int k = 0;
          int semi = (semi_mode >= 0);
          for (int i = 0; i < 3; i++) {
              float w = v[i * VERT_FLOATS + 12];
              float yy = v[i * VERT_FLOATS + 1];
              if (!(w > 1.f) || yy < 120.f) continue;      /* lower half of the frame */
              if (g_dcvk_room_pass) { rsum += w; rn++; }
              else if (semi)        { gsum += w; gn++; }
          }
          if (((++k) % 20000) == 0 && rn && gn)
              rlog("[WPROBE] room w avg %.0f (n=%ld)   game see-through w avg %.0f (n=%ld)   ratio %.3f%c",
                   rsum / rn, rn, gsum / gn, gn, (gsum / gn) / (rsum / rn), 10);
      } }
    for (int i = 0; i < 3; i++) {
        float y = v[i * VERT_FLOATS + 1];
        if (y < s_streamMinY) s_streamMinY = y;
        if (y > s_streamMaxY) s_streamMaxY = y;
        /* Only the GAME's own vertices vote on which VRAM half to display.
         * DC double-buffers vertically and the vote reads where the stream
         * drew; the room we inject follows whatever draw offset the game
         * already set, so it can only echo the answer - and it is a bad
         * witness besides, because we draw the WHOLE room and most of it
         * falls outside the 0..512 window that counts (measured span
         * y = -1944..6040). Letting it vote produced 260 frames displaying
         * y=0 against 42 at y=256 while the draw offset alternated evenly:
         * every frame the game drew into the high half showed the low one,
         * with no room in it. That was the flicker. */
        if (!g_dcvk_room_pass && y >= 0.f && y < 512.f) {
            if (y < 248.f) s_vertsLow++; else s_vertsHigh++;
            /* ⭐ Сколько вершин ПЕРСОНАЖА пришло в этот кадр (флаг F_ACTOR = 16,
             * ставит gpu.c вокруг примитивов сборщика персонажей). По этому
             * счётчику main.cpp узнаёт кадр, в котором игра Регину прислать не
             * успела, и не показывает его — см. «мусорный кадр» ниже. */
            if (((int)v[11] & 16) && !g_dcvk_room_pass) s_actorVerts++;
        }
    }
    /* Depth test/write ON (linear-w depth in the vertex shader). The stream
     * order is NOT reliably back-to-front — pure painter order let far
     * geometry overdraw near (field: "depth broken"), so a real z-buffer is
     * required. GREATER_OR_EQUAL + equal-depth coplanar decals resolve by
     * draw order, so no z-fight. ztest reflects whether PGXP supplied w;
     * without it (2D/HUD) depth-write is off so painter order stands. */
    /* 15.09: область рисования (s_clipNow ставит dcvk_push_tri) — в каждый новый draw;
     * сливаются только draw с одинаковым клипом */
    auto clipInto = [](Draw& d) {
        d.clip = s_clipNow;
        if (s_clipNow) memcpy(d.cr, s_clipRect, sizeof d.cr);
        d.sqz = s_sqzNow;                                   /* 21.09: кандидат сжатия интерфейса */
        d.scap = s_scapNow;                                 /* 21.09 вечер: захват сцены */
        if (s_sqzNow) { d.sqc = s_sqzC; d.crx0 = s_clipRawX0; d.crx1 = s_clipRawX1; }
    };
    if (s_topNow) {                           /* 23.09: DANGER — без глубины, в верхнюю группу (ui) */
        s_draws.push_back({ s_nverts, 3, 1, 0, 0, 0, 0, 1 });
        clipInto(s_draws.back());
        if (semi_mode >= 0) {
            s_draws.push_back({ s_nverts, 3, 6 + (semi_mode & 3), 1, 0, 0, 0, 1 });
            clipInto(s_draws.back());
        }
        s_nverts += 3;
        return;
    }
    if (semi_mode < 0) {
        int kind = ztest ? 0 : 1;   // opaque: depth test+write, or no-depth
        if (!s_draws.empty()) {
            Draw& d = s_draws.back();
            /* 06.10.2026 (игрок: «у окружающих NPC пропадают полигоны, выворачиваются наизнанку»): draw меша из mods
             * (dcvk_mesh_draw — count 0, kind 0) не принимает треугольники потока — иначе следующие за ним полигоны игры
             * (Рик и Гейл рядом с Адой, слот 41) сливались в него и рисовались только как меш */
            if (d.mesh < 0 &&
                d.kind == kind && d.pass == 0 && d.first + d.count == s_nverts && d.ui == g_dcvk_ui2d &&
                d.scap == s_scapNow &&                       /* 21.09 вечер: ячейка захвата сцены — свой draw (обычно 0 == 0) */
                d.clip == s_clipNow && (!s_clipNow || memcmp(d.cr, s_clipRect, sizeof d.cr) == 0)) {
                /* 21.09: слияние — как раньше (порядок вывода ниже смотрит на габариты draw: ведущая заливка кадра,
                 * «интерфейс наверх»; отдельный draw под кандидата дал чёрный первый кадр мира после инвентаря).
                 * Смешанный draw (кандидат + не кандидат при одном клипе) клип не меняет. */
                if (d.sqz != s_sqzNow) d.sqz = 0;
                d.count += 3;
                s_nverts += 3;
                return;
            }
        }
        s_draws.push_back({ s_nverts, 3, kind, 0, g_dcvk_room_pass, g_dcvk_wire_pass, 0, g_dcvk_ui2d });
        clipInto(s_draws.back());
    } else {
        /* 01.10 вечер: СТЕКЛО (semi_mode 8, gpu_sw_renderer.c dc_glass_prim) — один проход конвейером 11 после всего
         * непрозрачного, с проверкой глубины; без глубины — как «среднее» */
        if (semi_mode == 8) {
            if (ztest && !g_dcvk_wire_pass) {             /* pass 2 — шейдеру: проход стекла (params.x) */
                s_semiLate.push_back({ s_nverts, 3, 11, 2, g_dcvk_room_pass, 0, 1, 0 });
                clipInto(s_semiLate.back());
                s_nverts += 3;
                return;
            }
            semi_mode = 0;
        }
        // two-pass split: opaque texels first (depth test+write), then
        // blended texels (depth test, no write so translucency layers).
        /* ===== TRANSLUCENT LAST FOR THE GAME STREAM (2026-08-30) ===========
         * player: "лазеры поверх Регины / Регина поверх лазеров — посмотри
         * очередь отрисовки". A world semi (laser beam, gel, glass) sits in
         * the stream wherever the game's ordering table happened to put it,
         * and whether a character drawn nearby lands before or after it is
         * luck — with a z-buffer the only correct compose is ALL opaque
         * first, then the translucents, z-TESTED against the finished depth:
         * a beam behind her fails the test and hides, a beam in front blends
         * over her. So world semis (valid depth, not our room/wire passes)
         * are deferred to the end of the frame, keeping their relative
         * order. HUD/2D semis (no depth) stay in place — menus depend on
         * paint order. DC_SEMI_LATE=0 restores the old in-stream order. */
        static int late = -1;
        if (late < 0) { const char* e = getenv("DC_SEMI_LATE"); late = (e && *e == '0') ? 0 : 1; }
        /* DC_SEMI_STAT=1: are the world semis carrying depth? (the laser
         * compose question - a zt0 semi cannot be depth-sorted at all) */
        { static int st = -1; static long z1 = 0, z0 = 0, frames = 0;
          if (st < 0) { const char* e = getenv("DC_SEMI_STAT"); st = (e && *e == '1') ? 1 : 0; }
          if (st && !g_dcvk_room_pass && !g_dcvk_wire_pass) {
              if (ztest) z1++; else z0++;
              if (++frames % 2000 == 0) {
                  const float* d = s_vmap + (size_t)s_nverts * VERT_FLOATS;
                  fprintf(stderr, "semistat: zt1=%ld zt0=%ld  last zt%d sample xy=%.0f,%.0f tp=%.0f mode=%d\n",
                          z1, z0, ztest ? 1 : 0, d[0], d[1], d[8], semi_mode);
              }
          } }
        /* 2026-08-30, player: "лучи должны перекрывать Регину, а они всегда
         * позади, даже когда физически спереди". ROOM semis (our room-live
         * lasers, glow plates) used to be excluded here, stayed in-stream
         * with room=1, and the room reorder then pulled the whole room block
         * to the FRONT of the frame - the beams drew before Regina with no
         * depth write, and her opaque pass painted over every beam,
         * including the ones between her and the camera. Defer them too,
         * keeping the room flag; the draw loop composites all late pairs
         * AFTER the world so the z-test decides for real. */
        if (late && ztest && !g_dcvk_wire_pass) {
            s_semiLate.push_back({ s_nverts, 3, 0, 0, g_dcvk_room_pass, 0, 1, 0 });
            clipInto(s_semiLate.back());
            s_semiLate.push_back({ s_nverts, 3, 2 + (semi_mode & 3), 1, g_dcvk_room_pass, 0, 1, 0 });
            clipInto(s_semiLate.back());
        } else {
            s_draws.push_back({ s_nverts, 3, ztest ? 0 : 1, 0, g_dcvk_room_pass, g_dcvk_wire_pass, 0, g_dcvk_ui2d });
            clipInto(s_draws.back());
            s_draws.push_back({ s_nverts, 3, (ztest ? 2 : 6) + (semi_mode & 3), 1, g_dcvk_room_pass, g_dcvk_wire_pass, 0, g_dcvk_ui2d });
            clipInto(s_draws.back());
        }
    }
    s_nverts += 3;
}

static int s_outW = 0, s_outH = 0;      /* active region of the last render */
/* 09.10.2026 (DC_VK_SHARED): дампы и кольцо — кадр целиком в s_rbMap, если последний шёл в общую с D3D11 текстуру
 * (s_colImg ещё держит его) */
static void dx_full_readback(void) {
    if (ptvk_lost() || !s_lastShared || !s_rbBuf || !s_colImg || !s_outW || !s_outH) return;
    struct GpuGuard { GpuGuard() { ptvk_gpu_lock(); } ~GpuGuard() { ptvk_gpu_unlock(); } } gpu_guard;
    cmdBegin();
    VkBufferImageCopy out{};
    out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    out.imageExtent = { (uint32_t)s_outW, (uint32_t)s_outH, 1 };
    vkCmdCopyImageToBuffer(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rbBuf, 1, &out);
    if (cmdSubmitWait()) s_lastShared = 0;
}
/* main.cpp: общая текстура отказала посреди кадра — кадр целиком в память (прежний путь показа) */
extern "C" void dcvk_shared_full(void) { dx_full_readback(); }
static int s_lastFlat = 0;              /* последний отданный кадр плоский (нет 3D): вывод кладёт его 4:3 */
static float s_fade_level_pub = 1.f;    /* last body-fade level (dcvk_body_fade_level) */
extern "C" int dcvk_last_flat(void) { return s_lastFlat && dc_flat_pillarbox_on(); }
/* 21.09: решение «кадр целиком 4:3» последнего отданного кадра (то же, по которому сжимался его интерфейс);
 * -1 — правило выключено (DC_ASPECT_NOLAG без бита 2), вывод решает сам, как раньше */
extern "C" int dcvk_last_flat_final(void) { return (dc_aspect_nolag() & 2) ? s_lastPb : -1; }
extern "C" int dcvk_last_mon_final(void) { return s_lastMon; }   /* 21.09 ночь: кадр — загадка на мониторе (4:3 и в режиме «16:9») */

static void dcvk_primdump_write(int drawFrom);   /* defined near dcvk_dump_last */
extern "C" volatile int  g_dcvk_primdump;        /* определены там же */
extern "C" char          g_dcvk_primdump_path[260];
/* ⭐ 17.09.2026 вечер — КАДР B: ТОТ ЖЕ КАДР С КАМЕРОЙ НА ПОЛПУТИ (игрок: «плечо как сейчас жёстко привязано к Регине, но
 * сама камера — более гладко: анимации у Регины и сам кадр игры идут мелкими рывками; дрожаний камеры при беге и повороте
 * быть не должно»). Игра обновляет кадр 30 раз в секунду: на каждые две развёртки хоста один новый поток (кадр A) и один
 * пустой (кадр B), который до сих пор показывал ту же картинку. Замер по moverec.csv игрока (бег + поворот мышью): глаз
 * камеры шагал по 61 / 341 / 515 / 529 / 407 / 206 ед. за такт — рывками в 30 Гц, а хост показывал кадры через 22 и 11 мс.
 * Теперь в кадре B треугольники прошлого кадра рисуются ещё раз со своей камерой: наша камера показанного кадра (дельта
 * Dp) и следующего (D) уже известны (конвейер игры отстаёт на такт), берётся поза посередине (поворот — доля кватерниона,
 * глаз и пивот — линейно). Вид восстанавливается из (x, y, w) так же, как в ближнем клипе; мир — жёсткий сдвиг вида, Регина
 * (F_ACTOR) едет вместе с пивотом, то есть на экране стоит на месте, как и при жёсткой привязке плеча. 2D (w = 1) не трогаем.
 * Отставания нет: кадр B показывает промежуточную позу между двумя уже посчитанными. DC_FRAME_WARP=0 — как было,
 * DC_FRAME_WARP_ALPHA — доля пути (0.5), DC_WARP_DBG=1 — строка на кадр. */
struct WarpSnap {
    int valid = 0, used = 1, world = 0, nverts = 0;
    int dropped = 0;                       /* 10.10 (кадры с частотой монитора): кадр A удержан - его снимок не показывать */
    int dispX = 0, dispY = 0, dispW = 0, dispH = 0, w = 0, h = 0;
    float pc[8]{}, pcp[8]{}, bgpc[8]{};
    float ox = 0.f, oy = 0.f, H = 0.f, Hs = 0.f;
    std::vector<Draw> draws;
    std::vector<size_t> order;
    std::vector<Scap> scaps;               /* 21.09 вечер: захваты сцены этого кадра */
    unsigned long long frame = 0;
    int vcopy = 0;                         /* 25.10: вершины кадра скопированы в s_wsVb / s_wsRv */
    std::vector<MeshRef> mrefs;            /* 25.10: меши его draw (Draw::mref) */
    int pairs = 0;                         /* 26.10: пары кусков собраны (s_pairSet, pair_build) */
};
static WarpSnap s_wsnap;
/* 26.10.2026: пары кусков (gte.cpp - учёт проекций по проходам; сборка - pair_build у warp_pc_make) */
extern "C" int g_dc_pair_track, g_dc_dma_pass, g_dc_pass_id;
extern "C" int dc_pair_pass_get(int pass, int* nproj);
extern "C" int dc_pair_run_get(int pass, int i, uint32_t* key, int* n, double M[9], double t[3]);
extern "C" int dc_pair_vetoed(int passA);   /* gte.cpp: у кадров между тактами A -> A+1 пар нет (pt_renderer.c, смена ракурса в сцене) */
static int pair_build(int passA, unsigned long long frame);
/* ⭐ 25.10.2026 - КОПИЯ ВЕРШИН КАДРА A В ПАМЯТИ ВИДЕОКАРТЫ (поток показа, main.cpp DC_PRESENT_THREAD). Кадры между тактами
 * перерисовывают треугольники кадра A, а поток s_vbuf / s_rvBuf эмуляция следующего такта пишет с нуля. Здесь в конце кадра A
 * видеокарта копирует вершины кадра (s_nverts) в свои буферы - кадры между тактами (и кадр B) берут их оттуда, пока эмуляция
 * пишет новый поток. Включает main.cpp (dcvk_warp_copy_on); не вышло выделить - кадры между тактами остаются в главном потоке. */
static VkBuffer       s_wsVb = VK_NULL_HANDLE, s_wsRv = VK_NULL_HANDLE;
static VkDeviceMemory s_wsVbMem = VK_NULL_HANDLE, s_wsRvMem = VK_NULL_HANDLE;
static VkBuffer       s_wsPv = VK_NULL_HANDLE;          /* 26.10: и метки кусков кадра A */
static VkDeviceMemory s_wsPvMem = VK_NULL_HANDLE;
static int s_wsCopyOn = 0, s_wsCopyState = 0;      /* состояние: 0 - не создавали, 1 - есть, -1 - не вышло */
extern "C" void dcvk_warp_copy_on(int on) { s_wsCopyOn = on ? 1 : 0; }
extern "C" int  dcvk_warp_copy_state(void) { return s_wsCopyState; }   /* 25.10: main.cpp pt_on - -1: копии нет, поток показа не включать */
static bool ws_copy_ready(void) {                  /* под замком видеокарты */
    if (s_wsCopyState == 0) {
        const VkBufferUsageFlags u = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const bool ok = makeBuf((VkDeviceSize)MAX_VERTS * VERT_FLOATS * sizeof(float), u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                &s_wsVb, &s_wsVbMem, nullptr) &&
                        makeBuf((VkDeviceSize)MAX_VERTS * 4 * sizeof(float), u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                &s_wsRv, &s_wsRvMem, nullptr) &&
                        makeBuf((VkDeviceSize)MAX_VERTS * sizeof(uint32_t), u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                &s_wsPv, &s_wsPvMem, nullptr);
        s_wsCopyState = ok ? 1 : -1;
        rlog("[VKR] frame A vertex copy for the present thread: %s\n", ok ? "on" : "failed, extras stay on the main thread");
    }
    return s_wsCopyState > 0;
}

/* ===== 09.10.2026 (игрок: «максимально оптимизировать, качество не должно падать») — РАМКА ЗАСЛОНИВШИХ НА КАРТЕ ЛАМПЫ =====
 * На слое карты теней — только персонажи (dc_shadow.vert), остальное — глубина 1 (пусто). Ужатие луны (3 прохода 1024²) и
 * пирамиды ламп (9 уровней) шли по всей карте, хотя вне персонажей их выход — те же нули. Здесь рамка заслонивших на каждом
 * слое — те же вершины, что у прохода карты, та же выкладка, что в dc_shadow.vert, плюс запас. Проходы луны и пирамид рисуют
 * только в своей рамке (с запасом на окно размытия и фильтра), остальное цели заливается при начале прохода ровно тем, что
 * проход дал бы на пустом месте: картинка та же. Персонаж из mods (его тень рисует свой конвейер) или вершина за плоскостью
 * лампы — весь слой, как было. DC_SHADOW_RECT=0 — как было. */
static int s_shRectOn = -1;
static int sh_rect_on(void) {
    if (s_shRectOn < 0) { const char* e = getenv("DC_SHADOW_RECT"); s_shRectOn = (e && *e == '0') ? 0 : 1;
                          std::fprintf(stderr, "shadow rect: moon and pyramid passes only around the characters %s (DC_SHADOW_RECT)%c",
                                       s_shRectOn ? "on" : "off", 10); }
    return s_shRectOn;
}
extern "C" int  dcvk_shadow_rect(void)         { return sh_rect_on(); }
extern "C" void dcvk_shadow_rect_set(int on)   { s_shRectOn = on ? 1 : 0; }   /* порт post_set "shrect" — A/B в одном запуске */
static void sh_occ_rects(int nShx) {
    (void)nShx;                                           /* заслонившие кадра и списка shx — в s_occCpu (occ_note) */
    for (int li = 0; li < SH_MAX; li++) s_shrSt[li] = SHR_FULL;
    if (!sh_rect_on() || !s_shUboMap || s_shSize <= 0) return;
    for (const Draw& dm : s_draws) if (dm.mesh >= 0 && dm.mocc) return;   /* меш из mods: его тень рисует свой конвейер */
    const int nL = s_shN < SH_MAX ? s_shN : SH_MAX;
    float mn[SH_MAX][2] = {}, mx[SH_MAX][2] = {};
    int any[SH_MAX] = {}, bad[SH_MAX] = {};
    const float S = (float)s_shSize;
    const int nv = (int)(s_occCpu.size() / 5);
    /* матрицы ламп — копией: блок ShadowUBO тоже в памяти видеокарты, поэлементное чтение из него стоило 13 мс на кадр */
    float Mv[SH_MAX][16];
    memcpy(Mv, s_shUboMap, sizeof Mv);
    for (int t = 0; t + 2 < nv; t += 3) {                 /* треугольники, в которых есть вершина заслонившего */
        const float* tv[3] = { s_occCpu.data() + (size_t)t * 5, s_occCpu.data() + (size_t)(t + 1) * 5,
                               s_occCpu.data() + (size_t)(t + 2) * 5 };
        int ok[3], np = 0;
        for (int k = 0; k < 3; k++) {                    /* dc_shadow.vert: персонаж, не полупрозрачный, w >= 1.5 (или точка вида) */
            const float* v = tv[k];                       /* x, y, u, w, флаги */
            const int fl = (int)lrintf(v[4]);
            const bool vsp = v[2] < -500.f;
            ok[k] = ((fl & (16 | 8388608)) != 0 && (fl & 2) == 0 && (vsp || v[3] >= 1.5f)) ? 1 : 0;
            np += ok[k];
        }
        if (!np) continue;
        for (int k = 0; k < 3; k++) {
            float p[3] = { 0.f, 0.f, 0.f };
            if (ok[k]) {
                const float* v = tv[k];
                const float w = v[3] > 1e-6f ? v[3] : 1e-6f;
                if (v[2] < -500.f) { p[0] = v[0]; p[1] = v[1]; p[2] = v[3]; }
                else { p[0] = (v[0] - s_shWp[0]) * w / s_shWp[3]; p[1] = (v[1] - s_shWp[1]) * w / s_shWp[2]; p[2] = w; }
            }
            for (int li = 0; li < nL; li++) {
                float fx = 0.5f * S, fy = 0.5f * S;      /* вершина за ближней плоскостью: обрезка тянет край к центру карты */
                if (ok[k]) {
                    const float* M = Mv[li];                   /* vp[li], столбцами */
                    float c[4];
                    for (int r = 0; r < 4; r++) c[r] = M[0 * 4 + r] * p[0] + M[1 * 4 + r] * p[1] + M[2 * 4 + r] * p[2] + M[3 * 4 + r];
                    if (c[3] <= 1e-6f) { bad[li] = 1; continue; }
                    fx = (c[0] / c[3] * 0.5f + 0.5f) * S; fy = (c[1] / c[3] * 0.5f + 0.5f) * S;
                }
                if (!any[li]) { mn[li][0] = mx[li][0] = fx; mn[li][1] = mx[li][1] = fy; any[li] = 1; }
                else {
                    if (fx < mn[li][0]) mn[li][0] = fx; if (fx > mx[li][0]) mx[li][0] = fx;
                    if (fy < mn[li][1]) mn[li][1] = fy; if (fy > mx[li][1]) mx[li][1] = fy;
                }
            }
        }
    }
    for (int li = 0; li < nL; li++) {
        if (bad[li]) continue;                                    /* за плоскостью лампы — весь слой */
        if (!any[li]) { s_shrSt[li] = SHR_EMPTY; continue; }
        if (mn[li][0] < -1e6f || mx[li][0] > 1e6f || mn[li][1] < -1e6f || mx[li][1] > 1e6f) continue;
        int x0 = (int)floorf(mn[li][0]) - 2, y0 = (int)floorf(mn[li][1]) - 2;
        int x1 = (int)ceilf(mx[li][0]) + 3, y1 = (int)ceilf(mx[li][1]) + 3;
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > s_shSize) x1 = s_shSize; if (y1 > s_shSize) y1 = s_shSize;
        if (x1 <= x0 || y1 <= y0) { s_shrSt[li] = SHR_EMPTY; continue; }
        s_shrRc[li][0] = x0; s_shrRc[li][1] = y0; s_shrRc[li][2] = x1; s_shrRc[li][3] = y1;
        s_shrSt[li] = SHR_RECT;
    }
    { static int lg = -1; static long n = 0;              /* DC_SHADOW_RECT_LOG=1 — рамки раз в 60 кадров */
      if (lg < 0) { const char* e = getenv("DC_SHADOW_RECT_LOG"); lg = (e && *e == '1') ? 1 : 0; }
      if (lg && (++n % 60) == 1)
          for (int li = 0; li < nL; li++)
              std::fprintf(stderr, "shadow rect: layer %d %s %d,%d..%d,%d (map %d)%c", li,
                           s_shrSt[li] == SHR_RECT ? "frame" : s_shrSt[li] == SHR_EMPTY ? "empty" : "whole",
                           s_shrRc[li][0], s_shrRc[li][1], s_shrRc[li][2], s_shrRc[li][3], s_shSize, 10); }
}
/* 10.10.2026 (DC_HZ, кадры с частотой монитора): кадр A рисуется с камерой на момент своего показа - доля пути Dp -> D
 * задаётся из main.cpp перед dcvk_render и действует на один кадр (как у кадра B: основной проход, свечение, отражения,
 * объёмный свет). 0 - как раньше, без поправки. */
static float s_frame_alpha = 0.f, s_frame_alpha_used = 0.f;
extern "C" void dcvk_set_frame_alpha(float a) { s_frame_alpha = (a > 0.f && a < 1.f) ? a : 0.f; }
extern "C" float dcvk_frame_alpha_used(void) { return s_frame_alpha_used; }   /* какую долю кадр A применил на деле */
/* ⭐ 25.10.2026 - ВХОДЫ ПЕРЕПРОЕЦИРОВАНИЯ ОДНИМ СНИМКОМ (игрок: «процессор и видеокарта одновременно»). Камеры Dp и D, её
 * курс, прицел, кости рук - состояние игры (memory.c, pt_anim.c, pt_renderer.c), которое эмуляция следующего такта
 * перезаписывает. Поток показа (main.cpp DC_PRESENT_THREAD) рисует кадры между тактами вместе с эмуляцией - он берёт эти
 * значения из снимка на момент показа кадра такта (dcvk_warp_pub). Главный поток снимает их прямо перед расчётом, как было. */
struct WarpIn {
    int cams; float Nr[9], er[3], tr[3], Nn[9], en3[3], tn[3]; unsigned long long fr, fn;   /* dc_nots_warp_cams */
    int aim, tank, strafe, turn; float dcam, dher;           /* прицел, Tank / RE4, стрейф, её поворот (dc_nots_warp_turn) */
    int amode, pA, pN, dhA; float dA[9], tA[3];             /* руки: режим, проходы, переприцел прохода A */
    int dhN; float dNd[9], tNd[3];                          /* 26.10: переприцел прохода N (корпус между тактами) */
    unsigned okA, okN; float rA[16][9], tAb[16][3], rNb[16][9], tNb[16][3];   /* кости 0..15 проходов A и N (биты - есть; 25.10: все) */
    int okW; float rw[9], tw[3];                            /* корпус прохода A «миром» (строка armwarp) */
};
static void warp_in_take(WarpIn* w);
static int warp_pc_make(float alpha, float wpc[16], int* stand, int dbg, double* o_ang, double* o_de, double* o_dt,
                        unsigned long long* o_fr, unsigned long long* o_fn, const WarpIn* in = nullptr);
/* ⭐ 12.10.2026 — БЛЕСК ПРЕДМЕТОВ (игрок: «раз в 5 секунд волной подсвечивает предметы, как в Resident Evil»; опция GAME ->
 * Item Highlight, main.cpp g_dc_item_glint). Блок теней 456..459 (dc_raster.frag sh.glint): x сила (0 - полосы нет), y где полоса
 * вдоль диагонали предмета (0..1; до и после прохода - за краем), z общий отсвет за проход (0..1..0), w полуширина полосы. Период
 * DC_GLINT_PERIOD (10 с; 12.10 игрок: «раз в 10 секунд вместо раз в 5»), проход DC_GLINT_SWEEP (0.9 с), сила DC_GLINT_STR (0.45),
 * полуширина DC_GLINT_WIDTH (0.12). Какие вершины -
 * предметы, решает pt_renderer.c (rl_glint_*). Пишется и кадром A, и кадром B (60 кадров - полоса идёт плавно). */
extern "C" int g_dc_item_glint;
static void glint_ubo_update(void) {
    if (!s_shUboMap) return;
    static float per = -1.f, dur = 0.9f, str = 0.45f, wid = 0.12f;
    if (per < 0.f) {
        const char* e = getenv("DC_GLINT_PERIOD"); per = e ? (float)atof(e) : 10.0f; if (per < 0.5f) per = 0.5f;
        e = getenv("DC_GLINT_SWEEP"); if (e) dur = (float)atof(e); if (dur < 0.1f) dur = 0.1f; if (dur > per) dur = per;
        e = getenv("DC_GLINT_STR"); if (e) str = (float)atof(e);
        e = getenv("DC_GLINT_WIDTH"); if (e) wid = (float)atof(e); if (wid < 0.02f) wid = 0.02f;
    }
    float x = 0.f, y = -10.f, z = 0.f;
    if (g_dc_item_glint && str > 0.f) {
        const double t = (double)(GetTickCount64() % 86400000ull) * 0.001;
        const double ph = fmod(t, (double)per);
        if (ph < (double)dur) {
            const float u = (float)(ph / (double)dur);
            x = str; y = -2.f * wid + (1.f + 4.f * wid) * u; z = sinf(3.14159265f * u);
        }
    }
    s_shUboMap[456] = x; s_shUboMap[457] = y; s_shUboMap[458] = z; s_shUboMap[459] = wid;
}
extern "C" const unsigned int* dcvk_render(int dispX, int dispY,
                                           int dispW, int dispH, int scale) {
    if (ptvk_lost()) return nullptr;    /* 18.10: видеокарта потеряна - кадр родным путём (ptvk_wait_fence) */
    /* whole render (record + submit + fence + readback) under the GPU lock:
     * the PT trace thread shares the queue and the VRAM image/staging. */
    struct GpuGuard { GpuGuard() { ptvk_gpu_lock(); } ~GpuGuard() { ptvk_gpu_unlock(); } } gpu_guard;
    /* translucent-last: append the deferred world semis now that every
     * opaque draw of the frame is in the list (see dcvk_push_tri_raw) */
    if (!s_semiLate.empty()) {
        s_draws.insert(s_draws.end(), s_semiLate.begin(), s_semiLate.end());
        s_semiLate.clear();
    }
    if (!raster_init_impl()) return nullptr;
    int w = dcvk_target_w(dispW, scale), h = dispH * scale;   /* 10.10: ширина по форме картинки */
    if (w <= 0 || h <= 0) return nullptr;
    if (!ensureTargets(w, h)) return nullptr;
    pc_tick();                                                 /* 11.10: новые конвейеры - в файл кэша, в фоне */

    static LARGE_INTEGER s_qpf = { 0 };
    LARGE_INTEGER t0, t1;
    if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&t0);

    /* ⭐ ЛОВУШКА ПРОВАЛА ВЕРШИН (09.09.2026, «мусорный кадр на дверях»).
     * Мерой кольца видно, что в такой кадр приходит заметно МЕНЬШЕ вершин,
     * чем в соседние (2340 против 2658 — у Регины пропадает рука). Ловим это
     * прямо здесь: если поток кадра меньше DC_VERTDROP процентов от медианы
     * последних восьми, печатаем строку «vertdrop:» и одноразово сбрасываем
     * его состав в primdump-CSV — по нему видно, каких именно примитивов не
     * хватает. DC_VERTDROP=0 — выключить (по умолчанию выключено). */
    {
        /* Медиана вершин за восемь ПОКАЗАННЫХ кадров — опора для признака
         * «кадр недособран» (dcvk_vert_drop выше). */
        static int hist2[8] = { 0 }, hn2 = 0;
        hist2[hn2 % 8] = s_nverts; hn2++;
        if (hn2 >= 8) {
            int t[8]; for (int i = 0; i < 8; i++) t[i] = hist2[i];
            for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++)
                if (t[b] < t[a]) { int s = t[a]; t[a] = t[b]; t[b] = s; }
            s_vertMed = t[4];
        }
    }
    {
        static int thr = -1;
        if (thr < 0) { const char* e = getenv("DC_VERTDROP"); thr = e ? atoi(e) : 0; }
        if (thr > 0) {
            static int hist[8] = { 0 }, hn = 0, dumped = 0;
            int med = 0;
            if (hn >= 8) {
                int t[8]; for (int i = 0; i < 8; i++) t[i] = hist[i];
                for (int a = 0; a < 8; a++) for (int b = a + 1; b < 8; b++)
                    if (t[b] < t[a]) { int s = t[a]; t[a] = t[b]; t[b] = s; }
                med = t[4];
            }
            static int pend = 0;
            if (pend) {           /* следующий кадр после провала — для сравнения */
                pend = 0;
                snprintf(g_dcvk_primdump_path, sizeof g_dcvk_primdump_path,
                         "vertdrop_%d_next.csv", dumped);
                g_dcvk_primdump = 1;
            } else if (med > 600 && s_nverts * 100 < med * thr) {
                rlog("[VKR] vertdrop: frame %d vertices at median %d (%d%%), submits=%d\n",
                     s_nverts, med, med ? s_nverts * 100 / med : 0, s_submits);
                if (dumped < 6) {
                    dumped++;
                    snprintf(g_dcvk_primdump_path, sizeof g_dcvk_primdump_path,
                             "vertdrop_%d.csv", dumped);
                    g_dcvk_primdump = 1;
                    pend = 1;
                }
            }
            hist[hn % 8] = s_nverts; hn++;
        }
    }
    /* Mixed-frame guard (2026-08-16): >= 2 DrawOTag submissions accumulated
     * between presents (camera cut: last OLD-view frame + first NEW-view
     * frame, no flip in between). Rendering both mixed two views into one
     * torn image — draw only the TAIL (newest submission); its half-counts
     * drive the view vote below. */
    /* A normal frame is a tiny lead-in LL (~6 verts) plus the world LL —
     * that pair must render TOGETHER. Only when the dropped head would be
     * SUBSTANTIAL (>= 600 verts: a whole stale world frame from before the
     * camera cut) is the head discarded. */
    int drawFrom = 0;
    int voteLow = s_vertsLow, voteHigh = s_vertsHigh;
    if (s_submits >= 2) {
        static int tail = -1;
        if (tail < 0) { const char* e = getenv("DC_MIXED_TAIL"); tail = (e && *e == '0') ? 0 : 1; }
        /* Ищем последнюю сборку, ПОСЛЕ которой нарисовано хотя бы 600 вершин:
         * это начало самого свежего настоящего кадра мира. Мелкие сборки в
         * конце (HUD, плашка комнаты) отметку не сдвигают. */
        int mi = -1;
        /* 06.10.2026 (ten raptors in ST10D): if our room walk lands in the stream before the game's first submission it
         * is no stale frame of the game - the head that may be dropped is what the GAME submitted before the mark,
         * counted from the first submission. Opt-in (DC_MIXED_HEAD_GAME=1): with main.cpp DC_WALK_WAIT the walk comes
         * after the submissions as in a light frame. */
        static int headGame = -1;
        if (headGame < 0) { const char* e = getenv("DC_MIXED_HEAD_GAME"); headGame = (e && *e == '1') ? 1 : 0; }   /* opt-in */
        const int head0 = (headGame && s_markCount > 0) ? s_marks[0].verts : 0;
        if (tail) {
            for (int i = s_markCount - 1; i >= 0; i--)
                if (s_marks[i].verts - head0 >= 600 && s_nverts - s_marks[i].verts >= 600) { mi = i; break; }
        }
        if (mi >= 0) {
            drawFrom = s_marks[mi].draws;
            voteLow  = s_vertsLow  - s_marks[mi].low;
            voteHigh = s_vertsHigh - s_marks[mi].high;
            rlog("[VKR] mixed stream: %d submissions, tail-only from draw %d "
                 "(verts %d..%d, mark %d of %d)\n", s_submits, drawFrom,
                 s_marks[mi].verts, s_nverts, mi, s_markCount);
        } else if (!tail && s_submitMarkVerts >= 600 && s_submitMarkVerts < s_nverts) {
            drawFrom = s_submitMarkDraws;
            voteLow  = s_vertsLow  - s_submitMarkLow;
            voteHigh = s_vertsHigh - s_submitMarkHigh;
            rlog("[VKR] mixed stream: %d submissions, tail-only from draw %d "
                 "(verts %d..%d)\n", s_submits, drawFrom, s_submitMarkVerts, s_nverts);
        }
    }
    dcvk_primdump_write(drawFrom);   /* one-shot forensic CSV if armed */
    { /* ВРЕМЕННО (22.09): состав каждого отрисованного кадра — DC_FRAME_DBG=1 */
      static int fdbg = -1; static long nfd = 0;
      if (fdbg < 0) { const char* e = getenv("DC_FRAME_DBG"); fdbg = (e && *e == '1') ? 1 : 0; }
      if (fdbg && ++nfd <= 400) {
          int vg = 0, vr = 0, dg = 0, dr = 0; double cg = 0, cr = 0;
          for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++) {
              const Draw& d = s_draws[i];
              double sum = 0;
              for (int v = 0; v < d.count; v++) {
                  const float* p = s_vmap + (size_t)(d.first + v) * VERT_FLOATS;
                  sum += (p[4] + p[5] + p[6]) / 3.0;
              }
              if (d.room) { dr++; vr += d.count; cr += sum; } else { dg++; vg += d.count; cg += sum; }
          }
          std::fprintf(stderr, "framedbg: submits=%d marks=%d drawFrom=%d draws=%d | game draws=%d verts=%d color=%.1f | room draws=%d verts=%d color=%.1f%c",
                       s_submits, s_markCount, drawFrom, (int)s_draws.size(),
                       dg, vg, vg ? cg / vg : -1.0, dr, vr, vr ? cr / vr : -1.0, 10);
      } }

    /* Double-buffer alignment: the game flips DISPLAY at frame START, so
     * at flip time the accumulated stream belongs to the OTHER VRAM half.
     * Trust the stream itself: the view base is wherever the majority of
     * its (in-VRAM) vertices actually drew. */
    /* The back buffer (where the stream drew) holds the fully-composed
     * current frame at present time — the CPU wrote every fill, VRAM copy
     * and primitive into it. Render into it and sample it for the
     * background, so uploaded/copied content (menu image, HUD panels) that
     * the prim stream never carries still appears. */
    /* Only for the double-buffered 240p layout (buffers at y=0/256). Full-
     * height modes (480i status/item screens, splashes) have ONE frame at the
     * game's own dispY — snapping them to 0/256 scrolled the image by half a
     * screen with wrap-around (field 2026-08-09: item screen split, main menu
     * fine because its stream is empty and the snap never armed). */
    /* WHICH VRAM HALF THIS FRAME LIVES IN (2026-08-25, two bugs deep).
     * This used to be a VOTE: dispY = (voteHigh > voteLow) ? 256 : 0, counting
     * the GAME's own submitted vertices per half. It is a good witness only
     * while the game is actually drawing the room. With the boom collision-
     * clamped hard against Regina (measured arm=182 of a 1500 rest length) the
     * engine culls nearly everything and submits ~12 vertices in total: the
     * vote turns into a coin flip and the picture alternates between the fresh
     * back buffer and the still-displayed front one - "камера дрожит,
     * переключается на другой вид, чередует их", and the reason switching
     * camera collision off made it go away.
     * Forcing the deterministic back buffer instead (the half the game is NOT
     * displaying) stopped the alternation but rendered long runs of BLACK
     * frames: measured 27 in a row with 4161 stream vertices present, because
     * dispY is also the CLIP WINDOW (cy0/cy1 below), so naming the wrong half
     * throws our own geometry away entirely.
     * Take it from the source of truth instead. GP0 0xE5 sets the game's
     * drawing origin, and the room we inject is placed through that very
     * offset (see the near-clip path, gpu_get_draw_offset above), so window
     * and geometry cannot disagree by construction - no thresholds, no votes,
     * nothing to tune. The vote stays as the fallback for anything that is not
     * a clean 0/256 double-buffered layout.
     * DC_HALF_SRC=vote restores the bare vote, =back the back-buffer attempt;
     * DC_HALF_DBG=1 logs every frame (fields: dry = draw offset at submission,
     * drynow = at present, sy = the stream's Y span). */
    if ((voteLow || voteHigh) && dispH <= 256) {
        static int halfsrc = -1, halfdbg = 0;
        if (halfsrc < 0) {
            const char* e = getenv("DC_HALF_SRC");
            halfsrc = 2;                                   /* draw offset */
            if (e && (*e == 'v' || *e == 'V')) halfsrc = 0;
            else if (e && (*e == 'b' || *e == 'B')) halfsrc = 1;
            e = getenv("DC_HALF_DBG"); halfdbg = (e && *e == '1') ? 1 : 0;
        }
        const int dispYin = dispY;
        const int voteTot = voteLow + voteHigh;
        const int voteMar = (voteHigh > voteLow) ? (voteHigh - voteLow) : (voteLow - voteHigh);
        const int voted   = (voteHigh > voteLow) ? 256 : 0;
        int doxNow = 0, doyNow = 0;
        gpu_get_draw_offset(&doxNow, &doyNow);
        int chosen = voted;
        const char* how = "vote";
        if (halfsrc == 2) {
            /* the offset our own room geometry was placed through */
            if      (doyNow >= -8  && doyNow <= 8)   { chosen = 0;   how = "draw"; }
            else if (doyNow >= 248 && doyNow <= 264) { chosen = 256; how = "draw"; }
        } else if (halfsrc == 1 && (dispYin == 0 || dispYin == 256)) {
            const int back = (dispYin >= 256) ? 0 : 256;
            if (voteTot < 64 || voteMar * 4 < voteTot) { chosen = back; how = "back"; }
        }
        if (halfdbg)
            fprintf(stderr,
                    "half: in=%d low=%d high=%d tot=%d mar=%d dry=%d drynow=%d "
                    "sy=%.0f..%.0f vote=%d -> %d %s | subs=%d mark=%d nv=%d "
                    "draws=%d from=%d%c",
                    dispYin, voteLow, voteHigh, voteTot, voteMar,
                    s_submitDrawY, doyNow, s_streamMinY, s_streamMaxY,
                    voted, chosen, how, s_submits, s_submitMarkVerts, s_nverts,
                    (int)s_draws.size(), drawFrom, 10);
        dispY = chosen;
    }

    /* FLAT SCREEN: no depth-tested primitive in this frame and the game's own
     * screen byte says "not the world" -> the interface squeeze applied to
     * everything (see dc_ui_squeeze_tri), so paint the uncovered sides of
     * the display column black, on top of the whole stream. Remember the
     * frame's flatness for the next frame's push-time decisions. */
    {
        int had3d = 0;
        for (size_t di = (size_t)drawFrom; di < s_draws.size() && !had3d; di++) {
            int k = s_draws[di].kind;
            if (k == 0 || (k >= 2 && k < 6)) had3d = 1;
        }
        static int sq_env = -1;
        if (sq_env < 0) {
            const char* e = ::getenv("DC_UI_SQUEEZE");
            sq_env = (e && *e == '0') ? 0 : 1;
        }
        const int sq_en = sq_env && dc_ui43_squeeze();   /* 15.09: только режим 1 соотношения (меню SCREEN) */
        /* content only - see dc_flat_screen_hint (the title menu carries the
         * world's screen byte) */
        const int copyflat = dc_copyflat_now();                           /* 14.09: копия кадра вне мира */
        int flat = sq_en && (!had3d || copyflat) && dispW >= 64 && dc_game_loaded();   /* BIOS: frame is 4:3 already */
        /* полосы — только на прежнем пути сжатия; с пилларбоксом кадра бока
         * даёт сам вывод */
        if (flat && !dc_flat_pillarbox_on()) {
            const float x0 = (float)dispX, x1 = (float)dispX + (float)dispW;
            const float bw = (float)dispW * (1.f - dc_ws_sq()) * 0.5f;   /* 10.10: 16:9 - 1/8, как было */
            const float y0 = (float)dispY, y1 = (float)dispY + (float)dispH;
            float bar[2][4] = { { x0, x0 + bw, y0, y1 }, { x1 - bw, x1, y0, y1 } };
            for (int b = 0; b < 2; b++) {
                float v[3 * VERT_FLOATS];
                auto put = [&](int i, float x, float y) {
                    float* o = v + i * VERT_FLOATS;
                    o[0] = x; o[1] = y; o[2] = 0.f; o[3] = 0.f;
                    o[4] = 0.f; o[5] = 0.f; o[6] = 0.f; o[7] = 0.f;
                    o[8] = 0.f; o[9] = 0.f; o[10] = 0.f; o[11] = 0.f; o[12] = 1.f;
                };
                put(0, bar[b][0], bar[b][2]); put(1, bar[b][1], bar[b][2]); put(2, bar[b][0], bar[b][3]);
                dcvk_push_tri_raw(v, -1, 0);
                put(0, bar[b][1], bar[b][2]); put(1, bar[b][1], bar[b][3]); put(2, bar[b][0], bar[b][3]);
                dcvk_push_tri_raw(v, -1, 0);
            }
        }
        /* 21.09 вечер: ОДНО решение «кадр целиком 4:3» на растр и на вывод (раньше вывод считал своё — с гистерезисом
         * по показам и по яркости ПРОШЛОГО кадра, а сжатие примитивов шло по догадке прошлого кадра; на первом кадре
         * после смены экрана они расходились: интерфейс растянут / мир в 4:3). Правило то же, что было в
         * dc_frame_rect: известные плоские экраны (байт экрана 1, 5, 8) — сразу, прочие — со второго плоского кадра
         * подряд. Кандидаты сжатия приводятся к этому решению до записи кадра. */
        {
            static int pb_run = 0;
            pb_run = flat ? (pb_run < 1000 ? pb_run + 1 : pb_run) : 0;
            uint8_t* rpb = memory_get_ram_ptr();
            const int scrpb = rpb ? rpb[0xB7878] : 3;
            static int dsw = -1;
            if (dsw < 0) { const char* e = ::getenv("DC_DOORSCENE_WIDE"); dsw = (e && *e == '1') ? 1 : 0; }
            s_pb_frame = flat && dc_flat_pillarbox_on() && !(dsw && scrpb == 6)
                      && (scrpb == 1 || scrpb == 5 || scrpb == 8 || pb_run >= 2);
            {   /* 21.09 ночь: загадка на мониторе — кольцо рамки (все четыре стороны) в рисуемой части потока -> кадр целиком 4:3 */
                const int vfrom = (drawFrom > 0 && drawFrom < (int)s_draws.size()) ? s_draws[drawFrom].first : 0;
                int tiles = 0, nt = 0, nb = 0, nl = 0, nr = 0;
                for (const M43Tile& t : s_monTiles) {
                    if (t.vi < vfrom) continue;
                    tiles++; if (t.side & 1) nt++; if (t.side & 2) nb++; if (t.side & 4) nl++; if (t.side & 8) nr++;
                }
                s_mon_frame = dc_mon43_env() && dc_ws_world() && dc_flat_pillarbox_on() && tiles >= dc_mon43_tiles_min()
                           && nt >= 4 && nb >= 4 && nl >= 4 && nr >= 4;
                /* 01.10 вечер: кольцо рамки само по себе, в любом режиме соотношения (порядок потока загадки, см. monStream) */
                s_mon_ring = dc_mon43_env() && tiles >= dc_mon43_tiles_min() && nt >= 4 && nb >= 4 && nl >= 4 && nr >= 4;
                if (s_mon_frame) {
                    s_pb_frame = 1;
                    for (const M43Cand& c : s_m43Cands) {
                        if (c.vi < 0 || c.vi + 3 > s_nverts) continue;
                        float* d = s_vmap + (size_t)c.vi * VERT_FLOATS;
                        for (int i = 0; i < 3; i++) d[i * VERT_FLOATS] = c.altx[i];
                    }
                    /* 01.10 вечер (слот 29): область рисования у 3D ужата под 16:9 (clipSet(1, …)), а вершины растянуты обратно — ножницы
                     * срезали края загадки (детали стабилизатора у правого края). Края закрывает рамка монитора, вывод 4:3: у 3D-draw
                     * этого кадра ножниц нет (DC_MONITOR_STREAM=0 — как было). */
                    if (dc_monitor_stream_on())
                        for (size_t di = (size_t)drawFrom; di < s_draws.size(); di++) {
                            Draw& dd = s_draws[di];
                            const int k = dd.kind;
                            if (dd.clip && !dd.room && (k == 0 || (k >= 2 && k <= 5))) dd.clip = 0;
                        }
                }
                static int lastMon = -1;
                if (s_mon_frame != lastMon) { lastMon = s_mon_frame;
                    std::fprintf(stderr, "raster: monitor puzzle frame %s (border tiles 16x16: %d triangles, top/bottom/left/right %d/%d/%d/%d; 3D stretched back: %d triangles) - DC_MONITOR_43=0 off%c",
                                 s_mon_frame ? "ON - the whole frame 4:3 like the console" : "off", tiles, nt, nb, nl, nr,
                                 s_mon_frame ? (int)s_m43Cands.size() : 0, 10); }
            }
            int flipped = 0, ncand = (int)s_sqzCands.size();
            if ((dc_aspect_nolag() & 2) && dc_flat_pillarbox_on()) {
                const int want_sq = s_pb_frame ? 0 : 1;
                if (dc_aspdbg()) { static int cdl_w = 0, cdl_f = 0;      /* замер: что за кандидаты (мир / плоский экран) */
                    int k = 0; int& cdl = flat ? cdl_f : cdl_w;
                    for (SqzCand& c : s_sqzCands) {
                        if (cdl >= (flat ? 12 : 60) || k >= 4 || c.vi < 0 || c.vi + 3 > s_nverts) break;
                        const float* d = s_vmap + (size_t)c.vi * VERT_FLOATS;
                        std::fprintf(stderr, "aspdbg: cand vi=%d sq=%d want=%d xy=(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) alt=%.1f,%.1f,%.1f rgb=%.0f,%.0f,%.0f tp=%.0f fl=%.0f%c",
                                     c.vi, c.sq_now, want_sq, d[0], d[1], d[VERT_FLOATS], d[VERT_FLOATS + 1], d[2 * VERT_FLOATS], d[2 * VERT_FLOATS + 1],
                                     c.altx[0], c.altx[1], c.altx[2], d[4], d[5], d[6], d[8], d[11], 10);
                        k++; cdl++;
                    } }
                for (SqzCand& c : s_sqzCands) {
                    if (c.sq_now == want_sq || c.vi < 0 || c.vi + 3 > s_nverts) continue;
                    float* d = s_vmap + (size_t)c.vi * VERT_FLOATS;
                    for (int i = 0; i < 3; i++) { float t = d[i * VERT_FLOATS]; d[i * VERT_FLOATS] = c.altx[i]; c.altx[i] = t; }
                    c.sq_now = want_sq; flipped++;
                }
                for (Draw& d : s_draws) {
                    if (!d.sqz || (d.sqz == 1) == (want_sq == 1)) continue;
                    d.sqz = want_sq ? 1 : 2;
                    if (d.clip) {
                        d.cr[0] = want_sq ? d.sqc + (d.crx0 - d.sqc) * dc_ws_sq() : d.crx0;
                        d.cr[2] = want_sq ? d.sqc + (d.crx1 - d.sqc) * dc_ws_sq() : d.crx1;
                    }
                }
            }
            { int fs = s_fontSqN, ff = s_fontFlatN;             /* 27.09: буквы кадра после решения кадра — сжаты или нет */
              for (const SqzCand& c : s_sqzCands) if (c.font) { if (c.sq_now == 1) fs++; else ff++; }
              if (fs + ff > 0) s_fontSqPrev = (fs > ff) ? 1 : 0; }
            if (dc_aspdbg())
                std::fprintf(stderr, "aspdbg: render had3d=%d copyflat=%d flat=%d hint=%d pb=%d run=%d scr=%d cands=%d flipped=%d draws=%d%c",
                             had3d, copyflat, flat, (s_prev_copyflat || s_prev_no3d) ? 1 : 0, s_pb_frame, pb_run, scrpb,
                             ncand, flipped, (int)s_draws.size(), 10);
        }
        s_prev_no3d = !had3d; s_prev_no3d_pub = s_prev_no3d;
        s_prev_copyflat = copyflat;
        s_flat_frame = flat;
        /* Первый в сеансе кадр с 3D. До него идут экраны, которые в сборке
         * с образом BIOS попадали в ветку 4:3 «игра ещё не загружена»
         * (логотип, лицензия). Своё ядро грузит игру сразу, dc_game_loaded()
         * истинен с первого кадра, и ранние экраны растягивались. */
        { static int pf = -1;   /* диагностика: смена плоского режима */
          if (flat != pf) {
              std::fprintf(stderr, "raster: flat=%d had3d=%d disp=%d,%d %dx%d draws=%d%c",
                           flat, had3d, dispX, dispY, dispW, dispH, (int)s_draws.size(), 10);
              pf = flat;
          } }
    }

    ptvk_upload_vram(sw_vram_base());   /* fresh textures for this frame */

    /* 25.09: MSAA и SSAO этого кадра. Объекты собираются здесь, до записи команд (пересборка ждёт простоя GPU).
     * SSAO — только в кадре мира с 3D (как у кадра B): меню, плоские экраны, копии кадра, загадка на мониторе — без него. */
    raster_feat_sync();                                /* 25.10.2026: конвейеры - под опции меню */
    post_fit_note(w, h, !s_prev_no3d);                 /* 24.10: цели SSAO, свечения, дымки - по кадрам с 3D */
    const int msN = msaaEnsure(w, h, !s_prev_no3d);   /* 24.10: цели MSAA - по кадрам с 3D */
    const int ppWorld = !s_prev_no3d && !s_flat_frame && !s_frameCopy && !s_prev_copyflat && !s_mon_frame
                        && g_pt_proj_h > 0.f && (msN > 1 || s_rpFirst != VK_NULL_HANDLE);
    const int aoFrame = s_ppSsao && ppWorld && postEnsure();
    s_ppAoFrame = aoFrame; s_ppMsFrame = msN;
    /* 25.09 вечер: свечение источников и отражения — только кадр мира с 3D (как у SSAO), не в родном режиме (O) */
    const int fxWorld = !s_prev_no3d && !s_flat_frame && !s_frameCopy && !s_prev_copyflat && !s_mon_frame
                        && g_pt_proj_h > 0.f && !dc_orig_nogfx();   /* 28.09 (7): и в режиме O (DC_ORIG_GFX=0 — как было) */
    const int fxDbg = (int)(s_pp[PP_FX_DBG] + 0.5f);
    const int glowFrame = fxWorld && (s_pp[PP_GL_ON] > 0.5f || fxDbg == 1) && fxEnsure();
    const int reflFrame = fxWorld && g_dc_gfx_materials && s_pp[PP_RF_ON] > 0.5f && s_rfPlaneOk && fxEnsure();
    s_fxGlowFrame = glowFrame; s_fxReflFrame = reflFrame;
    /* 25.09 ночь: блики материалов — кадр мира, материалы включены (dc_raster.frag «БЛИКИ МАТЕРИАЛОВ») */
    const int specFrame = fxWorld && g_dc_gfx_materials && s_pp[PP_SP_ON] > 0.5f;
    /* 27.09: объёмный свет у ламп — кадр мира, не в родном режиме; глубина — проход 1 SSAO (при выключенном SSAO он идёт
     * один, pp_zonly). maskFrame — главный проход сохраняет глубину и пишет маску «3D / поверх плоское» в альфу */
    const int volFrame = ppWorld && fxWorld && s_pp[PP_VL_ON] > 0.5f && postEnsure() && volEnsure();
    /* 25.10.2026: FXAA / SMAA / CMAA2 - кадр мира без MSAA; маска в альфе бережёт от них интерфейс и текст */
    const int aaMode = aa_frame(msN, ppWorld, w, h);
    s_ppAAFrame = aaMode != 0;
    const int maskFrame = aoFrame || volFrame || aaMode;
    if (s_shUboMap) {                                /* плоскость и параметры отражения — в блок теней (dc_raster.frag) */
        for (int i = 0; i < 4; i++) s_shUboMap[120 + i] = s_rfPlane[i];
        /* 29.09 поздно: rfc.x/.y (были не заняты) — время и сила мигания текстуры, выделенной в приложении («Найти в игре»,
         * dc_highlight_user.cfg -> бит 19 страницы, dc_raster.frag «ПОДСВЕТКА ТЕКСТУРЫ»). DC_HL_TEX — сила (0.22; 0 — выкл) */
        static float s_hlStr = -1.f;
        if (s_hlStr < 0.f) { const char *e = getenv("DC_HL_TEX"); s_hlStr = (e && *e) ? (float)atof(e) : 0.22f; if (s_hlStr < 0.f) s_hlStr = 0.f; }
        s_shUboMap[124] = (float)(GetTickCount64() % 3600000ull) * 0.001f; s_shUboMap[125] = s_hlStr;
        glint_ubo_update();                          /* 12.10: блеск предметов */
        s_shUboMap[126] = s_pp[PP_RF_TOL];
        s_shUboMap[127] = reflFrame ? (s_pp[PP_RF_ALL] > 0.5f ? 2.f : 1.f) : 0.f;
        /* 25.09 ночь: материалы пола — у каждого своя сила и размытие (dc_raster.frag) */
        s_shUboMap[128] = s_pp[PP_RF_T_STR]; s_shUboMap[129] = s_pp[PP_RF_T_BLUR];
        s_shUboMap[130] = s_pp[PP_RF_M_STR]; s_shUboMap[131] = s_pp[PP_RF_M_BLUR];
        s_shUboMap[132] = s_pp[PP_RF_G_STR]; s_shUboMap[133] = s_pp[PP_RF_G_BLUR];
        s_shUboMap[134] = s_pp[PP_RF_M_TINT]; s_shUboMap[135] = s_pp[PP_SP_SOFT];   /* rfm1.w — 27.09: металл без общего осветления */
        s_shUboMap[136] = 0.f;                       /* луна гладкой тенью — ставится после прохода карты теней */
        /* 25.09 ночь: блики материалов */
        s_shUboMap[141] = specFrame ? 1.f : 0.f;
        s_shUboMap[208] = s_pp[PP_SP_T_SHIN]; s_shUboMap[209] = s_pp[PP_SP_T_STR];
        s_shUboMap[210] = s_pp[PP_SP_M_SHIN]; s_shUboMap[211] = s_pp[PP_SP_M_STR];
        s_shUboMap[212] = s_pp[PP_SP_G_SHIN]; s_shUboMap[213] = s_pp[PP_SP_G_STR];
        s_shUboMap[214] = s_pp[PP_RF_M_TINT]; s_shUboMap[215] = s_pp[PP_SP_GL];
        s_shUboMap[252] = s_pp[PP_SP_ENV_T]; s_shUboMap[253] = s_pp[PP_SP_ENV_M]; s_shUboMap[254] = s_pp[PP_SP_ENV_G]; s_shUboMap[255] = s_pp[PP_SP_OLD];   /* msp3.w — прежняя формула */
        /* 27.09 вечер: шероховатый металл (mrg); выключатель — все четыре 0, металл как был */
        const int mr = s_pp[PP_SP_MROUGH] > 0.5f;
        s_shUboMap[436] = mr ? s_pp[PP_SP_MMATTE] : 0.f; s_shUboMap[437] = mr ? s_pp[PP_SP_MBUMP] : 0.f;
        s_shUboMap[438] = mr ? s_pp[PP_SP_MRUST] : 0.f;  s_shUboMap[439] = mr ? s_pp[PP_SP_MGRAIN] : 0.f;
    }
    /* 25.09 ночь: луна гладкой тенью — объекты собираются до записи команд (создание ждёт простоя GPU) */
    const int mpfFrame = s_shOk && s_shN > 0 && s_mpfWant && s_shUboMap && s_shUboMap[115] >= 0.f
                         && (int)(s_shUboMap[115] + 0.5f) < s_shN && mpfEnsure();
    s_mpfFrame = mpfFrame;
    /* 27.09 (2): тени без зерна — пирамида карт теней ламп (объекты собираются до записи команд) */
    const int spfFrame = s_shOk && s_shN > 0 && s_spfWant && s_spfMask && s_shUboMap && spfEnsure();
    VkPipeline* const P  = (msN > 1) ? s_pipesMS : s_pipes;
    const VkPipeline  BG = (msN > 1) ? s_bgPipeMS : s_bgPipe;

    trep_frame_prepare();     /* 05.10: образы текстур PNG из mods, декодированных к этому кадру (до записи команд) */
    cmdBegin();
    pp_ts_reset();
    pp_ts(3);                 /* 09.10: начало кадра на GPU (DC_VK_STAGES) */
    if (glowFrame || reflFrame) fx_init_layouts();
    if (s_mpfOk > 0) mpf_init_layouts();
    if (s_spfOk > 0) spf_init_layouts();

    // VRAM staging copy if fresh content was staged
    if (ptvk_vram_dirty()) {
        VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toDst.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.image = C->vramImg;
        toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkBufferImageCopy bic{};
        bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bic.imageExtent = { 1024, 512, 1 };
        vkCmdCopyBufferToImage(s_cmd, C->vramStaging, C->vramImg,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        VkImageMemoryBarrier toRead = toDst;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
        ptvk_vram_clear_dirty();
    }
    fontHiresUpload(s_cmd);   /* hi-res атлас шрифта, если пришёл новый */
    trep_frame_upload(s_cmd); /* 05.10: и текстуры PNG из mods */
    if (s_prevImg && !s_prevOnGpu) {   /* 15.09: новый образ прошлого кадра — сразу в SHADER_READ_ONLY */
        VkImageMemoryBarrier pb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        pb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        pb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pb.srcAccessMask = 0; pb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        pb.image = s_prevImg;
        pb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &pb);
        s_prevOnGpu = 1;
    }
    pp_ts(7);                 /* 09.10 (2): после заливок (DC_VK_STAGES) */

    VkClearValue clears[2]{};
    clears[0].color = { { 0.f, 0.f, 0.f, 1.f } };
    clears[1].depthStencil = { 0.f, 0 };
    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = (msN > 1) ? s_rpMS : s_rp; rbi.framebuffer = (msN > 1) ? s_fbMS : s_fb;   /* 25.09: MSAA */
    rbi.renderArea = { { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    /* 21.09 вечер: в кадре есть ячейки захвата сцены — проход пойдёт отрезками, глубину первого отрезка сохраняем */
    const int scapFrame = (s_scapTris > 0 && dc_scap_on() && s_prevImg && s_prevOnGpu && s_rpFirst && s_rpResume) ? 1 : 0;
    /* 25.09: SSAO читает глубину после прохода — тоже проход с её сохранением (25.10: сглаживанию глубина не нужна) */
    if (scapFrame || aoFrame || volFrame) rbi.renderPass = (msN > 1) ? s_rpMSFirst : s_rpFirst;
    s_rpResumeCur = (msN > 1) ? s_rpMSResume : s_rpResume;
    { static int lastSF = -1;
      if (scapFrame != lastSF) { lastSF = scapFrame;
          std::fprintf(stderr, "raster: scene capture mid-frame %s (%d cells, %d triangles)%c",
                       scapFrame ? "ON" : "off", (int)s_scaps.size(), s_scapTris, 10); } }
    /* ===== ПРОХОД КАРТЫ ТЕНЕЙ (18.09) ================================
     * Глубина сцены из точки лампы: рисуем те же вершины тем же буфером, но
     * матрицей лампы. В dc_shadow.vert всё, кроме персонажа, уходит за
     * ближнюю плоскость, так что окклюдер — только подвижное. */
    /* ⛔ 18.09 вечер: параметры проекции берутся ЗДЕСЬ, а не в pt_renderer.c.
     * Там gpu_get_draw_offset отдаёт 0,0 — область рисования игры сдвинута
     * (обычно 0,256), и восстановленная видовая точка уезжала на четверть
     * экрана: тень ложилась мимо, хотя карта была верной. */
    const int glLightFrame = fxWorld && g_dc_gfx_materials && s_pp[PP_GL_LON] > 0.5f;   /* 25.09 поздно: свет от светящихся текстур */
    if (s_shUboMap) gl_set_count(0);
    if ((s_shOk && s_shN > 0) || s_flOn || glowFrame || reflFrame || glLightFrame || specFrame) {   /* 25.09: видовая точка нужна и вспышке, свечению, отражению, бликам */
        int dox = 0, doy = 0; gpu_get_draw_offset(&dox, &doy);
        { static int fd = -1; static long nf = 0;       /* DC_SHCAM_DBG=1: сдвиг рисования против вершин кадра */
          if (fd < 0) { const char* e = getenv("DC_SHCAM_DBG"); fd = (e && *e == '1') ? 1 : 0; }
          if (fd && s_vmap && (++nf % 30) == 1) {
              float y0 = 1e9f, y1 = -1e9f, c0 = 1e9f, c1 = -1e9f; long nd = 0, nc = 0;
              for (int i = 0; i < s_nverts; i++) {
                  const float* v = s_vmap + (size_t)i * VERT_FLOATS;
                  if (v[12] < 1.5f) continue;
                  nd++; if (v[1] < y0) y0 = v[1]; if (v[1] > y1) y1 = v[1];
                  if (((int)(v[11] + 0.5f) & 8388608) != 0) { nc++; if (v[1] < c0) c0 = v[1]; if (v[1] > c1) c1 = v[1]; }
              }
              std::fprintf(stderr, "shwp: dox=%d doy=%d disp=%d,%d ofy=%.1f | 3D verts %ld y %.0f..%.0f | char %ld y %.0f..%.0f%c",
                           dox, doy, dispX, dispY, g_pt_proj_ofy, nd, y0, y1, nc, c0, c1, 10);
          } }
        s_shWp[0] = g_pt_proj_ofx + (float)dox;
        s_shWp[1] = g_pt_proj_ofy + (float)doy;
        s_shWp[2] = g_pt_proj_h;
        s_shWp[3] = g_pt_proj_h * dc_ws_sq();
        /* 28.10: пирамида вида кадра A для gl_room_out (какой свет достаёт до кадра): края дисплея, как луч дымки */
        s_glFrOk = s_shWp[2] != 0.f && s_shWp[3] != 0.f && dispW > 0 && dispH > 0;
        if (s_glFrOk) {
            s_glFr[0] = ((float)dispX - s_shWp[0]) / s_shWp[3]; s_glFr[1] = ((float)(dispX + dispW) - s_shWp[0]) / s_shWp[3];
            s_glFr[2] = ((float)dispY - s_shWp[1]) / s_shWp[2]; s_glFr[3] = ((float)(dispY + dispH) - s_shWp[1]) / s_shWp[2];
        }
        if (glLightFrame) gl_lights_build();        /* 25.09 поздно: источники света от светящихся текстур этого кадра */
        { static int fd = -1; static long nf = 0;       /* DC_SHCAM_DBG=1: шейдерная выкладка на CPU для вершин персонажа */
          if (fd < 0) { const char* e = getenv("DC_SHCAM_DBG"); fd = (e && *e == '1') ? 1 : 0; }
          if (fd && s_vmap && s_shUboMap && s_shN > 0 && (++nf % 20) == 1) {
              const float* M = s_shUboMap;              /* vp[0], столбцами */
              int shown = 0;
              for (int i = 0; i < s_nverts && shown < 3; i += 97) {
                  const float* v = s_vmap + (size_t)i * VERT_FLOATS;
                  if (v[12] < 1.5f || (((int)(v[11] + 0.5f)) & 8388608) == 0) continue;
                  const float w = v[12];
                  const float p[3] = { (v[0] - s_shWp[0]) * w / s_shWp[3], (v[1] - s_shWp[1]) * w / s_shWp[2], w };
                  float c[4];
                  for (int r = 0; r < 4; r++) c[r] = M[0 * 4 + r] * p[0] + M[1 * 4 + r] * p[1] + M[2 * 4 + r] * p[2] + M[3 * 4 + r];
                  std::fprintf(stderr, "vkuv: vert %d scr %.1f,%.1f w %.0f -> view %.0f,%.0f,%.0f -> clip w %.0f uv %.3f,%.3f | wp %.1f,%.1f,%.1f,%.1f n %d%c",
                               i, v[0], v[1], w, p[0], p[1], p[2], c[3],
                               c[3] != 0.f ? c[0] / c[3] * 0.5f + 0.5f : -9.f, c[3] != 0.f ? c[1] / c[3] * 0.5f + 0.5f : -9.f,
                               s_shWp[0], s_shWp[1], s_shWp[2], s_shWp[3], (int)s_shUboMap[96], 10);
                  shown++;
              }
          } }
    }
    if (s_shOk && s_shN > 0) {
        /* 25.09 ночь (игрок: «за кадром полигоны персонажей режутся и с ними режется их тень»): полигоны персонажей, которые
         * игра не рисует (за краем кадра, спиной к камере), дописываются за вершинами кадра — ни один draw их не касается,
         * рисует их только этот проход (dc_shadow.vert: метка u = -1000, точка уже в видовом пространстве) */
        int nShx = 0;
        if (s_vmap && s_nverts < MAX_VERTS) {
            /* 09.10: сначала в память процессора (рамка заслонивших, sh_occ_rects), потом в буфер вершин — то же содержимое.
             * Списков у pt_shx_take не больше 8192 треугольников. */
            static std::vector<float> shxCpu((size_t)8192 * 3 * VERT_FLOATS);
            const int cap = (MAX_VERTS - s_nverts < 8192 * 3) ? MAX_VERTS - s_nverts : 8192 * 3;
            nShx = pt_shx_take(shxCpu.data(), cap, VERT_FLOATS);
            if (nShx > 0) {
                memcpy(s_vmap + (size_t)s_nverts * VERT_FLOATS, shxCpu.data(), (size_t)nShx * VERT_FLOATS * sizeof(float));
                occ_note(shxCpu.data(), nShx);
            }
        }
        VkViewport svp{ 0.f, 0.f, (float)s_shSize, (float)s_shSize, 0.f, 1.f };
        VkRect2D ssc{ { 0, 0 }, { (uint32_t)s_shSize, (uint32_t)s_shSize } };
        VkDeviceSize soff = 0;
        /* ⛔ 18.09 вечер: слои проходим ВСЕ, даже пустые. Слой, который в этом
         * кадре не рисовали, держит мусор видеопамяти, а шейдер читает его,
         * как только ламп стало больше — по кадру расползались чёрные блоки. */
        for (int li = 0; li < SH_MAX; li++) {
            /* номер лампы едет в свободном поле wd.w — из него вершинная
             * ступень берёт матрицу этой лампы; wp нужен ей для восстановления
             * видовой точки (обычный кадр этот блок обнуляет) */
            const float idx = (float)li;
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               108, 4, &idx);
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               112, 16, s_shWp);
            VkClearValue scv{}; scv.depthStencil = { 1.0f, 0 };
            VkRenderPassBeginInfo srbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            srbi.renderPass = s_shRp; srbi.framebuffer = s_shFb[li];
            srbi.renderArea = { { 0, 0 }, { (uint32_t)s_shSize, (uint32_t)s_shSize } };
            srbi.clearValueCount = 1; srbi.pClearValues = &scv;
            vkCmdBeginRenderPass(s_cmd, &srbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(s_cmd, 0, 1, &svp);
            vkCmdSetScissor(s_cmd, 0, 1, &ssc);
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_shPipe);
            if (li < s_shN && s_nverts + nShx > 0) {
                vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vbuf, &soff);
                vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plLay, 0, 1, &s_dset, 0, nullptr);
                vkCmdDraw(s_cmd, (uint32_t)(s_nverts + nShx), 1, 0, 0);
            }
            if (li < s_shN && s_shMeshPipe != VK_NULL_HANDLE)          /* 06.10.2026: меш-персонаж из mods тоже отбрасывает тень */
                for (const Draw& dm : s_draws)
                    if (dm.mesh >= 0 && dm.mocc) {
                        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plLay, 0, 1, &s_dset, 0, nullptr);
                        medx_record(s_cmd, s_shMeshPipe, dm);
                    }
            vkCmdEndRenderPass(s_cmd);
        }
        pp_ts(8);             /* 09.10 (2): после карт ламп (DC_VK_STAGES) */
        if (mpfFrame || spfFrame) sh_occ_rects(nShx);   /* 09.10: рамки заслонивших на слоях — луна и пирамиды только в них */
        /* 25.09 ночь: луна гладкой тенью — её слой ужать и размыть (растр берёт с биндинга 6) */
        if (mpfFrame) {
            mpf_run((int)(s_shUboMap[115] + 0.5f));
            s_shUboMap[136] = 1.f; s_shUboMap[137] = s_mpfPar[1]; s_shUboMap[138] = s_mpfPar[2]; s_shUboMap[139] = s_mpfPar[3];
        }
        pp_ts(9);             /* 09.10 (2): после луны (DC_VK_STAGES) */
        /* 27.09 (2): тени без зерна — слои ламп ужать в пирамиду (растр берёт с биндинга 7); без неё — прежний путь */
        if (spfFrame) {
            for (int li = 0; li < s_shN && li < SH_MAX; li++)
                if (s_spfMask & (1 << li)) spf_run(li);
            s_shUboMap[368] = 1.f;
        }
    } else { pp_ts(8); pp_ts(9); }
    pp_ts(4);                 /* 09.10: после заливок и теней (DC_VK_STAGES) */
    if (s_shUboMap) s_shUboMap[664] = 0.f;   /* 26.10.2026: кадр A - без пар кусков (их ставит только кадр между тактами) */
    /* 10.10.2026 (DC_HZ): кадр A с камерой на момент показа - только кадр мира, как у снимка для кадра B */
    float wpcA[16]; int warpA = 0;
    if (s_frame_alpha > 0.001f && g_pt_proj_h > 0.f && !s_flat_frame && !s_frameCopy && !s_prev_copyflat && !s_mon_frame &&
        s_scaps.empty()) {
        int had3dA = 0;
        for (size_t di = (size_t)drawFrom; di < s_draws.size() && !had3dA; di++) { int k = s_draws[di].kind; if (k == 0 || (k >= 2 && k < 6)) had3dA = 1; }
        int standA = 0;
        if (had3dA && warp_pc_make(s_frame_alpha, wpcA, &standA, 0, nullptr, nullptr, nullptr, nullptr, nullptr) && !standA) {
            int dxA = 0, dyA = 0; gpu_get_draw_offset(&dxA, &dyA);
            wpcA[12] = g_pt_proj_ofx + (float)dxA; wpcA[13] = g_pt_proj_ofy + (float)dyA;
            wpcA[14] = g_pt_proj_h; wpcA[15] = g_pt_proj_h * dc_ws_sq();
            warpA = 1;
        }
    }
    s_frame_alpha_used = warpA ? s_frame_alpha : 0.f;
    s_frame_alpha = 0.f;
    /* 25.09 вечер: проходы отражения и свечения — до основного (основной берёт отражение с биндинга 5) */
    if (reflFrame || glowFrame) {
        float wfx[16] = { 0.f, 0.f, 0.f, 1.f,  0.f, 0.f, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f,
                          s_shWp[0], s_shWp[1], s_shWp[2], s_shWp[3] };
        if (warpA) memcpy(wfx, wpcA, sizeof wfx);
        if (reflFrame) fx_geom_pass(2, s_draws, nullptr, (size_t)drawFrom, w, h, dispX, dispY, dispW, dispH, wfx);
        if (glowFrame) fx_geom_pass(1, s_draws, nullptr, (size_t)drawFrom, w, h, dispX, dispY, dispW, dispH, wfx);
    }

    pp_ts(0);
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vpt{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    VkRect2D sc{ { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(s_cmd, 0, 1, &vpt);
    vkCmdSetScissor(s_cmd, 0, 1, &sc);
    {   const VkBuffer vbs[3] = { s_vbuf, s_rvBuf, s_pvBuf }; const VkDeviceSize vofs[3] = { 0, 0, 0 };   /* 05.10: + текстуры PNG из mods; 26.10: + метки кусков */
        vkCmdBindVertexBuffers(s_cmd, 0, 3, vbs, vofs); }
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plLay,
                            0, 1, &s_dset, 0, nullptr);

    /* DuckStation-style true colour: full 8-bit shading, no ordered dither,
     * no 5-bit quantization. DC_TRUE_COLOR=0 restores the native look. */
    static int s_true_color = -1;
    static int s_tex_filter = -1;
    if (s_true_color < 0) {
        const char* e = getenv("DC_TRUE_COLOR");
        s_true_color = (e && *e == '0') ? 0 : 1;
        /* DuckStation "Texture Filtering: Bilinear" parity — the single
         * biggest visual difference vs their screenshots: CLUT texels are
         * filtered after palette decode, so distant low-res detail (faces!)
         * blends smoothly instead of ragged nearest blocks. DC_TEX_FILTER=0
         * restores the native point sampling. */
        /* Bilinear filtering came AFTER the reference state — native point
         * sampling is the console behaviour. DC_TEX_FILTER=1 re-enables. */
        e = getenv("DC_TEX_FILTER");
        s_tex_filter = (e && *e == '1') ? 1 : 0;
    }
    /* DC_DBG_PRIM=1: primitive-ID false colouring (flat unique colour per
     * polygon, no textures/blending) — dropped polygons read as background
     * holes, frame-to-frame diffs count them exactly. */
    /* 28.09: и режимы шейдера 2..7 (2 — Регина зелёная, прочая геометрия синяя, фон чёрный: дыра в теле видна сразу;
     * 3 — свой цвет у каждого полигона Регины, мир чёрный) — раньше сюда пускали только 1 */
    static int s_dbg_prim = -1;
    if (s_dbg_prim < 0) { const char* e2 = getenv("DC_DBG_PRIM");
                          s_dbg_prim = e2 ? atoi(e2) : 0;
                          if (s_dbg_prim < 0 || s_dbg_prim > 7) s_dbg_prim = 0; }
    /* BODY FADE (2026-08-23): params.y = dissolve level from the camera
     * solver (1 = solid), params.z = dither cell size in render-target
     * pixels (DC_BODY_FADE_CELL, default 2 - fine enough to read as
     * transparency at 5x, coarse enough to stay a visible dither). */
    static float s_fade_cell = -1.f;
    if (s_fade_cell < 0.f) { const char* e = getenv("DC_BODY_FADE_CELL"); s_fade_cell = e ? (float)atof(e) : 2.f;
                             if (s_fade_cell < 1.f) s_fade_cell = 1.f; }
    /* COVERAGE GATE (2026-08-23). The distance ramp faded her while she still
     * took a modest part of the frame (user: too early). Now the level is a
     * function of the share of the display area her screen bounding box
     * covers this frame: solid up to DC_BODY_FADE_COV0 (0.60), gone at
     * DC_BODY_FADE_COV1 (0.75), smoothstep between, low-passed per frame
     * (DC_BODY_FADE_RATE, 0.30). dc_body_fade() only says whether the dissolve
     * is allowed at all (nOTS camera, not a cutscene, DC_BODY_FADE=1). */
    float body_fade = 1.f;
    {
        extern int dc_actor_bbox_take(float*, float*, float*, float*);
        /* cov1 0.85 -> 0.75 (2026-08-23 evening): with the eye inside her the
         * clipped box read 0.77-0.80 and she hung at level 0.1-0.2 - a
         * checkered ghost instead of gone ("не пропадает полностью") */
        static float cov0 = -1.f, cov1 = 0.75f, rate = 0.30f, level_s = 1.f;
        if (cov0 < 0.f) { const char* e = getenv("DC_BODY_FADE_COV0"); cov0 = e ? (float)atof(e) : 0.60f;
                          e = getenv("DC_BODY_FADE_COV1"); if (e) cov1 = (float)atof(e);
                          e = getenv("DC_BODY_FADE_RATE"); if (e) rate = (float)atof(e);
                          if (cov1 < cov0 + 0.01f) cov1 = cov0 + 0.01f; }
        float bx0, by0, bx1, by1;
        int n = dc_actor_bbox_take(&bx0, &by0, &bx1, &by1);
        float cov = 0.f;
        if (n > 0 && dispW > 0 && dispH > 0) {
            float cx0 = bx0 < (float)dispX ? (float)dispX : bx0;
            float cy0 = by0 < (float)dispY ? (float)dispY : by0;
            float cx1 = bx1 > (float)(dispX + dispW) ? (float)(dispX + dispW) : bx1;
            float cy1 = by1 > (float)(dispY + dispH) ? (float)(dispY + dispH) : by1;
            if (cx1 > cx0 && cy1 > cy0) cov = ((cx1 - cx0) * (cy1 - cy0)) / ((float)dispW * (float)dispH);
        }
        float t = (cov - cov0) / (cov1 - cov0);
        if (t < 0.f) t = 0.f; if (t > 1.f) t = 1.f;
        float target = 1.f - t * t * (3.f - 2.f * t);              /* smoothstep, 1 = solid */
        extern int dc_body_fade_allowed(void);
        if (!dc_body_fade_allowed()) target = 1.f;
        level_s += (target - level_s) * rate;
        if (level_s > 0.999f) level_s = 1.f;
        body_fade = level_s;
        s_fade_level_pub = level_s;
        { static int st = -1; static int k = 0;
          if (st < 0) { const char* e = getenv("DC_BODY_FADE_DBG"); st = (e && *e == '1') ? 1 : 0; }
          if (st && (k++ % 30) == 0 && n > 0)
              std::fprintf(stderr, "bodyfade: cov=%.2f bbox=%.0f,%.0f..%.0f,%.0f disp=%d,%d %dx%d level=%.2f%c",
                           cov, bx0, by0, bx1, by1, dispX, dispY, dispW, dispH, level_s, 10); }
    }
    /* 15.09.2026 поздняя ночь (игрок, слот 0: «лампа — двойные полигоны, z-fighting»): не двойники — низ лампы натянут
     * на одну строку текстуры (v = 222 у всех вершин, ровно на границе текселей), перспективная интерполяция
     * давала 221.99998 / 222.00001 и шейдер брал то тёмную строку 221, то светлую 222 — серо-белый шум.
     * Бит 64 params.w: координата текселя округляется до 1/256 перед отбрасыванием дробной части
     * (dc_raster.frag texelUV). DC_UV_SNAP=0 — как было. */
    static int s_uv_snap = -1;
    if (s_uv_snap < 0) { const char* e = getenv("DC_UV_SNAP"); s_uv_snap = (e && *e == '0') ? 0 : 1; }
    float pc[8] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH,
                    0.f, body_fade, s_fade_cell,
                    (float)(s_true_color + 2 * s_tex_filter + 4 * s_dbg_prim + 64 * s_uv_snap) };
    /* Background pass: blit the back buffer (the just-composed current
     * frame) as the base image so uploaded/copied content the prim stream
     * never carries (menu image, HUD panels) appears. Opaque untextured
     * fills/clears are NOT mirrored (they'd repaint a black fullscreen quad
     * over this) — the CPU wrote them into VRAM, so they're already here.
     * Textured/semi/triangle prims paint on top for hi-res. */
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, BG);
    {
        /* params.x у фонового прохода = горизонтальное сжатие (0 — нет): на
         * плоском экране фон VRAM рисуем в те же центральные 3/4 ширины, что
         * и сжатые 2D-примитивы (2026-09-02, «начальный ролик наложен 2 раза») */
        float bgpc[8]; memcpy(bgpc, pc, sizeof bgpc);
        bgpc[4] = (s_flat_frame && !dc_flat_pillarbox_on()) ? dc_ws_sq() : 0.f;
        vkCmdPushConstants(s_cmd, s_plLay,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 32, bgpc);
        memcpy(s_wsnap.bgpc, bgpc, sizeof bgpc);   /* 17.09: снимок для кадра B */
    }
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    /* WHERE OUR ROOM SITS IN THE PAINT ORDER.
     * The room we inject is appended to the stream, so by default it paints
     * last - over the panels and the subtitles, which the game draws WITHOUT
     * a depth test and therefore relies on order for. Moving it to the front
     * is no good either: the game opens its frame with a full-screen
     * background fill, which would then wipe the room out (the black-screen-
     * with-Regina result from the first attempt).
     * So put it exactly where the game's own room would have been: after the
     * opening fills, before the first depth-tested world primitive. World
     * geometry sorts against it by depth as usual, and everything the game
     * draws flat afterwards - HUD, text, panels - lands on top of it. */
    /* ⭐ 23.09.2026 — КОМНАТА, КОТОРУЮ МЫ ДОЛЖНЫ ЭТОМУ КАДРУ (игрок, слот 13, подъём в вентиляцию: «мигание»; «как
     * можно просто не оставить на ОДИН КАДР ещё геометрию»). В показ идут таблица порядка игры прохода N-1 и наша
     * комната прохода N; комнату игры мы вынимаем из таблицы того прохода, где рисуем свою (dc_room_taken). Когда обход
     * в проходе N отдаёт комнату игре, на экране таблица N-1 БЕЗ комнаты игры и без нашей — чёрный фон, Регина и лампы
     * (замер DC_PASSDBG: показ 569 = таблица 111, где taken=16, обход 112 — exit=3; экран 3 -> 6). На консоли комната в
     * таблице N-1 есть. Правило: таблица в потоке — из прохода, где комнату забрали мы (dc_room_owed), а нашей комнаты в
     * потоке нет — кадр получает комнату, нарисованную нами в НЕПОСРЕДСТВЕННО ПРЕДЫДУЩЕМ показе; не больше раза на проход.
     * ⛔ 22.09 перенос делался в ЛЮБОМ кадре без нашей комнаты — и старая комната рисовалась сотни кадров поверх той, что
     * рисует сама игра (игрок: «сломал вообще всё»). DC_ROOM_CARRY=0 — как было. */
    {
        static int carry = -1;
        if (carry < 0) { const char* e = getenv("DC_ROOM_CARRY"); carry = (e && *e == '0') ? 0 : 1; }
        static std::vector<float> keepV;
        static std::vector<float> keepR;                /* 05.10: второй поток вершин (текстуры PNG из mods) тех же вершин */
        static std::vector<Draw>  keepD;
        static uint64_t renderNo = 0, keepNo = 0;
        static int carriedFor = 0;
        static int keepDX = 0, keepDY = 0;              /* начало показанной области того кадра (двойной буфер: y 0 / 256) */
        renderNo++;
        int haveRoomNow = 0;
        for (size_t i = 0; i < s_draws.size(); i++) if (s_draws[i].room) { haveRoomNow = 1; break; }
        {   int nui = 0; for (size_t i = 0; i < s_draws.size(); i++) if (s_draws[i].ui) nui++;
            s_lastUiDraws = nui; }                  /* 27.09 вечер: см. dcvk_last_ui_draws */
        if (carry) {
            if (haveRoomNow) {                              /* запомнить комнату этого кадра */
                keepV.clear(); keepR.clear(); keepD.clear();
                for (size_t i = 0; i < s_draws.size(); i++) {
                    const Draw& d = s_draws[i];
                    if (!d.room) continue;
                    Draw nd = d; nd.first = (int)(keepV.size() / VERT_FLOATS);
                    nd.scap = 0; nd.sqz = 0;                /* захваты сцены и сжатие интерфейса — данные ЭТОГО кадра */
                    keepD.push_back(nd);
                    keepV.insert(keepV.end(), s_vmap + (size_t)d.first * VERT_FLOATS,
                                 s_vmap + (size_t)(d.first + d.count) * VERT_FLOATS);
                    keepR.insert(keepR.end(), s_rvMap + (size_t)d.first * 4, s_rvMap + (size_t)(d.first + d.count) * 4);
                }
                keepNo = renderNo; keepDX = dispX; keepDY = dispY;
                {   static int clog2 = -1;
                    if (clog2 < 0) { const char* e = getenv("DC_CARRY_LOG"); clog2 = (e && *e == '1') ? 1 : 0; }
                    if (clog2) {
                        float ry0 = 1e9f, ry1 = -1e9f; const int nk = (int)(keepV.size() / VERT_FLOATS);
                        for (int k = 0; k < nk; k++) { const float y = keepV[(size_t)k * VERT_FLOATS + 1]; ry0 = std::min(ry0, y); ry1 = std::max(ry1, y); }
                        int nui = 0; for (size_t i = 0; i < s_draws.size(); i++) if (s_draws[i].ui) nui++;
                        std::fprintf(stderr, "carrylog: render %llu ROOM present disp %d,%d, room vertices %d y %.0f..%.0f, draws %d ui %d%c",
                                     (unsigned long long)renderNo, dispX, dispY, nk, ry0, ry1, (int)s_draws.size(), nui, 10);
                    }
                }
            } else {
                const int owed = dc_room_owed();            /* проход таблицы в потоке + 1, если комнату в нём забрали мы */
                {   /* 27.09 вечер: DC_CARRY_LOG=1 — состав кадра без нашей комнаты (мусорные кадры при записи на карту) */
                    static int clog = -1;
                    if (clog < 0) { const char* e = getenv("DC_CARRY_LOG"); clog = (e && *e == '1') ? 1 : 0; }
                    if (clog) {
                        int nd = (int)s_draws.size(), nui = 0, nw = 0; float uy0 = 1e9f, uy1 = -1e9f, wy0 = 1e9f, wy1 = -1e9f;
                        for (size_t i = 0; i < s_draws.size(); i++) {
                            const Draw& d = s_draws[i];
                            for (int k = 0; k < d.count; k++) {
                                const float y = s_vmap[(size_t)(d.first + k) * VERT_FLOATS + 1];
                                if (d.ui) { uy0 = std::min(uy0, y); uy1 = std::max(uy1, y); } else { wy0 = std::min(wy0, y); wy1 = std::max(wy1, y); }
                            }
                            if (d.ui) nui++; else nw++;
                        }
                        std::fprintf(stderr, "carrylog: render %llu owed %d carried %d keep %llu disp %d,%d keep %d,%d draws %d (ui %d y %.0f..%.0f, other %d y %.0f..%.0f) keepV %d%c",
                                     (unsigned long long)renderNo, owed, carriedFor, (unsigned long long)keepNo, dispX, dispY, keepDX, keepDY,
                                     nd, nui, uy0, uy1, nw, wy0, wy1, (int)(keepV.size() / VERT_FLOATS), 10);
                    }
                }
                if (owed && owed != carriedFor && keepNo + 1 == renderNo && !keepD.empty()
                    && s_nverts + (int)(keepV.size() / VERT_FLOATS) < MAX_VERTS) {
                    carriedFor = owed;
                    const int base = s_nverts;              /* комната прошлого показа */
                    const int nk = (int)(keepV.size() / VERT_FLOATS);
                    memcpy(s_vmap + (size_t)base * VERT_FLOATS, keepV.data(), keepV.size() * sizeof(float));
                    if (keepR.size() == (size_t)nk * 4) memcpy(s_rvMap + (size_t)base * 4, keepR.data(), keepR.size() * sizeof(float));
                    if (s_pvMap) memset(s_pvMap + base, 0, (size_t)nk * sizeof(uint32_t));   /* 26.10: комната прошлого показа - без меток */
                    /* игра рисует кадры попеременно в две половины видеопамяти (y 0 / 256): копия лежит в половине
                     * ПРОШЛОГО показа — переносим её в показываемую сейчас (замер 23.09: без сдвига кадр показал то,
                     * что было на 256 строк ниже комнаты, — пол «из-под» картинки) */
                    const float sdx = (float)(dispX - keepDX), sdy = (float)(dispY - keepDY);
                    if (sdx != 0.f || sdy != 0.f)
                        for (int k = 0; k < nk; k++) {
                            float* v = s_vmap + (size_t)(base + k) * VERT_FLOATS;
                            v[0] += sdx; v[1] += sdy;
                        }
                    s_nverts += nk;
                    for (size_t i = 0; i < keepD.size(); i++) {
                        Draw nd = keepD[i]; nd.first += base;
                        if (nd.clip) { nd.cr[0] += sdx; nd.cr[2] += sdx; nd.cr[1] += sdy; nd.cr[3] += sdy; }
                        s_draws.push_back(nd);
                    }
                    static long said = 0;
                    if (++said <= 40)
                        std::fprintf(stderr, "raster: in pass table %d we took the game's room, but our walk did not make it into this frame - "
                                             "the room of the previous present (%d vertices, shift %+d,%+d) #%ld%c",
                                     owed - 1, (int)(keepV.size() / VERT_FLOATS), dispX - keepDX, dispY - keepDY, said, 10);
                }
            }
        }
    }
    if (s_pauseDim && dc_pause_even_on()) {   /* 10.10: ровная плита паузы - на весь кадр, с запасом за края (место строки дисплея уже выбрано) */
        const size_t d0 = s_draws.size();
        const float x0 = (float)dispX - 16.f, x1 = (float)(dispX + dispW) + 16.f;
        const float y0 = (float)dispY - 16.f, y1 = (float)(dispY + dispH) + 16.f;
        float pv[3 * VERT_FLOATS];
        auto put = [&](int i, float x, float y) {
            float* o = pv + i * VERT_FLOATS;
            for (int k = 0; k < VERT_FLOATS; k++) o[k] = 0.f;
            o[0] = x; o[1] = y; o[11] = 2.f; o[12] = 1.f;   /* F_SEMI: шейдер смешивает во втором проходе */
        };
        put(0, x0, y0); put(1, x1, y0); put(2, x0, y1); dcvk_push_tri_raw(pv, 0, 0);
        put(0, x1, y0); put(1, x1, y1); put(2, x0, y1); dcvk_push_tri_raw(pv, 0, 0);
        for (size_t i = d0; i < s_draws.size(); i++) {
            Draw& d = s_draws[i];
            d.pdim = 1; d.ui = 0; d.room = 0; d.late = 0; d.clip = 0; d.sqz = 0; d.scap = 0;
        }
        { static int said = 0;
          if (said < 3) { said++; std::fprintf(stderr, "raster: pause - the game's frame copy replaced by one even plate (DC_PAUSE_EVEN=0 off)%c", 10); } }
    }
    static std::vector<size_t> s_order;
    s_order.clear();
    static std::vector<uint8_t> s_zclr;        /* 01.10 вечер: перед этим draw — очистка глубины (загадка на мониторе, см. ниже) */
    s_zclr.clear();
    int monStreamNow = 0;
    {
        /* THE ORDER PRIMITIVES ARE SUBMITTED IN IS NOT THE ORDER THEY ARE
         * DRAWN IN. The console sorts them through an ordering table by
         * depth, so the character's shadow is submitted BEFORE the room's
         * floor and drawn AFTER it. Injecting our room at submission time
         * therefore lands it in the wrong place, and the shadow and the blood
         * disappear under it - which is exactly what happened when this was
         * switched off. So the room is moved to the front here: it is the
         * background-most geometry in the frame either way.
         * DC_ROOM_REORDER=0 disables. */
        static int reorder = -1;
        if (reorder < 0) { const char* e = getenv("DC_ROOM_REORDER"); reorder = (e && *e == '0') ? 0 : 1; }
        int haveRoom = 0;
        if (reorder)
            for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++)
                if (s_draws[i].room) { haveRoom = 1; break; }
        if (!haveRoom) {
            for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++) s_order.push_back(i);
        } else {
            size_t firstWorld = s_draws.size();
            for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++) {
                const Draw& d = s_draws[i];
                if (d.room) continue;
                int k = d.kind;                       /* depth-tested kinds: 0, 2..5 */
                if (k == 0 || (k >= 2 && k <= 5)) { firstWorld = i; break; }
            }
            /* A FRAME WITH NO WORLD IN IT IS A MENU.
             * Then there is no "before the world" to insert into, the insert
             * point falls off the end, and the room would paint over the
             * inventory, the map and the text - the menu simply did not
             * appear. Nothing of ours belongs on top of a screen the game
             * drew flat: put the room first and let the game's own drawing
             * cover it completely. */
            /* ...BUT A GAMEPLAY FRAME CAN LOSE ITS WORLD TOO (2026-08-25).
             * That is the black screen while running against a wall. With
             * the boom collision-clamped hard the eye ends up inside
             * Regina, her primitives are near-clipped away whole and the
             * rest of the game's stream goes with them, so all that is left
             * of the GAME is its per-frame back-buffer CLEAR: two
             * full-screen untextured quads that merge into one 12-vertex
             * flat draw - the measured low=6 high=6 / low=12 high=0, tot=12
             * on every black frame. Rewinding the insert point then makes
             * the leading-fill scan below a ZERO-iteration loop, so that
             * clear is emitted LAST, through a pipeline with no depth test,
             * and it repaints the whole window black over the room that had
             * just been rasterised - 212 draws issued and the frame still
             * exactly zero in every pixel.
             * Tell the two frames apart by what the game actually drew: a
             * menu always carries something SMALL (text, icons, a cursor),
             * while a frame holding nothing but full-screen fills has no
             * menu in it at all - those fills are clears and must keep
             * their place in front. DC_NOWORLD_FIX=0 restores the old
             * unconditional rewind. */
            /* ⭐ 21.09.2026 вечер — ТЕКСТ СЦЕНЫ ПОД НАШЕЙ КОМНАТОЙ (игрок, слот 3: «загадка сломалась… текста не видно»).
             * С 20.09 сцены на движке рисует наш эмиттер (DC_SCENE_OUR_ROOM): игра в такой сцене шлёт только очистку кадра и
             * текст, без единого полигона мира. Они идут подряд и сливались в ОДИН draw (замер dump_prims: draw 0, 144
             * вершины, x 0..320 y 0..496, страницы 0/44/61); по габаритам весь draw считался «ведущей заливкой» и рисовался ДО
             * комнаты — текст уходил под неё. На экране мира (байт экрана 3) в кадре без мира игры ведущие полноэкранные
             * треугольники такого draw отделяются: очистка остаётся перед комнатой, остальное рисуется после неё. И правило
             * «меню: комната первой, игра закрывает её» на экране мира не применяется — там комната видна всегда (в меню
             * байт экрана другой, и комнаты в потоке нет). DC_SCENE_TEXT_TOP=0 — как было. */
            int worldscr_room = 0;
            if (firstWorld == s_draws.size()) {
                static int stt = -1;
                if (stt < 0) { const char* e = getenv("DC_SCENE_TEXT_TOP"); stt = (e && *e == '0') ? 0 : 1; }
                uint8_t* rstt = memory_get_ram_ptr();
                if (stt && rstt && rstt[0xB7878] == 3) {
                    worldscr_room = 1;
                    for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++) {
                        const Draw d = s_draws[i];
                        if (d.room) continue;
                        if (d.wire || d.late || d.kind != 1 || d.pass != 0 || d.count < 6) break;
                        int run = 0;                                 /* ведущие полноэкранные треугольники */
                        for (int t = 0; t + 3 <= d.count; t += 3) {
                            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                            for (int v = 0; v < 3; v++) {
                                const float* p = s_vmap + (size_t)(d.first + t + v) * VERT_FLOATS;
                                if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
                                if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
                            }
                            if ((x1 - x0) < 200.f || (y1 - y0) < 150.f) break;
                            run += 3;
                        }
                        if (run > 0 && run < d.count) {
                            Draw rest = d; rest.first = d.first + run; rest.count = d.count - run;
                            s_draws[i].count = run;
                            s_draws.insert(s_draws.begin() + (long)i + 1, rest);
                            { static int said = 0;
                              if (said < 4) { said++;
                                  std::fprintf(stderr, "raster: scene text split off the frame clear (draw %d: %d + %d verts) - drawn after our room%c",
                                               (int)i, run, rest.count, 10); } }
                        }
                        break;                                       /* смотрим только первый плоский draw игры */
                    }
                    firstWorld = s_draws.size();
                }
            }
            if (firstWorld == s_draws.size() && !worldscr_room) {
                static int nwf = -1;
                if (nwf < 0) { const char* e = getenv("DC_NOWORLD_FIX");
                               nwf = (e && *e == '0') ? 0 : 1; }
                int hasSmall = 0;
                if (nwf) {
                    for (size_t i = (size_t)drawFrom; i < s_draws.size(); i++) {
                        const Draw& d = s_draws[i];
                        if (d.room) continue;
                        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                        for (int v = 0; v < d.count; v++) {
                            const float* p = s_vmap + (size_t)(d.first + v) * VERT_FLOATS;
                            if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
                            if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
                        }
                        if ((x1 - x0) < 200.f || (y1 - y0) < 150.f) { hasSmall = 1; break; }
                    }
                }
                { static int dbg = -1; static long n = 0;
                  if (dbg < 0) { const char* e = getenv("DC_NOWORLD_DBG");
                                 dbg = (e && *e == '1') ? 1 : 0; }
                  if (dbg) fprintf(stderr, "noworld #%ld: draws=%d small=%d -> %s%c",
                                   ++n, (int)s_draws.size(), hasSmall,
                                   (!nwf || hasSmall) ? "menu(room first)" : "clears stay in front", 10); }
                if (!nwf || hasSmall) firstWorld = (size_t)drawFrom;
            }
            /* The order the ordering table would have produced, in four
             * groups:
             *   1. our room - the background-most geometry there is;
             *   2. what the game drew flat BEFORE the world: the character's
             *      shadow, blood, scorch marks. They belong on our floor, and
             *      they are submitted before it, so they have to come after
             *      the room and before everything else;
             *   3. the world itself, depth-sorted among itself as usual;
             *   4. what the game drew flat AFTER the world: subtitles, HUD,
             *      text. These have no depth at all, so nothing but being
             *      drawn last keeps them on top - submission order put them
             *      under Regina (user: "субтитры опять перекрываются").
             */
            /* ...but the very first thing in a frame is the background FILL,
             * and that covers the whole screen. Put the room before it and
             * the frame comes out black with only the characters in it (seen,
             * twice). So the leading run of full-screen flat draws keeps its
             * place, and the room goes in right after it. */
            /* ⭐ 22.09.2026 — ТЕКСТ ЗАГАДКИ СНОВА ПОД КОМНАТОЙ (игрок, слот 13: «в загадке опять текст пропадает…
             * после второй фразы не показывается текст»). Ведущей заливкой кадра считался draw, у которого
             * ГАБАРИТЫ ЦЕЛИКОМ больше 200x150. В окне выбора игра шлёт одним draw две строки текста и блит 2x1
             * в верхнем углу: габариты группы 210x211 — тест проходил, и текст уезжал в ведущие заливки, то есть
             * ПОД нашу комнату (замер order:: «#01 di=01 ui=1 n=282 bbox 40..250 / 0..211» ПЕРЕД комнатой; с одной
             * строкой ширина 146, тест не проходил, и тот же draw стоял последним — текст был виден).
             * Теперь заливка считается ПОТРЕУГОЛЬНО, и если draw начинается с полноэкранных треугольников, а
             * дальше идёт мелочь, он ДЕЛИТСЯ: очистка кадра остаётся первой, остаток рисуется своим чередом
             * (выбросить такой draw из ведущей группы целиком нельзя — потеряется очистка).
             * DC_FILL_PERTRI=0 — прежний тест по габаритам всего draw. */
            static int fpt = -1;
            if (fpt < 0) { const char* e = getenv("DC_FILL_PERTRI"); fpt = (e && *e == '0') ? 0 : 1; }
            size_t fillEnd = (size_t)drawFrom;
            /* ⭐ 22.09.2026 — ЗАТЕМНЕНИЕ ИГРЫ НЕ ЕСТЬ ОЧИСТКА КАДРА (игрок, слот 13, запись показанных кадров:
             * «625 33.9 — вот этого не должно быть. Видимо, когда кончается переход, фейд включается спустя кадр»).
             * Замер порядка вывода на этом самом кадре: `#00 di=24 kind=1 pass=0 bbox 0..320/0..240` (очистка),
             * `#01 di=25 kind=8 pass=1` и `#03 di=27 kind=8 pass=1` — ПОЛУПРОЗРАЧНЫЕ полноэкранные слои, которыми
             * игра гасит картинку (в дампах их цвет идёт 248 → 224 → 200 → 192 → 32 по ходу проявления), а
             * `fillEnd=28` уводил их в ведущие заливки, то есть ПОД нашу комнату: комната выходила в полную
             * яркость, пока игра держит экран чёрным — та самая вспышка после двери. Очистка кадра — это только
             * НЕПРОЗРАЧНАЯ заливка; полупрозрачный слой идёт своим чередом и ложится поверх мира.
             * DC_FILL_OPAQUE=0 — как было. */
            static int fop = -1;
            if (fop < 0) { const char* e = getenv("DC_FILL_OPAQUE"); fop = (e && *e == '0') ? 0 : 1; }
            for (size_t i = (size_t)drawFrom; i < firstWorld; i++) {
                if (s_draws[i].room) continue;
                const Draw d = s_draws[i];                           /* копия: ниже возможна вставка */
                if (fop && d.pass != 0) break;                       /* полупрозрачный слой — не очистка */
                int isFill = 0, run = 0;
                if (fpt) {
                    for (int t = 0; t + 3 <= d.count; t += 3) {
                        float tx0 = 1e9f, tx1 = -1e9f, ty0 = 1e9f, ty1 = -1e9f;
                        for (int v = 0; v < 3; v++) {
                            const float* p = s_vmap + (size_t)(d.first + t + v) * VERT_FLOATS;
                            if (p[0] < tx0) tx0 = p[0]; if (p[0] > tx1) tx1 = p[0];
                            if (p[1] < ty0) ty0 = p[1]; if (p[1] > ty1) ty1 = p[1];
                        }
                        if ((tx1 - tx0) < 200.f || (ty1 - ty0) < 150.f) break;
                        run += 3;
                    }
                    isFill = (run > 0);
                } else {
                    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                    for (int v = 0; v < d.count; v++) {
                        const float* p = s_vmap + (size_t)(d.first + v) * VERT_FLOATS;
                        if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
                        if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
                    }
                    isFill = !((x1 - x0) < 200.f || (y1 - y0) < 150.f);
                    run = d.count;
                }
                if (!isFill) break;                                  /* not a fill */
                if (run < d.count) {                                 /* очистка и мелочь в одном draw */
                    Draw rest = d; rest.first = d.first + run; rest.count = d.count - run;
                    s_draws[i].count = run;
                    s_draws.insert(s_draws.begin() + (long)i + 1, rest);
                    if (firstWorld > i) firstWorld++;
                    { static int said = 0;
                      if (said < 4) { said++;
                          std::fprintf(stderr, "raster: frame clear split off the draw (draw %d: %d + %d verts)%c",
                                       (int)i, run, rest.count, 10); } }
                    fillEnd = i + 1;
                    break;
                }
                fillEnd = i + 1;
            }
            for (size_t i = (size_t)drawFrom; i < fillEnd; i++)
                if (!s_draws[i].room) s_order.push_back(i);
            /* ⭐ 22.09.2026 — ОТСЕЧЕНИЕ ХВОСТА НЕ КАСАЕТСЯ НАШЕЙ КОМНАТЫ (игрок, слот 13, подъём в вентиляцию:
             * «есть мигания», кадр с чёрным фоном, Региной и лампами игры). Правило mixed stream отрезает голову
             * потока, чтобы выбросить СТАРЫЙ КАДР ИГРЫ при смене ракурса. Наша комната — не кадр игры: эмиттер
             * кладёт её один раз за кадр, и если она попала в голову, отсечение убирало из кадра весь мир.
             * Комнату берём ПОСЛЕДНЮЮ эмитированную, независимо от drawFrom. DC_ROOM_KEEP_TAIL=0 — как было. */
            size_t roomFrom = (size_t)drawFrom;
            { static int rkt = -1;
              if (rkt < 0) { const char* e = getenv("DC_ROOM_KEEP_TAIL"); rkt = (e && *e == '1') ? 1 : 0; }
              if (rkt && drawFrom > 0) {
                  long last = -1, run = -1;                    /* начало последней непрерывной группы room-draws */
                  for (size_t i = 0; i < s_draws.size(); i++) {
                      if (s_draws[i].room) { if (run < 0) run = (long)i; last = run; }
                      else run = -1;
                  }
                  if (last >= 0 && (size_t)last < (size_t)drawFrom) {
                      roomFrom = (size_t)last;
                      static int said = 0;
                      if (said < 4) { said++;
                          std::fprintf(stderr, "raster: the room lies in the cut-off head of the stream - drawing it (drawFrom=%d, room from %ld)%c",
                                       drawFrom, last, 10); }
                  }
              } }
            for (size_t i = roomFrom; i < s_draws.size(); i++)
                if (s_draws[i].room && !s_draws[i].late) s_order.push_back(i);
            /* ⭐ 01.10 вечер — ЗАГАДКА НА МОНИТОРЕ С НАШЕЙ КОМНАТОЙ: ПОРЯДОК ПОТОКА ИГРЫ (игрок, слот 29, ST503, «Will you assemble the
             * parts you have? → Yes»: «в ней всё неправильно отображается. Текст, 3D-элементы. Почини всё по глубине и отображению»).
             * Замер (DC_ORDER_DBG): загадка — мини-сцена, комнату в ней рисует наш эмиттер (сцена в новой комнате 59), поэтому кадр
             * шёл по группам «заливки / комната / мир / поздние / плоское после мира». Игра же рисует экран загадки слоями: очистка,
             * пробирка, ПОДЛОЖКА во весь экран (синий квад + сетка, draw 34), снова пробирка, зелёная плашка, текст, рамка. У нас
             * подложка, плашка и рамка (плоские) уходили в конец — поверх пробирки и текста (поздние полупрозрачные), а глубина
             * нашей комнаты под подложкой прятала их ещё и проверкой глубины. У консоли подложка закрывает всё, что до неё.
             * Теперь в кадре загадки на мониторе (кольцо рамки найдено, s_mon_ring — в любом режиме соотношения): заливки, наша
             * комната, её поздние, а дальше ВСЁ остальное — в порядке потока игры; перед непрозрачной плоской заливкой во весь экран
             * (первый треугольник ≥ 200x150) глубина очищается — что было под ней, у консоли закрыто. Кадры без нашей комнаты и так
             * идут порядком потока. Текст и цифры загадки — полупрозрачные без своей глубины: им больше не дают глубину соседа
             * (gpu_sw_renderer.c, dc_semw_adopt) — иначе они шли как 3D и растягивались ×4/3 (M43Cand). DC_MONITOR_STREAM=0 — как было. */
            if (dc_monitor_stream_on() && s_mon_ring) {
                monStreamNow = 1;
                for (size_t i = roomFrom; i < s_draws.size(); i++)
                    if (s_draws[i].room && s_draws[i].late) s_order.push_back(i);
                s_zclr.assign(s_draws.size(), 0);
                int nclr = 0;
                for (size_t i = fillEnd; i < s_draws.size(); i++) {
                    const Draw& d = s_draws[i];
                    if (d.room) continue;
                    if (d.kind == 1 && d.pass == 0 && !d.wire && d.count >= 3) {
                        float tx0 = 1e9f, tx1 = -1e9f, ty0 = 1e9f, ty1 = -1e9f;
                        for (int v = 0; v < 3; v++) {
                            const float* p = s_vmap + (size_t)(d.first + v) * VERT_FLOATS;
                            if (p[0] < tx0) tx0 = p[0]; if (p[0] > tx1) tx1 = p[0];
                            if (p[1] < ty0) ty0 = p[1]; if (p[1] > ty1) ty1 = p[1];
                        }
                        if ((tx1 - tx0) >= 200.f && (ty1 - ty0) >= 150.f) { s_zclr[i] = 1; nclr++; }
                    }
                    s_order.push_back(i);
                }
                { static int said = 0;
                  if (said < 6) { said++;
                      std::fprintf(stderr, "raster: monitor puzzle with our room - the game's stream order after the room, depth cleared before %d full-screen fill(s) (DC_MONITOR_STREAM=0 off)%c",
                                   nclr, 10); } }
            }
            /* ⭐ ИНТЕРФЕЙС — ПОВЕРХ ВСЕГО (12.09.2026, игрок: «сделай, чтобы любое
             * экранное UI отображалось поверх всего»). Двумерные по
             * построению примитивы (ui2d: ректы, спрайты, заливки, блиты —
             * тот же признак, по которому интерфейс сжимается в 4:3),
             * попавшие в поток ДО первого полигона мира, раньше рисовались
             * под миром (группа 2 вместе с тенью и кровью). Теперь они уходят
             * в самую верхнюю группу, сохраняя порядок между собой; тень и
             * кровь (полупрозрачные, не ui2d) остаются на полу. Ведущий блок
             * полноэкранных заливок (очистка кадра) по-прежнему первый.
             * Только в кадрах с миром; плоские экраны (инвентарь, карта) идут
             * без переупорядочивания. DC_UI_TOP=0 — прежнее поведение. */
            static int uitop = -1;
            if (uitop < 0) { const char* e = getenv("DC_UI_TOP"); uitop = (e && *e == '0') ? 0 : 1; }
            /* 21.09 вечер (игрок, слот 3, загадка с кнопками: «зеркало стоит по глубине выше текста и загораживает текст.
             * Надо чтобы текст был выше всего»): в сцене с нашей комнатой мира игры в кадре нет (firstWorld == конец), и текст
             * шёл группой «до мира» — раньше отложенных полупрозрачных нашей комнаты (стекло камеры батарей, late), которые
             * ложились поверх него. На экране мира интерфейс такого кадра тоже уходит в верхнюю группу, после late. */
            const int uiworld = uitop && (firstWorld < s_draws.size() || worldscr_room);
            if (!monStreamNow) {                       /* 01.10 вечер: кадр загадки на мониторе уже разложен выше */
            for (size_t i = fillEnd; i < firstWorld; i++)
                if (!s_draws[i].room && !(uiworld && s_draws[i].ui)) s_order.push_back(i);
            for (size_t i = firstWorld; i < s_draws.size(); i++) {
                if (s_draws[i].room || s_draws[i].late) continue;
                int k = s_draws[i].kind;
                if (k == 0 || (k >= 2 && k <= 5)) s_order.push_back(i);   /* world */
            }
            /* translucent-last pairs - room lasers/glows AND the game's own
             * world semis - after the whole world, z-tested against its
             * finished depth (2026-08-30) */
            for (size_t i = roomFrom < (size_t)drawFrom ? roomFrom : (size_t)drawFrom; i < s_draws.size(); i++)
                if (s_draws[i].late) s_order.push_back(i);
            if (uiworld)
                for (size_t i = fillEnd; i < firstWorld; i++)
                    if (!s_draws[i].room && s_draws[i].ui) s_order.push_back(i);   /* интерфейс до мира: наверх */
            for (size_t i = firstWorld; i < s_draws.size(); i++) {
                if (s_draws[i].room || s_draws[i].late) continue;
                int k = s_draws[i].kind;
                if (!(k == 0 || (k >= 2 && k <= 5))) s_order.push_back(i); /* flat: on top */
            }
            }
            { /* ВРЕМЕННО: снимок порядка вывода по файлу orderdbg.on (DC_ORDER_DBG=1) */
              static int odbg = -1;
              if (odbg < 0) { const char* e = getenv("DC_ORDER_DBG"); odbg = (e && *e == '1') ? 1 : 0; }
              static int odbg_left = 0;   /* 23.09: в файле число N — снимать N кадров подряд */
              if (odbg) { FILE* t = fopen("orderdbg.on", "r");
                if (t) { int nn = 1; if (fscanf(t, "%d", &nn) != 1 || nn < 1) nn = 1; fclose(t); remove("orderdbg.on"); odbg_left = nn; }
                if (odbg_left > 0) { odbg_left--;
                  { extern uint64_t s_frame_count; extern int g_fuse_replayed_now;
                  std::fprintf(stderr, "order: frame=%llu n=%llu disp %d,%d %dx%d  target %dx%d  drawFrom=%d firstWorld=%d fillEnd=%d worldscr=%d uiworld=%d draws=%d fuse=%d%c",
                               (unsigned long long)s_frame_count, s_scapLogN + 1, dispX, dispY, dispW, dispH, w, h, (int)drawFrom, (int)firstWorld, (int)fillEnd,
                               worldscr_room, uiworld, (int)s_draws.size(), g_fuse_replayed_now, 10); }
                  for (size_t oi = 0; oi < s_order.size(); oi++) {
                      const Draw& dd = s_draws[s_order[oi]];
                      float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                      double cr_ = 0, cg_ = 0, cb_ = 0;
                      for (int v = 0; v < dd.count; v++) {
                          const float* p = s_vmap + (size_t)(dd.first + v) * VERT_FLOATS;
                          if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
                          if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
                          cr_ += p[4]; cg_ += p[5]; cb_ += p[6]; }
                      const float* p0 = s_vmap + (size_t)dd.first * VERT_FLOATS;
                      const double nv_ = dd.count > 0 ? (double)dd.count : 1.0;
                      std::fprintf(stderr, "order: #%02d di=%02d kind=%d pass=%d room=%d ui=%d late=%d clip=%d scap=%d n=%d bbox %.0f..%.0f / %.0f..%.0f rgb=%.0f,%.0f,%.0f tp=%.0f cl=%.0f fl=%.0f%c",
                                   (int)oi, (int)s_order[oi], dd.kind, dd.pass, dd.room, dd.ui, dd.late, dd.clip, dd.scap,
                                   dd.count, x0, x1, y0, y1, cr_ / nv_, cg_ / nv_, cb_ / nv_, p0[8], p0[9], p0[11], 10); } } } }
        }
    }
    /* 10.10: ровная плита паузы (s_pauseDim) - после последнего draw с глубиной: мир под ней, надписи над ней */
    {
        std::vector<size_t> pd;
        for (size_t oi = 0; oi < s_order.size(); )
            if (s_draws[s_order[oi]].pdim) { pd.push_back(s_order[oi]); s_order.erase(s_order.begin() + (long)oi); }
            else oi++;
        if (!pd.empty()) {
            size_t at = 0;
            for (size_t oi = 0; oi < s_order.size(); oi++) {
                const Draw& d = s_draws[s_order[oi]];
                const int k = d.kind;
                if (d.room || d.late || k == 0 || (k >= 2 && k <= 5) || k == 11 || d.mesh >= 0) at = oi + 1;
            }
            s_order.insert(s_order.begin() + (long)at, pd.begin(), pd.end());
        }
    }
    /* 15.09: КОПИЯ КАДРА ИЗ ПРОШЛОГО HI-RES КАДРА (см. s_prevImg). Только на экранах вне
     * мира (байт экрана не 3/8), где в кадре есть копия кадра, и только если прошлый кадр
     * был того же размера. push-константы 32..64: prevRect (x, y, w, h в draw-space),
     * prevMap (доля активной части образа по x/y, включено, -). */
    {
        static int ch = -1;
        if (ch < 0) { const char* e = getenv("DC_COPY_HIRES"); ch = (e && *e == '0') ? 0 : 1; }
        uint8_t* ramc = memory_get_ram_ptr();
        const int scrc = ramc ? ramc[0xB7878] : 3;
        const int use = ch && s_prevValid && s_frameCopy && scrc != 3 && scrc != 8 &&
                        s_prevRect[2] == dispW && s_prevRect[3] == dispH && s_prevActW == w && s_prevActH == h;
        float pcp[8] = { (float)s_prevRect[0], (float)s_prevRect[1], (float)s_prevRect[2], (float)s_prevRect[3],
                         s_fbW > 0 ? (float)w / (float)s_fbW : 0.f, s_fbH > 0 ? (float)h / (float)s_fbH : 0.f,
                         use ? 1.f : 0.f, 0.f };
        vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           32, 32, pcp);
        memcpy(s_wsnap.pcp, pcp, sizeof pcp);   /* 17.09: снимок для кадра B */
        { static int lastUse = -1;
          if (use != lastUse) { lastUse = use;
              std::fprintf(stderr, "raster: frame copy from the previous hi-res frame %s (screen %d)%c",
                           use ? "ON" : "off", scrc, 10); } }
    }
    { const float wid[16] = { 0.f, 0.f, 0.f, 1.f,  0.f, 0.f, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f,  0.f, 0.f, 0.f, 0.f };   /* 17.09: кадр A — без сдвига */
      vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 64, 64, warpA ? wpcA : wid); }   /* 10.10: DC_HZ */
    /* wp — параметры проекции: по ним вершинная ступень восстанавливает видовую
     * точку для теней. Обычный кадр этот блок обнуляет (его заполняет только
     * кадр B), поэтому ставим их следом. Сами лампы лежат в блоке ShadowUBO.
     * 19.10.2026: и свету светящихся текстур, и бликам - видовая точка им тоже нужна (dc_raster.vert needView) */
    if ((s_shOk && s_shN > 0) || s_flOn || reflFrame || glLightFrame || specFrame) {
        vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           112, 16, s_shWp);
    }
    int bound = -1;
    const float pc7_base = pc[7];
    /* 15.09: scissor = область рисования игры у draw с клипом (см. Draw.clip) */
    const VkRect2D scFull = sc;
    VkRect2D scCur = sc;
    const int clipEn = dc_drawarea_clip_on();
    long clippedDraws = 0;
    int scapCur = 0, scapPcp = 0;                      /* 21.09 вечер: снятый захват / константы ячейки стоят */
    s_scapLogN++;                                      /* 01.10 вечер: номер показа растра для журнала ячеек */
    for (size_t oi_ = 0; oi_ < s_order.size(); oi_++) {
        size_t di_ = s_order[oi_];
        const Draw& d = s_draws[di_];
        if (monStreamNow && di_ < s_zclr.size() && s_zclr[di_]) {   /* 01.10 вечер: подложка загадки — под ней всё закрыто */
            VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; ca.clearValue.depthStencil = { 0.f, 0 };
            VkClearRect cr{}; cr.rect = scFull; cr.baseArrayLayer = 0; cr.layerCount = 1;
            vkCmdClearAttachments(s_cmd, 1, &ca, 1, &cr);
        }
        if (d.scap > 0 && (size_t)d.scap <= s_scaps.size() && s_scaps[(size_t)d.scap - 1].ab == 2) continue;   /* 01.10 вечер: квад кадра B */
        if (scapFrame && d.scap > 0 && (size_t)d.scap <= s_scaps.size()) {
            if (dc_scap_dbg()) continue;                     /* DC_SCENE_CAPTURE_DBG=1: кадр без ячеек (сверка) */
            const Scap& sc_ = s_scaps[(size_t)d.scap - 1];
            if (d.scap != scapCur) { scap_switch(sc_, rbi, w, h, dispX, dispY, dispW, dispH); scapCur = d.scap; scap_log('A', sc_); }
            float cp[8]; scap_pcp(sc_, w, h, dispX, dispY, dispW, dispH, cp);
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, cp);
            scapPcp = 1;
        } else if (scapPcp) {
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, s_wsnap.pcp);
            scapPcp = 0;
        }
        VkRect2D scWant = scFull;
        if (d.clip && clipEn && !d.wire) {
            const float kx = (float)w / (float)dispW, ky = (float)h / (float)dispH;
            int x0 = (int)floorf((d.cr[0] - (float)dispX) * kx + 0.5f);
            int x1 = (int)floorf((d.cr[2] - (float)dispX) * kx + 0.5f);
            int y0 = (int)floorf((d.cr[1] - (float)dispY) * ky + 0.5f);
            int y1 = (int)floorf((d.cr[3] - (float)dispY) * ky + 0.5f);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            if (x1 <= x0 || y1 <= y0) continue;              /* draw целиком вне области */
            scWant.offset = { x0, y0 };
            scWant.extent = { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
            clippedDraws++;
        }
        if (scWant.offset.x != scCur.offset.x || scWant.offset.y != scCur.offset.y ||
            scWant.extent.width != scCur.extent.width || scWant.extent.height != scCur.extent.height) {
            vkCmdSetScissor(s_cmd, 0, 1, &scWant);
            scCur = scWant;
        }
        if (d.mesh >= 0) {                                   /* 06.10.2026: меш из mods — свой конвейер, пул и экземпляр */
            if (P[12] != VK_NULL_HANDLE) {
                pc[7] = pc7_base + (maskFrame ? 384.0f : 0.0f);   /* как непрозрачный с записью глубины (маска SSAO) */
                pc[4] = 0.f;
                vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
                medx_record(s_cmd, P[12], d);
                bound = -1;
                const VkBuffer vbsM[3] = { s_vbuf, s_rvBuf, s_pvBuf }; const VkDeviceSize vofsM[3] = { 0, 0, 0 };
                vkCmdBindVertexBuffers(s_cmd, 0, 3, vbsM, vofsM);
            }
            continue;
        }
        /* a wire draw goes through the line pipeline, and tells the fragment
         * stage to output a flat colour instead of sampling a texture */
        int want = (d.wire && P[10] != VK_NULL_HANDLE) ? 10 : d.kind;
        if (want != bound) {
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P[want]);
            bound = want;
        }
        /* 25.09: биты маски SSAO — 128 «маска в кадре», 256 «непрозрачный с записью глубины» (dc_raster.frag aoMaskOut) */
        pc[7] = pc7_base + (d.wire ? 32.0f : 0.0f) +
                (maskFrame ? (128.0f + ((want == 0 || (d.scap > 0 && dc_scap_ao())) ? 256.0f : 0.0f)) : 0.0f);   /* 01.10: ячейка захвата — как мир */
        pc[4] = (float)d.pass;
        vkCmdPushConstants(s_cmd, s_plLay,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, 32, pc);
        vkCmdDraw(s_cmd, (uint32_t)d.count, 1, (uint32_t)d.first, 0);
    }
    if (scCur.offset.x != scFull.offset.x || scCur.offset.y != scFull.offset.y ||
        scCur.extent.width != scFull.extent.width || scCur.extent.height != scFull.extent.height)
        vkCmdSetScissor(s_cmd, 0, 1, &scFull);
    { static int cd = -1; static long lastN = -1;
      if (cd < 0) { const char* e = getenv("DC_CLIP_DBG"); cd = (e && *e == '1') ? 1 : 0; }
      if (cd && (clippedDraws > 0) != (lastN > 0)) {
          std::fprintf(stderr, "raster: draws with the game's drawing area: %ld%c", clippedDraws, 10);
          lastN = clippedDraws; } }
    /* DC_WIREFRAME=1: the same triangles once more, as edges, over the top.
     * Bit 5 of the flags word tells the fragment stage to output a flat colour
     * instead of texturing. */
    {
        static int wf = -1;
        if (wf < 0) { const char* e = getenv("DC_WIREFRAME"); wf = (e && *e == '1') ? 1 : 0; }
        if (wf && P[10] != VK_NULL_HANDLE) {
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P[10]);
            float pcw[8];
            for (int i = 0; i < 8; i++) pcw[i] = pc[i];
            pcw[7] = pc7_base + 32.0f + (maskFrame ? 128.0f : 0.0f);   /* the wire flag (25.09: от базы, не от последнего draw) */
            for (size_t oi_ = 0; oi_ < s_order.size(); oi_++) {
                const Draw& d = s_draws[s_order[oi_]];
                if (d.pass != 0) continue;        /* each triangle once */
                pcw[4] = (float)d.pass;
                vkCmdPushConstants(s_cmd, s_plLay,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, 32, pcw);
                vkCmdDraw(s_cmd, (uint32_t)d.count, 1, (uint32_t)d.first, 0);
            }
        }
    }
    vkCmdEndRenderPass(s_cmd);
    pp_ts(1);
    if (aaMode) aa_run(aaMode, w, h, aoFrame || volFrame);   /* 25.10.2026: сглаживание - до SSAO, свечения и дымки */
    pp_ts(13);
    if (glowFrame || volFrame) { veil_collect(s_draws, s_order, w, h, dispX, dispY, dispW, dispH); veil_ubo(); }   /* 28.09 (6): пелена игры */
    else s_veilN = 0;
    if (!aoFrame) { pp_ts(10); pp_ts(11); pp_ts(12); }   /* 09.10 (2): метки SSAO есть в каждом кадре (DC_VK_STAGES) */
    if (aoFrame) {                                   /* 25.09: затенение углов — по готовому кадру */
        int doxa = 0, doya = 0; gpu_get_draw_offset(&doxa, &doya);
        const float viewA[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpA[4] = { g_pt_proj_ofx + (float)doxa, g_pt_proj_ofy + (float)doya, g_pt_proj_h,
                               g_pt_proj_h * dc_ws_sq() };
        pp_run(w, h, msN, viewA, wpA);
    } else if (volFrame) {                           /* 27.09: SSAO выкл — для дымки только его проход глубины */
        int doxa = 0, doya = 0; gpu_get_draw_offset(&doxa, &doya);
        const float viewA[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpA[4] = { g_pt_proj_ofx + (float)doxa, g_pt_proj_ofy + (float)doya, g_pt_proj_h,
                               g_pt_proj_h * dc_ws_sq() };
        pp_zonly(w, h, msN, viewA, wpA);
    }
    if (glowFrame || (reflFrame && fxDbg == 2)) fx_glow_post(w, h, glowFrame, fxDbg);   /* 25.09 вечер: свечение — после SSAO */
    if (volFrame) {                                  /* 27.09: объёмный свет у ламп и пыль — после SSAO и свечения */
        int doxv = 0, doyv = 0; gpu_get_draw_offset(&doxv, &doyv);
        const float viewV[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpV[4] = { g_pt_proj_ofx + (float)doxv, g_pt_proj_ofy + (float)doyv, g_pt_proj_h,
                               g_pt_proj_h * dc_ws_sq() };
        vol_run(w, h, viewV, wpV, warpA ? wpcA : nullptr);   /* 10.10: DC_HZ - точки ламп в вид кадра с поправкой */
    }
    pp_ts(2);

    /* 09.10: в общую с D3D11 текстуру (+ строки яркости) или, как было, кадр целиком в память процессора */
    const bool shFrame = dx_record(w, h);
    if (!shFrame) {
        VkBufferImageCopy out{};
        out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        out.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImageToBuffer(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_rbBuf, 1, &out);
    }
    pp_ts(6);                                               /* 09.10: после чтения кадра в память (DC_VK_STAGES) */
    /* 15.09: этот кадр — «прошлый» для копии кадра следующего (см. s_prevImg) */
    if (s_prevImg && s_prevOnGpu) {
        VkImageMemoryBarrier pb{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        pb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; pb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        pb.image = s_prevImg;
        pb.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        pb.srcQueueFamilyIndex = pb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &pb);
        VkImageCopy ic{};
        ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.extent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImage(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_prevImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        VkImageMemoryBarrier pr = pb;
        pr.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        pr.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        pr.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; pr.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &pr);
    }
    int vcopy = 0;                                          /* 25.10: вершины кадра - в копию для потока показа */
    if (s_wsCopyOn && s_nverts > 0 && ws_copy_ready()) {
        const VkBufferCopy cv{ 0, 0, (VkDeviceSize)s_nverts * VERT_FLOATS * sizeof(float) };
        const VkBufferCopy cr{ 0, 0, (VkDeviceSize)s_nverts * 4 * sizeof(float) };
        const VkBufferCopy cp{ 0, 0, (VkDeviceSize)s_nverts * sizeof(uint32_t) };
        vkCmdCopyBuffer(s_cmd, s_vbuf, s_wsVb, 1, &cv);
        vkCmdCopyBuffer(s_cmd, s_rvBuf, s_wsRv, 1, &cr);
        vkCmdCopyBuffer(s_cmd, s_pvBuf, s_wsPv, 1, &cp);
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        vcopy = 1;
    }
    pp_ts(5);                                               /* 09.10: после копий кадра (DC_VK_STAGES) */
    LARGE_INTEGER tsub, twait;                              /* 09.10: DC_VK_STAGES=1 - время этапов кадра */
    QueryPerformanceCounter(&tsub);
    if (!cmdSubmitWait()) { rlog("[VKR] submit/hang\n"); s_prevValid = 0; s_wsnap.valid = 0; return nullptr; }
    test_gpu_slow(0);
    QueryPerformanceCounter(&twait);
    s_lastShared = shFrame ? 1 : 0;
    pp_ts_read();
    {   /* 17.09 вечер: снимок кадра для перепроецирования в кадре B */
        int had3d = 0;
        for (size_t di = (size_t)drawFrom; di < s_draws.size() && !had3d; di++) { int k = s_draws[di].kind; if (k == 0 || (k >= 2 && k < 6)) had3d = 1; }
        int doxw = 0, doyw = 0; gpu_get_draw_offset(&doxw, &doyw);
        s_wsnap.valid = 1; s_wsnap.used = 0; s_wsnap.dropped = 0;
        s_wsnap.world = had3d && !s_flat_frame && !s_frameCopy && !s_prev_copyflat && (g_pt_proj_h > 0.f)
                     && !s_mon_frame;                    /* 21.09 ночь: загадка на мониторе — 3D уже растянуто обратно, не перепроецировать */
        { static int dbg = -1; static int last = -1;
          if (dbg < 0) { const char* e = getenv("DC_WARP_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
          int code = had3d | (s_flat_frame << 1) | (s_frameCopy << 2) | (s_prev_copyflat << 3);
          if (dbg && code != last) { last = code;
              std::fprintf(stderr, "warp: snapshot world=%d (had3d %d flat %d copy %d copyflat %d) draws %d verts %d%c",
                           s_wsnap.world, had3d, s_flat_frame, s_frameCopy, s_prev_copyflat, (int)s_draws.size(), s_nverts, 10); } }
        s_wsnap.nverts = s_nverts;
        s_wsnap.dispX = dispX; s_wsnap.dispY = dispY; s_wsnap.dispW = dispW; s_wsnap.dispH = dispH; s_wsnap.w = w; s_wsnap.h = h;
        s_wsnap.pc[0] = pc[0]; s_wsnap.pc[1] = pc[1]; s_wsnap.pc[2] = pc[2]; s_wsnap.pc[3] = pc[3];
        s_wsnap.pc[4] = 0.f; s_wsnap.pc[5] = pc[5]; s_wsnap.pc[6] = pc[6]; s_wsnap.pc[7] = pc7_base;
        s_wsnap.ox = g_pt_proj_ofx + (float)doxw; s_wsnap.oy = g_pt_proj_ofy + (float)doyw;
        s_wsnap.H = g_pt_proj_h; s_wsnap.Hs = g_pt_proj_h * dc_ws_sq();
        s_wsnap.draws = s_draws; s_wsnap.order = s_order; s_wsnap.scaps = s_scaps;
        s_wsnap.mrefs.clear();                           /* 25.10: меши снимка - буферы и диапазоны сейчас */
        for (Draw& d : s_wsnap.draws) {
            if (d.mesh < 0) continue;
            MeshRef r;
            if (d.mesh < (int)s_mesh.size()) {
                const MeshGpu& g = s_mesh[(size_t)d.mesh];
                if (g.blk >= 0 && g.blk < (int)s_mblk.size()) {
                    r.vb = s_mblk[(size_t)g.blk].vb; r.ib = s_mblk[(size_t)g.blk].ib;
                    r.icount = g.icount; r.ibase = g.ibase; r.vbase = (int32_t)g.vbase;
                }
            }
            d.mref = (int)s_wsnap.mrefs.size();
            s_wsnap.mrefs.push_back(r);
        }
        s_wsnap.vcopy = vcopy;
        s_wsnap.frame++;
        s_wsnap.pairs = 0;                               /* 26.10: пары кусков кадра A и следующего такта (двери; main.cpp включает учёт) */
        { static int pen = -1;
          if (pen < 0) { const char* e = getenv("DC_PAIR_WARP"); pen = (e && *e == '0') ? 0 : 1; }
          if (pen && g_dc_pair_track && s_wsnap.world) s_wsnap.pairs = pair_build(g_dc_dma_pass, s_wsnap.frame); }
    }
    if (s_prevImg && s_prevOnGpu) {
        s_prevValid = 1;
        s_prevRect[0] = dispX; s_prevRect[1] = dispY; s_prevRect[2] = dispW; s_prevRect[3] = dispH;
        s_prevActW = w; s_prevActH = h;
    }
    s_outW = w; s_outH = h; s_lastFlat = s_flat_frame; s_lastPb = s_pb_frame; s_lastMon = s_mon_frame; s_lastMonRing = s_mon_ring;
    {
        static int vlogn = 0;
        if (vlogn < 40 || (vlogn & 63) == 0)
            rlog("[VKR] view=(%d,%d %dx%d) stream y=[%.0f..%.0f] draws=%d verts=%d\n",
                 dispX, dispY, dispW, dispH, s_streamMinY, s_streamMaxY,
                 (int)s_draws.size(), s_nverts);
        vlogn++;
    }
    QueryPerformanceCounter(&t1);
    {
        static double acc = 0; static int n = 0;
        acc += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_qpf.QuadPart;
        if (++n >= 120) {
            rlog("[VKR] render avg %.2f ms (%d draws, %d verts)\n",
                 acc / n, (int)s_draws.size(), s_nverts);
            acc = 0; n = 0;
        }
    }
    {   /* 09.10.2026: DC_VK_STAGES=1 - раз в 120 кадров среднее по этапам: подготовка и запись команд / отправка и ожидание
         * GPU / хвост (снимок для кадра B), плюс время GPU по меткам (основной проход, после него) */
        static int st = -1; static double a1 = 0, a2 = 0, a3 = 0, g[6] = { 0, 0, 0, 0, 0, 0 }; static int sn = 0, gn = 0;
        static double gd[7] = { 0, 0, 0, 0, 0, 0, 0 }; static int gdn = 0;   /* 09.10 (2): подробности (pp_ts_detail) */
        if (st < 0) { const char* e = getenv("DC_VK_STAGES"); st = (e && *e == '1') ? 1 : 0; }
        if (st) {
            const double k = 1000.0 / (double)s_qpf.QuadPart;
            a1 += (double)(tsub.QuadPart - t0.QuadPart) * k; a2 += (double)(twait.QuadPart - tsub.QuadPart) * k;
            a3 += (double)(t1.QuadPart - twait.QuadPart) * k;
            double gs[6];
            if (pp_ts_stages(gs)) { for (int i = 0; i < 6; i++) g[i] += gs[i]; gn++; }
            double gx[7];
            if (pp_ts_detail(gx)) { for (int i = 0; i < 7; i++) gd[i] += gx[i]; gdn++; }
            if (++sn >= 120) {
                const double d = gn ? (double)gn : 1.0, dd = gdn ? (double)gdn : 1.0;
                std::fprintf(stderr, "vkstages: CPU record %.2f, submit+wait %.2f, after %.2f ms | GPU shadows+uploads %.2f, refl/glow geom %.2f, "
                             "main %.2f, post %.2f, readback %.2f, prev copy %.2f = %.2f ms | draws %d verts %d sh %d%c",
                             a1 / sn, a2 / sn, a3 / sn, g[0] / d, g[1] / d, g[2] / d, g[3] / d, g[4] / d, g[5] / d,
                             (g[0] + g[1] + g[2] + g[3] + g[4] + g[5]) / d, (int)s_draws.size(), s_nverts, s_shN, 10);
                std::fprintf(stderr, "vkstages2: uploads %.3f, lamp maps %.3f, moon %.3f, pyramids %.3f | SSAO depth %.3f, occlusion %.3f, "
                             "blur+multiply %.3f ms%c", gd[0] / dd, gd[1] / dd, gd[2] / dd, gd[3] / dd, gd[4] / dd, gd[5] / dd, gd[6] / dd, 10);
                a1 = a2 = a3 = 0; sn = 0; for (int i = 0; i < 6; i++) g[i] = 0; gn = 0;
                for (int i = 0; i < 7; i++) gd[i] = 0; gdn = 0;
            }
        }
    }
    return (const unsigned int*)s_rbMap;
}

/* ⭐ 17.09.2026 вечер: кадр B (см. WarpSnap выше). nullptr — показывать прежнюю картинку. */
extern "C" int dc_nots_warp_cams(float Nr[9], float er[3], float tr[3], float Nn[9], float en_[3], float tn[3],
                                 unsigned long long* flip_r, unsigned long long* flip_n);
static void wq_from_m(const double m[9], double q[4]) {   /* строки m: единичная матрица поворота -> кватернион (x,y,z,w) */
    double tr = m[0] + m[4] + m[8];
    if (tr > 0.0) {
        double s = sqrt(tr + 1.0) * 2.0;
        q[3] = 0.25 * s; q[0] = (m[7] - m[5]) / s; q[1] = (m[2] - m[6]) / s; q[2] = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        double s = sqrt(1.0 + m[0] - m[4] - m[8]) * 2.0;
        q[0] = 0.25 * s; q[1] = (m[3] + m[1]) / s; q[2] = (m[2] + m[6]) / s; q[3] = (m[7] - m[5]) / s;
    } else if (m[4] > m[8]) {
        double s = sqrt(1.0 + m[4] - m[0] - m[8]) * 2.0;
        q[0] = (m[3] + m[1]) / s; q[1] = 0.25 * s; q[2] = (m[7] + m[5]) / s; q[3] = (m[2] - m[6]) / s;
    } else {
        double s = sqrt(1.0 + m[8] - m[0] - m[4]) * 2.0;
        q[0] = (m[2] + m[6]) / s; q[1] = (m[7] + m[5]) / s; q[2] = 0.25 * s; q[3] = (m[3] - m[1]) / s;
    }
    double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0) for (int i = 0; i < 4; i++) q[i] /= n;
    if (q[3] < 0.0) for (int i = 0; i < 4; i++) q[i] = -q[i];
}
static void wq_to_m(const double q[4], double m[9]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w);     m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);     m[7] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}
static int s_warp_en = -1;
/* кадр B ждёт показа: снимок мира есть и ещё не показан (main.cpp — не гасить такую развёртку удержанием «без мира») */
extern "C" int dcvk_warp_pending(void) {
    if (s_warp_en < 0) { const char* e = getenv("DC_FRAME_WARP"); s_warp_en = (e && *e == '0') ? 0 : 1; }
    int r = s_warp_en && s_wsnap.valid && !s_wsnap.used && s_wsnap.world && s_nverts == 0;
    { static int dbg = -1; static int last = -1;
      if (dbg < 0) { const char* e = getenv("DC_WARP_DBG"); dbg = (e && *e == '1') ? 1 : 0; }
      int why = r ? 0 : (!s_wsnap.valid ? 1 : s_wsnap.used ? 2 : !s_wsnap.world ? 3 : s_nverts ? 4 : 5);
      if (dbg && why != last && why != 2) {
          std::fprintf(stderr, "warp: pending %d (reason %d: 1 no snap, 2 used, 3 not world, 4 stream %d verts) frame %llu%c",
                       r, why, s_nverts, s_wsnap.frame, 10); }
      if (why != 2) last = why; }
    return r;
}
/* ⭐ 04.10.2026: кадр A нарисован, но не показан (удержание после рендера: камера сменилась, доворот не сделан и т.п.) — кадр B
 * из его снимка тоже не показываем, иначе на следующей развёртке на экран выходит ровно то, что прятали (слот 43, конец сцены:
 * вид камеры зоны сверху на f=881–883 — кадр B снимка f=880, удержанного правилом takeover). */
extern "C" void dcvk_warp_drop(void) { s_wsnap.used = 1; s_wsnap.dropped = 1; }
/* ⭐ 10.10.2026 (игрок: «нужно, чтобы игра работала плавно, в 60, а то и в 120 FPS»; копия для моддинга) — КАДРЫ С ЧАСТОТОЙ
 * МОНИТОРА. Снимок кадра A теперь можно перерисовывать много раз и с любой долей пути камеры (alpha >= 0; < 0 - доля из
 * DC_FRAME_WARP_ALPHA, как раньше): main.cpp показывает кадры между тактами игры на каждом обновлении монитора, камера - на
 * точный момент показа. extra = 1: дополнительный кадр - снимок не помечается показанным (кадр B такта рисуется как раньше),
 * удержанный кадр A (dcvk_warp_drop) не перерисовывается. Есть ли снимок для дополнительного кадра - dcvk_warp_ready(). */
extern "C" int dcvk_warp_ready(void) {
    if (s_warp_en < 0) { const char* e = getenv("DC_FRAME_WARP"); s_warp_en = (e && *e == '0') ? 0 : 1; }
    return s_warp_en && s_wsnap.valid && !s_wsnap.dropped && s_wsnap.world && s_nverts == 0 && s_wsnap.nverts > 0 && s_vmap;
}
/* 26.10.2026: кадр B такта ещё не показан, а вершины кадра A - в копии (s_wsVb): главный поток рисует его и тогда, когда поток
 * кадра уже пишет следующий кадр игры (придержанная или отложенная развёртка, main.cpp s_vb_held) */
extern "C" int dcvk_warp_copy_ok(void) {
    if (s_warp_en < 0) { const char* e = getenv("DC_FRAME_WARP"); s_warp_en = (e && *e == '0') ? 0 : 1; }
    return s_warp_en && s_wsnap.valid && !s_wsnap.used && !s_wsnap.dropped && s_wsnap.world && s_wsnap.vcopy && s_wsnap.nverts > 0;
}
static const unsigned int* warp_render_impl(float alpha_in, int extra, const WarpIn* in_pt);
extern "C" int dc_tank_warp_rigid(void);     /* 15.10: memory.c - камера Tank / RE4 идёт за её поворотом */
extern "C" int dc_nots_warp_turn(float* dcam, float* dher);   /* 19.10: memory.c - поворот камеры и её самой между Dp и D */
extern "C" int dc_strafe_body_yaw(int* yaw);                   /* 20.10: pt_anim.c - корпус рисует стрейф (не по курсу логики) */
extern "C" const unsigned int* dcvk_render_warp(void) { return warp_render_impl(-1.f, 0, nullptr); }
extern "C" const unsigned int* dcvk_render_warp_at(float alpha, int extra) { return warp_render_impl(alpha, extra, nullptr); }
/* 10.10.2026 (кадры с частотой монитора): поправка камеры на долю пути alpha (Dp -> D) - wpc[0..11] (кватернион,
 * перенос, сдвиг Регины, признак прицела); wpc[12..15] (проекция кадра) заполняет вызывающий. Общая для кадра B,
 * дополнительных кадров и кадра A. 0 - нет камер / переход / телепорт; *stand = 1 - камера стоит. */
/* ⭐ 20.10.2026 - РУКИ, КИСТИ С ОРУЖИЕМ И ГОЛОВА МЕЖДУ ТАКТАМИ (игрок, слот 42: «в прицеле целюсь вверх-вниз - рука с
 * пистолетом стробит, двигается рывками»; «сами анимации не меняй - сделай гладким»; подробно - pt_renderer.c «ЕЁ КОСТИ ДЛЯ
 * КАДРОВ МЕЖДУ ТАКТАМИ»). Её вершины кадра A несут номер кости (in_rep.w = -(кость + 1)); у проходов отрисовки кадра A (камера
 * Dp) и следующего (камера D) есть снимки её мировых костей. Для костей 1..7 (руки, кисти с оружием, голова) - поворот и
 * сдвиг ОТНОСИТЕЛЬНО КОРПУСА (кость 0) от позы этого такта к позе следующего, доля alpha, в виде кадра A: ячейки UBO
 * 536..567 bq[b] - кватернион, 568..599 bt[b] - перенос, w = 1 - есть. Поворот корпуса и её ход - прежние (её доворот 528..535,
 * сдвиг с пивотом). Только когда она не «жёсткая» (wd.w = 0) и в прицеле (DC_ARM_WARP=2 - всегда, 0 - выкл.); нет снимка
 * следующего прохода или скачок позы больше 50° / 300 ед. за такт (перезарядка, смена стойки) - кость как была. */
extern "C" int pt_skel_get(int pass, int f, float rN[9], float trN[3]);
extern "C" int pt_skel_get_view(int pass, int f, float r9[9], float tr3[3]);   /* 21.10 (2): кость в виде камеры прохода */
extern "C" int pt_skel_get_delta(int pass, float D[9], float T[3]);            /* 21.10 (3): переприцел этого вида в нашу камеру */
extern "C" int pt_arm_warp_mode(void);
extern "C" int dc_nots_delta_pass(int prev);
static int arm_warp_vmode(void) {                                /* 21.10 (2): DC_ARM_WARP_VIEW=0 - кости «миром» */
    static int vmode = -1;
    if (vmode < 0) { const char* e = getenv("DC_ARM_WARP_VIEW"); vmode = (e && *e == '0') ? 0 : 1; }
    return vmode;
}
static void warp_in_orig(WarpIn* w);                             /* 26.10: тип Original - камеры игры (ниже) */
static void warp_in_take(WarpIn* w) {                            /* 25.10: все живые входы warp_pc_make / arm_warp_make */
    memset(w, 0, sizeof *w);
    w->cams = dc_nots_warp_cams(w->Nr, w->er, w->tr, w->Nn, w->en3, w->tn, &w->fr, &w->fn);
    const uint8_t* wr = memory_get_ram_ptr();
    w->aim = (g_dc_aiming || (wr && wr[0xB92C9] == 6)) ? 1 : 0;
    w->tank = dc_tank_warp_rigid();
    int sby = 0; w->strafe = dc_strafe_body_yaw(&sby);
    w->turn = dc_nots_warp_turn(&w->dcam, &w->dher);
    w->amode = pt_arm_warp_mode();
    w->pA = dc_nots_delta_pass(1); w->pN = dc_nots_delta_pass(0);
    const int vmode = arm_warp_vmode();
    for (int f = 0; f < 16; f++) {
        if (vmode ? pt_skel_get_view(w->pA, f, w->rA[f], w->tAb[f]) : pt_skel_get(w->pA, f, w->rA[f], w->tAb[f])) w->okA |= 1u << f;
        if (w->pN == w->pA + 1 &&
            (vmode ? pt_skel_get_view(w->pN, f, w->rNb[f], w->tNb[f]) : pt_skel_get(w->pN, f, w->rNb[f], w->tNb[f]))) w->okN |= 1u << f;
    }
    w->dhA = vmode ? pt_skel_get_delta(w->pA, w->dA, w->tA) : 0;
    w->dhN = (vmode && w->pN == w->pA + 1) ? pt_skel_get_delta(w->pN, w->dNd, w->tNd) : 0;
    w->okW = pt_skel_get(w->pA, 0, w->rw, w->tw);
    if (w->cams == 2) warp_in_orig(w);
}
/* ⭐ 25.10.2026 - ВЕСЬ ЕЁ СКЕЛЕТ МЕЖДУ ТАКТАМИ (игрок: «вся анимация Регины... стробит больше, чем... при малых FPS, типа
 * 30-60 FPS. Когда 120+, смотрится более дерганно»). Кадры между тактами двигали только руки, кисти и голову и только в прицеле
 * (DC_ARM_WARP=1): ноги, таз и всё тело вне прицела стояли в позе такта и ехали с пивотом камеры - на 165 Гц 4-5 кадров
 * скольжения позой такта, затем рывок к новой позе (на 30-60 кадрах такт - 1-2 кадра, рывок мельче). Теперь доля пути от позы
 * этого такта к позе следующего - у всех 15 костей и всегда (DC_ARM_WARP=2 по умолчанию, pt_renderer.c), по иерархии (замер
 * skel_probe: длины костей постоянны): 0 корпус; 1-2-3 рука (плечо, предплечье, кисть), 4-5-6 - вторая; 7 голова; 8 таз;
 * 9-10-11 и 12-13-14 ноги (бедро, голень, стопа). Поворот кости относительно родителя - доля пути (slerp), сустав - в системе
 * родителя (там он постоянен), поворот и место родителя на этот миг уже посчитаны (номер родителя всегда меньше). Корпус - как
 * был: стоит в позе такта, едет с пивотом, курс - доворот 19.10. Повороты сперва строго ортогональные (через кватернион): при
 * доле 0 сдвиг ровно 0. В «жёстких» кадрах (прицел на ходу, камера Tank / RE4: модель держится за камерой) кости тоже идут
 * долей пути относительно корпуса (dc_raster.vert). DC_ARM_WARP_FK=0 - кости 1..7 относительно корпуса, как 20.10;
 * DC_ARM_WARP_RIGID=0 - в жёстких кадрах без костей, как было; DC_ARM_WARP=1 - только в прицеле. */
/* ⭐ 26.10.2026 - И КОРПУС (кость 0). Он стоял в позе такта: ход и курс её шли долей пути (пивот камеры, доворот 19.10), а
 * качание и наклон самой походки (замер skel_root: на ходу 1.5° за такт, до 16°; ноги висят на корпусе) приходили тактом -
 * всё тело вздрагивало вместе с ним. Теперь и он идёт долей пути от позы этого такта к позе следующего, относительно её хода и
 * курса: цель на доле 1 - корпус прохода N, переведённый в вид кадра A нашими камерами (вид N -> вид A), за вычетом доворота
 * 19.10 вокруг пивота и хода пивота (их добавит dc_raster.vert) - к доле 1 корпус встаёт ровно в позу N, рывка на такте нет.
 * В жёстких кадрах она едет с камерой - цель прямо корпус N в виде N. Скачок больше 50° / 300 ед. - корпус как был.
 * DC_ARM_WARP_TORSO=0 - как было (корпус в позе такта). Переприцел вида GTE в наш (dA) - строго ортогональный: у сырого
 * |D D^T - I| до 3e-4, и обратный перевод сдвигал движущиеся кости на (D D^T - I) tA - единицы при |tA| в тысячи. */
static const int s_bone_par[16] = { -1, 0, 1, 2, 0, 4, 5, 0, 0, 8, 9, 10, 8, 12, 13, -1 };
static inline void m3_mul(const double* a, const double* b, double* o) {    /* o = a b */
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}
static inline void m3_tmul(const double* a, const double* b, double* o) {   /* o = a^T b */
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r] * b[c] + a[3 + r] * b[3 + c] + a[6 + r] * b[6 + c];
}
static inline void m3_mult(const double* a, const double* b, double* o) {   /* o = a b^T */
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) o[r * 3 + c] = a[r * 3] * b[c * 3] + a[r * 3 + 1] * b[c * 3 + 1] + a[r * 3 + 2] * b[c * 3 + 2];
}
static inline void m3_v(const double* a, const double* v, double* o) {      /* o = a v */
    for (int r = 0; r < 3; r++) o[r] = a[r * 3] * v[0] + a[r * 3 + 1] * v[1] + a[r * 3 + 2] * v[2];
}
static inline void m3_tv(const double* a, const double* v, double* o) {     /* o = a^T v */
    for (int r = 0; r < 3; r++) o[r] = a[r] * v[0] + a[3 + r] * v[1] + a[6 + r] * v[2];
}
static void m3_ortho(const float* m, double o[9]) {                         /* строгий поворот (через кватернион) */
    double d[9], q[4];
    for (int i = 0; i < 9; i++) d[i] = m[i];
    wq_from_m(d, q); wq_to_m(q, o);
}
/* ⭐ 26.10.2026 - ТИП ORIGINAL: КАДРЫ МЕЖДУ ТАКТАМИ ПО КАМЕРЕ ИГРЫ (memory.c dc_nots_warp_cams, ответ 2). Поворот пары игры - числа
 * 1.3.12 (|R R^T - I| ~ 1e-4): здесь он строго ортогональный, и у неподвижной камеры обе пары одинаковы до бита - сдвиг мира ровно
 * 0. Переприцела нет: вид кадра - вид GTE (единичная дельта - корпус между тактами, arm_warp_make). Пивот - её корпус (кость 0) в
 * виде своего прохода: в составной комнате секции рисуются разными парами, и место из RAM ложится в вид только со своей парой;
 * корпуса нет в проходе N - ход по RAM, нет в проходе A - место из RAM как есть. Прицел, Tank, стрейф - не для камеры игры: она
 * за ней не поворачивается. */
static void warp_in_orig(WarpIn* w) {
    double o[9];
    m3_ortho(w->Nr, o); for (int i = 0; i < 9; i++) w->Nr[i] = (float)o[i];
    m3_ortho(w->Nn, o); for (int i = 0; i < 9; i++) w->Nn[i] = (float)o[i];
    w->aim = 0; w->tank = 0; w->strafe = 0;
    for (int i = 0; i < 9; i++) w->dA[i] = w->dNd[i] = (i % 4 == 0) ? 1.f : 0.f;
    for (int i = 0; i < 3; i++) w->tA[i] = w->tNd[i] = 0.f;
    w->dhA = 1; w->dhN = (w->pN == w->pA + 1) ? 1 : 0;
    if (w->okA & 1u) {
        const float hA[3] = { w->tr[0], w->tr[1], w->tr[2] }, hN[3] = { w->tn[0], w->tn[1], w->tn[2] };
        const float *a = w->tAb[0], *n = w->tNb[0];
        for (int r = 0; r < 3; r++) w->tr[r] = w->er[r] + w->Nr[r] * a[0] + w->Nr[3 + r] * a[1] + w->Nr[6 + r] * a[2];
        if (w->okN & 1u)
            for (int r = 0; r < 3; r++) w->tn[r] = w->en3[r] + w->Nn[r] * n[0] + w->Nn[3 + r] * n[1] + w->Nn[6 + r] * n[2];
        else
            for (int r = 0; r < 3; r++) w->tn[r] = w->tr[r] + (hN[r] - hA[r]);
    }
}
static int arm_warp_make(float alpha, const WarpIn& in, int rigid, float hdl, int dbg) {   /* -> сколько костей сдвинуто;
                                                                    hdl - её доворот 19.10 за весь такт (рад, 0 - его нет) */
    const float* const Nr = in.Nr; const float* const er = in.er;     /* 25.10: входы - снимком (WarpIn) */
    const unsigned long long fr = in.fr, fn = in.fn;
    if (!s_shUboMap) return 0;
    float* bq = s_shUboMap + 536; float* bt = s_shUboMap + 600;
    for (int b = 0; b < 16; b++) { bq[b * 4 + 0] = 0.f; bq[b * 4 + 1] = 0.f; bq[b * 4 + 2] = 0.f; bq[b * 4 + 3] = 1.f;
                                   bt[b * 4 + 0] = 0.f; bt[b * 4 + 1] = 0.f; bt[b * 4 + 2] = 0.f; bt[b * 4 + 3] = 0.f; }
    static int fk = -1, rig = -1;
    if (fk < 0) { const char* e = getenv("DC_ARM_WARP_FK"); fk = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_ARM_WARP_RIGID"); rig = (e && *e == '0') ? 0 : 1; }
    const int mode = in.amode;
    if (!mode || alpha <= 0.f || (rigid && !(fk && rig))) return 0;
    if (mode == 1 && !in.aim) return 0;
    const int pA = in.pA, pN = in.pN;
    /* ⭐ 21.10.2026 (2) (игрок, дев слот 0: «жмёшь прицел - голову и руку Регины разносит в разные стороны»). Снимок костей
     * был «миром» через слот камеры игры 0xBF9F4, а в нашем режиме там неподвижная камера комнаты (замер: её глаз в 18 000 ед.
     * от нашего, поворот другой) - сдвиг кости, переведённый в вид нашей камерой (Nr, er), уходил на сотни единиц, как только
     * кость поворачивалась (вход в прицел: рука вскидывается, камера едет к плечу). Стоя в прицеле поворот почти 0 - не видно.
     * Теперь всё в видовом пространстве самого прохода: кости - матрицы GTE, которыми её вершины и переведены (pt_skel_get_view),
     * поворот и сустав относительно корпуса (камера сокращается), сдвиг вершин - прямо в виде кадра A, без камер.
     * DC_ARM_WARP_VIEW=0 - как было (мир через слот, вид через Nr). */
    const int vmode = arm_warp_vmode();
    const float *r0a = in.rA[0], *t0a = in.tAb[0];
    const int okA = (int)(in.okA & 1u), okN = (int)(in.okN & 1u);   /* okN - только при pN = pA + 1 (warp_in_take) */
    const float *dA = in.dA, *tA = in.tA;
    const int dhA = in.dhA;                                      /* 21.10 (3): переприцел вида GTE прохода A в нашу камеру */
    int nb = 0; char lst[256]; int ll = 0; lst[0] = 0;
    double tor_ang = -1.0, tor_dj = -1.0;                        /* 26.10: поворот (рад) и сдвиг корпуса за такт - строка armwarp */
    static double maxang = -1.0, maxj = -1.0; static int gate_all = -1;
    if (maxang < 0.0) { const char* e = getenv("DC_ARM_WARP_MAXANG"); maxang = (e ? atof(e) : 50.0) / 57.2957795;
                        e = getenv("DC_ARM_WARP_MAXJ"); maxj = e ? atof(e) : 300.0;
                        e = getenv("DC_ARM_WARP_GATE_ALL"); gate_all = (e && *e == '1') ? 1 : 0; }
    if (okA && okN) {
        const int nbone = fk ? 15 : 8;
        double RA[16][9], RN[16][9];             /* строгие повороты костей проходов A и N */
        double Qf[16][9], JA[16][3], JN[16][3];  /* поворот кости к родителю на долю пути; сустав в системе родителя (A, N) */
        int par[16]; unsigned have = 0;
        int skip_all = 0;
        for (int f = 0; f < nbone && !skip_all; f++) {
            par[f] = -1;
            if (!((in.okA >> f) & 1u)) continue;     /* кости нет в проходе A - и её вершин в кадре нет */
            m3_ortho(in.rA[f], RA[f]);
            if ((in.okN >> f) & 1u) m3_ortho(in.rNb[f], RN[f]);
            have |= 1u << f;
            if (f == 0) continue;
            int q = fk ? s_bone_par[f] : 0;
            while (q > 0 && !((have >> q) & 1u)) q = s_bone_par[q];   /* родителя нет в проходе - ближайший предок */
            par[f] = q;
            const float *tfa = in.tAb[f], *tqa = in.tAb[q];
            const double da[3] = { (double)tfa[0] - tqa[0], (double)tfa[1] - tqa[1], (double)tfa[2] - tqa[2] };
            m3_tv(RA[q], da, JA[f]);                 /* J = Rq^T (Tf - Tq) */
            for (int i = 0; i < 9; i++) Qf[f][i] = (i % 4 == 0) ? 1.0 : 0.0;
            for (int i = 0; i < 3; i++) JN[f][i] = JA[f][i];
            if (!((in.okN >> f) & 1u) || !((in.okN >> q) & 1u)) continue;   /* нет в проходе N - идёт за родителем */
            const float *tfn = in.tNb[f], *tqn = in.tNb[q];
            const double dn[3] = { (double)tfn[0] - tqn[0], (double)tfn[1] - tqn[1], (double)tfn[2] - tqn[2] };
            double jn[3]; m3_tv(RN[q], dn, jn);
            double LA[9], LN[9], DL[9];
            m3_tmul(RA[q], RA[f], LA); m3_tmul(RN[q], RN[f], LN);    /* L = Rq^T Rf - кость в системе родителя */
            m3_mult(LN, LA, DL);                                       /* dL = LN LA^T */
            double q4[4]; wq_from_m(DL, q4);
            const double ang = 2.0 * acos(q4[3] > 1.0 ? 1.0 : q4[3]);
            const double dj = sqrt((jn[0] - JA[f][0]) * (jn[0] - JA[f][0]) + (jn[1] - JA[f][1]) * (jn[1] - JA[f][1]) + (jn[2] - JA[f][2]) * (jn[2] - JA[f][2]));
            if (ang > maxang || dj > maxj) {         /* скачок позы - не тянем: кость идёт за родителем */
                if (gate_all) {                      /* DC_ARM_WARP_GATE_ALL: одна большая кость - не тянем ни одной */
                    skip_all = 1;
                    if (dbg && ll < 230) ll += snprintf(lst + ll, sizeof(lst) - (size_t)ll, " all-skip %d:%.0f/%.0f", f, ang * 57.2957795, dj);
                    break;
                }
                if (dbg && ll < 230) ll += snprintf(lst + ll, sizeof(lst) - (size_t)ll, " %d:x%.0f/%.0f", f, ang * 57.2957795, dj);
                continue;
            }
            double qa[4];
            { const double s = sin(ang * 0.5);
              if (s > 1e-9) { const double k = sin(0.5 * ang * alpha) / s; qa[0] = q4[0] * k; qa[1] = q4[1] * k; qa[2] = q4[2] * k; qa[3] = cos(0.5 * ang * alpha); }
              else { qa[0] = q4[0] * alpha; qa[1] = q4[1] * alpha; qa[2] = q4[2] * alpha; qa[3] = 1.0; }
              const double n = sqrt(qa[0] * qa[0] + qa[1] * qa[1] + qa[2] * qa[2] + qa[3] * qa[3]); for (int i = 0; i < 4; i++) qa[i] /= n; }
            wq_to_m(qa, Qf[f]);
            for (int i = 0; i < 3; i++) JN[f][i] = jn[i];
            if (dbg && ll < 230) ll += snprintf(lst + ll, sizeof(lst) - (size_t)ll, " %d:%.1f/%.0f", f, ang * 57.2957795, dj);
        }
        if (!skip_all) {
            double M[16][9], T[16][3];               /* поворот её вершин кости и сустав кости на этот миг (вид прохода A) */
            for (int i = 0; i < 9; i++) M[0][i] = (i % 4 == 0) ? 1.0 : 0.0;
            for (int i = 0; i < 3; i++) T[0][i] = t0a[i];
            /* 26.10: корпус - доля пути относительно её хода и курса (шапка выше) */
            static int torso_on = -1;
            if (torso_on < 0) { const char* e = getenv("DC_ARM_WARP_TORSO"); torso_on = (e && *e == '0') ? 0 : 1; }
            if (torso_on && fk && vmode && dhA && in.dhN && ((in.okN >> 0) & 1u)) {
                double Do[9], DNo[9], RAv[9], RNv[9], tav[3], tnv[3], x[3];
                m3_ortho(dA, Do); m3_ortho(in.dNd, DNo);
                m3_mul(Do, RA[0], RAv); m3_mul(DNo, RN[0], RNv);          /* корпус в нашем виде своего кадра */
                { const double a3[3] = { t0a[0], t0a[1], t0a[2] }, n3[3] = { in.tNb[0][0], in.tNb[0][1], in.tNb[0][2] };
                  m3_v(Do, a3, x);  for (int r = 0; r < 3; r++) tav[r] = x[r] + tA[r];
                  m3_v(DNo, n3, x); for (int r = 0; r < 3; r++) tnv[r] = x[r] + in.tNd[r]; }
                double Rt[9], Tt[3];                 /* где корпус на доле 1 - до её доворота и хода (их добавит dc_raster.vert) */
                if (rigid) { for (int i = 0; i < 9; i++) Rt[i] = RNv[i]; for (int r = 0; r < 3; r++) Tt[r] = tnv[r]; }
                else {
                    double NrD[9], NnD[9], K[9], H[9], t1[9], RNa[9], tna[3], pa[3], pn[3], ke[3];
                    for (int i = 0; i < 9; i++) { NrD[i] = Nr[i]; NnD[i] = in.Nn[i]; }
                    m3_mult(NrD, NnD, K);                /* вид N -> вид A: v_A = Nr Nn^T v_N + Nr (eN - eA) */
                    const double ee[3] = { (double)in.en3[0] - er[0], (double)in.en3[1] - er[1], (double)in.en3[2] - er[2] };
                    m3_v(NrD, ee, ke);
                    m3_mul(K, RNv, RNa); m3_v(K, tnv, x); for (int r = 0; r < 3; r++) tna[r] = x[r] + ke[r];
                    const double cs = cos((double)hdl), sn = sin((double)hdl);
                    const double Ry[9] = { cs, 0.0, sn,   0.0, 1.0, 0.0,   -sn, 0.0, cs };
                    m3_mul(NrD, Ry, t1); m3_mult(t1, NrD, H);   /* её доворот за такт в виде A (как warp_pc_make) */
                    const double a3[3] = { (double)in.tr[0] - er[0], (double)in.tr[1] - er[1], (double)in.tr[2] - er[2] };
                    const double n3[3] = { (double)in.tn[0] - er[0], (double)in.tn[1] - er[1], (double)in.tn[2] - er[2] };
                    m3_v(NrD, a3, pa); m3_v(NrD, n3, pn);   /* пивот камеры A и N в виде A */
                    m3_tmul(H, RNa, Rt);                     /* Rt = H^T RN */
                    const double d3[3] = { tna[0] - pn[0], tna[1] - pn[1], tna[2] - pn[2] };
                    m3_tv(H, d3, x); for (int r = 0; r < 3; r++) Tt[r] = pa[r] + x[r];   /* Tt = PA + H^T (TN - PN) */
                }
                double DR[9], q4[4]; m3_mult(Rt, RAv, DR); wq_from_m(DR, q4);   /* поворот корпуса за такт, вид A */
                tor_ang = 2.0 * acos(q4[3] > 1.0 ? 1.0 : q4[3]);
                tor_dj = sqrt((Tt[0] - tav[0]) * (Tt[0] - tav[0]) + (Tt[1] - tav[1]) * (Tt[1] - tav[1]) + (Tt[2] - tav[2]) * (Tt[2] - tav[2]));
                if (tor_ang > maxang || tor_dj > maxj) {
                    if (dbg && ll < 230) ll += snprintf(lst + ll, sizeof(lst) - (size_t)ll, " 0:x%.0f/%.0f", tor_ang * 57.2957795, tor_dj);
                } else {
                    double qa[4], Ra[9], t2[9];
                    const double s = sin(tor_ang * 0.5);
                    if (s > 1e-9) { const double k = sin(0.5 * tor_ang * alpha) / s; qa[0] = q4[0] * k; qa[1] = q4[1] * k; qa[2] = q4[2] * k; qa[3] = cos(0.5 * tor_ang * alpha); }
                    else { qa[0] = q4[0] * alpha; qa[1] = q4[1] * alpha; qa[2] = q4[2] * alpha; qa[3] = 1.0; }
                    const double n = sqrt(qa[0] * qa[0] + qa[1] * qa[1] + qa[2] * qa[2] + qa[3] * qa[3]); for (int i = 0; i < 4; i++) qa[i] /= n;
                    wq_to_m(qa, Ra);
                    m3_tmul(Do, Ra, t2); m3_mul(t2, Do, M[0]);              /* в вид GTE прохода A (там цепочка): M0 = D^T Ra D */
                    const double y[3] = { alpha * (Tt[0] - tav[0]), alpha * (Tt[1] - tav[1]), alpha * (Tt[2] - tav[2]) };
                    m3_tv(Do, y, x); for (int r = 0; r < 3; r++) T[0][r] = t0a[r] + x[r];   /* T0 = T0A + D^T a (Tt - TA) */
                    if (dbg && ll < 230) ll += snprintf(lst + ll, sizeof(lst) - (size_t)ll, " 0:%.1f/%.0f", tor_ang * 57.2957795, tor_dj);
                }
            }
            for (int f = 0; f < nbone; f++) {
                if (f > 0) {
                    const int q = par[f];
                    if (q < 0) continue;
                    double t1[9], t2[9];             /* M_f = M_q Rq Qf Rq^T */
                    m3_mul(RA[q], Qf[f], t1); m3_mult(t1, RA[q], t2); m3_mul(M[q], t2, M[f]);
                    double y[3], y2[3], y3[3];       /* T_f = T_q + M_q Rq (JA + a (JN - JA)) */
                    for (int i = 0; i < 3; i++) y[i] = JA[f][i] + (double)alpha * (JN[f][i] - JA[f][i]);
                    m3_v(RA[q], y, y2); m3_v(M[q], y2, y3);
                    for (int i = 0; i < 3; i++) T[f][i] = T[q][i] + y3[i];
                }
                const float* tfa = in.tAb[f];
                const double tf3[3] = { tfa[0], tfa[1], tfa[2] };
                double mt[3], cw[3];                 /* c = T_f - M_f T_fA: вершина u кадра A -> M_f u + c */
                m3_v(M[f], tf3, mt);
                for (int i = 0; i < 3; i++) cw[i] = T[f][i] - mt[i];
                { double mq0[4]; wq_from_m(M[f], mq0);   /* кость и её предки стоят - вершины как есть */
                  const double am = 2.0 * acos(mq0[3] > 1.0 ? 1.0 : mq0[3]);
                  const double dsp = sqrt((T[f][0] - tf3[0]) * (T[f][0] - tf3[0]) + (T[f][1] - tf3[1]) * (T[f][1] - tf3[1]) + (T[f][2] - tf3[2]) * (T[f][2] - tf3[2]));
                  if (am * 400.0 + dsp < 0.02) continue; }
                if (vmode) {                         /* 21.10 (2): M и c - в виде GTE прохода A (камера игры) */
                    /* 21.10 (3): её вершины кадра A - вид GTE, переприцеленный в нашу камеру: v = D u + T (сборщик тела
                     * pt_renderer.c). u' = M u + c  =>  v' = (D M D^T) v + D c + T - (D M D^T) T. Нет дельты - вид кадра = вид GTE. */
                    double Mo[9], co[3];
                    if (dhA) {
                        double D[9], T3[9], dc3[3], mo3[3]; const double tA3[3] = { tA[0], tA[1], tA[2] };
                        m3_ortho(dA, D);             /* 26.10: строго ортогональный (шапка 26.10 выше) */
                        m3_mul(D, M[f], T3); m3_mult(T3, D, Mo);
                        m3_v(D, cw, dc3); m3_v(Mo, tA3, mo3);
                        for (int r = 0; r < 3; r++) co[r] = dc3[r] + tA3[r] - mo3[r];
                    } else { for (int i = 0; i < 9; i++) Mo[i] = M[f][i]; for (int r = 0; r < 3; r++) co[r] = cw[r]; }
                    double mq[4]; wq_from_m(Mo, mq);
                    for (int i = 0; i < 4; i++) bq[f * 4 + i] = (float)mq[i];
                    for (int r = 0; r < 3; r++) bt[f * 4 + r] = (float)co[r];
                } else {
                    /* вид кадра A: v' = (Nr M Nr^T) v + Nr (M er + c - er) */
                    double N3[9], T2[9], Mv[9], me[3], u3[3], o3[3]; const double e3[3] = { er[0], er[1], er[2] };
                    for (int i = 0; i < 9; i++) N3[i] = Nr[i];
                    m3_mul(N3, M[f], T2); m3_mult(T2, N3, Mv);
                    m3_v(M[f], e3, me);
                    for (int r = 0; r < 3; r++) u3[r] = me[r] + cw[r] - e3[r];
                    m3_v(N3, u3, o3);
                    double mq[4]; wq_from_m(Mv, mq);
                    for (int i = 0; i < 4; i++) bq[f * 4 + i] = (float)mq[i];
                    for (int r = 0; r < 3; r++) bt[f * 4 + r] = (float)o3[r];
                }
                bt[f * 4 + 3] = 1.f;
                nb++;
            }
        }
    }
    if (dbg) { static unsigned long long lfn = ~0ull; static int la = -1;
        const int ai = (int)(alpha * 100.f + 0.5f);
        if (lfn != fn || la != ai) { lfn = fn; la = ai;
            auto ortho = [](const float* m) { double e = 0.0;          /* max |R R^T - I| */
                for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
                    const double v = (double)m[r * 3] * m[c * 3] + (double)m[r * 3 + 1] * m[c * 3 + 1] + (double)m[r * 3 + 2] * m[c * 3 + 2] - (r == c ? 1.0 : 0.0);
                    if (fabs(v) > e) e = fabs(v); }
                return e; };
            std::fprintf(stderr, "armwarp: flips %llu->%llu alpha %.2f passes %d->%d skel %d/%d delta %d (ortho R0 %.4f D %.4f) bones %d%s masks %04x/%04x fk %d rigid %d"
                                 " torso %.2f/%.1f turn %.4f tA %.0f%c",
                         fr, fn, alpha, pA, pN, okA, okN, dhA, okA ? ortho(r0a) : -1.0, dhA ? ortho(dA) : -1.0, nb, lst,
                         in.okA & 0xFFFFu, in.okN & 0xFFFFu, fk, rigid,
                         tor_ang * 57.2957795, tor_dj, (double)hdl, sqrt((double)tA[0] * tA[0] + (double)tA[1] * tA[1] + (double)tA[2] * tA[2]), 10);
            /* 21.10 (3) замер: корпус кадра A в нашем виде - как его собрал сборщик (вид GTE + дельта) и как считала формула
             * 20.10 (мир через слот камеры игры, затем наша камера кадра A Nr/er) */
            const float *rw = in.rw, *tw = in.tw;
            if (vmode && okA && dhA && in.okW) {
                double pv[3], po[3], rv = 0.0;
                for (int r = 0; r < 3; r++) {
                    pv[r] = (double)dA[r * 3] * t0a[0] + (double)dA[r * 3 + 1] * t0a[1] + (double)dA[r * 3 + 2] * t0a[2] + tA[r];
                    po[r] = (double)Nr[r * 3] * (tw[0] - er[0]) + (double)Nr[r * 3 + 1] * (tw[1] - er[1]) + (double)Nr[r * 3 + 2] * (tw[2] - er[2]);
                    for (int c = 0; c < 3; c++) {      /* D r0a против Nr rN */
                        const double a = (double)dA[r * 3] * r0a[c] + (double)dA[r * 3 + 1] * r0a[3 + c] + (double)dA[r * 3 + 2] * r0a[6 + c];
                        const double b = (double)Nr[r * 3] * rw[c] + (double)Nr[r * 3 + 1] * rw[3 + c] + (double)Nr[r * 3 + 2] * rw[6 + c];
                        if (fabs(a - b) > rv) rv = fabs(a - b); }
                }
                std::fprintf(stderr, "armwarp: torso in frame A view - assembler %.0f,%.0f,%.0f | 20.10 formula %.0f,%.0f,%.0f | off by %.0f, turn diff %.3f%c",
                             pv[0], pv[1], pv[2], po[0], po[1], po[2],
                             sqrt((pv[0] - po[0]) * (pv[0] - po[0]) + (pv[1] - po[1]) * (pv[1] - po[1]) + (pv[2] - po[2]) * (pv[2] - po[2])), rv, 10);
            } } }
    return nb;
}
/* ⭐ 26.10.2026 - ПАРЫ КУСКОВ: КАДРЫ МЕЖДУ ТАКТАМИ У ДВЕРЕЙ (игрок: «ни сцены с открытием двери, ни катсцены... не должны менять
 * своё значение FPS»; «если игрок выставил 120 кадров, значит, будет везде 120 кадров»). Кадры между тактами двигают картинку
 * такта по нашей камере (warp_pc_make) - у дверей её нет, и там шло 30 новых кадров в секунду при любой частоте. Здесь каждый
 * кусок модели прохода кадра A (gte.cpp pair_note: подряд идущие проекции с одной матрицей, ключ - первые три вершины модели)
 * сопоставляется с тем же куском прохода N (следующий такт - к показу A он уже посчитан): точка вида N = Q·(точка вида A) + c,
 * Q = M_N·M_A^-1, c = t_N - Q·t_A. На долю пути alpha: поворот Q - доля от единицы (slerp), остаток Q (масштаб) - линейно,
 * начало куска - линейно (кусок поворачивается вокруг своего начала; при повороте камеры ошибка ~ alpha(1-alpha)/2·угол²·дальность
 * - доли пикселя). Метку куска несёт вершина (третий поток, s_pvMap), шейдер берёт строки своей пары (UBO 664.., dc_raster.vert).
 * Пара не взята (куска нет в N, скачок > 0.7 рад / 2000 ед., вырожденная матрица, масштаб > 25%) - кусок
 * стоит, как в кадре A. У кусков без пары больше 10% вершин кадра A или движется кусок за PAIR_MAX - пар нет: кадр между тактами -
 * повтор, как было. Собирает снимок кадра A (под замком видеокарты), берёт кадр между тактами. DC_PAIR_WARP=0 - выкл (main.cpp),
 * DC_PAIR_DBG=1 - строка pairwarp: на снимок. */
struct PairBase { double q[4], S[9], tA[3], tN[3]; int on; };
struct PairSet { int ok = 0, passA = -1, count = 0, moving = 0; unsigned long long frame = 0; PairBase p[PAIR_MAX]; };
static PairSet s_pairSet;
static void m3_inv(const double* m, double* o, double* det) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double A = e * i - f * h, B = -(d * i - f * g), Cc = d * h - e * g;
    *det = a * A + b * B + c * Cc;
    if (fabs(*det) < 1e-12) return;
    const double k = 1.0 / *det;
    o[0] = A * k;  o[1] = -(b * i - c * h) * k; o[2] = (b * f - c * e) * k;
    o[3] = B * k;  o[4] = (a * i - c * g) * k;  o[5] = -(a * f - c * d) * k;
    o[6] = Cc * k; o[7] = -(a * h - b * g) * k; o[8] = (a * e - b * d) * k;
}
/* пара куска по его матрицам кадров A и N (pair_build): 1 - взята (b заполнена, *mv - кусок движется), 0 - скачок (поворот
 * больше 0.7 рад, сдвиг больше 2000 ед., масштаб больше 25%), -1 - матрица кадра A вырождена */
static int pair_base_make(const double* MA, const double* tA, const double* MN, const double* tN, PairBase& b, int* mv) {
    *mv = 0;
    double iA[9], det = 0.0;
    m3_inv(MA, iA, &det);
    if (fabs(det) < 1e-12) return -1;
    double Q[9]; m3_mul(MN, iA, Q);
    double R[9]; memcpy(R, Q, sizeof R);                     /* поворот Q: полярное разложение (итерации Хайэма) */
    for (int it = 0; it < 6; it++) {
        double Ri[9], d2 = 0.0; m3_inv(R, Ri, &d2);
        if (fabs(d2) < 1e-12) break;
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) R[r * 3 + c] = 0.5 * (R[r * 3 + c] + Ri[c * 3 + r]);
    }
    double q[4]; wq_from_m(R, q);
    double Ro[9]; wq_to_m(q, Ro);
    double S[9]; m3_mult(Q, Ro, S);                          /* S = Q·R^T */
    const double ang = 2.0 * acos(q[3] > 1.0 ? 1.0 : q[3]);
    double sdev = 0.0;
    for (int k = 0; k < 9; k++) { const double dv = fabs(S[k] - ((k % 4 == 0) ? 1.0 : 0.0)); if (dv > sdev) sdev = dv; }
    const double dt = sqrt((tN[0] - tA[0]) * (tN[0] - tA[0]) + (tN[1] - tA[1]) * (tN[1] - tA[1]) + (tN[2] - tA[2]) * (tN[2] - tA[2]));
    *mv = ang > 1e-5 || dt > 0.05 || sdev > 1e-5;
    if (ang > 0.7 || dt > 2000.0 || sdev > 0.25) return 0;
    memcpy(b.q, q, sizeof q); memcpy(b.S, S, sizeof S); memcpy(b.tA, tA, 3 * sizeof(double)); memcpy(b.tN, tN, 3 * sizeof(double));
    b.on = 1;
    return 1;
}
static int pair_build(int passA, unsigned long long frame) {
    PairSet& P = s_pairSet;
    P.ok = 0; P.passA = passA; P.count = 0; P.moving = 0; P.frame = frame;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("DC_PAIR_DBG"); dbg = e ? atoi(e) : 0; }   /* 2 - строка на каждый снимок */
    if (dc_pair_vetoed(passA)) {
        if (dbg) { static long nl = 0; if (++nl % 30 == 1) std::fprintf(stderr, "pairwarp: pass %d - no pairs (a shot change or unknown motion of our room)%c", passA, 10); }
        return 0;
    }
    int npA = 0, npN = 0;
    const int rA = dc_pair_pass_get(passA, &npA), rN = dc_pair_pass_get(passA + 1, &npN);
    if (rA <= 0 || rN <= 0) {
        if (dbg) { static long nl = 0; if (++nl % 60 == 1) std::fprintf(stderr, "pairwarp: pass %d runs %d, next %d (current %d) - no pairs%c",
                                                                       passA, rA, rN, g_dc_pass_id, 10); }
        return 0;
    }
    static std::vector<std::pair<uint32_t, int>> ka, kn;
    ka.clear(); kn.clear();
    { int n0 = 0; double M0[9], t0[3];                          /* dc_pair_run_get пишет M и t всегда */
      for (int i = 0; i < rA; i++) { uint32_t k = 0; if (dc_pair_run_get(passA, i, &k, &n0, M0, t0)) ka.push_back({ k, i }); }
      for (int j = 0; j < rN; j++) { uint32_t k = 0; if (dc_pair_run_get(passA + 1, j, &k, &n0, M0, t0)) kn.push_back({ k, j }); } }
    std::sort(ka.begin(), ka.end()); std::sort(kn.begin(), kn.end());
    P.count = rA < PAIR_MAX ? rA : PAIR_MAX;
    for (int i = 0; i < P.count; i++) P.p[i].on = 0;
    int matched = 0, rej = 0, beyond = 0, beyondMoving = 0, movingRej = 0, nDiff = 0;
    long vOn = 0, vStill = 0;                                    /* вершины кусков с парой и неподвижных кусков за PAIR_MAX */
    static std::vector<char> usedA, usedN;                       /* 27.10: прогоны, у которых ключ нашёлся (DC_PAIR_SAMEPLACE ниже) */
    usedA.assign((size_t)rA, 0); usedN.assign((size_t)rN, 0);
    size_t ia = 0, in = 0;
    while (ia < ka.size() && in < kn.size()) {                   /* одинаковые ключи - по порядку появления */
        if (ka[ia].first < kn[in].first) { ia++; continue; }
        if (ka[ia].first > kn[in].first) { in++; continue; }
        const int i = ka[ia].second, j = kn[in].second;
        ia++; in++;
        usedA[(size_t)i] = 1; usedN[(size_t)j] = 1;
        int nA = 0, nN = 0; uint32_t k0 = 0; double MA[9], tA[3], MN[9], tN[3];
        if (!dc_pair_run_get(passA, i, &k0, &nA, MA, tA) || !dc_pair_run_get(passA + 1, j, &k0, &nN, MN, tN)) {
            if (i < PAIR_MAX) { matched++; rej++; }
            continue;
        }
        if (nA != nN) nDiff++;          /* число вершин куска от такта к такту меняется (у двери - на 1-2): матрица та же, пара годится */
        if (i >= PAIR_MAX) {                                     /* кусок за PAIR_MAX стоит, как в кадре A, - годится, только если не движется */
            int mvb = 0;
            for (int k = 0; k < 9; k++) if (fabs(MA[k] - MN[k]) > 1e-9) mvb = 1;
            for (int k = 0; k < 3; k++) if (fabs(tA[k] - tN[k]) > 0.05) mvb = 1;
            beyond++;
            if (mvb) beyondMoving++; else vStill += nA;
            continue;
        }
        matched++;
        int mv = 0;
        const int pr = pair_base_make(MA, tA, MN, tN, P.p[i], &mv);
        if (pr < 0) { rej++; continue; }
        if (pr == 0) { rej++; if (mv) movingRej++; continue; }
        vOn += nA;
        if (mv) P.moving++;
    }
    /* ⭐ 27.10.2026 - КУСОК С НОВЫМИ ВЕРШИНАМИ (игрок, слот 43: «катсцена как будто тозмозит и дрожит. мало фпс в пролетах,
     * когда камера поворачивается»). Ключ куска - его первые три вершины, а их меняет сама игра: в облёте сцены ST10D пять
     * мелких кусков (48 вершин, спрайты лицом к камере: поворот единичный, центр едет с камерой) она перестраивает раз в два
     * такта, в крупных планах у головы (75 вершин) шевелится рот. Ключ новый - пары нет, и каждый второй такт облёта шёл
     * повтором (порог ниже: 48 из 473 - чуть больше 10%; 75 - 16%), новых картинок 90-105 из 165. Кусок без пары по ключу
     * берёт в следующем такте ближайший по сдвигу кусок без пары с тем же числом вершин, и пара считается как всегда (поворот,
     * масштаб, сдвиг; скачок - отказ): между тактами едет весь кусок, а новые вершины (рот, спрайт) приходят с тактом, как и
     * шли. Куски с парой по ключу должны покрывать хотя бы половину вершин кадра A (по ним видно движение кадра), иначе - как
     * было. DC_PAIR_SAMEPLACE=0 - как было. */
    int samePlace = 0;
    { static int sp = -1;
      if (sp < 0) { const char* e = getenv("DC_PAIR_SAMEPLACE"); sp = (e && *e == '0') ? 0 : 1; }
      if (sp) {
          struct Fb { int i, j, nA, mv; PairBase b; };
          static Fb fb[64];
          int nf = 0; long pv = 0;
          for (int i = 0; i < rA && i < P.count && nf < 64; i++) {
              if (usedA[(size_t)i]) continue;
              uint32_t k0 = 0; int nA = 0; double MA[9], tA[3];
              if (!dc_pair_run_get(passA, i, &k0, &nA, MA, tA) || nA <= 0) continue;
              int best = -1; double bd = 2000.0, MB[9] = {}, tB[3] = {};
              for (int j = 0; j < rN; j++) {
                  if (usedN[(size_t)j]) continue;
                  int nN = 0; double MN[9], tN[3];
                  if (!dc_pair_run_get(passA + 1, j, &k0, &nN, MN, tN) || nN != nA) continue;
                  const double d = sqrt((tN[0] - tA[0]) * (tN[0] - tA[0]) + (tN[1] - tA[1]) * (tN[1] - tA[1]) + (tN[2] - tA[2]) * (tN[2] - tA[2]));
                  if (d <= bd) { bd = d; best = j; memcpy(MB, MN, sizeof MB); memcpy(tB, tN, sizeof tB); }
              }
              if (best < 0) continue;
              Fb& f = fb[nf];
              f.b.on = 0;
              if (pair_base_make(MA, tA, MB, tB, f.b, &f.mv) != 1) continue;
              usedN[(size_t)best] = 1;
              f.i = i; f.j = best; f.nA = nA; nf++; pv += nA;
              if (dbg >= 2) { static long nl = 0;
                  if (++nl <= 600) std::fprintf(stderr, "pairfb: pass %d run %d -> %d n %d move %.0f turn %.3f%c", passA, i, best, nA, bd,
                                                2.0 * acos(f.b.q[3] > 1.0 ? 1.0 : f.b.q[3]), 10); }
          }
          if (nf && (vOn + vStill) * 2 >= (long)npA) {
              for (int q = 0; q < nf; q++) {
                  P.p[fb[q].i] = fb[q].b;
                  if (fb[q].mv) P.moving++;
              }
              vOn += pv; samePlace = nf;
          } else {
              for (int q = 0; q < nf; q++) usedN[(size_t)fb[q].j] = 0;
          }
      } }
    /* кусок без пары стоит в кадре между тактами, пока соседи едут, - дрожь: таких вершин больше 10% кадра A - пар нет (повтор) */
    P.ok = matched > 0 && !beyondMoving && (vOn + vStill) * 10 >= (long)npA * 9;
    if (dbg >= 3 && !P.ok) { static long nd = 0;   /* 27.10 (проба): прогоны обоих проходов - у каких кусков нет пары */
        if (++nd <= 12) {
            for (int side = 0; side < 2; side++) {
                const int ps = passA + side, rc = side ? rN : rA;
                for (int i = 0; i < rc && i < 64; i++) {
                    uint32_t k = 0; int n = 0; double M[9], t[3];
                    if (!dc_pair_run_get(ps, i, &k, &n, M, t)) continue;
                    int seen = 0;
                    const auto& other = side ? ka : kn;
                    for (const auto& o : other) if (o.first == k) { seen = 1; break; }
                    std::fprintf(stderr, "pairruns: pass %d %s run %d key %08X n %d t %.0f,%.0f,%.0f M0 %.4f,%.4f,%.4f %s%c", ps, side ? "N" : "A", i, k, n,
                                 t[0], t[1], t[2], M[0], M[1], M[2], seen ? "" : "<- no match", 10);
                }
            }
        } }
    /* 26.10.2026: то же по потоку кадра A - вершины мира (с глубиной, без интерфейса) с меткой своей пары. Нашу комнату в сцене
     * (pt_renderer.c rl_ptag_of) порог выше не видит: проекций GTE у неё нет. Меш из mods в кадре - пар нет: его вершины меток
     * не несут (dc_mesh.vert), он стоял бы, пока мир едет. DC_PAIR_COVER=0 - без этой проверки. */
    long cw = 0, co = 0; int cmesh = 0;
    { static int cov_on = -1;
      if (cov_on < 0) { const char* e = getenv("DC_PAIR_COVER"); cov_on = (e && *e == '0') ? 0 : 1; }
      if (cov_on && P.ok && s_pvMap && s_vmap) {
          for (const Draw& d : s_wsnap.draws) {
              if (d.ui) continue;
              if (d.mesh >= 0) { cmesh++; continue; }
              for (int k = 0; k < d.count; k++) {
                  const size_t vi = (size_t)(d.first + k);
                  if (s_vmap[vi * VERT_FLOATS + 12] < 1.5f) continue;
                  cw++;
                  const uint32_t t = s_pvMap[vi];
                  const int ix = (int)(t & 0x1FFFu) - 1;
                  if (t && (int)(t >> 13) == (passA & 3) && ix >= 0 && (ix >= P.count || P.p[ix].on)) co++;   /* за PAIR_MAX - стоят (beyondMoving = 0) */
              }
          }
          if (cmesh || co * 10 < cw * 9) P.ok = 0;
      } }
    if (dbg) { static int last = -2; static long nl = 0;
        if (P.ok != last || ++nl % 120 == 0 || dbg >= 2) { last = P.ok;
            std::fprintf(stderr, "pairwarp: pass %d runs %d (proj %d), next %d (proj %d, current %d): matched %d (other vertex count %d), "
                                 "rejected %d (moving %d), beyond %d (moving %d), verts paired %ld still %ld, moving %d, stream %ld of %ld "
                                 "(mods meshes %d), same place %d -> %s%c", passA, rA, npA, rN, npN, g_dc_pass_id, matched, nDiff, rej, movingRej, beyond,
                                 beyondMoving, vOn, vStill, P.moving, co, cw, cmesh, samePlace, P.ok ? "pairs" : "off", 10); } }
    if (dbg) { static long nc = 0;   /* 26.10 (проба): сколько вершин мира кадра A несут метку своей пары */
        if (++nc % 30 == 1 && s_pvMap && s_vmap) {
            long wv = 0, tg = 0, own = 0, rv = 0, rt = 0, av = 0, at = 0; int md = 0;
            struct Hk { int kind, fl, tp, late, room; long n; float x, y, w; };
            Hk hk[32]; int nh = 0;                            /* вершины без своей пары: вид draw, флаги, страница, поздний */
            for (const Draw& d : s_wsnap.draws) {
                if (d.ui) continue;
                if (d.mesh >= 0) { md++; continue; }
                for (int k = 0; k < d.count; k++) {
                    const size_t vi = (size_t)(d.first + k);
                    const float* v = s_vmap + vi * VERT_FLOATS;
                    if (v[12] < 1.5f) continue;
                    wv++;
                    const int act = ((int)v[11] & 16) != 0;
                    if (d.room) rv++;
                    if (act) av++;
                    const uint32_t t = s_pvMap[vi];
                    const int ix = (int)(t & 0x1FFFu) - 1;
                    const int mine = t && (int)(t >> 13) == (passA & 3) && ix >= 0 && (ix >= P.count || P.p[ix].on);
                    if (t) { tg++; if (d.room) rt++; if (act) at++; }
                    if (mine) { own++; continue; }
                    const int fl = (int)v[11], tp = (int)v[8];
                    int h = 0;
                    while (h < nh && !(hk[h].kind == d.kind && hk[h].fl == fl && hk[h].tp == tp && hk[h].late == d.late && hk[h].room == d.room)) h++;
                    if (h == nh && nh < 32) { hk[nh] = Hk{ d.kind, fl, tp, d.late, d.room, 0, v[0], v[1], v[12] }; nh++; }
                    if (h < nh) hk[h].n++;
                }
            }
            std::fprintf(stderr, "paircover: frame %llu pass %d world verts %ld tagged %ld own pair %ld (our room %ld tagged %ld, hers %ld tagged %ld), "
                                 "mesh draws %d -> %s%c", frame, passA, wv, tg, own, rv, rt, av, at, md, P.ok ? "pairs" : "off", 10);
            for (int r = 0; r < 6 && nh > 0; r++) {
                int b = 0;
                for (int h = 1; h < nh; h++) if (hk[h].n > hk[b].n) b = h;
                if (hk[b].n <= 0) break;
                std::fprintf(stderr, "paircover:   no own pair %ld verts: kind %d flags %d tpage %d late %d room %d (e.g. %.1f,%.1f w %.1f)%c",
                             hk[b].n, hk[b].kind, hk[b].fl, hk[b].tp, hk[b].late, hk[b].room, hk[b].x, hk[b].y, hk[b].w, 10);
                hk[b].n = -hk[b].n;
            }
        } }
    return P.ok;
}
/* строки пар на долю пути alpha (pr - PAIR_MAX * 12 чисел); wpc - единичная камера с признаком перепроецирования. 0 - пар нет */
static int pair_pc_make(float alpha, float wpc[16], float* pr, int* stand) {
    const PairSet& P = s_pairSet;
    *stand = 0;
    if (!P.ok) return 0;
    for (int i = 0; i < 16; i++) wpc[i] = 0.f;
    wpc[3] = 1.f; wpc[7] = 1.f;
    const double a = alpha < 0.f ? 0.0 : alpha > 1.f ? 1.0 : (double)alpha;
    for (int i = 0; i < P.count; i++) {
        float* r = pr + (size_t)i * 12;
        const PairBase& b = P.p[i];
        if (!b.on) {
            for (int k = 0; k < 12; k++) r[k] = 0.f;
            r[0] = r[5] = r[10] = 1.f;
            continue;
        }
        const double ang = 2.0 * acos(b.q[3] > 1.0 ? 1.0 : b.q[3]), s = sin(ang * 0.5);
        double qa[4];
        if (s > 1e-9) { const double k = sin(0.5 * ang * a) / s; qa[0] = b.q[0] * k; qa[1] = b.q[1] * k; qa[2] = b.q[2] * k; qa[3] = cos(0.5 * ang * a); }
        else { qa[0] = b.q[0] * a; qa[1] = b.q[1] * a; qa[2] = b.q[2] * a; qa[3] = 1.0; }
        { const double n = sqrt(qa[0] * qa[0] + qa[1] * qa[1] + qa[2] * qa[2] + qa[3] * qa[3]); for (int k = 0; k < 4; k++) qa[k] /= n; }
        double Ra[9]; wq_to_m(qa, Ra);
        double L[9];
        for (int k = 0; k < 9; k++) L[k] = ((k % 4 == 0) ? 1.0 : 0.0) * (1.0 - a) + b.S[k] * a;
        double M[9]; m3_mul(L, Ra, M);
        for (int row = 0; row < 3; row++) {
            const double tm = b.tA[row] + (b.tN[row] - b.tA[row]) * a;
            const double c = tm - (M[row * 3] * b.tA[0] + M[row * 3 + 1] * b.tA[1] + M[row * 3 + 2] * b.tA[2]);
            r[row * 4] = (float)M[row * 3]; r[row * 4 + 1] = (float)M[row * 3 + 1]; r[row * 4 + 2] = (float)M[row * 3 + 2]; r[row * 4 + 3] = (float)c;
        }
    }
    if (!P.moving) *stand = 1;
    return 1;
}
/* main.cpp: у снимка кадра A есть пары кусков (кадры между тактами без нашей камеры - двери) */
extern "C" int dcvk_pair_ready(void) { return s_wsnap.valid && !s_wsnap.dropped && s_wsnap.world && s_wsnap.pairs; }
static int warp_pc_make(float alpha, float wpc[16], int* stand, int dbg, double* o_ang, double* o_de, double* o_dt,
                        unsigned long long* o_fr, unsigned long long* o_fn, const WarpIn* in_) {
    *stand = 0;
    WarpIn loc;                                                      /* 25.10: главный поток - снимок прямо сейчас, как было */
    if (!in_) { warp_in_take(&loc); in_ = &loc; }
    const WarpIn& in = *in_;
    if (!in.cams) return 0;
    const float *Nr = in.Nr, *er = in.er, *tr = in.tr, *Nn = in.Nn, *en3 = in.en3, *tn = in.tn;
    const unsigned long long fr = in.fr, fn = in.fn;
    /* относительный поворот Q = Nn.Nr^T, его доля alpha; перенос и пивот — линейно */
    double Q[9];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            Q[r * 3 + c] = (double)Nn[r * 3] * Nr[c * 3] + (double)Nn[r * 3 + 1] * Nr[c * 3 + 1] + (double)Nn[r * 3 + 2] * Nr[c * 3 + 2];
    double q[4]; wq_from_m(Q, q);
    double ang = 2.0 * acos(q[3] > 1.0 ? 1.0 : q[3]);
    double de = sqrt((double)(en3[0] - er[0]) * (en3[0] - er[0]) + (double)(en3[1] - er[1]) * (en3[1] - er[1]) + (double)(en3[2] - er[2]) * (en3[2] - er[2]));
    double dt = sqrt((double)(tn[0] - tr[0]) * (tn[0] - tr[0]) + (double)(tn[1] - tr[1]) * (tn[1] - tr[1]) + (double)(tn[2] - tr[2]) * (tn[2] - tr[2]));
    const int og = in.cams == 2 && (ang > 0.35 || de > 1200.0);   /* 26.10: камера игры (тип Original) за такт столько не ходит */
    if (fn == fr || ang > 0.7 || de > 1800.0 || dt > 700.0 || og) {   /* нет следующей камеры / переход / телепорт */
        if (dbg) std::fprintf(stderr, "warp: skip flips %llu->%llu ang %.3f eye %.0f pivot %.0f%c", fr, fn, ang, de, dt, 10);
        return 0;
    }
    if (ang < 1e-6 && de < 0.05 && dt < 0.05) *stand = 1;              /* камера стоит */
    double qa[4];
    { double half = 0.5 * ang * alpha, s = sin(ang * 0.5);
      if (s > 1e-9) { double k = sin(half) / s; qa[0] = q[0] * k; qa[1] = q[1] * k; qa[2] = q[2] * k; qa[3] = cos(half); }
      else { qa[0] = q[0] * alpha; qa[1] = q[1] * alpha; qa[2] = q[2] * alpha; qa[3] = 1.0; }
      double n = sqrt(qa[0] * qa[0] + qa[1] * qa[1] + qa[2] * qa[2] + qa[3] * qa[3]); for (int i = 0; i < 4; i++) qa[i] /= n; }
    double Ra[9]; wq_to_m(qa, Ra);
    double Nd[9];                                                    /* Nd = Ra.Nr */
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            Nd[r * 3 + c] = Ra[r * 3] * Nr[c] + Ra[r * 3 + 1] * Nr[3 + c] + Ra[r * 3 + 2] * Nr[6 + c];
    double deye[3] = { alpha * (en3[0] - er[0]), alpha * (en3[1] - er[1]), alpha * (en3[2] - er[2]) };
    double dtgt[3] = { alpha * (tn[0] - tr[0]),  alpha * (tn[1] - tr[1]),  alpha * (tn[2] - tr[2]) };
    wpc[0] = (float)qa[0]; wpc[1] = (float)qa[1]; wpc[2] = (float)qa[2]; wpc[3] = (float)qa[3];
    for (int r = 0; r < 3; r++)                                     /* t = -Nd.(eye_d - eye_r) */
        wpc[4 + r] = (float)-(Nd[r * 3] * deye[0] + Nd[r * 3 + 1] * deye[1] + Nd[r * 3 + 2] * deye[2]);
    wpc[7] = 1.f;
    for (int r = 0; r < 3; r++)                                     /* d = Nr.(tgt_d - tgt_r) */
        wpc[8 + r] = (float)(Nr[r * 3] * dtgt[0] + Nr[r * 3 + 1] * dtgt[1] + Nr[r * 3 + 2] * dtgt[2]);
    /* 20.10.2026 замер (слот 49, ход в прицеле): её вершины едут с пивотом - он ближе всех к сдвигу нарисованной модели
     * (центр её вершин в мире от такта к такту: пивот 4.8 ед., живые ячейки места 5.1, тело столкновений 4.9); остаток - качание
     * самой походки (вверх-вниз до 5 ед. за такт), его по такту не предсказать. */
    /* 20.09.2026 (игрок: «когда целишься, оружие и рука дрожат — раньше были плавными, когда крутишь камерой в
     * прицеливании»). В прицеливании Регина поворачивается ВМЕСТЕ с камерой (курс привязан к ней, 30 Гц игры), а кадр B
     * поворачивал её вместе с миром на полшага камеры — следующий кадр A возвращал обратно: рука и оружие, далёкие от
     * пивота, ходили зигзагом через кадр (замер: центр кожи −1.2…−2.5 px на каждом B и обратно на каждом A). Теперь в
     * прицеливании её вершины кадр B не трогает вовсе (wd.w = 1). DC_WARP_AIM_RIGID=0 — как было. */
    /* 15.10.2026: и пока камера Tank / RE4 идёт за её поворотом (memory.c dc_tank_warp_rigid) - тот же зигзаг всей модели */
    { static int rigid = -1; if (rigid < 0) { const char* e = getenv("DC_WARP_AIM_RIGID"); rigid = (e && *e == '0') ? 0 : 1; }
      wpc[11] = ((rigid && in.aim) || in.tank) ? 1.f : 0.f; }
    /* ⭐ 20.10.2026 (игрок, слот 49: «в прицеле вожу камерой - она дрожит вся: тело, рука, всё смещается неравномерно... если
     * это просто кадры, стоит их интерполировать»). С 20.09 в прицеле её вершины на кадрах между тактами держались за камерой
     * (wd.w = 1) - верно, только если на каждом такте её курс ровно курс камеры. Но корпус догоняет камеру рулёжкой (доля 0.5
     * за кадр, main.cpp ots_slew_yaw2), отставание меняется с каждым движением мыши: на такте она рывком вставала в новое
     * положение относительно камеры, между тактами стояла; наклон камеры она тоже «везла» с собой и на такте прыгала на место.
     * Теперь в прицеле - как на ходу (19.10, ниже): она стоит в мире, а её курс между тактами - доля пути от курса этого такта к
     * курсу следующего (оба известны: показ отстаёт на такт). Нет пары курсов - как было. Камера Tank / RE4 - как была.
     * На ходу в прицеле (корпус рисует стрейф, pt_anim.c dc_strafe_body_yaw) - как было: замер (слот 49, доля 0.5, DC_HZ=0) -
     * доля поворота и по курсу логики, и по нарисованному корпусу не лучше (неравномерность волос 1.25 против 1.09): дрожь там -
     * качание самой походки на 30 Гц. DC_WARP_AIM_INTERP=0 - как было. */
    { static int ai = -1; if (ai < 0) { const char* e = getenv("DC_WARP_AIM_INTERP"); ai = (e && *e == '0') ? 0 : 1; }
      if (ai && wpc[11] > 0.5f && !in.tank && !in.strafe && in.turn && fabsf(in.dher) < 0.9f) wpc[11] = 0.f; }
    /* ⭐ 19.10.2026 (игрок, слот 49: «иду, держу только вперёд и поворачиваю камеру - она поворачивается за камерой рывками;
     * повернул камеру, потом жму вперёд - доворачивается неплавно; на 60 кадрах тоже; стоя в прицеле - нормально»). Кадры
     * между тактами игры поворачивали её вместе с миром (она стоит в мире), а её собственный поворот приходил только тактом:
     * когда она идёт за камерой, модель на промежуточных кадрах отъезжала назад с миром и рывком догоняла на такте; когда
     * камера стоит, поворот шёл ступенями по 64 кванта (5.6°) на 30 Гц. В прицеле так не было - там её вершины не трогаются
     * (20.09). Теперь её вершины на этих кадрах поворачиваются вокруг её вертикальной оси на долю её поворота от этого такта
     * к следующему (сырой курс на тех же двух камерах, memory.c dc_nots_warp_turn): ячейки UBO 528..531 - кватернион в
     * видовом пространстве кадра A, 532..534 - точка на её оси (пивот камеры), 535 = 1 - есть. Камера стоит, а она
     * поворачивается - кадр тоже новый. Скачок больше 0.9 рад (телепорт, загрузка) - без доворота. Прицел и Tank - как было.
     * DC_WARP_HER_TURN=0 - как было; DC_WARP_DBG=1 - строки warp-her. */
    float hdl_used = 0.f;                                            /* 26.10: её доворот за такт - корпусу (arm_warp_make) */
    { static int hen = -1; if (hen < 0) { const char* e = getenv("DC_WARP_HER_TURN"); hen = (e && *e == '0') ? 0 : 1; }
      double hq[4] = { 0.0, 0.0, 0.0, 1.0 }, hc[3] = { 0.0, 0.0, 0.0 }; int hon = 0;
      const float dcam = in.dcam, dher = in.dher;
      if (hen && wpc[11] < 0.5f && in.turn && fabsf(dher) > 1e-4f && fabsf(dher) < 0.9f) {
          const double dl = (double)alpha * (double)dher;
          const double cs = cos(dl), sn = sin(dl);
          const double Ry[9] = { cs, 0.0, sn,   0.0, 1.0, 0.0,   -sn, 0.0, cs };   /* курс h -> h + dl: (sin h, cos h) по (x, z) */
          double T[9], M[9];                                                       /* M = Nr.Ry.Nr^T - тот же поворот в виде кадра A */
          for (int r = 0; r < 3; r++)
              for (int c = 0; c < 3; c++)
                  T[r * 3 + c] = (double)Nr[r * 3] * Ry[c] + (double)Nr[r * 3 + 1] * Ry[3 + c] + (double)Nr[r * 3 + 2] * Ry[6 + c];
          for (int r = 0; r < 3; r++)
              for (int c = 0; c < 3; c++)
                  M[r * 3 + c] = T[r * 3] * Nr[c * 3] + T[r * 3 + 1] * Nr[c * 3 + 1] + T[r * 3 + 2] * Nr[c * 3 + 2];
          wq_from_m(M, hq);
          const double pv[3] = { (double)tr[0] - er[0], (double)tr[1] - er[1], (double)tr[2] - er[2] };
          for (int r = 0; r < 3; r++) hc[r] = Nr[r * 3] * pv[0] + Nr[r * 3 + 1] * pv[1] + Nr[r * 3 + 2] * pv[2];
          hon = 1; hdl_used = dher;
          *stand = 0;                                                          /* камера стоит, а она поворачивается - кадр новый */
      }
      if (dbg && hen) { static unsigned long long lfn = ~0ull;
          if (lfn != fn) { lfn = fn; std::fprintf(stderr, "warp-her: flips %llu->%llu cam %.4f her %.4f rigid %d -> %s q %.4f,%.4f,%.4f,%.4f c %.0f,%.0f,%.0f%c",
                                                   fr, fn, dcam, dher, (int)(wpc[11] > 0.5f), hon ? "turn" : "-",
                                                   hq[0], hq[1], hq[2], hq[3], hc[0], hc[1], hc[2], 10); } }
      if (s_shUboMap) {
          for (int i = 0; i < 4; i++) s_shUboMap[528 + i] = (float)hq[i];
          for (int i = 0; i < 3; i++) s_shUboMap[532 + i] = (float)hc[i];
          s_shUboMap[535] = hon ? 1.f : 0.f;
      } }
    { static int adbg = -1; if (adbg < 0) { const char* e = getenv("DC_ARMWARP_DBG"); adbg = (e && *e == '1') ? 1 : 0; }   /* 20.10 */
      if (arm_warp_make(alpha, in, wpc[11] > 0.5f, hdl_used, dbg || adbg) > 0) *stand = 0; }   /* 25.10: камера стоит, а кости идут - кадр новый */
    if (o_ang) *o_ang = ang; if (o_de) *o_de = de; if (o_dt) *o_dt = dt; if (o_fr) *o_fr = fr; if (o_fn) *o_fn = fn;
    return 1;
}
static const unsigned int* warp_render_impl(float alpha_in, int extra, const WarpIn* in_pt) {
    if (ptvk_lost()) return nullptr;    /* 18.10: видеокарта потеряна */
    static int en = -1, dbg = -1; static float alpha_env = -1.f;
    if (en < 0) { const char* e = getenv("DC_FRAME_WARP"); en = (e && *e == '0') ? 0 : 1;
                  e = getenv("DC_WARP_DBG"); dbg = (e && *e == '1') ? 1 : 0;
                  e = getenv("DC_FRAME_WARP_ALPHA"); alpha_env = e ? (float)atof(e) : 0.5f;
                  if (alpha_env < 0.f) alpha_env = 0.f; if (alpha_env > 1.f) alpha_env = 1.f;
                  std::fprintf(stderr, "frame warp: %s, alpha %.2f (DC_FRAME_WARP=0 off)%c", en ? "on" : "off", alpha_env, 10); }
    float alpha = alpha_in >= 0.f ? (alpha_in > 1.f ? 1.f : alpha_in) : alpha_env;
    if (extra) {
        if (!en || !s_wsnap.valid || s_wsnap.dropped || !s_wsnap.world) return nullptr;
    } else {
        if (!en || !s_wsnap.valid || s_wsnap.used || !s_wsnap.world) return nullptr;
        s_wsnap.used = 1;                                /* один кадр B на кадр A */
    }
    if (in_pt) {                                                    /* 25.10: поток показа - только из копии вершин кадра */
        if (!s_wsnap.vcopy || s_wsnap.nverts <= 0) return nullptr;
    } else if ((s_nverts != 0 && !s_wsnap.vcopy) || s_wsnap.nverts <= 0 || (!s_vmap && !s_wsnap.vcopy)) return nullptr;   /* поток уже пишет новый кадр; 26.10: с копией вершин - можно (main.cpp s_vb_held) */
    float wpc[16]; int stand = 0; double ang = 0.0, de = 0.0, dt = 0.0; unsigned long long fr = 0, fn = 0;
    const int pairB = s_wsnap.pairs;              /* 26.10: у кадра A пары кусков (двери) - камеры нет, строки пар ниже, под замком */
    if (!pairB && !warp_pc_make(alpha, wpc, &stand, dbg, &ang, &de, &dt, &fr, &fn, in_pt)) return nullptr;
    if (dbg && !in_pt && s_nverts == 0 && s_vmap) { static int nn = 0;   /* 19.10: её вершины в снимке (F_ACTOR, с глубиной) */
        if (nn++ < 12) { int na = 0, naw = 0;
            for (int i = 0; i < s_wsnap.nverts; i++) { const float* v = s_vmap + (size_t)i * VERT_FLOATS;
                if ((int)v[11] & 16) { na++; if (v[12] >= 1.5f) naw++; } }
            std::fprintf(stderr, "warp-her: snapshot verts %d, hers %d, hers with depth %d, ubo hc.w %.0f%c", s_wsnap.nverts, na, naw,
                         s_shUboMap ? s_shUboMap[535] : -1.f, 10); } }
    int scapB = 0;                                                  /* 01.10 вечер: ячейки «третьей энергии» идут на 60 Гц */
    for (size_t i = 0; i < s_wsnap.scaps.size() && !scapB; i++) if (s_wsnap.scaps[i].ab == 2) scapB = 1;
    if (!pairB && stand && !scapB) return nullptr;                  /* камера стоит — кадр тот же */
    wpc[12] = s_wsnap.ox; wpc[13] = s_wsnap.oy; wpc[14] = s_wsnap.H; wpc[15] = s_wsnap.Hs;

    struct GpuGuard { GpuGuard() { ptvk_gpu_lock(); } ~GpuGuard() { ptvk_gpu_unlock(); } } gpu_guard;
    if (!raster_init_impl()) return nullptr;
    if (s_shUboMap) s_shUboMap[664] = 0.f;                          /* 26.10.2026: пары кусков - только если взяты ниже */
    if (pairB) {                                                    /* строки пар на эту долю пути: UBO общий с кадром A - под замком */
        int standP = 0;
        if (!s_shUboMap || s_pairSet.frame != s_wsnap.frame || !pair_pc_make(alpha, wpc, s_shUboMap + 668, &standP)) return nullptr;
        if (standP && !scapB) return nullptr;                       /* ни один кусок не движется - кадр тот же */
        { static int pdb = -1; if (pdb < 0) { const char* e = getenv("DC_PAIR_DBG"); pdb = e ? atoi(e) : 0; }
          if (pdb >= 3) {                                           /* 3 - строка на каждый кадр между тактами: доля пути, сдвиг, метки */
              float mo = 0.f; const float* pr = s_shUboMap + 668;
              for (int i = 0; i < s_pairSet.count; i++) for (int r = 0; r < 3; r++) { const float c = fabsf(pr[i * 12 + r * 4 + 3]); if (c > mo) mo = c; }
              int nt = 0, nm = 0;
              if (!in_pt && s_nverts == 0 && s_pvMap) for (int i = 0; i < s_wsnap.nverts; i++) { const uint32_t t = s_pvMap[i];
                  if (t) { nt++; const int ix = (int)(t & 0x1FFFu) - 1;
                           if ((int)(t >> 13) == (s_pairSet.passA & 3) && ix >= 0 && ix < s_pairSet.count && s_pairSet.p[ix].on) nm++; } }
              std::fprintf(stderr, "pairwarp-b: alpha %.3f extra %d thread %d frame %llu pass %d moving %d max offset %.2f, tags %d of %d verts, own pair %d%c",
                           alpha, extra, in_pt ? 1 : 0, s_wsnap.frame, s_pairSet.passA, s_pairSet.moving, mo, nt, s_wsnap.nverts, nm, 10); } }
        wpc[12] = s_wsnap.ox; wpc[13] = s_wsnap.oy; wpc[14] = s_wsnap.H; wpc[15] = s_wsnap.Hs;
        s_shUboMap[665] = (float)(s_pairSet.passA & 3); s_shUboMap[666] = (float)s_pairSet.count; s_shUboMap[667] = 0.f;
        s_shUboMap[664] = 1.f;
    }
    const int w = s_wsnap.w, h = s_wsnap.h;
    if (w <= 0 || h <= 0 || !ensureTargets(w, h)) return nullptr;
    LARGE_INTEGER t0, t1; static LARGE_INTEGER qpf = { 0 };
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    /* 25.09: MSAA и SSAO кадра B — как у кадра A (снимок — всегда кадр мира) */
    raster_feat_sync();                              /* 25.10.2026: конвейеры - под опции меню */
    post_fit_note(w, h, 1);
    const int msN = msaaEnsure(w, h, 1);
    const int ppWorldB = (msN > 1 || s_rpFirst != VK_NULL_HANDLE);
    const int aoFrameB = s_ppSsao && ppWorldB && postEnsure();
    s_ppAoFrame = aoFrameB; s_ppMsFrame = msN;
    /* 25.09 вечер: свечение и отражение кадра B — своей проекцией (снимок — всегда кадр мира) */
    const int fxDbgB = (int)(s_pp[PP_FX_DBG] + 0.5f);
    const int glowB = !dc_orig_nogfx() && (s_pp[PP_GL_ON] > 0.5f || fxDbgB == 1) && fxEnsure();
    const int reflB = !dc_orig_nogfx() && g_dc_gfx_materials && s_pp[PP_RF_ON] > 0.5f && s_rfPlaneOk && fxEnsure();
    const int volB = ppWorldB && !dc_orig_nogfx() && s_pp[PP_VL_ON] > 0.5f && postEnsure() && volEnsure();   /* 27.09: объёмный свет */
    const int aaModeB = aa_frame(msN, ppWorldB, w, h);   /* 25.10.2026: FXAA / SMAA / CMAA2 - как в кадре A */
    s_ppAAFrame = aaModeB != 0;
    const int maskB = aoFrameB || volB || aaModeB;
    if (s_shUboMap) s_shUboMap[127] = reflB ? (s_pp[PP_RF_ALL] > 0.5f ? 2.f : 1.f) : 0.f;
    glint_ubo_update();                              /* 12.10: блеск предметов - и в кадре B */
    VkPipeline* const P  = (msN > 1) ? s_pipesMS : s_pipes;
    const VkPipeline  BG = (msN > 1) ? s_bgPipeMS : s_bgPipe;
    const VkBuffer wvb = s_wsnap.vcopy ? s_wsVb : s_vbuf, wrv = s_wsnap.vcopy ? s_wsRv : s_rvBuf;   /* 25.10: копия вершин кадра A */
    const VkBuffer wpv = s_wsnap.vcopy ? s_wsPv : s_pvBuf;                                         /* 26.10: и его меток кусков */
    cmdBegin();
    pp_ts_reset();
    VkClearValue clears[2]{};
    clears[0].color = { { 0.f, 0.f, 0.f, 1.f } };
    clears[1].depthStencil = { 0.f, 0 };
    VkRenderPassBeginInfo rbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = (msN > 1) ? s_rpMS : s_rp; rbi.framebuffer = (msN > 1) ? s_fbMS : s_fb;   /* 25.09: MSAA */
    rbi.renderArea = { { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    /* 21.09 вечер: ячейки захвата сцены — как в кадре A */
    int scapFrameB = 0;
    if (!s_wsnap.scaps.empty() && dc_scap_on() && s_prevImg && s_prevOnGpu && s_rpFirst && s_rpResume)
        for (size_t qi = 0; qi < s_wsnap.draws.size() && !scapFrameB; qi++) if (s_wsnap.draws[qi].scap > 0) scapFrameB = 1;
    if (scapFrameB || aoFrameB || volB) rbi.renderPass = (msN > 1) ? s_rpMSFirst : s_rpFirst;   /* 25.09: SSAO (27.09: и дымка) — глубина сохраняется */
    s_rpResumeCur = (msN > 1) ? s_rpMSResume : s_rpResume;
    if (glowB || reflB) {
        fx_init_layouts();
        if (reflB) fx_geom_pass(2, s_wsnap.draws, &s_wsnap.order, 0, w, h, s_wsnap.dispX, s_wsnap.dispY, s_wsnap.dispW, s_wsnap.dispH, wpc,
                                wvb, &s_wsnap.mrefs, wpv);
        if (glowB) fx_geom_pass(1, s_wsnap.draws, &s_wsnap.order, 0, w, h, s_wsnap.dispX, s_wsnap.dispY, s_wsnap.dispW, s_wsnap.dispH, wpc,
                                wvb, &s_wsnap.mrefs, wpv);
    }
    pp_ts(0);
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt{ 0.f, 0.f, (float)w, (float)h, 0.f, 1.f };
    VkRect2D sc{ { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(s_cmd, 0, 1, &vpt);
    vkCmdSetScissor(s_cmd, 0, 1, &sc);
    {   const VkBuffer vbs[3] = { wvb, wrv, wpv }; const VkDeviceSize vofs[3] = { 0, 0, 0 };   /* 05.10: + текстуры PNG из mods; 26.10: + метки */
        vkCmdBindVertexBuffers(s_cmd, 0, 3, vbs, vofs); }
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_plLay, 0, 1, &s_dset, 0, nullptr);
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, BG);
    vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, s_wsnap.bgpc);
    vkCmdDraw(s_cmd, 3, 1, 0, 0);
    vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, s_wsnap.pcp);
    vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 64, 64, wpc);
    float pc[8]; memcpy(pc, s_wsnap.pc, sizeof pc);
    const float pc7_base = pc[7];
    const VkRect2D scFull = sc; VkRect2D scCur = sc;
    const int clipEn = dc_drawarea_clip_on();
    const int dispX = s_wsnap.dispX, dispY = s_wsnap.dispY, dispW = s_wsnap.dispW, dispH = s_wsnap.dispH;
    int bound = -1;
    int scapCurB = 0, scapPcpB = 0;
    s_scapLogN++;
    for (size_t oi = 0; oi < s_wsnap.order.size(); oi++) {
        size_t di = s_wsnap.order[oi];
        if (di >= s_wsnap.draws.size()) continue;
        const Draw& d = s_wsnap.draws[di];
        if (d.scap > 0 && (size_t)d.scap <= s_wsnap.scaps.size() && s_wsnap.scaps[(size_t)d.scap - 1].ab == 1) continue;   /* 01.10 вечер: квад кадра A */
        if (scapFrameB && d.scap > 0 && (size_t)d.scap <= s_wsnap.scaps.size()) {
            if (dc_scap_dbg()) continue;                     /* 01.10 вечер: и в кадре B */
            const Scap& sc_ = s_wsnap.scaps[(size_t)d.scap - 1];
            if (d.scap != scapCurB) { scap_switch(sc_, rbi, w, h, dispX, dispY, dispW, dispH); scapCurB = d.scap; scap_log('B', sc_); }
            float cp[8]; scap_pcp(sc_, w, h, dispX, dispY, dispW, dispH, cp);
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, cp);
            scapPcpB = 1;
        } else if (scapPcpB) {
            vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 32, 32, s_wsnap.pcp);
            scapPcpB = 0;
        }
        VkRect2D scWant = scFull;
        if (d.clip && clipEn && !d.wire) {
            const float kx = (float)w / (float)dispW, ky = (float)h / (float)dispH;
            int x0 = (int)floorf((d.cr[0] - (float)dispX) * kx + 0.5f), x1 = (int)floorf((d.cr[2] - (float)dispX) * kx + 0.5f);
            int y0 = (int)floorf((d.cr[1] - (float)dispY) * ky + 0.5f), y1 = (int)floorf((d.cr[3] - (float)dispY) * ky + 0.5f);
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w) x1 = w; if (y1 > h) y1 = h;
            if (x1 <= x0 || y1 <= y0) continue;
            scWant.offset = { x0, y0 }; scWant.extent = { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) };
        }
        if (scWant.offset.x != scCur.offset.x || scWant.offset.y != scCur.offset.y ||
            scWant.extent.width != scCur.extent.width || scWant.extent.height != scCur.extent.height) {
            vkCmdSetScissor(s_cmd, 0, 1, &scWant); scCur = scWant;
        }
        if (d.mesh >= 0) {                                   /* 06.10.2026: меш из mods (тот же экземпляр, перепроецирование — в шейдере) */
            if (P[12] != VK_NULL_HANDLE) {
                pc[7] = pc7_base + (maskB ? 384.0f : 0.0f);
                pc[4] = 0.f;
                vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
                medx_record(s_cmd, P[12], d, mesh_ref_of(d, s_wsnap.mrefs));
                bound = -1;
                const VkBuffer vbsM[3] = { wvb, wrv, wpv }; const VkDeviceSize vofsM[3] = { 0, 0, 0 };
                vkCmdBindVertexBuffers(s_cmd, 0, 3, vbsM, vofsM);
            }
            continue;
        }
        int want = (d.wire && P[10] != VK_NULL_HANDLE) ? 10 : d.kind;
        if (want != bound) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P[want]); bound = want; }
        pc[7] = pc7_base + (d.wire ? 32.0f : 0.0f) +
                (maskB ? (128.0f + ((want == 0 || (d.scap > 0 && dc_scap_ao())) ? 256.0f : 0.0f)) : 0.0f);   /* 01.10: ячейка — как мир */
        pc[4] = (float)d.pass;
        vkCmdPushConstants(s_cmd, s_plLay, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
        vkCmdDraw(s_cmd, (uint32_t)d.count, 1, (uint32_t)d.first, 0);
    }
    vkCmdEndRenderPass(s_cmd);
    pp_ts(1);
    if (aaModeB) aa_run(aaModeB, w, h, aoFrameB || volB);   /* 25.10.2026: сглаживание - до SSAO, свечения и дымки */
    pp_ts(13);
    if (glowB || volB) veil_ubo();                   /* 28.09 (6): пелена игры — та же, что в кадре A (снимок его draw) */
    if (aoFrameB) {                                  /* 25.09: затенение углов кадра B — проекция снимка */
        const float viewB[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpB[4] = { s_wsnap.ox, s_wsnap.oy, s_wsnap.H, s_wsnap.Hs };
        pp_run(w, h, msN, viewB, wpB);
    } else if (volB) {                               /* 27.09: SSAO выкл — для дымки только его проход глубины */
        const float viewB[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpB[4] = { s_wsnap.ox, s_wsnap.oy, s_wsnap.H, s_wsnap.Hs };
        pp_zonly(w, h, msN, viewB, wpB);
    }
    if (glowB || (reflB && fxDbgB == 2)) fx_glow_post(w, h, glowB, fxDbgB);
    if (volB) {                                      /* 27.09: объёмный свет кадра B — проекция снимка, точки — в кадр A */
        const float viewV[4] = { (float)dispX, (float)dispY, (float)dispW, (float)dispH };
        const float wpV[4] = { s_wsnap.ox, s_wsnap.oy, s_wsnap.H, s_wsnap.Hs };
        vol_run(w, h, viewV, wpV, wpc);
    }
    pp_ts(2);
    const bool shFrameB = dx_record(w, h);                  /* 09.10: кадр B — тоже в общую с D3D11 текстуру */
    if (!shFrameB) {
        VkBufferImageCopy out{};
        out.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        out.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyImageToBuffer(s_cmd, s_colImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rbBuf, 1, &out);
    }
    if (!cmdSubmitWait()) { rlog("[VKR] warp submit/hang\n"); return nullptr; }
    test_gpu_slow(extra);
    s_lastShared = shFrameB ? 1 : 0;
    pp_ts_read();
    QueryPerformanceCounter(&t1);
    g_vk_last_rec_ms = (double)(s_vk_last_tq0.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    { static long n = 0; static double acc = 0;
      acc += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart;
      if (dbg) std::fprintf(stderr, "warp: %s %llu flips %llu->%llu alpha %.2f ang %.4f eye %.1f pivot %.1f t %.1f,%.1f,%.1f d %.1f,%.1f,%.1f rigid %.0f %.2f ms at %.2f%c",
                            extra ? "extra" : "frame",
                            s_wsnap.frame, fr, fn, alpha, ang, de, dt, wpc[4], wpc[5], wpc[6], wpc[8], wpc[9], wpc[10], wpc[11],
                            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpf.QuadPart,
                            (double)t0.QuadPart * 1000.0 / (double)qpf.QuadPart, 10);
      if (++n % 600 == 0) { rlog("[VKR] warp render avg %.2f ms over 600\n", acc / 600.0); acc = 0; } }
    s_outW = w; s_outH = h;
    return (const unsigned int*)s_rbMap;
}
/* ⭐ 25.10.2026 - КАДРЫ МЕЖДУ ТАКТАМИ В ПОТОКЕ ПОКАЗА (main.cpp DC_PRESENT_THREAD; игрок: «процессор и видеокарта
 * одновременно», «если цель 300 - должно быть 300»). Главный поток после показа кадра такта снимает входы перепроецирования
 * (dcvk_warp_pub, под замком вывода) - дальше поток показа рисует кадры из снимка кадра A и копии его вершин, пока эмуляция
 * пишет следующий кадр. dcvk_warp_pt_ok - снимок всё тот же: не заменён новым кадром A, не удержан (dcvk_warp_drop). */
static WarpIn s_wiPub;
static unsigned long long s_wiPubFrame = ~0ull;
extern "C" int dcvk_warp_pub(unsigned long long* frame) {
    if (s_warp_en < 0) { const char* e = getenv("DC_FRAME_WARP"); s_warp_en = (e && *e == '0') ? 0 : 1; }
    s_wiPubFrame = ~0ull;
    if (!s_warp_en || !s_wsnap.valid || s_wsnap.dropped || !s_wsnap.world || !s_wsnap.vcopy || s_wsnap.nverts <= 0) return 0;
    warp_in_take(&s_wiPub);
    s_wiPubFrame = s_wsnap.frame;
    if (frame) *frame = s_wsnap.frame;
    return 1;
}
extern "C" int dcvk_warp_pt_ok(unsigned long long frame) {
    return s_wiPubFrame == frame && s_wsnap.frame == frame && s_wsnap.valid && !s_wsnap.dropped && s_wsnap.world && s_wsnap.vcopy;
}
extern "C" const unsigned int* dcvk_render_warp_pt(float alpha) {
    if (s_wiPubFrame != s_wsnap.frame) return nullptr;
    return warp_render_impl(alpha, 1, &s_wiPub);
}

#ifndef PSX_NO_DEBUG_TOOLS
/* 25.10.2026 (dev only): DC_BENCH_WARP=<n> - ЗАМЕР ВИДЕОКАРТЫ БЕЗ СБРОСА ЧАСТОТЫ. В игре видеокарта ждёт процессор между
 * кадрами и сбрасывает частоту - время прохода по меткам скачет вдвое от прогона к прогону, сравнить варианты шейдера
 * нельзя. Здесь кадр между тактами рисуется n раз подряд (видеокарта занята сплошь), по второй половине - медиана и 10-й
 * процентиль: основной проход, после него (SSAO, свечение, дымка), весь кадр по часам. Три замера: через DC_BENCH_WARP_AT
 * (умолч. 6) секунд после старта и ещё через 4 и 8, в месте дополнительных кадров (main.cpp, поток кадра пуст, камера идёт). */
extern "C" void dcvk_warp_bench_tick(int ok) {
    static int n = -1, done = 0; static double at = 6.0; static LARGE_INTEGER t0, f;
    if (n < 0) { const char* e = getenv("DC_BENCH_WARP"); n = e ? atoi(e) : 0; e = getenv("DC_BENCH_WARP_AT"); if (e) at = atof(e);
                 QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    if (n < 8 || done >= 3 || !ok) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart < at + 4.0 * done) return;
    done++;
    std::vector<double> gm, gp, gw;
    for (int i = 0; i < n; i++) {
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        if (!warp_render_impl(0.5f, 1, nullptr)) break;
        QueryPerformanceCounter(&b);
        uint64_t q[3] = { 0, 0, 0 };
        if (s_tqOk && vkGetQueryPoolResults(C->device, s_tq, 0, 3, sizeof q, q, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t m = (s_tsBits >= 64) ? ~0ull : ((1ull << s_tsBits) - 1ull);
            const double k = (double)s_tsPeriod * 1e-6;
            gm.push_back((double)((q[1] - q[0]) & m) * k); gp.push_back((double)((q[2] - q[1]) & m) * k);
        }
        gw.push_back((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    auto st = [](std::vector<double> v, double& med, double& p10) {
        med = p10 = 0.0;
        if (v.size() < 4) return;
        v.erase(v.begin(), v.begin() + (long)(v.size() / 2));
        std::sort(v.begin(), v.end());
        med = v[v.size() / 2]; p10 = v[v.size() / 10];
    };
    double m1, m2, p1, p2, w1, w2;
    st(gm, m1, m2); st(gp, p1, p2); st(gw, w1, w2);
    std::fprintf(stderr, "bench: warp x%d (%dx%d, MSAA %dx) - GPU main med %.3f p10 %.3f, after main med %.3f p10 %.3f, "
                 "frame wall med %.3f p10 %.3f ms%c", (int)gw.size(), s_wsnap.w, s_wsnap.h, s_ppMsFrame, m1, m2, p1, p2, w1, w2, 10);
}
#endif

/* Dump the last rendered GPU frame (readback buffer) for verification. */
static void writeBMP(const char* path, const unsigned int* rgba, int w, int h);
extern "C" float dcvk_body_fade_level(void) { return s_fade_level_pub; }

/* 14.09 вечер (игрок: «при отталкивании от решётки чёрный экран»): первый кадр режима 5
 * (захват/отбрасывание) приходит БЕЗ единого полигона с глубиной — ни мира, ни Регины,
 * только заливки игры, и «clears stay in front» честно красит окно в чёрный. Признак для
 * main.cpp: в накопленном потоке нет мира, а в прошлом показанном кадре был, и это не
 * копия кадра (пауза/меню). Решение «удержать прошлый кадр» — в main.cpp. */
extern "C" int dcvk_no_world_now(void) { return (!s_worldDrawn && !s_prev_no3d_pub && !s_frameCopy) ? 1 : 0; }
/* 21.09: в накопленном потоке уже есть геометрия с глубиной — ждёт показа кадр МИРА (main.cpp: DC_MENU_EXIT_HOLD) */
extern "C" int dcvk_world_pending(void) { return s_worldDrawn ? 1 : 0; }
/* ...и последний ОТРИСОВАННЫЙ кадр был без единого примитива с глубиной (плоский экран, чёрный кадр) — в любом режиме соотношения */
extern "C" int dcvk_last_no3d(void) { return s_prev_no3d_pub ? 1 : 0; }
/* 27.09 вечер: примитивов интерфейса игры (Draw.ui) в последнем dcvk_render — main.cpp держит прежнюю картинку экрана SAVE DATA,
 * если игра, пока ждёт карту, прислала кадр без меню */
extern "C" int dcvk_last_ui_draws(void) { return s_lastUiDraws; }
/* ===== FLICKER FUSER (2026-08-23 night) ====================================
 * PS1 games draw a "flashing light" by submitting its translucent quad only
 * every OTHER frame - on a CRT the alternation fused into a steady half-bright
 * glow. On a 60 Hz LCD present it strobes, and under the re-lit renderer the
 * alarm strip in the laser corridor strobed the whole scene's shading
 * (measured: 120k/110k visible pixels alternating per present; our own room
 * emit was steady at 13 pieces - the blinking came from the game's stream).
 * The fuser keys every translucent triangle by its texture identity (page,
 * clut, uvs, colours - NOT position, which moves with the camera) and, when a
 * triangle was in the stream last present but not this one, replays the stored
 * copy once. A quad the game flashes 1-on/1-off becomes continuous; anything
 * off for two or more presents disappears normally, so slow deliberate blinks
 * survive. DC_FLICKER_FUSE=0 disables. */
#define FUSE_SLOTS 512
extern "C" { int g_fuse_replayed_now = 0; }   /* 23.09: ДИАГНОСТИКА — треугольников повтора в этом кадре (DC_ORDER_DBG) */
typedef struct { uint64_t key; float v[3 * VERT_FLOATS]; int semi, ztest;
                 int room; int dox, doy; uint64_t seen; uint64_t replayed;
                 int led, wok, lent; float wv[9], lw0; } FuseEnt;   /* 28.09 (5): огонёк игры — вершины относительно его места */
static FuseEnt s_fuse[FUSE_SLOTS];
/* ⭐ 28.09 (5) — ПОВТОР СЛОЯ ОГОНЬКА ПО МИРУ (игрок, слот 15, терминал комнаты 48: «огонёк скачет — не можешь привязать к точке,
 * координатам, вертексу, полигону?»). Огоньки пультов игра рисует слоями через кадр (одна чётность — окна 208,160 + 224,168,
 * другая — 232,160 + 232,144 + 224,160); сглаживатель ниже дорисовывает пропущенный слой копией ПРОШЛОГО показа, то есть
 * старыми экранными координатами. Его сторож движения смотрит на матрицу камеры игры 0xBF9F4, а в комнатах 3-4 этапа там
 * нули (замер scene_probe: R0 = 0,0,0 во всех состояниях), и шаг камеры игры за кадр (до 24 ед.) ниже порога: на ходу
 * копия слоя вставала в 2–4 точках растра от огонька (dump_prims: draw 4–15 — огонёк, draw 216+ — копия рядом) — огонёк
 * «двоился и скакал». Теперь треугольник огонька (страница эффектов 896,256, палитра 800,503, окна u 208..247 v 144..175)
 * запоминается ОТНОСИТЕЛЬНО места своего огонька в том показе (gpu.c dc_led_now_*: где посаженный огонёк стоит на экране), а
 * при повторе ставится относительно места ТОГО ЖЕ огонька в текущем показе (размер — обратно глубине): слои концентричны и на
 * ходу, и на повороте. (Первый вариант — перевод через мир нашей камерой — оставлял 2 точки расхождения: камера, которой огонёк
 * сажают посреди кадра, и камера на показе — не одна и та же.) Огонька в показе нет — слой не повторяется. Прочие эффекты —
 * как было. DC_FUSE_LED_WORLD=0 — как было. */
extern "C" int dc_led_now_find(unsigned long long flip, float sx, float sy, float maxd, float *osx, float *osy, float *ow);   /* gpu.c */
extern "C" int dc_led_now_get(int ent, unsigned long long flip, float *osx, float *osy, float *ow);
static int fuse_led_world_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("DC_FUSE_LED_WORLD"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int fuse_is_led(const float* v) {
    for (int k = 0; k < 3; k++) {
        const float* o = v + k * VERT_FLOATS;
        const int tp = (int)(o[8] + 0.5f) & 0x1FF, cx = (int)(o[9] + 0.5f), cy = (int)(o[10] + 0.5f);
        if ((tp & 0x19F) != 0x1E || cx != 800 || cy != 503) return 0;       /* 4 бита, страница 896,256; палитра 800,503 */
        if (o[2] < 207.5f || o[2] > 247.5f || o[3] < 143.5f || o[3] > 175.5f) return 0;
    }
    return 1;
}
/* s_fuse_replaying is defined near the top (dcvk_push_tri_raw logs it) */
extern "C" uint64_t gpu_flip_seq(void);
static void fuse_note2(const float* v, int semi, int ztest) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("DC_FLICKER_FUSE"); en = (e && *e == '0') ? 0 : 1; }
    if (!en || s_fuse_replaying || semi < 0) return;
    uint64_t key = 1469598103934665603ull;
    for (int k = 0; k < 3; k++) {
        const float* o = v + k * VERT_FLOATS;
        uint32_t bits = (uint32_t)o[2] | ((uint32_t)o[3] << 8) |
                        ((uint32_t)o[8] << 16);                       /* u, v, texpage */
        key ^= bits; key *= 1099511628211ull;
        bits = (uint32_t)o[4] | ((uint32_t)o[5] << 8) | ((uint32_t)o[6] << 16) |
               ((uint32_t)o[9] << 24);                                /* r,g,b, clut */
        key ^= bits; key *= 1099511628211ull;
    }
    if (!key) key = 1;
    FuseEnt* e2 = &s_fuse[(uint32_t)((key ^ (key >> 23)) & (FUSE_SLOTS - 1u))];
    e2->key = key; e2->semi = semi; e2->ztest = ztest;
    e2->room = g_dcvk_room_pass;          /* who submitted: room walk or the game */
    /* AND WHICH VRAM HALF IT WAS DRAWN INTO (2026-08-24). The positions in v[]
     * already carry the GPU draw offset (see dcvk_push_tri: it un-applies
     * g_pt_proj_of* + draw offset to get view space). DC double-buffers
     * VERTICALLY - the offset alternates 0 / 256 every flip - so a copy
     * replayed one flip later without correction lands 256 px away from where
     * it belongs: the ghost slabs the player sees. Remember the offset so the
     * replay can rebase it. */
    { extern void gpu_get_draw_offset(int *ox, int *oy);
      gpu_get_draw_offset(&e2->dox, &e2->doy); }
    memcpy(e2->v, v, sizeof(e2->v));
    e2->seen = gpu_flip_seq();
    e2->led = 0; e2->wok = 0; e2->lent = -1;                     /* 28.09 (5): огонёк — вершины относительно его места */
    if (fuse_led_world_on() && fuse_is_led(v)) {
        e2->led = 1;
        float cx = 0.f, cy = 0.f;
        for (int k = 0; k < 3; k++) { cx += v[k * VERT_FLOATS] / 3.f; cy += v[k * VERT_FLOATS + 1] / 3.f; }
        float sx, sy, sw;
        const int ent = dc_led_now_find(gpu_flip_seq(), cx, cy, 24.f, &sx, &sy, &sw);   /* огонёк этого показа, которому он слой */
        if (ent >= 0 && sw > 1.f) {
            for (int k = 0; k < 3; k++) {
                const float* o = v + k * VERT_FLOATS;
                e2->wv[k * 3] = o[0] - sx; e2->wv[k * 3 + 1] = o[1] - sy; e2->wv[k * 3 + 2] = o[12];
            }
            e2->lent = ent; e2->lw0 = sw; e2->wok = 1;
        }
    }
}
extern "C" void dcvk_flicker_replay(void) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("DC_FLICKER_FUSE"); en = (e && *e == '0') ? 0 : 1; }
    if (!en) return;
    uint64_t now = gpu_flip_seq();
    int fuse_moved = 0;                      /* 28.09 (5): вид сдвинулся — повторяем только огоньки, по миру (см. fuse_is_led) */
    /* MOTION GUARD (2026-08-24, player: "мигающие плашки лезут мусором в
     * экран" - the streaks in the rest room). A replayed triangle is a copy
     * of the PREVIOUS present's screen-space coordinates. While the view is
     * changing - the engine cuts or pans its camera, the nOTS delta turns,
     * Regina herself moves - those stale coordinates land in the wrong place
     * and a blinking additive plate becomes a slab smeared across the frame
     * (the original 2026-08-23 night build had exactly this guard's ancestor;
     * it was lost in the render rollback). Replay only when the whole view
     * held still since the previous flip: engine camera matrix + translation,
     * the nOTS delta, and the player position, byte-compared. A blinking
     * plate simply strobes honestly for the frames the view moves.
     * DC_FUSE_MOTION=0 disables the guard. */
    { static int mg = -1;
      if (mg < 0) { const char* e = getenv("DC_FUSE_MOTION"); mg = (e && *e == '0') ? 0 : 1; }
      if (mg) {
          /* block-scope externs: C linkage inherited from the extern "C"
           * enclosing function (a literal `extern "C"` here is a parse error) */
          extern uint8_t* memory_get_ram_ptr(void);
          extern int dc_nots_get_delta(float D[9], float T[3]);
          static uint8_t prev_cam[18 + 12 + 6]; static float prevD[12];
          static uint64_t prev_flip = ~0ull; static int prev_have = 0;
          uint8_t cur[18 + 12 + 6]; float D[12] = {0};
          uint8_t* ram = memory_get_ram_ptr();
          int moved = 0;
          if (ram) {
              memcpy(cur, ram + 0xBF9F4, 18);        /* engine R_cam */
              memcpy(cur + 18, ram + 0xBFA08, 12);   /* engine T_cam */
              memcpy(cur + 30, ram + 0xB92AC, 6);    /* player X,Y,Z */
          } else memset(cur, 0, sizeof cur);
          { float Dr[9], Dt[3];
            if (dc_nots_get_delta(Dr, Dt)) { memcpy(D, Dr, 36); memcpy(D + 9, Dt, 12); } }
          /* TOLERANCE, NOT EQUALITY. A byte-exact comparison never matches -
           * the camera filters and the dolly move the delta a hair every
           * flip - so it silenced the fuser completely and the strobing
           * light plate (the 2026-08-23 fix the player confirmed) came back.
           * What matters is whether a one-flip-old screen position is still
           * close enough to be invisible: allow a small view change, refuse a
           * real pan or step. Thresholds from DC_FUSE_MOTION_R / _T / _P. */
          static float thR = -1.f, thT = 24.f, thP = 24.f;
          if (thR < 0.f) { const char* e = getenv("DC_FUSE_MOTION_R"); thR = e ? (float)atof(e) : 0.004f;
                           e = getenv("DC_FUSE_MOTION_T"); if (e) thT = (float)atof(e);
                           e = getenv("DC_FUSE_MOTION_P"); if (e) thP = (float)atof(e); }
          float mR = 0.f, mT = 0.f, mP = 0.f;
          if (!prev_have) moved = 1;
          else {
              const int16_t* cR = (const int16_t*)cur;      const int16_t* pR = (const int16_t*)prev_cam;
              for (int k = 0; k < 9; k++) { float d = (float)(cR[k] - pR[k]) * (1.f/4096.f);
                                            if (d < 0) d = -d; if (d > mR) mR = d; }
              const int32_t* cT = (const int32_t*)(cur + 18); const int32_t* pT = (const int32_t*)(prev_cam + 18);
              for (int k = 0; k < 3; k++) { float d = (float)(cT[k] - pT[k]); if (d < 0) d = -d; if (d > mT) mT = d; }
              const int16_t* cP = (const int16_t*)(cur + 30);  const int16_t* pP = (const int16_t*)(prev_cam + 30);
              for (int k = 0; k < 3; k++) { float d = (float)(cP[k] - pP[k]); if (d < 0) d = -d; if (d > mP) mP = d; }
              for (int k = 0; k < 9; k++) { float d = D[k] - prevD[k]; if (d < 0) d = -d; if (d > mR) mR = d; }
              for (int k = 9; k < 12; k++) { float d = D[k] - prevD[k]; if (d < 0) d = -d; if (d > mT) mT = d; }
              if (mR > thR || mT > thT || mP > thP) moved = 1;
          }
          { static int ml = -1; static long n = 0;
            if (ml < 0) { const char* e = getenv("DC_FUSE_MOTION_LOG"); ml = (e && *e == '1') ? 1 : 0; }
            if (ml && (++n % 30) == 1)
                rlog("[FUSEMOT] dR=%.5f dT=%.1f dP=%.1f moved=%d (th %.5f/%.1f/%.1f)\n",
                     mR, mT, mP, moved, thR, thT, thP); }
          memcpy(prev_cam, cur, sizeof cur); memcpy(prevD, D, sizeof prevD);
          prev_have = 1; prev_flip = now; (void)prev_flip;
          if (moved) { if (!fuse_led_world_on()) return; fuse_moved = 1; }
      } }
    s_fuse_replaying = 1;
    g_fuse_replayed_now = 0;
    for (int i = 0; i < FUSE_SLOTS; i++) {
        FuseEnt* e2 = &s_fuse[i];
        if (!e2->key) continue;
        /* NEVER REPLAY ROOM DECALS (2026-08-24, player: "декали лезут в
         * экран, когда на них не смотришь; мигающие пусть мигают материалом").
         * A room semi (blood pool, glass, blinking plate) captured during the
         * room walk and replayed HERE runs in the GAME context instead:
         * g_dcvk_room_pass==0 hands the copy the 3% decal lift toward the eye
         * and a positive painter stamp, so a one-flip-stale duplicate floats
         * in front of its own wall - visible even standing still, smeared
         * while moving. The room re-emits every decal fresh each flip, so it
         * never needs fusing; blinking plates simply blink, as authored.
         * Game effects (muzzle flash, sparks) keep the fuse as before.
         * DC_FUSE_ROOM=1 restores the old behaviour. */
        { static int fr = -1;
          if (fr < 0) { const char* e = getenv("DC_FUSE_ROOM"); fr = (e && *e == '1') ? 1 : 0; }
          if (e2->room && !fr) continue; }
        /* GAME prims: replay only ADDITIVE blends (PS1 modes 1 and 3 - muzzle
         * flash, sparks, glows). Averaged/subtractive semis are DECALS: dino
         * blood pools, Regina's shadow disc, dark overlay plates - replaying
         * one paints a one-flip-stale ghost (player 2026-08-24: "к луже крови
         * от динозавров, мигающим плашкам и тени Регины - тот же фикс").
         * DC_FUSE_ADDONLY=0 restores the old behaviour. */
        { static int ao = -1;
          if (ao < 0) { const char* e = getenv("DC_FUSE_ADDONLY"); ao = (e && *e == '0') ? 0 : 1; }
          if (ao && e2->semi != 1 && e2->semi != 3) continue; }
        /* missing for exactly one present, and not already replayed twice in a
         * row (a replay must never keep itself alive) */
        const int ledw = e2->led && e2->wok && fuse_led_world_on();      /* 28.09 (5): огонёк — по миру, и на ходу */
        if (fuse_moved && !ledw) continue;
        if (e2->seen + 1 == now && e2->replayed != now) {
            e2->replayed = now;
            if (ledw) {                                                  /* слой — туда, где этот огонёк стоит в ЭТОМ показе */
                float sx, sy, sw;
                if (!dc_led_now_get(e2->lent, now, &sx, &sy, &sw) || !(sw > 1.f)) continue;   /* огонька в показе нет — и слоя нет */
                const float k = e2->lw0 / sw;                            /* ближе/дальше — экранный размер обратно глубине */
                float vv[3 * VERT_FLOATS];
                memcpy(vv, e2->v, sizeof(vv));
                for (int q = 0; q < 3; q++) {
                    vv[q * VERT_FLOATS]      = sx + e2->wv[q * 3] * k;
                    vv[q * VERT_FLOATS + 1]  = sy + e2->wv[q * 3 + 1] * k;
                    vv[q * VERT_FLOATS + 12] = e2->wv[q * 3 + 2] / k;
                }
                dcvk_push_tri(vv, e2->semi, e2->ztest);
                g_fuse_replayed_now++;
                continue;
            }
            /* REBASE ONTO THIS FLIP'S VRAM HALF (2026-08-24). Without this the
             * stored screen positions belong to the OTHER buffer half and the
             * replayed copy appears 256 px off - a translucent slab sitting in
             * the middle of the picture with nothing under it. Same family of
             * bug as the room_pass and per-piece-state ones: a replay must
             * restore the SUBMITTER'S CONTEXT, and the draw offset is part of
             * that context. DC_FUSE_OFFSET=0 restores the un-rebased replay. */
            float vv[3 * VERT_FLOATS];
            memcpy(vv, e2->v, sizeof(vv));
            int nox = 0, noy = 0;
            { extern void gpu_get_draw_offset(int *ox, int *oy);
              gpu_get_draw_offset(&nox, &noy); }
            int ddx = nox - e2->dox, ddy = noy - e2->doy;
            static int oc = -1;
            if (oc < 0) { const char* e = getenv("DC_FUSE_OFFSET"); oc = (e && *e == '0') ? 0 : 1; }
            if (oc && (ddx || ddy))
                for (int k = 0; k < 3; k++) {
                    vv[k * VERT_FLOATS]     += (float)ddx;
                    vv[k * VERT_FLOATS + 1] += (float)ddy;
                }
            { static int fl = -1; static long n = 0;
              if (fl < 0) { const char* e = getenv("DC_FUSE_LOG"); fl = (e && *e == '1') ? 1 : 0; }
              if (fl && (++n % 16) == 1)
                  rlog("[FUSE] replay #%ld semi=%d ztest=%d room=%d tpage=%.0f "
                       "xy=(%.0f,%.0f) off stored=(%d,%d) now=(%d,%d) shift=(%d,%d)\n",
                       n, e2->semi, e2->ztest, e2->room, e2->v[8], vv[0], vv[1],
                       e2->dox, e2->doy, nox, noy, ddx, ddy); }
            dcvk_push_tri(vv, e2->semi, e2->ztest);
            g_fuse_replayed_now++;
        }
    }
    s_fuse_replaying = 0;
}
/* ===== PRIM DUMP (2026-08-30) =====
 * One-shot CSV of every triangle the raster will draw this present — screen
 * xy, PGXP w, painter seq, tpage, vertex flags, queue kind/pass. Armed from
 * the debug port ("dump_prims"); written inside dcvk_render once the draw
 * list is complete (semiLate merged). This is the ground truth for depth
 * disputes (wall signs, laser beams) instead of guessing from screenshots. */
extern "C" volatile int  g_dcvk_primdump = 0;
extern "C" char          g_dcvk_primdump_path[260] = { 0 };
static void dcvk_primdump_write(int drawFrom) {
    /* BURST MODE (2026-08-31, sub-pixel shimmer forensics): touching
     * primburst.on next to the exe arms a CSV dump of the next 20 RENDERS
     * back to back - strictly one per rendered frame, no sampling alias.
     * TCP-paced one-shot dumps land 40-60 ms apart and beat against the
     * 33 ms tick, faking 2:1 speed raggedness in every differential. */
    {
        static int series = 0, sidx = 0, gate = 0;
        if (series == 0 && ((gate++ & 15) == 0)) {
            /* ME (17.09.2026): no primburst.on trigger file in a release */
        }
        if (series > 0) {
            series--;
            g_dcvk_primdump = 1;
            snprintf(g_dcvk_primdump_path, sizeof g_dcvk_primdump_path,
                     "prims_series_%03d.csv", sidx++);
        }
    }
    if (!g_dcvk_primdump) return;
    g_dcvk_primdump = 0;
    const char* path = g_dcvk_primdump_path[0] ? g_dcvk_primdump_path
                                               : "primdump.csv";
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "draw,kind,pass,room,wire,vi,x,y,u,v,r,g,b,seq,tpage,i1,i2,flags,w\n");
    for (size_t di = (size_t)drawFrom; di < s_draws.size(); di++) {
        const Draw& d = s_draws[di];
        for (int t = 0; t < d.count; t++) {
            const float* v = s_vmap + (size_t)(d.first + t) * VERT_FLOATS;
            fprintf(f, "%zu,%d,%d,%d,%d,%d,%.2f,%.2f,%.1f,%.1f,%.0f,%.0f,%.0f,"
                       "%.0f,%.0f,%.0f,%.0f,%.0f,%.4f\n",
                    di, d.kind, d.pass, d.room, d.wire, t,
                    v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                    v[7], v[8], v[9], v[10], v[11], v[12]);
        }
    }
    fclose(f);
    rlog("[VKR] primdump: %zu draws -> %s\n", s_draws.size(), path);
}
/* 22.09: сколько вершин НАШЕЙ комнаты в накопленном потоке (для правила «кадр без комнаты» в main.cpp) */
extern "C" int dcvk_stream_room_verts(void) {
    /* считаем только то, что реально попадает в кадр: на смене ракурса комната бывает в потоке, но вся мимо экрана */
    int n = 0;
    /* считаем ТЕКУЩУЮ сборку: room-draws прошлого кадра игры в потоке не в счёт */
    size_t from = 0;
    if (s_markCount > 0 && s_marks[s_markCount - 1].draws >= 0) from = (size_t)s_marks[s_markCount - 1].draws;
    for (size_t i = from; i < s_draws.size(); i++) {
        const Draw& d = s_draws[i];
        /* 22.09: считаем ВЕСЬ мир кадра — нашу комнату и трёхмерную геометрию игры (kind 0 и 2..5):
         * кадр, где нет ни того, ни другого, у игры не бывает — это провал на смене комнаты */
        if (!d.room || d.wire) continue;          /* только НАША комната текущей сборки */
        for (int t = 0; t + 3 <= d.count; t += 3) {      /* площадь треугольников комнаты в кадре, в сотнях точек */
            const float* p0 = s_vmap + (size_t)(d.first + t) * VERT_FLOATS;
            const float* p1 = p0 + VERT_FLOATS, *p2 = p0 + 2 * VERT_FLOATS;
            float ar = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
            if (ar < 0) ar = -ar;
            float cx = (p0[0] + p1[0] + p2[0]) / 3.f, cy = (p0[1] + p1[1] + p2[1]) / 3.f;
            if (cx >= -64.f && cx <= 384.f && cy >= -64.f && cy <= 560.f) n += (int)(ar * 0.5f / 100.f);
        }
    }
    return n;
}
/* 22.09: очищала ли игра кадр в текущей сборке (полноэкранный НЕПРОЗРАЧНЫЙ draw)? Если нет, на консоли в буфере
 * остаётся прошлая картинка, а наш растр рисует кадр с нуля — отсюда чёрные провалы на смене комнаты. */
extern "C" int dcvk_stream_has_clear(void) {
    size_t from = 0;
    if (s_markCount > 0 && s_marks[s_markCount - 1].draws >= 0) from = (size_t)s_marks[s_markCount - 1].draws;
    for (size_t i = from; i < s_draws.size(); i++) {
        const Draw& d = s_draws[i];
        if (d.room || d.pass != 0 || d.kind != 1 || d.wire) continue;
        for (int t = 0; t + 3 <= d.count; t += 3) {
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (int v = 0; v < 3; v++) {
                const float* p = s_vmap + (size_t)(d.first + t + v) * VERT_FLOATS;
                if (p[0] < x0) x0 = p[0]; if (p[0] > x1) x1 = p[0];
                if (p[1] < y0) y0 = p[1]; if (p[1] > y1) y1 = p[1];
            }
            if ((x1 - x0) >= 200.f && (y1 - y0) >= 150.f) return 1;
        }
    }
    return 0;
}
extern "C" int dcvk_dump_last(const char* path) {
    /* The readback is tightly packed at the ACTIVE render size, not the
     * max-allocated framebuffer size. */
    dx_full_readback();       /* 09.10: кадр ушёл в общую с D3D11 текстуру — дочитать целиком */
    if (!s_rbMap || !s_outW || !s_outH) return 0;
    writeBMP(path, (const unsigned int*)s_rbMap, s_outW, s_outH);
    return 1;
}
/* Copy the last rendered frame, decimated, into a caller buffer (the flight
 * recorder keeps a ring of these in RAM so a mark can dump the seconds BEFORE
 * it). Returns 0 if nothing has been rendered yet or the buffer is too small. */
extern "C" int dcvk_copy_last_small(unsigned int* dst, int cap, int f, int* ow, int* oh) {
    dx_full_readback();       /* 09.10 */
    if (!s_rbMap || !s_outW || !s_outH || !dst) return 0;
    if (f < 1) f = 1;
    int w = s_outW / f, h = s_outH / f;
    if (w < 1 || h < 1 || w * h > cap) return 0;
    const unsigned int* src = (const unsigned int*)s_rbMap;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[(size_t)y * w + x] = src[(size_t)(y * f) * s_outW + (size_t)x * f];
    if (ow) *ow = w; if (oh) *oh = h;
    return 1;
}
extern "C" void dcvk_write_bmp(const char* path, const unsigned int* px, int w, int h) {
    writeBMP(path, px, w, h);
}
/* Same, decimated by an integer factor (every Nth pixel) - for per-frame
 * forensic bursts where hundreds of frames are written (DC_DOOR_DUMP). */
extern "C" int dcvk_dump_last_small(const char* path, int f) {
    dx_full_readback();       /* 09.10 */
    if (!s_rbMap || !s_outW || !s_outH) return 0;
    if (f < 1) f = 1;
    int w = s_outW / f, h = s_outH / f;
    if (w < 1 || h < 1) return 0;
    unsigned int* tmp = (unsigned int*)malloc((size_t)w * h * 4);
    if (!tmp) return 0;
    const unsigned int* src = (const unsigned int*)s_rbMap;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            tmp[(size_t)y * w + x] = src[(size_t)(y * f) * s_outW + (size_t)x * f];
    writeBMP(path, tmp, w, h);
    free(tmp);
    return 1;
}

// --------------------------------------------------------------- selftest --
static void writeBMP(const char* path, const unsigned int* rgba, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int stride = w * 3, pad = (4 - (stride & 3)) & 3;
    int dataSz = (stride + pad) * h;
    unsigned char hdr[54] = { 'B','M' };
    *(int*)(hdr + 2) = 54 + dataSz; *(int*)(hdr + 10) = 54;
    *(int*)(hdr + 14) = 40; *(int*)(hdr + 18) = w; *(int*)(hdr + 22) = h;
    *(short*)(hdr + 26) = 1; *(short*)(hdr + 28) = 24;
    *(int*)(hdr + 34) = dataSz;
    fwrite(hdr, 1, 54, f);
    unsigned char padb[3] = { 0, 0, 0 };
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            unsigned int p = rgba[(size_t)y * w + x];
            unsigned char px[3] = { (unsigned char)((p >> 16) & 255),
                                    (unsigned char)((p >> 8) & 255),
                                    (unsigned char)(p & 255) };
            fwrite(px, 1, 3, f);
        }
        if (pad) fwrite(padb, 1, pad, f);
    }
    fclose(f);
}
