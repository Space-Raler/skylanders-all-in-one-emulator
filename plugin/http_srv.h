#ifndef SKY_HTTP_SRV_H
#define SKY_HTTP_SRV_H
#include "api.h"

/* Blocking accept loop (one connection at a time). Serves the embedded phone UI at "/"
 * and sky_api_handle() under /api/. Returns only on bind/listen failure. */
int sky_http_serve(struct sky_api *api, int port);
#endif
