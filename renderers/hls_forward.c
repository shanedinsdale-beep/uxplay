/**
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2026 Shane Dinsdale
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

/* -hls-forward: see hls_forward.h.  A thread owns the connection to the player: it (re)connects every second,
   sends "hello", and reads the player's messages into a status cache.  The AirPlay http handlers send events
   and read the cache from their own threads; a mutex protects the socket and the cache. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hls_forward.h"

#define HLS_FORWARD_VERSION 1
#define MAX_LINE (64 * 1024)          /* protocol limit, including the newline */
#define MAX_SESSION 64

#ifdef _WIN32

int hls_forward_init(logger_t *logger, const char *socket_path, const char *version) {
    logger_log(logger, LOGGER_ERR, "-hls-forward is not supported on Windows");
    return -1;
}
void hls_forward_destroy(void) {}
bool hls_forward_connected(void) { return false; }
bool hls_forward_play(const char *session, const char *location, float start_position,
                      const char *client, const char *kind) { return false; }
bool hls_forward_rate(float rate) { return false; }
bool hls_forward_scrub(float position) { return false; }
bool hls_forward_stop(const char *reason) { return false; }
bool hls_forward_disconnect(const char *reason) { return false; }
bool hls_forward_volume(double volume, double db) { return false; }
bool hls_forward_audio(const char *state, const char *codec, bool screen) { return false; }
bool hls_forward_metadata(const char *title, const char *artist, const char *album) { return false; }
bool hls_forward_coverart(int bytes) { return false; }
bool hls_forward_get_status(hls_forward_status_t *status) { return false; }

#else

#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   /* macOS: SO_NOSIGPIPE is set on the socket instead */
#endif

static logger_t *logger = NULL;
static struct sockaddr_un address;
static char uxplay_version[32];
static pthread_t thread;
static bool running = false;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;  /* protects everything below */
static int sock = -1;
static char session[MAX_SESSION] = "";   /* uuid of the current video, "" if none */
static bool have_status = false;
static hls_forward_status_t status;

/* ---- JSON output ---- */

typedef struct {
    char *data;
    size_t len;
    bool overflow;
} line_t;

static void put(line_t *line, const char *s, size_t n) {
    if (line->len + n + 2 > MAX_LINE) {   /* room for "\n\0" */
        line->overflow = true;
        return;
    }
    memcpy(line->data + line->len, s, n);
    line->len += n;
}

static void put_str(line_t *line, const char *s) {
    put(line, s, strlen(s));
}

static void put_json_string(line_t *line, const char *s) {
    char esc[8];
    put(line, "\"", 1);
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\';
            esc[1] = (char) *p;
            put(line, esc, 2);
        } else if (*p < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            put_str(line, esc);
        } else {
            put(line, (const char *) p, 1);
        }
    }
    put(line, "\"", 1);
}

static void put_json_number(line_t *line, double value) {
    char num[32];
    snprintf(num, sizeof(num), "%.9g", isfinite(value) ? value : 0.0);
    put_str(line, num);
}

/* {"v":1,"type":"<type>","session":"<session>"  (caller adds fields, then end_message) */
static void begin_message(line_t *line, const char *type, const char *session_id) {
    put_str(line, "{\"v\":1,\"type\":");
    put_json_string(line, type);
    if (session_id) {
        put_str(line, ",\"session\":");
        put_json_string(line, session_id);
    }
}

/* call with the mutex held */
static bool send_message_locked(line_t *line) {
    put_str(line, "}");
    if (line->overflow) {
        logger_log(logger, LOGGER_ERR, "hls_forward: message longer than %d bytes not sent", MAX_LINE);
        return false;
    }
    line->data[line->len++] = '\n';
    if (sock < 0) {
        return false;
    }
    size_t sent = 0;
    while (sent < line->len) {
        ssize_t n = send(sock, line->data + sent, line->len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            logger_log(logger, LOGGER_WARNING, "hls_forward: send failed: %s", strerror(errno));
            shutdown(sock, SHUT_RDWR);   /* the reader thread sees this and reconnects */
            return false;
        }
        sent += (size_t) n;
    }
    return true;
}

