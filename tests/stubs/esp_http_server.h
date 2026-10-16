// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string>
#include <sys/types.h>
using esp_err_t = int;
static constexpr ssize_t HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t { std::string status, type, cache, sent; };
inline int httpd_resp_set_status(httpd_req_t *r, const char *s) { r->status = s; return 0; }
inline int httpd_resp_set_type(httpd_req_t *r, const char *s) { r->type = s; return 0; }
inline int httpd_resp_set_hdr(httpd_req_t *r, const char *key, const char *value) {
    if (std::string(key) == "Cache-Control") r->cache = value;
    return 0;
}
inline int httpd_resp_send(httpd_req_t *r, const char *p, ssize_t n) {
    if (n == HTTPD_RESP_USE_STRLEN) r->sent = p;
    else r->sent.assign(p, n);
    return 0;
}
