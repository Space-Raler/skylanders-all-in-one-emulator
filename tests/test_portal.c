#include "sky_portal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void fig_fill(uint8_t *f, uint8_t seed) { for (int i = 0; i < SKY_FIG_SIZE; i++) f[i] = (uint8_t)(seed + i * 7); }

int main(void) {
    static struct sky_portal p;
    uint8_t fig[SKY_FIG_SIZE], r[SKY_REPORT], cmd[SKY_REPORT];
    sky_portal_init(&p);

    /* reset / activate */
    assert(sky_portal_command(&p, (const uint8_t *)"R", 1) == 1);
    assert(sky_portal_next_report(&p, r) && r[0] == 'R' && r[1] == 0x02 && r[2] == 0x1b);
    cmd[0] = 'A'; cmd[1] = 1;
    assert(sky_portal_command(&p, cmd, 2) == 1);
    assert(sky_portal_next_report(&p, r) && !memcmp(r, "A\x01\xff\x77", 4));

    /* empty portal: nothing spontaneous, 'S' reports no figures, counter increments */
    assert(!sky_portal_in_ready(&p));
    assert(sky_portal_command(&p, (const uint8_t *)"S", 1) == 1);
    assert(sky_portal_next_report(&p, r) && r[0] == 'S' && !r[1] && !r[2] && !r[3] && !r[4] && r[5] == 0 && r[6] == 1);
    sky_portal_command(&p, (const uint8_t *)"S", 1); sky_portal_next_report(&p, r);
    assert(r[5] == 1);

    /* add a figure to slot 2: spontaneous status, bits 0b11 once, then 0b01 */
    fig_fill(fig, 3);
    assert(sky_portal_load(&p, 2, fig) == 0);
    assert(sky_portal_in_ready(&p));
    assert(sky_portal_next_report(&p, r) && r[0] == 'S' && r[1] == (3u << 4));
    assert(!sky_portal_in_ready(&p));
    sky_portal_command(&p, (const uint8_t *)"S", 1); sky_portal_next_report(&p, r);
    assert(r[1] == (1u << 4));

    /* read every block back and compare to what was loaded */
    for (int b = 0; b < SKY_BLOCKS; b++) {
        cmd[0] = 'Q'; cmd[1] = 0x12; cmd[2] = (uint8_t)b;
        assert(sky_portal_command(&p, cmd, 3) == 1);
        assert(sky_portal_next_report(&p, r));
        assert(r[0] == 'Q' && r[1] == 0x12 && r[2] == b && !memcmp(&r[3], fig + b * 16, 16));
    }
    /* query on an empty slot / out of range block: bit 4 clear */
    cmd[1] = 0x05; cmd[2] = 0; sky_portal_command(&p, cmd, 3); sky_portal_next_report(&p, r);
    assert(r[1] == 0x05);
    cmd[1] = 0x12; cmd[2] = 64; sky_portal_command(&p, cmd, 3); sky_portal_next_report(&p, r);
    assert(r[1] == 0x02);

    /* write a block: persisted, flagged dirty, seqlock even */
    memset(cmd, 0, sizeof cmd); cmd[0] = 'W'; cmd[1] = 0x12; cmd[2] = 8;
    for (int i = 0; i < 16; i++) cmd[3 + i] = (uint8_t)(0xA0 + i);
    assert(sky_portal_command(&p, cmd, 19) == 1);
    assert(sky_portal_next_report(&p, r) && r[0] == 'W' && r[1] == 0x12 && r[2] == 8);
    assert(p.fig[2].dirty == 1 && (p.fig[2].seq & 1) == 0 && p.fig[2].data[8 * 16] == 0xA0);
    assert(p.fig[2].data[0] == fig[0]);
    /* short W is ignored */
    assert(sky_portal_command(&p, cmd, 10) == 0);

    /* remove: status shows 0b10 once, then 0b00; Q now reports absent */
    assert(sky_portal_remove(&p, 2) == 0);
    assert(sky_portal_next_report(&p, r) && r[1] == (2u << 4));
    sky_portal_command(&p, (const uint8_t *)"S", 1); sky_portal_next_report(&p, r);
    assert(r[1] == 0);

    /* light/colour commands are accepted silently; M answers */
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 'C'; cmd[1] = 10; cmd[2] = 20; cmd[3] = 30;
    assert(sky_portal_command(&p, cmd, 4) == 0);
    assert(p.lights.ring[0] == 10 && p.lights.ring[1] == 20 && p.lights.ring[2] == 30 && p.lights.seq == 1);
    assert(sky_portal_command(&p, cmd, 3) == 0 && p.lights.seq == 1);              /* too short: ignored */
    { uint8_t l[5] = {'L', 0, 1, 2, 3};
      assert(sky_portal_command(&p, l, 5) == 0 && p.lights.ring[2] == 3 && p.lights.seq == 2);
      l[1] = 1; l[2] = 40; assert(sky_portal_command(&p, l, 3) == 0);
      assert(p.lights.backlight_set && p.lights.backlight == 40 && p.lights.seq == 3);
      l[1] = 3; assert(sky_portal_command(&p, l, 3) == 0 && p.lights.backlight == 255);
      l[1] = 2; assert(sky_portal_command(&p, l, 5) == 0 && p.lights.seq == 4);    /* other sides ignored */ }
    { uint8_t j[7] = {'J', 0, 100, 150, 200, 0xE8, 0x03};                           /* 1000 ms */
      assert(sky_portal_command(&p, j, 7) == 1);
      assert(sky_portal_next_report(&p, r) && r[0] == 'J' && r[1] == 0);
      assert(p.lights.ring[0] == 100 && p.lights.fade_ms == 1000);
      j[5] = j[6] = 0xFF; assert(sky_portal_command(&p, j, 7) == 1); sky_portal_next_report(&p, r);
      assert(p.lights.fade_ms == SKY_LIGHT_FADE_MAX_MS);                            /* capped */
      uint32_t s = p.lights.seq; j[1] = 2;                                          /* other side: acked, untouched */
      assert(sky_portal_command(&p, j, 7) == 1); sky_portal_next_report(&p, r);
      assert(p.lights.seq == s);
      assert(sky_portal_command(&p, j, 5) == 0 && sky_portal_command(&p, j, 32) == 0); /* not 7 bytes: ignored */
      assert(!sky_portal_in_ready(&p)); }
    assert(p.speaker == 0);
    cmd[0] = 'M'; cmd[1] = 1; assert(sky_portal_command(&p, cmd, 2) == 1);
    assert(p.speaker == 1);
    assert(sky_portal_next_report(&p, r) && !memcmp(r, "M\x01\x00\x19", 4));
    assert(!sky_portal_in_ready(&p));
    cmd[1] = 0; sky_portal_command(&p, cmd, 2); sky_portal_next_report(&p, r);
    assert(p.speaker == 0);

    /* speaker audio on interrupt OUT: a game that only sends commands via SET_REPORT has OUT packets dropped */
    {
        struct sky_portal q; sky_portal_init(&q);
        uint8_t m[2] = {'M', 1}, audio[32] = {'R', 1, 2, 3}, rr[32];
        assert(sky_portal_command(&q, m, 2) == 1); sky_portal_next_report(&q, rr);
        assert(sky_portal_out_packet(&q, audio, 32) == 0);           /* would otherwise be a reset */
        assert(!sky_portal_in_ready(&q) && q.speaker == 1);
        /* a game that sends commands on OUT before the speaker is on keeps working while it is on */
        struct sky_portal w; sky_portal_init(&w);
        assert(sky_portal_out_packet(&w, (const uint8_t *)"S", 1) == 1); sky_portal_next_report(&w, rr);
        assert(w.cmd_via_out == 1);
        assert(sky_portal_out_packet(&w, m, 2) == 1); sky_portal_next_report(&w, rr);
        uint8_t junk[32] = {0x80, 0x7f};
        assert(sky_portal_out_packet(&w, junk, 32) == 0);
        assert(sky_portal_out_packet(&w, (const uint8_t *)"S", 1) == 1);
        /* reset turns the speaker off */
        sky_portal_out_packet(&w, (const uint8_t *)"R", 1);
        assert(w.speaker == 0);
    }

    /* bounds */
    assert(sky_portal_load(&p, SKY_SLOTS, fig) < 0);
    assert(sky_portal_remove(&p, SKY_SLOTS) < 0);
    puts("portal protocol tests passed");
    return 0;
}