/* ---- JSON input: flat objects from the player ---- */

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/* p points at an opening quote; returns the position after the closing quote, or NULL */
static const char *skip_string(const char *p, const char *end) {
    for (p++; p < end; p++) {
        if (*p == '\\') {
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* returns the position after one JSON value, or NULL */
static const char *skip_value(const char *p, const char *end) {
    int depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (!p) {
                return NULL;
            }
            if (depth == 0) {
                return p;
            }
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0) {
                return p;
            }
            if (--depth == 0) {
                return p + 1;
            }
        } else if (depth == 0 && (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            return p;
        }
        p++;
    }
    return depth == 0 ? p : NULL;
}

/* find a top-level member of the object in [json, end): sets *value to the start of its value */
static bool json_find(const char *json, const char *end, const char *key, const char **value) {
    size_t key_len = strlen(key);
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;
    while (true) {
        p = skip_ws(p, end);
        if (p >= end || *p != '"') {
            return false;
        }
        const char *name = p + 1;
        p = skip_string(p, end);
        if (!p) {
            return false;
        }
        size_t name_len = (size_t) (p - 1 - name);
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = skip_ws(p + 1, end);
        if (name_len == key_len && !strncmp(name, key, key_len)) {
            *value = p;
            return true;
        }
        p = skip_value(p, end);
        if (!p) {
            return false;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ',') {
            return false;
        }
        p++;
    }
}

static bool json_number(const char *json, const char *end, const char *key, double *number) {
    const char *value;
    if (!json_find(json, end, key, &value) || value >= end) {
        return false;
    }
    char *stop;
    double d = strtod(value, &stop);
    if (stop == value) {
        return false;
    }
    *number = d;
    return true;
}

static bool json_true(const char *json, const char *end, const char *key) {
    const char *value;
    return json_find(json, end, key, &value) && end - value >= 4 && !strncmp(value, "true", 4);
}

/* copy a string member (escapes are kept as they are) into buf */
static bool json_string(const char *json, const char *end, const char *key, char *buf, size_t size) {
    const char *value;
    if (!json_find(json, end, key, &value) || value >= end || *value != '"') {
        return false;
    }
    const char *close = skip_string(value, end);
    if (!close) {
        return false;
    }
    size_t len = (size_t) (close - 1 - (value + 1));
    if (len >= size) {
        len = size - 1;
    }
    memcpy(buf, value + 1, len);
    buf[len] = '\0';
    return true;
}

/* [a, b] -> a, b; false for null or anything else */
static bool json_range(const char *json, const char *end, const char *key, double *a, double *b) {
    const char *value;
    if (!json_find(json, end, key, &value) || value >= end || *value != '[') {
        return false;
    }
    char *stop;
    *a = strtod(value + 1, &stop);
    const char *p = skip_ws(stop, end);
    if (stop == value + 1 || p >= end || *p != ',') {
        return false;
    }
    *b = strtod(p + 1, &stop);
    return stop != p + 1;
}

/* handle one line from the player; returns false to close the connection */
static bool handle_line(const char *line, const char *end) {
    char type[32], id[MAX_SESSION], text[256];
    double v = 0.0, number;
    if (!json_number(line, end, "v", &v) || !json_string(line, end, "type", type, sizeof(type))) {
        logger_log(logger, LOGGER_WARNING, "hls_forward: ignoring invalid message from player");
        return true;
    }
    if (v > HLS_FORWARD_VERSION) {
        logger_log(logger, LOGGER_ERR, "hls_forward: player speaks protocol version %d, this UxPlay speaks %d; closing",
                   (int) v, HLS_FORWARD_VERSION);
        return false;
    }
    if (!strcmp(type, "hello")) {
        if (!json_string(line, end, "version", text, sizeof(text))) {
            text[0] = '\0';
        }
        logger_log(logger, LOGGER_INFO, "hls_forward: player connected (version %s)", text);
        return true;
    }
    if (!json_string(line, end, "session", id, sizeof(id))) {
        logger_log(logger, LOGGER_DEBUG, "hls_forward: ignoring \"%s\" message without a session", type);
        return true;
    }

    pthread_mutex_lock(&mutex);
    bool current = (session[0] && !strcmp(id, session));
    if (!current) {
        /* a late message about a previous video */
    } else if (!strcmp(type, "status")) {
        hls_forward_status_t s = status;
        s.duration = json_number(line, end, "duration", &number) ? number : 0.0;
        s.position = json_number(line, end, "position", &number) ? number : 0.0;
        s.rate = json_number(line, end, "rate", &number) ? (float) number : 0.0f;
        s.ready = json_true(line, end, "ready");
        if (!json_range(line, end, "seekable", &s.seek_start, &s.seek_end)) {
            s.seek_start = 0.0;
            s.seek_end = s.duration;
        }
        status = s;
        have_status = true;
    } else if (!strcmp(type, "ended") || !strcmp(type, "failed")) {
        if (!json_string(line, end, "reason", text, sizeof(text))) {
            text[0] = '\0';
        }
        logger_log(logger, LOGGER_INFO, "hls_forward: player reports video %s %s", type, text);
        status.finished = true;
        have_status = true;
    } else {
        logger_log(logger, LOGGER_DEBUG, "hls_forward: ignoring unknown message type \"%s\"", type);
    }
    pthread_mutex_unlock(&mutex);
    return true;
}

/* ---- the connection thread ---- */

static int connect_player(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    if (connect(s, (struct sockaddr *) &address, sizeof(address)) < 0) {
        int err = errno;
        close(s);
        errno = err;
        return -1;
    }
    struct timeval timeout = { 2, 0 };   /* never block an http handler for long */
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    return s;
}

static void read_player(int s, char *buf) {
    size_t len = 0;
    while (running) {
        ssize_t n = recv(s, buf + len, MAX_LINE - len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return;
        }
        len += (size_t) n;
        char *start = buf;
        char *newline;
        while ((newline = memchr(start, '\n', len - (size_t) (start - buf)))) {
            if (!handle_line(start, newline)) {
                return;
            }
            start = newline + 1;
        }
        len -= (size_t) (start - buf);
        memmove(buf, start, len);
        if (len == MAX_LINE) {
            logger_log(logger, LOGGER_ERR, "hls_forward: line from player longer than %d bytes; closing", MAX_LINE);
            return;
        }
    }
}

static void *connection_thread(void *arg) {
    char *buf = (char *) malloc(MAX_LINE);
    char *hello = (char *) malloc(MAX_LINE);
    if (!buf || !hello) {
        printf("Memory allocation failed (hls_forward)\n");
        exit(1);
    }
    bool outage_logged = false;
    while (running) {
        int s = connect_player();
        if (s < 0) {
            if (!outage_logged) {
                logger_log(logger, LOGGER_WARNING, "hls_forward: cannot connect to %s (%s); retrying every second",
                           address.sun_path, strerror(errno));
                outage_logged = true;
            }
            sleep(1);
            continue;
        }
        outage_logged = false;
        logger_log(logger, LOGGER_INFO, "hls_forward: connected to %s", address.sun_path);

        line_t line = { hello, 0, false };
        begin_message(&line, "hello", NULL);
        put_str(&line, ",\"role\":\"uxplay\",\"version\":");
        put_json_string(&line, uxplay_version);
        put_str(&line, ",\"features\":[\"volume\",\"audio\"]");
        pthread_mutex_lock(&mutex);
        sock = s;
        send_message_locked(&line);
        pthread_mutex_unlock(&mutex);

        read_player(s, buf);

        pthread_mutex_lock(&mutex);
        sock = -1;
        pthread_mutex_unlock(&mutex);
        close(s);
        if (running) {
            logger_log(logger, LOGGER_WARNING, "hls_forward: player disconnected; reconnecting");
        }
    }
    free(buf);
    free(hello);
    return NULL;
}

int hls_forward_init(logger_t *render_logger, const char *socket_path, const char *version) {
    logger = render_logger;
    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        logger_log(logger, LOGGER_ERR, "hls_forward: socket path is longer than %zu bytes",
                   sizeof(address.sun_path) - 1);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    snprintf(uxplay_version, sizeof(uxplay_version), "%s", version);
    running = true;
    if (pthread_create(&thread, NULL, connection_thread, NULL)) {
        running = false;
        return -1;
    }
    return 0;
}

void hls_forward_destroy(void) {
    if (!running) {
        return;
    }
    running = false;
    pthread_mutex_lock(&mutex);
    if (sock >= 0) {
        shutdown(sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&mutex);
    pthread_join(thread, NULL);
}

bool hls_forward_connected(void) {
    pthread_mutex_lock(&mutex);
    bool connected = (sock >= 0);
    pthread_mutex_unlock(&mutex);
    return connected;
}

static char *new_line_buffer(void) {
    char *data = (char *) malloc(MAX_LINE);
    if (!data) {
        printf("Memory allocation failed (hls_forward)\n");
        exit(1);
    }
    return data;
}

bool hls_forward_play(const char *session_id, const char *location, float start_position,
                      const char *client, const char *kind) {
    line_t line = { new_line_buffer(), 0, false };
    begin_message(&line, "play", session_id);
    put_str(&line, ",\"location\":");
    put_json_string(&line, location);
    put_str(&line, ",\"start\":");
    put_json_number(&line, start_position);
    put_str(&line, ",\"client\":");
    put_json_string(&line, client ? client : "");
    put_str(&line, ",\"kind\":");
    put_json_string(&line, kind);

    pthread_mutex_lock(&mutex);
    bool ok = send_message_locked(&line);
    snprintf(session, sizeof(session), "%s", session_id);
    memset(&status, 0, sizeof(status));
    /* not sent: report the video finished at once, so the client takes it back (iOS ignores an error
       status to /play, and then waits on the receiver indefinitely) */
    status.finished = !ok;
    have_status = !ok;
    pthread_mutex_unlock(&mutex);
    free(line.data);
    return ok;
}

/* a message about the current session with one numeric field, or no field if name is NULL */
static bool send_session_message(const char *type, const char *name, double value, const char *reason,
                                 bool end_session) {
    char data[512];
    line_t line = { data, 0, false };
    bool ok = false;
    pthread_mutex_lock(&mutex);
    if (session[0]) {
        begin_message(&line, type, session);
        if (name) {
            put_str(&line, ",\"");
            put_str(&line, name);
            put_str(&line, "\":");
            put_json_number(&line, value);
        }
        if (reason) {
            put_str(&line, ",\"reason\":");
            put_json_string(&line, reason);
        }
        ok = send_message_locked(&line);
        if (end_session) {
            session[0] = '\0';
            have_status = false;
        }
    }
    pthread_mutex_unlock(&mutex);
    return ok;
}

bool hls_forward_rate(float rate) {
    return send_session_message("rate", "rate", rate, NULL, false);
}

bool hls_forward_scrub(float position) {
    return send_session_message("scrub", "position", position, NULL, false);
}

bool hls_forward_stop(const char *reason) {
    return send_session_message("stop", NULL, 0.0, reason, true);
}

bool hls_forward_volume(double volume, double db) {
    /* during a video, for that session; otherwise for the audio-only connection (no session field) */
    char data[512];
    line_t line = { data, 0, false };
    pthread_mutex_lock(&mutex);
    begin_message(&line, "volume", session[0] ? session : NULL);
    put_str(&line, ",\"volume\":");
    put_json_number(&line, volume);
    put_str(&line, ",\"db\":");
    put_json_number(&line, db);
    bool ok = send_message_locked(&line);
    pthread_mutex_unlock(&mutex);
    return ok;
}

bool hls_forward_audio(const char *state, const char *codec, bool screen) {
    /* AirPlay audio (no video session): "start" (with codec and screen), "flush" or "stop" */
    char data[512];
    line_t line = { data, 0, false };
    pthread_mutex_lock(&mutex);
    begin_message(&line, "audio", NULL);
    put_str(&line, ",\"state\":");
    put_json_string(&line, state);
    if (codec) {
        put_str(&line, ",\"codec\":");
        put_json_string(&line, codec);
        put_str(&line, screen ? ",\"screen\":true" : ",\"screen\":false");
    }
    bool ok = send_message_locked(&line);
    pthread_mutex_unlock(&mutex);
    return ok;
}

bool hls_forward_metadata(const char *title, const char *artist, const char *album) {
    /* the non-empty fields only */
    char *data = (char *) malloc(MAX_LINE);   /* put() limits a line to MAX_LINE bytes */
    if (!data) {
        return false;
    }
    line_t line = { data, 0, false };
    pthread_mutex_lock(&mutex);
    begin_message(&line, "metadata", NULL);
    if (title && title[0]) {
        put_str(&line, ",\"title\":");
        put_json_string(&line, title);
    }
    if (artist && artist[0]) {
        put_str(&line, ",\"artist\":");
        put_json_string(&line, artist);
    }
    if (album && album[0]) {
        put_str(&line, ",\"album\":");
        put_json_string(&line, album);
    }
    bool ok = send_message_locked(&line);
    pthread_mutex_unlock(&mutex);
    free(data);
    return ok;
}

bool hls_forward_coverart(int bytes) {
    char data[128];
    line_t line = { data, 0, false };
    pthread_mutex_lock(&mutex);
    begin_message(&line, "coverart", NULL);
    put_str(&line, ",\"bytes\":");
    put_json_number(&line, bytes > 0 ? (double) bytes : 0.0);
    bool ok = send_message_locked(&line);
    pthread_mutex_unlock(&mutex);
    return ok;
}

bool hls_forward_disconnect(const char *reason) {
    return send_session_message("disconnect", NULL, 0.0, reason, true);
}

bool hls_forward_get_status(hls_forward_status_t *out) {
    pthread_mutex_lock(&mutex);
    bool current = (session[0] != '\0');
    if (current) {
        if (have_status) {
            *out = status;
        } else {
            memset(out, 0, sizeof(*out));   /* not ready until the first status */
        }
        if (status.finished) {
            session[0] = '\0';   /* reported once; the session is over */
            have_status = false;
        }
    }
    pthread_mutex_unlock(&mutex);
    return current;
}

#endif
