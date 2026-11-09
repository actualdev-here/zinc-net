/* Optional Linux/libcurl transport ABI. No shell, subprocess or token logging.
 * net.call returns a borrowed thread-local JSON envelope, valid until next call.
 * Zinc wrappers copy fields before making another call. Single-threaded handles.
 */
#define _POSIX_C_SOURCE 200809L
#include <curl/curl.h>
#include <curl/websockets.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <time.h>

#if LIBCURL_VERSION_NUM < 0x080200
#error "Zinc networking requires libcurl >= 8.2 with WebSocket support"
#endif
#define CAP 65536
#define SLOTS 16
static _Thread_local char result[CAP * 6 + 8192];
static _Thread_local size_t used;
static _Thread_local int overflow;
static int initialized;

static void add(const char *s) {
    size_t n = strlen(s);
    if (n >= sizeof(result) - used) { overflow = 1; return; }
    memcpy(result + used, s, n + 1); used += n;
}
static void integer(long value) {
    char b[32]; snprintf(b, sizeof b, "%ld", value); add(b);
}
static void quote(const char *s) {
    add("\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        char b[8];
        if (*p == '"' || *p == '\\') { b[0] = '\\'; b[1] = *p; b[2] = 0; }
        else if (*p < 32) snprintf(b, sizeof b, "\\u%04x", *p);
        else { b[0] = *p; b[1] = 0; }
        add(b);
    }
    add("\"");
}
static char *error(const char *message) {
    used = 0; overflow = 0; result[0] = 0;
    add("{\"ok\":false,\"error\":"); quote(message); add("}");
    return result;
}
static char *value(long n) {
    add("{\"ok\":true,\"value\":"); integer(n); add("}"); return result;
}
static int64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int numeric(const char *s, int max, int *out) {
    if (!*s) return 0;
    long n = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || n > (max - (*s - '0')) / 10) return 0;
        n = n * 10 + *s - '0';
    }
    *out = (int)n; return 1;
}
static int clean(const char *s, size_t cap) {
    if (strlen(s) > cap) return 0;
    for (; *s; ++s) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return 0;
    return 1;
}
static int utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned c = s[i++];
        if (!c) return 0;
        if (c < 128) continue;
        unsigned count, code, min;
        if (c >= 194 && c <= 223) { count = 1; code = c & 31; min = 128; }
        else if (c >= 224 && c <= 239) { count = 2; code = c & 15; min = 2048; }
        else if (c >= 240 && c <= 244) { count = 3; code = c & 7; min = 65536; }
        else return 0;
        if (count > n - i) return 0;
        while (count--) { c = s[i++]; if ((c & 192) != 128) return 0; code = (code << 6) | (c & 63); }
        if (code < min || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}
static int url_valid(const char *url, int ws) {
    if (strlen(url) > 2048 || !clean(url, 2048)) return 0;
    CURLU *u = curl_url();
    if (!u) return 0;
    char *scheme = NULL, *host = NULL, *user = NULL, *fragment = NULL;
    int ok = !curl_url_set(u, CURLUPART_URL, url, 0) &&
        !curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) &&
        !curl_url_get(u, CURLUPART_HOST, &host, 0) && *host &&
        !strcmp(scheme, ws ? "wss" : "https") &&
        curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_NO_USER &&
        curl_url_get(u, CURLUPART_FRAGMENT, &fragment, 0) == CURLUE_NO_FRAGMENT;
    curl_free(scheme); curl_free(host); curl_free(user); curl_free(fragment); curl_url_cleanup(u);
    return ok;
}
static CURL *setup(const char *url, int ws) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
#define SET(option, val) do { if (curl_easy_setopt(c, option, val) != CURLE_OK) goto fail; } while (0)
    SET(CURLOPT_URL, url);
    SET(CURLOPT_PROTOCOLS_STR, ws ? "wss" : "https");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CONNECTTIMEOUT_MS, 5000L); SET(CURLOPT_TIMEOUT_MS, 15000L);
    SET(CURLOPT_NOSIGNAL, 1L); SET(CURLOPT_PROXY, "");
    SET(CURLOPT_USERAGENT, "ZincDiscord/0.1 (https://github.com/ItsParadoxism/Zinc)");
    const char *ca = getenv("ZINC_NET_CA_FILE");
    if (ca && *ca) SET(CURLOPT_CAINFO, ca);
    return c;
