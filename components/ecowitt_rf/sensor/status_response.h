// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <esp_http_server.h>

namespace esphome::ecowitt_rf {

// ESPHome 2026.6.2's IDF shim maps unrecognised integer statuses (including
// 400/413/503) to 500. Use explicit IDF status lines and a synchronous send.
inline esp_err_t send_json_response(httpd_req_t *req, const char *status,
                                   const char *body, ssize_t length = HTTPD_RESP_USE_STRLEN) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, length);
}

}  // namespace esphome::ecowitt_rf
