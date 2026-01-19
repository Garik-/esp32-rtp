#include "camera_pins.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "rtp/rtp.h"

static const char* TAG = "ESP32-UDP-RTP";

#define UDP_MAX_PAYLOAD 1200

#define BUFFER_SIZE UDP_MAX_PAYLOAD
#define RTP_HEADER_SIZE 12
#define JPEG_HEADER_SIZE 8
#define JPEG_PAYLOAD_TYPE 26

struct rtp_header {
    uint8_t v_p_x_cc;
    uint8_t m_pt;
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));

struct jpeg_rtp_header {
    uint8_t type_specific;
    uint8_t fragment_offset[3];
    uint8_t type;
    uint8_t q;
    uint8_t width;
    uint8_t height;
} __attribute__((packed));

static esp_err_t nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (unlikely(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t camera_init() {
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer = LEDC_TIMER_0,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_sscb_sda = SIOD_GPIO_NUM,
        .pin_sscb_scl = SIOC_GPIO_NUM,
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .xclk_freq_hz = 20000000,
        .frame_size = FRAMESIZE_QVGA,
        .pixel_format = PIXFORMAT_JPEG, // for streaming
        // .pixel_format = PIXFORMAT_RGB565, // for face detection/recognition
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .jpeg_quality = 12,
        .fb_count = 1,
    };

    // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
    //                      for larger pre-allocated frame buffer.
    if (likely(config.pixel_format == PIXFORMAT_JPEG)) {
        if (likely(esp_psram_is_initialized())) {
            config.jpeg_quality = 10;
            config.fb_count = 2;
            config.grab_mode = CAMERA_GRAB_LATEST;
        } else {
            // Limit the frame size when PSRAM is not available
            config.frame_size = FRAMESIZE_SVGA;
            config.fb_location = CAMERA_FB_IN_DRAM;
        }
    } else {
        // Best option for face detection/recognition
        config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
        config.fb_count = 2;
#endif
    }

    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "esp_camera_init");

    sensor_t* s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "Sensor PID: 0x%04x", s->id.PID);
    // initial sensors are flipped vertically and colors are a bit saturated
    if (likely(s->id.PID == OV3660_PID)) {
        s->set_vflip(s, 1);       // flip it back
        s->set_brightness(s, 1);  // up the brightness just a bit
        s->set_saturation(s, -2); // lower the saturation
    }

    return ESP_OK;
}

static void set_fragment_offset(uint8_t* buf, size_t offset) {
    buf[0] = (offset >> 16) & 0xFF;
    buf[1] = (offset >> 8) & 0xFF;
    buf[2] = offset & 0xFF;
}

static void udp_server_task(void* pvParameters) {
    struct sockaddr_in dest_addr;
    const socklen_t dest_addr_len = sizeof(dest_addr);
    memset(&dest_addr, 0, sizeof(dest_addr));

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESPRTP_UDP_PORT);
    inet_pton(AF_INET, "192.168.1.78", &dest_addr.sin_addr.s_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Socket created");

    close(sock);
}

