/* Tiny mem helpers so shared code builds freestanding. */
#ifndef SKY_MEM_H
#define SKY_MEM_H
#include <stddef.h>
#include <stdint.h>

#ifdef SKY_FREESTANDING
static inline void sky_copy(void *d, const void *s, size_t n) {
    volatile uint8_t *dd = (volatile uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}
static inline void sky_zero(void *d, size_t n) {
    volatile uint8_t *dd = (volatile uint8_t *)d;
    while (n--) *dd++ = 0;
}
#else
#include <string.h>
static inline void sky_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static inline void sky_zero(void *d, size_t n) { memset(d, 0, n); }
#endif
#endif
