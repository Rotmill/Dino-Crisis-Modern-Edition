// dc_movie.cpp — ролики MP4 из mods вместо роликов игры (05.10.2026).
//
// Игрок (04.10): «видео — вместо STR, в обычном формате, подменяем на нашей стороне, игра не знает»; 05.10: «доделай всё».
// Приложение Dino Crisis ME Studio выгружает ролик в source\movie\<ИМЯ>\<ИМЯ>.mp4 (30 кадров/с, звук — XA ролика с первого
// сектора). Положил такой файл (любого разрешения) в mods\movie\<ИМЯ>\ — игра показывает его:
//  * картинка: игра по-прежнему читает и декодирует свой STR (время, длина, пропуск — её), dc_mods.c по номеру кадра в
//    заголовке видеосектора задаёт время (dc_movie_at), этот модуль в своём потоке декодирует MP4 (Media Foundation) до
//    кадра этого времени, а показ (main.cpp) рисует его вместо 24-битного кадра из VRAM;
//  * звук: dc_movie_audio декодирует звуковую дорожку MP4 целиком при старте, dc_mods.c кладёт её на канал XA ролика
//    (та же подмена, что для речи из WAV).
// DC_MODS=0 / DC_MODS_MOVIE=0 — выключить (dc_mods.c).
//
// 17.11.2026: Linux - the same module over FFmpeg (libavformat/libavcodec/libswscale/libswresample) instead of Media
// Foundation: the Reader and the audio decode below have a POSIX twin; the thread, the timing and the exported calls
// are shared.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#else
#include "dc_platform.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace {

// ---------------------------------------------------------------- картинка
struct Frame { std::vector<uint32_t> px; int w = 0, h = 0; double t = -1; };

#ifdef _WIN32

// 18.10.2026 (разбор кода): Media Foundation - не статическим импортом, а при первом ролике MP4. В Windows N и KN её нет
// без Media Feature Pack, и exe с mfplat.dll/mfreadwrite.dll в таблице импорта там не запускался вовсе, даже без модов.
// Теперь библиотеки грузятся из System32 при первом ролике; нет их - ролик игры, строка в журнал.
typedef HRESULT (WINAPI* PfnMFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI* PfnMFCreateAttributes)(IMFAttributes**, UINT32);
typedef HRESULT (WINAPI* PfnMFCreateMediaType)(IMFMediaType**);
typedef HRESULT (WINAPI* PfnMFCreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**);
PfnMFCreateAttributes          pMFCreateAttributes = nullptr;
PfnMFCreateMediaType           pMFCreateMediaType = nullptr;
PfnMFCreateSourceReaderFromURL pMFCreateSourceReaderFromURL = nullptr;
bool g_mf_ok = false;

