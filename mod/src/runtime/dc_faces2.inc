/* ===== ЛИЦА v2: ВСЕ ПЕРСОНАЖИ, РЕЧЬ ПО РОЛЯМ И ЭМОЦИИ (26.09.2026, игрок: «добавь все анимированные лица в игру,
 * подставь под аудио. Пусть говорят как регина» + эмоции по сценарию реплик) ======================================
 * Включается в gpu.c (нужны vram, sw_vram_transfer_in, помощники прежнего лица Регины выше).
 * Файлы — assets/faces/<id>_eyes.bin и <id>_talk.bin (приложение tools\face_eyes, «В игру: все лица»); новые и изменённые
 * файлы подхватываются на лету (27.09 (2): по уведомлению Windows об изменении папки; DC_FACE_DIR_WATCH=0 — просмотр раз в 2 с).
 * ГДЕ ЛИЦО: у НПС место текстуры в VRAM зависит от комнаты (страница 512..960, строка палитры) — ищем по содержимому:
 * у каждого варианта (костюма) подпись всего кропа вне глаз и рта; кандидаты — 32 страницы (x 0..960 шаг 64,
 * y 0/256): выборка 48 текселей, затем полная сверка (≤ 8 несовпадений). Регина — тем же поиском (её страница 896,0).
 * ГЛАЗА: у каждого лица своё моргание и взгляд (как у Регины); кадр — из сетки своей эмоции: «спокойно» — полная сетка
 * (взгляд 13×5 × веко 7), остальные — крупнее шаг, два уровня силы (0.5 и 1).
 * РЕЧЬ И ЭМОЦИИ: сценарий (кто какую фразу говорит, эмоции на фразах и словах) запечён в <id>_talk.bin: на каждый шаг
 * клипа (30/с) — состояние рта (кадр прямоугольника рта этого лица) и эмоция с уровнем. Клип — по звучащему сектору
 * XA: файл, канал, сектор от начала файла; таблица файлов XA в <id>_talk.bin сверяется с диском (версия игры) —
 * не совпала → речь этого файла не подставляется. Слышимое место — очередь CD-звука SPU (как у Регины).
 * Сейв: VRAM уходит с исходными глазами и ртами всех лиц. DC_FACE_V2=0 — прежнее (только Регина, face_anim.bin/
 * face_talk.bin); без папки assets/faces — тоже прежнее. DC_FACE_NPC=0 — только Регина; DC_FACE_EMO=0 — глаза без
 * эмоций; DC_FACE_DIR — своя папка; DC_FACE_LOG=1 — строки face2: раз в 10 с (2 — и каждая запись: face2w:). Остальное — как у Регины
 * (DC_FACE_ANIM, DC_TALK, DC_TALK_LEAD, DC_FACE_LOOK_AMP).
 * Файлы сжаты: 'DCZ1', u32 размер, поток zlib (кадры похожи — в ~10 раз меньше); распаковка — f2_inflate ниже.
 * КАДРЫ ЛИЦА: DC_FACE_FPS=N — лицо своими кадрами N/с (ретро; по умолчанию 0 — каждую развёртку, как было; игрок про
 * 12 кадров/с: «Поставь сколько было»): всё считается на границе кадра лица и держится до следующего.
 * РЕЗКОСТЬ (игрок: «все анимации интерполируются очень гладко. Сделай резче» → «взгляд, моргание, эмоции и рот пусть
 * переключаются с блендом в один промежуточный кадр»): DC_FACE_SHARP (по умолчанию 1; 0 — плавно, как было) — каждая смена
 * через DC_FACE_TWEEN промежуточных кадров по 1/30 с (по умолчанию 1; смесь прежнего и нового): взгляд (было — перетекал
 * 0.07 с), моргание ступенями — полузакрыто 0.033 с, закрыто 0.067, полузакрыто 0.033 (игрок: «сделай моргания быстрее»;
 * плавное — 0.16 с, прежнее 0.26 — DC_FACE_BLINK_FAST=0). Рот и эмоции — так же из запекания (приложение, «Переходы»).
 * ПОСЛЕ ФРАЗЫ (игрок: «в конце каждой фразы … лицо принимает какую то рандомную эмоцию»): пауза CD в конце куска
 * реплики обнуляла счёт CD-звука SPU — «слышимое место» падало в первый отрезок куска, и лицо показывало эмоцию и рот
 * начала реплики. Счёт теперь сквозной (spu.c); пошёл назад (сейв, DC_TALK_RESET_OLD=1) — отрезки забываются.
 * ЧЕЛЮСТЬ (27.09, игрок: «у голов персонажей при открывании текстурного рта сделать небольшое смещение вертексов их
 * челюсти с подбородком, чтобы максимально реалистично передать что рот открывается»): разметка — приложение лица, режим
 * «3D: челюсть» (head3d.js): вершины головы с весом, шарнир (ось X кости головы через точку y, z), угол при «Челюсть вниз»
 * рта = 1, опускание. Запекается хвостом <id>_talk.bin — секция 'JAW1' после клипов (старый рантайм её не читает):
 * вершины челюсти, исходный блок вершин головы (подпись — по ней блок ищется в RAM; у Регины 0x80180B40 — вершины 114..187 модели CORE.DAT; у НПС блок головы в модели комнаты, место зависит от комнаты)
 * и раскрытие челюсти на каждое состояние рта. Игра проецирует вершины модели из RAM каждый кадр (func_80073F10, вершины в
 * пространстве своей кости) — сдвинутые вершины пишем прямо в модель; вершина поворачивается вокруг шарнира на
 * раскрытие × угол × вес (как в приложении, jawPos). Рот закрыт — вершины исходные; сейв — с исходными (savestate.c →
 * dc_jaw_savestate); опция лиц выключена — исходные. DC_JAW=0 — челюсть не двигается; DC_JAW_LEAD (1/30 с) — вершины по
 * состоянию рта на столько вперёд (кадр текстуры виден сразу, вершины — со следующей проекции игры); DC_JAW_FORCE=x —
 * челюсть всегда раскрыта на x (проверка); DC_JAW_LOG=1 — строки jaw: на каждую смену.
 * ВСЕ КОПИИ ЛИЦА В VRAM (27.09, игрок: «слот 10. почему текстура рта у гейла не работает тут? … Челюсть у него двигается»):
 * игра грузит текстуру персонажа на страницу СВОЕЙ комнаты (Гейл: ST10E и ST611 — 704,0, остальные — 640,0, ST50F — 576,0;
 * Рик: 576, 640 или 704), а копия прошлой комнаты лежит, пока её не затрут. Поиск брал первую найденную — в ST10E старую 640,0:
 * глаза и рот писались туда, а игра рисовала Гейла с 704,0 (блок вершин головы в RAM один — челюсть двигалась). Теперь глаза
 * и рот пишутся во ВСЕ копии (до F2_MAXCP; у каждой свой вариант — в ST50F обычный Гейл на 576 и раненый на 640), новые копии
 * ищутся раз в 30 развёрток и тогда, когда прежние на месте. DC_FACE_ALLCOPIES=0 — как было (первая найденная).
 * БЕЗ ТАЙМЕРА (27.09 (2), игрок: «а оптимизировать это можно? чтобы не было проверок раз в пол секунды»): gpu.c отмечает
 * страницы VRAM 64x256, которые изменила ИГРА (s_f2_vpages: заливка 0xA0, копия 0x80, заливка 0x02, примитив в текстуру,
 * загрузка сейва). Копии ищутся и сверяются только на отмеченных страницах (f2_find_pages; после чтения файлов лица и
 * загрузки сейва — на всех); пока игра не трогает страницы, поиска и полной сверки нет. Перед записью кадра — 48 точек
 * подписи копии (страховка: не совпали — страница сверяется заново, строка в журнал). DC_FACE_PAGE_EVENTS=0 — по таймеру
 * (раз в 30 развёрток), как в первой версии всех копий. Проверка отметок: DC_FACE_PAGE_CHECK=1 — хеш всех 32 страниц
 * каждую развёртку, изменение без отметки — строка в журнал (только для тестов, дорого). DC_FACE_LOG=1 — раз в 10 с
 * время работы лиц (всего и на поиск/сверку копий).
 * ⭐ ФОРМАТ v3 «ТОЛЬКО НАШЕ» (04.10.2026, игрок: «а что с анимацией лиц? тоже все перевел в наш код без чужого?» →
 * «Переделать сейчас» + «но если что, чтобы мог откатится»). В файлах v2 лежат куски игры: подпись — весь кроп лица из
 * текстуры, исходные глаза и рот, блок вершин головы модели. В v3 ('DCE3'/'DCT3', те же имена файлов; делает
 * tools\face_v3.py из файлов v2) вместо них хеши (FNV-1a 64): выборки 48 текселей, кропа вне глаз и рта, исходных глаз,
 * исходного рта, вершин головы вне челюсти и вершин челюсти; кадры глаз и рта — разница (XOR) с исходником, сжатая своим
 * кодом (разница с похожим прежним кадром + серии нулей, f2_fs_decode). Исходные глаза, рот и вершины челюсти рантайм
 * берёт из VRAM/RAM, когда первая копия найдена и её хеш совпал (f2_capture, f2_jaw_at) — до записи наших кадров туда;
 * дальше они живут в памяти до перечитывания файла (сейв, выключение опции, перечитывание — пишутся они). Блок головы в
 * RAM ищется по полю кости (у всех вершин блока одна кость — 7) и хешу. Замер 04.10 (384 копии лиц в сейвах): вне глаз
 * и рта копии совпадают с исходником байт в байт, глаза и рот — исходные, так что точный хеш вместо «≤ 8 несовпадений»
 * ничего не теряет. Откат: DC_FACE_V3=0 — файлы v3 не читаются (файлы v2 работают как были); релиз с файлами v2 —
 * rebuild.ps1 -FacesV2. */
#include <io.h>
extern uint8_t* memory_get_ram_ptr(void);           /* memory.c — челюсть пишет вершины модели */

#define F2_MAXCH 12
#define F2_MAXV 6
#define F2_MAXE 24
#define F2_NS 48
#define F2_MAXJ 64                                  /* вершин челюсти */
#define F2_MAXJB 512                                /* вершин в блоке головы */
#define F2_MAXCP 4                                  /* копий лица в VRAM */
static const char F2_SERIAL[] = "SLUS_009.22";

typedef struct {
    uint16_t var; FaceRect r; const uint8_t* sig; const uint8_t* fr; uint8_t* orig;
    uint16_t sx[F2_NS], sy[F2_NS]; int ns;
    uint64_t hsamp, hsig, heye; int have;            /* v3: хеши выборки, кропа вне глаз и рта, исходных глаз; have — orig взят из VRAM */
} F2Var;
typedef struct { int xa; uint16_t ch; uint32_t nsec, nsteps; float secdur; const uint8_t* rel; const uint8_t* steps; } F2Clip;
typedef struct {
    char id[32];
    /* глаза: <id>_eyes.bin */
    uint8_t* eb; char ep[512]; long long emt, esz;
    uint16_t u0, v0, W, H, nx, ny, nl, enx, eny, enl, nemo, nlev;
    float dx0, dxs, dy0, dys, edx0, edxs, edy0, edys, lev[4];
    char emo[F2_MAXE][24];
    int nv; F2Var v[F2_MAXV];
    /* речь и эмоции: <id>_talk.bin */
    uint8_t* tb; char tp[512]; long long tmt, tsz;
    FaceRect mr; uint16_t fps, nclips; uint32_t nst; int tnemo, talk_ok;
    char temo[F2_MAXE][24]; int emap[F2_MAXE];      /* эмоции talk и их сетки глаз (0 — спокойно) */
    int ntv; uint16_t tvar[F2_MAXV]; const uint8_t* tfr[F2_MAXV];
    F2Clip* clips;
    /* где лицо и что записано: копии в VRAM (вариант, страница); cur/px/py — копия, с которой сейчас работают помощники */
    int cur, px, py, last_e, last_m, force;
    int ncp, cpk[F2_MAXCP], cpx[F2_MAXCP], cpy[F2_MAXCP];
    uint32_t pend;                                   /* страницы VRAM, изменённые игрой с прошлого взгляда (все — после чтения файлов, сейва) */
    /* моргание и взгляд */
    uint32_t rng; float lk_fx, lk_fy, lk_tx, lk_ty; double lk_t0, lk_next, bl_t0, bl_next; int bl_double;
    long writes, mwrites, blinks;
    int hold_e, hold_m;                              /* кадр глаз и состояние рта, держимые до следующего кадра лица (-1 — нет) */
    int et_emo, et_from, et_cls; double et_t0;       /* эмоция глаз: цель, прежняя, класс (0 спокойно, 1 половина, 2 полная; -1 нет), начало перехода */
    /* челюсть: секция 'JAW1' в <id>_talk.bin — блок вершин головы jfirst..+jcount модели из jnv вершин, вершины челюсти jvi с весом jw */
    int jaw_ok, jn, jq, japplied, hold_j; uint16_t jfirst, jcount, jvi[F2_MAXJ]; int16_t jmap[F2_MAXJB];
    uint32_t jnv, jaddr; float jw[F2_MAXJ], jpy, jpz, jang, jdrop; const uint8_t* jsig; const uint8_t* jst;
    int16_t jcur[F2_MAXJ][3]; long jwrites; uint64_t jseek_vb; int jss;
    /* 28.09 (7): мёртв по сюжету — актёр лица, его поза (узлы костей) и когда в последний раз двигался / говорил */
    int dead, dslot, dhave; uint64_t dchk_vb, dmove_vb, dtalk_vb, dseek_vb; int16_t dpts[15][3];
    /* 03.10: начало фразы внутри идущего куска — развёрток удержания покоя осталось; прошлая развёртка «говорит/играет лицом» */
    int onh, was_talk; long onh_held, onh_cut; uint64_t onh_vb;
    /* 04.10 — формат v3: 2/3 у файлов глаз и речи; рот, исключённый из хеша кропа (из файла глаз); раскрытые кадры рта;
     * исходный рот каждого варианта речи (хеш, взят ли из VRAM); вершины головы — хеши вне челюсти и челюсти, кость блока,
     * исходные вершины челюсти (взяты из RAM) */
    int efmt, tfmt, xmr_on; FaceRect xmr; uint8_t* tdec;
    uint64_t hmouth[F2_MAXV]; int mhave[F2_MAXV]; uint8_t* morig[F2_MAXV];
    uint64_t jhn, jhj; uint16_t jbone; int jhave; uint8_t jorig[F2_MAXJ][6];
    long nocap;
} F2Char;
static F2Char s_f2[F2_MAXCH];
static int s_f2_n = 0, s_f2_on = -1, s_f2_npc = 1, s_f2_emo = 1, s_f2_talk = 1, s_f2_log = 0, s_f2_tlog = 0;
static uint64_t s_f2_played = 0, s_f2_played_vb = 0;       /* слышимое место CD-звука и развёртка, когда оно сдвинулось */
static int s_f2_stalled = 0;
static int s_f2_fps = 0, s_f2_bfast = 1, s_f2_sharp = 1;   /* DC_FACE_FPS (0 — каждую развёртку), DC_FACE_BLINK_FAST, DC_FACE_SHARP */
static int s_f2_tween = 1;                                  /* DC_FACE_TWEEN — промежуточных кадров (по 1/30 с) на смену */
static int s_f2_bcalm = 1;                                  /* DC_FACE_BLINK_CALM — реже моргают (21.10), 0 — как было */
static long s_f2_fr = -1;                                   /* номер текущего кадра лица */
static uint64_t s_f2_vb = 0;
static char s_f2_dir[512] = "";
typedef struct { uint64_t start; uint32_t frames; int xa, ch, rel; } F2Seg;
static F2Seg s_f2seg[64];
static unsigned s_f2seg_n = 0;
static int s_f2log_xa = -2, s_f2log_ch = -1, s_f2log_rel = -100;
/* 27.09: рот и эмоции — на DC_FACE_DELAY с позже звука, выданного SPU (игрок: «анимация лиц немного быстрее звука… на 0.25
 * секунды»; 0 — как было). Выданный звук ещё лежит в очереди звукового устройства (до 0.2 с) и в его буфере. Линия задержки
 * по развёрткам: на лице то, что звучало D с назад, — хвост фразы доигрывается после остановки звука, пауза тоже приходит позже */
typedef struct { uint64_t vb; F2Seg sg; double frac; int has; } F2Hist;
static F2Hist s_f2hist[128];
/* ⭐ 01.10.2026 (игрок: «убери отставание, которое я раньше попросил сделать для рта в игре»; рты теперь поставлены по звукам
 * текста — lipsync_text.py): по умолчанию 0. DC_FACE_DELAY=0.25 — как было 27.09–01.10. Линия задержки остаётся на опережение
 * челюсти: вершины челюсти игра проецирует со своего следующего кадра (DC_JAW_LEAD = 1/30 с), поэтому рот-текстура идёт на эти
 * 1/30 с позже звука, а челюсть — по звуку «сейчас» (иначе в начале фразы текстура открывалась на кадр раньше челюсти).
 * s_f2_dline — задержка линии: max(DC_FACE_DELAY, DC_JAW_LEAD при челюсти). */