static void udp_server_camera_task(void* pvParameters) {
    struct sockaddr_in dest_addr;
    const socklen_t dest_addr_len = sizeof(dest_addr);
    memset(&dest_addr, 0, sizeof(dest_addr));

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(CONFIG_ESPRTP_UDP_PORT);
    inet_pton(AF_INET, "192.168.1.78", &dest_addr.sin_addr.s_addr);

    // int broadcast = 1;
    // struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "Socket created");

    // setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    // setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint8_t buffer[BUFFER_SIZE];

    uint16_t seq = 0;
    uint32_t timestamp = 0;
    const uint32_t ssrc = 0x12345678;

    int last_frame_time = esp_timer_get_time();
    double avg_frame_time = 0;

    while (1) {

        camera_fb_t* fb = esp_camera_fb_get();

        if (!fb) {
            ESP_LOGE(TAG, "Camera capture");
            vTaskDelay(pdMS_TO_TICKS(100)); // Даем время системе
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t frame_time_us = now - last_frame_time; // мксек
        last_frame_time = now;

        // Переводим в RTP timestamp (частота 90 kHz)
        timestamp += (uint32_t)((frame_time_us / 1000000.0) * 90000);

        // Считаем усреднённое время кадра для fps
        if (avg_frame_time == 0)
            avg_frame_time = frame_time_us / 1000.0;
        else
            avg_frame_time = avg_frame_time * 0.9 + (frame_time_us / 1000.0) * 0.1; // EMA

        // Логирование
        ESP_LOGI(TAG, "MJPG: %uB, frame_time: %lldms (%.2ffps), AVG: %.1fms (%.2ffps)", (uint32_t)fb->len,
                 frame_time_us / 1000, 1000000.0 / frame_time_us, avg_frame_time, 1000.0 / avg_frame_time);

        size_t offset = 0;
        while (offset < fb->len) {
            size_t payload_size = BUFFER_SIZE - RTP_HEADER_SIZE - JPEG_HEADER_SIZE;
            if (fb->len - offset < payload_size)
                payload_size = fb->len - offset;

            // RTP заголовок
            struct rtp_header* rtp = (struct rtp_header*)buffer;
            rtp->v_p_x_cc = 0x80;
            rtp->m_pt = JPEG_PAYLOAD_TYPE;
            if (offset + payload_size >= fb->len) {
                rtp->m_pt |= 0x80; // Marker bit последнего пакета кадра
            }
            rtp->seq = htons(seq++);
            rtp->timestamp = htonl(timestamp);
            rtp->ssrc = htonl(ssrc);

            // JPEG RTP заголовок
            struct jpeg_rtp_header* jpeg_hdr = (struct jpeg_rtp_header*)(buffer + RTP_HEADER_SIZE);
            jpeg_hdr->type_specific = 0;
            set_fragment_offset(jpeg_hdr->fragment_offset, offset);
            jpeg_hdr->type = 1;
            jpeg_hdr->q = 255;
            jpeg_hdr->width = 0;
            jpeg_hdr->height = 0;

            memcpy(buffer + RTP_HEADER_SIZE + JPEG_HEADER_SIZE, fb->buf + offset, payload_size);

            ESP_LOGI(TAG, "sendto: seq=%u, payload=%uB, total=%uB, offset=%u", seq, payload_size, fb->len, offset);

            while (sendto(sock, buffer, RTP_HEADER_SIZE + JPEG_HEADER_SIZE + payload_size, 0,
                          (struct sockaddr*)&dest_addr, dest_addr_len) < 0) {
                ESP_LOGE(TAG, "sendto error: %d (%s)", errno, strerror(errno));
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            offset += payload_size;

            vTaskDelay(pdMS_TO_TICKS(5));
        }

        esp_camera_fb_return(fb);
        // timestamp += 90000 / 25;

        // vTaskDelay(pdMS_TO_TICKS(1));
    }

    close(sock);
    vTaskDelete(NULL);
}

static esp_err_t app_logic() {
    ESP_RETURN_ON_ERROR(nvs_init(), TAG, "NVS init");
    ESP_RETURN_ON_ERROR(camera_init(), TAG, "camera init");
    ESP_RETURN_ON_ERROR(wifi_connect(), TAG, "wifi_connect");

    return ESP_OK;
}

void app_main(void) {
    ESP_ERROR_CHECK(app_logic());

    rtp_init();

    // xTaskCreate(udp_server_task, "udp_server", 4096, NULL, 5, NULL);

    /*while (1) {
        ESP_LOGI(TAG, "Taking picture...");
        camera_fb_t* pic = esp_camera_fb_get();
        if (!pic) {
            ESP_LOGE(TAG, "Camera capture");
            vTaskDelay(pdMS_TO_TICKS(100)); // Даем время системе
            continue;                       // Пропускаем итерацию, чтобы не упасть
        }

        // use pic->buf to access the image
        ESP_LOGI(TAG, "Picture taken! Its size was: %zu bytes", pic->len);
        if (pic->len > 1) {
            ESP_LOGI(TAG, "First bytes: 0x%02x 0x%02x", pic->buf[0], pic->buf[1]);
        }
        esp_camera_fb_return(pic);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }*/
}
