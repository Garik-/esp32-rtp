

#include "esp_camera.h"
#include "esp_log.h"

#include "jpeg.h"

DRAM_ATTR static uint8_t rtp_jpeg_packet[RTP_PACKET_SIZE];

/**
 * RTP send task
 */
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
                to.sin_port = htons(RTP_STREAM_PORT);
                to.sin_addr.s_addr = rtp_stream_address;

                /* send RTP packets */
                memset(rtp_jpeg_packet, 0, sizeof(rtp_jpeg_packet));

                while (1) {
                    camera_fb_t* fb = esp_camera_fb_get();
                    if (fb) {
                        rtp_send_jpeg_packets(sock, &to, rtp_jpeg_packet, fb);
                        esp_camera_fb_return(fb);
                    }
                }
            }

            /* close the socket */
            closesocket(sock);
        }
    }
}

void rtp_init(void) {
    xTaskCreate(rtp_send_jpeg_task, "rtp_send_jpeg_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
}