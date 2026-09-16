/* q_beep — see q_beep.h.  Adapted from zserge/beep (MIT, Serge Zaitsev 2020;
 * licence at docs/licenses/zserge-beep-LICENSE): an 8 kHz unsigned-8-bit sawtooth
 * on the default output, synchronous, the device opened on first use and kept.
 * peachq links NOTHING for audio: on Linux ALSA arrives by dlopen("libasound.so.2")
 * with locally declared entry points (the q_duckdb.c loader pattern), on macOS
 * AudioToolbox the same way, on Windows kernel32's Beep().  Any missing library
 * or failed open falls back to the terminal bell through the termbox stream
 * (or stdout), so a game on a headless box still gets feedback: 1b = a tone
 * was started, 0b = the bell (or a tone already playing — one at a time).
 * The tone plays on a detached thread that touches NOTHING of the engine —
 * no ray_t, no env, no console — only its copy of hz/ms and the audio handle. */
#define _GNU_SOURCE
#include "qlang/io/q_beep.h"
#include "qlang/io/q_termbox.h"    /* q_termbox_emit — the bell rides the session's stream */
#include "qlang/base/q_err.h"
#include "qlang/base/q_type.h"
#include "qlang/q_env.h"
#include "core/platform.h"
#include "lang/env.h"              /* ray_fn_vary */
#include "lang/eval.h"             /* RAY_FN_NONE */
#include <rayforce.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#if defined(RAY_OS_WINDOWS)
#include <windows.h>
#include <io.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

enum { BEEP_RATE = 8000, BEEP_CHUNK = 2400 };

#if defined(RAY_OS_WINDOWS)

static int beep_open(void) { return 1; }
static void beep_play(int hz, int ms) { if (hz >= 37) Beep((DWORD)hz, (DWORD)ms); }

#else

static unsigned char beep_phase;

static void beep_fill(unsigned char* buf, size_t n, int hz) {
    for (size_t i = 0; i < n; i++, beep_phase++) buf[i] = hz > 0 ? (unsigned char)(255u * beep_phase * (unsigned)hz / BEEP_RATE) : 0;
}

#endif

#if defined(__linux__)

/* libasound entry points, declared here so no ALSA header is needed */
static struct {
    int  state;   /* 0 untried, 1 loaded, 2 failed */
    void* pcm;
    int  (*open)(void** pcm, const char* name, int stream, int mode);
    int  (*set_params)(void* pcm, int format, int access, unsigned ch, unsigned rate, int resample, unsigned latency_us);
    int  (*prepare)(void* pcm);
    long (*writei)(void* pcm, const void* buf, unsigned long frames);
    int  (*recover)(void* pcm, int err, int silent);
} alsa;

static int alsa_load(void) {
    if (alsa.state) return alsa.state == 1;
    void* dl = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (dl) {
        *(void**)&alsa.open       = dlsym(dl, "snd_pcm_open");
        *(void**)&alsa.set_params = dlsym(dl, "snd_pcm_set_params");
        *(void**)&alsa.prepare    = dlsym(dl, "snd_pcm_prepare");
        *(void**)&alsa.writei     = dlsym(dl, "snd_pcm_writei");
        *(void**)&alsa.recover    = dlsym(dl, "snd_pcm_recover");
    }
    int ok = dl && alsa.open && alsa.set_params && alsa.prepare && alsa.writei && alsa.recover
          && alsa.open(&alsa.pcm, "default", 0, 0) == 0
          && alsa.set_params(alsa.pcm, 1, 3, 1, BEEP_RATE, 1, 20000) == 0;   /* U8, RW_INTERLEAVED, mono */
    if (!ok && dl) dlclose(dl);                  /* a failure latches: no re-probe per game beep */
    alsa.state = ok ? 1 : 2;
    return ok;
}

static int beep_open(void) { return alsa_load(); }

static void beep_play(int hz, int ms) {
    unsigned char buf[BEEP_CHUNK];
    long left = (long)ms * BEEP_RATE / 1000;
    alsa.prepare(alsa.pcm);
    while (left > 0) {
        size_t n = left < BEEP_CHUNK ? (size_t)left : BEEP_CHUNK;
        beep_fill(buf, n, hz);
        long r = alsa.writei(alsa.pcm, buf, n);
        if (r < 0 && alsa.recover(alsa.pcm, (int)r, 1) < 0) return;
        left -= r > 0 ? r : (long)n;
    }
}

#elif defined(__APPLE__)

/* UNVERIFIED (no mac in the tree): the reference's AudioUnit render-callback path over a dlopen'd
 * AudioToolbox, the framework's structs and constants declared locally; libdispatch is in libSystem
 * (its SDK header — hand prototypes conflicted with Xcode 26's, the v0.83 macOS build break). */
#include <dispatch/dispatch.h>
typedef struct { uint32_t type, sub, manu, flags, mask; } au_desc;
typedef struct { double rate; uint32_t fmt, flags, bpp, fpp, bpf, cpf, bpc, res; } au_stream;
typedef struct { uint32_t ch, bytes; void* data; } au_buf;
typedef struct { uint32_t n; au_buf b[1]; } au_buflist;
typedef int32_t (*au_render)(void*, uint32_t*, const void*, uint32_t, uint32_t, au_buflist*);
typedef struct { au_render proc; void* ref; } au_cb;