static double s_f2_delay = 0.0;
static double s_f2_dline = 0.0;
static int s_f2_jaw = 1, s_f2_jlog = 0;                      /* DC_JAW, DC_JAW_LOG */
static double s_f2_jlead = 1.0 / 30.0;                      /* DC_JAW_LEAD */
static int s_f2_jhist = 1;                                  /* DC_JAW_LEAD_HIST: опережение челюсти — из линии задержки (27.09 вечер) */
static float s_f2_jforce = -1.f;                            /* DC_JAW_FORCE (-1 — по рту) */
static int s_f2_allcp = 1;                                  /* DC_FACE_ALLCOPIES: глаза и рот во все копии лица в VRAM */
static int s_f2_ev = 1, s_f2_pchk = 0;                      /* DC_FACE_PAGE_EVENTS (без таймера), DC_FACE_PAGE_CHECK (проверка отметок) */
static int s_f2_dead = 1, s_f2_dead_lie = 400, s_f2_dead_low = 750;   /* DC_FACE_DEAD, DC_FACE_DEAD_LIE, DC_FACE_DEAD_LOW (высота головы) */
/* ⭐ 03.10 — ЛИШНИЙ КАДР ЛИЦА НА КРАЮ ФРАЗЫ (игрок, слот 46: «в конце фразы или вначале лицо персонажа меняется на кадр на
 * какое-то рандомное… это точно баг»; «этот баг у всех есть»). Замер (skipq/face46.py + face_spikes.py, DC_FACE_LOG=2,
 * DC_CD_LOG=1, сцена Регины и Рика ST106): игра крутит КУСОК канала XA, каждые 2 кадра спрашивает позицию (GetlocP 0x11) и
 * ставит паузу (0x09), когда кусок кончился. Сектор, дочитанный перед паузой, уже несёт НАЧАЛО СЛЕДУЮЩЕЙ фразы — лицо её
 * говорящего на 2–4 развёртки открывало рот и вспыхивало эмоцией (Регина: глаза 1095 -> 1200 -> 1095, рот 13/14; рот 0 -> 237
 * -> 0), а настоящая фраза шла через 0.4 с новым куском (SetLoc + ReadS). Пик звука в эти развёртки — 0..16 из 32767 (тишина:
 * разметка по тексту тянет начало слова к краю тишины, а голоса ещё нет); у настоящих начал — от сотен до тысяч (spu.c
 * spu_cd_audio_peak). Правило («нет слов — лицо в покое»): из покоя (рот, челюсть или эмоция) лицо выходит, только когда в звуке
 * пошёл голос — пик за 2 развёртки уже ВЫДАННОГО звука >= DC_FACE_VOICE_THR (1000; очередь при паузе игры выбрасывается, её не слышно — в перебегах Рика там было 301..512, вдох); пауза игры раньше голоса — лицо так и
 * остаётся в покое. Для всех лиц. DC_FACE_ONSET_HOLD=0 — как было. */
static int s_f2_onset = 1, s_f2_vthr = 1000;
static int s_f2_v3 = 1;                                     /* DC_FACE_V3: читать файлы формата v3 (04.10) */
static uint8_t s_f2_escr[256 * 256], s_f2_mscr[256 * 256];   /* v3: кадр глаз и кадр рта = исходник XOR разница */
static long long s_f2_tall = 0, s_f2_tfind = 0, s_f2_tscan = 0;   /* время лиц, поиска/сверки копий, просмотра папки (такты QPC) за 10 с */
static long s_f2_nfind = 0;
static uint32_t s_f2_phash[32]; static int s_f2_phave = 0; static long s_f2_pmarked = 0, s_f2_punmarked = 0;
/* 27.09 (2): папка лиц — новые файлы из приложения («В игру») по уведомлению Windows: поток ждёт изменения папки (в ядре,
 * без работы) и поднимает флаг; развёртка только читает флаг. Было — просмотр папки раз в 2 с (~0.2 мс); DC_FACE_DIR_WATCH=0 — как было */
static int s_f2_dwatch = 1;                                 /* DC_FACE_DIR_WATCH; -1 — поток не вышел, просмотр раз в 2 с */
static volatile LONG s_f2_dirflag = 0;
static HANDLE s_f2_dirh = NULL;
static DWORD WINAPI f2_dir_thread(LPVOID p) {
    HANDLE h = (HANDLE)p;
    for (;;) {
        if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0) return 0;
        if (!FindNextChangeNotification(h)) { InterlockedExchange(&s_f2_dirflag, 1); return 0; }   /* сначала снова ждать, потом флаг */
        InterlockedExchange(&s_f2_dirflag, 1);
    }
}
/* нужно ли смотреть папку в эту развёртку */
static int f2_dir_changed(void) {
    if (s_f2_dwatch == 1 && !s_f2_dirh && s_f2_dir[0]) {
        HANDLE h = FindFirstChangeNotificationA(s_f2_dir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        HANDLE t = (h != INVALID_HANDLE_VALUE) ? CreateThread(NULL, 64 * 1024, f2_dir_thread, h, 0, NULL) : NULL;
        if (t) {
            CloseHandle(t); s_f2_dirh = h;
            fprintf(stderr, "face2: папка лиц %s — новые файлы по уведомлению Windows (просмотра раз в 2 с нет)%c", s_f2_dir, 10);
            return 1;                                       /* что изменилось до уведомлений — один просмотр сразу */
        }
        if (h != INVALID_HANDLE_VALUE) FindCloseChangeNotification(h);
        s_f2_dwatch = -1;
        fprintf(stderr, "face2: уведомления папки лиц %s не вышли — просмотр раз в 2 с, как было%c", s_f2_dir, 10);
    }
    if (s_f2_dirh) return s_f2_dirflag && InterlockedExchange(&s_f2_dirflag, 0);
    return s_f2_vb % 120 == 0;
}

/* ME V3 (04.10.2026): no deflate decoder in the release - the face files in the pack are v3 (hashes and
 * differences, our own coder: f2_fs_decode; rebuild.ps1, tools/face_v3.py) or, with rebuild.ps1 -FacesV2,
 * v2 unpacked by Python's zlib. The decoder of the dev tree followed puff.c and is not shipped. */
static uint8_t* f2_unzip(const uint8_t* b, size_t n, size_t* outn) {
    (void)b; (void)n; (void)outn;
    return NULL;
}
/* файл лица: прочитать и, если сжат, распаковать */
static uint8_t* f2_read(const char* path, char* used, size_t usedsz, long long* mt, long long* sz, size_t* got) {
    const char* paths[] = { path };
    uint8_t* b = face_read((const char* const*)paths, 1, used, usedsz, mt, sz, got);
    if (b && *got >= 4 && memcmp(b, "DCZ1", 4) == 0) {
        size_t rn = 0;
        uint8_t* r = f2_unzip(b, *got, &rn);
        free(b);
        b = r; *got = rn;
    }
    return b;
}

static float f2_rand(F2Char* c) {
    c->rng ^= c->rng << 13; c->rng ^= c->rng >> 17; c->rng ^= c->rng << 5;
    return (float)(c->rng >> 8) * (1.0f / 16777216.0f);
}
static int f2_isdir(const char* p) {
    struct _stat64 st;
    return p && *p && _stat64(p, &st) == 0 && (st.st_mode & _S_IFDIR);
}
/* ---- v3: хеш FNV-1a 64 и кадры «разница с прежним кадром + серии нулей» ---- */
#define F2_FNV0 0xCBF29CE484222325ull
static inline uint64_t f2_fnv1(uint64_t h, uint8_t b) { return (h ^ b) * 0x100000001B3ull; }
static uint64_t f2_fnv(uint64_t h, const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) h = f2_fnv1(h, p[i]); return h; }
/* стопка из N кадров по S байт: на кадр u32 ref (0xFFFFFFFF — с нуля, иначе номер прежнего кадра), u32 длина, записи:
 * байт c < 0x80 — c+1 байт XOR-ом в кадр, c >= 0x80 — c-0x7F байт без изменений (хвост кадра можно не писать) */
static int f2_fs_decode(const uint8_t* p, size_t n, uint32_t N, uint32_t S, uint8_t* out) {
    size_t o = 0;
    for (uint32_t k = 0; k < N; k++) {
        if (n - o < 8) return 0;
        const uint32_t ref = f_u32(p + o), len = f_u32(p + o + 4);
        o += 8;
        if (len > n - o) return 0;
        uint8_t* d = out + (size_t)k * S;
        if (ref == 0xFFFFFFFFu) memset(d, 0, S);
        else if (ref < k) memcpy(d, out + (size_t)ref * S, S);
        else return 0;
        const uint8_t* q = p + o; const uint8_t* e = q + len;
        size_t pos = 0;
        while (q < e) {
            const unsigned c = *q++;
            if (c < 0x80u) {
                const size_t l = (size_t)c + 1;
                if (l > (size_t)(e - q) || pos + l > S) return 0;
                for (size_t i = 0; i < l; i++) d[pos + i] ^= q[i];
                q += l; pos += l;
            } else {
                pos += (size_t)c - 0x7Fu;
                if (pos > S) return 0;
            }
        }
        o += len;
    }
    return o == n;
}
/* ---- тексели кропа лица c на его странице (px, py) ---- */
static inline uint8_t f2_tex(const uint16_t* vr, int px, int py, const F2Char* c, unsigned x, unsigned y) {
    const unsigned u = c->u0 + x;
    const uint16_t hw = vr[(py + c->v0 + y) * 1024u + px + (u >> 1)];
    return (uint8_t)((u & 1) ? (hw >> 8) : (hw & 0xFF));
}
static int f2_rows(const uint16_t* vr, const F2Char* c, const FaceRect* r, const uint8_t* src, uint16_t* out, int* hx0, int* nh) {
    const unsigned us = c->u0 + r->rx, ue = us + r->rw - 1, h0 = us >> 1, h1 = ue >> 1, n = h1 - h0 + 1;
    for (unsigned y = 0; y < r->rh; y++) {
        const unsigned vy = c->py + c->v0 + r->ry + y;
        for (unsigned i = 0; i < n; i++) {
            uint16_t w = vr[vy * 1024u + c->px + h0 + i];
            for (unsigned bt = 0; bt < 2; bt++) {
                const unsigned u = 2 * (h0 + i) + bt;
                if (u < us || u > ue) continue;
                const uint8_t v = src[y * r->rw + (u - us)];
                w = bt ? (uint16_t)((w & 0x00FF) | (v << 8)) : (uint16_t)((w & 0xFF00) | v);
            }
            out[y * n + i] = w;
        }
    }
    *hx0 = (int)(c->px + h0); *nh = (int)n;
    return (int)r->rh;
}
static void f2_blit(const F2Char* c, const FaceRect* r, const uint8_t* src) {
    int hx0, nh, rows = f2_rows(vram, c, r, src, s_face_buf, &hx0, &nh);
    sw_vram_transfer_in(hx0, (int)(c->py + c->v0 + r->ry), nh, rows, s_face_buf);
}
static void f2_put(uint16_t* vr, const F2Char* c, const FaceRect* r, const uint8_t* src) {
    int hx0, nh, rows = f2_rows(vr, c, r, src, s_face_buf, &hx0, &nh);
    const int vy0 = (int)(c->py + c->v0 + r->ry);
    for (int y = 0; y < rows; y++) memcpy(vr + (vy0 + y) * 1024 + hx0, s_face_buf + y * nh, (size_t)nh * 2);
}
static int f2_rect_eq(const uint16_t* vr, const F2Char* c, const FaceRect* r, const uint8_t* src) {
    for (unsigned y = 0; y < r->rh; y++)
        for (unsigned x = 0; x < r->rw; x++)
            if (f2_tex(vr, c->px, c->py, c, r->rx + x, r->ry + y) != src[y * r->rw + x]) return 0;
    return 1;
}
static const FaceRect* f2_mrect(const F2Char* c) { return (c->talk_ok && s_f2_talk) ? &c->mr : NULL; }
/* рот, исключённый из подписи: v2 — рот файла речи (если речь читается), v3 — рот из файла глаз (хеш посчитан без него) */
static const FaceRect* f2_xrect(const F2Char* c) { return c->efmt == 3 ? (c->xmr_on ? &c->xmr : NULL) : f2_mrect(c); }
/* вариант речи для варианта глаз k (-1 — нет) */
static int f2_tv(const F2Char* c, int k) {
    if (!c->talk_ok || k < 0) return -1;
    for (int i = 0; i < c->ntv; i++) if (c->tvar[i] == c->v[k].var) return i;
    return -1;
}
/* кадры рта варианта k (NULL — у варианта нет; v3 — ещё нет исходного рта) */
static const uint8_t* f2_mouth(const F2Char* c, int k, uint32_t st) {
    if (!c->talk_ok || k < 0 || st >= c->nst) return NULL;
    const int i = f2_tv(c, k);
    if (i < 0) return NULL;
    const size_t S = (size_t)c->mr.rw * c->mr.rh;
    if (c->tfmt != 3) return c->tfr[i] + (size_t)st * S;
    if (!c->mhave[i]) return NULL;
    const uint8_t* d = c->tfr[i] + (size_t)st * S;
    for (size_t j = 0; j < S; j++) s_f2_mscr[j] = c->morig[i][j] ^ d[j];
    return s_f2_mscr;
}
/* исходный рот варианта k (v2 — кадр 0, v3 — взятый из VRAM) */
static const uint8_t* f2_mouth0(const F2Char* c, int k) {
    if (c->tfmt != 3) return f2_mouth(c, k, 0);
    const int i = f2_tv(c, k);
    return (i >= 0 && c->mhave[i]) ? c->morig[i] : NULL;
}
/* кадр глаз fi варианта v (v3 — исходные глаза XOR разница; NULL — исходных ещё нет) */
static const uint8_t* f2_eyefr(const F2Char* c, const F2Var* v, int fi) {
    const size_t S = (size_t)v->r.rw * v->r.rh;
    if (c->efmt != 3) return v->fr + (size_t)fi * S;
    if (!v->have) return NULL;
    const uint8_t* d = v->fr + (size_t)fi * S;
    for (size_t j = 0; j < S; j++) s_f2_escr[j] = v->orig[j] ^ d[j];
    return s_f2_escr;
}
/* выборка для быстрого отсева страниц: 48 текселей подписи вне глаз и рта, по сетке */
static void f2_samples(F2Char* c) {
    const FaceRect* mr = f2_xrect(c);
    for (int k = 0; k < c->nv; k++) {
        F2Var* v = &c->v[k]; v->ns = 0;
        for (unsigned y = 2; y + 2 < c->H && v->ns < F2_NS; y += (c->H > 16 ? c->H / 8 : 1))
            for (unsigned x = 3; x + 3 < c->W && v->ns < F2_NS; x += (c->W > 12 ? c->W / 6 : 1)) {
                if (face_in(&v->r, x, y) || face_in(mr, x, y)) continue;
                v->sx[v->ns] = (uint16_t)x; v->sy[v->ns] = (uint16_t)y; v->ns++;
            }
    }
}
/* вариант v на странице (px, py) по выборке 48 текселей (≤ 2 несовпадений) — быстрый отсев и сверка перед записью */
static int f2_sig_quick(const uint16_t* vr, const F2Char* c, const F2Var* v, int px, int py) {
    if (px < 0 || py < 0 || px + ((c->u0 + c->W + 1) >> 1) > 1024 || py + c->v0 + c->H > 512) return 0;
    if (c->efmt == 3) {                                          /* v3: хеш выборки — точно */
        uint64_t h = F2_FNV0;
        for (int i = 0; i < v->ns; i++) h = f2_fnv1(h, f2_tex(vr, px, py, c, v->sx[i], v->sy[i]));
        return h == v->hsamp;
    }
    int bad = 0;
    for (int i = 0; i < v->ns; i++)
        if (f2_tex(vr, px, py, c, v->sx[i], v->sy[i]) != v->sig[v->sy[i] * c->W + v->sx[i]] && ++bad > 2) return 0;
    return 1;
}
/* вариант v на странице (px, py)? выборка (≤ 2 несовпадений), потом весь кроп вне глаз и рта (≤ 8); v3 — хеши, точно */
static int f2_sig_at(const uint16_t* vr, const F2Char* c, const F2Var* v, int px, int py) {
    if (!f2_sig_quick(vr, c, v, px, py)) return 0;
    const FaceRect* mr = f2_xrect(c);
    if (c->efmt == 3) {
        uint64_t h = F2_FNV0;
        for (unsigned y = 0; y < c->H; y++)
            for (unsigned x = 0; x < c->W; x++)
                if (!face_in(&v->r, x, y) && !face_in(mr, x, y)) h = f2_fnv1(h, f2_tex(vr, px, py, c, x, y));
        return h == v->hsig;
    }
    int bad = 0;
    for (unsigned y = 0; y < c->H; y++)
        for (unsigned x = 0; x < c->W; x++) {
            if (face_in(&v->r, x, y) || face_in(mr, x, y)) continue;
            if (f2_tex(vr, px, py, c, x, y) != v->sig[y * c->W + x] && ++bad > 8) return 0;
        }
    return 1;
}
/* v3: хеш прямоугольника r кропа на странице (px, py) */
static uint64_t f2_rect_hash(const uint16_t* vr, const F2Char* c, const FaceRect* r, int px, int py) {
    uint64_t h = F2_FNV0;
    for (unsigned y = 0; y < r->rh; y++)
        for (unsigned x = 0; x < r->rw; x++) h = f2_fnv1(h, f2_tex(vr, px, py, c, r->rx + x, r->ry + y));
    return h;
}
static void f2_rect_get(const uint16_t* vr, const F2Char* c, const FaceRect* r, int px, int py, uint8_t* out) {
    for (unsigned y = 0; y < r->rh; y++)
        for (unsigned x = 0; x < r->rw; x++) out[y * r->rw + x] = f2_tex(vr, px, py, c, r->rx + x, r->ry + y);
}
/* v3: копия варианта k на странице (px, py) подписью совпала — исходные глаза (и рот) ещё не взяты? взять, если там
 * исходник (хеш совпал). 0 — копию не трогать: исходных глаз нет и здесь их нет (наши кадры, скопированные игрой) */
