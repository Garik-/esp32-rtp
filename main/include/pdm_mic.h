#pragma once

#include "esp_err.h"

#define FRAME_16K 320 // 20 ms @ 16 kHz
#define FRAME_8K 160  // 160 @ 8 kHz

esp_err_t pdm_mic_init();
esp_err_t pdm_mic_read(uint8_t* ulaw_buffer, size_t* ulaw_size);