std::once_flag g_mf_once;
bool mf_start() {
    std::call_once(g_mf_once, [] {
        HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        PfnMFStartup start = plat ? (PfnMFStartup)(void*)GetProcAddress(plat, "MFStartup") : nullptr;
        pMFCreateAttributes = plat ? (PfnMFCreateAttributes)(void*)GetProcAddress(plat, "MFCreateAttributes") : nullptr;
        pMFCreateMediaType = plat ? (PfnMFCreateMediaType)(void*)GetProcAddress(plat, "MFCreateMediaType") : nullptr;
        pMFCreateSourceReaderFromURL = rw ? (PfnMFCreateSourceReaderFromURL)(void*)GetProcAddress(rw, "MFCreateSourceReaderFromURL") : nullptr;
        g_mf_ok = start && pMFCreateAttributes && pMFCreateMediaType && pMFCreateSourceReaderFromURL &&
                  SUCCEEDED(start(MF_VERSION, MFSTARTUP_LITE));
        if (!g_mf_ok)
            std::fprintf(stderr, "mods: Media Foundation is not available (Windows N/KN without the Media Feature Pack?) - "
                                 "the game's own videos are shown%c", 10);
    });
    return g_mf_ok;
}

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Reader {
    IMFSourceReader* r = nullptr;
    int w = 0, h = 0, stride = 0;
    bool eof = false;
    bool open(const std::wstring& path) {
        close();
        IMFAttributes* a = nullptr;
        if (!g_mf_ok || FAILED(pMFCreateAttributes(&a, 2))) return false;
        a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        HRESULT hr = pMFCreateSourceReaderFromURL(path.c_str(), a, &r);
        release(a);
        if (FAILED(hr)) { r = nullptr; return false; }
        r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        IMFMediaType* t = nullptr;
        if (FAILED(pMFCreateMediaType(&t))) { close(); return false; }
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
        release(t);
        if (FAILED(hr)) { close(); return false; }
        IMFMediaType* cur = nullptr;
        if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) { close(); return false; }
        UINT32 fw = 0, fh = 0;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &fw, &fh);
        UINT32 st = 0;
        if (FAILED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &st))) st = fw * 4;
        release(cur);
        w = (int)fw; h = (int)fh; stride = (int)(INT32)st;
        eof = false;
        return w > 0 && h > 0;
    }
    void close() { release(r); eof = false; }
    bool rewind() {
        if (!r) return false;
        PROPVARIANT pv; PropVariantInit(&pv);
        pv.vt = VT_I8; pv.hVal.QuadPart = 0;
        HRESULT hr = r->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);
        eof = false;
        return SUCCEEDED(hr);
    }
    // следующий кадр: пиксели BGRA сверху вниз и время (с)
    bool next(Frame& f) {
        if (!r || eof) return false;
        for (int guard = 0; guard < 64; guard++) {
            DWORD flags = 0; LONGLONG ts = 0;
            IMFSample* s = nullptr;
            if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) { eof = true; return false; }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { release(s); eof = true; return false; }
            if (!s) continue;
            IMFMediaBuffer* b = nullptr;
            if (FAILED(s->ConvertToContiguousBuffer(&b))) { release(s); continue; }
            BYTE* p = nullptr; DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                f.w = w; f.h = h; f.t = (double)ts / 1e7;
                f.px.resize((size_t)w * h);
                const int as = stride < 0 ? -stride : stride;
                for (int y = 0; y < h; y++) {
                    const BYTE* row = stride < 0 ? p + (size_t)(h - 1 - y) * as : p + (size_t)y * as;
                    if ((size_t)(row - p) + (size_t)w * 4 > len) break;
                    memcpy(&f.px[(size_t)y * w], row, (size_t)w * 4);
                }
                for (auto& c : f.px) c |= 0xFF000000u;      // RGB32: старший байт не задан
                b->Unlock();
            }
            release(b);
            release(s);
            return true;
        }
        return false;
    }
};

struct ComScope {
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};
bool movie_backend_start() { return mf_start(); }
#else
// ---------------------------------------------------------------- FFmpeg (Linux)
struct ComScope {};
bool movie_backend_start() { return true; }

std::string utf8_path(const std::wstring& w) {
    char p[MAX_PATH];
    return dc_posix_wpath(w.c_str(), p, sizeof p);
}

#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100)
#define DC_AV_CH_LAYOUT 1           // FFmpeg 5.1+: AVChannelLayout
#else
#define DC_AV_CH_LAYOUT 0
#endif

int codec_channels(const AVCodecContext* c) {
#if DC_AV_CH_LAYOUT
    return c->ch_layout.nb_channels;
#else
    return c->channels;
#endif
}

