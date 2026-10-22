// Dino Crisis ME — подмена мешей игры своими (glTF 2.0). 05.10.2026 ночь (2), игрок: «твоя задача, чтобы любой меш в игре можно
// было подменить на кастомный».
//
// Studio («Сконвертировать всё») кладёт в папки mods файлы index.txt со строками
//   actor <ключ> <glTF>          персонаж (скелетная модель игры); ключ — FNV-1a 64 счётчиков, костей, нормалей и полигонов модели
//                                (без координат вершин: их двигает челюсть, dc_faces2);
//   group <ключ> <glTF> [узел]   группа полигонов (комнаты, двери, предметы, оружие в руке); ключ — счётчики и координаты вершин.
// Игра сама, без опций:
//   персонаж — dc_meshrep_actor перед сборщиком пакетов func_800756F4 (проектор func_80073F10): модель заменена — снимок матриц
//     и света всех костей из узлов, сборщик игры не зовётся; glTF рисуется по живому скелету: вершины к костям по весам glTF,
//     кости — по именам выгрузки Studio (spine … foot_l у Регины, bone_NN у остальных), меш стоит как в позе покоя выгрузки;
//   группа — dc_meshrep_group в листовых функциях 80075F68/80075C44 (секции комнаты, двери, предметы) и в 80075094 (вложения
//     костей: оружие в руке); заменена — снимок матрицы GTE (и света), отрисовка игры пропускается; наш обход комнаты
//     (rl_emit_poly) — dc_meshrep_room_rec: группа комнаты рисуется мешем сразу, в обходе;
//   оружие в руке — вложение кости (кисть с оружием одним мешем): меняется так же, группой; у персонажа из mods без своего
//     вложения игра рисует его сама без кисти Регины (hand_cut, 06.10.2026 - папки in_hand моддеру не нужны).
// Снятое за сборку списка игры рисуется в конце этого списка (dma.c): перенос нашей камеры, глубина, подрезка у глаза — как у тела
// (pt_body_flush); картинки — в массиве repTex растра (gpu_vk_raster.cpp dcvk_rep_*), с мипами, повтор края по сэмплеру glTF.
// Свет персонажа и вложений — как у игры (NCCT: LLM/LCM кости, фон, цвет персонажа); у комнат — цвет вершин COLOR_0 или ровный.
// 06.10.2026 (игрок: «Айя поверх всего — затемнение и полосы кино должны действовать, как на Регину»; «помимо glTF — OBJ и FBX»;
// «убрал Айю — пропала кисть»): снятое рисуется В ОБХОДЕ списка игры на месте своей модели (dc_meshrep_dma_at из dma.c: вход
// списка = ближняя вершина модели игры), всё ближнее (затемнение, полосы, интерфейс) и область рисования игры — как у её модели;
// модель — glTF/GLB, FBX или OBJ (ufbx.c, MIT); оружие в руке из папки персонажа (characters\<костюм>\<in_hand_XX>\) — только
// для него, из weapons\ — для всех.
// Откат: DC_MODS_MESHES=0. Разбор: DC_MESHREP_LOG=1 (ключи моделей, которые видит игра, и что заменено).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cpu_state.h"
#include "gpu_sw_renderer.h"
#include "ufbx.h"

extern "C" {
uint8_t* memory_get_ram_ptr(void);
const uint8_t* memory_mod_ptr(uint32_t phys, uint32_t len);
int dc_nots_get_delta(float D[9], float T[3]);
int dc_ws_world(void);
float dc_ws_sq(void);   /* 10.10: множитель X мира - любая форма картинки (memory.c) */
void gpu_get_draw_offset(int* ox, int* oy);
int dcvk_enabled(void);
void dcvk_push_tri(const float* v, int semi_mode, int ztest);
int dcvk_rep_tex_add(const wchar_t* path, const uint8_t* png, size_t png_n, const uint8_t* rgba, int w, int h, int repeat);
int dcvk_rep_tex_slot(int idx);
void dcvk_rep_override(int slot, const float* uv);
int dcvk_mesh_ok(void);
int dcvk_mesh_add(const void* verts, int nv, const uint32_t* idx, int ni);
int dcvk_mesh_draw(int mesh, const float* inst, const float* bones, int nb, int occ);
void dcvk_rep_tex_drop(int idx);            // 06.10.2026: подхват на лету — картинка и меш снятой модели уходят
void dcvk_mesh_drop(int mesh);
void dc_actor_bbox_note(float x0, float y0, float x1, float y1);
void dc_shadow_note_k(const float* fx, const float* fy, const float* w, int kind, int semi);
void sw_draw_rep_triangle(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, int x2, int y2, uint32_t c2,
                          uint16_t clut_x, uint16_t clut_y, uint16_t texpage, int slot, const float* uv);
extern int g_sw_actor_prim, g_sw_char_prim, g_sw_texclass;
extern float g_pt_proj_h, g_pt_proj_ofx, g_pt_proj_ofy;
int dc_room_current(void);                  // 06.10.2026: the room of the visit (pt_renderer.c) - models of one room's kinds
const char* dc_room_name(int room);
// 06.10.2026: a creature drawn from mods under the crosshair - the depth of its nearest triangle there (pt_renderer.c)
void dc_aim_modpick_note(uint32_t ctx, float depth);
int dc_regina_hidden(void);                 // 19.10.2026 вечер: CAMERA -> Hide Regina (memory.c)
}
int gpu_env();                              // 06.10.2026 (2): DC_MESH_GPU (ниже, у gpu_build)

namespace {

// ================================================================== JSON (то, что нужно glTF)
struct Jv {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    double n = 0;
    std::string s;
    std::vector<Jv> a;
    std::vector<std::pair<std::string, Jv>> o;
    const Jv* get(const char* k) const {
        if (t != OBJ) return nullptr;
        for (const auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
    const Jv* at(size_t i) const { return (t == ARR && i < a.size()) ? &a[i] : nullptr; }
    size_t size() const { return t == ARR ? a.size() : 0; }
};
const Jv* jg(const Jv* v, const char* k) { return v ? v->get(k) : nullptr; }
const Jv* ja(const Jv* v, size_t i) { return v ? v->at(i) : nullptr; }
int ji(const Jv* v, int d = -1) { return (v && v->t == Jv::NUM) ? (int)v->n : d; }
double jd(const Jv* v, double d = 0.0) { return (v && v->t == Jv::NUM) ? v->n : d; }
std::string js(const Jv* v) { return (v && v->t == Jv::STR) ? v->s : std::string(); }

struct JParse {
    const char* p;
    const char* e;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    static void utf8(std::string& s, unsigned c) {
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63)); }
        else {
            s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 63));
            s += (char)(0x80 | ((c >> 6) & 63)); s += (char)(0x80 | (c & 63));
        }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& s) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            char c = *p++;
            if (c != '\\') { s += c; continue; }
            if (p >= e) return false;
            char x = *p++;
            switch (x) {
            case '"': s += '"'; break;
            case '\\': s += '\\'; break;
            case '/': s += '/'; break;
            case 'b': s += '\b'; break;
            case 'f': s += '\f'; break;
            case 'n': s += '\n'; break;
            case 'r': s += '\r'; break;
            case 't': s += '\t'; break;
            case 'u': {
                unsigned u;
                if (!hex4(u)) return false;
                if (u >= 0xD800 && u < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(s, u);
                break;
            }
            default: return false;
            }
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool val(Jv& v, int depth) {
        if (depth > 96) return false;
        ws();
        if (p >= e) return false;
        const char c = *p;
        if (c == '{') {
            v.t = Jv::OBJ;
            p++;
            ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p >= e || *p != ':') return false;
                p++;
                v.o.emplace_back(std::move(k), Jv());
                if (!val(v.o.back().second, depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = Jv::ARR;
            p++;
            ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!val(v.a.back(), depth + 1)) return false;
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = Jv::STR; return str(v.s); }
        if (c == 't' && e - p >= 4 && !strncmp(p, "true", 4)) { v.t = Jv::BOOL; v.n = 1; p += 4; return true; }
        if (c == 'f' && e - p >= 5 && !strncmp(p, "false", 5)) { v.t = Jv::BOOL; v.n = 0; p += 5; return true; }
        if (c == 'n' && e - p >= 4 && !strncmp(p, "null", 4)) { v.t = Jv::NUL; p += 4; return true; }
        char* end = nullptr;
        v.n = strtod(p, &end);
        if (!end || end == p || end > e) return false;
        v.t = Jv::NUM;
        p = end;
        return true;
    }
};

// ================================================================== файлы, строки
bool read_all(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    const bool ok = n == 0 || fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}
std::wstring u8w(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
/* 18.10.2026: корень mods приходит из dc_mods.c в кодировке ANSI (GetModuleFileNameA) - он переводится так, а имена из
 * index.txt и names.txt (их пишет студия) - как UTF-8 (u8w). Прежде корень тоже шёл через UTF-8, и при русских буквах в
 * пути к игре модели модов молча не находились. */
std::wstring aw(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string w8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::string uri_dec(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += (s[i] == '/' ? '\\' : s[i]);
    }
    return o;
}
bool b64(const std::string& s, std::vector<uint8_t>& out) {
    static int8_t tab[256];
    static bool init = false;
    if (!init) {
        memset(tab, -1, sizeof tab);
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tab[(unsigned char)a[i]] = (int8_t)i;
        init = true;
    }
    uint32_t acc = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=') break;
        if (tab[c] < 0) continue;
        acc = (acc << 6) | (uint32_t)tab[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return true;
}
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
int16_t rs16(const uint8_t* p) { return (int16_t)rd16(p); }
// 06.10.2026: a skeleton node (172 bytes) by its game address: the console's RAM, or past the game's pool of 100 - the
// memory for mods (memory.c dc_node_alloc_extra; ten raptors in a room took them all). nullptr - not a node's place.
const uint8_t* node_at(const uint8_t* R, uint32_t a) {
    const uint32_t p = a & 0x1FFFFFFFu;
    if (p >= 0x10000u && p + 172u <= 0x200000u) return R + p;
    if (p >= 0x01000000u) return memory_mod_ptr(p, 172u);
    return nullptr;
}

// ================================================================== матрицы glTF (4x4 по столбцам)
struct M4 { float m[16]; };
M4 m4id() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
M4 m4mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
M4 m4trs(const float t[3], const float q[4], const float s[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = m4id();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0]; r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1]; r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2]; r.m[9] = (2 * (y * z - x * w)) * s[2]; r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}
void m4pt(const M4& a, const float* p, float* o) {
    for (int r = 0; r < 3; r++) o[r] = a.m[r] * p[0] + a.m[4 + r] * p[1] + a.m[8 + r] * p[2] + a.m[12 + r];
}
void m4dir(const M4& a, const float* p, float* o) {
    for (int r = 0; r < 3; r++) o[r] = a.m[r] * p[0] + a.m[4 + r] * p[1] + a.m[8 + r] * p[2];
}

// ================================================================== glTF
struct Gltf {
    Jv j;
    std::vector<std::vector<uint8_t>> bufs;
    std::wstring dir;
};

bool gltf_load(const std::wstring& path, Gltf& g, std::string& err) {
    std::vector<uint8_t> raw;
    if (!read_all(path, raw)) { err = "file cannot be read"; return false; }
    const size_t sl = path.find_last_of(L"\\/");
    g.dir = sl == std::wstring::npos ? std::wstring() : path.substr(0, sl + 1);
    std::string text;
    std::vector<uint8_t> glbbin;
    bool glb = false;
    if (raw.size() >= 20 && memcmp(raw.data(), "glTF", 4) == 0) {
        glb = true;
        uint32_t len = rd32(&raw[8]);
        if (len > raw.size()) len = (uint32_t)raw.size();
        size_t off = 12;
        while (off + 8 <= len) {
            const uint32_t cl = rd32(&raw[off]), ct = rd32(&raw[off + 4]);
            off += 8;
            if (off + cl > len) break;
            if (ct == 0x4E4F534Au) text.assign((const char*)&raw[off], cl);
            else if (ct == 0x004E4942u) glbbin.assign(raw.begin() + off, raw.begin() + off + cl);
            off += (cl + 3u) & ~3u;
        }
    } else {
        text.assign((const char*)raw.data(), raw.size());
    }
    JParse jp{text.c_str(), text.c_str() + text.size()};
    if (!jp.val(g.j, 0) || g.j.t != Jv::OBJ) { err = "JSON is broken"; return false; }
    const Jv* bufs = g.j.get("buffers");
    for (size_t i = 0; i < (bufs ? bufs->size() : 0); i++) {
        const std::string uri = js(jg(bufs->at(i), "uri"));
        std::vector<uint8_t> data;
        if (uri.empty()) {
            if (glb && i == 0) data = glbbin;
        } else if (uri.compare(0, 5, "data:") == 0) {
            const size_t c = uri.find(',');
            if (c != std::string::npos) b64(uri.substr(c + 1), data);
        } else if (!read_all(g.dir + u8w(uri_dec(uri)), data)) {
            err = "no buffer file " + uri;
            return false;
        }
        g.bufs.push_back(std::move(data));
    }
    return true;
}

// значения аксессора как float (нормированные — в 0..1 / −1..1), nc — компонент на элемент
bool acc_read(const Gltf& g, int ai, std::vector<float>& out, int& nc) {
    out.clear();
    nc = 0;
    const Jv* a = ja(g.j.get("accessors"), (size_t)ai);
    if (!a || ai < 0) return false;
    const int ct = ji(jg(a, "componentType"));
    const double cntd = jd(jg(a, "count"));
    if (!(cntd >= 0.0 && cntd <= 4000000.0)) return false;   /* 18.10: до приведения к size_t (см. смещение ниже) */
    const size_t count = (size_t)cntd;
    const std::string type = js(jg(a, "type"));
    nc = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
    if (!nc) return false;
    const Jv* nj = jg(a, "normalized");
    const bool norm = nj && nj->t == Jv::BOOL && nj->n != 0;
    const int cs = (ct == 5120 || ct == 5121) ? 1 : (ct == 5122 || ct == 5123) ? 2 : (ct == 5125 || ct == 5126) ? 4 : 0;
    if (!cs) return false;
    out.assign(count * (size_t)nc, 0.f);
    const int bvi = ji(jg(a, "bufferView"));
    if (bvi < 0) return true;
    const Jv* bv = ja(g.j.get("bufferViews"), (size_t)bvi);
    if (!bv) return false;
    const int bi = ji(jg(bv, "buffer"), 0);
    if (bi < 0 || bi >= (int)g.bufs.size()) return false;
    const std::vector<uint8_t>& buf = g.bufs[(size_t)bi];
    const size_t es = (size_t)cs * (size_t)nc;
    /* 18.10.2026 (разбор кода): смещение и шаг из файла мода проверяются до приведения к size_t - отрицательное или
     * огромное число переполняло проверку границ ниже, и чтение уходило за буфер (битый файл мода - падение игры) */
    const double offd = jd(jg(bv, "byteOffset")) + jd(jg(a, "byteOffset"));
    const double strd = jg(bv, "byteStride") ? jd(jg(bv, "byteStride")) : (double)es;
    if (!(offd >= 0.0 && offd <= (double)buf.size()) || !(strd >= 0.0 && strd <= (double)buf.size())) return false;
    const size_t off = (size_t)offd;
    const size_t stride = (size_t)strd;
    if (count && off + (count - 1) * stride + es > buf.size()) return false;
    for (size_t i = 0; i < count; i++) {
        const uint8_t* p = buf.data() + off + i * stride;
        for (int k = 0; k < nc; k++) {
            const uint8_t* q = p + (size_t)k * cs;
            float v = 0.f;
            switch (ct) {
            case 5126: memcpy(&v, q, 4); break;
            case 5125: v = (float)rd32(q); break;
            case 5123: v = norm ? rd16(q) / 65535.f : (float)rd16(q); break;
            case 5122: v = norm ? std::max(rs16(q) / 32767.f, -1.f) : (float)rs16(q); break;
            case 5121: v = norm ? q[0] / 255.f : (float)q[0]; break;
            case 5120: v = norm ? std::max((int8_t)q[0] / 127.f, -1.f) : (float)(int8_t)q[0]; break;
            }
            out[i * (size_t)nc + (size_t)k] = v;
        }
    }
    return true;
}

// ================================================================== меш замены (координаты игры: мм, Y вниз, Z вперёд)
const char* kReginaBones[15] = {"spine", "arm_r_upper", "arm_r_lower", "hand_r", "arm_l_upper", "arm_l_lower", "hand_l",
                                "head", "pelvis", "leg_r_upper", "leg_r_lower", "foot_r", "leg_l_upper", "leg_l_lower", "foot_l"};
// имя кости выгрузки Studio → номер кости игры (−1 — не кость); хвосты Blender «.001» не мешают
int bone_of_name(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    const size_t dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    for (int i = 0; i < 15; i++)
        if (s == kReginaBones[i]) return i;
    if (s.compare(0, 4, "bone") == 0) {
        size_t k = 4;
        while (k < s.size() && (s[k] == '_' || s[k] == ' ')) k++;
        if (k < s.size() && isdigit((unsigned char)s[k])) return atoi(s.c_str() + k);
    }
    return -1;
}

struct RTex {
    std::wstring path;
    std::vector<uint8_t> png, rgba;
    int w = 0, h = 0;
    bool repeat = true;
    int idx = -2;                       // −2 — ещё не отдана растру, −1 — растр её не взял
};
struct RMat { int tex = -1; float col[4] = {1.f, 1.f, 1.f, 1.f}; };
struct RMesh {
    std::vector<float> pos, nrm, uv, col;   // col — RGBA 0..1 на вершину (пусто — COLOR_0 нет)
    std::vector<uint8_t> jb;                // 4 кости игры на вершину (255 — корень)
    std::vector<float> jw;
    std::vector<uint32_t> idx;
    std::vector<int> tmat;                  // материал треугольника
    std::vector<RMat> mats;
    std::vector<RTex> texs;
    float jpos[64][3] = {};                 // суставы скелета glTF в позе покоя (координаты игры), по костям игры
    uint8_t jhave[64] = {};
    int nverts() const { return (int)(pos.size() / 3); }
};

