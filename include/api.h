/* Socket-free request handler for the phone UI. http_srv.c is the only socket code. */
#ifndef SKY_API_H
#define SKY_API_H
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include "sky_portal.h"

struct sky_api_link {
    void *ctx;
    int (*connected)(void *ctx);                                  /* hook installed in a running game */
    int (*load)(void *ctx, unsigned slot, const uint8_t *fig);
    int (*remove)(void *ctx, unsigned slot);
    int (*read)(void *ctx, unsigned slot, uint8_t *out, int *dirty);
};

/* Autosave bookkeeping for one slot: what the figure file on disk currently holds, and the last
 * (not yet saved) game copy, used to wait until the game has stopped writing before saving. */
struct sky_track {
    uint8_t shadow[SKY_FIG_SIZE];
    uint8_t last[SKY_FIG_SIZE];
    uint8_t have;                      /* shadow is valid (the slot was loaded by this session) */
    uint8_t have_last;                 /* last holds a changed copy seen on the previous poll */
};

struct sky_api {
    char root[128];                    /* e.g. /data/skylanders */
    struct sky_api_link link;
    char slot_name[SKY_SLOTS][96];     /* what the phone UI believes is in each slot */
    pthread_mutex_t lock;              /* the HTTP thread and the autosave tick share slots + tracking */
    struct sky_track track[SKY_SLOTS];
    int autosave;                      /* 1 = write game progress back to the figure files automatically */
    uint32_t saves;                    /* number of figure files written since start */
    char last_saved[96];               /* name of the figure file written last */
    void *log_ctx;
    void (*log)(void *ctx, const char *msg);   /* optional */
};

#define SKY_API_MAX_BODY (SKY_FIG_SIZE)

void sky_api_init(struct sky_api *a, const char *root, const struct sky_api_link *link);

/* Autosave tick (call a few times per second while a game is hooked). Reads every loaded figure, and once the
 * game has stopped changing one, writes it back to its figure file (atomically; the first overwrite also keeps
 * the original in <root>/backup/). Returns the number of files written. */
int  sky_api_autosave(struct sky_api *a);
/* Forget the slots and what was tracked for them (the game process went away). */
void sky_api_reset(struct sky_api *a);

/* Returns an HTTP status code. Response body goes to out (cap bytes), length in *out_len. */
int sky_api_handle(struct sky_api *a, const char *method, const char *target,
                   const uint8_t *body, size_t body_len,
                   char *out, size_t cap, size_t *out_len, const char **content_type);
#endif
