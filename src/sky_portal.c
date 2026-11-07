#include "sky_portal.h"
#include "sky_mem.h"

void sky_portal_init(struct sky_portal *p) { sky_zero(p, sizeof(*p)); }

static void enqueue(struct sky_portal *p, const uint8_t *r, size_t n) {
    if (p->rtail - p->rhead >= SKY_RESPQ) p->rhead++;      /* drop oldest */
    uint8_t *dst = p->resp[p->rtail % SKY_RESPQ];
    sky_zero(dst, SKY_REPORT);
    sky_copy(dst, r, n > SKY_REPORT ? SKY_REPORT : n);
    p->rtail++;
}

int sky_portal_load(struct sky_portal *p, unsigned slot, const uint8_t *data) {
    if (slot >= SKY_SLOTS || !data) return -1;
    struct sky_figure *f = &p->fig[slot];
    f->seq++;
    sky_copy(f->data, data, SKY_FIG_SIZE);
    f->status = SKY_ST_PRESENT | SKY_ST_CHANGED;
    f->dirty = 0;
    f->seq++;
    return 0;
}

int sky_portal_remove(struct sky_portal *p, unsigned slot) {
    if (slot >= SKY_SLOTS) return -1;
    struct sky_figure *f = &p->fig[slot];
    if (f->status & SKY_ST_PRESENT) f->status = SKY_ST_CHANGED;  /* removed */
    return 0;
}

static void build_status(struct sky_portal *p, uint8_t out[SKY_REPORT]) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < SKY_SLOTS; i++) {
        struct sky_figure *f = &p->fig[i];
        bits |= (uint32_t)(f->status & 3u) << (2 * i);
        f->status &= SKY_ST_PRESENT;        /* clear the "changed" bit once reported */
    }
    sky_zero(out, SKY_REPORT);
    out[0] = 'S';
    out[1] = (uint8_t)bits; out[2] = (uint8_t)(bits >> 8);
    out[3] = (uint8_t)(bits >> 16); out[4] = (uint8_t)(bits >> 24);
    out[5] = p->counter++;
    out[6] = 1;
}

static void lights_ring(struct sky_portal *p, uint8_t r, uint8_t g, uint8_t b, unsigned fade_ms) {
    struct sky_lights *l = &p->lights;
    l->ring[0] = r; l->ring[1] = g; l->ring[2] = b;
    l->fade_ms = (uint16_t)(fade_ms > SKY_LIGHT_FADE_MAX_MS ? SKY_LIGHT_FADE_MAX_MS : fade_ms);
    l->seq++;
}

static void lights_backlight(struct sky_portal *p, uint8_t level) {
    p->lights.backlight = level;
    p->lights.backlight_set = 1;
    p->lights.seq++;
}

void sky_portal_status_report(struct sky_portal *p, uint8_t out[SKY_REPORT]) { build_status(p, out); }