// вершина модели для видеокарты: позы покоя склеены (= MeshVtx растра, 48 Б); slot — номер картинки растра (в потоке
// разбора — номер материала, номер картинки ставит gpu_build)
struct GVtx { float p[3], n[3], uv[2]; uint8_t j[4], w[4], c[4]; float slot; };
static_assert(sizeof(GVtx) == 48, "GVtx");

struct Entry {
    int kind = 0;                           // 0 персонаж, 1 группа
    uint64_t key = 0;
    std::wstring path;
    std::string node, rel;
    std::atomic<int> state{0};              // 0 ждёт, 2 готово, −1 не читается
    std::string err;
    RMesh mesh;
    bool tex_reg = false;
    int said = 0;
    // 06.10.2026 (игрок: «разные костюмы Регины — разные модели… все остальные костюмы не должны быть тронуты»): вложение
    // (кисть с оружием) одно и то же у двух костюмов (H20 = H22) — подменять только при модели персонажа с этими ключами
    // (поле строки «actor=<ключ>[,<ключ>]»); пусто — при любой
    std::vector<uint64_t> only_actor;
    // 06.10.2026 (player: "the compsognathus has its texture shifted again"): an enemy kind put into a room whose video memory
    // has not its pictures - the Studio writes its model with its own pictures to mods\rooms\<ROOM>\kinds\ (kinds\index.txt);
    // that body is drawn from there in this room only. Empty - everywhere.
    std::string only_room;
    // 06.10.2026: модель в видеопамяти (gpu_vk_raster.cpp dcvk_mesh_add): −2 — ещё не пробовали, −1 — нет (путь процессора)
    int gpu = -2;
    float rad = 0.f;                        // наибольшее расстояние вершины от сустава своей кости (рамка и тени по суставам)
    // 06.10.2026 (2): склеенные вершины и индексы для видеокарты — готовит поток разбора (gpu_prep), после загрузки в
    // видеопамять — пусто. Прежде склейка шла в кадре игры при первом появлении модели: OBJ на 10 млн треугольников держал
    // кадр ~10 с, и сторож зависаний закрывал игру.
    std::vector<GVtx> gpv;
    std::vector<uint32_t> gpi;
    bool prepped = false;
    // 06.10.2026: the crosshair's body (the game's assembler does not run for a model from mods): the model's own triangles,
    // a big model thinned on a grid - vertices in model space with their bones (pick_build, parse thread)
    std::vector<float> pk_pos, pk_jw;
    std::vector<uint8_t> pk_jb;
    std::vector<uint32_t> pk_idx;
    // 06.10.2026 (подхват модов на лету): отметка папки модели (имена, размеры и время файлов); снята с подмены — поток
    // разбора её пропускает; при перечитывании — прежняя запись, которая остаётся вместо этой
    uint64_t stamp = 0;
    std::atomic<bool> dead{false};
    Entry* reuse = nullptr;
};

// записи моделей и их ключи: то, по чему рисует игра (s_ent…), и то, что собирает перечитывание в своём потоке
struct MeshSet {
    std::vector<std::unique_ptr<Entry>> ent;
    std::unordered_map<uint64_t, Entry*> actor, group;
    std::unordered_map<uint64_t, std::vector<Entry*>> group_own;
    std::unordered_map<uint64_t, std::vector<Entry*>> actor_room;   // 06.10.2026: a body of one room only (Entry::only_room)
};

// узлы: мировые матрицы позы покоя
void node_worlds(const Gltf& g, std::vector<M4>& W, std::vector<int>& parent) {
    const Jv* nodes = g.j.get("nodes");
    const size_t n = nodes ? nodes->size() : 0;
    std::vector<M4> L(n, m4id());
    parent.assign(n, -1);
    for (size_t i = 0; i < n; i++) {
        const Jv* nd = nodes->at(i);
        const Jv* mt = jg(nd, "matrix");
        if (mt && mt->size() == 16) {
            for (int k = 0; k < 16; k++) L[i].m[k] = (float)jd(mt->at((size_t)k));
        } else {
            float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            const Jv* tj = jg(nd, "translation");
            const Jv* qj = jg(nd, "rotation");
            const Jv* sj = jg(nd, "scale");
            for (int k = 0; k < 3; k++) {
                if (tj && tj->size() == 3) t[k] = (float)jd(tj->at((size_t)k));
                if (sj && sj->size() == 3) s[k] = (float)jd(sj->at((size_t)k), 1.0);
            }
            if (qj && qj->size() == 4)
                for (int k = 0; k < 4; k++) q[k] = (float)jd(qj->at((size_t)k));
            L[i] = m4trs(t, q, s);
        }
        const Jv* ch = jg(nd, "children");
        for (size_t c = 0; c < (ch ? ch->size() : 0); c++) {
            const int ci = ji(ch->at(c));
            if (ci >= 0 && (size_t)ci < n) parent[(size_t)ci] = (int)i;
        }
    }
    W.assign(n, m4id());
    std::vector<int> done(n, 0);
    for (size_t i = 0; i < n; i++) {
        std::vector<int> chain;
        for (int k = (int)i; k >= 0 && !done[(size_t)k] && chain.size() < 256; k = parent[(size_t)k]) chain.push_back(k);
        for (size_t c = chain.size(); c-- > 0;) {
            const int k = chain[c];
            W[(size_t)k] = parent[(size_t)k] >= 0 ? m4mul(W[(size_t)parent[(size_t)k]], L[(size_t)k]) : L[(size_t)k];
            done[(size_t)k] = 1;
        }
    }
}

int node_bone(const Gltf& g, const std::vector<int>& parent, int ni) {
    const Jv* nodes = g.j.get("nodes");
    for (int k = ni, guard = 0; k >= 0 && guard < 256; k = parent[(size_t)k], guard++) {
        const int b = bone_of_name(js(jg(ja(nodes, (size_t)k), "name")));
        if (b >= 0) return b;
    }
    return 255;
}

int mesh_material(const Gltf& g, int mi, RMesh& m, std::unordered_map<int, int>& done) {
    auto it = done.find(mi);
    if (it != done.end()) return it->second;
    RMat rm;
    const Jv* mat = mi >= 0 ? ja(g.j.get("materials"), (size_t)mi) : nullptr;
    const Jv* pbr = jg(mat, "pbrMetallicRoughness");
    const Jv* f = jg(pbr, "baseColorFactor");
    if (f && f->size() == 4)
        for (int k = 0; k < 4; k++) rm.col[k] = (float)jd(f->at((size_t)k), 1.0);
    const int ti = ji(jg(jg(pbr, "baseColorTexture"), "index"));
    const Jv* tex = ti >= 0 ? ja(g.j.get("textures"), (size_t)ti) : nullptr;
    const int si = ji(jg(tex, "source"));
    const Jv* img = si >= 0 ? ja(g.j.get("images"), (size_t)si) : nullptr;
    if (img) {
        RTex t;
        const Jv* smp = ja(g.j.get("samplers"), (size_t)ji(jg(tex, "sampler"), 0x7FFFFFFF));
        const int wrap = ji(jg(smp, "wrapS"), 10497);
        t.repeat = wrap != 33071;
        const std::string uri = js(jg(img, "uri"));
        if (!uri.empty() && uri.compare(0, 5, "data:") == 0) {
            const size_t c = uri.find(',');
            if (c != std::string::npos) b64(uri.substr(c + 1), t.png);
        } else if (!uri.empty()) {
            t.path = g.dir + u8w(uri_dec(uri));
        } else {
            const Jv* bv = ja(g.j.get("bufferViews"), (size_t)ji(jg(img, "bufferView"), 0x7FFFFFFF));
            const int bi = ji(jg(bv, "buffer"), 0);
            if (bv && bi >= 0 && bi < (int)g.bufs.size()) {
                const size_t o = (size_t)jd(jg(bv, "byteOffset")), l = (size_t)jd(jg(bv, "byteLength"));
                if (o + l <= g.bufs[(size_t)bi].size()) t.png.assign(g.bufs[(size_t)bi].begin() + o, g.bufs[(size_t)bi].begin() + o + l);
            }
        }
        if (!t.path.empty() || !t.png.empty()) {
            rm.tex = (int)m.texs.size();
            m.texs.push_back(std::move(t));
        }
    }
    if (rm.tex < 0) {                       // без картинки — цвет материала (2x2), вершины его не умножают
        RTex t;
        t.w = t.h = 2;
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) t.rgba.push_back((uint8_t)std::min(255.f, std::max(0.f, rm.col[k] * 255.f + 0.5f)));
        rm.tex = (int)m.texs.size();
        m.texs.push_back(std::move(t));
        rm.col[0] = rm.col[1] = rm.col[2] = rm.col[3] = 1.f;
    }
    const int id = (int)m.mats.size();
    m.mats.push_back(rm);
    done[mi] = id;
    return id;
}

// меш замены: actor — со скелетом (вершины к костям игры), иначе — узел node (или все меши сцены) неподвижно
bool build_mesh(const Gltf& g, bool actor, const std::string& node, RMesh& m, std::string& err) {
    std::vector<M4> W;
    std::vector<int> parent;
    node_worlds(g, W, parent);
    const Jv* nodes = g.j.get("nodes");
    const size_t nn = nodes ? nodes->size() : 0;
    std::vector<int> pick;
    for (size_t i = 0; i < nn; i++) {
        if (!jg(nodes->at(i), "mesh")) continue;
        if (!node.empty()) {
            bool under = false;
            for (int k = (int)i, guard = 0; k >= 0 && guard < 256 && !under; k = parent[(size_t)k], guard++)
                under = js(jg(nodes->at((size_t)k), "name")) == node;
            if (!under) continue;
        }
        pick.push_back((int)i);
    }
    if (pick.empty() && !node.empty()) { err = "no node " + node; return false; }   // пустая сцена — меш игры просто прячется
    std::unordered_map<int, int> matmap;
    for (int ni : pick) {
        const Jv* nd = nodes->at((size_t)ni);
        const Jv* mesh = ja(g.j.get("meshes"), (size_t)ji(jg(nd, "mesh")));
        const Jv* skin = actor ? ja(g.j.get("skins"), (size_t)ji(jg(nd, "skin"), 0x7FFFFFFF)) : nullptr;
        // кости скина → кости игры (у не найденной — ближний предок с именем кости, иначе корень)
        std::vector<int> jbone;
        std::vector<M4> jm;
        if (skin) {
            const Jv* joints = jg(skin, "joints");
            std::vector<float> ibm;
            int nc = 0;
            const bool have_ibm = acc_read(g, ji(jg(skin, "inverseBindMatrices")), ibm, nc) && nc == 16;
            for (size_t k = 0; k < (joints ? joints->size() : 0); k++) {
                const int jn = ji(joints->at(k));
                jbone.push_back(jn >= 0 && (size_t)jn < nn ? node_bone(g, parent, jn) : 255);
                // сустав, названный костью игры: его место в позе покоя — длины костей модели (свои пропорции)
                const int jb = jn >= 0 && (size_t)jn < nn ? bone_of_name(js(jg(nodes->at((size_t)jn), "name"))) : -1;
                if (jb >= 0 && jb < 64 && !m.jhave[jb]) {
                    const float* t = &W[(size_t)jn].m[12];
                    m.jpos[jb][0] = t[0] * 1000.f; m.jpos[jb][1] = -t[1] * 1000.f; m.jpos[jb][2] = -t[2] * 1000.f;
                    m.jhave[jb] = 1;
                }
                M4 ib = m4id();
                if (have_ibm && (k + 1) * 16 <= ibm.size()) memcpy(ib.m, &ibm[k * 16], sizeof ib.m);
                jm.push_back(jn >= 0 && (size_t)jn < nn ? m4mul(W[(size_t)jn], ib) : m4id());
            }
        }
        const int rigid_bone = actor ? node_bone(g, parent, ni) : 255;
        const Jv* prims = jg(mesh, "primitives");
        for (size_t pi = 0; pi < (prims ? prims->size() : 0); pi++) {
            const Jv* pr = prims->at(pi);
            const int mode = ji(jg(pr, "mode"), 4);
            if (mode != 4) continue;
            const Jv* at = jg(pr, "attributes");
            std::vector<float> P, N, T, C, J, Wt, I;
            int ncP = 0, ncN = 0, ncT = 0, ncC = 0, ncJ = 0, ncW = 0, ncI = 0;
            if (!acc_read(g, ji(jg(at, "POSITION")), P, ncP) || ncP != 3) continue;
            acc_read(g, ji(jg(at, "NORMAL")), N, ncN);
            acc_read(g, ji(jg(at, "TEXCOORD_0")), T, ncT);
            acc_read(g, ji(jg(at, "COLOR_0")), C, ncC);
            const bool skinned = skin && acc_read(g, ji(jg(at, "JOINTS_0")), J, ncJ) && ncJ == 4 &&
                                 acc_read(g, ji(jg(at, "WEIGHTS_0")), Wt, ncW) && ncW == 4;
            const size_t nv = P.size() / 3;
            const uint32_t base = (uint32_t)m.nverts();
            for (size_t v = 0; v < nv; v++) {
                float p[3] = {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]}, n[3] = {0, 1, 0}, op[3] = {0, 0, 0}, on[3] = {0, 0, 0};
                if (ncN == 3 && N.size() >= v * 3 + 3) { n[0] = N[v * 3]; n[1] = N[v * 3 + 1]; n[2] = N[v * 3 + 2]; }
                uint8_t bb[4] = {255, 255, 255, 255};
                float bw[4] = {0, 0, 0, 0};
                if (skinned) {
                    float ws = 0.f;
                    for (int k = 0; k < 4; k++) {
                        const float w = Wt[v * 4 + (size_t)k];
                        const int ji2 = (int)J[v * 4 + (size_t)k];
                        if (w <= 0.f || ji2 < 0 || ji2 >= (int)jm.size()) continue;
                        float tp[3], tn[3];
                        m4pt(jm[(size_t)ji2], p, tp);
                        m4dir(jm[(size_t)ji2], n, tn);
                        for (int c = 0; c < 3; c++) { op[c] += w * tp[c]; on[c] += w * tn[c]; }
                        ws += w;
                        const int b = jbone[(size_t)ji2];
                        int slot = -1;
                        for (int q = 0; q < 4; q++) if (bb[q] == (uint8_t)b && bw[q] > 0.f) slot = q;
                        if (slot < 0) for (int q = 0; q < 4; q++) if (bw[q] <= 0.f) { slot = q; break; }
                        if (slot >= 0) { bb[slot] = (uint8_t)b; bw[slot] += w; }
                    }
                    if (ws > 0.f) for (int c = 0; c < 3; c++) { op[c] /= ws; on[c] /= ws; }
                    float bs = bw[0] + bw[1] + bw[2] + bw[3];
                    if (bs > 0.f) for (int q = 0; q < 4; q++) bw[q] /= bs;
                    else { bb[0] = 255; bw[0] = 1.f; }
                } else {
                    m4pt(W[(size_t)ni], p, op);
                    m4dir(W[(size_t)ni], n, on);
                    bb[0] = (uint8_t)rigid_bone;
                    bw[0] = 1.f;
                }
                const float ln = sqrtf(on[0] * on[0] + on[1] * on[1] + on[2] * on[2]);
                if (ln > 1e-8f) for (int c = 0; c < 3; c++) on[c] /= ln;
                // glTF (метры, Y вверх) → игра (мм, Y вниз, Z от экрана)
                m.pos.push_back(op[0] * 1000.f); m.pos.push_back(-op[1] * 1000.f); m.pos.push_back(-op[2] * 1000.f);
                m.nrm.push_back(on[0]); m.nrm.push_back(-on[1]); m.nrm.push_back(-on[2]);
                m.uv.push_back(ncT == 2 && T.size() >= v * 2 + 2 ? T[v * 2] : 0.f);
                m.uv.push_back(ncT == 2 && T.size() >= v * 2 + 2 ? T[v * 2 + 1] : 0.f);
                if (ncC >= 3 && C.size() >= v * (size_t)ncC + 3) {
                    if (m.col.empty()) m.col.assign((size_t)base * 4, 1.f);
                    for (int c = 0; c < 3; c++) m.col.push_back(C[v * (size_t)ncC + (size_t)c]);
                    m.col.push_back(ncC == 4 ? C[v * 4 + 3] : 1.f);
                } else if (!m.col.empty()) {
                    for (int c = 0; c < 4; c++) m.col.push_back(1.f);
                }
                for (int q = 0; q < 4; q++) { m.jb.push_back(bb[q]); m.jw.push_back(bw[q]); }
            }
            const int mat = mesh_material(g, ji(jg(pr, "material")), m, matmap);
            const size_t i0 = m.idx.size();
            if (acc_read(g, ji(jg(pr, "indices")), I, ncI) && ncI == 1) {
                for (size_t k = 0; k + 2 < I.size(); k += 3)
                    for (int c = 0; c < 3; c++) m.idx.push_back(base + (uint32_t)I[k + (size_t)c]);
            } else {
                for (uint32_t k = 0; k + 2 < (uint32_t)nv; k += 3)
                    for (uint32_t c = 0; c < 3; c++) m.idx.push_back(base + k + c);
            }
            for (size_t k = i0; k < m.idx.size(); k += 3) m.tmat.push_back(mat);
        }
    }
    for (uint32_t& i : m.idx)
        if (i >= (uint32_t)m.nverts()) i = 0;
    return true;
}