static struct {
    int state;
    void *stopped, *playing, *done;
    int hz, samples, counter;
    void* (*find)(void*, const au_desc*);
    int32_t (*inst)(void*, void**);
    int32_t (*setprop)(void*, uint32_t, uint32_t, uint32_t, const void*, uint32_t);
    int32_t (*init)(void*);
    int32_t (*start)(void*);
} au;

static int32_t au_tone_cb(void* ref, uint32_t* flags, const void* ts, uint32_t bus, uint32_t frames, au_buflist* io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    unsigned char* buf = io->b[0].data;
    for (uint32_t i = 0; i < frames; i++) {
        while (au.counter == 0) { dispatch_semaphore_wait(au.playing, ~0ull); au.counter = au.samples; }
        beep_fill(buf + i, 1, au.hz);
        if (--au.counter == 0) { dispatch_semaphore_signal(au.done); dispatch_semaphore_signal(au.stopped); }
    }
    return 0;
}

static int au_load(void) {
    if (au.state) return au.state == 1;
    void* dl = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_LOCAL);
    if (dl) {
        *(void**)&au.find    = dlsym(dl, "AudioComponentFindNext");
        *(void**)&au.inst    = dlsym(dl, "AudioComponentInstanceNew");
        *(void**)&au.setprop = dlsym(dl, "AudioUnitSetProperty");
        *(void**)&au.init    = dlsym(dl, "AudioUnitInitialize");
        *(void**)&au.start   = dlsym(dl, "AudioOutputUnitStart");
    }
    void* unit = NULL;
    au_desc  d  = { 0x61756F75u, 0x64656620u, 0x6170706Cu, 0, 0 };           /* 'auou' 'def ' 'appl' */
    au_stream st = { BEEP_RATE, 0x6C70636Du, 0, 1, 1, 1, 1, 8, 0 };            /* 'lpcm', 8-bit mono */
    au_cb    cb = { au_tone_cb, NULL };
    int ok = dl && au.find && au.inst && au.setprop && au.init && au.start
          && au.inst(au.find(NULL, &d), &unit) == 0
          && au.setprop(unit, 23, 1, 0, &cb, sizeof cb) == 0                 /* SetRenderCallback, scope Input */
          && au.setprop(unit, 8, 1, 0, &st, sizeof st) == 0                  /* StreamFormat, scope Input */
          && au.init(unit) == 0 && au.start(unit) == 0;
    if (ok) { au.stopped = dispatch_semaphore_create(1); au.playing = dispatch_semaphore_create(0); au.done = dispatch_semaphore_create(0); }
    else if (dl) dlclose(dl);
    au.state = ok ? 1 : 2;
    return ok;
}

static int beep_open(void) { return au_load(); }

static void beep_play(int hz, int ms) {
    dispatch_semaphore_wait(au.stopped, ~0ull);
    au.hz = hz; au.samples = ms * BEEP_RATE / 1000;
    dispatch_semaphore_signal(au.playing);
    dispatch_semaphore_wait(au.done, ~0ull);
}

#elif !defined(RAY_OS_WINDOWS)

static int beep_open(void) { return 0; }
static void beep_play(int hz, int ms) { (void)hz; (void)ms; }

#endif

static void beep_bell(void) {
    if (q_termbox_emit("\a", 1)) return;
#if defined(RAY_OS_WINDOWS)
    _write(1, "\a", 1);
#else
    if (write(STDOUT_FILENO, "\a", 1) < 0) return;
#endif
}

static atomic_int beep_busy;
static struct { int hz, ms; } beep_req;

#if defined(RAY_OS_WINDOWS)
static DWORD WINAPI beep_thread(LPVOID p) { (void)p; beep_play(beep_req.hz, beep_req.ms); atomic_store(&beep_busy, 0); return 0; }
#else
static void* beep_thread(void* p) { (void)p; beep_play(beep_req.hz, beep_req.ms); atomic_store(&beep_busy, 0); return NULL; }
#endif

/* 1 = a tone was started on a detached thread; 0 = no audio path (the caller rings the bell) or one is playing */
static int beep_start(int hz, int ms) {
    if (!beep_open()) return 0;
    int idle = 0;
    if (!atomic_compare_exchange_strong(&beep_busy, &idle, 1)) return -1;
    beep_req.hz = hz; beep_req.ms = ms;
#if defined(RAY_OS_WINDOWS)
    HANDLE h = CreateThread(NULL, 0, beep_thread, NULL, 0, NULL);
    if (!h) { atomic_store(&beep_busy, 0); return 0; }
    CloseHandle(h);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, beep_thread, NULL) != 0) { atomic_store(&beep_busy, 0); return 0; }
    pthread_detach(t);
#endif
    return 1;
}

static ray_t* beep_fn(ray_t** args, int64_t n) {
    if (n != 2) return q_err(QE_RANK);
    if (!q_type_is_int_atom(args[0]) || !q_type_is_int_atom(args[1])) return q_err(QE_TYPE);
    int64_t hz = q_type_iatom_val(args[0]), ms = q_type_iatom_val(args[1]);
    if (hz < 0 || ms < 0 || hz > 32767 || ms > 60000) return q_err(QE_DOMAIN);
    int r = beep_start((int)hz, (int)ms);
    if (r == 0) beep_bell();
    return ray_bool(r == 1);
}

void q_beep_register(void) {
    static const char nm[] = ".termbox.i.beep";
    ray_t* obj = ray_fn_vary(nm, RAY_FN_NONE, beep_fn);
    q_env_bind(ray_sym_intern(nm, strlen(nm)), obj);
    ray_release(obj);
}
