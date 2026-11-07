#include "api.h"
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int safe_name(const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 80 || s[0] == '.') return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-' || *s == '.')) return 0;
    return 1;
}

static int qget(const char *q, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    out[0] = 0;
    while (q && *q) {
        if (!strncmp(q, key, kl) && q[kl] == '=') {
            q += kl + 1;
            size_t n = 0;
            while (*q && *q != '&' && n + 1 < cap) out[n++] = *q++;
            out[n] = 0;
            return 0;
        }
        q = strchr(q, '&');
        if (q) q++;
    }
    return -1;
}

static int parse_slot(const char *q, unsigned *slot) {
    char v[16];
    if (qget(q, "slot", v, sizeof v) || !*v) return -1;
    char *end = NULL;
    long s = strtol(v, &end, 10);
    if (*end || s < 0 || s >= SKY_SLOTS) return -1;
    *slot = (unsigned)s;
    return 0;
}

static void fig_path(const struct sky_api *a, const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/figures/%s", a->root, name);
}

static void persist(const struct sky_api *a) {
    char p[256];
    snprintf(p, sizeof p, "%s/skyportal_etahen_selection.txt", a->root);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (unsigned i = 0; i < SKY_SLOTS; i++) fprintf(f, "%u=%s\n", i, a->slot_name[i]);
    fclose(f);
}

void sky_api_init(struct sky_api *a, const char *root, const struct sky_api_link *link) {
    memset(a, 0, sizeof *a);
    pthread_mutex_init(&a->lock, NULL);
    a->autosave = 1;
    snprintf(a->root, sizeof a->root, "%s", root);
    if (link) a->link = *link;
    char p[256];
    mkdir(a->root, 0777);
    snprintf(p, sizeof p, "%s/figures", a->root);
    mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/skyportal_etahen_selection.txt", a->root);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            unsigned i; char name[96];
            if (sscanf(line, "%u=%95s", &i, name) == 2 && i < SKY_SLOTS && safe_name(name))
                snprintf(a->slot_name[i], sizeof a->slot_name[i], "%s", name);
        }
        fclose(f);
    }
}