fail:
    curl_easy_cleanup(c); return NULL;
#undef SET
}
struct response {
    char body[CAP + 1], bucket[129];
    size_t n, headers;
    int retry, reset, remaining, invalid, global;
};
static size_t body_cb(char *p, size_t a, size_t b, void *ctx) {
    struct response *r = ctx;
    if (b && a > SIZE_MAX / b) return 0;
    size_t n = a * b;
    if (n > CAP - r->n || memchr(p, 0, n)) return 0;
    memcpy(r->body + r->n, p, n); r->n += n; r->body[r->n] = 0; return n;
}
static int milliseconds(const char *s) {
    char *end;
    errno = 0; double sec = strtod(s, &end);
    while (*end == ' ' || *end == '\r' || *end == '\n') ++end;
    if (errno || end == s || *end || !isfinite(sec) || sec < 0 || sec > 86400) return -1;
    return (int)ceil(sec * 1000);
}
static size_t header_cb(char *p, size_t a, size_t b, void *ctx) {
    struct response *r = ctx;
    if (b && a > SIZE_MAX / b) return 0;
    size_t n = a * b;
    if (n > 4096 || r->headers + n > 32768 || memchr(p, 0, n)) return 0;
    r->headers += n;
    char line[4097]; memcpy(line, p, n); line[n] = 0;
    char *colon = strchr(line, ':');
    if (!colon) return n;
    *colon++ = 0;
    for (char *x = line; *x; ++x) if (*x >= 'A' && *x <= 'Z') *x += 32;
    while (*colon == ' ' || *colon == '\t') ++colon;
    if (!strcmp(line, "retry-after")) { r->retry = milliseconds(colon); if (r->retry < 0) r->invalid = 1; }
    if (!strcmp(line, "x-ratelimit-reset-after")) { r->reset = milliseconds(colon); if (r->reset < 0) r->invalid = 1; }
    if (!strcmp(line, "x-ratelimit-remaining")) {
        colon[strcspn(colon, "\r\n")] = 0;
        if (!numeric(colon, INT_MAX, &r->remaining)) r->invalid = 1;
    }
    if (!strcmp(line, "x-ratelimit-bucket")) {
        colon[strcspn(colon, "\r\n")] = 0;
        if (!clean(colon, 128)) r->invalid = 1;
        else strcpy(r->bucket, colon);
    }
    if (!strcmp(line, "x-ratelimit-global")) {
        colon[strcspn(colon, "\r\n")] = 0;
        if (!strcmp(colon, "true")) r->global = 1;
        else if (strcmp(colon, "false")) r->invalid = 1;
    }
    if (!strcmp(line, "x-ratelimit-scope")) {
        colon[strcspn(colon, "\r\n")] = 0;
        if (!strcmp(colon, "global")) r->global = 1;
    }
    return n;
}
static int progress(void *context, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return atomic_load((atomic_int *)context);
}
static char *http(const char *url, const char *method, const char *auth, const char *body, atomic_int *cancel, int timeout) {
    if (!url_valid(url, 0) || !clean(auth, 1024) || strlen(body) > CAP ||
        (strcmp(method, "GET") && strcmp(method, "POST") && strcmp(method, "PUT") && strcmp(method, "PATCH") && strcmp(method, "DELETE"))) return error("invalid HTTPS request");
    CURL *c = setup(url, 0);
    if (!c) return error("HTTPS setup failed");
    if (curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeout) ||
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout < 5000 ? timeout : 5000))) {
        curl_easy_cleanup(c); return error("HTTPS timeout setup failed");
    }
    struct response *r = calloc(1, sizeof *r);
    struct curl_slist *headers = NULL, *next;
    CURLcode code = CURLE_OUT_OF_MEMORY;
    if (!r) goto done;
    if (cancel && (curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L) ||
                  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress) ||
                  curl_easy_setopt(c, CURLOPT_XFERINFODATA, cancel))) goto done;
    r->retry = r->reset = r->remaining = -1;
    headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (!headers) goto done;
    if (*auth) {
        char line[1050]; snprintf(line, sizeof line, "Authorization: %s", auth);
        next = curl_slist_append(headers, line); if (!next) goto done; headers = next;
    }
    if (curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers) ||
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method) ||
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body_cb) ||
        curl_easy_setopt(c, CURLOPT_WRITEDATA, r) ||
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb) ||
        curl_easy_setopt(c, CURLOPT_HEADERDATA, r)) goto done;
    if (*body && (curl_easy_setopt(c, CURLOPT_POSTFIELDS, body) ||
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body)))) goto done;
    code = curl_easy_perform(c);