// ================================================================== FBX и OBJ (ufbx) — 06.10.2026, игрок: «помимо glTF надо ещё OBJ и FBX»
// Тот же меш замены, что из glTF: ufbx приводит сцену к метрам и осям glTF (Y вверх, правая), дальше — игра (мм, Y вниз).
// Кости — по именам узлов (как у glTF; приставки «Armature|», «mixamorig:» не мешают), веса — до 4 костей, суставы покоя —
// по узлам костей; без скина — меш едет с костью ближайшего узла-предка с именем кости (у OBJ — по имени объекта), иначе с корнем.
int bone_of_any_name(const char* s, size_t n) {
    std::string t(s ? s : "", s ? n : 0);
    const size_t p = t.find_last_of("|:");
    if (p != std::string::npos) t = t.substr(p + 1);
    return bone_of_name(t);
}
int ufbx_node_bone(const ufbx_node* nd) {
    for (int guard = 0; nd && guard < 256; nd = nd->parent, guard++) {
        const int b = bone_of_any_name(nd->name.data, nd->name.length);
        if (b >= 0) return b;
    }
    return 255;
}
bool ufbx_node_under(const ufbx_node* nd, const std::string& want) {
    for (int guard = 0; nd && guard < 256; nd = nd->parent, guard++) {
        std::string t(nd->name.data, nd->name.length);
        const size_t p = t.find_last_of("|:");
        if (p != std::string::npos) t = t.substr(p + 1);
        if (t == want) return true;
    }
    return false;
}
// картинка материала: вложенные байты или файл (путь от модели, затем имя файла рядом с моделью)
bool ufbx_tex(const ufbx_texture* tx, const std::wstring& dir, RTex& t) {
    if (!tx) return false;
    if (tx->content.size) {
        const uint8_t* b = (const uint8_t*)tx->content.data;
        t.png.assign(b, b + tx->content.size);
        return true;
    }
    const ufbx_string* names[3] = {&tx->relative_filename, &tx->filename, &tx->absolute_filename};
    for (const ufbx_string* s : names) {
        if (!s->length) continue;
        std::wstring w = u8w(std::string(s->data, s->length));
        for (auto& c : w) if (c == L'/') c = L'\\';
        const size_t sl = w.find_last_of(L'\\');
        const std::wstring cands[2] = {(w.size() > 1 && w[1] == L':') ? w : dir + w, dir + (sl == std::wstring::npos ? w : w.substr(sl + 1))};
        for (const std::wstring& c : cands)
            if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) { t.path = c; return true; }
    }
    return false;
}
int ufbx_mat(const ufbx_material* mt, const std::wstring& dir, RMesh& m, std::unordered_map<const void*, int>& done) {
    auto it = done.find(mt);
    if (it != done.end()) return it->second;
    RMat rm;
    RTex t;
    bool have_tex = false;
    if (mt) {
        const ufbx_material_map& bc = mt->pbr.base_color.has_value || mt->pbr.base_color.texture ? mt->pbr.base_color : mt->fbx.diffuse_color;
        have_tex = ufbx_tex(bc.texture ? bc.texture : mt->fbx.diffuse_color.texture, dir, t);
        if (!have_tex) {                      // без картинки — цвет материала (у картинки цвет не множим: экспортёры пишут туда 0.8)
            const ufbx_vec4 c = bc.value_vec4;
            float f = 1.f;
            if (mt->pbr.base_factor.has_value) f = (float)mt->pbr.base_factor.value_real;
            rm.col[0] = (float)c.x * f; rm.col[1] = (float)c.y * f; rm.col[2] = (float)c.z * f;
            rm.col[3] = bc.value_components >= 4 ? (float)c.w : 1.f;
        }
    }
    if (have_tex) {
        rm.tex = (int)m.texs.size();
        m.texs.push_back(std::move(t));
    } else {
        RTex ct;
        ct.w = ct.h = 2;
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) ct.rgba.push_back((uint8_t)std::min(255.f, std::max(0.f, rm.col[k] * 255.f + 0.5f)));
        rm.tex = (int)m.texs.size();
        m.texs.push_back(std::move(ct));
        rm.col[0] = rm.col[1] = rm.col[2] = rm.col[3] = 1.f;
    }
    const int id = (int)m.mats.size();
    m.mats.push_back(rm);
    done[mt] = id;
    return id;
}

bool ufbx_build(const std::wstring& path, bool actor, const std::string& node, RMesh& m, std::string& err) {
    ufbx_load_opts o = {};
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.generate_missing_normals = true;
    o.load_external_files = true;              // .mtl у OBJ
    o.ignore_missing_external_files = true;
    o.obj_search_mtl_by_filename = true;
    ufbx_error ue;
    const std::string u8 = w8(path);
    ufbx_scene* sc = ufbx_load_file(u8.c_str(), &o, &ue);
    if (!sc) { err = std::string("the file cannot be read: ") + std::string(ue.description.data, ue.description.length); return false; }
    const size_t sl = path.find_last_of(L"\\/");
    const std::wstring dir = sl == std::wstring::npos ? std::wstring() : path.substr(0, sl + 1);
    std::unordered_map<const void*, int> matmap;
    std::vector<uint32_t> tri;
    bool any = false;
    for (size_t ni = 0; ni < sc->nodes.count; ni++) {
        const ufbx_node* nd = sc->nodes.data[ni];
        const ufbx_mesh* me = nd ? nd->mesh : nullptr;
        if (!me) continue;
        if (!node.empty() && !ufbx_node_under(nd, node)) continue;
        any = true;
        const ufbx_skin_deformer* skin = actor && me->skin_deformers.count ? me->skin_deformers.data[0] : nullptr;
        std::vector<int> cbone;
        if (skin) {
            cbone.assign(skin->clusters.count, 255);
            for (size_t k = 0; k < skin->clusters.count; k++) {
                const ufbx_skin_cluster* cl = skin->clusters.data[k];
                if (!cl || !cl->bone_node) continue;
                cbone[k] = ufbx_node_bone(cl->bone_node);
                // сустав, названный костью игры: его место в позе покоя — длины костей модели (свои пропорции)
                const int jb = bone_of_any_name(cl->bone_node->name.data, cl->bone_node->name.length);
                if (jb >= 0 && jb < 64 && !m.jhave[jb]) {
                    const ufbx_vec3 t = cl->bone_node->node_to_world.cols[3];
                    m.jpos[jb][0] = (float)t.x * 1000.f; m.jpos[jb][1] = -(float)t.y * 1000.f; m.jpos[jb][2] = -(float)t.z * 1000.f;
                    m.jhave[jb] = 1;
                }
            }
        }
        const int rigid = actor ? ufbx_node_bone(nd) : 255;
        tri.resize(me->max_face_triangles * 3 + 3);
        for (size_t fi = 0; fi < me->faces.count; fi++) {
            const uint32_t nt = ufbx_triangulate_face(tri.data(), tri.size(), me, me->faces.data[fi]);
            const uint32_t mi = me->face_material.count > fi ? me->face_material.data[fi] : 0;
            const ufbx_material* mt = mi < nd->materials.count ? nd->materials.data[mi] : (mi < me->materials.count ? me->materials.data[mi] : nullptr);
            const int mat = ufbx_mat(mt, dir, m, matmap);
            for (uint32_t t = 0; t < nt; t++) {
                for (int c = 0; c < 3; c++) {
                    const uint32_t ix = tri[t * 3 + (uint32_t)c];
                    const ufbx_vec3 p = ufbx_get_vertex_vec3(&me->vertex_position, ix);
                    ufbx_vec3 n;
                    n.x = 0; n.y = 1; n.z = 0;
                    if (me->vertex_normal.exists) n = ufbx_get_vertex_vec3(&me->vertex_normal, ix);
                    float op[3] = {0, 0, 0}, on[3] = {0, 0, 0};
                    uint8_t bb[4] = {255, 255, 255, 255};
                    float bw[4] = {0, 0, 0, 0};
                    const uint32_t vi = ix < me->vertex_indices.count ? me->vertex_indices.data[ix] : 0;
                    bool skinned = false;
                    if (skin && vi < skin->vertices.count && skin->vertices.data[vi].num_weights) {
                        const ufbx_skin_vertex sv = skin->vertices.data[vi];
                        float ws = 0.f;
                        for (uint32_t k = 0; k < sv.num_weights; k++) {
                            const ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + k];
                            const float w = (float)sw.weight;
                            if (w <= 0.f || sw.cluster_index >= skin->clusters.count) continue;
                            const ufbx_skin_cluster* cl = skin->clusters.data[sw.cluster_index];
                            const ufbx_vec3 tp = ufbx_transform_position(&cl->geometry_to_world, p);
                            const ufbx_vec3 tn = ufbx_transform_direction(&cl->geometry_to_world, n);
                            op[0] += w * (float)tp.x; op[1] += w * (float)tp.y; op[2] += w * (float)tp.z;
                            on[0] += w * (float)tn.x; on[1] += w * (float)tn.y; on[2] += w * (float)tn.z;
                            ws += w;
                            const int b = cbone[sw.cluster_index];
                            int slot = -1;
                            for (int q = 0; q < 4; q++) if (bb[q] == (uint8_t)b && bw[q] > 0.f) slot = q;
                            if (slot < 0) for (int q = 0; q < 4; q++) if (bw[q] <= 0.f) { slot = q; break; }
                            if (slot < 0) {               // больше 4 костей — вытесняем самую лёгкую
                                int lo = 0;
                                for (int q = 1; q < 4; q++) if (bw[q] < bw[lo]) lo = q;
                                if (bw[lo] < w) { slot = lo; bw[lo] = 0.f; }
                            }
                            if (slot >= 0) { bb[slot] = (uint8_t)b; bw[slot] += w; }
                        }
                        if (ws > 0.f) {
                            skinned = true;
                            for (int k = 0; k < 3; k++) { op[k] /= ws; on[k] /= ws; }
                            const float bs = bw[0] + bw[1] + bw[2] + bw[3];
                            if (bs > 0.f) for (int q = 0; q < 4; q++) bw[q] /= bs;
                            else { bb[0] = 255; bw[0] = 1.f; }
                        }
                    }
                    if (!skinned) {
                        const ufbx_vec3 tp = ufbx_transform_position(&nd->geometry_to_world, p);
                        const ufbx_vec3 tn = ufbx_transform_direction(&nd->geometry_to_world, n);
                        op[0] = (float)tp.x; op[1] = (float)tp.y; op[2] = (float)tp.z;
                        on[0] = (float)tn.x; on[1] = (float)tn.y; on[2] = (float)tn.z;
                        bb[0] = (uint8_t)rigid; bw[0] = 1.f;
                        bb[1] = bb[2] = bb[3] = 255; bw[1] = bw[2] = bw[3] = 0.f;
                    }
                    const float ln = sqrtf(on[0] * on[0] + on[1] * on[1] + on[2] * on[2]);
                    if (ln > 1e-8f) for (int k = 0; k < 3; k++) on[k] /= ln;
                    // метры, Y вверх → игра (мм, Y вниз, Z от экрана) — как у glTF
                    m.pos.push_back(op[0] * 1000.f); m.pos.push_back(-op[1] * 1000.f); m.pos.push_back(-op[2] * 1000.f);
                    m.nrm.push_back(on[0]); m.nrm.push_back(-on[1]); m.nrm.push_back(-on[2]);
                    float u = 0.f, v = 0.f;
                    if (me->vertex_uv.exists) { const ufbx_vec2 uv = ufbx_get_vertex_vec2(&me->vertex_uv, ix); u = (float)uv.x; v = 1.f - (float)uv.y; }
                    m.uv.push_back(u); m.uv.push_back(v);
                    if (me->vertex_color.exists) {
                        if (m.col.empty()) m.col.assign((size_t)(m.nverts() - 1) * 4, 1.f);
                        const ufbx_vec4 cc = ufbx_get_vertex_vec4(&me->vertex_color, ix);
                        m.col.push_back((float)cc.x); m.col.push_back((float)cc.y); m.col.push_back((float)cc.z); m.col.push_back((float)cc.w);
                    } else if (!m.col.empty()) {
                        for (int k = 0; k < 4; k++) m.col.push_back(1.f);
                    }
                    for (int q = 0; q < 4; q++) { m.jb.push_back(bb[q]); m.jw.push_back(bw[q]); }
                    m.idx.push_back((uint32_t)(m.nverts() - 1));
                }
                m.tmat.push_back(mat);
            }
        }
    }
    ufbx_free_scene(sc);
    if (!any && !node.empty()) { err = "no node " + node; return false; }
    return true;
}

// модель по расширению: .fbx / .obj — ufbx, иначе glTF / GLB
bool is_ufbx_path(const std::wstring& p) {
    const size_t dot = p.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring x = p.substr(dot);
    return !_wcsicmp(x.c_str(), L".fbx") || !_wcsicmp(x.c_str(), L".obj");
}

// ================================================================== состояние
int s_on = -1;
int s_log = 0;
std::vector<std::unique_ptr<Entry>> s_ent;
std::unordered_map<uint64_t, Entry*> s_actor, s_group;
// 06.10.2026: вложение только для своего персонажа (оружие костюма — из папки костюма; строки index.txt с actor=) — ключ группы
// → записи; общая запись (s_group) действует для остальных персонажей
std::unordered_map<uint64_t, std::vector<Entry*>> s_group_own;
std::unordered_map<uint64_t, std::vector<Entry*>> s_actor_room;   // 06.10.2026: bodies of one room's enemy kinds (kinds\index.txt)
std::string s_base;
// 06.10.2026: подхват модов на лету (dc_meshrep_reload)
bool s_env_off = false;                       // DC_MODS_MESHES=0
std::atomic<int> s_rl_state{0};               // 0 — нет, 1 — список читается в своём потоке, 2 — готов к подмене
std::atomic<bool> s_rl_again{false};          // папка менялась, пока читался список: ещё раз после подмены
std::unique_ptr<MeshSet> s_rl_set;            // готовый список (поток отдаёт при 1 -> 2)
struct Retired { std::unique_ptr<Entry> e; uint32_t after; };
std::vector<Retired> s_retired;               // снятые с подмены: живут ещё несколько сборок (их могут рисовать снимки)
uint32_t s_otc = 0;                           // очистки списка игры — часы для снятых

uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}
const uint64_t kFnv0 = 0xCBF29CE484222325ull;

// 06.10.2026: отметка папки модели — имена, размеры и время всех её файлов (модель, .bin, .mtl, картинки рядом)
uint64_t dir_stamp(const std::wstring& path) {
    const size_t sl = path.find_last_of(L"\\/");
    const std::wstring dir = sl == std::wstring::npos ? std::wstring() : path.substr(0, sl + 1);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    uint64_t s = kFnv0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        s = fnv(s, (const uint8_t*)fd.cFileName, wcslen(fd.cFileName) * sizeof(wchar_t));
        s = fnv(s, (const uint8_t*)&fd.ftLastWriteTime, sizeof fd.ftLastWriteTime);
        s = fnv(s, (const uint8_t*)&fd.nFileSizeLow, sizeof fd.nFileSizeLow);
        s = fnv(s, (const uint8_t*)&fd.nFileSizeHigh, sizeof fd.nFileSizeHigh);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return s | 1u;
}

// 06.10.2026 (2): склейка вершин для видеокарты (бывшая часть gpu_build) — в потоке разбора. Ключ склейки — вершина
// целиком с номером МАТЕРИАЛА (номер картинки растра станет известен потом, в кадре игры: gpu_build); вершины материалов
// без картинки не идут, как и прежде. Плоская таблица с открытой адресацией вместо unordered_map<string> (строка на каждую
// вершину).
uint64_t gv_hash(const GVtx& g) {
    uint64_t w[6];
    memcpy(w, &g, sizeof w);
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 6; i++) { h ^= w[i]; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 31; }
    return h;
}
void gpu_prep(Entry* e) {
    e->prepped = false;
    if (!gpu_env()) return;
    const RMesh& m = e->mesh;
    const int nv = m.nverts();
    const size_t nt = m.idx.size() / 3;
    if (!nv || !nt) return;
    std::vector<int> vmat((size_t)nv, -1);
    for (size_t t = 0; t < nt; t++)
        for (int k = 0; k < 3; k++) vmat[m.idx[t * 3 + (size_t)k]] = m.tmat[t];
    std::vector<char> muse(m.mats.size(), 0);
    for (size_t i = 0; i < m.mats.size(); i++) muse[i] = m.mats[i].tex >= 0 && (size_t)m.mats[i].tex < m.texs.size();
    std::vector<GVtx> out;
    out.reserve((size_t)nv);
    std::vector<uint32_t> remap((size_t)nv, UINT32_MAX);
    size_t cap = 16;
    while (cap < (size_t)nv * 2) cap <<= 1;
    std::vector<uint32_t> tab(cap, UINT32_MAX);
    float rad = 0.f;
    for (int v = 0; v < nv; v++) {
        const int mi = vmat[(size_t)v];
        if (mi < 0 || !muse[(size_t)mi]) continue;
        const RMat& mt = m.mats[(size_t)mi];
        GVtx g;
        memset(&g, 0, sizeof g);
        for (int k = 0; k < 3; k++) { g.p[k] = m.pos[(size_t)v * 3 + (size_t)k]; g.n[k] = m.nrm[(size_t)v * 3 + (size_t)k]; }
        g.uv[0] = m.uv[(size_t)v * 2]; g.uv[1] = m.uv[(size_t)v * 2 + 1];
        float ws = 0.f;
        for (int k = 0; k < 4; k++) ws += m.jw[(size_t)v * 4 + (size_t)k];
        int best = 0;
        for (int k = 0; k < 4; k++) {
            g.j[k] = m.jb[(size_t)v * 4 + (size_t)k];
            const float w = ws > 0.f ? m.jw[(size_t)v * 4 + (size_t)k] / ws : (k == 0 ? 1.f : 0.f);
            g.w[k] = (uint8_t)lrintf(w * 255.f);
            if (m.jw[(size_t)v * 4 + (size_t)k] > m.jw[(size_t)v * 4 + (size_t)best]) best = k;
        }
        if (!(g.w[0] | g.w[1] | g.w[2] | g.w[3])) g.w[best] = 255;
        for (int k = 0; k < 3; k++) {
            const float vc = m.col.empty() ? 1.f : m.col[(size_t)v * 4 + (size_t)k];
            const float c = vc * mt.col[k];
            g.c[k] = (uint8_t)lrintf(std::min(1.f, std::max(0.f, c)) * 255.f);
        }
        g.c[3] = 255;
        g.slot = (float)mi;
        const int b = g.j[best];
        if (b < 64 && m.jhave[b]) {
            const float dx = g.p[0] - m.jpos[b][0], dy = g.p[1] - m.jpos[b][1], dz = g.p[2] - m.jpos[b][2];
            rad = std::max(rad, sqrtf(dx * dx + dy * dy + dz * dz));
        }
        size_t h = (size_t)gv_hash(g) & (cap - 1);
        for (;;) {
            const uint32_t t = tab[h];
            if (t == UINT32_MAX) { tab[h] = (uint32_t)out.size(); remap[(size_t)v] = (uint32_t)out.size(); out.push_back(g); break; }
            if (!memcmp(&out[t], &g, sizeof g)) { remap[(size_t)v] = t; break; }
            h = (h + 1) & (cap - 1);
        }
    }
    std::vector<uint32_t>().swap(tab);
    std::vector<uint32_t> idx;
    idx.reserve(nt * 3);
    for (size_t t = 0; t < nt; t++) {
        const uint32_t a = remap[m.idx[t * 3]], b = remap[m.idx[t * 3 + 1]], c = remap[m.idx[t * 3 + 2]];
        if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) continue;
        idx.push_back(a); idx.push_back(b); idx.push_back(c);
    }
    if (out.empty() || idx.empty()) return;
    if (rad <= 0.f) {                              // без суставов — от центра модели
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (auto& g : out) for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], g.p[k]); hi[k] = std::max(hi[k], g.p[k]); }
        rad = 0.5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    }
    e->rad = rad;
    e->gpv = std::move(out);
    e->gpi = std::move(idx);
    e->prepped = true;
}

