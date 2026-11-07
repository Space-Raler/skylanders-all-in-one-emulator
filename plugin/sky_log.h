#ifndef SKY_LOG_H
#define SKY_LOG_H
#ifdef __cplusplus
extern "C" {
#endif
/* printf-style; appends to /data/skylanders/skyportal.log and stdout. Defined in main_payload.cpp. */
void sky_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#endif