done:
    if (code != CURLE_OK || !r || r->invalid || !utf8((unsigned char *)r->body, r->n)) {
        curl_slist_free_all(headers); curl_easy_cleanup(c); free(r);
        return error("HTTPS transfer failed (TLS, timeout, invalid headers or response limit)");
    }
    long status = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    add("{\"ok\":true,\"status\":"); integer(status); add(",\"body\":"); quote(r->body);
    add(",\"retry_ms\":"); integer(r->retry); add(",\"reset_ms\":"); integer(r->reset);
    add(",\"remaining\":"); integer(r->remaining); add(",\"bucket\":"); quote(r->bucket);
    add(r->global ? ",\"global\":true}" : ",\"global\":false}");
    curl_slist_free_all(headers); curl_easy_cleanup(c); free(r);
    return overflow ? error("response envelope limit") : result;
}
struct transfer {
    pthread_t worker; int id, timeout;
    atomic_int done, cancel;
    char *args[4], *output;
};
static _Thread_local struct transfer transfers[4];
static _Thread_local int last_transfer;
static void *http_worker(void *context) {
    struct transfer *t = context;
    used = 0; overflow = 0; result[0] = 0;
    char *raw = http(t->args[0], t->args[1], t->args[2], t->args[3], &t->cancel, t->timeout);
    t->output = strdup(raw);
    atomic_store_explicit(&t->done, 1, memory_order_release);
    return NULL;
}
static void free_transfer(struct transfer *t) {
    for (int i = 0; i < 4; ++i) { free(t->args[i]); t->args[i] = NULL; }
    free(t->output); t->output = NULL; t->id = 0;
}
static char *http_start(const char *a, const char *b, const char *c, const char *d, int timeout) {
    const char *args[4] = {a,b,c,d};
    if (strlen(a) > 2048 || strlen(b) > 16 || strlen(c) > 1024 || strlen(d) > CAP) return error("HTTP argument limit");
    struct transfer *t = NULL;
    for (int i = 0; i < 4; ++i) if (!transfers[i].id) { t = &transfers[i]; break; }
    if (!t || last_transfer == INT_MAX) return error("HTTP transfer limit");
    memset(t, 0, sizeof *t);
    t->timeout = timeout;
    for (int i = 0; i < 4; ++i) {
        t->args[i] = strdup(args[i]);
        if (!t->args[i]) { free_transfer(t); return error("HTTP allocation failed"); }
    }
    atomic_init(&t->done, 0); atomic_init(&t->cancel, 0);
    t->id = ++last_transfer;
    if (pthread_create(&t->worker, NULL, http_worker, t)) { free_transfer(t); return error("HTTP worker creation failed"); }
    return value(t->id);
}
static char *http_poll(const char *handle, int cancel) {
    int id; struct transfer *t = NULL;
    if (numeric(handle, INT_MAX, &id))
        for (int i = 0; i < 4; ++i) if (transfers[i].id == id && id) { t = &transfers[i]; break; }
    if (!t) return error("invalid HTTP transfer handle");
    if (cancel) atomic_store(&t->cancel, 1);
    if (!cancel && !atomic_load_explicit(&t->done, memory_order_acquire)) {
        add("{\"ok\":true,\"kind\":\"pending\"}"); return result;
    }
    pthread_join(t->worker, NULL);
    if (cancel) value(1);
    else if (t->output) add(t->output);
    else error("HTTP worker allocation failed");
    free_transfer(t);
    return overflow ? error("HTTP envelope limit") : result;
}
struct websocket {
    CURL *curl; int id;
    char data[CAP + 1]; size_t n;
    unsigned char control[125]; size_t control_n;
};
static _Thread_local struct websocket sockets[SLOTS];
static _Thread_local int last_id;
static struct websocket *lookup(const char *id) {
    int n;
    if (!numeric(id, INT_MAX, &n) || !n) return NULL;
    for (int i = 0; i < SLOTS; ++i) if (sockets[i].curl && sockets[i].id == n) return &sockets[i];
    return NULL;
}
static void dispose(struct websocket *ws) {
    curl_easy_cleanup(ws->curl); memset(ws, 0, sizeof *ws);
}
static int wait_socket(CURL *c, short events, int64_t deadline) {
    curl_socket_t fd;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &fd) || fd == CURL_SOCKET_BAD) return -1;
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) return 0;
        struct pollfd p = {fd, events, 0};
        int n = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n;
        if (p.revents & events) return 1;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    }
}
struct upgrade {
    char key[128], accept[128]; int accepts, invalid, websocket, connection; size_t headers;
};
static int capture_header(const char *data, size_t n, const char *name, char *out, size_t cap) {
    const char *end = data + n;
    size_t wanted = strlen(name);
    while (data < end) {
        const char *stop = memchr(data, '\n', end - data);
        if (!stop) stop = end;
        const char *colon = memchr(data, ':', stop - data);
        if (colon && (size_t)(colon - data) == wanted && !strncasecmp(data, name, wanted)) {
            const char *value = colon + 1, *finish = stop;
            while (value < finish && (*value == ' ' || *value == '\t')) ++value;
            while (finish > value && (finish[-1] == '\r' || finish[-1] == ' ' || finish[-1] == '\t')) --finish;
            size_t len = finish - value;
            if (len >= cap || memchr(value, 0, len)) return -1;
            memcpy(out, value, len); out[len] = 0; return 1;
        }
        data = stop < end ? stop + 1 : end;
    }
    return 0;
}
static int upgrade_debug(CURL *c, curl_infotype kind, char *data, size_t n, void *context) {
    (void)c;
    struct upgrade *u = context;
    if (kind == CURLINFO_HEADER_OUT && capture_header(data, n, "Sec-WebSocket-Key", u->key, sizeof u->key) < 0) u->invalid = 1;
    return 0; /* Never print or retain curl debug traffic. */
}
static size_t upgrade_headers(char *data, size_t a, size_t b, void *context) {
    struct upgrade *u = context;
    if (b && a > SIZE_MAX / b) return 0;
    size_t n = a * b;
    if (n > 4096 || u->headers + n > 32768) return 0;
    u->headers += n;
    if (n >= 5 && !memcmp(data, "HTTP/", 5)) { u->accepts = u->websocket = u->connection = 0; u->accept[0] = 0; }
    int found = capture_header(data, n, "Sec-WebSocket-Accept", u->accept, sizeof u->accept);
    if (found < 0 || (found > 0 && ++u->accepts > 1)) { u->invalid = 1; return 0; }
    char value[256];
    found = capture_header(data, n, "Upgrade", value, sizeof value);
    if (found < 0 || (found && strcasecmp(value, "websocket"))) { u->invalid = 1; return 0; }
    if (found) u->websocket = 1;
    found = capture_header(data, n, "Connection", value, sizeof value);
    if (found < 0) { u->invalid = 1; return 0; }
    if (found) {
        char *part = value;
        while (*part) {
            char *comma = strchr(part, ','); if (comma) *comma = 0;
            while (*part == ' ' || *part == '\t') ++part;
            char *end = part + strlen(part);
            while (end > part && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
            if (!strcasecmp(part, "upgrade")) u->connection = 1;
            if (!comma) break;
            part = comma + 1;
        }
    }
    return n;
}
static int upgrade_valid(CURL *c, struct upgrade *u) {
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 101 || u->invalid || !u->websocket || !u->connection || u->accepts != 1 || strlen(u->key) != 24) return 0;
    char input[128], expected[64]; unsigned char digest[SHA_DIGEST_LENGTH];
    snprintf(input, sizeof input, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", u->key);
    if (!SHA1((unsigned char *)input, strlen(input), digest)) return 0;
    if (EVP_EncodeBlock((unsigned char *)expected, digest, sizeof digest) != 28) return 0;
    return !strcmp(expected, u->accept);
}
static char *ws_open(const char *url) {
    if (!url_valid(url, 1)) return error("invalid WSS URL");
    struct websocket *ws = NULL;
    for (int i = 0; i < SLOTS; ++i) if (!sockets[i].curl) { ws = &sockets[i]; break; }
    if (!ws || last_id == INT_MAX) return error("WebSocket handle limit");
    CURL *c = setup(url, 1);
    if (!c) return error("WSS setup failed");
    struct upgrade upgrade = {0};
    if (curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L) ||
        curl_easy_setopt(c, CURLOPT_VERBOSE, 1L) ||
        curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, upgrade_debug) ||
        curl_easy_setopt(c, CURLOPT_DEBUGDATA, &upgrade) ||
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, upgrade_headers) ||
        curl_easy_setopt(c, CURLOPT_HEADERDATA, &upgrade) ||
        curl_easy_perform(c) || !upgrade_valid(c, &upgrade)) {
        curl_easy_cleanup(c); return error("WSS handshake failed");
    }
    /* The handshake context is stack-owned. Detach all callbacks before returning. */
    if (curl_easy_setopt(c, CURLOPT_VERBOSE, 0L) ||
        curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, NULL) ||
        curl_easy_setopt(c, CURLOPT_DEBUGDATA, NULL) ||
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, NULL) ||
        curl_easy_setopt(c, CURLOPT_HEADERDATA, NULL)) {
        curl_easy_cleanup(c); return error("WSS callback teardown failed");
    }
    memset(ws, 0, sizeof *ws); ws->curl = c; ws->id = ++last_id;
    return value(ws->id);
}
static char *ws_send(struct websocket *ws, const char *message) {
    size_t len = strlen(message), offset = 0;
    if (!len || len > 4096 || !utf8((const unsigned char *)message, len)) return error("invalid outgoing WebSocket text");
    int64_t deadline = now_ms() + 5000;
    while (offset < len) {
        size_t sent = 0;
        CURLcode rc = curl_ws_send(ws->curl, message + offset, len - offset, &sent, 0, CURLWS_TEXT);
        if (rc != CURLE_OK && rc != CURLE_AGAIN) { dispose(ws); return error("WebSocket send failed"); }
        offset += sent;
        if (offset < len && wait_socket(ws->curl, POLLOUT, deadline) != 1) { dispose(ws); return error("WebSocket send timeout"); }
    }
    return value(1);
}
static char *ws_receive(struct websocket *ws, int timeout) {
    int64_t deadline = now_ms() + timeout;
    int first = 1;
    for (;;) {
        if (!first && now_ms() >= deadline) { add("{\"ok\":true,\"kind\":\"timeout\"}"); return result; }
        first = 0;
        unsigned char buf[4096]; size_t n = 0; const struct curl_ws_frame *meta = NULL;
        CURLcode rc = curl_ws_recv(ws->curl, buf, sizeof buf, &n, &meta);
        if (rc == CURLE_AGAIN) {
            int ready = wait_socket(ws->curl, POLLIN, deadline);
            if (ready == 0) { add("{\"ok\":true,\"kind\":\"timeout\"}"); return result; }
            if (ready == 1) continue;
        }
        if (rc != CURLE_OK || !meta) { dispose(ws); return error("WebSocket receive failed"); }
        int flags = meta->flags;
        if (flags & CURLWS_CLOSE) {
            if (!meta->offset) ws->control_n = 0;
            if (n > sizeof(ws->control) - ws->control_n) { dispose(ws); return error("invalid close frame"); }
            memcpy(ws->control + ws->control_n, buf, n); ws->control_n += n;
            if (meta->bytesleft) continue;
            int code = 1005;
            if (ws->control_n == 1) { dispose(ws); return error("invalid close payload"); }
            if (ws->control_n >= 2) code = ws->control[0] * 256 + ws->control[1];
            if (ws->control_n >= 2 && (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015 || (code >= 1016 && code < 3000) || !utf8(ws->control + 2, ws->control_n - 2))) {
                dispose(ws); return error("invalid WebSocket close code/reason");
            }
            /* Acknowledge the peer's Close before dropping transport. Best effort,
             * bounded by 200ms; a dead peer must not prevent shutdown. */
            size_t offset = 0;
            int64_t close_deadline = now_ms() + 200;
            do {
                size_t sent = 0;
                CURLcode replied = curl_ws_send(ws->curl, ws->control + offset, ws->control_n - offset, &sent, 0, CURLWS_CLOSE);
                offset += sent;
                if (replied != CURLE_OK && replied != CURLE_AGAIN) break;
                if (offset == ws->control_n) break;
            } while (wait_socket(ws->curl, POLLOUT, close_deadline) == 1);
            dispose(ws); add("{\"ok\":true,\"kind\":\"close\",\"code\":"); integer(code); add("}"); return result;
        }
        if (flags & (CURLWS_PING | CURLWS_PONG)) continue; /* libcurl automatically pongs */
        if (!(flags & CURLWS_TEXT) || (flags & CURLWS_BINARY) || n > CAP - ws->n) { dispose(ws); return error("unsupported/oversized WebSocket message"); }
        memcpy(ws->data + ws->n, buf, n); ws->n += n;
        if (meta->bytesleft || (flags & CURLWS_CONT)) continue;
        if (!utf8((unsigned char *)ws->data, ws->n)) { dispose(ws); return error("invalid WebSocket UTF-8/NUL"); }
        ws->data[ws->n] = 0;
        add("{\"ok\":true,\"kind\":\"text\",\"data\":"); quote(ws->data); add("}"); ws->n = 0;
        return overflow ? error("WebSocket envelope limit") : result;
    }
}