int sky_portal_command(struct sky_portal *p, const uint8_t *cmd, size_t len) {
    uint8_t r[SKY_REPORT];
    if (!cmd || len == 0) return 0;
    sky_zero(r, sizeof(r));
    switch (cmd[0]) {
    case 'R':                                  /* reset / ready */
        p->active = 0;
        p->speaker = 0;                        /* a reset also silences the speaker */
        r[0] = 'R'; r[1] = 0x02; r[2] = 0x1b;
        enqueue(p, r, 3); return 1;
    case 'A':                                  /* activate */
        p->active = len > 1 ? cmd[1] : 0;
        r[0] = 'A'; r[1] = len > 1 ? cmd[1] : 0; r[2] = 0xFF; r[3] = 0x77;
        enqueue(p, r, 4); return 1;
    case 'S':                                  /* status */
        build_status(p, r); enqueue(p, r, SKY_REPORT); return 1;
    case 'Q': {                                /* read block */
        if (len < 3) return 0;
        unsigned idx = cmd[1] & 0x0F, blk = cmd[2];
        struct sky_figure *f = &p->fig[idx];
        r[0] = 'Q'; r[2] = (uint8_t)blk;
        if ((f->status & SKY_ST_PRESENT) && blk < SKY_BLOCKS) {
            r[1] = (uint8_t)(0x10 | idx);
            sky_copy(&r[3], &f->data[blk * SKY_BLOCK], SKY_BLOCK);
        } else {
            r[1] = (uint8_t)idx;               /* bit 4 clear = no figure / bad block */
        }
        enqueue(p, r, SKY_REPORT); return 1;
    }
    case 'W': {                                /* write block */
        if (len < 3 + SKY_BLOCK) return 0;
        unsigned idx = cmd[1] & 0x0F, blk = cmd[2];
        struct sky_figure *f = &p->fig[idx];
        r[0] = 'W'; r[2] = (uint8_t)blk;
        if ((f->status & SKY_ST_PRESENT) && blk < SKY_BLOCKS) {
            f->seq++;
            sky_copy(&f->data[blk * SKY_BLOCK], &cmd[3], SKY_BLOCK);
            f->dirty = 1;
            f->seq++;
            r[1] = (uint8_t)(0x10 | idx);
        } else {
            r[1] = (uint8_t)idx;
        }
        enqueue(p, r, 3); return 1;
    }
    case 'M':                                  /* speaker / music: nonzero argument = on */
        p->speaker = len > 1 && cmd[1] != 0;
        r[0] = 'M'; r[1] = len > 1 ? cmd[1] : 0; r[2] = 0x00; r[3] = 0x19;
        enqueue(p, r, 4); return 1;
    case 'J':                                  /* ring fade (Swap Force onwards); exactly 7 bytes, acked with 'J' */
        if (len != 7) return 0;
        if (cmd[1] == 0) {
            unsigned ms = (unsigned)cmd[5] | ((unsigned)cmd[6] << 8);
            lights_ring(p, cmd[2], cmd[3], cmd[4], ms);
        }
        r[0] = 'J';
        enqueue(p, r, 1); return 1;
    case 'C':                                  /* ring colour, no reply */
        if (len >= 4) lights_ring(p, cmd[1], cmd[2], cmd[3], 0);
        return 0;
    case 'L':                                  /* side lights, no reply */
        if (len >= 3) {
            if (cmd[1] == 0 && len >= 5) lights_ring(p, cmd[2], cmd[3], cmd[4], 0);
            else if (cmd[1] == 1) lights_backlight(p, cmd[2]);
            else if (cmd[1] == 3) lights_backlight(p, 0xFF);
        }
        return 0;
    default:
        return 0;                              /* accepted, no response */
    }
}

static int known_command(uint8_t c) {
    switch (c) {
    case 'R': case 'A': case 'S': case 'Q': case 'W': case 'M': case 'J': case 'C': case 'L': return 1;
    default: return 0;
    }
}

int sky_portal_out_packet(struct sky_portal *p, const uint8_t *buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (p->speaker) {
        if (!p->cmd_via_out) return 0;                 /* game only uses SET_REPORT for commands: this is audio */
        if (!known_command(buf[0])) return 0;
    } else if (known_command(buf[0])) {
        p->cmd_via_out = 1;                            /* this game does send commands on the OUT endpoint */
    }
    return sky_portal_command(p, buf, len);
}

int sky_portal_in_ready(const struct sky_portal *p) {
    if (p->rtail != p->rhead) return 1;
    for (unsigned i = 0; i < SKY_SLOTS; i++)
        if (p->fig[i].status & SKY_ST_CHANGED) return 1;
    return 0;
}

int sky_portal_next_report(struct sky_portal *p, uint8_t out[SKY_REPORT]) {
    if (p->rtail != p->rhead) {
        sky_copy(out, p->resp[p->rhead % SKY_RESPQ], SKY_REPORT);
        p->rhead++;
        return 1;
    }
    for (unsigned i = 0; i < SKY_SLOTS; i++) {
        if (p->fig[i].status & SKY_ST_CHANGED) { build_status(p, out); return 1; }
    }
    return 0;
}