struct Reader {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* fr = nullptr;
    AVPacket* pkt = nullptr;
    int vs = -1;
    int w = 0, h = 0;
    bool eof = false, flushing = false;
    bool open(const std::wstring& path) {
        close();
        const std::string p = utf8_path(path);
        if (avformat_open_input(&fmt, p.c_str(), nullptr, nullptr) < 0) { fmt = nullptr; return false; }
        if (avformat_find_stream_info(fmt, nullptr) < 0) { close(); return false; }
        const AVCodec* codec = nullptr;
        vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (vs < 0 || !codec) { close(); return false; }
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, fmt->streams[vs]->codecpar) < 0) { close(); return false; }
        dec->thread_count = 0;                   // FFmpeg picks
        if (avcodec_open2(dec, codec, nullptr) < 0) { close(); return false; }
        fr = av_frame_alloc();
        pkt = av_packet_alloc();
        w = dec->width; h = dec->height;
        eof = false; flushing = false;
        return fr && pkt && w > 0 && h > 0;
    }
    void close() {
        if (sws) { sws_freeContext(sws); sws = nullptr; }
        if (fr) av_frame_free(&fr);
        if (pkt) av_packet_free(&pkt);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
        vs = -1; eof = false; flushing = false;
    }
    bool rewind() {
        if (!fmt) return false;
        const int r = av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        eof = false; flushing = false;
        return r >= 0;
    }
    // следующий кадр: пиксели BGRA сверху вниз и время (с)
    bool next(Frame& f) {
        if (!fmt || eof) return false;
        for (;;) {
            const int r = avcodec_receive_frame(dec, fr);
            if (r == 0) break;
            if (r == AVERROR_EOF) { eof = true; return false; }
            if (r != AVERROR(EAGAIN)) { eof = true; return false; }
            if (flushing) { eof = true; return false; }
            const int rd = av_read_frame(fmt, pkt);
            if (rd < 0) { flushing = true; avcodec_send_packet(dec, nullptr); continue; }
            if (pkt->stream_index == vs) avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
        }
        sws = sws_getCachedContext(sws, fr->width, fr->height, (AVPixelFormat)fr->format, fr->width, fr->height,
                                   AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) { av_frame_unref(fr); return false; }
        f.w = fr->width; f.h = fr->height;
        const int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
        const int64_t st = fmt->streams[vs]->start_time != AV_NOPTS_VALUE ? fmt->streams[vs]->start_time : 0;
        f.t = ts == AV_NOPTS_VALUE ? (f.t < 0 ? 0.0 : f.t + 1.0 / 30.0) : (double)(ts - st) * av_q2d(fmt->streams[vs]->time_base);
        if (f.t < 0) f.t = 0;
        f.px.resize((size_t)f.w * f.h);
        uint8_t* dst[4] = {(uint8_t*)f.px.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {f.w * 4, 0, 0, 0};
        sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, dst_stride);
        for (auto& c : f.px) c |= 0xFF000000u;
        w = f.w; h = f.h;
        av_frame_unref(fr);
        return true;
    }
};

// звуковая дорожка целиком: int16, первые два канала (или один), своя частота
bool decode_audio(const std::wstring& path, std::vector<int16_t>& out, int& out_ch, int& rate) {
    const std::string p = utf8_path(path);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, p.c_str(), nullptr, nullptr) < 0) return false;
    bool ok = false;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* fr = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    const AVCodec* codec = nullptr;
    int as = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0)
        as = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (as >= 0 && codec && fr && pkt && (dec = avcodec_alloc_context3(codec)) &&
        avcodec_parameters_to_context(dec, fmt->streams[as]->codecpar) >= 0 && avcodec_open2(dec, codec, nullptr) >= 0) {
        const int ch = codec_channels(dec);
        rate = dec->sample_rate;
        if (ch >= 1 && ch <= 8 && rate >= 4000) {
            out_ch = ch >= 2 ? 2 : 1;
#if DC_AV_CH_LAYOUT
            AVChannelLayout ol;
            av_channel_layout_default(&ol, out_ch);
            if (swr_alloc_set_opts2(&swr, &ol, AV_SAMPLE_FMT_S16, rate, &dec->ch_layout, dec->sample_fmt, rate, 0, nullptr) < 0)
                swr = nullptr;
#else
            const int64_t il = dec->channel_layout ? (int64_t)dec->channel_layout : av_get_default_channel_layout(ch);
            swr = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(out_ch), AV_SAMPLE_FMT_S16, rate, il,
                                     dec->sample_fmt, rate, 0, nullptr);
#endif
            if (swr && swr_init(swr) >= 0) {
                bool flushing = false;
                for (;;) {
                    const int r = avcodec_receive_frame(dec, fr);
                    if (r == 0) {
                        const int cap = swr_get_out_samples(swr, fr->nb_samples);
                        const size_t o = out.size();
                        out.resize(o + (size_t)cap * out_ch);
                        uint8_t* dst = (uint8_t*)&out[o];
                        const int got = swr_convert(swr, &dst, cap, (const uint8_t**)fr->extended_data, fr->nb_samples);
                        out.resize(o + (size_t)(got > 0 ? got : 0) * out_ch);
                        av_frame_unref(fr);
                        continue;
                    }
                    if (r != AVERROR(EAGAIN) || flushing) break;
                    if (av_read_frame(fmt, pkt) < 0) { flushing = true; avcodec_send_packet(dec, nullptr); continue; }
                    if (pkt->stream_index == as) avcodec_send_packet(dec, pkt);
                    av_packet_unref(pkt);
                }
                ok = !out.empty();
            }
        }
    }
    if (swr) swr_free(&swr);
    if (pkt) av_packet_free(&pkt);
    if (fr) av_frame_free(&fr);
    if (dec) avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ok;
}
#endif