char *zinc_net_call(const char *op, const char *a, const char *b, const char *c, const char *d) {
    used = 0; overflow = 0; result[0] = 0;
    if (!op || !a || !b || !c || !d) return error("null network argument");
    if (!initialized) { if (curl_global_init(CURL_GLOBAL_DEFAULT)) return error("libcurl initialization failed"); initialized = 1; }
    if (!strcmp(op, "capabilities")) {
        const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
        int wss = 0;
        if (info && info->protocols)
            for (const char *const *p = info->protocols; *p; ++p) if (!strcmp(*p, "wss")) wss = 1;
        add(wss ? "{\"ok\":true,\"wss\":true}" : "{\"ok\":true,\"wss\":false}"); return result;
    }
    if (!strcmp(op, "http")) return http(a, b, c, d, NULL, 15000);
    if (!strcmp(op, "http_start")) return http_start(a, b, c, d, 15000);
    /* Encode the optional budget in the private operation tag: the ABI has four
     * data strings, all already occupied by URL/method/auth/body. No global config. */
    static const char timed[] = "http_start_timed:";
    if (!strncmp(op, timed, sizeof timed - 1)) {
        int timeout;
        if (!numeric(op + sizeof timed - 1, 15000, &timeout) || !timeout) return error("invalid HTTP budget");
        return http_start(a, b, c, d, timeout);
    }
    if (!strcmp(op, "http_poll")) return http_poll(a, 0);
    if (!strcmp(op, "http_cancel")) return http_poll(a, 1);
    if (!strcmp(op, "ws_open")) return ws_open(a);
    if (!strcmp(op, "ws_send") || !strcmp(op, "ws_receive") || !strcmp(op, "ws_close")) {
        struct websocket *ws = lookup(a);
        if (!ws) return error("invalid WebSocket handle");
        if (!strcmp(op, "ws_send")) return ws_send(ws, b);
        if (!strcmp(op, "ws_close")) { dispose(ws); return value(1); } /* drop TCP: preserve Discord resume */
        int timeout;
        if (!numeric(b, 30000, &timeout)) return error("invalid receive timeout");
        return ws_receive(ws, timeout);
    }
    if (!strcmp(op, "clock")) { int64_t now = now_ms(); if (now < 0) return error("clock failed"); return value(now % 1000000000); }
    if (!strcmp(op, "milliseconds")) { int n = milliseconds(a); if (n < 0) return error("invalid seconds delay"); return value(n); }
    if (!strcmp(op, "sleep")) {
        int ms;
        if (!numeric(a, 86400000, &ms)) return error("invalid sleep duration");
        struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
        while (nanosleep(&t, &t)) if (errno != EINTR) return error("sleep failed");
        return value(1);
    }
    if (!strcmp(op, "random")) {
        int bound;
        if (!numeric(a, INT_MAX, &bound) || !bound) return error("invalid random bound");
        uint32_t n, limit = UINT32_MAX - UINT32_MAX % (uint32_t)bound;
        do { if (getrandom(&n, sizeof n, 0) != sizeof n) return error("random source failed"); } while (n >= limit);
        return value(n % (uint32_t)bound);
    }
    return error("unknown network operation");
}