static int f2_capture(F2Char* c, int k, int px, int py) {
    if (c->efmt != 3 && c->tfmt != 3) return 1;
    F2Var* v = &c->v[k];
    if (c->efmt == 3 && !v->have) {
        if (f2_rect_hash(vram, c, &v->r, px, py) == v->heye) {
            f2_rect_get(vram, c, &v->r, px, py, v->orig); v->have = 1;
            fprintf(stderr, "face2: %s — вариант %u: исходные глаза взяты из VRAM (страница %d,%d)%c", c->id, v->var, px, py, 10);
        }
    }
    const int i = f2_tv(c, k);
    if (c->tfmt == 3 && i >= 0 && !c->mhave[i] && f2_rect_hash(vram, c, &c->mr, px, py) == c->hmouth[i]) {
        f2_rect_get(vram, c, &c->mr, px, py, c->morig[i]); c->mhave[i] = 1;
        fprintf(stderr, "talk2: %s — вариант %u: исходный рот взят из VRAM (страница %d,%d)%c", c->id, v->var, px, py, 10);
    }
    if (c->efmt == 3 && !v->have) {
        if (c->nocap++ < 8)
            fprintf(stderr, "face2: %s — копия варианта %u на странице %d,%d: глаза не исходные, а исходных ещё нет — не трогаю%c", c->id,
                    v->var, px, py, 10);
        return 0;
    }
    return 1;
}
/* копия j — текущая (помощники выше работают по c->cur, c->px, c->py) */
static void f2_use(F2Char* c, int j) { c->cur = c->cpk[j]; c->px = c->cpx[j]; c->py = c->cpy[j]; }
/* все копии лица в VRAM (DC_FACE_ALLCOPIES=0 — первая найденная, как было); текущая — первая */
static void f2_find(F2Char* c) {
    c->ncp = 0; c->cur = -1;
    for (int k = 0; k < c->nv && c->ncp < F2_MAXCP; k++)
        for (int py = 0; py <= 256 && c->ncp < F2_MAXCP; py += 256)
            for (int px = 0; px < 1024 && c->ncp < F2_MAXCP; px += 64) {
                int dup = 0;
                for (int j = 0; j < c->ncp; j++) if (c->cpx[j] == px && c->cpy[j] == py) dup = 1;
                if (dup || !f2_sig_at(vram, c, &c->v[k], px, py) || !f2_capture(c, k, px, py)) continue;
                c->cpk[c->ncp] = k; c->cpx[c->ncp] = px; c->cpy[c->ncp] = py; c->ncp++;
                if (!s_f2_allcp) { f2_use(c, 0); return; }
            }
    if (c->ncp) f2_use(c, 0);
}
/* лицо сейчас записано нами — вернуть исходные глаза и рот во все копии (перед перечитыванием файлов) */
static void f2_release(F2Char* c) {
    for (int j = 0; j < c->ncp; j++) {
        f2_use(c, j);
        if (!f2_sig_at(vram, c, &c->v[c->cur], c->px, c->py)) continue;
        if (c->efmt != 3 || c->v[c->cur].have) f2_blit(c, &c->v[c->cur].r, c->v[c->cur].orig);
        const uint8_t* m0 = f2_mouth0(c, c->cur);
        if (m0) f2_blit(c, &c->mr, m0);
    }
    c->ncp = 0; c->cur = -1;
}
static inline uint32_t f2_pagebit(int px, int py) { return 1u << (((py >> 8) & 1) * 16 + ((px >> 6) & 15)); }
/* набор копий сменился — строка в журнал */
static void f2_log_copies(const F2Char* c) {
    if (c->ncp) {
        char s[400]; size_t l = 0; s[0] = 0;
        for (int j = 0; j < c->ncp && l + 64 < sizeof(s); j++)
            l += (size_t)snprintf(s + l, sizeof(s) - l, "%sвариант %u, страница %d,%d", j ? "; " : "", c->v[c->cpk[j]].var, c->cpx[j], c->cpy[j]);
        if (c->ncp > 1) fprintf(stderr, "face2: %s в VRAM — копий %d: %s — глаза и рот пишутся во все (развёртка %llu)%c", c->id, c->ncp, s,
                                (unsigned long long)s_f2_vb, 10);
        else fprintf(stderr, "face2: %s в VRAM — %s (развёртка %llu)%c", c->id, s, (unsigned long long)s_f2_vb, 10);
    }
    else fprintf(stderr, "face2: %s пропал из VRAM (развёртка %llu)%c", c->id, (unsigned long long)s_f2_vb, 10);
}
/* 27.09 (2): копии лица на страницах pm (их изменила игра). Копии на прочих страницах остаются как есть (их никто не
 * трогал), на pm — сверка всего кропа и поиск новых (все варианты). 1 — на pm лежала копия (игра могла залить исходник
 * поверх наших кадров — переписать глаза и рот) или набор копий сменился */
static int f2_find_pages(F2Char* c, uint32_t pm) {
    int nk = 0, kk[F2_MAXCP], kx[F2_MAXCP], ky[F2_MAXCP], hit = 0;
    for (int j = 0; j < c->ncp; j++) {
        if (pm & f2_pagebit(c->cpx[j], c->cpy[j])) {
            hit = 1;
            if (!f2_sig_at(vram, c, &c->v[c->cpk[j]], c->cpx[j], c->cpy[j]) || !f2_capture(c, c->cpk[j], c->cpx[j], c->cpy[j])) continue;
        }
        kk[nk] = c->cpk[j]; kx[nk] = c->cpx[j]; ky[nk] = c->cpy[j]; nk++;
    }
    for (int k = 0; k < c->nv && nk < F2_MAXCP; k++)
        for (int p = 0; p < 32 && nk < F2_MAXCP; p++) {
            if (!(pm & (1u << p))) continue;
            const int px = (p & 15) * 64, py = (p >> 4) * 256;
            int dup = 0;
            for (int j = 0; j < nk; j++) if (kx[j] == px && ky[j] == py) dup = 1;
            if (dup || !f2_sig_at(vram, c, &c->v[k], px, py) || !f2_capture(c, k, px, py)) continue;
            kk[nk] = k; kx[nk] = px; ky[nk] = py; nk++;
        }
    for (int a = 1; a < nk; a++)                                  /* порядок как у f2_find: вариант, ряд, столбец */
        for (int b = a; b > 0; b--) {
            const int ka = kk[b - 1] * 32 + (ky[b - 1] >> 8) * 16 + (kx[b - 1] >> 6), kb = kk[b] * 32 + (ky[b] >> 8) * 16 + (kx[b] >> 6);
            if (ka <= kb) break;
            int t = kk[b]; kk[b] = kk[b - 1]; kk[b - 1] = t; t = kx[b]; kx[b] = kx[b - 1]; kx[b - 1] = t; t = ky[b]; ky[b] = ky[b - 1]; ky[b - 1] = t;
        }
    int same = nk == c->ncp;
    for (int j = 0; j < nk && same; j++) same = kk[j] == c->cpk[j] && kx[j] == c->cpx[j] && ky[j] == c->cpy[j];
    c->ncp = nk;
    for (int j = 0; j < nk; j++) { c->cpk[j] = kk[j]; c->cpx[j] = kx[j]; c->cpy[j] = ky[j]; }
    if (nk) f2_use(c, 0); else c->cur = -1;
    if (!same) f2_log_copies(c);
    return hit || !same;
}
/* DC_FACE_PAGE_CHECK=1 (тест): хеш каждой страницы в начале развёртки против конца прошлой (после наших записей) — изменение
 * без отметки игрой = дыра в отметках gpu.c */
static uint32_t f2_page_hash(int p) {
    const int px = (p & 15) * 64, py = (p >> 4) * 256;
    uint32_t h = 2166136261u;
    /* буферы кадра (x < 320, y < 496) меняются рисованием каждый кадр и не отмечаются (лиц там нет) — не считаем их и здесь */
    const int y0 = (px + 64 <= 320) ? (py < 496 ? 496 - py : 0) : 0;
    for (int y = y0; y < 256; y++) {
        const uint16_t* r = vram + (size_t)(py + y) * 1024 + px;
        for (int x = 0; x < 64; x++) h = (h ^ r[x]) * 16777619u;
    }
    return h;
}
static void f2_page_check(uint32_t touched) {
    if (!s_f2_phave) return;
    for (int p = 0; p < 32; p++) {
        if (f2_page_hash(p) == s_f2_phash[p]) continue;
        if (touched & (1u << p)) { s_f2_pmarked++; continue; }
        if (s_f2_punmarked++ < 20)
            fprintf(stderr, "face2: ПРОВЕРКА ОТМЕТОК — страница %d,%d изменилась без отметки (развёртка %llu)%c", (p & 15) * 64, (p >> 4) * 256,
                    (unsigned long long)s_f2_vb, 10);
    }
}
static void f2_page_snap(void) { for (int p = 0; p < 32; p++) s_f2_phash[p] = f2_page_hash(p); s_f2_phave = 1; }

/* ---- <id>_eyes.bin: 'DCE2' u16 1, u16 вариантов | id[24] | u16 u0 v0 W H | u16 nx ny nl enx eny enl nemo nlev |
 * f32 dx0 dxs dy0 dys edx0 edxs edy0 edys | f32 lev[4] | nemo × имя[24] | вариант: u16 номер rx ry rw rh 0, W*H подпись,
 * кадры: спокойно nx*ny*nl, затем для эмоции 1..nemo-1 и уровня 0..nlev-1 — enx*eny*enl; кадр rw*rh (индексы 8bpp). ---- */
static int f2_parse_eyes(F2Char* c, uint8_t* b, size_t n) {
    if (n < 104 || memcmp(b, "DCE2", 4) != 0 || f_u16(b + 4) != 1) return 0;
    const int nv = f_u16(b + 6);
    const unsigned u0 = f_u16(b + 32), v0 = f_u16(b + 34), W = f_u16(b + 36), H = f_u16(b + 38);
    const unsigned nx = f_u16(b + 40), ny = f_u16(b + 42), nl = f_u16(b + 44), enx = f_u16(b + 46), eny = f_u16(b + 48),
                   enl = f_u16(b + 50), nemo = f_u16(b + 52), nlev = f_u16(b + 54);
    const size_t NF = (size_t)nx * ny * nl + (size_t)(nemo ? nemo - 1 : 0) * nlev * enx * eny * enl;
    if (nv < 1 || nv > F2_MAXV || !W || !H || W > 256 || H > 256 || u0 + W > 256 || v0 + H > 256 || nx * ny * nl == 0 ||
        nl < 2 || nemo < 1 || nemo > F2_MAXE || nlev > 4 || (nemo > 1 && (enx * eny * enl == 0 || enl < 2 || !nlev)) || NF > 60000)
        return 0;
    size_t o = 104 + (size_t)nemo * 24;
    F2Var vv[F2_MAXV]; memset(vv, 0, sizeof(vv));
    for (int k = 0; k < nv; k++) {
        if (o + 12 > n) return 0;
        F2Var* v = &vv[k];
        v->var = f_u16(b + o); v->r.rx = f_u16(b + o + 2); v->r.ry = f_u16(b + o + 4); v->r.rw = f_u16(b + o + 6); v->r.rh = f_u16(b + o + 8);
        o += 12;
        if (!v->r.rw || !v->r.rh || v->r.rx + v->r.rw > W || v->r.ry + v->r.rh > H) return 0;
        if (o + (size_t)W * H + NF * v->r.rw * v->r.rh > n) return 0;
        v->sig = b + o; o += (size_t)W * H;
        v->fr = b + o; o += NF * v->r.rw * v->r.rh;
    }
    for (int k = 0; k < nv; k++) {                                   /* исходные глаза — одним куском */
        F2Var* v = &vv[k];
        v->orig = (uint8_t*)malloc((size_t)v->r.rw * v->r.rh);
        if (!v->orig) { for (int j = 0; j < k; j++) free(vv[j].orig); return 0; }
        for (unsigned y = 0; y < v->r.rh; y++) memcpy(v->orig + y * v->r.rw, v->sig + (v->r.ry + y) * W + v->r.rx, v->r.rw);
    }
    for (int k = 0; k < c->nv; k++) free(c->v[k].orig);
    memcpy(c->v, vv, sizeof(vv)); c->nv = nv;
    c->u0 = (uint16_t)u0; c->v0 = (uint16_t)v0; c->W = (uint16_t)W; c->H = (uint16_t)H;
    c->nx = (uint16_t)nx; c->ny = (uint16_t)ny; c->nl = (uint16_t)nl; c->enx = (uint16_t)enx; c->eny = (uint16_t)eny; c->enl = (uint16_t)enl;
    c->nemo = (uint16_t)nemo; c->nlev = (uint16_t)nlev;
    c->dx0 = f_f32(b + 56); c->dxs = f_f32(b + 60); c->dy0 = f_f32(b + 64); c->dys = f_f32(b + 68);
    c->edx0 = f_f32(b + 72); c->edxs = f_f32(b + 76); c->edy0 = f_f32(b + 80); c->edys = f_f32(b + 84);
    for (int i = 0; i < 4; i++) c->lev[i] = f_f32(b + 88 + 4 * i);
    if (!(c->dxs > 0.f)) c->dxs = 1.f;
    if (!(c->dys > 0.f)) c->dys = 1.f;
    if (!(c->edxs > 0.f)) c->edxs = 1.f;
    if (!(c->edys > 0.f)) c->edys = 1.f;
    for (unsigned e = 0; e < nemo; e++) { memcpy(c->emo[e], b + 104 + e * 24, 24); c->emo[e][23] = 0; }
    c->efmt = 2; c->xmr_on = 0;
    return 1;
}
/* ---- v3 <id>_eyes.bin: 'DCE3' u16 1, u16 вариантов | id[24] | u16 u0 v0 W H | u16 nx ny nl enx eny enl nemo nlev |
 * f32 dx0 dxs dy0 dys edx0 edxs edy0 edys | f32 lev[4] | u16 рот вне подписи rx ry rw rh (нули — нет) | 8 байт 0 |
 * nemo × имя[24] | вариант: u16 номер rx ry rw rh 0, u64 хеш выборки, хеш кропа вне глаз и рта, хеш исходных глаз,
 * u32 кадров, u32 байт на кадр (rw*rh), u32 длина стопки, стопка кадров (разница с исходными глазами, f2_fs_decode).
 * Кадры раскрываются в один буфер (*dec); исходные глаза — из VRAM (f2_capture). ---- */
