// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdio>
#include "status_response.h"
using namespace esphome::ecowitt_rf;
int main() {
    httpd_req_t r;
    send_json_response(&r, "503 Service Unavailable", "{\"err\":\"busy, retry\"}");
    assert(r.status == "503 Service Unavailable" && r.cache == "no-store");
    assert(r.type == "application/json" && r.sent == "{\"err\":\"busy, retry\"}");
    send_json_response(&r, "400 Bad Request", "{}"); assert(r.status == "400 Bad Request");
    send_json_response(&r, "413 Content Too Large", "{}"); assert(r.status == "413 Content Too Large");
    // Main-loop buffer deliberately has no C-string terminator; send only its exact bytes.
    const char bounded[3] = {'{', '}', 'X'};
    send_json_response(&r, "200 OK", bounded, 2);
    assert(r.status == "200 OK" && r.sent == "{}");
    std::puts("IDF explicit 503/400/413 statuses and bounded synchronous response passed");
}
