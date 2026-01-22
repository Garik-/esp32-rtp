

#include "esp_camera.h"
#include "esp_log.h"

#include "jpeg.h"

#include "../pdm_mic.h"

DRAM_ATTR static uint8_t rtp_jpeg_packet[RTP_PACKET_SIZE];
DRAM_ATTR static uint8_t rtp_audio_packet[RTP_PACKET_SIZE];

static void rtp_send_jpeg_task(void* pvParameters) {
    int sock;
    struct sockaddr_in local;
    struct sockaddr_in to;
    uint32_t rtp_stream_address;

    /* initialize RTP stream address */
    rtp_stream_address = RTP_STREAM_ADDRESS;

    /* if we got a valid RTP stream address... */
    if (rtp_stream_address != 0) {
        /* create new socket */
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            /* prepare local address */
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_port = htons(INADDR_ANY);
            local.sin_addr.s_addr = htonl(INADDR_ANY);

            /* bind to local address */
            if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == 0) {
                /* prepare RTP stream address */
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = htons(CONFIG_ESPRTP_UDP_VIDEO_PORT);
                to.sin_addr.s_addr = rtp_stream_address;

                /* send RTP packets */
                memset(rtp_jpeg_packet, 0, sizeof(rtp_jpeg_packet));

                while (1) {
                    camera_fb_t* fb = esp_camera_fb_get();
                    if (fb) {
                        rtp_send_jpeg_packets(sock, &to, rtp_jpeg_packet, fb);
                        esp_camera_fb_return(fb);
                    } else {
                        ESP_LOGE(TAG, "esp_camera_fb_get failed");
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
            }

            /* close the socket */
            closesocket(sock);
        }
    }
}

#define RTP_AUDIO_FRAME_MS 20

static void rtp_send_audio_task(void* pvParameters) {
    int sock;
    struct sockaddr_in local;
    struct sockaddr_in to;
    uint32_t rtp_stream_address;

    /* initialize RTP stream address */
    rtp_stream_address = RTP_STREAM_ADDRESS;

    /* if we got a valid RTP stream address... */
    if (rtp_stream_address != 0) {
        /* create new socket */
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            /* prepare local address */
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_port = htons(INADDR_ANY);
            local.sin_addr.s_addr = htonl(INADDR_ANY);

            /* bind to local address */
            if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == 0) {
                /* prepare RTP stream address */
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = htons(CONFIG_ESPRTP_UDP_AUDIO_PORT);
                to.sin_addr.s_addr = rtp_stream_address;

                /* send RTP packets */
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

                    sendto(sock, rtp_audio_packet, sizeof(struct rtp_header) + bytes_read, 0, (struct sockaddr*)&to,
                           sizeof(to));

                next_frame:
                    vTaskDelayUntil(&xLastWakeTime, xFrequency);
                }

                /* close the socket */
                closesocket(sock);
            }
        }
    }
}

__attribute__((cold)) void rtp_init(void) {
#ifdef CONFIG_ESPRTP_AUDIO_SUPPORT
    xTaskCreate(rtp_send_audio_task, "rtp_send_audio_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
#endif

#ifdef CONFIG_ESPRTP_VIDEO_SUPPORT
    xTaskCreate(rtp_send_jpeg_task, "rtp_send_jpeg_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
#endif
}