static int reply(char *out, size_t cap, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static int reply(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    *len = (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
    return 0;
}

static int read_figure_file(const struct sky_api *a, const char *name, uint8_t *buf) {
    char p[256];
    fig_path(a, name, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, SKY_FIG_SIZE, f);
    int extra = fgetc(f);
    fclose(f);
    return (n == SKY_FIG_SIZE && extra == EOF) ? 0 : -2;
}


static void alog(const struct sky_api *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void alog(const struct sky_api *a, const char *fmt, ...) {
    if (!a->log) return;
    char m[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    a->log(a->log_ctx, m);
}

/* Keep the very first version of a figure file in <root>/backup/ before it is overwritten for the first time. */
static void backup_once(const struct sky_api *a, const char *name) {
    char dir[256], src[256], dst[300];
    snprintf(dir, sizeof dir, "%s/backup", a->root);
    mkdir(dir, 0777);
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    if (access(dst, F_OK) == 0) return;                      /* already have one */
    fig_path(a, name, src, sizeof src);
    FILE *in = fopen(src, "rb");
    if (!in) return;
    uint8_t buf[SKY_FIG_SIZE];
    size_t n = fread(buf, 1, sizeof buf, in);
    fclose(in);
    if (n != SKY_FIG_SIZE) return;
    FILE *out = fopen(dst, "wb");
    if (!out) return;
    fwrite(buf, 1, n, out);
    fclose(out);
}

/* Atomic replace: write <file>.tmp, flush it to disk, rename over the figure file. */
static int save_figure_file(const struct sky_api *a, const char *name, const uint8_t *data) {
    char p[256], t[272];
    fig_path(a, name, p, sizeof p);
    backup_once(a, name);
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, SKY_FIG_SIZE, f);
    int ok = w == SKY_FIG_SIZE && fflush(f) == 0;
    if (ok) fsync(fileno(f));
    fclose(f);
    if (!ok || rename(t, p)) { remove(t); return -1; }
    return 0;
}

static void track_set(struct sky_api *a, unsigned slot, const uint8_t *data) {
    struct sky_track *t = &a->track[slot];
    memcpy(t->shadow, data, SKY_FIG_SIZE);
    t->have = 1;
    t->have_last = 0;
}

static void track_clear(struct sky_api *a, unsigned slot) {
    a->track[slot].have = 0;
    a->track[slot].have_last = 0;
}

/* Compare the game's copy of one slot with what is on disk and save it if it changed.
 *   force = 0 (autosave tick): only save a changed copy that was identical on the previous poll too,
 *                              so a burst of block writes is saved once, after it has finished;
 *   force = 1 (Save button, before a slot is changed): save right away.
 * Returns 1 if a file was written, 0 if there was nothing to do, <0 on error. Caller holds the lock. */
static int sync_slot(struct sky_api *a, unsigned slot, int force) {
    struct sky_track *t = &a->track[slot];
    if (!t->have || !a->slot_name[slot][0]) return 0;
    if (!a->link.connected || !a->link.connected(a->link.ctx) || !a->link.read) return 0;
    uint8_t cur[SKY_FIG_SIZE]; int dirty = 0;
    if (a->link.read(a->link.ctx, slot, cur, &dirty)) return -1;
    if (!dirty) return 0;                                    /* the game never wrote to it */
    /* Block 0 (UID etc.) never changes. If it differs, the slot still holds another figure (a load or remove
     * request is queued and not applied yet): never write that figure's data into this figure's file. */
    if (memcmp(cur, t->shadow, SKY_BLOCK)) return 0;
    if (!memcmp(cur, t->shadow, SKY_FIG_SIZE)) { t->have_last = 0; return 0; }
    if (!force && !(t->have_last && !memcmp(cur, t->last, SKY_FIG_SIZE))) {
        memcpy(t->last, cur, SKY_FIG_SIZE);
        t->have_last = 1;
        return 0;
    }
    if (save_figure_file(a, a->slot_name[slot], cur)) {
        alog(a, "autosave: slot %u: cannot write %s", slot, a->slot_name[slot]);
        return -1;
    }
    memcpy(t->shadow, cur, SKY_FIG_SIZE);
    t->have_last = 0;
    a->saves++;
    snprintf(a->last_saved, sizeof a->last_saved, "%s", a->slot_name[slot]);
    alog(a, "saved slot %u -> %s", slot, a->slot_name[slot]);
    return 1;
}

int sky_api_autosave(struct sky_api *a) {
    int n = 0;
    pthread_mutex_lock(&a->lock);
    if (a->autosave && a->link.connected && a->link.connected(a->link.ctx))
        for (unsigned i = 0; i < SKY_SLOTS; i++) {
            int r = sync_slot(a, i, 0);
            if (r > 0) n += r;
        }
    pthread_mutex_unlock(&a->lock);
    return n;
}

void sky_api_reset(struct sky_api *a) {
    pthread_mutex_lock(&a->lock);
    for (unsigned i = 0; i < SKY_SLOTS; i++) { a->slot_name[i][0] = 0; track_clear(a, i); }
    pthread_mutex_unlock(&a->lock);
}

static int api_handle(struct sky_api *a, const char *method, const char *target,
                      const uint8_t *body, size_t body_len,
                      char *out, size_t cap, size_t *out_len, const char **ctype);

int sky_api_handle(struct sky_api *a, const char *method, const char *target,
                   const uint8_t *body, size_t body_len,
                   char *out, size_t cap, size_t *out_len, const char **ctype) {
    pthread_mutex_lock(&a->lock);
    int rc = api_handle(a, method, target, body, body_len, out, cap, out_len, ctype);
    pthread_mutex_unlock(&a->lock);
    return rc;
}

static int api_handle(struct sky_api *a, const char *method, const char *target,
                      const uint8_t *body, size_t body_len,
                      char *out, size_t cap, size_t *out_len, const char **ctype) {
    char path[128];
    const char *q = strchr(target, '?');
    size_t pl = q ? (size_t)(q - target) : strlen(target);
    if (pl >= sizeof path) pl = sizeof path - 1;
    memcpy(path, target, pl); path[pl] = 0;
    q = q ? q + 1 : "";
    *ctype = "application/json";
    *out_len = 0;

#define BAD(msg) do { reply(out, cap, out_len, "{\"ok\":false,\"error\":\"%s\"}", msg); return 400; } while (0)

    if (!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
        int c = a->link.connected ? a->link.connected(a->link.ctx) : 0;
        reply(out, cap, out_len,
              "{\"ok\":true,\"service\":\"SkyPortal\",\"game_hooked\":%s,\"slots\":%d,"
              "\"autosave\":%s,\"saves\":%u,\"last_saved\":\"%s\"}",
              c ? "true" : "false", SKY_SLOTS, a->autosave ? "true" : "false", (unsigned)a->saves, a->last_saved);
        return 200;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/slots")) {
        size_t n = 0;
        n += (size_t)snprintf(out + n, cap - n, "{\"ok\":true,\"slots\":[");
        for (unsigned i = 0; i < SKY_SLOTS && n < cap; i++)
            n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"", i ? "," : "", a->slot_name[i]);
        if (n < cap) n += (size_t)snprintf(out + n, cap - n, "]}");
        if (n >= cap) return 500;
        *out_len = n;
        return 200;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/figures")) {
        char dp[256];
        snprintf(dp, sizeof dp, "%s/figures", a->root);
        DIR *d = opendir(dp);
        size_t n = (size_t)snprintf(out, cap, "{\"ok\":true,\"figures\":[");
        int first = 1;
        struct dirent *e;
        while (d && (e = readdir(d)) && n + 100 < cap) {
            if (!safe_name(e->d_name)) continue;
            n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"", first ? "" : ",", e->d_name);
            first = 0;
        }
        if (d) closedir(d);
        n += (size_t)snprintf(out + n, cap - n, "]}");
        if (n >= cap) return 500;
        *out_len = n;
        return 200;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/figure")) {      /* upload a 1024-byte dump */
        char name[96];
        if (qget(q, "name", name, sizeof name) || !safe_name(name)) BAD("bad name");
        if (!body || body_len != SKY_FIG_SIZE) BAD("figure dump must be exactly 1024 bytes");
        char p[256]; fig_path(a, name, p, sizeof p);
        FILE *f = fopen(p, "wb");
        if (!f) BAD("cannot write");
        size_t w = fwrite(body, 1, body_len, f);
        fclose(f);
        if (w != body_len) BAD("short write");
        reply(out, cap, out_len, "{\"ok\":true}");
        return 200;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/slot")) {        /* load (figure=name) or remove (figure empty) */
        unsigned slot; char name[96]; int pending = 0, rc;
        if (parse_slot(q, &slot)) BAD("bad slot");
        if (qget(q, "figure", name, sizeof name)) name[0] = 0;
        if (!a->link.connected || !a->link.connected(a->link.ctx)) BAD("no hooked game");
        if (a->autosave && a->slot_name[slot][0]) sync_slot(a, slot, 1);    /* don't lose progress on the old figure */
        if (!name[0]) {
            rc = a->link.remove(a->link.ctx, slot);
            if (rc == -2) pending = 1;
            else if (rc) { char em[64]; snprintf(em, sizeof em, "remove failed, code %d", rc); BAD(em); }
            a->slot_name[slot][0] = 0;
            track_clear(a, slot);
        } else {
            uint8_t fig[SKY_FIG_SIZE];
            if (!safe_name(name)) BAD("bad figure name");
            if (read_figure_file(a, name, fig)) BAD("figure missing or not 1024 bytes");
            rc = a->link.load(a->link.ctx, slot, fig);
            if (rc == -2) pending = 1;
            else if (rc) { char em[64]; snprintf(em, sizeof em, "load failed, code %d", rc); BAD(em); }
            snprintf(a->slot_name[slot], sizeof a->slot_name[slot], "%s", name);
            track_set(a, slot, fig);
        }
        persist(a);
        reply(out, cap, out_len, pending ? "{\"ok\":true,\"pending\":true}" : "{\"ok\":true}");
        return 200;
    }
    if (!strcmp(method, "POST") && !strcmp(path, "/api/save")) {        /* write game's copy back to its file */
        unsigned slot;
        if (parse_slot(q, &slot)) BAD("bad slot");
        if (!a->slot_name[slot][0]) BAD("slot empty");
        if (!a->link.connected || !a->link.connected(a->link.ctx)) BAD("no hooked game");
        if (!a->track[slot].have) BAD("figure was not loaded in this session");
        int r = sync_slot(a, slot, 1);
        if (r < 0) BAD("save failed");
        reply(out, cap, out_len, "{\"ok\":true,\"saved\":%s}", r > 0 ? "true" : "false");
        return 200;
    }
    if (!strcmp(method, "GET") && !strcmp(path, "/api/dump")) {         /* raw current figure data */
        unsigned slot;
        if (parse_slot(q, &slot)) BAD("bad slot");
        uint8_t fig[SKY_FIG_SIZE]; int dirty = 0;
        if (cap < SKY_FIG_SIZE || !a->link.read || a->link.read(a->link.ctx, slot, fig, &dirty)) BAD("read failed");
        memcpy(out, fig, SKY_FIG_SIZE);
        *out_len = SKY_FIG_SIZE;
        *ctype = "application/octet-stream";
        return 200;
    }
    reply(out, cap, out_len, "{\"ok\":false,\"error\":\"not found\"}");
    return 404;
}