static int f2_parse_eyes3(F2Char* c, const uint8_t* b, size_t n, uint8_t** dec) {
    if (n < 120 || memcmp(b, "DCE3", 4) != 0 || f_u16(b + 4) != 1) return 0;
    const int nv = f_u16(b + 6);
    const unsigned u0 = f_u16(b + 32), v0 = f_u16(b + 34), W = f_u16(b + 36), H = f_u16(b + 38);
    const unsigned nx = f_u16(b + 40), ny = f_u16(b + 42), nl = f_u16(b + 44), enx = f_u16(b + 46), eny = f_u16(b + 48),
                   enl = f_u16(b + 50), nemo = f_u16(b + 52), nlev = f_u16(b + 54);
    const FaceRect xmr = { f_u16(b + 104), f_u16(b + 106), f_u16(b + 108), f_u16(b + 110) };
    const size_t NF = (size_t)nx * ny * nl + (size_t)(nemo ? nemo - 1 : 0) * nlev * enx * eny * enl;
    if (nv < 1 || nv > F2_MAXV || !W || !H || W > 256 || H > 256 || u0 + W > 256 || v0 + H > 256 || nx * ny * nl == 0 ||
        nl < 2 || nemo < 1 || nemo > F2_MAXE || nlev > 4 || (nemo > 1 && (enx * eny * enl == 0 || enl < 2 || !nlev)) || NF > 60000 ||
        xmr.rx + xmr.rw > W || xmr.ry + xmr.rh > H || (!xmr.rw) != (!xmr.rh))
        return 0;
    size_t o = 120 + (size_t)nemo * 24, tot = 0;
    F2Var vv[F2_MAXV]; memset(vv, 0, sizeof(vv));
    size_t at[F2_MAXV], len[F2_MAXV];
    for (int k = 0; k < nv; k++) {                                   /* первый проход: размеры и места стопок */
        if (o + 48 > n) return 0;
        F2Var* v = &vv[k];
        v->var = f_u16(b + o); v->r.rx = f_u16(b + o + 2); v->r.ry = f_u16(b + o + 4); v->r.rw = f_u16(b + o + 6); v->r.rh = f_u16(b + o + 8);
        memcpy(&v->hsamp, b + o + 12, 8); memcpy(&v->hsig, b + o + 20, 8); memcpy(&v->heye, b + o + 28, 8);
        const uint32_t N = f_u32(b + o + 36), S = f_u32(b + o + 40), L = f_u32(b + o + 44);
        o += 48;
        if (!v->r.rw || !v->r.rh || v->r.rx + v->r.rw > W || v->r.ry + v->r.rh > H || N != NF || S != v->r.rw * v->r.rh || L > n - o) return 0;
        at[k] = o; len[k] = L; o += L;
        tot += NF * S;
    }
    if (o != n || tot > (256u << 20)) return 0;
    uint8_t* d = (uint8_t*)malloc(tot ? tot : 1);
    if (!d) return 0;
    size_t off = 0;
    for (int k = 0; k < nv; k++) {
        F2Var* v = &vv[k];
        const uint32_t S = v->r.rw * v->r.rh;
        if (!f2_fs_decode(b + at[k], len[k], (uint32_t)NF, S, d + off)) { free(d); return 0; }
        v->fr = d + off; off += NF * S;
        v->orig = (uint8_t*)malloc(S);
        if (!v->orig) { for (int j = 0; j <= k; j++) free(vv[j].orig); free(d); return 0; }
    }
    for (int k = 0; k < c->nv; k++) free(c->v[k].orig);
    memcpy(c->v, vv, sizeof(vv)); c->nv = nv;
    c->u0 = (uint16_t)u0; c->v0 = (uint16_t)v0; c->W = (uint16_t)W; c->H = (uint16_t)H;
    c->nx = (uint16_t)nx; c->ny = (uint16_t)ny; c->nl = (uint16_t)nl; c->enx = (uint16_t)enx; c->eny = (uint16_t)eny; c->enl = (uint16_t)enl;
    c->nemo = (uint16_t)nemo; c->nlev = (uint16_t)nlev;
    c->dx0 = f_f32(b + 56); c->dxs = f_f32(b + 60); c->dy0 = f_f32(b + 64); c->dys = f_f32(b + 68);
    c->edx0 = f_f32(b + 72); c->edxs = f_f32(b + 76); c->edy0 = f_f32(b + 80); c->edys = f_f32(b + 84);
    for (int i = 0; i < 4; i++) c->lev[i] = f_f32(b + 88 + 4 * i);
    if (!(c->dxs > 0.f)) c->dxs = 1.f;
    if (!(c->dys > 0.f)) c->dys = 1.f;
    if (!(c->edxs > 0.f)) c->edxs = 1.f;
    if (!(c->edys > 0.f)) c->edys = 1.f;
    for (unsigned e = 0; e < nemo; e++) { memcpy(c->emo[e], b + 120 + e * 24, 24); c->emo[e][23] = 0; }
    c->efmt = 3; c->xmr = xmr; c->xmr_on = xmr.rw != 0; c->nocap = 0;
    *dec = d;
    return 1;
}
/* ---- <id>_talk.bin: 'DCT2' u16 1, u16 вариантов с кадрами рта | id[24] | serial[16] | u16 rx ry rw rh (рот в кропе) |
 * u16 fps, u16 клипов | u32 состояний | u16 эмоций, u16 файлов XA | u32 0 | эмоции × имя[24] | файлы XA × {имя[16], u32 LBA,
 * u32 секторов} | вариант: u16 номер, u16 0, состояний × rw*rh | клип: u16 файл XA (номер в таблице), u16 канал,
 * u32 секторов, u32 шагов, f32 длительность сектора; секторов × u32 сектор от начала файла (по возрастанию);
 * шагов × {u16 состояние рта, u8 эмоция, u8 уровень 0..255}.
 * 27.09, необязательно после клипов: 'JAW1' u16 1, u16 вершин челюсти | u32 вершин модели | u16 первая, u16 число (блок вершин
 * головы) | f32 шарнир y, z (мм, кость головы), угол (° при «Челюсть вниз» = 1), опускание (мм) | вершины × {u16 номер в
 * модели, u16 вес ×1000} | блок головы × 8 байт (x, y, z, кость — как в RAM) | u32 состояний (= состояний рта) | × u8
 * раскрытие челюсти ×100. ---- */
static int f2_parse_talk(F2Char* c, uint8_t* b, size_t n, char* why, size_t whysz) {
    snprintf(why, whysz, "формат/размер");
    if (n < 72 || memcmp(b, "DCT2", 4) != 0 || f_u16(b + 4) != 1) return 0;
    char serial[17]; memcpy(serial, b + 32, 16); serial[16] = 0;
    if (strcmp(serial, F2_SERIAL) != 0) { snprintf(why, whysz, "другая версия игры (%s, а у нас %s)", serial, F2_SERIAL); return 0; }
    const int ntv = f_u16(b + 6);
    const FaceRect mr = { f_u16(b + 48), f_u16(b + 50), f_u16(b + 52), f_u16(b + 54) };
    const unsigned fps = f_u16(b + 56), nclips = f_u16(b + 58), nemo = f_u16(b + 64), nxa = f_u16(b + 66);
    const uint32_t nst = f_u32(b + 60);
    const size_t fsz = (size_t)mr.rw * mr.rh;
    if (ntv < 0 || ntv > F2_MAXV || !fsz || mr.rx + mr.rw > 256 || mr.ry + mr.rh > 256 || !fps || fps > 240 || !nst ||
        nst > 65535 || nemo > F2_MAXE || nxa > 64) return 0;
    size_t o = 72;
    char names[F2_MAXE][24];
    for (unsigned e = 0; e < nemo; e++) { if (o + 24 > n) return 0; memcpy(names[e], b + o, 24); names[e][23] = 0; o += 24; }
    int xmap[64];
    for (unsigned j = 0; j < nxa; j++) {
        if (o + 24 > n) return 0;
        char nm[17]; memcpy(nm, b + o, 16); nm[16] = 0;
        const uint32_t lba = f_u32(b + o + 16);
        xmap[j] = -1;
        for (size_t i = 0; i < sizeof(s_xa_files) / sizeof(s_xa_files[0]); i++)
            if (strcmp(s_xa_files[i].name, nm) == 0) {
                if ((uint32_t)s_xa_files[i].lba == lba) xmap[j] = (int)i;
                else fprintf(stderr, "talk2: %s — %s на другом месте диска (LBA %u, у нас %d) — реплики этого файла не подставляются%c",
                             c->id, nm, lba, s_xa_files[i].lba, 10);
            }
        o += 24;
    }
    uint16_t tvar[F2_MAXV]; const uint8_t* tfr[F2_MAXV];
    for (int i = 0; i < ntv; i++) {
        if (o + 4 + (size_t)nst * fsz > n) return 0;
        tvar[i] = f_u16(b + o); o += 4;
        tfr[i] = b + o; o += (size_t)nst * fsz;
    }
    F2Clip* cl = (F2Clip*)calloc(nclips ? nclips : 1, sizeof(F2Clip));
    if (!cl) return 0;
    for (unsigned k = 0; k < nclips; k++) {
        if (o + 16 > n) { free(cl); return 0; }
        F2Clip* q = &cl[k];
        const unsigned xi = f_u16(b + o);
        q->xa = xi < nxa ? xmap[xi] : -1; q->ch = f_u16(b + o + 2); q->nsec = f_u32(b + o + 4); q->nsteps = f_u32(b + o + 8); q->secdur = f_f32(b + o + 12);
        o += 16;
        if (q->nsec > 100000 || q->nsteps > 1000000 || !(q->secdur > 0.f) || o + (size_t)q->nsec * 4 + (size_t)q->nsteps * 4 > n) { free(cl); return 0; }
        q->rel = b + o; o += (size_t)q->nsec * 4;
        q->steps = b + o; o += (size_t)q->nsteps * 4;
        for (uint32_t i = 0; i < q->nsteps; i++)
            if (f_u16(q->steps + 4 * i) >= nst || q->steps[4 * i + 2] >= (nemo ? nemo : 1)) { free(cl); return 0; }
    }
    /* 27.09: челюсть — необязательная секция 'JAW1' после клипов */
    int jok = 0, jn = 0; uint16_t jfirst = 0, jcount = 0, jvi[F2_MAXJ]; uint32_t jnv = 0; float jw[F2_MAXJ], jf[4] = { 0.f, 0.f, 0.f, 0.f };
    const uint8_t *jsig = NULL, *jst = NULL;
    if (o + 32 <= n && memcmp(b + o, "JAW1", 4) == 0) {
        jn = f_u16(b + o + 6); jnv = f_u32(b + o + 8); jfirst = f_u16(b + o + 12); jcount = f_u16(b + o + 14);
        for (int k = 0; k < 4; k++) jf[k] = f_f32(b + o + 16 + 4 * k);
        size_t p = o + 32;
        int good = f_u16(b + o + 4) == 1 && jn > 0 && jn <= F2_MAXJ && jcount > 0 && jcount <= F2_MAXJB && jnv <= 16384 &&
                   (uint32_t)jfirst + jcount <= jnv && jf[2] > -60.f && jf[2] < 60.f && jf[3] > -100.f && jf[3] < 100.f &&
                   jf[0] > -2000.f && jf[0] < 2000.f && jf[1] > -2000.f && jf[1] < 2000.f &&
                   p + (size_t)jn * 4 + (size_t)jcount * 8 + 4 <= n;
        if (good) {
            for (int k = 0; k < jn; k++) {
                jvi[k] = f_u16(b + p + 4 * k); jw[k] = f_u16(b + p + 4 * k + 2) / 1000.f;
                if (jvi[k] < jfirst || jvi[k] >= jfirst + jcount || !(jw[k] > 0.f) || jw[k] > 1.001f) good = 0;
            }
            p += (size_t)jn * 4;
            jsig = b + p; p += (size_t)jcount * 8;
            const uint32_t jns = f_u32(b + p); p += 4;
            if (jns != nst || p + jns > n) good = 0;
            jst = b + p;
        }
        jok = good;
        if (!good) fprintf(stderr, "talk2: %s — секция челюсти не годна (вершин %d, блок %u+%u из %u), челюсть не двигается%c", c->id, jn,
                           (unsigned)jfirst, (unsigned)jcount, (unsigned)jnv, 10);
    }
    free(c->clips);
    c->clips = cl; c->nclips = (uint16_t)nclips; c->mr = mr; c->fps = (uint16_t)fps; c->nst = nst; c->tnemo = (int)nemo;
    c->ntv = ntv;
    for (int i = 0; i < F2_MAXV; i++) { c->tvar[i] = i < ntv ? tvar[i] : 0; c->tfr[i] = i < ntv ? tfr[i] : NULL; }
    memcpy(c->temo, names, sizeof(names));
    c->talk_ok = 1;
    c->jaw_ok = jok; c->jaddr = 0; c->japplied = 0; c->jq = 0; c->hold_j = 0; c->jseek_vb = 0; c->jss = 0;
    if (jok) {
        c->jn = jn; c->jnv = jnv; c->jfirst = jfirst; c->jcount = jcount; c->jsig = jsig; c->jst = jst;
        c->jpy = jf[0]; c->jpz = jf[1]; c->jang = jf[2]; c->jdrop = jf[3];
        for (int i = 0; i < F2_MAXJB; i++) c->jmap[i] = -1;
        for (int k = 0; k < jn; k++) { c->jvi[k] = jvi[k]; c->jw[k] = jw[k]; c->jmap[jvi[k] - jfirst] = (int16_t)k; }
    }
    c->tfmt = 2;
    return 1;
}
/* ---- v3 <id>_talk.bin: как 'DCT2' (шапка 72 байта, эмоции, файлы XA, клипы), но вариант рта: u16 номер, u16 0, u64 хеш
 * исходного рта, u32 состояний, u32 байт на кадр, u32 длина стопки, стопка (разница с исходным ртом); челюсть — секция
 * 'JAW3' после клипов: u16 1, u16 вершин челюсти | u32 вершин модели | u16 первая, u16 число | f32 шарнир y, z, угол,
 * опускание | u16 кость блока, u16 0, u32 0 | u64 хеш вершин вне челюсти, u64 хеш вершин челюсти (по 6 байт x y z, по
 * порядку блока) | вершины × {u16 номер, u16 вес ×1000} | u32 состояний | × u8 раскрытие ×100. Кадры рта — в *dec. ---- */
