/*
 * pelican_intro_link.h -- single-header client for `pelican_cli intro-link`.
 *
 * Development form (default): connects to `pelican_cli intro-link serve` over
 * TCP and receives curve tracks, SPIR-V shaders and the server's time.
 *
 *     // in exactly one C or C++ file
 *     #define PELICAN_INTRO_LINK_IMPLEMENTATION
 *     #include "pelican_intro_link.h"
 *
 *     pil_connect("127.0.0.1", 47611, 5000);
 *     for (;;) {
 *         pil_poll();
 *         float fade = PIL_TRACK(fade, t);
 *         if (PIL_SHADER_CHANGED(main_frag)) {
 *             uint32_t words;
 *             const uint32_t *spirv = PIL_SHADER(main_frag, &words);
 *             ... rebuild the pipeline ...
 *         }
 *     }
 *
 * Release form: `pelican_cli intro-link bake <project> --out baked.h`, then
 *
 *     #define PELICAN_INTRO_LINK_BAKED
 *     #include "baked.h"
 *     #include "pelican_intro_link.h"
 *
 * The same calls read the baked tables. No socket, malloc, stdio or string.h
 * code is compiled in this form; an unknown track or shader name is a compile
 * error. pil_eval is the one evaluator both forms use.
 *
 * Windows: the implementation includes <winsock2.h> and links ws2_32 through
 * #pragma comment on MSVC. Include this header before <windows.h> in that
 * file, or define WIN32_LEAN_AND_MEAN before <windows.h>. POSIX builds with
 * -std=c99 need _POSIX_C_SOURCE=200112L for getaddrinfo and clock_gettime.
 *
 * Wire v1 (little-endian, frame = u32 type, u32 payload_bytes, payload):
 *   0 HELLO        client->server  u32 magic 'PLNK' (0x4B4E4C50), u32 version 1
 *   1 TRACK        server->client  u16 name_len, name, u32 key_count,
 *                                  key x N = f32 time, f32 value, f32 in_tangent,
 *                                  f32 out_tangent, u32 interpolation
 *   2 SHADER       server->client  u16 name_len, name, u32 word_count, u32 x N
 *   3 SHADER_ERROR server->client  u16 name_len, name, u32 text_len, text
 *   4 TIME         server->client  u32 flags (bit0 paused, bit1 seek), f32 time
 *   5 SNAPSHOT_END server->client  (empty)
 */
#ifndef PELICAN_INTRO_LINK_H
#define PELICAN_INTRO_LINK_H

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__cplusplus)
#define PIL_INLINE static __inline
#else
#define PIL_INLINE static inline
#endif

#define PIL_INTERPOLATION_LINEAR 0u
#define PIL_INTERPOLATION_STEP 1u
#define PIL_INTERPOLATION_CUBIC 2u

#ifndef PIL_KEY_DEFINED
#define PIL_KEY_DEFINED
typedef struct pil_key {
    float time;
    float value;
    float in_tangent;
    float out_tangent;
    uint32_t interpolation; /* 0 linear, 1 step, 2 cubic */
} pil_key;
#endif

/*
 * The engine's ScalarCurve::sample in float: outside the keys the end value;
 * inside, the left key's interpolation decides the segment (step holds it,
 * linear lerps, cubic is Hermite with tangents as slopes per second).
 */
PIL_INLINE float pil_eval(const pil_key *keys, uint32_t count, float t) {
    uint32_t lo, hi;
    float width, u, u2, u3;
    const pil_key *a, *b;
    if (count == 0) return 0.0f;
    if (t <= keys[0].time) return keys[0].value;
    if (t >= keys[count - 1].time) return keys[count - 1].value;
    lo = 0;
    hi = count - 1; /* keys[lo].time <= t < keys[hi].time */
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid].time <= t) lo = mid;
        else hi = mid;
    }
    a = &keys[lo];
    b = &keys[hi];
    if (a->interpolation == PIL_INTERPOLATION_STEP) return a->value;
    width = b->time - a->time;
    u = (t - a->time) / width;
    if (a->interpolation != PIL_INTERPOLATION_CUBIC) return a->value + (b->value - a->value) * u;
    u2 = u * u;
    u3 = u2 * u;
    return (2.0f * u3 - 3.0f * u2 + 1.0f) * a->value + (u3 - 2.0f * u2 + u) * width * a->out_tangent +
           (-2.0f * u3 + 3.0f * u2) * b->value + (u3 - u2) * width * b->in_tangent;
}

