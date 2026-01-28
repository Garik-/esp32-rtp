

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "include/jpeg.h"

#include "../include/pdm_mic.h"

#define RTP_AUDIO_FRAME_MS 20

static const char* const TAG = "rtp_sender";

DRAM_ATTR static uint8_t rtp_jpeg_packet[RTP_PACKET_SIZE];
DRAM_ATTR static uint8_t rtp_audio_packet[RTP_PACKET_SIZE];

typedef void handle_func_t(int sock, struct sockaddr_in* to);

static void jpeg_handle(int sock, struct sockaddr_in* to) {
    memset(rtp_jpeg_packet, 0, sizeof(rtp_jpeg_packet));

    while (1) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            rtp_send_jpeg_packets(sock, to, rtp_jpeg_packet, fb);
            esp_camera_fb_return(fb);
        } else {
            ESP_LOGE(TAG, "esp_camera_fb_get failed");
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static void audio_handle(int sock, struct sockaddr_in* to) {
    memset(rtp_audio_packet, 0, sizeof(rtp_audio_packet));

    struct rtp_header* header = (struct rtp_header*)rtp_audio_packet;
    header->version = RTP_VERSION;
    header->payloadtype = RTP_PCMU_PAYLOADTYPE;
    header->ssrc = htonl(RTP_PCMU_SSRC);

    uint16_t seq = esp_random() & 0xFFFF;
    uint32_t timestamp = 0;
    size_t bytes_read = 0;

    const TickType_t xFrequency = pdMS_TO_TICKS(RTP_AUDIO_FRAME_MS);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        if (pdm_mic_read(rtp_audio_packet + sizeof(struct rtp_header), &bytes_read) != ERR_OK) {
            ESP_LOGW(TAG, "pdm_mic_read failed, skipping this frame");
            goto next_frame;
        }

        header->seqNum = htons(seq++); // RFC 3550
        header->timestamp = htonl(timestamp);
        timestamp += FRAME_8K;

        sendto(sock, rtp_audio_packet, sizeof(struct rtp_header) + bytes_read, 0, (struct sockaddr*)to,
               sizeof(struct sockaddr));

    next_frame:
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

static void udp_connect(in_port_t port, handle_func_t handle) {
    int sock;
    struct sockaddr_in to;

    /* create new socket */
    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock >= 0) {
        /* prepare RTP stream address */
        memset(&to, 0, sizeof(to));
        to.sin_family = PF_INET;
        to.sin_port = htons(port);

        inet_aton(RTP_IPV4_ADDRESS, &to.sin_addr.s_addr);

        ESP_LOGI(TAG, "handle UDP %s:%d", RTP_IPV4_ADDRESS, port);

        handle(sock, &to);

        /* close the socket */
        closesocket(sock);
    }
}

static void rtp_send_jpeg_task(void* pvParameters) {
    udp_connect(RTP_VIDEO_PORT, jpeg_handle);
}

static void rtp_send_audio_task(void* pvParameters) {
    udp_connect(RTP_AUDIO_PORT, audio_handle);
}

__attribute__((cold)) void rtp_init(void) {
#ifdef AUDIO_SUPPORT
    xTaskCreate(rtp_send_audio_task, "rtp_send_audio_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
#endif

#ifdef VIDEO_SUPPORT
    xTaskCreate(rtp_send_jpeg_task, "rtp_send_jpeg_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
#endif
}