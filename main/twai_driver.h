#pragma once
#include "app_config.h"

esp_err_t twai_driver_init(can_bitrate_t bitrate);
void      twai_task(void *arg);
