// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// No-op ESPHome log macros so any header that (transitively) includes esphome/core/log.h
// compiles without the ESPHome logger. The host tests do not assert on log output.

#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