// 06.10.2026: the crosshair's body of a character from mods. A model of up to kPickTris triangles is its own body; a bigger one
// is thinned by vertex clustering (one vertex per grid cell, its bones; triangles that stay whole, once each), so testing it
// in the game's frame stays cheap for a model of millions of triangles.
const size_t kPickTris = 6000;
void pick_build(Entry* e) {
    e->pk_pos.clear(); e->pk_jw.clear(); e->pk_jb.clear(); e->pk_idx.clear();
    const RMesh& m = e->mesh;
    const int nv = m.nverts();
    const size_t nt = m.idx.size() / 3;
    if (e->kind != 0 || !nv || !nt || m.jb.size() < (size_t)nv * 4 || m.jw.size() < (size_t)nv * 4) return;
    auto add = [&](int v) {
        for (int c = 0; c < 3; c++) e->pk_pos.push_back(m.pos[(size_t)v * 3 + (size_t)c]);
        for (int c = 0; c < 4; c++) { e->pk_jb.push_back(m.jb[(size_t)v * 4 + (size_t)c]); e->pk_jw.push_back(m.jw[(size_t)v * 4 + (size_t)c]); }
    };
    if (nt <= kPickTris) {
        for (int v = 0; v < nv; v++) add(v);
        e->pk_idx = m.idx;
        return;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int v = 0; v < nv; v++)
        for (int c = 0; c < 3; c++) { lo[c] = std::min(lo[c], m.pos[(size_t)v * 3 + (size_t)c]); hi[c] = std::max(hi[c], m.pos[(size_t)v * 3 + (size_t)c]); }
    const float ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
    if (!(ext > 0.f)) return;
    std::vector<uint32_t> remap((size_t)nv);
    for (int g = 40; ; g = g * 3 / 4) {
        e->pk_pos.clear(); e->pk_jw.clear(); e->pk_jb.clear(); e->pk_idx.clear();
        const float cell = ext / (float)g;
        std::unordered_map<uint64_t, uint32_t> cv;
        for (int v = 0; v < nv; v++) {
            uint64_t key = 0;
            for (int c = 0; c < 3; c++) key = (key << 21) | (uint64_t)((int)((m.pos[(size_t)v * 3 + (size_t)c] - lo[c]) / cell) & 0x1FFFFF);
            auto it = cv.find(key);
            if (it != cv.end()) { remap[(size_t)v] = it->second; continue; }
            const uint32_t k = (uint32_t)(e->pk_pos.size() / 3);
            cv.emplace(key, k);
            remap[(size_t)v] = k;
            add(v);
        }
        std::unordered_set<uint64_t> seen;
        for (size_t t = 0; t < nt; t++) {
            uint32_t a = remap[m.idx[t * 3]], b = remap[m.idx[t * 3 + 1]], c = remap[m.idx[t * 3 + 2]];
            if (a == b || b == c || a == c) continue;
            uint32_t s[3] = {a, b, c};
            std::sort(s, s + 3);
            if (!seen.insert(((uint64_t)s[0] << 42) | ((uint64_t)s[1] << 21) | (uint64_t)s[2]).second) continue;
            e->pk_idx.push_back(a); e->pk_idx.push_back(b); e->pk_idx.push_back(c);
        }
        if (e->pk_idx.size() / 3 <= kPickTris * 2 || g <= 8) break;
    }
}

void load_entry(Entry* e) {
    Gltf g;
    std::string err;
    const bool ok = is_ufbx_path(e->path) ? ufbx_build(e->path, e->kind == 0, e->node, e->mesh, err)
                                          : (gltf_load(e->path, g, err) && build_mesh(g, e->kind == 0, e->node, e->mesh, err));
    if (!ok) {
        e->err = err;
        fprintf(stderr, "mods: %s — not used: %s%c", e->rel.c_str(), err.c_str(), 10);
        e->state = -1;                           // последним: снятую с подмены запись после этого можно освободить
        return;
    }
    if (e->mesh.idx.empty()) fprintf(stderr, "mods: %s — empty: the game's mesh is hidden%c", e->rel.c_str(), 10);
    gpu_prep(e);                             // 06.10.2026 (2): склейка для видеокарты — здесь, до «готово», а не в кадре игры
    pick_build(e);                           // 06.10.2026: тело для прицела
    e->state = 2;
}

void read_index(const std::wstring& file, const std::string& base, MeshSet& S, const std::string& room = std::string()) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char kind[16] = {0};
        unsigned long long key = 0;
        int pos = 0;
        if (sscanf(line, "%15s %llx %n", kind, &key, &pos) < 2 || pos <= 0) continue;
        const bool actor = !strcmp(kind, "actor");
        if (!actor && strcmp(kind, "group") != 0) continue;
        std::string rest = line + pos;
        while (!rest.empty() && (rest.back() == '\n' || rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
        std::string node;
        std::vector<uint64_t> only;
        const size_t tab = rest.find('\t');                    // путь\tузел\tполя (в пути бывают пробелы)
        if (tab != std::string::npos) {
            std::string tail = rest.substr(tab + 1);
            rest.resize(tab);
            const size_t t2 = tail.find('\t');
            node = tail.substr(0, t2);
            std::string opts = t2 == std::string::npos ? std::string() : tail.substr(t2 + 1);
            for (size_t p = 0; p < opts.size();) {             // «actor=<hex>[,<hex>]» — только при модели персонажа с этим ключом
                size_t q = opts.find('\t', p);
                if (q == std::string::npos) q = opts.size();
                const std::string f = opts.substr(p, q - p);
                if (f.compare(0, 6, "actor=") == 0)
                    for (size_t a = 6; a < f.size();) {
                        size_t b = f.find(',', a);
                        if (b == std::string::npos) b = f.size();
                        only.push_back(strtoull(f.substr(a, b - a).c_str(), nullptr, 16));
                        a = b + 1;
                    }
                p = q + 1;
            }
        }
        if (rest.empty()) continue;
        if (!room.empty() && !actor) continue;                 // a room's own list: bodies of its enemy kinds only
        auto e = std::make_unique<Entry>();
        e->only_actor = only;
        e->only_room = room;
        e->kind = actor ? 0 : 1;
        e->key = key;
        e->rel = rest;
        e->node = node;
        e->path = aw(base) + u8w(uri_dec(rest));
        e->stamp = dir_stamp(e->path);
        if (!room.empty()) S.actor_room[key].push_back(e.get());
        else if (actor) S.actor[key] = e.get();
        else if (!only.empty()) S.group_own[key].push_back(e.get());
        else S.group[key] = e.get();
        S.ent.push_back(std::move(e));
    }
    fclose(f);
}

// ---------------------------------------------------------------- модель, положенная под своим именем (без конвертации)
// 06.10.2026, игрок: «просто ложит под нужным названием меш, и этот меш появляется в игре после перезапуска… ящик, модель с
// тысячами полигонов, машина — всё должно подхватываться и работать, как обычные модели». mods\names.txt (пишет Studio при
// каждом запуске и установщик) — каждый меш игры: ключ и путь <папка>\<имя>\<имя>.gltf. Есть там glTF/GLB — подмена; нетронутая
// копия выгрузки (отпечаток export_fnv в <имя>.json рядом: FNV-1a 64 <имя>.gltf + <имя>.bin) — пропуск, рядом своя модель — она.
uint64_t files_fnv(const std::wstring& dir, const std::wstring& id, std::initializer_list<const wchar_t*> exts) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (const wchar_t* ext : exts) {
        std::vector<uint8_t> b;
        if (!read_all(dir + id + ext, b)) continue;
        for (uint8_t c : b) { h ^= c; h *= 0x100000001B3ull; }
    }
    return h;
}

// нетронутая выгрузка Studio: отпечаток в <имя>.json («export_fnv» — <имя>.gltf + .bin, «export_fnv_obj» — <имя>.obj + .mtl,
// «export_fnv_fbx» — <имя>.fbx: 08.10.2026, Studio выгружает модели и в FBX; DC_MODS_FBX_FP=0 - нетронутый FBX берётся, как было)
bool untouched_export(const std::wstring& dir, const std::wstring& id, const char* field, std::initializer_list<const wchar_t*> exts) {
    std::vector<uint8_t> j;
    if (!read_all(dir + id + L".json", j)) return false;
    const std::string s(j.begin(), j.end());
    const size_t k = s.find(std::string("\"") + field + "\"");
    if (k == std::string::npos) return false;
    const size_t q = s.find('"', s.find(':', k) + 1);
    if (q == std::string::npos) return false;
    const uint64_t want = strtoull(s.c_str() + q + 1, nullptr, 16);
    return want && files_fnv(dir, id, exts) == want;
}

bool model_ext(const std::wstring& n) {
    const size_t dot = n.rfind(L'.');
    if (dot == std::wstring::npos) return false;
    const std::wstring x = n.substr(dot);
    return !_wcsicmp(x.c_str(), L".gltf") || !_wcsicmp(x.c_str(), L".glb") || !_wcsicmp(x.c_str(), L".fbx") || !_wcsicmp(x.c_str(), L".obj");
}

// модель папки: <имя>.gltf / .glb / .fbx / .obj, иначе любая такая модель папки; нетронутая выгрузка Studio не в счёт
bool model_in(const std::wstring& dir, const std::wstring& id, std::wstring& out) {
    const bool same_gltf = untouched_export(dir, id, "export_fnv", {L".gltf", L".bin"});
    const bool same_obj = untouched_export(dir, id, "export_fnv_obj", {L".obj", L".mtl"});
    static int fbx_fp = -1;
    if (fbx_fp < 0) { const char* e = getenv("DC_MODS_FBX_FP"); fbx_fp = (e && *e == '0') ? 0 : 1; }
    const bool same_fbx = fbx_fp && untouched_export(dir, id, "export_fnv_fbx", {L".fbx"});
    std::vector<std::wstring> cand;
    if (!same_gltf) cand.push_back(id + L".gltf");
    cand.push_back(id + L".glb");
    if (!same_fbx) cand.push_back(id + L".fbx");
    if (!same_obj) cand.push_back(id + L".obj");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring n = fd.cFileName;
            if (!model_ext(n)) continue;
            const size_t dot = n.rfind(L'.');
            if (!_wcsicmp(n.substr(0, dot).c_str(), id.c_str())) continue;   // <имя>.* — уже выше
            cand.push_back(n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto& c : cand)
        if (GetFileAttributesW((dir + c).c_str()) != INVALID_FILE_ATTRIBUTES) { out = dir + c; return true; }
    return false;
}