std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_thr;
int g_req_id = -1;               // ролик, который сейчас читает игра (−1 — нет)
std::wstring g_req_path;
double g_req_t = 0;
ULONGLONG g_req_tick = 0;        // когда пришёл последний видеосектор
Frame g_show;                    // кадр для показа (под g_mu)
uint64_t g_show_serial = 0;

bool g_stop = false;             // выход из игры: поток заканчивает работу (под g_mu)

void worker() {
    ComScope com;
    movie_backend_start();
    Reader rd;
    int cur_id = -1;
    Frame have, ahead;          // показанный и следующий кадр
    bool ahead_ok = false;
    for (;;) {
        int id; std::wstring path; double t;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait_for(lk, std::chrono::milliseconds(15));
            if (g_stop) break;
            id = g_req_id; path = g_req_path; t = g_req_t;
            if (id >= 0 && GetTickCount64() - g_req_tick > 1500) { id = -1; g_req_id = -1; g_show.t = -1; }
        }
        if (id < 0) {
            if (cur_id >= 0) { rd.close(); cur_id = -1; ahead_ok = false; have.t = -1; }
            continue;
        }
        if (id != cur_id) {
            ahead_ok = false; have.t = -1;
            if (!rd.open(path)) {
                std::fprintf(stderr, "mods: movie %ls - cannot decode the video, the game's own is shown%c", path.c_str(), 10);
                cur_id = id;    // не пытаться снова, пока не сменится ролик
                rd.eof = true;
                continue;
            }
            cur_id = id;
            std::fprintf(stderr, "mods: movie %ls %dx%d - shown instead of the game's%c", path.c_str(), rd.w, rd.h, 10);
        }
        if (have.t >= 0 && t + 0.25 < have.t) {             // ролик пошёл сначала
            rd.rewind(); ahead_ok = false; have.t = -1;
        }
        bool changed = false;
        for (int k = 0; k < 8; k++) {                        // догнать время игры (не больше 8 кадров за проход)
            if (!ahead_ok) ahead_ok = rd.next(ahead);
            if (!ahead_ok) break;
            if (have.t >= 0 && ahead.t > t + 0.001) break;
            std::swap(have, ahead); ahead_ok = false; changed = true;
        }
        if (changed) {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_req_id == cur_id) { g_show = have; g_show_serial++; }
        }
    }
    rd.close();
}

// 18.10.2026 (разбор кода): поток роликов жил до конца процесса, и при выходе из игры после MP4-мода деструктор
// std::thread с живым потоком звал std::terminate - игра падала на выходе. Объект объявлен после g_thr, значит
// разрушается раньше него: останавливает и дожидается поток.
struct WorkerStop {
    ~WorkerStop() {
        { std::lock_guard<std::mutex> lk(g_mu); g_stop = true; }
        g_cv.notify_one();
        if (g_thr.joinable()) g_thr.join();
    }
} g_worker_stop;

}  // namespace