static int f2_parse_talk3(F2Char* c, uint8_t* b, size_t n, uint8_t** dec, char* why, size_t whysz) {
    snprintf(why, whysz, "формат/размер");
    if (n < 72 || memcmp(b, "DCT3", 4) != 0 || f_u16(b + 4) != 1) return 0;
    char serial[17]; memcpy(serial, b + 32, 16); serial[16] = 0;
    if (strcmp(serial, F2_SERIAL) != 0) { snprintf(why, whysz, "другая версия игры (%s, а у нас %s)", serial, F2_SERIAL); return 0; }
    const int ntv = f_u16(b + 6);
    const FaceRect mr = { f_u16(b + 48), f_u16(b + 50), f_u16(b + 52), f_u16(b + 54) };
    const unsigned fps = f_u16(b + 56), nclips = f_u16(b + 58), nemo = f_u16(b + 64), nxa = f_u16(b + 66);
    const uint32_t nst = f_u32(b + 60);
    const size_t fsz = (size_t)mr.rw * mr.rh;
    if (ntv < 0 || ntv > F2_MAXV || !fsz || mr.rx + mr.rw > 256 || mr.ry + mr.rh > 256 || !fps || fps > 240 || !nst ||
        nst > 65535 || nemo > F2_MAXE || nxa > 64) return 0;
    size_t o = 72;
    char names[F2_MAXE][24];
    for (unsigned e = 0; e < nemo; e++) { if (o + 24 > n) return 0; memcpy(names[e], b + o, 24); names[e][23] = 0; o += 24; }
    int xmap[64];
    for (unsigned j = 0; j < nxa; j++) {
        if (o + 24 > n) return 0;
        char nm[17]; memcpy(nm, b + o, 16); nm[16] = 0;
        const uint32_t lba = f_u32(b + o + 16);
        xmap[j] = -1;
        for (size_t i = 0; i < sizeof(s_xa_files) / sizeof(s_xa_files[0]); i++)
            if (strcmp(s_xa_files[i].name, nm) == 0) {
                if ((uint32_t)s_xa_files[i].lba == lba) xmap[j] = (int)i;
                else fprintf(stderr, "talk2: %s — %s на другом месте диска (LBA %u, у нас %d) — реплики этого файла не подставляются%c",
                             c->id, nm, lba, s_xa_files[i].lba, 10);
            }
        o += 24;
    }
    uint16_t tvar[F2_MAXV]; uint64_t hm[F2_MAXV]; size_t at[F2_MAXV], len[F2_MAXV];
    for (int i = 0; i < ntv; i++) {
        if (o + 24 > n) return 0;
        tvar[i] = f_u16(b + o); memcpy(&hm[i], b + o + 4, 8);
        const uint32_t N = f_u32(b + o + 12), S = f_u32(b + o + 16), L = f_u32(b + o + 20);
        o += 24;
        if (N != nst || S != fsz || L > n - o) return 0;
        at[i] = o; len[i] = L; o += L;
    }
    F2Clip* cl = (F2Clip*)calloc(nclips ? nclips : 1, sizeof(F2Clip));
    if (!cl) return 0;
    for (unsigned k = 0; k < nclips; k++) {
        if (o + 16 > n) { free(cl); return 0; }
        F2Clip* q = &cl[k];
        const unsigned xi = f_u16(b + o);
        q->xa = xi < nxa ? xmap[xi] : -1; q->ch = f_u16(b + o + 2); q->nsec = f_u32(b + o + 4); q->nsteps = f_u32(b + o + 8); q->secdur = f_f32(b + o + 12);
        o += 16;
        if (q->nsec > 100000 || q->nsteps > 1000000 || !(q->secdur > 0.f) || o + (size_t)q->nsec * 4 + (size_t)q->nsteps * 4 > n) { free(cl); return 0; }
        q->rel = b + o; o += (size_t)q->nsec * 4;
        q->steps = b + o; o += (size_t)q->nsteps * 4;
        for (uint32_t i = 0; i < q->nsteps; i++)
            if (f_u16(q->steps + 4 * i) >= nst || q->steps[4 * i + 2] >= (nemo ? nemo : 1)) { free(cl); return 0; }
    }
    int jok = 0, jn = 0; uint16_t jfirst = 0, jcount = 0, jbone = 0, jvi[F2_MAXJ]; uint32_t jnv = 0; float jw[F2_MAXJ], jf[4] = { 0.f, 0.f, 0.f, 0.f };
    uint64_t jhn = 0, jhj = 0; const uint8_t* jst = NULL;
    if (o + 56 <= n && memcmp(b + o, "JAW3", 4) == 0) {
        jn = f_u16(b + o + 6); jnv = f_u32(b + o + 8); jfirst = f_u16(b + o + 12); jcount = f_u16(b + o + 14);
        for (int k = 0; k < 4; k++) jf[k] = f_f32(b + o + 16 + 4 * k);
        jbone = f_u16(b + o + 32); memcpy(&jhn, b + o + 40, 8); memcpy(&jhj, b + o + 48, 8);
        size_t p = o + 56;
        int good = f_u16(b + o + 4) == 1 && jn > 0 && jn <= F2_MAXJ && jcount > 0 && jcount <= F2_MAXJB && jnv <= 16384 &&
                   (uint32_t)jfirst + jcount <= jnv && jf[2] > -60.f && jf[2] < 60.f && jf[3] > -100.f && jf[3] < 100.f &&
                   jf[0] > -2000.f && jf[0] < 2000.f && jf[1] > -2000.f && jf[1] < 2000.f &&
                   p + (size_t)jn * 4 + 4 <= n;
        if (good) {
            for (int k = 0; k < jn; k++) {
                jvi[k] = f_u16(b + p + 4 * k); jw[k] = f_u16(b + p + 4 * k + 2) / 1000.f;
                if (jvi[k] < jfirst || jvi[k] >= jfirst + jcount || !(jw[k] > 0.f) || jw[k] > 1.001f) good = 0;
                for (int j = 0; j < k && good; j++) if (jvi[j] == jvi[k]) good = 0;
            }
            p += (size_t)jn * 4;
            const uint32_t jns = f_u32(b + p); p += 4;
            if (jns != nst || p + jns > n) good = 0;
            jst = b + p;
        }
        jok = good;
        if (!good) fprintf(stderr, "talk2: %s — секция челюсти не годна (вершин %d, блок %u+%u из %u), челюсть не двигается%c", c->id, jn,
                           (unsigned)jfirst, (unsigned)jcount, (unsigned)jnv, 10);
    }
    /* кадры рта: раскрыть все варианты; исходный рот — из VRAM */
    uint8_t* d = (uint8_t*)malloc((size_t)(ntv ? ntv : 1) * nst * fsz);
    if (!d) { free(cl); return 0; }
    uint8_t* mo[F2_MAXV] = { 0 };
    for (int i = 0; i < ntv; i++) {
        mo[i] = (uint8_t*)malloc(fsz);
        if (!mo[i] || !f2_fs_decode(b + at[i], len[i], nst, (uint32_t)fsz, d + (size_t)i * nst * fsz)) {
            for (int j = 0; j <= i; j++) free(mo[j]);
            free(d); free(cl); snprintf(why, whysz, "кадры рта не раскрылись"); return 0;
        }
    }
    free(c->clips);
    c->clips = cl; c->nclips = (uint16_t)nclips; c->mr = mr; c->fps = (uint16_t)fps; c->nst = nst; c->tnemo = (int)nemo;
    c->ntv = ntv;
    for (int i = 0; i < F2_MAXV; i++) {
        free(c->morig[i]);
        c->tvar[i] = i < ntv ? tvar[i] : 0; c->tfr[i] = i < ntv ? d + (size_t)i * nst * fsz : NULL;
        c->morig[i] = i < ntv ? mo[i] : NULL; c->mhave[i] = 0; c->hmouth[i] = i < ntv ? hm[i] : 0;
    }
    memcpy(c->temo, names, sizeof(names));
    c->talk_ok = 1;
    c->jaw_ok = jok; c->jaddr = 0; c->japplied = 0; c->jq = 0; c->hold_j = 0; c->jseek_vb = 0; c->jss = 0;
    c->jhave = 0; c->jsig = NULL;
    if (jok) {
        c->jn = jn; c->jnv = jnv; c->jfirst = jfirst; c->jcount = jcount; c->jst = jst; c->jbone = jbone; c->jhn = jhn; c->jhj = jhj;
        c->jpy = jf[0]; c->jpz = jf[1]; c->jang = jf[2]; c->jdrop = jf[3];
        for (int i = 0; i < F2_MAXJB; i++) c->jmap[i] = -1;
        for (int k = 0; k < jn; k++) { c->jvi[k] = jvi[k]; c->jw[k] = jw[k]; c->jmap[jvi[k] - jfirst] = (int16_t)k; }
    }
    c->tfmt = 3;
    *dec = d;
    return 1;
}
/* ---- челюсть: блок вершин головы в RAM модели ---- */
static inline int16_t f2_r16(const uint8_t* p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline void f2_w16(uint8_t* p, int v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF); }
/* блок на месте a? вершины не из челюсти — ровно подпись (x, y, z); челюсть — подпись или (cur) то, что писали мы.
 * v3: вершины вне челюсти — хеш; челюсть — исходные (взяты из RAM) или наши; исходных ещё нет — хеш челюсти, и тогда они
 * берутся отсюда */
