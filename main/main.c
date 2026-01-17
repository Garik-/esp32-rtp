#include "camera_pins.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "esp32-udp-rtp";

static esp_err_t nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (unlikely(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
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
        .frame_size = FRAMESIZE_SVGA,
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

    ESP_RETURN_ON_ERROR(esp_camera_init(&config), TAG, "esp_camera_init failed");

    sensor_t* s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "Sensor PID: 0x%04x", s->id.PID);
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);       // flip it back
        s->set_brightness(s, 1);  // up the brightness just a bit
        s->set_saturation(s, -2); // lower the saturation
    }
    // drop down frame size for higher initial frame rate
    if (likely(config.pixel_format == PIXFORMAT_JPEG)) {
        s->set_framesize(s, FRAMESIZE_QVGA);
    }

    return ESP_OK;
}

static esp_err_t app_logic() {
    ESP_RETURN_ON_ERROR(nvs_init(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(camera_init(), TAG, "camera init failed");

    return ESP_OK;
}

void app_main(void) {
    ESP_ERROR_CHECK(app_logic());

    while (1) {
        ESP_LOGI(TAG, "Taking picture...");
        camera_fb_t* pic = esp_camera_fb_get();
        if (!pic) {
            ESP_LOGE(TAG, "Camera capture failed");
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
    }
}
