#include "api.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int connected_flag = 1, loads, removes;
static uint8_t game_copy[SKY_FIG_SIZE]; static int game_dirty;
static int l_conn(void *c) { (void)c; return connected_flag; }
static int l_load(void *c, unsigned s, const uint8_t *f) { (void)c; (void)s; memcpy(game_copy, f, SKY_FIG_SIZE); loads++; return 0; }
static int l_rem(void *c, unsigned s) { (void)c; (void)s; removes++; return 0; }
static int l_read(void *c, unsigned s, uint8_t *o, int *d) { (void)c; (void)s; memcpy(o, game_copy, SKY_FIG_SIZE); *d = game_dirty; return 0; }

static int call(struct sky_api *a, const char *m, const char *t, const void *body, size_t n, char *out, size_t *len) {
    const char *ct;
    return sky_api_handle(a, m, t, body, n, out, 4096, len, &ct);
}

int main(void) {
    char dir[] = "/tmp/skyapiXXXXXX";
    assert(mkdtemp(dir));
    struct sky_api_link lk = { NULL, l_conn, l_load, l_rem, l_read };
    static struct sky_api a;
    sky_api_init(&a, dir, &lk);
    static char out[4096]; size_t n;
    uint8_t fig[SKY_FIG_SIZE]; for (int i = 0; i < SKY_FIG_SIZE; i++) fig[i] = (uint8_t)i;

    assert(call(&a, "GET", "/api/status", NULL, 0, out, &n) == 200 && strstr(out, "\"game_hooked\":true"));
    /* upload validation */
    assert(call(&a, "POST", "/api/figure?name=spyro.bin", fig, 100, out, &n) == 400);          /* wrong size */
    assert(call(&a, "POST", "/api/figure?name=../evil", fig, SKY_FIG_SIZE, out, &n) == 400);    /* traversal */
    assert(call(&a, "POST", "/api/figure?name=.hidden", fig, SKY_FIG_SIZE, out, &n) == 400);
    assert(call(&a, "POST", "/api/figure?name=spyro.bin", fig, SKY_FIG_SIZE, out, &n) == 200);
    assert(call(&a, "GET", "/api/figures", NULL, 0, out, &n) == 200 && strstr(out, "\"spyro.bin\""));

    /* load / persist / remove */
    assert(call(&a, "POST", "/api/slot?slot=3&figure=spyro.bin", NULL, 0, out, &n) == 200 && loads == 1);
    assert(!memcmp(game_copy, fig, SKY_FIG_SIZE));
    assert(call(&a, "POST", "/api/slot?slot=16&figure=spyro.bin", NULL, 0, out, &n) == 400);
    assert(call(&a, "POST", "/api/slot?slot=3&figure=missing.bin", NULL, 0, out, &n) == 400);
    static struct sky_api b; sky_api_init(&b, dir, &lk);                     /* selection survives restart */
    assert(!strcmp(b.slot_name[3], "spyro.bin"));

    /* save-back only when the game dirtied the figure */
    assert(call(&a, "POST", "/api/save?slot=3", NULL, 0, out, &n) == 200 && strstr(out, "\"saved\":false"));
    game_copy[100] ^= 0xFF; game_dirty = 1;
    assert(call(&a, "POST", "/api/save?slot=3", NULL, 0, out, &n) == 200 && strstr(out, "\"saved\":true"));
    char p[200]; snprintf(p, sizeof p, "%s/figures/spyro.bin", dir);
    FILE *f = fopen(p, "rb"); uint8_t disk[SKY_FIG_SIZE]; assert(f && fread(disk, 1, SKY_FIG_SIZE, f) == SKY_FIG_SIZE); fclose(f);
    assert(!memcmp(disk, game_copy, SKY_FIG_SIZE));
    const char *ct; assert(sky_api_handle(&a, "GET", "/api/dump?slot=3", NULL, 0, out, 4096, &n, &ct) == 200 && n == SKY_FIG_SIZE);

    assert(call(&a, "POST", "/api/slot?slot=3&figure=", NULL, 0, out, &n) == 200 && removes == 1 && !a.slot_name[3][0]);

    /* ---- autosave ---- */
    memcpy(game_copy, fig, SKY_FIG_SIZE); game_dirty = 0;
    assert(call(&a, "POST", "/api/slot?slot=4&figure=spyro.bin", NULL, 0, out, &n) == 200);
    assert(sky_api_autosave(&a) == 0);                                   /* untouched figure: nothing written */
    game_copy[200] ^= 0x5A; game_dirty = 1;                              /* the game writes progress */
    assert(sky_api_autosave(&a) == 0);                                   /* first poll: could still be mid-burst */
    game_copy[201] ^= 0x33;                                              /* ...and it was: still changing */
    assert(sky_api_autosave(&a) == 0);
    assert(sky_api_autosave(&a) == 1);                                   /* stable on two polls: saved */
    assert(sky_api_autosave(&a) == 0);                                   /* and not again */
    f = fopen(p, "rb"); assert(f && fread(disk, 1, SKY_FIG_SIZE, f) == SKY_FIG_SIZE); fclose(f);
    assert(!memcmp(disk, game_copy, SKY_FIG_SIZE));
    char bp[200]; snprintf(bp, sizeof bp, "%s/backup/spyro.bin", dir);   /* original preserved on first overwrite */
    f = fopen(bp, "rb"); assert(f && fread(disk, 1, SKY_FIG_SIZE, f) == SKY_FIG_SIZE); fclose(f);
    assert(!memcmp(disk, fig, SKY_FIG_SIZE));
    assert(a.saves >= 2 && !strcmp(a.last_saved, "spyro.bin"));
    assert(call(&a, "GET", "/api/status", NULL, 0, out, &n) == 200 && strstr(out, "\"autosave\":true") && strstr(out, "\"last_saved\":\"spyro.bin\""));

    /* a queued load: the slot still holds the OLD figure; its data must never land in the new figure's file */
    uint8_t other[SKY_FIG_SIZE]; for (int i = 0; i < SKY_FIG_SIZE; i++) other[i] = (uint8_t)(255 - i);
    assert(call(&a, "POST", "/api/figure?name=cynder.bin", other, SKY_FIG_SIZE, out, &n) == 200);
    uint8_t spyro_now[SKY_FIG_SIZE]; memcpy(spyro_now, game_copy, SKY_FIG_SIZE);
    assert(call(&a, "POST", "/api/slot?slot=4&figure=", NULL, 0, out, &n) == 200);              /* (mock returns one copy for every slot) */
    assert(call(&a, "POST", "/api/slot?slot=5&figure=cynder.bin", NULL, 0, out, &n) == 200);   /* game_copy = cynder now */
    memcpy(game_copy, spyro_now, SKY_FIG_SIZE); game_copy[300] ^= 1; game_dirty = 1;           /* game still shows spyro */
    assert(sky_api_autosave(&a) == 0 && sky_api_autosave(&a) == 0);
    char cp[200]; snprintf(cp, sizeof cp, "%s/figures/cynder.bin", dir);
    f = fopen(cp, "rb"); assert(f && fread(disk, 1, SKY_FIG_SIZE, f) == SKY_FIG_SIZE); fclose(f);
    assert(!memcmp(disk, other, SKY_FIG_SIZE));

    /* changing a slot flushes the old figure's unsaved progress first */
    memcpy(game_copy, other, SKY_FIG_SIZE); game_copy[500] ^= 0x7F; game_dirty = 1;
    assert(call(&a, "POST", "/api/slot?slot=5&figure=", NULL, 0, out, &n) == 200);
    f = fopen(cp, "rb"); assert(f && fread(disk, 1, SKY_FIG_SIZE, f) == SKY_FIG_SIZE); fclose(f);
    assert(!memcmp(disk, game_copy, SKY_FIG_SIZE));

    /* autosave off: only Save now writes */
    memcpy(game_copy, fig, SKY_FIG_SIZE); game_dirty = 0; a.autosave = 0;
    assert(call(&a, "POST", "/api/slot?slot=6&figure=spyro.bin", NULL, 0, out, &n) == 200);
    game_copy[10 + 16] ^= 1; game_dirty = 1;
    assert(sky_api_autosave(&a) == 0 && sky_api_autosave(&a) == 0);
    assert(call(&a, "POST", "/api/save?slot=6", NULL, 0, out, &n) == 200 && strstr(out, "\"saved\":true"));
    a.autosave = 1;
    sky_api_reset(&a);                                                   /* game gone: names + tracking dropped */
    assert(!a.slot_name[4][0] && !a.track[4].have);

    /* no game: control endpoints refuse */
    connected_flag = 0;
    assert(call(&a, "POST", "/api/slot?slot=1&figure=spyro.bin", NULL, 0, out, &n) == 400);
    assert(call(&a, "GET", "/api/nope", NULL, 0, out, &n) == 404);
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); (void)!system(cmd);
    puts("api tests passed");
    return 0;
}