// ролик id (номер в таблице dc_mods.c) читается игрой; t — время его текущего кадра (с)
extern "C" void dc_movie_at(int id, const wchar_t* path, double t) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_thr.joinable()) g_thr = std::thread(worker);
    if (id != g_req_id) { g_req_id = id; g_req_path = path ? path : L""; g_show.t = -1; }
    g_req_t = t;
    g_req_tick = GetTickCount64();
    g_cv.notify_one();
}

// кадр подмены для показа (BGRA сверху вниз): NULL — подмены нет (ролик игры или не ролик). Копия — в буфер вызывающего.
extern "C" const uint32_t* dc_movie_frame(int* w, int* h) {
    static std::vector<uint32_t> out;
    static uint64_t got = ~0ull;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_req_id < 0 || g_show.t < 0 || g_show.px.empty() || GetTickCount64() - g_req_tick > 1500) return nullptr;
    if (got != g_show_serial) { out = g_show.px; got = g_show_serial; }
    *w = g_show.w; *h = g_show.h;
    return out.data();
}

// звуковая дорожка MP4 целиком: стерео/моно int16 на своей частоте (*ch, *rate); 0 — дорожки нет или не читается
extern "C" int dc_movie_audio(const wchar_t* path, int16_t** pcm, uint32_t* frames, int* ch, int* rate) {
    *pcm = nullptr; *frames = 0; *ch = 0; *rate = 0;
#ifndef _WIN32
    std::vector<int16_t> all;
    int oc = 0, sr = 0;
    if (!path || !decode_audio(path, all, oc, sr)) return 0;
    const uint32_t fr = (uint32_t)(all.size() / (size_t)oc);
    int16_t* out = (int16_t*)malloc(all.size() * 2);
    if (!out || !fr) { free(out); return 0; }
    memcpy(out, all.data(), all.size() * 2);
    *pcm = out; *frames = fr; *ch = oc; *rate = sr;
    return 1;
#else
    int ok = 0;
    std::thread th([&] {                                     // свой поток: COM-апартамент вызывающего не трогаем
        struct Com { Com() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); } ~Com() { CoUninitialize(); } } com;
        IMFSourceReader* r = nullptr;
        if (!mf_start() || FAILED(pMFCreateSourceReaderFromURL(path, nullptr, &r))) return;
        r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED(r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) { release(r); return; }
        IMFMediaType* t = nullptr;
        if (FAILED(pMFCreateMediaType(&t))) { release(r); return; }
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        HRESULT hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
        release(t);
        IMFMediaType* cur = nullptr;
        if (FAILED(hr) || FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) { release(r); return; }
        UINT32 c = 0, sr = 0;
        cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        release(cur);
        if (c < 1 || c > 8 || sr < 4000) { release(r); return; }
        std::vector<int16_t> all;
        for (;;) {
            DWORD flags = 0; LONGLONG ts = 0;
            IMFSample* s = nullptr;
            if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
            if (s) {
                IMFMediaBuffer* b = nullptr;
                if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                    BYTE* p = nullptr; DWORD len = 0;
                    if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                        const size_t n = len / 2;
                        const size_t o = all.size();
                        all.resize(o + n);
                        memcpy(&all[o], p, n * 2);
                        b->Unlock();
                    }
                    release(b);
                }
                release(s);
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        release(r);
        if (all.empty()) return;
        // больше двух каналов — берём первые два
        const uint32_t fr = (uint32_t)(all.size() / c);
        const int oc = c >= 2 ? 2 : 1;
        int16_t* out = (int16_t*)malloc((size_t)fr * oc * 2);
        if (!out) return;
        for (uint32_t i = 0; i < fr; i++)
            for (int k = 0; k < oc; k++) out[(size_t)i * oc + k] = all[(size_t)i * c + k];
        *pcm = out; *frames = fr; *ch = oc; *rate = (int)sr;
        ok = 1;
    });
    th.join();
    return ok;
#endif
}
