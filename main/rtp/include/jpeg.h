#pragma once

#include "esp_camera.h"
#include "esp_log.h"

#include "common.h"

#define MAX_QUANT_TABLES 4
#define QUANT_TABLE_SIZE 64

#define JPEG_TYPE_YUV422 0U
#define JPEG_Q_DEFAULT 255U

struct rtp_jpeg_header {
    uint8_t type_specific;
    uint8_t fragment_offset[3];
    uint8_t type;
    uint8_t q;
    uint8_t width;
    uint8_t height;
} __attribute__((packed));

struct jpeg_quant_header {
    uint8_t mbz;
    uint8_t precision;
    uint16_t length;
} __attribute__((packed));

void rtp_send_jpeg_packets(int sock, const struct sockaddr_in* to, uint8_t* buf, const camera_fb_t* fb);