static int f2_jaw_at(F2Char* c, const uint8_t* ram, uint32_t a, int cur) {
    if (a + (uint32_t)c->jcount * 8u > 0x200000u) return 0;
    if (c->tfmt == 3) {
        uint64_t hn = F2_FNV0, hj = F2_FNV0;
        for (unsigned i = 0; i < c->jcount; i++) {
            const uint8_t* r = ram + a + i * 8u;
            const int k = c->jmap[i];
            if (k < 0) { hn = f2_fnv(hn, r, 6); continue; }
            if (!c->jhave) { hj = f2_fnv(hj, r, 6); continue; }
            if (memcmp(r, c->jorig[k], 6) == 0) continue;
            if (!cur || f2_r16(r) != c->jcur[k][0] || f2_r16(r + 2) != c->jcur[k][1] || f2_r16(r + 4) != c->jcur[k][2]) return 0;
        }
        if (hn != c->jhn) return 0;
        if (!c->jhave) {
            if (hj != c->jhj) return 0;
            for (int k = 0; k < c->jn; k++) memcpy(c->jorig[k], ram + a + (uint32_t)(c->jvi[k] - c->jfirst) * 8u, 6);
            c->jhave = 1;
            fprintf(stderr, "jaw: %s — исходные вершины челюсти взяты из RAM (0x%08X)%c", c->id, 0x80000000u | a, 10);
        }
        return 1;
    }
    for (unsigned i = 0; i < c->jcount; i++) {
        const uint8_t* r = ram + a + i * 8u;
        if (memcmp(r, c->jsig + i * 8u, 6) == 0) continue;
        const int k = c->jmap[i];
        if (k < 0 || !cur || f2_r16(r) != c->jcur[k][0] || f2_r16(r + 2) != c->jcur[k][1] || f2_r16(r + 4) != c->jcur[k][2]) return 0;
    }
    return 1;
}
static int f2_jaw_seek(F2Char* c, const uint8_t* ram) {
    if (c->tfmt == 3) {                                          /* v3: отсев по полю кости (у всех вершин блока одна), потом хеш */
        const uint32_t len = (uint32_t)c->jcount * 8u, last = len - 2u;
        const uint16_t bn = c->jbone;
        for (uint32_t a = 0; a + len <= 0x200000u; a += 4) {
            const uint8_t* r = ram + a;
            if (r[6] != (uint8_t)bn || f_u16(r + 6) != bn || f_u16(r + last) != bn) continue;
            if (c->jcount > 2 && (f_u16(r + 14) != bn || f_u16(r + 22) != bn)) continue;
            if (f2_jaw_at(c, ram, a, c->japplied)) { c->jaddr = a; return 1; }
        }
        return 0;
    }
    unsigned k0 = 0;
    while (k0 < c->jcount && c->jmap[k0] >= 0) k0++;              /* первая вершина не из челюсти — быстрый отсев */
    if (k0 >= c->jcount) return 0;
    const uint8_t* s0 = c->jsig + k0 * 8u;
    const uint32_t len = (uint32_t)c->jcount * 8u;
    for (uint32_t a = 0; a + len <= 0x200000u; a += 4) {
        const uint8_t* r = ram + a + k0 * 8u;
        if (r[0] != s0[0] || memcmp(r, s0, 6) != 0) continue;
        if (f2_jaw_at(c, ram, a, c->japplied)) { c->jaddr = a; return 1; }
    }
    return 0;
}
/* вершины челюсти при раскрытии open (0 — исходные): поворот вокруг оси X через (jpy, jpz) на open·угол·вес + опускание */
static void f2_jaw_write(F2Char* c, uint8_t* ram, float open) {
    for (int k = 0; k < c->jn; k++) {
        const unsigned i = (unsigned)(c->jvi[k] - c->jfirst);
        const uint8_t* s = c->tfmt == 3 ? c->jorig[k] : c->jsig + i * 8u;
        const float x = f2_r16(s), y = f2_r16(s + 2), z = f2_r16(s + 4), w = c->jw[k];
        int nx = (int)x, ny = (int)y, nz = (int)z;
        if (open > 0.f) {
            const float th = open * c->jang * 0.01745329252f * w, cs = cosf(th), sn = sinf(th), dy = y - c->jpy, dz = z - c->jpz;
            ny = (int)lroundf(c->jpy + dy * cs + dz * sn + open * c->jdrop * w);
            nz = (int)lroundf(c->jpz - dy * sn + dz * cs);
        }
        uint8_t* r = ram + c->jaddr + i * 8u;
        f2_w16(r, nx); f2_w16(r + 2, ny); f2_w16(r + 4, nz);
        c->jcur[k][0] = (int16_t)nx; c->jcur[k][1] = (int16_t)ny; c->jcur[k][2] = (int16_t)nz;
    }
    c->japplied = open > 0.f; c->jwrites++;
}
/* вернуть исходные вершины (перед перечитыванием файла, опция лиц выкл) */
static void f2_jaw_restore(F2Char* c) {
    uint8_t* ram = memory_get_ram_ptr();
    if (c->jaw_ok && c->japplied && c->jaddr && ram && f2_jaw_at(c, ram, c->jaddr, 1)) f2_jaw_write(c, ram, 0.f);
    c->japplied = 0; c->jq = 0;
}
/* каждая развёртка: состояние рта jst → раскрытие → вершины (пишем только при смене) */
static void f2_jaw_tick(F2Char* c, uint32_t jst) {
    uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return;
    float open = s_f2_jforce >= 0.f ? s_f2_jforce : (jst < c->nst ? c->jst[jst] / 100.f : 0.f);
    if (!s_f2_jaw) open = 0.f;
    const int q = (int)lroundf(open * 100.f);
    if (q == 0 && !c->japplied) return;                          /* рот закрыт и мы ничего не писали — модель не трогаем */
    if (!c->jaddr || !f2_jaw_at(c, ram, c->jaddr, c->japplied)) {
        if (c->jaddr && s_f2_jlog) fprintf(stderr, "jaw: %s — блок вершин головы в RAM сменился, ищу заново%c", c->id, 10);
        c->jaddr = 0; c->japplied = 0; c->jq = 0;
        if (q == 0 || (c->jseek_vb && s_f2_vb - c->jseek_vb < 60)) return;
        c->jseek_vb = s_f2_vb;
        if (!f2_jaw_seek(c, ram)) { if (s_f2_jlog) fprintf(stderr, "jaw: %s — блока вершин головы в RAM нет%c", c->id, 10); return; }
        fprintf(stderr, "jaw: %s — вершины головы в RAM 0x%08X (%u, челюсть %d вершин), шарнир y %.0f z %.0f, %.1f° при «Челюсть вниз» = 1%s%c",
                c->id, 0x80000000u | c->jaddr, (unsigned)c->jcount, c->jn, c->jpy, c->jpz, c->jang,
                s_f2_jforce >= 0.f ? " (DC_JAW_FORCE)" : "", 10);
    }
    if (q == c->jq && c->japplied == (q > 0)) return;
    f2_jaw_write(c, ram, q / 100.f);
    c->jq = q;
    if (s_f2_jlog) fprintf(stderr, "jaw: %s раскрытие %.2f (состояние рта %u, развёртка %llu)%c", c->id, q / 100.f, (unsigned)jst,
                           (unsigned long long)s_f2_vb, 10);
}
/* сейв (savestate.c, до и после memory_savestate): в файл — исходные вершины челюсти, потом наши обратно */
void dc_jaw_savestate(int after) {
    uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return;
    for (int i = 0; i < s_f2_n; i++) {
        F2Char* c = &s_f2[i];
        if (!after) c->jss = c->jaw_ok && c->japplied && c->jaddr && f2_jaw_at(c, ram, c->jaddr, 1);
        if (!c->jss) continue;
        for (int k = 0; k < c->jn; k++) {
            const unsigned j = (unsigned)(c->jvi[k] - c->jfirst);
            uint8_t* r = ram + c->jaddr + j * 8u;
            if (!after) memcpy(r, c->tfmt == 3 ? c->jorig[k] : c->jsig + j * 8u, 6);
            else { f2_w16(r, c->jcur[k][0]); f2_w16(r + 2, c->jcur[k][1]); f2_w16(r + 4, c->jcur[k][2]); }
        }
        if (after) c->jss = 0;
    }
}
/* эмоции talk → сетки глаз по имени (после перечитывания любого из двух файлов) */
static void f2_map_emo(F2Char* c) {
    for (int e = 0; e < c->tnemo; e++) {
        c->emap[e] = 0;
        for (unsigned j = 1; j < c->nemo; j++) if (strcmp(c->temo[e], c->emo[j]) == 0) { c->emap[e] = (int)j; break; }
    }
}
static F2Char* f2_char(const char* id) {
    for (int i = 0; i < s_f2_n; i++) if (strcmp(s_f2[i].id, id) == 0) return &s_f2[i];
    if (s_f2_n >= F2_MAXCH) return NULL;
    F2Char* c = &s_f2[s_f2_n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id);
    c->cur = -1; c->last_e = -2; c->last_m = -2; c->force = 1; c->hold_e = -1; c->hold_m = 0; c->et_cls = -1; c->et_t0 = -1.0;
    c->pend = 0xFFFFFFFFu;
    c->rng = 0x9E3779B9u; for (const char* p = id; *p; p++) c->rng = (c->rng ^ (uint8_t)*p) * 16777619u;
    if (!c->rng) c->rng = 1;
    c->lk_next = 0.8 + 0.8 * f2_rand(c); c->bl_t0 = -10.0; c->bl_next = 1.5 + 3.0 * f2_rand(c);
    return c;
}
/* v3: рот файла речи должен быть тем, что файл глаз исключил из хеша кропа (иначе наши кадры рта ломали бы подпись) */
static void f2_check_xmr(F2Char* c) {
    if (c->efmt != 3 || !c->talk_ok) return;
    if (c->xmr_on && c->xmr.rx == c->mr.rx && c->xmr.ry == c->mr.ry && c->xmr.rw == c->mr.rw && c->xmr.rh == c->mr.rh) return;
    c->talk_ok = 0;
    fprintf(stderr, "talk2: %s — рот файла речи (%u,%u %ux%u) не тот, что исключён из подписи файла глаз — рот не говорит%c", c->id,
            c->mr.rx, c->mr.ry, c->mr.rw, c->mr.rh, 10);
}
static void f2_load_eyes(F2Char* c, const char* path) {
    char used[512]; long long mt = 0, sz = 0; size_t got = 0;
    uint8_t* b = f2_read(path, used, sizeof(used), &mt, &sz, &got);
    if (!b) { face_stat(path, &c->emt, &c->esz); return; }
    f2_release(c);
    const int v3 = got >= 4 && memcmp(b, "DCE3", 4) == 0;      /* 04.10: формат v3 — только наше (хеши и разница) */
    uint8_t* keep = b; int ok;
    if (v3 && !s_f2_v3) ok = 0;
    else if (v3) { uint8_t* dec = NULL; ok = f2_parse_eyes3(c, b, got, &dec); if (ok) { free(b); keep = dec; } }
    else ok = f2_parse_eyes(c, b, got);
    if (!ok) {
        free(b);
        fprintf(stderr, "face2: %s — файл не годен (%s), глаза %s не оживают%c", used,
                (v3 && !s_f2_v3) ? "формат v3, а DC_FACE_V3=0" : "формат/размер", c->id, 10);
        c->emt = mt; c->esz = sz; snprintf(c->ep, sizeof(c->ep), "%s", used);
        return;
    }
    free(c->eb); c->eb = keep;
    f2_check_xmr(c);
    snprintf(c->ep, sizeof(c->ep), "%s", used); c->emt = mt; c->esz = sz;
    f2_map_emo(c);
    c->cur = -1; c->ncp = 0; c->pend = 0xFFFFFFFFu; c->force = 1; c->last_e = -2; c->hold_e = -1;
    f2_samples(c);
    const F2Var* v0 = &c->v[0];
    fprintf(stderr, "face2: %s — %s: вариантов %d, кадров глаз %u (эмоций %u), глаза %ux%u у (%u,%u) кропа %ux%u%s%c", used, c->id, c->nv,
            (unsigned)c->nx * c->ny * c->nl + (unsigned)(c->nemo - 1) * c->nlev * c->enx * c->eny * c->enl, c->nemo, v0->r.rw, v0->r.rh,
            v0->r.rx, v0->r.ry, c->W, c->H, c->efmt == 3 ? "; формат v3 (хеши, исходник — из VRAM)" : "", 10);
}
static void f2_load_talk(F2Char* c, const char* path) {
    char used[512], why[160]; long long mt = 0, sz = 0; size_t got = 0;
    f2_jaw_restore(c);                                  /* вершины челюсти — исходные, пока файл перечитывается */
    uint8_t* b = f2_read(path, used, sizeof(used), &mt, &sz, &got);
    if (!b) { face_stat(path, &c->tmt, &c->tsz); return; }
    f2_release(c);
    snprintf(c->tp, sizeof(c->tp), "%s", used); c->tmt = mt; c->tsz = sz;
    const int v3 = got >= 4 && memcmp(b, "DCT3", 4) == 0;      /* 04.10: формат v3 — только наше */
    uint8_t* dec = NULL; int ok;
    if (v3 && !s_f2_v3) { snprintf(why, sizeof(why), "формат v3, а DC_FACE_V3=0"); ok = 0; }
    else ok = v3 ? f2_parse_talk3(c, b, got, &dec, why, sizeof(why)) : f2_parse_talk(c, b, got, why, sizeof(why));
    if (!ok) {
        free(b);
        fprintf(stderr, "talk2: %s — не годен (%s), рот %s не говорит%c", used, why, c->id, 10);
        return;
    }
    free(c->tb); c->tb = b; free(c->tdec); c->tdec = dec;
    f2_check_xmr(c);
    f2_map_emo(c);
    c->cur = -1; c->ncp = 0; c->pend = 0xFFFFFFFFu; c->force = 1; c->last_m = -2; c->hold_e = -1;
    f2_samples(c);
    fprintf(stderr, "talk2: %s — %s: клипов %u, состояний рта %u (%ux%u у (%u,%u) кропа), вариантов %d, эмоций %d, %u шагов/с%s%c", used, c->id,
            c->nclips, c->nst, c->mr.rw, c->mr.rh, c->mr.rx, c->mr.ry, c->ntv, c->tnemo, c->fps,
            c->tfmt == 3 ? "; формат v3 (хеши, исходный рот и челюсть — из VRAM и RAM)" : "", 10);
    if (c->jaw_ok) fprintf(stderr, "talk2: %s — челюсть: %d вершин (блок головы %u+%u), шарнир y %.0f z %.0f, %.1f° при «Челюсть вниз» = 1, "
                           "опускание %.1f мм%s%c", c->id, c->jn, (unsigned)c->jfirst, (unsigned)c->jcount, c->jpy, c->jpz, c->jang, c->jdrop,
                           s_f2_jaw ? "" : " — DC_JAW=0, не двигается", 10);
}
/* папка лиц: найти (один раз) и перечитать новые/изменённые файлы */
static void f2_scan(void) {
    if (!s_f2_dir[0]) {
        const char* cand[] = { getenv("DC_FACE_DIR"), "assets/faces", "data/faces", "../../assets/faces", "../../../assets/faces" };
        for (int i = 0; i < 5; i++) if (f2_isdir(cand[i])) { snprintf(s_f2_dir, sizeof(s_f2_dir), "%s", cand[i]); break; }
        if (!s_f2_dir[0]) return;
    }
    char pat[600];
    snprintf(pat, sizeof(pat), "%s/*_eyes.bin", s_f2_dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        char id[64]; const size_t ln = strlen(fd.name);
        if (ln <= 9 || ln - 9 >= sizeof(id)) continue;
        memcpy(id, fd.name, ln - 9); id[ln - 9] = 0;
        if (!s_f2_npc && strcmp(id, "regina") != 0) continue;
        F2Char* c = f2_char(id);
        if (!c) continue;
        char ep[600], tp[600]; long long mt, sz;
        snprintf(ep, sizeof(ep), "%s/%s", s_f2_dir, fd.name);
        snprintf(tp, sizeof(tp), "%s/%s_talk.bin", s_f2_dir, id);
        if (face_stat(ep, &mt, &sz) && (mt != c->emt || sz != c->esz)) f2_load_eyes(c, ep);
        if (s_f2_talk && c->nv && face_stat(tp, &mt, &sz) && (mt != c->tmt || sz != c->tsz)) f2_load_talk(c, tp);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
}
/* вкл: DC_FACE_ANIM и DC_FACE_V2 не выключены и есть папка лиц хотя бы с одним файлом глаз (проверка раз в 10 с) */
static uint64_t s_f2_probe = 0;
static int f2_enabled(void) {
    if (s_f2_on == 0) return 0;
    if (s_f2_on == 1) return 1;
    const char* a = getenv("DC_FACE_ANIM"); const char* v = getenv("DC_FACE_V2");
    if ((a && *a == '0') || (v && *v == '0')) { s_f2_on = 0; return 0; }
    if (s_f2_probe++ % 600 != 0) return 0;
    const char* np = getenv("DC_FACE_NPC"); s_f2_npc = !(np && *np == '0');
    const char* em = getenv("DC_FACE_EMO"); s_f2_emo = !(em && *em == '0');
    const char* tk = getenv("DC_TALK"); s_f2_talk = !(tk && *tk == '0');
    const char* lg = getenv("DC_FACE_LOG"); s_f2_log = (lg && *lg >= '1' && *lg <= '9') ? *lg - '0' : 0;
    const char* tl = getenv("DC_TALK_LOG"); s_f2_tlog = (tl && *tl == '1');
    const char* am = getenv("DC_FACE_LOOK_AMP"); if (am && *am) { float x = (float)atof(am); if (x >= 0.f && x <= 3.f) s_face_amp = x; }
    const char* ld = getenv("DC_TALK_LEAD"); if (ld && *ld) { float x = (float)atof(ld); if (x >= -0.5f && x <= 0.5f) s_talk_lead = x; }
    const char* fp = getenv("DC_FACE_FPS"); if (fp && *fp) { const int x = atoi(fp); s_f2_fps = x <= 0 ? 0 : x > 60 ? 60 : x; }
    const char* bf = getenv("DC_FACE_BLINK_FAST"); s_f2_bfast = !(bf && *bf == '0');
    const char* bc = getenv("DC_FACE_BLINK_CALM"); s_f2_bcalm = !(bc && *bc == '0');
    const char* shp = getenv("DC_FACE_SHARP"); s_f2_sharp = !(shp && *shp == '0');
    const char* tw = getenv("DC_FACE_TWEEN"); if (tw && *tw) { const int x = atoi(tw); s_f2_tween = x < 0 ? 0 : x > 4 ? 4 : x; }
    const char* dl = getenv("DC_FACE_DELAY"); if (dl && *dl) { const double x = atof(dl); if (x >= 0.0 && x <= 2.0) s_f2_delay = x; }
    const char* jw = getenv("DC_JAW"); s_f2_jaw = !(jw && *jw == '0');
    const char* jl = getenv("DC_JAW_LEAD"); if (jl && *jl) { const double x = atof(jl); if (x >= -0.5 && x <= 0.5) s_f2_jlead = x; }
    const char* jfo = getenv("DC_JAW_FORCE"); if (jfo && *jfo) { const float x = (float)atof(jfo); if (x >= 0.f && x <= 1.3f) s_f2_jforce = x; }
    const char* jlh = getenv("DC_JAW_LEAD_HIST"); s_f2_jhist = !(jlh && *jlh == '0');
    s_f2_dline = s_f2_delay;                         /* 01.10: линия задержки не короче опережения челюсти */
    if (s_f2_jaw && s_f2_jhist && s_f2_jlead > s_f2_dline) s_f2_dline = s_f2_jlead;
    const char* jg = getenv("DC_JAW_LOG"); s_f2_jlog = (jg && *jg == '1');
    const char* acp = getenv("DC_FACE_ALLCOPIES"); s_f2_allcp = !(acp && *acp == '0');
    const char* pev = getenv("DC_FACE_PAGE_EVENTS"); s_f2_ev = !(pev && *pev == '0');
    const char* pck = getenv("DC_FACE_PAGE_CHECK"); s_f2_pchk = (pck && *pck == '1');
    const char* dwt = getenv("DC_FACE_DIR_WATCH"); if (dwt && *dwt == '0') s_f2_dwatch = 0;
    const char* dd = getenv("DC_FACE_DEAD"); s_f2_dead = !(dd && *dd == '0');
    const char* ddl = getenv("DC_FACE_DEAD_LIE"); if (ddl && *ddl) { const int x = atoi(ddl); if (x >= 0 && x <= 2000) s_f2_dead_lie = x; }
    const char* ddw = getenv("DC_FACE_DEAD_LOW"); if (ddw && *ddw) { const int x = atoi(ddw); if (x >= 0 && x <= 2000) s_f2_dead_low = x; }
    const char* onh = getenv("DC_FACE_ONSET_HOLD"); if (onh && *onh) { const int x = atoi(onh); s_f2_onset = x < 0 ? 0 : x > 30 ? 30 : x; }
    const char* vth = getenv("DC_FACE_VOICE_THR"); if (vth && *vth) { const int x = atoi(vth); s_f2_vthr = x < 0 ? 0 : x > 20000 ? 20000 : x; }
    const char* fv3 = getenv("DC_FACE_V3"); s_f2_v3 = !(fv3 && *fv3 == '0');
    f2_scan();
    int any = 0;
    for (int i = 0; i < s_f2_n; i++) if (s_f2[i].nv) any = 1;
    if (!any) {
        if (s_f2_probe == 1) fprintf(stderr, "face2: папки assets/faces с лицами нет — работает прежнее лицо Регины (face_anim.bin)%c", 10);
        return 0;
    }
    /* прежнее лицо Регины (если уже писалось) — вернуть исходное, дальше всё ведёт v2 */
    if (s_face_cur >= 0 && s_face.nc > 0 && face_match(vram) == s_face_cur) {
        face_blit(&s_face.c[s_face_cur].r, s_face.c[s_face_cur].orig);
        const FaceRect* tr = talk_rect_for(s_face_cur);
        if (tr) face_blit(tr, talk_src(0));
        s_face_cur = -1;
    }
    s_f2_on = 1;
    char fps_s[48];
    if (s_f2_fps) snprintf(fps_s, sizeof(fps_s), "%d/с (DC_FACE_FPS)", s_f2_fps); else snprintf(fps_s, sizeof(fps_s), "каждую развёртку");
    char sh_s[160];
    if (s_f2_sharp) snprintf(sh_s, sizeof(sh_s), "переходы через %d промежуточн. кадр(а) по 1/30 с", s_f2_tween);
    else snprintf(sh_s, sizeof(sh_s), "плавно (DC_FACE_SHARP=0)");
    fprintf(stderr, "face2: лица v2 — папка %s, персонажей %d%s%s%s; кадры лица %s, %s, моргание %s; рот и эмоции на %.3f с позже "
            "выдачи звука (DC_FACE_DELAY %.2f, челюсть — на столько раньше рта, DC_JAW_LEAD); %s%s%c", s_f2_dir, s_f2_n,
            s_f2_npc ? "" : " (DC_FACE_NPC=0 — только Регина)", s_f2_emo ? "" : ", DC_FACE_EMO=0 — без эмоций",
            s_f2_talk ? "" : ", DC_TALK=0 — рты молчат", fps_s, sh_s,
            !s_f2_bfast ? "прежнее (0.26 с, DC_FACE_BLINK_FAST=0)" : s_f2_sharp ? "ступенями" : "плавное быстрое (0.16 с)", s_f2_dline, s_f2_delay,
            !s_f2_allcp ? "только первая копия лица в VRAM (DC_FACE_ALLCOPIES=0)" :
            s_f2_ev ? "глаза и рот — во все копии лица в VRAM, поиск копий только на страницах, изменённых игрой" :
                      "глаза и рот — во все копии лица в VRAM, поиск раз в 30 развёрток (DC_FACE_PAGE_EVENTS=0)",
            s_f2_pchk ? "; ПРОВЕРКА ОТМЕТОК СТРАНИЦ (DC_FACE_PAGE_CHECK=1)" : "", 10);
    return 1;
}
/* cdrom.c: аудиосектор XA ушёл в очередь CD-звука SPU */
static void f2_xa_pushed(int lba, int channel, int frames) {
    if (frames <= 0) return;
    int xa = -1, rel = lba;
    for (size_t i = 0; i < sizeof(s_xa_files) / sizeof(s_xa_files[0]); i++)
        if (lba >= s_xa_files[i].lba && lba < s_xa_files[i].lba + s_xa_files[i].n) { xa = (int)i; rel = lba - s_xa_files[i].lba; break; }
    F2Seg* sg = &s_f2seg[s_f2seg_n & 63];
    sg->frames = (uint32_t)frames; sg->start = spu_cd_audio_pushed() - (uint64_t)frames; sg->xa = xa; sg->ch = channel; sg->rel = rel;
    s_f2seg_n++;
    const int cont = xa == s_f2log_xa && channel == s_f2log_ch && rel > s_f2log_rel && rel - s_f2log_rel <= 64;
    s_f2log_xa = xa; s_f2log_ch = channel; s_f2log_rel = rel;
    if (cont) {
        if (s_f2_tlog && rel % 8 == 0) { extern uint64_t s_frame_count;
            fprintf(stderr, "talk2: XA %s канал %d, сектор %d, кадр %llu%c", xa >= 0 ? s_xa_files[xa].name : "?", channel, rel, (unsigned long long)s_frame_count, 10); }
        return;
    }
    /* новый поток: кто в нём говорит (у кого рот двигается в ближайшие 8 с) */
    char who[256] = ""; size_t wl = 0;
    for (int i = 0; i < s_f2_n && xa >= 0; i++) {
        const F2Char* c = &s_f2[i];
        if (!c->talk_ok) continue;
        for (unsigned k = 0; k < c->nclips; k++) {
            const F2Clip* q = &c->clips[k];
            if (q->xa != xa || q->ch != (uint16_t)channel) continue;
            size_t lo = 0, hi = q->nsec;
            while (lo < hi) { const size_t mid = (lo + hi) / 2; if (f_u32(q->rel + 4 * mid) < (uint32_t)rel) lo = mid + 1; else hi = mid; }
            const uint32_t st = (uint32_t)(lo * q->secdur * c->fps);
            int talky = 0, emo = 0;
            for (uint32_t j = st; j < q->nsteps && j < st + (uint32_t)(c->fps * 8); j++) {
                if (f_u16(q->steps + 4 * j)) talky = 1;
                if (q->steps[4 * j + 2]) emo = 1;
            }
            if ((talky || emo) && wl + 40 < sizeof(who))
                wl += (size_t)snprintf(who + wl, sizeof(who) - wl, "%s%s%s", wl ? ", " : "", c->id, talky ? "" : " (только эмоция)");
            break;
        }
    }
    extern uint64_t s_frame_count;
    fprintf(stderr, "talk2: XA %s канал %d, сектор %d (LBA %d) — %s%s, кадр %llu%c", xa >= 0 ? s_xa_files[xa].name : "?", channel, rel, lba,
            who[0] ? "говорят/играют лицом: " : "лица в покое (реплик в сценарии нет)", who, (unsigned long long)s_frame_count, 10);
}
/* что звучит сейчас: отрезок кольца и доля внутри него; NULL — CD-звук молчит (очередь пуста или стоит: игра ставит XA
 * на паузу посреди канала, очередь не пустеет — без этой проверки рот застывал открытым) */
static const F2Seg* f2_now(double* frac) {
    const uint64_t played = spu_cd_audio_played();
    if (played < s_f2_played) {                  /* счёт CD-звука пошёл назад (сейв, DC_TALK_RESET_OLD=1) — отрезки не годны */
        memset(s_f2seg, 0, sizeof(s_f2seg)); s_f2seg_n = 0;
        if (s_f2_tlog) fprintf(stderr, "talk2: счёт CD-звука сброшен (развёртка %llu) — рты в покое%c", (unsigned long long)s_f2_vb, 10);
    }
    if (played != s_f2_played) { s_f2_played = played; s_f2_played_vb = s_f2_vb; }
    const int stalled = s_f2_vb - s_f2_played_vb > 6;
    if (stalled != s_f2_stalled) {
        s_f2_stalled = stalled;
        if (s_f2_tlog) fprintf(stderr, "talk2: CD-звук %s (развёртка %llu)%c", stalled ? "стоит — рты в покое" : "пошёл", (unsigned long long)s_f2_vb, 10);
    }
    if (stalled) return NULL;
    for (unsigned n = 0; n < 64 && n < s_f2seg_n; n++) {
        const F2Seg* sg = &s_f2seg[(s_f2seg_n - 1 - n) & 63];
        if (played >= sg->start + sg->frames) return NULL;
        if (played < sg->start) continue;
        *frac = (double)(played - sg->start) / sg->frames;
        return sg;
    }
    return NULL;
}
/* линия задержки DC_FACE_DELAY: запомнить, что звучит в этой развёртке, и отдать то, что звучало D с назад (NULL — молчало
 * или записи нет: загрузка, опция лиц выключалась) */
static int s_f2_dl_in = -1, s_f2_dl_out = -1;
static const F2Seg* f2_delayed(const F2Seg* sg, double* frac) {
    F2Hist* h = &s_f2hist[s_f2_vb & 127];
    h->vb = s_f2_vb; h->has = sg != NULL;
    if (sg) { h->sg = *sg; h->frac = *frac; }
    const uint64_t d = (uint64_t)(s_f2_dline * 59.94 + 0.5);
    const F2Seg* out = sg;
    if (d) {
        const F2Hist* o = &s_f2hist[(s_f2_vb - d) & 127];
        out = (s_f2_vb > d && o->vb == s_f2_vb - d && o->has) ? &o->sg : NULL;
        if (out) *frac = o->frac;
    }
    if (s_f2_tlog) {                             /* DC_TALK_LOG=1: когда звук реплики вышел из SPU и когда он дошёл до лица */
        if ((sg != NULL) != s_f2_dl_in) {
            s_f2_dl_in = sg != NULL;
            fprintf(stderr, "talk2: звук реплики из SPU %s (развёртка %llu)%c", s_f2_dl_in ? "идёт" : "кончился", (unsigned long long)s_f2_vb, 10);
        }
        if ((out != NULL) != s_f2_dl_out) {
            s_f2_dl_out = out != NULL;
            fprintf(stderr, "talk2: на лице звук реплики %s (развёртка %llu, задержка %llu развёрток)%c", s_f2_dl_out ? "идёт" : "кончился",
                    (unsigned long long)s_f2_vb, (unsigned long long)d, 10);
        }
    }
    return out;
}
/* 27.09 вечер: что звучало delay_s назад — из той же линии (запись этой развёртки f2_delayed уже сделал); NULL — молчало */
static const F2Seg* f2_hist_at(double delay_s, double* frac) {
    const uint64_t d = (uint64_t)(delay_s * 59.94 + 0.5);
    const F2Hist* o = &s_f2hist[(s_f2_vb - d) & 127];
    if (s_f2_vb > d && o->vb == s_f2_vb - d && o->has) { *frac = o->frac; return &o->sg; }
    return NULL;
}
/* шаг сценария лица c в звучащем месте (+ lead с): состояние рта, эмоция (номер сетки глаз), уровень */
static void f2_talk_now(const F2Char* c, const F2Seg* sg, double frac, double lead, uint32_t* st, int* emo, float* lev) {
    *st = 0; *emo = 0; *lev = 0.f;
    if (!c->talk_ok || !sg || sg->xa < 0) return;
    for (unsigned k = 0; k < c->nclips; k++) {
        const F2Clip* q = &c->clips[k];
        if (q->xa != sg->xa || q->ch != (uint16_t)sg->ch) continue;
        size_t lo = 0, hi = q->nsec;
        while (lo < hi) { const size_t mid = (lo + hi) / 2; if (f_u32(q->rel + 4 * mid) < (uint32_t)sg->rel) lo = mid + 1; else hi = mid; }
        if (lo >= q->nsec || f_u32(q->rel + 4 * lo) != (uint32_t)sg->rel) return;
        const double t = (lo + frac) * q->secdur + s_talk_lead + lead;
        long s = (long)(t * c->fps + 0.5);
        if (s < 0) s = 0;
        if (s >= (long)q->nsteps) s = (long)q->nsteps - 1;
        if (s < 0) return;
        const uint8_t* p = q->steps + 4 * s;
        *st = f_u16(p);
        const int e = p[2];
        *emo = (e < c->tnemo) ? c->emap[e] : 0;
        *lev = p[3] / 255.f;
        return;
    }
}
/* моргание и взгляд лица c в момент t (как у Регины, своё случайное) */
static void f2_look_now(const F2Char* c, double t, float* x, float* y) {
    double f;
    if (s_f2_sharp) {                            /* резко: DC_FACE_TWEEN промежуточных кадров по 1/30 с — смесь прежнего и нового */
        const int k = (int)floor((t - c->lk_t0) * 30.0);
        f = k >= s_f2_tween ? 1.0 : k < 0 ? 0.0 : (double)(k + 1) / (s_f2_tween + 1);
    } else { f = (t - c->lk_t0) / 0.07; f = f < 0 ? 0 : f > 1 ? 1 : f; f = f * f * (3 - 2 * f); }
    *x = c->lk_fx + (c->lk_tx - c->lk_fx) * (float)f; *y = c->lk_fy + (c->lk_ty - c->lk_fy) * (float)f;
}
static void f2_eyes_step(F2Char* c, double t, float* dx, float* dy, float* lid) {
    /* быстрое моргание при кадрах лица начинается с кадра «закрыто»: первый кадр — на 0.04 с от начала */
    const double pre = (s_f2_bfast && !s_f2_sharp && s_f2_fps > 0) ? 0.04 : 0.0;
    /* 21.10.2026 (игрок: «кажется, они часто моргают» -> «да, поправь моргание»): часы лиц от частоты кадров не зависят
     * (замер 30/60/120 FPS - одинаково), но выходило ~20 морганий в минуту: раз в 2.2-5.5 с, 15 % двойных и 50 % на каждом
     * дальнем переводе взгляда (а взгляд меняется раз в 0.7-2.2 с). Теперь раз в 3-7 с, двойных 8 %, на взгляд 20 % -
     * ~13-14 в минуту, раз в 4-5 с. DC_FACE_BLINK_CALM=0 - как было */
    const double bl_min = s_f2_bcalm ? 3.0 : 2.2, bl_rng = s_f2_bcalm ? 4.0 : 3.3;
    const float p_double = s_f2_bcalm ? 0.08f : 0.15f, p_glance = s_f2_bcalm ? 0.2f : 0.5f;
    if (t >= c->bl_next) {
        c->bl_t0 = t - pre; c->blinks++;
        if (!c->bl_double && f2_rand(c) < p_double) { c->bl_double = 1; c->bl_next = t + 0.34; }
        else { c->bl_double = 0; c->bl_next = t + bl_min + bl_rng * f2_rand(c); }
    }
    if (t >= c->lk_next) {
        float cx, cy; f2_look_now(c, t, &cx, &cy);
        c->lk_fx = cx; c->lk_fy = cy;
        if (f2_rand(c) < 0.35f) { c->lk_tx = 0.f; c->lk_ty = 0.f; }
        else { c->lk_tx = f2_rand(c) * 2.f - 1.f; c->lk_ty = f2_rand(c) * 0.9f - 0.5f; }
        c->lk_t0 = t; c->lk_next = t + 0.7 + 1.5 * f2_rand(c);
        if (fabsf(c->lk_tx - cx) + fabsf(c->lk_ty - cy) > 1.1f && t - c->bl_t0 > 0.6 && f2_rand(c) < p_glance) {
            c->bl_t0 = t - pre; c->blinks++; c->bl_double = 0; c->bl_next = t + bl_min + bl_rng * f2_rand(c);
        }
    }
    float lx, ly; f2_look_now(c, t, &lx, &ly);
    *dx = lx * s_face_amp; *dy = ly * s_face_amp * 0.5f;
    const double u = t - c->bl_t0;
    if (!s_f2_bfast) *lid = u < 0 ? 0.f : u < 0.08 ? (float)(u / 0.08) : u < 0.12 ? 1.f : u < 0.26 ? (float)(1.0 - (u - 0.12) / 0.14) : 0.f;
    else if (s_f2_sharp) {                       /* ступенями: DC_FACE_TWEEN кадров по 1/30 с на закрытие и открытие, закрыто 0.067 с */
        const double fr = 1.0 / 30.0, tc = s_f2_tween * fr, th = tc + 0.067, to = th + s_f2_tween * fr;
        const float n1 = (float)(s_f2_tween + 1);
        *lid = u < 0 ? 0.f : u < tc ? (float)(floor(u / fr) + 1) / n1 : u < th ? 1.f : u < to ? 1.f - (float)(floor((u - th) / fr) + 1) / n1 : 0.f;
    }
    else *lid = u < 0 ? 0.f : u < 0.035 ? (float)(u / 0.035) : u < 0.075 ? 1.f : u < 0.16 ? (float)(1.0 - (u - 0.075) / 0.085) : 0.f;
}
static int f2_clampi(int v, int n) { return v < 0 ? 0 : v >= n ? n - 1 : v; }
/* эмоция глаз при резких переходах: смена «целиком» (спокойно ↔ эмоция в полную силу, одна эмоция ↔ другая) — через
 * DC_FACE_TWEEN промежуточных кадров по 1/30 с на половинной силе: к «спокойно» — прежняя, иначе новая. Нужно на стыках
 * кусков реплики (звук начался или встал посреди эмоции — запечённого перехода там нет); запечённые переходы уже идут
 * через половинную силу и проходят как есть */
static void f2_emo_tween(F2Char* c, double t, int* emo, float* lev) {
    const int cls = (*emo <= 0 || *lev < 0.25f) ? 0 : *lev < 0.75f ? 1 : 2, e = cls ? *emo : 0;
    if (e != c->et_emo || cls != c->et_cls) {
        const int whole = c->et_cls >= 0 && cls != 1 && c->et_cls != 1 && s_f2_tween > 0;
        c->et_from = c->et_emo; c->et_emo = e; c->et_cls = cls; c->et_t0 = whole ? t : -1.0;
    }
    if (c->et_t0 >= 0 && t - c->et_t0 < s_f2_tween / 30.0) {
        const int m = c->et_emo ? c->et_emo : c->et_from;
        if (m > 0) { *emo = m; *lev = 0.5f; }
    }
}
/* номер кадра глаз: сетка «спокойно» или эмоции emo с ближайшим уровнем */
static int f2_frame(const F2Char* c, float dx, float dy, float lid, int emo, float lev) {
    if (emo <= 0 || emo >= c->nemo || !c->nlev || lev < 0.25f) {
        const int ix = f2_clampi((int)lroundf((dx - c->dx0) / c->dxs), c->nx), iy = f2_clampi((int)lroundf((dy - c->dy0) / c->dys), c->ny),
                  il = f2_clampi((int)lroundf(lid * (c->nl - 1)), c->nl);
        return (il * c->ny + iy) * c->nx + ix;
    }
    int li = 0; float bd = 9.f;
    for (int i = 0; i < c->nlev; i++) { const float d = fabsf(c->lev[i] - lev); if (d < bd) { bd = d; li = i; } }
    const int ix = f2_clampi((int)lroundf((dx - c->edx0) / c->edxs), c->enx), iy = f2_clampi((int)lroundf((dy - c->edy0) / c->edys), c->eny),
              il = f2_clampi((int)lroundf(lid * (c->enl - 1)), c->enl);
    return c->nx * c->ny * c->nl + ((emo - 1) * c->nlev + li) * c->enx * c->eny * c->enl + (il * c->eny + iy) * c->enx + ix;
}
/* ⭐ 28.09 (7) — МЁРТВ ПО СЮЖЕТУ: ГЛАЗА ЗАКРЫТЫ, БЕЗ МОРГАНИЯ И ВЗГЛЯДА (игрок, слот 19 = ST308: «когда персонаж мертв по
 * сюжету включай кадр с закрытыми глазами у его лица и все. Останавливай анимацию моргания и глаз»). Там лежит мёртвый учёный
 * с головой «npcdark» (та же текстура, что у Тома) — актёр игры (+80 = 0x0F, «служебный объект сцены»), а наше лицо моргало
 * и водило глазами. Лицо → актёр: блок вершин головы (подпись челюсти, jaddr) лежит в модели актёра — +0x9C начало модели,
 * голова через 0x5C4..0x6E4 (Том/учёный, Рик, Гейл — замер по сейвам); узлы костей — +0x4C, 15 узлов по 172 байта, мировая
 * точка s16×3 на +0x20, голова — кость первой вершины блока (7). Замеры поз (skipq/npc_*.py): учёный ST308 лежит — узлы на
 * 40..219 над полом, неподвижен; Том после смерти (ST406) сидит, уронив голову, — голова 634, неподвижен (один кадр анимации);
 * живой раненый Том (ST30C) сидит так же (630), но дышит (узлы ходят на 12 ед., 15 кадров); Рик на корточках рядом —
 * голова 979, неподвижен; стоят 1556..1569, Рик на стуле 1189. Правило: персонаж молчит (его реплики нет 3 с) и лежит (голова
 * ниже DC_FACE_DEAD_LIE = 400) или сидит низко (голова ниже DC_FACE_DEAD_LOW = 750) и не двигается больше 1 с → глаза —
 * кадр «закрыто» при взгляде прямо, рот и челюсть в покое, эмоций нет. Заговорил или задвигался — снова живой.
 * DC_FACE_DEAD=0 — как было. Журнал: «face2: <id> — мёртв по сюжету …» / «… снова живой». */
static int f2_clip_playing(const F2Char* c, const F2Seg* sg) {
    if (!c->talk_ok || !sg || sg->xa < 0) return 0;
    for (unsigned k = 0; k < c->nclips; k++)
        if (c->clips[k].xa == sg->xa && c->clips[k].ch == (uint16_t)sg->ch) return 1;
    return 0;
}
static int f2_dead_now(F2Char* c) {
    if (!s_f2_dead || !c->jaw_ok || !c->ncp || !strcmp(c->id, "regina")) { c->dead = 0; return 0; }
    if (s_f2_vb - c->dchk_vb < 10) return c->dead;
    c->dchk_vb = s_f2_vb;
    const uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return c->dead;
    if (!c->jaddr || !f2_jaw_at(c, ram, c->jaddr, c->japplied)) {        /* блок головы — как у челюсти (поиск раз в 2 с) */
        c->jaddr = 0; c->japplied = 0; c->jq = 0;
        if (c->dseek_vb && s_f2_vb - c->dseek_vb < 120) return c->dead = 0;
        c->dseek_vb = s_f2_vb;
        if (!f2_jaw_seek(c, ram)) return c->dead = 0;
    }
    int slot = -1; const uint8_t* a = NULL;
    for (int i = 0; i < 10; i++) {
        const uint8_t* q = ram + 0xB7ACCu + (uint32_t)i * 608u;
        if (q[0x50] != 0x0F || !(q[0x30] & 1)) continue;
        const uint32_t m = (uint32_t)(q[0x9C] | (q[0x9D] << 8) | (q[0x9E] << 16) | ((uint32_t)q[0x9F] << 24)) & 0x1FFFFFu;
        if (m && c->jaddr >= m && c->jaddr < m + 0x4000u) { slot = i; a = q; break; }
    }
    if (!a) { c->dhave = 0; return c->dead = 0; }
    const uint32_t nd = (uint32_t)(a[0x4C] | (a[0x4D] << 8) | (a[0x4E] << 16) | ((uint32_t)a[0x4F] << 24)) & 0x1FFFFFu;
    if (nd < 0x1000u || nd + 15u * 172u > 0x200000u) return c->dead = 0;
    const int ax = f2_r16(a + 32), ay = f2_r16(a + 34), az = f2_r16(a + 36);
    int hb = f2_r16(ram + c->jaddr + 6); if (hb < 0 || hb >= 15) hb = 7;
    int16_t p[15][3]; int maxd = 0;
    for (int k = 0; k < 15; k++)
        for (int j = 0; j < 3; j++) {
            p[k][j] = f2_r16(ram + nd + (uint32_t)k * 172u + 0x20u + (uint32_t)j * 2u);
            if (c->dhave && c->dslot == slot) { const int d = abs(p[k][j] - c->dpts[k][j]); if (d > maxd) maxd = d; }
        }
    if (abs(p[hb][0] - ax) > 3000 || abs(p[hb][2] - az) > 3000) return c->dead = 0;   /* узлы не этого актёра */
    if (!c->dhave || c->dslot != slot) c->dmove_vb = s_f2_vb;               /* первый взгляд — неподвижность ещё не видна */
    else if (maxd > 3) c->dmove_vb = s_f2_vb;
    memcpy(c->dpts, p, sizeof(p)); c->dslot = slot; c->dhave = 1;
    const int head = ay - p[hb][1];
    const int quiet = s_f2_vb - c->dtalk_vb > 180, still = s_f2_vb - c->dmove_vb > 60;
    const int dead = quiet && (head < s_f2_dead_lie || (head < s_f2_dead_low && still));
    if (dead != c->dead)
        fprintf(stderr, "face2: %s — %s (актёр %d, голова на %d над полом, %s, %s)%c", c->id,
                dead ? "мёртв по сюжету: глаза закрыты, моргание и взгляд стоят" : "снова живой: моргание и взгляд",
                slot, head, still ? "неподвижен" : "двигается", quiet ? "молчит" : "говорит", 10);
    return c->dead = dead;
}
static void f2_tick(void) {
    s_f2_vb++;
    if (f2_dir_changed()) {                                  /* папка лиц — новые файлы из приложения */
        LARGE_INTEGER s0, s1; QueryPerformanceCounter(&s0);
        f2_scan();
        QueryPerformanceCounter(&s1); s_f2_tscan += s1.QuadPart - s0.QuadPart;
    }
    if (s_f2_log && s_f2_vb % 600 == 0) {
        for (int i = 0; i < s_f2_n; i++) {
            F2Char* c = &s_f2[i];
            if (!c->nv) continue;
            char where[32];
            if (c->ncp > 1) snprintf(where, sizeof(where), "да, копий %d", c->ncp); else snprintf(where, sizeof(where), "%s", c->ncp ? "да" : "нет");
            fprintf(stderr, "face2: %s за 10 с: глаза %ld, рот %ld, морганий %ld, в VRAM: %s%c", c->id, c->writes, c->mwrites, c->blinks, where, 10);
            c->writes = c->mwrites = c->blinks = 0;
        }
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        const double us = 1e6 / (double)qf.QuadPart;
        fprintf(stderr, "face2: время лиц за 10 с: всего %.0f мкс (%.4f%%), из них поиск/сверка копий %.0f мкс (%ld раз) — %s; плюс просмотр папки "
                "лиц %.0f мкс (%s)%c", (double)s_f2_tall * us, (double)s_f2_tall * us / 1e5, (double)s_f2_tfind * us, s_f2_nfind,
                (s_f2_ev && s_f2_allcp) ? "по страницам, изменённым игрой" : "по таймеру", (double)s_f2_tscan * us,
                s_f2_dirh ? "по уведомлению" : "раз в 2 с", 10);
        s_f2_tall = s_f2_tfind = s_f2_tscan = 0; s_f2_nfind = 0;
        if (s_f2_pchk) {
            fprintf(stderr, "face2: проверка отметок за 10 с: страниц изменено с отметкой %ld, БЕЗ отметки %ld%c", s_f2_pmarked, s_f2_punmarked, 10);
            s_f2_pmarked = s_f2_punmarked = 0;
        }
    }
    LARGE_INTEGER qa; QueryPerformanceCounter(&qa);
    const uint32_t touched = s_f2_vpages;                    /* страницы, которые игра изменила с прошлой развёртки */
    s_f2_vpages = 0;
    if (s_f2_pchk) f2_page_check(touched);
    for (int i = 0; i < s_f2_n; i++) s_f2[i].pend |= touched;
    const double t = (double)s_f2_vb / 59.94;
    double frac = 0.0;
    const F2Seg* sg = s_f2_talk ? f2_now(&frac) : NULL;      /* каждую развёртку — иначе пауза звука не ловится */
    const F2Seg* sg_now = sg; const double frac_now = frac;
    sg = f2_delayed(sg, &frac);                              /* на лице — что звучало DC_FACE_DELAY с назад */
    /* 27.09 вечер (игрок: «у Рика в начале фраз теперь есть мусорный кадр с открытым ртом… и только на следующий кадр он в
     * нормальное состояние приходит и начинает говорить»): вершины челюсти игра проецирует только со своего следующего кадра,
     * поэтому челюсть идёт на DC_JAW_LEAD раньше рта-текстуры. Прежде опережение бралось внутри того же отрезка звука — в начале
     * фразы (до неё тишина) его не было: рот-текстура открывался на кадр раньше челюсти. Теперь челюсть берёт звук из той же линии
     * задержки, но на DC_JAW_LEAD ближе к «сейчас» — опережение есть и в начале фразы. DC_JAW_LEAD_HIST=0 — как было */
    const F2Seg* sgj = sg; double fracj = frac, jlead = s_f2_jlead;
    if (s_f2_jhist && s_f2_jlead > 0.0 && s_f2_dline > 0.0) {
        const double dj = s_f2_dline - s_f2_jlead;
        if (dj > 0.0) { sgj = f2_hist_at(dj, &fracj); jlead = 0.0; }
        else { sgj = sg_now; fracj = frac_now; jlead = -dj; }   /* опережение больше задержки: «сейчас» + остаток */
    }
    /* кадр лица: новый — всё считается заново, иначе держится прежнее (DC_FACE_FPS кадров/с; 0 — каждая развёртка) */
    double tq = t, lead = 0.0; int newfr = 1;
    if (s_f2_fps > 0) {
        const long fr = (long)floor(t * s_f2_fps);
        newfr = fr != s_f2_fr; s_f2_fr = fr;
        tq = (double)fr / s_f2_fps; lead = 0.5 / s_f2_fps;
    }
    for (int i = 0; i < s_f2_n; i++) {
        F2Char* c = &s_f2[i];
        if (!c->nv) continue;
        int fi; uint32_t st;
        if (newfr || c->hold_e < 0) {
            float dx, dy, lid; f2_eyes_step(c, tq, &dx, &dy, &lid);
            int emo = 0; float lev = 0.f; st = 0;
            f2_talk_now(c, sg, frac, lead, &st, &emo, &lev);
            uint32_t jst = st;                                   /* челюсть — по рту на DC_JAW_LEAD вперёд (вершины — со следующей проекции) */
            if (c->jaw_ok && s_f2_jlead != 0.0) { int je = 0; float jlv = 0.f; f2_talk_now(c, sgj, fracj, lead + jlead, &jst, &je, &jlv); }
            if (f2_clip_playing(c, sg) || f2_clip_playing(c, sg_now)) c->dtalk_vb = s_f2_vb;
            if (f2_dead_now(c)) { dx = dy = 0.f; lid = 1.f; emo = 0; lev = 0.f; st = 0; jst = 0; }   /* 28.09 (7): мёртв по сюжету */
            if (s_f2_onset > 0 && !c->dead) {                 /* 03.10: из покоя — только когда в звуке пошёл голос */
                extern int spu_cd_audio_peak(uint32_t back, uint32_t ahead);
                const int talk_now = st != 0 || jst != 0 || (s_f2_emo && emo > 0 && lev >= 0.25f);
                if (talk_now && !c->was_talk) {               /* челюсть идёт на DC_JAW_LEAD раньше рта — начало ловится по ней */
                    c->onh = 1; c->onh_vb = s_f2_vb;
                }
                if (!talk_now) c->onh = 0;
                if (c->onh) {
                    const int pk = sg_now ? spu_cd_audio_peak(1470, 0) : 0;   /* 2 развёртки уже ВЫДАННОГО (очередь при паузе игры выбрасывается — её не слышно) */
                    if (pk >= s_f2_vthr) {
                        if (s_f2_tlog) fprintf(stderr, "talk2: %s — голос пошёл, лицо из покоя (развёртка %llu, ждали %llu разв., пик %d)%c", c->id,
                                               (unsigned long long)s_f2_vb, (unsigned long long)(s_f2_vb - c->onh_vb), pk, 10);
                        c->onh = 0;
                    } else {
                        if (!sg_now && c->onh == 1) {
                            c->onh = 2; c->onh_cut++;
                            if (s_f2_tlog) fprintf(stderr, "talk2: %s — по разметке начало фразы, а голоса не было: игра поставила паузу, лицо осталось в покое "
                                                           "(развёртка %llu; DC_FACE_ONSET_HOLD=0 — показывать)%c", c->id, (unsigned long long)s_f2_vb, 10);
                        }
                        c->onh_held++; st = 0; jst = 0; emo = 0; lev = 0.f;
                    }
                }
                c->was_talk = talk_now;
            }
            c->hold_j = (int)jst;
            if (!s_f2_emo) emo = 0;
            if (s_f2_sharp && !c->dead) f2_emo_tween(c, tq, &emo, &lev);
            fi = f2_frame(c, dx, dy, lid, emo, lev);
            c->hold_e = fi; c->hold_m = (int)st;
        } else { fi = c->hold_e; st = (uint32_t)c->hold_m; }
        if (c->jaw_ok) f2_jaw_tick(c, (uint32_t)c->hold_j);
        if (s_f2_ev && s_f2_allcp) {
            /* без таймера: страницы, изменённые игрой, — поиск и полная сверка копий там; остальные копии не трогаем */
            if (c->pend) {
                const uint32_t pm = c->pend; c->pend = 0;
                LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
                if (f2_find_pages(c, pm)) c->force = 1;
                QueryPerformanceCounter(&q1); s_f2_tfind += q1.QuadPart - q0.QuadPart; s_f2_nfind++;
            }
            if (!c->ncp) { c->cur = -1; c->last_e = fi; c->last_m = (int)st; c->force = 0; continue; }
            if (!c->force && fi == c->last_e && (int)st == c->last_m) continue;
            int odd = 0;                                         /* страховка перед записью: 48 точек подписи каждой копии */
            for (int j = 0; j < c->ncp; j++)
                if (!f2_sig_quick(vram, c, &c->v[c->cpk[j]], c->cpx[j], c->cpy[j])) { c->pend |= f2_pagebit(c->cpx[j], c->cpy[j]); odd = 1; }
            if (odd) {
                fprintf(stderr, "face2: %s — копия лица изменилась без отметки страницы, сверяю заново (развёртка %llu)%c", c->id,
                        (unsigned long long)s_f2_vb, 10);
                continue;
            }
        } else {
            if (!c->force && fi == c->last_e && (int)st == c->last_m && s_f2_vb % 30 != 0) continue;
            /* копии на месте? затёрта хоть одна — искать заново; раз в 30 развёрток — всё равно искать (новая копия: комната
             * загрузила текстуру на другую страницу, а прежняя копия ещё лежит) */
            LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
            int lost = 0, none = 0;
            for (int j = 0; j < c->ncp && !lost; j++) { f2_use(c, j); lost = !f2_sig_at(vram, c, &c->v[c->cur], c->px, c->py); }
            if (!c->ncp || lost || (s_f2_allcp && s_f2_vb % 30 == 0)) {
                const int wn = c->ncp; int wk[F2_MAXCP], wx[F2_MAXCP], wy[F2_MAXCP];
                memcpy(wk, c->cpk, sizeof(wk)); memcpy(wx, c->cpx, sizeof(wx)); memcpy(wy, c->cpy, sizeof(wy));
                if (wn > 0 || c->force || s_f2_vb % 30 == 0) f2_find(c);
                else c->cur = -1;
                int same = c->ncp == wn;
                for (int j = 0; j < c->ncp && same; j++) same = c->cpk[j] == wk[j] && c->cpx[j] == wx[j] && c->cpy[j] == wy[j];
                if (!same) { f2_log_copies(c); c->force = 1; }
                none = !c->ncp;
            }
            QueryPerformanceCounter(&q1); s_f2_tfind += q1.QuadPart - q0.QuadPart; s_f2_nfind++;
            if (none) { c->cur = -1; c->last_e = fi; c->last_m = (int)st; c->force = 0; continue; }
        }
        int wr = 0;
        for (int j = 0; j < c->ncp; j++) {                       /* глаза и рот — в каждую копию (у копии свой вариант и кадры) */
            f2_use(c, j);
            const F2Var* v = &c->v[c->cur];
            const uint8_t* es = f2_eyefr(c, v, fi);
            if (es && (c->force || fi != c->last_e || !f2_rect_eq(vram, c, &v->r, es))) { f2_blit(c, &v->r, es); c->writes++; wr |= 1; }
            const uint8_t* ms = f2_mouth(c, c->cur, st);
            if (ms && (c->force || (int)st != c->last_m || !f2_rect_eq(vram, c, &c->mr, ms))) { f2_blit(c, &c->mr, ms); c->mwrites++; wr |= 2; }
        }
        f2_use(c, 0);
        if (wr && s_f2_log >= 2)
            fprintf(stderr, "face2w: %s развёртка %llu кадр лица %ld глаза %d рот %u%s%s%c", c->id, (unsigned long long)s_f2_vb, s_f2_fr, fi, st,
                    (wr & 1) ? " +глаза" : "", (wr & 2) ? " +рот" : "", 10);
        c->last_e = fi; c->last_m = (int)st; c->force = 0;
    }
    if (s_f2_pchk) f2_page_snap();                           /* после наших записей — точка отсчёта проверки отметок */
    LARGE_INTEGER qb; QueryPerformanceCounter(&qb); s_f2_tall += qb.QuadPart - qa.QuadPart;
}
/* 27.09: опция «Анимация лиц» выключена (gpu.c dc_face_opt_off): всем записанным лицам — исходные глаза и рот; при
 * включении — искать и писать заново */
static void f2_off_all(void) {
    for (int i = 0; i < s_f2_n; i++) {
        F2Char* c = &s_f2[i];
        f2_jaw_restore(c);                               /* вершины челюсти — исходные */
        if (c->nv) f2_release(c);
        c->cur = -1; c->ncp = 0; c->pend = 0xFFFFFFFFu; c->force = 1; c->hold_e = -1; c->et_cls = -1; c->et_t0 = -1.0; c->last_e = -2; c->last_m = -2;
    }
    memset(s_f2hist, 0, sizeof(s_f2hist));
}
/* сейв: исходные глаза и рты всех записанных лиц; загрузка — всё найти и переписать заново */
static int f2_dirty(void) {
    if (s_f2_on != 1) return 0;
    for (int i = 0; i < s_f2_n; i++) if (s_f2[i].ncp > 0) return 1;
    return 0;
}
static void f2_restore_into(uint16_t* vr) {
    if (s_f2_on != 1) return;
    for (int i = 0; i < s_f2_n; i++) {
        F2Char* c = &s_f2[i];
        for (int j = 0; j < c->ncp; j++) {                       /* все копии — с исходными глазами и ртом */
            f2_use(c, j);
            if (!f2_sig_at(vr, c, &c->v[c->cur], c->px, c->py)) continue;
            if (c->efmt != 3 || c->v[c->cur].have) f2_put(vr, c, &c->v[c->cur].r, c->v[c->cur].orig);
            const uint8_t* m0 = f2_mouth0(c, c->cur);
            if (m0) f2_put(vr, c, &c->mr, m0);
        }
        if (c->ncp) f2_use(c, 0);
    }
}
static void f2_on_load(void) {
    s_f2_vpages = 0xFFFFFFFFu; s_f2_phave = 0;                  /* VRAM целиком из сейва — все страницы изменены */
    for (int i = 0; i < s_f2_n; i++) { s_f2[i].cur = -1; s_f2[i].ncp = 0; s_f2[i].pend = 0xFFFFFFFFu; s_f2[i].force = 1; s_f2[i].hold_e = -1; s_f2[i].et_cls = -1; s_f2[i].et_t0 = -1.0;
                                       s_f2[i].japplied = 0; s_f2[i].jq = 0; s_f2[i].hold_j = 0; }   /* RAM из сейва — вершины челюсти исходные */
    s_f2_fr = -1;
    memset(s_f2seg, 0, sizeof(s_f2seg)); s_f2seg_n = 0; s_f2log_xa = -2; s_f2log_rel = -100;
    memset(s_f2hist, 0, sizeof(s_f2hist));
}
