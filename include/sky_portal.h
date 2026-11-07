/* Skylanders portal protocol state machine.
 *
 * Pure C, no libc dependency beyond what sky_mem.h provides, so it can be
 * linked both into the host tests and into the freestanding in-game hook blob.
 *
 * PROTOCOL SOURCE: written from the publicly documented behaviour of the
 * Activision portal (HID, VID 0x1430 / PID 0x0150, 32-byte reports, commands
 * R/A/S/Q/W/C/J/L/M). It has NOT been diffed against the Trap Team Helper's
 * gamehook blob or captured from a real game on PS5. Treat every response
 * byte as "believed correct" until checked against a capture.
 */
#ifndef SKY_PORTAL_H
#define SKY_PORTAL_H
#include <stddef.h>
#include <stdint.h>

#define SKY_SLOTS     16          /* portal addresses figures by a 4-bit index */
#define SKY_FIG_SIZE  1024        /* 64 blocks x 16 bytes, as in the Helper's dumps */
#define SKY_BLOCK     16
#define SKY_BLOCKS    64
#define SKY_REPORT    32
#define SKY_RESPQ     32

#define SKY_ST_PRESENT 0x1u
#define SKY_ST_CHANGED 0x2u       /* added/removed since last status report */

struct sky_figure {
    volatile uint32_t seq;        /* seqlock: odd while data is being modified */
    uint8_t status;               /* SKY_ST_* */
    uint8_t dirty;                /* game wrote a block since last remote read */
    uint8_t pad[2];
    uint8_t data[SKY_FIG_SIZE];
};

/* What the game asked the portal's lights to do. The PS5 has nothing to drive; this is recorded so it shows up in the
 * log (and could feed the web UI). Byte layouts follow the All-in-One Toypad Emulator's sky_core (Flipper):
 *   C r g b                 ring colour, immediately
 *   L 0 r g b               ring colour, immediately (same as C)
 *   L 1 level               side-light / backlight brightness 0..255
 *   L 3 ...                 brightness to full
 *   J 0 r g b ms_lo ms_hi   fade the ring to r,g,b over ms (16-bit LE, capped at SKY_LIGHT_FADE_MAX_MS)
 * Other sides are ignored. 'J' is only honoured (and acknowledged) at exactly 7 bytes. */
#define SKY_LIGHT_FADE_MAX_MS 5000
struct sky_lights {
    uint8_t  ring[3];
    uint8_t  backlight;
    uint8_t  backlight_set;
    uint8_t  pad;
    uint16_t fade_ms;
    uint32_t seq;                 /* bumped on every change the game asked for */
};

struct sky_portal {
    struct sky_figure fig[SKY_SLOTS];
    uint8_t  resp[SKY_RESPQ][SKY_REPORT];
    uint32_t rhead, rtail;
    uint8_t  active;              /* last 'A' argument */
    uint8_t  counter;             /* status interrupt counter */
    uint8_t  speaker;             /* 'M' turned the speaker on: interrupt-OUT may now carry audio */
    uint8_t  cmd_via_out;         /* the game has sent a real command on interrupt OUT (not only SET_REPORT) */
    struct sky_lights lights;
};

void sky_portal_init(struct sky_portal *p);
int  sky_portal_load(struct sky_portal *p, unsigned slot, const uint8_t *data /*1024*/);
int  sky_portal_remove(struct sky_portal *p, unsigned slot);

/* Feed one OUT report / SET_REPORT payload. Returns 1 if a response was queued. */
int  sky_portal_command(struct sky_portal *p, const uint8_t *cmd, size_t len);

/* Feed one interrupt-OUT packet. Same as sky_portal_command(), except that while the speaker is on the packet may be
 * audio, not a command: if the game has never sent a command on this endpoint it is dropped (returns 0). If it
 * has, the packet is only accepted when its first byte is a known command letter. SET_REPORT always uses
 * sky_portal_command() directly. */
int  sky_portal_out_packet(struct sky_portal *p, const uint8_t *buf, size_t len);

/* Build a status report ('S', presence bits, counter) right now, even if nothing changed. A real portal answers every
 * IN poll with one; the hook uses this to do the same at a bounded rate. */
void sky_portal_status_report(struct sky_portal *p, uint8_t out[SKY_REPORT]);

/* True when an IN transfer could be completed right now (queued response, or a
 * figure add/remove the game has not been told about yet). */
int  sky_portal_in_ready(const struct sky_portal *p);

/* Produce the next IN report (queued response first, otherwise a status report).
 * Returns 1 and fills out[32], or 0 if nothing is ready. */
int  sky_portal_next_report(struct sky_portal *p, uint8_t out[SKY_REPORT]);

#endif