std::string rel_of(const std::wstring& model, const std::string& base) {   // путь от mods — для журнала
    int len = WideCharToMultiByte(CP_UTF8, 0, model.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(len > 0 ? len - 1 : 0, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, model.c_str(), -1, &u[0], len, nullptr, nullptr);
    const std::string ub = w8(aw(base));   /* 18.10: длина корня - в той же кодировке, что путь */
    return u.size() > ub.size() ? u.substr(ub.size()) : u;
}

struct NameLine { bool actor = false; uint64_t key = 0; std::string tag, rest, node; std::wstring dir, id; };

void read_names(const std::wstring& file, const std::string& base, MeshSet& S) {
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) return;
    char line[2048];
    std::vector<NameLine> L;
    while (fgets(line, sizeof line, f)) {
        char kind[16] = {0}, tag[16] = {0};
        unsigned long long key = 0;
        int pos = 0;
        if (line[0] == '#' || sscanf(line, "%15s %llx %15s %n", kind, &key, tag, &pos) < 3 || pos <= 0) continue;
        NameLine x;
        x.actor = !strcmp(kind, "actor");
        if (!x.actor && strcmp(kind, "group") != 0) continue;
        x.key = key;
        x.tag = tag;
        x.rest = line + pos;
        while (!x.rest.empty() && (x.rest.back() == '\n' || x.rest.back() == '\r' || x.rest.back() == ' ')) x.rest.pop_back();
        const size_t tab = x.rest.find('\t');
        if (tab != std::string::npos) { x.node = x.rest.substr(tab + 1); x.rest.resize(tab); }
        if (x.rest.empty()) continue;
        std::wstring wp = aw(base) + u8w(x.rest);
        for (auto& c : wp) if (c == L'/') c = L'\\';
        const size_t sl = wp.find_last_of(L'\\');
        if (sl == std::wstring::npos) continue;
        x.dir = wp.substr(0, sl + 1);
        x.id = wp.substr(sl + 1);
        const size_t dot = x.id.rfind(L'.');
        if (dot != std::wstring::npos) x.id.resize(dot);
        L.push_back(std::move(x));
    }
    fclose(f);
    int n = 0, own = 0;
    for (const NameLine& x : L) {
        // модель под своим именем — везде, где она есть (index.txt «Сконвертировать всё» важнее)
        std::wstring model;
        if (!(x.actor ? S.actor : S.group).count(x.key) && GetFileAttributesW(x.dir.c_str()) != INVALID_FILE_ATTRIBUTES &&
            model_in(x.dir, x.id, model)) {
            auto e = std::make_unique<Entry>();
            e->kind = x.actor ? 0 : 1;
            e->key = x.key;
            e->node = x.node;
            e->path = model;
            e->stamp = dir_stamp(model);
            e->rel = rel_of(model, base) + (x.node.empty() ? "" : " " + x.node);
            (x.actor ? S.actor : S.group)[x.key] = e.get();
            S.ent.push_back(std::move(e));
            n++;
        }
        // 06.10.2026, игрок: «убрал из mods Айю — у Регины пропала кисть». Вложение в руке (оружие с кистью костюма), положенное
        // в папку персонажа (characters\<костюм>\<in_hand_XX>\), — только для этого персонажа и уходит вместе с его папкой;
        // в weapons\<in_hand_XX>\ — для всех. Больше никаких правил «при заменённых костюмах».
        if (!x.actor || GetFileAttributesW(x.dir.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        for (const NameLine& h : L) {
            if (h.actor || h.tag != "in_hand") continue;
            const std::wstring hd = x.dir + h.id + L"\\";
            if (GetFileAttributesW(hd.c_str()) == INVALID_FILE_ATTRIBUTES || !model_in(hd, h.id, model)) continue;
            bool have = false;                         // своя строка index.txt для этого персонажа важнее
            auto it = S.group_own.find(h.key);
            if (it != S.group_own.end())
                for (Entry* o : it->second) have |= std::find(o->only_actor.begin(), o->only_actor.end(), x.key) != o->only_actor.end();
            if (have) continue;
            auto e = std::make_unique<Entry>();
            e->kind = 1;
            e->key = h.key;
            e->node = h.node;
            e->path = model;
            e->stamp = dir_stamp(model);
            e->only_actor.push_back(x.key);
            e->rel = rel_of(model, base) + (h.node.empty() ? "" : " " + h.node);
            S.group_own[h.key].push_back(e.get());
            S.ent.push_back(std::move(e));
            n++;
            own++;
        }
    }
    if (n) fprintf(stderr, "mods: %d model(s) put under their names (names.txt), %d of them for one character only%c", n, own, 10);
}

// все модели папки mods: index.txt в подпапках (кроме textures и screens — там свой формат) и names.txt
void scan_all(const std::string& base, MeshSet& S) {
    const std::wstring wb = aw(base);
    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW((wb + L"*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            if (!_wcsicmp(fd.cFileName, L"textures") || !_wcsicmp(fd.cFileName, L"screens")) continue;
            read_index(wb + fd.cFileName + L"\\index.txt", base, S);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    // 06.10.2026: rooms\<ROOM>\kinds\index.txt - bodies of enemy kinds put into that room, drawn with their own pictures there
    {
        const std::wstring rd = wb + L"rooms\\";
        HANDLE hr = FindFirstFileW((rd + L"*").c_str(), &fd);
        if (hr != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                std::string room;
                for (const wchar_t* p = fd.cFileName; *p; p++) room += (char)((*p >= L'a' && *p <= L'z') ? *p - 32 : *p);
                read_index(rd + fd.cFileName + L"\\kinds\\index.txt", base, S, room);
            } while (FindNextFileW(hr, &fd));
            FindClose(hr);
        }
    }
    read_names(wb + L"names.txt", base, S);     // модели, положенные под своим именем (без конвертации)
}

bool ready(Entry* e) {
    if (!e || e->state.load() != 2) return false;
    RMesh& m = e->mesh;
    if (!e->tex_reg) {                       // картинки — растру, один раз и только из потока эмуляции
        e->tex_reg = true;
        for (RTex& t : m.texs) {
            t.idx = dcvk_rep_tex_add(t.path.empty() ? nullptr : t.path.c_str(), t.png.empty() ? nullptr : t.png.data(), t.png.size(),
                                     t.rgba.empty() ? nullptr : t.rgba.data(), t.w, t.h, t.repeat ? 1 : 0);
            if (t.idx < 0) { e->state = -1; fprintf(stderr, "mods: %s — the video card cannot take its pictures%c", e->rel.c_str(), 10); return false; }
        }
    }
    bool ok = true;
    for (RTex& t : m.texs)
        if (dcvk_rep_tex_slot(t.idx) < 0) ok = false;      // первая просьба ставит декод в очередь
    return ok;
}

bool on() {
    if (s_on <= 0) return false;
    return dcvk_enabled() != 0;
}

// ---------------------------------------------------------------- ключи
// персонаж: заголовок модели hdr (смещение в RAM) — счётчики, кости (без указателей), нормали, полигоны (без страницы и палитры)
bool actor_key(const uint8_t* R, uint32_t hdr, uint64_t& key, int& nb) {
    if (hdr < 0x10000 || hdr + 24 > 0x200000) return false;
    const uint32_t vp = rd32(R + hdr) & 0x1FFFFFu, np = rd32(R + hdr + 4) & 0x1FFFFFu;
    const uint32_t tp = rd32(R + hdr + 8) & 0x1FFFFFu, qp = rd32(R + hdr + 12) & 0x1FFFFFu;
    const int nt = rd16(R + hdr + 16), nq = rd16(R + hdr + 18);
    nb = rd16(R + hdr + 20);
    if (nb < 1 || nb > 64 || np <= vp || (np - vp) % 8 || np - vp > 0x20000 || hdr + 24 + (uint32_t)nb * 20 > 0x200000) return false;
    if (tp + (uint32_t)nt * 16 > 0x200000 || qp + (uint32_t)nq * 20 > 0x200000) return false;
    uint64_t h = fnv(kFnv0, R + hdr + 16, 6);
    for (int b = 0; b < nb; b++) {
        h = fnv(h, R + hdr + 24 + b * 20, 8);
        h = fnv(h, R + hdr + 24 + b * 20 + 16, 4);
    }
    const uint32_t nv = (np - vp) / 8;
    for (uint32_t v = 0; v < nv; v++) h = fnv(h, R + np + v * 8, 6);
    for (int i = 0; i < nt; i++) { h = fnv(h, R + tp + i * 16, 10); h = fnv(h, R + tp + i * 16 + 12, 2); }
    for (int i = 0; i < nq; i++) { h = fnv(h, R + qp + i * 20, 10); h = fnv(h, R + qp + i * 20 + 12, 2); h = fnv(h, R + qp + i * 20 + 16, 4); }
    key = h;
    return true;
}
// группа: заголовок {треугольники, квады, nt, nq}; записи 40/52 (lit — 48/64), вершины — первые 8-байтовые слоты
bool group_key(const uint8_t* R, uint32_t hdr, int lit, uint64_t& key) {
    if (hdr < 0x10000 || hdr + 12 > 0x200000) return false;
    const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, quad = rd32(R + hdr + 4) & 0x1FFFFFu;
    const int nt = rd16(R + hdr + 8), nq = rd16(R + hdr + 10);
    const uint32_t ts = lit ? 48u : 40u, qs = lit ? 64u : 52u;
    if (nt + nq == 0 || nt > 8000 || nq > 8000) return false;
    if ((nt && tri + (uint32_t)nt * ts > 0x200000) || (nq && quad + (uint32_t)nq * qs > 0x200000)) return false;
    uint64_t h = fnv(kFnv0, R + hdr + 8, 4);
    for (int i = 0; i < nt; i++)
        for (int k = 0; k < 3; k++) h = fnv(h, R + tri + (uint32_t)i * ts + (uint32_t)k * 8, 6);
    for (int i = 0; i < nq; i++)
        for (int k = 0; k < 4; k++) h = fnv(h, R + quad + (uint32_t)i * qs + (uint32_t)k * 8, 6);
    key = h;
    return true;
}
// подпись группы для кэша ключа: заголовок, первая и последняя записи
uint32_t group_sig(const uint8_t* R, uint32_t hdr, int lit) {
    const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, quad = rd32(R + hdr + 4) & 0x1FFFFFu;
    const int nt = rd16(R + hdr + 8), nq = rd16(R + hdr + 10);
    uint64_t h = fnv(kFnv0, R + hdr, 12);
    if (nt && tri + 24 <= 0x200000) h = fnv(h, R + tri, 24);
    if (nq && quad + 32 <= 0x200000) h = fnv(h, R + quad, 32);
    const uint32_t qs = lit ? 64u : 52u;
    if (nq && quad + (uint32_t)nq * qs <= 0x200000) h = fnv(h, R + quad + (uint32_t)(nq - 1) * qs, 32);
    return (uint32_t)(h ^ (h >> 32));
}
struct GKey { uint32_t sig; uint64_t key; bool ok; };
std::unordered_map<uint64_t, GKey> s_gkey;   // (hdr << 1 | lit) → ключ
Entry* group_entry(const uint8_t* R, uint32_t hdr, int lit, uint64_t owner = 0) {
    const uint64_t ck = ((uint64_t)hdr << 1) | (uint64_t)(lit & 1);
    const uint32_t sig = group_sig(R, hdr, lit);
    auto it = s_gkey.find(ck);
    if (it == s_gkey.end() || it->second.sig != sig) {
        GKey k{sig, 0, false};
        k.ok = group_key(R, hdr, lit, k.key);
        it = s_gkey.insert_or_assign(ck, k).first;
        if (s_log && k.ok) {
            static int n = 0;
            if (n < 400) { n++; fprintf(stderr, "meshrep: group %06X %s key %016llx%s%c", hdr, lit ? "lit" : "room",
                                       (unsigned long long)k.key, s_group.count(k.key) || s_group_own.count(k.key) ? " — replaced" : "", 10); }
        }
    }
    if (!it->second.ok) return nullptr;
    if (owner) {
        auto o = s_group_own.find(it->second.key);
        if (o != s_group_own.end())
            for (Entry* x : o->second)
                if (std::find(x->only_actor.begin(), x->only_actor.end(), owner) != x->only_actor.end()) return x;
    }
    auto e = s_group.find(it->second.key);
    return e == s_group.end() ? nullptr : e->second;
}

// ---------------------------------------------------------------- оружие в руке без кисти Регины
// 06.10.2026, игрок: «люди будут скинить любых персонажей на скелеты Dino Crisis и класть в папку персонажа - папки in_hand
// они создавать не могут; унифицируй». Правой кисти в модели Регины нет (кость 3 на диске пустая): игра рисует её вложением
// кости вместе с оружием (меши in_hand_*, на диске 17). У персонажа из mods свои кисти - вложение рисуется без кисти Регины:
// полигоны её страницы 0x8E, центр текселей которых - в областях кисти этой страницы (виды перчатки, кулак, кожа запястья).
// Каждый костюм заливает в страницу свою картинку на те же места - правило одно на все четыре; проверено на всех 17 мешах
// (рендер с текстурами: оружие осталось, кисть убрана; P20 = ручная разметка, проверенная в игре). Рисуют сами циклы игры -
// по кускам без этих полигонов (dc_meshrep_attach_draw), RAM не меняется. Откат: DC_MODS_HANDLESS=0.
struct HandCut { uint32_t sig = 0; uint64_t tri = 0, quad = 0; bool ok = false; };
std::unordered_map<uint64_t, HandCut> s_handcut;   // (hdr << 1 | 1) → полигоны кисти вложения
const HandCut* s_cut_last = nullptr;              // от dc_meshrep_attach (вернул 2) к dc_meshrep_attach_draw
uint32_t s_cut_hdr = 0;
int handless_env() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_MODS_HANDLESS"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
// запись вложения (со светом): вершины по 8 Б {x, y, z, u, v}; центр текселей в прямоугольнике (включительно) - без деления
bool hand_texel(uint16_t tpage, const uint8_t* rec, int nv) {
    if ((tpage & 0x1FFu) != 0x8Eu) return false;
    static const int kHand[3][4] = {{0, 0, 48, 24}, {130, 130, 151, 151}, {84, 121, 128, 131}};
    int su = 0, sv = 0;
    for (int k = 0; k < nv; k++) { su += rec[k * 8 + 6]; sv += rec[k * 8 + 7]; }
    for (const auto& r : kHand)
        if (su >= r[0] * nv && su <= r[2] * nv && sv >= r[1] * nv && sv <= r[3] * nv) return true;
    return false;
}
const HandCut* hand_cut(const uint8_t* R, uint32_t hdr) {
    const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, quad = rd32(R + hdr + 4) & 0x1FFFFFu;
    const int nt = rd16(R + hdr + 8), nq = rd16(R + hdr + 10);
    if (nt > 64 || nq > 64 || nt + nq == 0) return nullptr;
    if ((nt && (tri < 0x10000 || tri + (uint32_t)nt * 48u > 0x200000)) || (nq && (quad < 0x10000 || quad + (uint32_t)nq * 64u > 0x200000)))
        return nullptr;
    const uint32_t sig = group_sig(R, hdr, 1);
    HandCut& c = s_handcut[((uint64_t)hdr << 1) | 1u];
    if (!c.ok || c.sig != sig) {
        c = HandCut();
        c.ok = true;
        c.sig = sig;
        for (int i = 0; i < nt; i++) { const uint8_t* r = R + tri + (uint32_t)i * 48u; if (hand_texel(rd16(r + 30), r, 3)) c.tri |= 1ull << i; }
        for (int i = 0; i < nq; i++) { const uint8_t* r = R + quad + (uint32_t)i * 64u; if (hand_texel(rd16(r + 38), r, 4)) c.quad |= 1ull << i; }
        static std::unordered_set<uint32_t> said;
        if ((c.tri || c.quad) && said.size() < 64 && said.insert(sig).second) {
            uint64_t key = 0;
            group_key(R, hdr, 1, key);
            int n = 0;
            for (uint64_t m = c.tri; m; m &= m - 1) n++;
            for (uint64_t m = c.quad; m; m &= m - 1) n++;
            fprintf(stderr, "mods: weapon in hand %016llx - drawn without Regina's hand (%d of %d polygons)%c", (unsigned long long)key, n,
                    nt + nq, 10);
        }
    }
    return (c.tri || c.quad) ? &c : nullptr;
}

// ---------------------------------------------------------------- снимки за сборку списка игры
struct Inst {
    Entry* e = nullptr;
    int kind = 0, lit = 0, player = 0, nb = 0, root = 0;
    std::vector<float> rt, tr, llm, lcm, rest;   // на кость: 9, 3, 9, 9, 3
    float bk[3] = {0, 0, 0}, rgbc[3] = {128, 128, 128};
    float D[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, T[3] = {0, 0, 0};
    int reaim = 0;
    uint16_t tpage = 0, clut = 0;
    uint32_t hdr = 0;
    uint64_t msig = 0;
    // 06.10.2026, игрок: «Айя рисуется поверх всего… затемнение и полосы кино должны действовать, как на Регину» — снимок
    // рисуется в обходе списка игры на месте своей модели (вход списка 0..1023: ближняя вершина модели игры), а не в его конце
    int slot = 1023;
    bool drawn = false;
};
std::vector<Inst> s_inst[2];
const uint32_t kOtBase = 0xB5850u;           // списки игры: 0x800B5850 + (байт 0x800B56F4) << 12, по 1024 входа

int ot_now(const uint8_t* R) { return R[0xB56F4] & 1; }
int ot_of(uint32_t addr) {
    const uint32_t a = addr & 0x1FFFFFu;
    if (a < kOtBase || a >= kOtBase + 0x2000u) return -1;
    return (int)((a - kOtBase) >> 12);
}

void mat_from_ctrl(const uint32_t* c, float* m9) {   // c0..c4: 3x3 int16 1.3.12 по строкам
    const int16_t v[9] = {(int16_t)(c[0] & 0xFFFF), (int16_t)(c[0] >> 16), (int16_t)(c[1] & 0xFFFF), (int16_t)(c[1] >> 16),
                          (int16_t)(c[2] & 0xFFFF), (int16_t)(c[2] >> 16), (int16_t)(c[3] & 0xFFFF), (int16_t)(c[3] >> 16),
                          (int16_t)(c[4] & 0xFFFF)};
    for (int i = 0; i < 9; i++) m9[i] = v[i] / 4096.f;
}
void mat_from_ram(const uint8_t* p, float* m9) {
    for (int i = 0; i < 9; i++) m9[i] = rs16(p + i * 2) / 4096.f;
}

// свет вершины как NCCT игры: IR = LLM·n (≥0), L = фон + LCM·IR, цвет = RGBC·L
uint32_t gte_light(const float* n, const float* llm, const float* lcm, const float* bk, const float* rgbc, const float* f) {
    float ir[3], c[3];
    for (int i = 0; i < 3; i++) {
        ir[i] = llm[i * 3] * n[0] + llm[i * 3 + 1] * n[1] + llm[i * 3 + 2] * n[2];
        ir[i] = ir[i] < 0.f ? 0.f : ir[i] > 7.999f ? 7.999f : ir[i];
    }
    for (int k = 0; k < 3; k++) {
        float l = bk[k] + lcm[k * 3] * ir[0] + lcm[k * 3 + 1] * ir[1] + lcm[k * 3 + 2] * ir[2];
        l = l < 0.f ? 0.f : l > 7.999f ? 7.999f : l;
        c[k] = rgbc[k] * l * f[k];
        c[k] = c[k] < 0.f ? 0.f : c[k] > 255.f ? 255.f : c[k];
    }
    return (uint32_t)(c[0] + 0.5f) | ((uint32_t)(c[1] + 0.5f) << 8) | ((uint32_t)(c[2] + 0.5f) << 16);
}

// вершины замены в виде камеры игры (+ перенос нашей камеры) и их цвета
std::vector<float> s_V;
std::vector<float> s_C;
void transform_inst(const Inst& in) {
    const RMesh& m = in.e->mesh;
    const int nv = m.nverts();
    s_V.resize((size_t)nv * 3);
    s_C.resize((size_t)nv * 3);
    for (int i = 0; i < nv; i++) {
        const float* p = &m.pos[(size_t)i * 3];
        const float* n = &m.nrm[(size_t)i * 3];
        float v[3] = {0, 0, 0}, nn[3] = {0, 0, 0};
        int dom = 0;
        if (in.kind == 0) {
            float best = -1.f;
            for (int k = 0; k < 4; k++) {
                const float w = m.jw[(size_t)i * 4 + (size_t)k];
                if (w <= 0.f) continue;
                int b = m.jb[(size_t)i * 4 + (size_t)k];
                if (b >= in.nb) b = in.root;
                const float* R = &in.rt[(size_t)b * 9];
                const float* t = &in.tr[(size_t)b * 3];
                const float* rp = &in.rest[(size_t)b * 3];
                const float l[3] = {p[0] - rp[0], p[1] - rp[1], p[2] - rp[2]};
                for (int r = 0; r < 3; r++) {
                    v[r] += w * (R[r * 3] * l[0] + R[r * 3 + 1] * l[1] + R[r * 3 + 2] * l[2] + t[r]);
                    nn[r] += w * (R[r * 3] * n[0] + R[r * 3 + 1] * n[1] + R[r * 3 + 2] * n[2]);
                }
                if (w > best) { best = w; dom = b; }
            }
        } else {
            const float* R = in.rt.data();
            const float* t = in.tr.data();
            for (int r = 0; r < 3; r++) {
                v[r] = R[r * 3] * p[0] + R[r * 3 + 1] * p[1] + R[r * 3 + 2] * p[2] + t[r];
                nn[r] = R[r * 3] * n[0] + R[r * 3 + 1] * n[1] + R[r * 3 + 2] * n[2];
            }
        }
        const float ln = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
        if (ln > 1e-8f) for (int r = 0; r < 3; r++) nn[r] /= ln;
        float vc[3] = {1.f, 1.f, 1.f};
        if (!m.col.empty()) for (int c = 0; c < 3; c++) vc[c] = m.col[(size_t)i * 4 + (size_t)c];
        if (in.kind == 0 || in.lit) {
            const uint32_t c = gte_light(nn, &in.llm[(size_t)dom * 9], &in.lcm[(size_t)dom * 9], in.bk, in.rgbc, vc);
            s_C[(size_t)i * 3] = (float)(c & 255); s_C[(size_t)i * 3 + 1] = (float)((c >> 8) & 255); s_C[(size_t)i * 3 + 2] = (float)((c >> 16) & 255);
        } else {
            for (int c = 0; c < 3; c++) s_C[(size_t)i * 3 + (size_t)c] = 128.f * vc[c];
        }
        if (in.reaim) {
            const float a[3] = {v[0], v[1], v[2]};
            for (int r = 0; r < 3; r++) v[r] = in.D[r * 3] * a[0] + in.D[r * 3 + 1] * a[1] + in.D[r * 3 + 2] * a[2] + in.T[r];
        }
        s_V[(size_t)i * 3] = v[0]; s_V[(size_t)i * 3 + 1] = v[1]; s_V[(size_t)i * 3 + 2] = v[2];
    }
}

uint32_t pack_col(const float* c, const float* f) {
    uint32_t o = 0;
    for (int k = 0; k < 3; k++) {
        float v = c[k] * f[k];
        v = v < 0.f ? 0.f : v > 255.f ? 255.f : v;
        o |= (uint32_t)(v + 0.5f) << (8 * k);
    }
    return o;
}

struct BV { float x, y, z, u, v, r, g, b; };
// треугольник в виде камеры → подрезка у глаза (near) → экран → растр (sw: в конце списка игры; direct: в обходе комнаты)
void emit_tri(const BV in[3], float near_z, float H, float OFX, float OFY, float wsx, float dox, float doy, int slot,
              uint16_t tpage, uint16_t clut, bool direct, float zmul, float flags) {
    BV poly[4];
    int np = 0;
    const bool allfront = in[0].z >= near_z && in[1].z >= near_z && in[2].z >= near_z;
    if (allfront) { poly[0] = in[0]; poly[1] = in[1]; poly[2] = in[2]; np = 3; }
    else {
        for (int k = 0; k < 3 && np < 4; k++) {
            const BV* a = &in[k];
            const BV* c = &in[(k + 1) % 3];
            const float da = a->z - near_z, dc = c->z - near_z;
            if (da >= 0.f) poly[np++] = *a;
            if ((da < 0.f) != (dc < 0.f) && np < 4) {
                const float tt = da / (da - dc);
                BV o;
                o.x = a->x + (c->x - a->x) * tt; o.y = a->y + (c->y - a->y) * tt; o.z = near_z;
                o.u = a->u + (c->u - a->u) * tt; o.v = a->v + (c->v - a->v) * tt;
                o.r = a->r + (c->r - a->r) * tt; o.g = a->g + (c->g - a->g) * tt; o.b = a->b + (c->b - a->b) * tt;
                poly[np++] = o;
            }
        }
        if (np < 3) return;
    }
    const uint16_t cx = (uint16_t)((clut & 0x3F) * 16), cy = (uint16_t)((clut >> 6) & 0x1FF);
    for (int f = 1; f + 1 < np; f++) {
        const BV* pv[3] = {&poly[0], &poly[f], &poly[f + 1]};
        float px[3], py[3], pw[3], uv[6];
        uint32_t cc[3];
        for (int k = 0; k < 3; k++) {
            const float z = pv[k]->z > 1e-3f ? pv[k]->z : 1e-3f;
            px[k] = OFX + H * pv[k]->x / z * wsx + dox;
            py[k] = OFY + H * pv[k]->y / z + doy;
            pw[k] = pv[k]->z * zmul;
            uv[k * 2] = pv[k]->u; uv[k * 2 + 1] = pv[k]->v;
            const float c3[3] = {pv[k]->r, pv[k]->g, pv[k]->b}, one[3] = {1.f, 1.f, 1.f};
            cc[k] = pack_col(c3, one);
        }
        if (direct) {
            float vv[3 * 13];
            for (int k = 0; k < 3; k++) {
                float* o = vv + k * 13;
                o[0] = px[k]; o[1] = py[k]; o[2] = 0.f; o[3] = 0.f;
                o[4] = (float)(cc[k] & 255); o[5] = (float)((cc[k] >> 8) & 255); o[6] = (float)((cc[k] >> 16) & 255); o[7] = 0.f;
                o[8] = (float)tpage; o[9] = (float)cx; o[10] = (float)cy; o[11] = flags; o[12] = pw[k];
            }
            dcvk_rep_override(slot, uv);
            dcvk_push_tri(vv, -1, 1);
            dcvk_rep_override(-1, nullptr);
        } else {
            int xi[3], yi[3];
            for (int k = 0; k < 3; k++) { xi[k] = (int)floorf(px[k] + 0.5f); yi[k] = (int)floorf(py[k] + 0.5f); }
            sw_set_pgxp_w(pw[0], pw[1], pw[2], 1);
            sw_set_pgxp_pos(px[0], py[0], px[1], py[1], px[2], py[2], 1);
            sw_draw_rep_triangle(xi[0], yi[0], cc[0], xi[1], yi[1], cc[1], xi[2], yi[2], cc[2], cx, cy, tpage, slot, uv);
        }
    }
}

// все треугольники замены: дальние первыми (как упорядочил бы список игры)
void draw_tris(const Inst& in, float H, float OFX, float OFY, float wsx, float dox, float doy, bool direct, float zmul, float flags) {
    const RMesh& m = in.e->mesh;
    const size_t nt = m.idx.size() / 3;
    static std::vector<std::pair<float, uint32_t>> ord;
    ord.clear();
    for (size_t t = 0; t < nt; t++) {
        const uint32_t a = m.idx[t * 3], b = m.idx[t * 3 + 1], c = m.idx[t * 3 + 2];
        const float z = s_V[(size_t)a * 3 + 2] + s_V[(size_t)b * 3 + 2] + s_V[(size_t)c * 3 + 2];
        if (z < -3.f * 65536.f) continue;
        ord.push_back({z, (uint32_t)t});
    }
    std::sort(ord.begin(), ord.end(), [](const std::pair<float, uint32_t>& x, const std::pair<float, uint32_t>& y) { return x.first > y.first; });
    const float near_z = direct ? 4.f : 32.f;
    for (const auto& o : ord) {
        const size_t t = o.second;
        const RMat& mt = m.mats[(size_t)m.tmat[t]];
        const int slot = dcvk_rep_tex_slot(m.texs[(size_t)mt.tex].idx);
        if (slot < 0) continue;
        BV v[3];
        for (int k = 0; k < 3; k++) {
            const uint32_t i = m.idx[t * 3 + (size_t)k];
            v[k].x = s_V[(size_t)i * 3]; v[k].y = s_V[(size_t)i * 3 + 1]; v[k].z = s_V[(size_t)i * 3 + 2];
            v[k].u = m.uv[(size_t)i * 2]; v[k].v = m.uv[(size_t)i * 2 + 1];
            v[k].r = s_C[(size_t)i * 3] * mt.col[0]; v[k].g = s_C[(size_t)i * 3 + 1] * mt.col[1]; v[k].b = s_C[(size_t)i * 3 + 2] * mt.col[2];
        }
        emit_tri(v, near_z, H, OFX, OFY, wsx, dox, doy, slot, in.tpage, in.clut, direct, zmul, flags);
    }
}

void say_once(Entry* e, const char* what) {
    if (e->said) return;
    e->said = 1;
    fprintf(stderr, "mods: %s — %s%c", e->rel.c_str(), what, 10);
}

// персонаж по контексту: заголовок модели, ключ, запись (кэш на сборку списка)
struct CtxKey { uint32_t hdr = 0; uint64_t key = 0; int nb = 0; bool ok = false; uint32_t serial = ~0u; Entry* e = nullptr; };
std::unordered_map<uint32_t, CtxKey> s_ctx;
uint32_t s_build_serial = 0;                 // растёт на каждой очистке списка игры (dc_meshrep_otc)
CtxKey* ctx_actor(const uint8_t* R, uint32_t ctx) {
    ctx &= 0x1FFFFFu;
    if (ctx < 0x10000 || ctx + 180 > 0x200000) return nullptr;
    CtxKey& c = s_ctx[ctx];
    const uint32_t hdr = rd32(R + ctx + 156) & 0x1FFFFFu;
    if (c.serial == s_build_serial && c.hdr == hdr) return c.ok ? &c : nullptr;
    c.serial = s_build_serial;
    c.hdr = hdr;
    c.ok = actor_key(R, hdr, c.key, c.nb);
    c.e = nullptr;
    if (!c.ok) return nullptr;
    auto it = s_actor.find(c.key);
    if (it != s_actor.end()) c.e = it->second;
    else if (!s_actor_room.empty()) {        // 06.10.2026: a body of this room's enemy kinds with its own pictures
        auto r = s_actor_room.find(c.key);
        if (r != s_actor_room.end()) {
            const char* rn = dc_room_name(dc_room_current());
            for (Entry* x : r->second)
                if (!_stricmp(x->only_room.c_str(), rn)) { c.e = x; break; }
        }
    }
    if (s_log) {
        static std::unordered_set<uint64_t> seen;
        if (seen.size() < 200 && seen.insert(c.key).second)
            fprintf(stderr, "meshrep: actor ctx %06X model %06X bones %d key %016llx%s%c", ctx, hdr, c.nb, (unsigned long long)c.key,
                    c.e ? " — replaced" : "", 10);
    }
    return &c;
}

// обход комнаты (rl_emit_poly): таблица групп блока комнаты 0x80100000 и что уже нарисовано в этом обходе
struct RG { uint32_t lo, hi, hdr; Entry* e; };
std::vector<RG> s_rg;
uint32_t s_rg_sig = 0;
std::unordered_set<uint64_t> s_walk_done;
void room_table(const uint8_t* R) {
    const uint32_t base = 0x100000u;
    uint64_t h = fnv(kFnv0, R + base, 0x30);
    h = fnv(h, R + base + 0x28, 24);
    const uint32_t sig = (uint32_t)(h ^ (h >> 32));
    if (sig == s_rg_sig) return;
    s_rg_sig = sig;
    s_rg.clear();
    const uint32_t p7 = rd32(R + base + 7 * 4), p8 = rd32(R + base + 8 * 4);
    const uint32_t nt = rd16(R + base + 0x24);
    if (p7 != 0x80100028u || p8 != p7 + nt * 40u) return;
    auto add = [&](uint32_t hdr) {
        const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, gnt = rd16(R + hdr + 8), gnq = rd16(R + hdr + 10);
        const uint32_t end = tri + gnt * 40u + gnq * 52u;
        RG g{tri, end, hdr, group_entry(R, hdr, 0)};
        s_rg.push_back(g);
        return end;
    };
    uint32_t pos = add(base + 0x1C);
    for (int guard = 0; guard < 4096 && pos + 12 <= 0x160000u; guard++) {
        const uint32_t a = rd32(R + pos), b = rd32(R + pos + 4);
        const uint32_t t = rd16(R + pos + 8), q = rd16(R + pos + 10);
        if (a != (0x80000000u | (pos + 12)) || b != a + t * 40u || t > 5000 || q > 5000 || t + q == 0) break;
        pos = add(pos);
    }
}

// СВОИ ПРОПОРЦИИ: кости игры в позе покоя стоят в суставах игры (rest = суммы смещений), у модели из mods — в своих суставах
// (jpos: кости с именами игры, поставленные моддером в суставы своего меша). Повороты костей — из анимации игры, места —
// по длинам модели: T'_корень = T_корень + R_корень·(J'_корень − J_корень), T'_f = T'_родитель + R_родитель·(J'_f − J'_родитель).
// rt/tr — матрицы костей кадра (вид камеры игры); на выходе tr = T', rest = J' (вершина: R_f·(p − J'_f) + T'_f).
// Кость без сустава в glTF — смещение игры от сустава родителя. nb_upto — сколько костей считать (вложения: до своей кости).
void retarget(const uint8_t* R, uint32_t hdr, const RMesh& m, int nb, const float* rt, float* tr, float* rest, int nb_upto = -1) {
    if (nb_upto < 0) nb_upto = nb;
    float J[64][3], Jn[64][3], T0[64][3];
    for (int f = 0; f < nb && f < 64; f++)
        for (int k = 0; k < 3; k++) { J[f][k] = rest[f * 3 + k]; T0[f][k] = tr[f * 3 + k]; }
    for (int f = 0; f < nb_upto && f < 64; f++) {
        const int par = R[hdr + 24 + f * 20 + 6] == 0xFF ? -1 : R[hdr + 24 + f * 20 + 6];
        for (int k = 0; k < 3; k++) Jn[f][k] = m.jhave[f] ? m.jpos[f][k] : (par >= 0 && par < f ? Jn[par][k] + J[f][k] - J[par][k] : J[f][k]);
        float d[3], o[3];
        const float* Rm;
        if (par >= 0 && par < f) {
            for (int k = 0; k < 3; k++) d[k] = Jn[f][k] - Jn[par][k];
            Rm = rt + par * 9;
            for (int r = 0; r < 3; r++) o[r] = tr[par * 3 + r] + Rm[r * 3] * d[0] + Rm[r * 3 + 1] * d[1] + Rm[r * 3 + 2] * d[2];
        } else {
            for (int k = 0; k < 3; k++) d[k] = Jn[f][k] - J[f][k];
            Rm = rt + f * 9;
            for (int r = 0; r < 3; r++) o[r] = T0[f][r] + Rm[r * 3] * d[0] + Rm[r * 3 + 1] * d[1] + Rm[r * 3 + 2] * d[2];
        }
        for (int k = 0; k < 3; k++) { tr[f * 3 + k] = o[k]; rest[f * 3 + k] = Jn[f][k]; }
    }
}

// ---------------------------------------------------------------- 06.10.2026: подхват модов на лету
// Папка mods изменилась (dc_mods.c): список моделей читается заново в своём потоке; модель, чья папка не менялась, остаётся
// загруженной (та же запись), новые и изменённые разбираются там же — игра до подмены рисует прежние. Подмена — в очистке
// списка игры (dc_meshrep_otc); снятые записи живут ещё несколько сборок (их снимки могут рисоваться), потом уходят вместе
// со своими картинками и местом в видеопамяти.
struct OldId { int kind; uint64_t key, stamp; std::wstring path; std::string node; std::vector<uint64_t> only; std::string room; Entry* e; };

void rl_start() {
    auto old = std::make_shared<std::vector<OldId>>();
    for (auto& x : s_ent) old->push_back({x->kind, x->key, x->stamp, x->path, x->node, x->only_actor, x->only_room, x.get()});
    s_rl_state = 1;
    const std::string base = s_base;
    std::thread([old, base] {
        auto S = std::make_unique<MeshSet>();
        scan_all(base, *S);
        for (auto& x : S->ent) {
            for (OldId& o : *old)
                if (o.e && o.kind == x->kind && o.key == x->key && o.stamp == x->stamp && o.path == x->path && o.node == x->node &&
                    o.only == x->only_actor && o.room == x->only_room) { x->reuse = o.e; o.e = nullptr; break; }
            if (!x->reuse) load_entry(x.get());
        }
        s_rl_set = std::move(S);
        s_rl_state = 2;
    }).detach();
}

void rl_swap() {
    std::unique_ptr<MeshSet> S = std::move(s_rl_set);
    s_rl_state = 0;
    if (!S) return;
    std::unordered_map<Entry*, std::unique_ptr<Entry>*> own;     // прежняя запись -> где она в s_ent
    for (auto& x : s_ent) own[x.get()] = &x;
    std::unordered_map<Entry*, Entry*> to;                        // запись нового списка -> та, что будет рисоваться
    std::vector<std::unique_ptr<Entry>> ent;
    int fresh = 0, kept = 0, gone = 0;
    for (auto& x : S->ent) {
        Entry* nx = x.get();
        Entry* use = nullptr;
        if (nx->reuse) {
            auto it = own.find(nx->reuse);
            if (it != own.end() && *it->second) { use = nx->reuse; ent.push_back(std::move(*it->second)); kept++; }
        } else { use = nx; ent.push_back(std::move(x)); fresh++; }
        to[nx] = use;
    }
    auto fix = [&](Entry* p) -> Entry* { auto it = to.find(p); return it == to.end() ? nullptr : it->second; };
    std::unordered_map<uint64_t, Entry*> actor, group;
    std::unordered_map<uint64_t, std::vector<Entry*>> group_own, actor_room;
    for (auto& kv : S->actor) if (Entry* p = fix(kv.second)) actor[kv.first] = p;
    for (auto& kv : S->group) if (Entry* p = fix(kv.second)) group[kv.first] = p;
    for (auto& kv : S->group_own) for (Entry* p : kv.second) if (Entry* q = fix(p)) group_own[kv.first].push_back(q);
    for (auto& kv : S->actor_room) for (Entry* p : kv.second) if (Entry* q = fix(p)) actor_room[kv.first].push_back(q);
    for (auto& x : s_ent)
        if (x) { x->dead = true; s_retired.push_back({std::move(x), s_otc + 6}); gone++; }
    s_ent = std::move(ent);
    for (auto& x : s_ent) x->reuse = nullptr;
    s_actor = std::move(actor);
    s_group = std::move(group);
    s_group_own = std::move(group_own);
    s_actor_room = std::move(actor_room);
    s_ctx.clear();                                                // персонажи и группы комнаты — по новым записям
    s_rg.clear();
    s_rg_sig = 0;
    s_on = s_ent.empty() ? 0 : 1;
    fprintf(stderr, "mods: meshes picked up - %d new or changed, %d kept, %d gone%c", fresh, kept, gone, 10);
    if (s_rl_again.exchange(false)) rl_start();
}

// снятые записи: прошло несколько сборок и поток разбора с ними закончил — уходят
void rl_reap() {
    for (size_t i = 0; i < s_retired.size();) {
        Retired& r = s_retired[i];
        if ((int32_t)(s_otc - r.after) < 0 || r.e->state.load() == 0) { i++; continue; }
        for (RTex& t : r.e->mesh.texs) if (t.idx >= 0) dcvk_rep_tex_drop(t.idx);
        if (r.e->gpu >= 0) dcvk_mesh_drop(r.e->gpu);
        s_retired.erase(s_retired.begin() + (std::ptrdiff_t)i);
    }
}

}  // namespace

// ==================================================================== вход из игры
extern "C" void dc_meshrep_boot(const char* mods) {
    if (s_on >= 0) return;
    s_on = 0;
    const char* e = getenv("DC_MODS_MESHES");
    if ((e && *e == '0') || !mods) { s_env_off = true; return; }
    const char* lg = getenv("DC_MESHREP_LOG");
    s_log = lg ? atoi(lg) : 0;
    s_base = mods;
    MeshSet S;
    scan_all(s_base, S);
    if (S.ent.empty()) return;
    s_on = 1;
    int na = 0, ng = 0;
    for (auto& x : S.ent) (x->kind == 0 ? na : ng)++;
    fprintf(stderr, "mods: meshes from mods (glTF, FBX, OBJ) — %d characters, %d groups%c", na, ng, 10);
    std::vector<Entry*> todo;
    for (auto& x : S.ent) todo.push_back(x.get());
    s_ent = std::move(S.ent);
    s_actor = std::move(S.actor);
    s_group = std::move(S.group);
    s_group_own = std::move(S.group_own);
    s_actor_room = std::move(S.actor_room);
    // разбор glTF — в своём потоке (картинки отдаются растру потом, из эмуляции). 06.10.2026: по своему списку указателей —
    // подхват на лету меняет s_ent; снятую с подмены запись поток пропускает, освобождается она после него (rl_reap)
    std::thread([todo] {
        for (Entry* x : todo) {
            if (x->dead.load()) { x->state = -1; continue; }
            load_entry(x);
        }
    }).detach();
}

// 06.10.2026: папка mods изменилась (dc_mods.c, поток эмуляции) — список моделей перечитывается в своём потоке
extern "C" void dc_meshrep_reload(const char* mods) {
    if (s_env_off || s_on < 0 || !mods) return;
    if (s_base.empty()) s_base = mods;
    if (s_rl_state.load() != 0) { s_rl_again = true; return; }   // читается: ещё раз после подмены
    rl_start();
}

// место снимка в списке игры (вход 0..1023, 0 — ближний). Персонаж: проектор уже посчитал вход каждой вершины модели игры
// (пул 0x800C6780, по 4 Б на вершину; сборщик func_80075770 кладёт треугольник по дальней из трёх) — берём ближнюю вершину.
static int actor_slot(const uint8_t* R, uint32_t hdr) {
    const uint32_t vp = rd32(R + hdr) & 0x1FFFFFu, np = rd32(R + hdr + 4) & 0x1FFFFFu;
    if (np <= vp) return 1023;
    const uint32_t nv = std::min<uint32_t>((np - vp) / 8u, 2048u);
    int best = 1023;
    for (uint32_t i = 0; i < nv; i++) {
        const uint32_t v = rd32(R + 0xC6780u + i * 4u);
        if (v < (uint32_t)best) best = (int)v;
    }
    return best;
}
// группа: глубины её вершин по матрице GTE, вход — глубина >> сдвиг списка (байт +33 контекста кадра, как в проекторе)
extern "C" uint32_t psx_read_word(uint32_t addr);
static int group_slot(const uint8_t* R, uint32_t hdr, int lit, const float* rt, const float* tr) {
    const uint32_t cx = psx_read_word(0x1F800000u) & 0x1FFFFFu;
    const int sh = (cx >= 0x10000 && cx + 64 < 0x200000) ? (R[cx + 33] & 31) : 2;
    const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, quad = rd32(R + hdr + 4) & 0x1FFFFFu;
    const int nt = rd16(R + hdr + 8), nq = rd16(R + hdr + 10);
    const uint32_t ts = lit ? 48u : 40u, qs = lit ? 64u : 52u;
    float zmin = 1e30f;
    auto vz = [&](uint32_t o) {
        if (o < 0x10000 || o + 6 > 0x200000) return;
        const float x = (float)rs16(R + o), y = (float)rs16(R + o + 2), z = (float)rs16(R + o + 4);
        const float zz = rt[6] * x + rt[7] * y + rt[8] * z + tr[2];
        if (zz < zmin) zmin = zz;
    };
    for (int i = 0; i < nt && i < 4096; i++) for (int k = 0; k < 3; k++) vz(tri + (uint32_t)i * ts + (uint32_t)k * 8u);
    for (int i = 0; i < nq && i < 4096; i++) for (int k = 0; k < 4; k++) vz(quad + (uint32_t)i * qs + (uint32_t)k * 8u);
    if (!(zmin < 1e29f)) return 1023;
    const int s2 = zmin <= 0.f ? 0 : (int)zmin >> sh;
    return s2 < 0 ? 0 : s2 > 1023 ? 1023 : s2;
}

// 06.10.2026 (player: "the dinosaurs of other kinds I added are not detected - the reticle does not go red, they cannot be
// killed"): the game's assembler does not run for a creature drawn from mods, so it put no polygons where the crosshair looks.
// Its own triangles (pick_build) go through the same skinning, camera and projection as its drawing, and the crosshair (the
// projection centre, as pt_renderer.c tests the game's polygons) is tested against them; the nearest depth goes to
// pt_renderer.c. Only while aiming - nothing else asks.
extern int g_dc_aiming;                      // main.cpp
static int pick_in_tri(float px, float py, const float* a, const float* b, const float* c) {
    const float d1 = (px - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (py - b[1]);
    const float d2 = (px - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (py - c[1]);
    const float d3 = (px - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (py - a[1]);
    const bool neg = d1 < 0.f || d2 < 0.f || d3 < 0.f, pos = d1 > 0.f || d2 > 0.f || d3 > 0.f;
    return !(neg && pos);
}
static void pick_note(const Inst& in, uint32_t cx) {
    if (!g_dc_aiming) return;
    const Entry* e = in.e;
    const float H = g_pt_proj_h, OFX = g_pt_proj_ofx, OFY = g_pt_proj_ofy;
    const int n = (int)(e->pk_pos.size() / 3);
    if (!(H > 0.f) || !n || e->pk_jb.size() < (size_t)n * 4 || e->pk_jw.size() < (size_t)n * 4 || e->pk_idx.empty()) return;
    const float wsx = dc_ws_sq();
    static std::vector<float> S;                 // per vertex: screen x, y, depth
    S.resize((size_t)n * 3);
    for (int i = 0; i < n; i++) {
        const float* p = &e->pk_pos[(size_t)i * 3];
        float v[3] = {0.f, 0.f, 0.f};
        for (int k = 0; k < 4; k++) {
            const float w = e->pk_jw[(size_t)i * 4 + (size_t)k];
            if (w <= 0.f) continue;
            int b = e->pk_jb[(size_t)i * 4 + (size_t)k];
            if (b >= in.nb) b = in.root;
            const float* R = &in.rt[(size_t)b * 9];
            const float* t = &in.tr[(size_t)b * 3];
            const float* rp = &in.rest[(size_t)b * 3];
            const float l[3] = {p[0] - rp[0], p[1] - rp[1], p[2] - rp[2]};
            for (int r = 0; r < 3; r++) v[r] += w * (R[r * 3] * l[0] + R[r * 3 + 1] * l[1] + R[r * 3 + 2] * l[2] + t[r]);
        }
        if (in.reaim) {
            const float a[3] = {v[0], v[1], v[2]};
            for (int r = 0; r < 3; r++) v[r] = in.D[r * 3] * a[0] + in.D[r * 3 + 1] * a[1] + in.D[r * 3 + 2] * a[2] + in.T[r];
        }
        float* o = &S[(size_t)i * 3];
        o[2] = v[2];
        if (v[2] > 32.f) { o[0] = OFX + H * wsx * v[0] / v[2]; o[1] = OFY + H * v[1] / v[2]; }
    }
    float best = 0.f;
    const size_t nt = e->pk_idx.size() / 3;
    for (size_t t = 0; t < nt; t++) {
        const uint32_t ia = e->pk_idx[t * 3], ib = e->pk_idx[t * 3 + 1], ic = e->pk_idx[t * 3 + 2];
        if (ia >= (uint32_t)n || ib >= (uint32_t)n || ic >= (uint32_t)n) continue;
        const float *a = &S[(size_t)ia * 3], *b = &S[(size_t)ib * 3], *c = &S[(size_t)ic * 3];
        if (!(a[2] > 32.f) || !(b[2] > 32.f) || !(c[2] > 32.f)) continue;
        if (!pick_in_tri(OFX, OFY, a, b, c)) continue;
        const float d = std::min(a[2], std::min(b[2], c[2]));
        if (best == 0.f || d < best) best = d;
    }
    dc_aim_modpick_note(cx, best);
    static int lg = -1;                          // DC_AIM_MODPICK_LOG=1: where its body is on the screen, ~2 lines a second
    if (lg < 0) { const char* v = getenv("DC_AIM_MODPICK_LOG"); lg = (v && *v == '1') ? 1 : 0; }
    if (lg) {
        static uint32_t lc[16];
        static int lk[16];
        int j = 0;
        while (j < 15 && lc[j] && lc[j] != cx) j++;
        lc[j] = cx;
        if ((++lk[j] % 30) == 1) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f, z0 = 1e30f;
            for (int i = 0; i < n; i++) {
                const float* o = &S[(size_t)i * 3];
                if (!(o[2] > 32.f)) continue;
                x0 = std::min(x0, o[0]); x1 = std::max(x1, o[0]); y0 = std::min(y0, o[1]); y1 = std::max(y1, o[1]); z0 = std::min(z0, o[2]);
            }
            fprintf(stderr, "modpick: actor %05X verts %d tris %d screen x %.0f..%.0f y %.0f..%.0f near %.0f, crosshair %.0f,%.0f -> %s %.0f%c",
                    cx, n, (int)nt, x0, x1, y0, y1, z0, OFX, OFY, best > 0.f ? "ON" : "off", best, 10);
        }
    }
}

// проектор, перед сборщиком пакетов персонажа: 1 — модель заменена, сборщик игры не зовётся
extern "C" int dc_meshrep_actor(CPUState* cpu, uint32_t ctx) {
    if (!on() || (s_actor.empty() && s_actor_room.empty())) return 0;
    const uint8_t* R = memory_get_ram_ptr();
    CtxKey* c = ctx_actor(R, ctx);
    if (!c || !c->e || !ready(c->e)) return 0;
    Entry* e = c->e;
    const uint32_t cx = ctx & 0x1FFFFFu, hdr = c->hdr;
    Inst in;
    in.e = e;
    in.kind = 0;
    in.player = cx == 0xB928Cu;
    in.nb = c->nb;
    in.hdr = hdr;
    in.rt.resize((size_t)in.nb * 9); in.tr.resize((size_t)in.nb * 3);
    in.llm.resize((size_t)in.nb * 9); in.lcm.resize((size_t)in.nb * 9); in.rest.resize((size_t)in.nb * 3);
    uint32_t node = rd32(R + cx + 76);
    for (int f = 0; f < in.nb; f++) {
        const uint8_t* N = node_at(R, node);
        if (!N) return 0;
        mat_from_ram(N, &in.rt[(size_t)f * 9]);
        for (int k = 0; k < 3; k++) in.tr[(size_t)f * 3 + (size_t)k] = (float)(int32_t)rd32(N + 20 + k * 4);
        mat_from_ram(N + 108, &in.llm[(size_t)f * 9]);
        mat_from_ram(N + 140, &in.lcm[(size_t)f * 9]);
        node = rd32(N + 52);
    }
    int root = -1;
    for (int f = 0; f < in.nb; f++) {          // покой костей: суммы смещений от корня
        const uint8_t* b = R + hdr + 24 + f * 20;
        const int par = b[6] == 0xFF ? -1 : b[6];
        const float off[3] = {(float)rs16(b), (float)rs16(b + 2), (float)rs16(b + 4)};
        for (int k = 0; k < 3; k++)
            in.rest[(size_t)f * 3 + (size_t)k] = (par >= 0 && par < f ? in.rest[(size_t)par * 3 + (size_t)k] : 0.f) + off[k];
        if (par < 0 && root < 0) root = f;
    }
    in.root = root < 0 ? 0 : root;
    retarget(R, hdr, e->mesh, in.nb, in.rt.data(), in.tr.data(), in.rest.data());
    for (int k = 0; k < 3; k++) in.bk[k] = (float)(int32_t)cpu->gte_ctrl[13 + k] / 4096.f;
    const uint32_t rgbc = rd32(R + cx + 176);
    in.rgbc[0] = (float)(rgbc & 255); in.rgbc[1] = (float)((rgbc >> 8) & 255); in.rgbc[2] = (float)((rgbc >> 16) & 255);
    in.reaim = dc_nots_get_delta(in.D, in.T) ? 1 : 0;
    if (!in.player) pick_note(in, cx);          // 06.10.2026: the crosshair sees it (the game's polygons are not built)
    const uint32_t tp = rd32(R + hdr + 8) & 0x1FFFFFu, qp = rd32(R + hdr + 12) & 0x1FFFFFu;
    if (rd16(R + hdr + 16)) { in.tpage = rd16(R + tp + 14); in.clut = rd16(R + tp + 10); }
    else if (rd16(R + hdr + 18)) { in.tpage = rd16(R + qp + 14); in.clut = rd16(R + qp + 10); }
    in.slot = actor_slot(R, hdr);
    s_inst[ot_now(R)].push_back(std::move(in));
    say_once(e, "the character is drawn from the mod");
    return 1;
}

// группа: hdr — заголовок {треугольники, квады, nt, nq}; arr — массив, с которым позвана листовая функция (0 — не проверять);
// lit — записи с нормалями (вложения костей). 1 — заменена, отрисовка игры пропускается.
static int group_snap(CPUState* cpu, uint32_t hdr, uint32_t arr, int lit, uint64_t owner) {
    if (!on() || (s_group.empty() && s_group_own.empty())) return 0;
    const uint8_t* R = memory_get_ram_ptr();
    hdr &= 0x1FFFFFu;
    if (hdr < 0x10000 || hdr + 12 > 0x200000) return 0;
    if (arr) {
        const uint32_t a = arr & 0x1FFFFFu;
        if (a != (rd32(R + hdr) & 0x1FFFFFu) && a != (rd32(R + hdr + 4) & 0x1FFFFFu)) return 0;
    }
    Entry* e = group_entry(R, hdr, lit, owner);
    if (!e || !ready(e)) return 0;
    const uint32_t* c = cpu->gte_ctrl;
    uint64_t ms = fnv(kFnv0, (const uint8_t*)c, 32);
    ms = fnv(ms, (const uint8_t*)&hdr, 4);
    std::vector<Inst>& L = s_inst[ot_now(R)];
    for (const Inst& x : L)                    // квады и треугольники одной группы с одной матрицей — один раз
        if (x.kind == 1 && x.hdr == hdr && x.msig == ms) return 1;
    Inst in;
    in.e = e;
    in.kind = 1;
    in.lit = lit;
    in.nb = 1;
    in.hdr = hdr;
    in.msig = ms;
    in.rt.resize(9); in.tr.resize(3); in.llm.resize(9); in.lcm.resize(9);
    mat_from_ctrl(c, in.rt.data());
    for (int k = 0; k < 3; k++) in.tr[(size_t)k] = (float)(int32_t)c[5 + k];
    if (lit) {
        mat_from_ctrl(c + 8, in.llm.data());
        mat_from_ctrl(c + 16, in.lcm.data());
        for (int k = 0; k < 3; k++) in.bk[k] = (float)(int32_t)c[13 + k] / 4096.f;
        const uint32_t rgbc = cpu->gte_data[6];
        in.rgbc[0] = (float)(rgbc & 255); in.rgbc[1] = (float)((rgbc >> 8) & 255); in.rgbc[2] = (float)((rgbc >> 16) & 255);
    }
    in.reaim = dc_nots_get_delta(in.D, in.T) ? 1 : 0;
    const uint32_t tri = rd32(R + hdr) & 0x1FFFFFu, quad = rd32(R + hdr + 4) & 0x1FFFFFu;
    // страница и палитра записи: без света — +24/+26 (квад +32/+34), со светом — в пустых полусловах нормалей (+30/+38, квад +38/+46)
    if (rd16(R + hdr + 8)) { in.tpage = rd16(R + tri + (lit ? 30u : 24u)); in.clut = rd16(R + tri + (lit ? 38u : 26u)); }
    else { in.tpage = rd16(R + quad + (lit ? 38u : 32u)); in.clut = rd16(R + quad + (lit ? 46u : 34u)); }
    in.slot = group_slot(R, hdr, lit, in.rt.data(), in.tr.data());
    L.push_back(std::move(in));
    say_once(e, "the mesh is drawn from the mod");
    return 1;
}

extern "C" int dc_meshrep_group(CPUState* cpu, uint32_t hdr, uint32_t arr, int lit) { return group_snap(cpu, hdr, arr, lit, 0); }

// вложение кости (func_80075094, в проекторе): hdr — группа вложения, node — узел кости (матрица), ctx — персонаж. Персонаж
// заменён моделью со своими пропорциями — вложение (кисть с оружием) встаёт в сустав модели (перенос GTE), игра рисует его там;
// само вложение тоже может быть заменено (группа). 1 — заменено, отрисовка игры пропускается.
extern "C" int dc_meshrep_attach(CPUState* cpu, uint32_t hdr, uint32_t node, uint32_t ctx) {
    if (!on()) return 0;
    uint64_t owner = 0;                          // своё вложение персонажа (папка костюма, actor=) важнее общего
    if (!s_group_own.empty()) {
        CtxKey* c = ctx_actor(memory_get_ram_ptr(), ctx);
        if (c) owner = c->key;
    }
    bool from_mod = false;                       // персонаж рисуется из mods (как в dc_meshrep_actor): кисть Регины не нужна
    if (!s_actor.empty() || !s_actor_room.empty()) {
        const uint8_t* R = memory_get_ram_ptr();
        CtxKey* c = ctx_actor(R, ctx);
        if (c && c->e && c->e->state.load() == 2) {
            from_mod = ready(c->e);
            const uint32_t cx = ctx & 0x1FFFFFu, nd0 = rd32(R + cx + 76) & 0x1FFFFFFFu, want = node & 0x1FFFFFFFu;
            int f = -1;
            uint32_t nd = nd0;
            for (int i = 0; i < c->nb && i < 64; i++) {
                if (nd == want) { f = i; break; }
                const uint8_t* N = node_at(R, nd);
                if (!N) break;
                nd = rd32(N + 52) & 0x1FFFFFFFu;
            }
            float rt[64 * 9] = {}, tr[64 * 3] = {}, rest[64 * 3] = {};
            if (f >= 0) {
                nd = nd0;
                for (int i = 0; i <= f; i++) {
                    const uint8_t* N = node_at(R, nd);
                    if (!N) { f = -1; break; }
                    mat_from_ram(N, rt + i * 9);
                    for (int k = 0; k < 3; k++) tr[i * 3 + k] = (float)(int32_t)rd32(N + 20 + k * 4);
                    nd = rd32(N + 52) & 0x1FFFFFFFu;
                }
            }
            if (f >= 0) {
                for (int i = 0; i < c->nb && i < 64; i++) {
                    const uint8_t* b = R + c->hdr + 24 + i * 20;
                    const int par = b[6] == 0xFF ? -1 : b[6];
                    const float off[3] = {(float)rs16(b), (float)rs16(b + 2), (float)rs16(b + 4)};
                    for (int k = 0; k < 3; k++) rest[i * 3 + k] = (par >= 0 && par < i ? rest[par * 3 + k] : 0.f) + off[k];
                }
                retarget(R, c->hdr, c->e->mesh, c->nb, rt, tr, rest, f + 1);
                for (int k = 0; k < 3; k++) cpu->gte_ctrl[5 + k] = (uint32_t)(int32_t)lrintf(tr[f * 3 + k]);
            }
        }
    }
    if (group_snap(cpu, hdr, 0, 1, owner)) return 1;
    // 06.10.2026: своего вложения нет - оружие игры без кисти Регины (hand_cut); 2 - хук зовёт dc_meshrep_attach_draw
    if (from_mod && handless_env()) {
        const uint32_t h = hdr & 0x1FFFFFu;
        if (h >= 0x10000 && h + 12 <= 0x200000) {
            const HandCut* hc = hand_cut(memory_get_ram_ptr(), h);
            if (hc) { s_cut_last = hc; s_cut_hdr = h; return 2; }
        }
    }
    return 0;
}

// циклы вложения кости игры (func_80075094): треугольники и квады записей со светом, a0 - записи, a1 - их число, курсор пакетов
// в t9 (его продолжает следующий вызов, func_80075094 возвращает его в 0x1F800004), список и его сдвиг - t8 и s4 (только чтение).
// Зовём их на кусках подряд идущих записей без кисти - то же, что сделала бы игра, минус эти полигоны. Регистры выложены
// хуком (PSX_SPILL до, PSX_RELOAD после).
extern "C" void func_80075484(CPUState* cpu);
extern "C" void func_8007511C(CPUState* cpu);
extern "C" void dc_meshrep_attach_draw(CPUState* cpu, uint32_t hdr) {
    const uint8_t* R = memory_get_ram_ptr();
    const uint32_t h = hdr & 0x1FFFFFu;
    const HandCut* hc = (s_cut_last && s_cut_hdr == h) ? s_cut_last : nullptr;   // не тот заголовок - рисуем всё, как игра
    s_cut_last = nullptr;
    const uint32_t tri = rd32(R + h), quad = rd32(R + h + 4);
    const int nt = rd16(R + h + 8), nq = rd16(R + h + 10);
    auto runs = [&](uint32_t base, int n, uint32_t size, uint64_t cut, void (*fn)(CPUState*), uint32_t ra) {
        int i = 0;
        while (i < n) {
            if (i < 64 && ((cut >> i) & 1u)) { i++; continue; }
            int j = i + 1;
            while (j < n && !(j < 64 && ((cut >> j) & 1u))) j++;
            cpu->gpr[4] = base + (uint32_t)i * size;
            cpu->gpr[5] = (uint32_t)(j - i);
            cpu->gpr[31] = ra;
            fn(cpu);
            i = j;
        }
    };
    runs(tri, nt, 48u, hc ? hc->tri : 0, func_80075484, 0x800750E4u);
    runs(quad, nq, 64u, hc ? hc->quad : 0, func_8007511C, 0x800750F4u);
}

// очистка списка игры (DMA 6): новая сборка — прежние снимки этого списка больше не нужны
extern "C" void dc_meshrep_otc(uint32_t madr) {
    s_otc++;
    if (s_rl_state.load() == 2) rl_swap();       // 06.10.2026: подхват на лету — новый список моделей с новой сборкой
    if (!s_retired.empty()) rl_reap();
    if (s_on <= 0) return;
    const int o = ot_of(madr);
    if (s_log) {
        static int n = 0;
        if (n < 40) { n++; fprintf(stderr, "meshrep: otc %06X list %d build %d%c", madr & 0x1FFFFFu, o, (int)(memory_get_ram_ptr()[0xB56F4] & 1), 10); }
    }
    if (o < 0) return;
    s_inst[o].clear();
    s_build_serial++;
}

// ---------------------------------------------------------------- меш на видеокарте (06.10.2026)
// Игрок: «мод не должен тормозить даже на сильно устаревших ПК — 60 кадров». Модель уходит в видеопамять один раз (вершины
// позы покоя склеены: позиция, нормаль, место в картинке, кости, веса, цвет вершины × цвет материала, номер картинки), за кадр
// — экземпляр (gpu_vk_raster.cpp, dc_mesh_common.glsl). Процессор вершин больше не трогает.
// GVtx — выше, у Entry (06.10.2026 (2): склейку готовит поток разбора, gpu_prep)
int gpu_env() {
    static int on = -1;
    if (on < 0) { const char* e = getenv("DC_MESH_GPU"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
void gpu_build(Entry* e) {
    e->gpu = -1;
    if (!gpu_env() || !dcvk_mesh_ok() || !e->prepped) return;
    // 06.10.2026 (2): вершины склеены в потоке разбора (gpu_prep, в slot — номер материала); здесь — номер картинки растра
    // (ready(e): все картинки модели уже на месте) и загрузка в видеопамять
    const RMesh& m = e->mesh;
    std::vector<float> mslot(m.mats.size(), -1.f);
    for (size_t i = 0; i < m.mats.size(); i++)
        if (m.mats[i].tex >= 0 && (size_t)m.mats[i].tex < m.texs.size()) mslot[i] = (float)dcvk_rep_tex_slot(m.texs[(size_t)m.mats[i].tex].idx);
    for (GVtx& g : e->gpv) g.slot = mslot[(size_t)g.slot];
    e->gpu = dcvk_mesh_add(e->gpv.data(), (int)e->gpv.size(), e->gpi.data(), (int)e->gpi.size());
    if (e->gpu >= 0)
        fprintf(stderr, "mods: %s — %d triangles on the video card (%d vertices)%c", e->rel.c_str(), (int)(e->gpi.size() / 3), (int)e->gpv.size(), 10);
    std::vector<GVtx>().swap(e->gpv);            // копия в видеопамяти; при отказе — путь процессора по e->mesh
    std::vector<uint32_t>().swap(e->gpi);
    e->prepped = false;
}

// экземпляр в кадр: 9 vec4 + кости (dc_mesh_common.glsl). occ — персонаж (тень, рамка Регины)
bool gpu_draw(const Inst& in, float H, float OFX, float OFY, float wsx, float dox, float doy, float zmul, float flags) {
    Entry* e = in.e;
    if (e->gpu == -2 && ready(e)) gpu_build(e);
    if (e->gpu < 0) return false;
    float I[36];
    memset(I, 0, sizeof I);
    I[0] = OFX + dox; I[1] = OFY + doy; I[2] = H; I[3] = H * wsx;
    I[4] = zmul; I[5] = 0.f; I[6] = in.kind == 0 ? 0.f : 1.f; I[7] = in.lit ? 1.f : 0.f;
    const uint16_t cx = (uint16_t)((in.clut & 0x3F) * 16), cy = (uint16_t)((in.clut >> 6) & 0x1FF);
    I[8] = (float)in.tpage; I[9] = (float)cx; I[10] = (float)cy; I[11] = flags;
    for (int k = 0; k < 3; k++) { I[12 + k] = in.bk[k]; I[16 + k] = in.rgbc[k]; }
    I[15] = (float)in.nb; I[19] = (float)in.root;
    static const float id9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, z3[3] = {0, 0, 0};
    const float* D = in.reaim ? in.D : id9;
    const float* T = in.reaim ? in.T : z3;
    for (int r = 0; r < 3; r++) { I[20 + r * 4] = D[r * 3]; I[21 + r * 4] = D[r * 3 + 1]; I[22 + r * 4] = D[r * 3 + 2]; I[23 + r * 4] = T[r]; }
    const int nb = std::max(1, std::min(in.nb, 64));
    static std::vector<float> B;
    B.assign((size_t)nb * 40, 0.f);
    for (int b = 0; b < nb; b++) {
        float* o = &B[(size_t)b * 40];
        for (int r = 0; r < 3; r++) {
            o[r * 4] = in.rt[(size_t)b * 9 + (size_t)r * 3]; o[r * 4 + 1] = in.rt[(size_t)b * 9 + (size_t)r * 3 + 1];
            o[r * 4 + 2] = in.rt[(size_t)b * 9 + (size_t)r * 3 + 2]; o[r * 4 + 3] = in.tr[(size_t)b * 3 + (size_t)r];
        }
        if (in.kind == 0 && in.rest.size() >= (size_t)(b + 1) * 3)
            for (int k = 0; k < 3; k++) o[12 + k] = in.rest[(size_t)b * 3 + (size_t)k];
        if (in.llm.size() >= (size_t)(b + 1) * 9 && in.lcm.size() >= (size_t)(b + 1) * 9)
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) {
                    o[16 + r * 4 + c] = in.llm[(size_t)b * 9 + (size_t)r * 3 + (size_t)c];
                    o[28 + r * 4 + c] = in.lcm[(size_t)b * 9 + (size_t)r * 3 + (size_t)c];
                }
    }
    if (!dcvk_mesh_draw(e->gpu, I, B.data(), nb, in.kind == 0 ? 1 : 0)) return false;
    if (in.kind == 0 && H > 0.f) {                 // тень (габарит окклюдера) и рамка Регины — по суставам с запасом rad
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (int b = 0; b < nb; b++) {
            float v[3] = {in.tr[(size_t)b * 3], in.tr[(size_t)b * 3 + 1], in.tr[(size_t)b * 3 + 2]};
            if (in.reaim) {
                const float a[3] = {v[0], v[1], v[2]};
                for (int r = 0; r < 3; r++) v[r] = D[r * 3] * a[0] + D[r * 3 + 1] * a[1] + D[r * 3 + 2] * a[2] + T[r];
            }
            if (!(v[2] > 32.f)) continue;
            const float sx = OFX + dox + H * wsx * v[0] / v[2], sy = OFY + doy + H * v[1] / v[2];
            const float rs = H * e->rad / v[2];
            bx0 = std::min(bx0, sx - rs); bx1 = std::max(bx1, sx + rs);
            by0 = std::min(by0, sy - rs); by1 = std::max(by1, sy + rs);
            const float fx[3] = {sx - rs, sx + rs, sx}, fy[3] = {sy + rs, sy + rs, sy - rs}, w[3] = {v[2] * zmul, v[2] * zmul, v[2] * zmul};
            dc_shadow_note_k(fx, fy, w, in.player ? 1 : 2, -1);
        }
        if (in.player && bx1 > bx0) dc_actor_bbox_note(bx0, by0, bx1, by1);
    }
    return true;
}

static bool s_walk_log = false;
// снимки списка o: все не нарисованные с местом не ближе входа idx (idx < 0 — все оставшиеся)
static void draw_list(int o, int idx) {
    int dox = 0, doy = 0;
    gpu_get_draw_offset(&dox, &doy);
    const float wsx = dc_ws_sq();
    const float H = g_pt_proj_h, OFX = g_pt_proj_ofx, OFY = g_pt_proj_ofy;
    if (!(H > 0.f)) return;
    bool any = false;
    for (Inst& in : s_inst[o]) {
        if (in.drawn || (idx >= 0 && in.slot < idx)) continue;
        in.drawn = true;
        if (s_log >= 2) { static int n = 0; if (n < 200) { n++; fprintf(stderr, "otwalk:   draw list %d kind %d slot %d at %d%c", o, in.kind, in.slot, idx, 10); } }
        if (!in.e || in.e->state.load() != 2) continue;
        if (in.player && dc_regina_hidden()) continue;   // 19.10.2026 вечер: CAMERA -> Hide Regina (memory.c) - и её меш из mods
        if (gpu_draw(in, H, OFX, OFY, wsx, (float)dox, (float)doy, 1.f,
                     1.f + (in.kind == 0 ? 8388608.f : 0.f) + (in.player ? 16.f : 0.f))) continue;   // 06.10.2026: на видеокарте
        if (!any) { sw_set_semi_transparency(0, 0); any = true; }
        transform_inst(in);
        g_sw_actor_prim = in.player ? 1 : 0;
        g_sw_char_prim = in.kind == 0 ? 1 : 0;
        g_sw_texclass = 0;
        draw_tris(in, H, OFX, OFY, wsx, (float)dox, (float)doy, false, 1.f, 1.f);
        if (s_log) {
            static int n = 0;
            if (n < 20) { n++;
                const RMesh& m = in.e->mesh;
                fprintf(stderr, "meshrep: drew kind %d slot %d at %d verts %d tris %d H %.0f OF %.0f,%.0f do %d,%d%c", in.kind, in.slot, idx,
                        m.nverts(), (int)(m.idx.size() / 3), H, OFX, OFY, dox, doy, 10); }
        }
    }
    if (any) {
        g_sw_actor_prim = 0;
        g_sw_char_prim = 0;
        sw_set_semi_transparency(0, 0);
    }
}

// начало передачи списка (DMA 2): снимки обоих списков ещё не нарисованы в этой передаче (список игры бывает отдан дважды)
extern "C" void dc_meshrep_dma_begin(void) {
    if (s_on <= 0) return;
    s_walk_log = s_log >= 2 && (!s_inst[0].empty() || !s_inst[1].empty());
    if (s_walk_log) {
        static int n = 0;
        if (n < 60) { n++; fprintf(stderr, "otwalk: --- begin, snaps list0 %d list1 %d%c", (int)s_inst[0].size(), (int)s_inst[1].size(), 10);
                      for (int o = 0; o < 2; o++) for (const Inst& in : s_inst[o]) fprintf(stderr, "otwalk:   snap list %d kind %d slot %d%c", o, in.kind, in.slot, 10); }
    }
    for (auto& L : s_inst) for (Inst& in : L) in.drawn = false;
}

// узел списка игры в передаче: a6 — адрес входа (0x800B5850 + n*0x1000 + 4*вход). Вход idx проходит раньше своих пакетов:
// снимок с местом idx встаёт перед пакетами того же входа, а всё ближнее (затемнение, полосы кино, интерфейс) — поверх него.
extern "C" void dc_meshrep_dma_at(uint32_t a6) {
    if (s_on <= 0 || a6 < kOtBase || a6 >= kOtBase + 0x2000u) return;
    const int o = (int)((a6 - kOtBase) >> 12);
    if (s_walk_log) {                            // разбор: порядок входов списка в передаче
        static int nlog = 0;
        static int last = -2, run0 = -2, cnt = 0;
        const int idx = (int)(((a6 - kOtBase) & 0xFFFu) >> 2) + o * 10000;
        if (nlog < 400) {
            if (idx != last - 1) { if (run0 != -2) { fprintf(stderr, "otwalk: %d..%d (%d)%c", run0, last, cnt, 10); nlog++; } run0 = idx; cnt = 0; }
            cnt++;
            last = idx;
        }
    }
    if (s_inst[o].empty()) return;
    draw_list(o, (int)(((a6 - kOtBase) & 0xFFFu) >> 2));
}

// конец передачи списка (DMA 2): что не встретилось в обходе (место не прошли) — в кадр поверх, как раньше
// ot_mask — какие списки игры прошла передача (бит n — список 0x800B5850 + n*0x1000; кадр отдаётся через малый список,
// который ведёт в основной)
extern "C" void dc_meshrep_dma(uint32_t ot_mask) {
    if (s_on <= 0) return;
    const int o = (ot_mask & 1) ? 0 : (ot_mask & 2) ? 1 : -1;
    if (s_log && o >= 0 && !s_inst[o].empty()) {
        static int n = 0;
        if (n < 12) { n++; fprintf(stderr, "meshrep: list %d snaps %d%c", o, (int)s_inst[o].size(), 10); }
    }
    if (s_walk_log) {
        static int n = 0;
        if (n < 60) { n++; int left = 0; if (o >= 0) for (const Inst& in : s_inst[o]) left += !in.drawn;
                      fprintf(stderr, "otwalk: --- end mask %u list %d left %d%c", ot_mask, o, left, 10); }
    }
    if (o < 0 || s_inst[o].empty()) return;
    draw_list(o, -1);
}

// наш обход комнаты: начало
extern "C" void dc_meshrep_room_walk(void) {
    s_walk_done.clear();
    if (!on() || (s_group.empty() && s_group_own.empty())) return;
    room_table(memory_get_ram_ptr());
}

// полигон нашего обхода комнаты (rl_emit_poly): группа заменена — её меш (раз на группу и матрицу), 1 — полигон не рисовать
extern "C" int dc_meshrep_room_rec(const uint8_t* ram, uint32_t rec, const float* R, const float* TR, const float* D, const float* T,
                                   float h, float ofx, float ofy, float wsx, int world_space, int is_object) {
    if (s_on <= 0 || s_rg.empty() || ram != memory_get_ram_ptr()) return 0;
    const uint32_t a = rec & 0x1FFFFFu;
    if (a < s_rg.front().lo || a >= s_rg.back().hi) return 0;
    auto it = std::upper_bound(s_rg.begin(), s_rg.end(), a, [](uint32_t x, const RG& g) { return x < g.lo; });
    if (it == s_rg.begin()) return 0;
    --it;
    if (a >= it->hi || !it->e || !ready(it->e)) return 0;
    uint64_t ms = fnv(kFnv0, (const uint8_t*)R, 36);
    ms = fnv(ms, (const uint8_t*)TR, 12);
    ms = fnv(ms, (const uint8_t*)&it->hdr, 4);
    if (!s_walk_done.insert(ms).second) return 1;
    Inst in;
    in.e = it->e;
    in.kind = 1;
    in.nb = 1;
    in.rt.assign(R, R + 9);
    in.tr.assign(TR, TR + 3);
    in.llm.assign(9, 0.f); in.lcm.assign(9, 0.f);
    memcpy(in.D, D, sizeof in.D);
    memcpy(in.T, T, sizeof in.T);
    in.reaim = 1;
    const uint8_t* M = memory_get_ram_ptr();
    const uint32_t hdr = it->hdr;
    if (rd16(M + hdr + 8)) { const uint32_t t0 = rd32(M + hdr) & 0x1FFFFFu; in.tpage = rd16(M + t0 + 24); in.clut = rd16(M + t0 + 26); }
    else { const uint32_t q0 = rd32(M + hdr + 4) & 0x1FFFFFu; in.tpage = rd16(M + q0 + 32); in.clut = rd16(M + q0 + 34); }
    static float zb = -1.f;
    if (zb < 0.f) { const char* e2 = getenv("DC_ROOM_ZBIAS"); zb = e2 ? (float)atof(e2) : 1.005f; }
    if (!gpu_draw(in, h, ofx, ofy, wsx, 0.f, 0.f, (world_space && !is_object) ? zb : 1.f, 1.f)) {   // 06.10.2026: на видеокарте
        transform_inst(in);
        draw_tris(in, h, ofx, ofy, wsx, 0.f, 0.f, true, (world_space && !is_object) ? zb : 1.f, 1.f);
    }
    say_once(in.e, "the room mesh is drawn from the mod");
    return 1;
}
