#pragma once
#include "esp_wifi.h"
struct dhcps_lease_t { bool enable=false;esp_ip4_addr_t start_ip,end_ip; };