#ifdef PELICAN_INTRO_LINK_BAKED
/* ------------------------------------------------------------------ release */

#ifndef PIL_BAKED_DATA_INCLUDED
#error "include the header written by `pelican_cli intro-link bake` before pelican_intro_link.h"
#endif

PIL_INLINE int pil_connect(const char *host, int port, int timeout_ms) {
    (void)host;
    (void)port;
    (void)timeout_ms;
    return 1;
}
PIL_INLINE void pil_poll(void) {}
PIL_INLINE void pil_disconnect(void) {}
PIL_INLINE int pil_time(float *t, int *paused) {
    (void)t;
    (void)paused;
    return 0;
}

#define PIL_TRACK(name, t)                                                                     \
    pil_eval(pil_baked_keys + pil_baked_track_first[PIL_TRACK_##name],                         \
             pil_baked_track_count[PIL_TRACK_##name], (t))
#define PIL_SHADER(name, word_count)                                                           \
    (*(word_count) = pil_baked_shader_word_count[PIL_SHADER_##name],                           \
     pil_baked_shader_words[PIL_SHADER_##name])
#define PIL_SHADER_CHANGED(name) ((void)PIL_SHADER_##name, 0)
#define PIL_SHADER_ERROR(name) ((void)PIL_SHADER_##name, (const char *)0)

#else
/* -------------------------------------------------------------- development */

#ifdef __cplusplus
extern "C" {
#endif

/* Connects and waits for the snapshot (every track, shader and the time).
 * 1 on success, 0 when no server answered within timeout_ms. */
int pil_connect(const char *host, int port, int timeout_ms);
/* Reads whatever has arrived without blocking. Keeps the last values when the
 * server goes away. */
void pil_poll(void);
/* Closes the connection and frees everything received. */
void pil_disconnect(void);
/* 0 before any TIME arrived; 1 while the server holds the clock (*paused set);
 * 2 once right after a seek, with *t set to the time to jump to. */
int pil_time(float *t, int *paused);

float pil_track(const char *name, float t);
const uint32_t *pil_shader(const char *name, uint32_t *word_count);
int pil_shader_changed(const char *name);
const char *pil_shader_error(const char *name);

#ifdef __cplusplus
}
#endif

#define PIL_TRACK(name, t) pil_track(#name, (t))
#define PIL_SHADER(name, word_count) pil_shader(#name, (word_count))
#define PIL_SHADER_CHANGED(name) pil_shader_changed(#name)
#define PIL_SHADER_ERROR(name) pil_shader_error(#name)

#endif /* PELICAN_INTRO_LINK_BAKED */
#endif /* PELICAN_INTRO_LINK_H */

/* ============================================================ implementation */
#if defined(PELICAN_INTRO_LINK_IMPLEMENTATION) && !defined(PELICAN_INTRO_LINK_BAKED) && \
    !defined(PELICAN_INTRO_LINK_IMPLEMENTED)
#define PELICAN_INTRO_LINK_IMPLEMENTED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32")
#endif
typedef SOCKET pil__socket;
#define PIL__INVALID_SOCKET INVALID_SOCKET
#define pil__close closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int pil__socket;
#define PIL__INVALID_SOCKET (-1)
#define pil__close close
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pil__track {
    char *name;
    pil_key *keys;
    uint32_t count;
} pil__track;

typedef struct pil__shader {
    char *name;
    uint32_t *words;
    uint32_t count;
    int changed;
    char *error;
} pil__shader;

static struct {
    int connected;
    int started; /* WSAStartup done */
    pil__socket sock;
    unsigned char *buffer;
    size_t length, capacity;
    pil__track *tracks;
    uint32_t track_count, track_capacity;
    pil__shader *shaders;
    uint32_t shader_count, shader_capacity;
    int snapshot_done;
    int has_time, paused, seek_pending;
    float seek_time;
} pil__state;

static uint32_t pil__u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static float pil__f32(const unsigned char *p) {
    uint32_t bits = pil__u32(p);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static unsigned long long pil__now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ull + (unsigned long long)(now.tv_nsec / 1000000);
#endif
}

static void pil__sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec wait;
    wait.tv_sec = ms / 1000;
    wait.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&wait, NULL);
#endif
}

static char *pil__strndup(const unsigned char *text, size_t length) {
    char *copy = (char *)malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* Reads `u16 name_len, name` at *p; returns 0 when it does not fit. */
static int pil__read_name(const unsigned char **p, const unsigned char *end, const unsigned char **name,
                          size_t *length) {
    size_t n;
    if (end - *p < 2) return 0;
    n = (size_t)(*p)[0] | ((size_t)(*p)[1] << 8);
    *p += 2;
    if ((size_t)(end - *p) < n) return 0;
    *name = *p;
    *length = n;
    *p += n;
    return 1;
}

static int pil__name_is(const char *stored, const unsigned char *name, size_t length) {
    return strlen(stored) == length && memcmp(stored, name, length) == 0;
}

static pil__track *pil__find_track(const char *name) {
    uint32_t i;
    for (i = 0; i < pil__state.track_count; ++i)
        if (strcmp(pil__state.tracks[i].name, name) == 0) return &pil__state.tracks[i];
    return NULL;
}

static pil__shader *pil__find_shader(const char *name) {
    uint32_t i;
    for (i = 0; i < pil__state.shader_count; ++i)
        if (strcmp(pil__state.shaders[i].name, name) == 0) return &pil__state.shaders[i];
    return NULL;
}

static pil__track *pil__track_slot(const unsigned char *name, size_t length) {
    uint32_t i;
    pil__track *slot;
    for (i = 0; i < pil__state.track_count; ++i)
        if (pil__name_is(pil__state.tracks[i].name, name, length)) return &pil__state.tracks[i];
    if (pil__state.track_count == pil__state.track_capacity) {
        uint32_t capacity = pil__state.track_capacity ? pil__state.track_capacity * 2 : 16;
        pil__track *grown = (pil__track *)realloc(pil__state.tracks, capacity * sizeof(pil__track));
        if (!grown) return NULL;
        pil__state.tracks = grown;
        pil__state.track_capacity = capacity;
    }
    slot = &pil__state.tracks[pil__state.track_count];
    slot->name = pil__strndup(name, length);
    if (!slot->name) return NULL;
    slot->keys = NULL;
    slot->count = 0;
    ++pil__state.track_count;
    return slot;
}

static pil__shader *pil__shader_slot(const unsigned char *name, size_t length) {
    uint32_t i;
    pil__shader *slot;
    for (i = 0; i < pil__state.shader_count; ++i)
        if (pil__name_is(pil__state.shaders[i].name, name, length)) return &pil__state.shaders[i];
    if (pil__state.shader_count == pil__state.shader_capacity) {
        uint32_t capacity = pil__state.shader_capacity ? pil__state.shader_capacity * 2 : 8;
        pil__shader *grown = (pil__shader *)realloc(pil__state.shaders, capacity * sizeof(pil__shader));
        if (!grown) return NULL;
        pil__state.shaders = grown;
        pil__state.shader_capacity = capacity;
    }
    slot = &pil__state.shaders[pil__state.shader_count];
    slot->name = pil__strndup(name, length);
    if (!slot->name) return NULL;
    slot->words = NULL;
    slot->count = 0;
    slot->changed = 0;
    slot->error = NULL;
    ++pil__state.shader_count;
    return slot;
}

static void pil__handle(uint32_t type, const unsigned char *p, uint32_t bytes) {
    const unsigned char *end = p + bytes;
    const unsigned char *name;
    size_t name_length;
    uint32_t count, i;
    switch (type) {
    case 1: { /* TRACK */
        pil__track *track;
        pil_key *keys;
        if (!pil__read_name(&p, end, &name, &name_length) || end - p < 4) return;
        count = pil__u32(p);
        p += 4;
        if ((size_t)(end - p) / 20 < count) return;
        keys = (pil_key *)malloc((count ? count : 1) * sizeof(pil_key));
        if (!keys) return;
        for (i = 0; i < count; ++i, p += 20) {
            keys[i].time = pil__f32(p);
            keys[i].value = pil__f32(p + 4);
            keys[i].in_tangent = pil__f32(p + 8);
            keys[i].out_tangent = pil__f32(p + 12);
            keys[i].interpolation = pil__u32(p + 16);
        }
        track = pil__track_slot(name, name_length);
        if (!track) {
            free(keys);
            return;
        }
        free(track->keys);
        track->keys = keys;
        track->count = count;
        break;
    }
    case 2: { /* SHADER */
        pil__shader *shader;
        uint32_t *words;
        if (!pil__read_name(&p, end, &name, &name_length) || end - p < 4) return;
        count = pil__u32(p);
        p += 4;
        if ((size_t)(end - p) / 4 < count) return;
        words = (uint32_t *)malloc((count ? count : 1) * sizeof(uint32_t));
        if (!words) return;
        for (i = 0; i < count; ++i, p += 4) words[i] = pil__u32(p);
        shader = pil__shader_slot(name, name_length);
        if (!shader) {
            free(words);
            return;
        }
        free(shader->words);
        free(shader->error);
        shader->words = words;
        shader->count = count;
        shader->error = NULL;
        shader->changed = 1;
        break;
    }
    case 3: { /* SHADER_ERROR */
        pil__shader *shader;
        char *text;
        if (!pil__read_name(&p, end, &name, &name_length) || end - p < 4) return;
        count = pil__u32(p);
        p += 4;
        if ((size_t)(end - p) < count) return;
        text = pil__strndup(p, count);
        if (!text) return;
        shader = pil__shader_slot(name, name_length);
        if (!shader) {
            free(text);
            return;
        }
        free(shader->error);
        shader->error = text;
        break;
    }
    case 4: { /* TIME */
        uint32_t flags;
        if (bytes < 8) return;
        flags = pil__u32(p);
        pil__state.has_time = 1;
        pil__state.paused = (flags & 1u) != 0;
        if (flags & 2u) {
            pil__state.seek_pending = 1;
            pil__state.seek_time = pil__f32(p + 4);
        }
        break;
    }
    case 5: /* SNAPSHOT_END: what arrived so far is the starting state */
        pil__state.snapshot_done = 1;
        for (i = 0; i < pil__state.shader_count; ++i) pil__state.shaders[i].changed = 0;
        break;
    default: /* unknown types are skipped */
        break;
    }
}

static void pil__close_socket(void) {
    if (pil__state.connected) pil__close(pil__state.sock);
    pil__state.connected = 0;
    pil__state.length = 0;
}

/* Waits up to timeout_ms for data and handles every complete frame.
 * -1 when the connection closed, 0 when nothing arrived, 1 otherwise. */
static int pil__pump(int timeout_ms) {
    fd_set readable;
    struct timeval wait;
    int ready, received;
    size_t offset;
    if (!pil__state.connected) return -1;
    FD_ZERO(&readable);
    FD_SET(pil__state.sock, &readable);
    wait.tv_sec = timeout_ms / 1000;
    wait.tv_usec = (timeout_ms % 1000) * 1000;
    ready = select((int)pil__state.sock + 1, &readable, NULL, NULL, &wait);
    if (ready < 0) return -1;
    if (ready == 0) return 0;
    if (pil__state.capacity - pil__state.length < 65536) {
        size_t capacity = pil__state.capacity ? pil__state.capacity * 2 : 131072;
        unsigned char *grown;
        while (capacity - pil__state.length < 65536) capacity *= 2;
        grown = (unsigned char *)realloc(pil__state.buffer, capacity);
        if (!grown) return -1;
        pil__state.buffer = grown;
        pil__state.capacity = capacity;
    }
    received = (int)recv(pil__state.sock, (char *)pil__state.buffer + pil__state.length, 65536, 0);
    if (received <= 0) return -1;
    pil__state.length += (size_t)received;
    offset = 0;
    while (pil__state.length - offset >= 8) {
        const uint32_t type = pil__u32(pil__state.buffer + offset);
        const uint32_t bytes = pil__u32(pil__state.buffer + offset + 4);
        if (pil__state.length - offset - 8 < bytes) break;
        pil__handle(type, pil__state.buffer + offset + 8, bytes);
        offset += 8 + (size_t)bytes;
    }
    if (offset) {
        memmove(pil__state.buffer, pil__state.buffer + offset, pil__state.length - offset);
        pil__state.length -= offset;
    }
    return 1;
}

static int pil__open(const char *host, int port) {
    struct addrinfo hints, *found = NULL, *it;
    char service[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &found) != 0) return 0;
    for (it = found; it; it = it->ai_next) {
        pil__socket sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == PIL__INVALID_SOCKET) continue;
        if (connect(sock, it->ai_addr, (int)it->ai_addrlen) == 0) {
            int no_delay = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay));
            pil__state.sock = sock;
            pil__state.connected = 1;
            break;
        }
        pil__close(sock);
    }
    freeaddrinfo(found);
    return pil__state.connected;
}

int pil_connect(const char *host, int port, int timeout_ms) {
    const unsigned long long deadline = pil__now_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
    unsigned char hello[16] = {0, 0, 0, 0, 8, 0, 0, 0, 0x50, 0x4C, 0x4E, 0x4B, 1, 0, 0, 0};
#ifdef _WIN32
    if (!pil__state.started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
        pil__state.started = 1;
    }
#endif
    pil__close_socket();
    pil__state.snapshot_done = 0;
    while (!pil__open(host, port)) {
        if (pil__now_ms() >= deadline) return 0;
        pil__sleep_ms(50);
    }
    if (send(pil__state.sock, (const char *)hello, (int)sizeof(hello), 0) != (int)sizeof(hello)) {
        pil__close_socket();
        return 0;
    }
    while (!pil__state.snapshot_done) {
        const unsigned long long now = pil__now_ms();
        if (now >= deadline || pil__pump((int)(deadline - now)) < 0) {
            pil__close_socket();
            return 0;
        }
    }
    return 1;
}

void pil_poll(void) {
    int result = 0;
    while (pil__state.connected && (result = pil__pump(0)) > 0) {
    }
    if (pil__state.connected && result < 0) pil__close_socket();
}

void pil_disconnect(void) {
    uint32_t i;
    pil__close_socket();
    for (i = 0; i < pil__state.track_count; ++i) {
        free(pil__state.tracks[i].name);
        free(pil__state.tracks[i].keys);
    }
    for (i = 0; i < pil__state.shader_count; ++i) {
        free(pil__state.shaders[i].name);
        free(pil__state.shaders[i].words);
        free(pil__state.shaders[i].error);
    }
    free(pil__state.tracks);
    free(pil__state.shaders);
    free(pil__state.buffer);
#ifdef _WIN32
    if (pil__state.started) WSACleanup();
#endif
    memset(&pil__state, 0, sizeof(pil__state));
}

int pil_time(float *t, int *paused) {
    if (!pil__state.has_time) return 0;
    if (paused) *paused = pil__state.paused;
    if (pil__state.seek_pending) {
        pil__state.seek_pending = 0;
        if (t) *t = pil__state.seek_time;
        return 2;
    }
    return 1;
}

float pil_track(const char *name, float t) {
    const pil__track *track = pil__find_track(name);
    return track ? pil_eval(track->keys, track->count, t) : 0.0f;
}

const uint32_t *pil_shader(const char *name, uint32_t *word_count) {
    const pil__shader *shader = pil__find_shader(name);
    if (word_count) *word_count = shader ? shader->count : 0;
    return shader ? shader->words : NULL;
}

int pil_shader_changed(const char *name) {
    pil__shader *shader = pil__find_shader(name);
    int changed;
    if (!shader) return 0;
    changed = shader->changed;
    shader->changed = 0;
    return changed;
}

const char *pil_shader_error(const char *name) {
    const pil__shader *shader = pil__find_shader(name);
    return shader ? shader->error : NULL;
}

#ifdef __cplusplus
}
#endif

#endif /* PELICAN_INTRO_LINK_IMPLEMENTATION */
