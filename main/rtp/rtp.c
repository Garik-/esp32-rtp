#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "esp_camera.h"
#include "esp_log.h"

/** RTP stream port */
#define RTP_STREAM_PORT CONFIG_ESPRTP_UDP_PORT
/** RTP stream multicast address as IPv4 address in "uint32_t" format */
#define RTP_STREAM_ADDRESS inet_addr("192.168.1.78")

/** RTP send delay - in milliseconds */
#define RTP_SEND_DELAY 40

#define min(a, b) ((a) < (b) ? (a) : (b))

/** RTP packet/payload size */
#define RTP_PACKET_SIZE 1500
#define RTP_PAYLOAD_SIZE 1024

/** RTP header constants */
#define RTP_VERSION 0x80
#define RTP_TIMESTAMP_INCREMENT 3600
#define RTP_SSRC 0
#define RTP_PAYLOADTYPE 26
#define RTP_MARKER_MASK 0x80

struct rtp_header {
    uint8_t version;
    uint8_t payloadtype;
    uint16_t seqNum;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));

struct rtp_jpeg_header {
    uint8_t type_specific;
    uint8_t fragment_offset[3];
    uint8_t type;
    uint8_t q;
    uint8_t width;
    uint8_t height;
} __attribute__((packed));

static uint8_t rtp_send_packet[RTP_PACKET_SIZE];

static const char* TAG = "rtp_sender";

static void inline set_fragment_offset(uint8_t* buf, const size_t offset) {
    buf[0] = (offset >> 16) & 0xFF;
    buf[1] = (offset >> 8) & 0xFF;
    buf[2] = offset & 0xFF;
}

/**
 * RTP send packets (fragmented for full JPEG)
 */
static void rtp_send_packets(int sock, const struct sockaddr_in* to, const uint8_t* jpeg_data, size_t jpeg_size) {
    struct rtp_header* header;
    struct rtp_jpeg_header* jpeg_header;
    uint8_t* payload;
    size_t data_index = 0;

    // Prepare common headers
    header = (struct rtp_header*)rtp_send_packet;
    header->version = RTP_VERSION;
    header->ssrc = PP_HTONL(RTP_SSRC);
    header->timestamp = htonl(ntohl(header->timestamp) + RTP_TIMESTAMP_INCREMENT);

    jpeg_header = (struct rtp_jpeg_header*)(rtp_send_packet + sizeof(struct rtp_header));
    jpeg_header->type_specific = 0;
    jpeg_header->type = 0; // YUV 4:2:0
    jpeg_header->q = 255;  // Default quantization table
    jpeg_header->width = 320 / 8;
    jpeg_header->height = 240 / 8;

    // Fragment and send
    while (data_index < jpeg_size) {
        payload = rtp_send_packet + sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header);
        size_t chunk_size = min(RTP_PAYLOAD_SIZE, jpeg_size - data_index);

        set_fragment_offset(jpeg_header->fragment_offset, data_index);
        memcpy(payload, jpeg_data + data_index, chunk_size);

        header->payloadtype = RTP_PAYLOADTYPE | (((data_index + chunk_size) >= jpeg_size) ? RTP_MARKER_MASK : 0);
        header->seqNum = htons(ntohs(header->seqNum) + 1);

        size_t packet_size = sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header) + chunk_size;
        if (packet_size > RTP_PACKET_SIZE) {
            ESP_LOGE(TAG, "Packet size %zu exceeds RTP_PACKET_SIZE %d", packet_size, RTP_PACKET_SIZE);
            return;
        }
        if (sendto(sock, rtp_send_packet, packet_size, 0, (struct sockaddr*)to, sizeof(struct sockaddr)) < 0) {
            ESP_LOGE(TAG, "sendto error: %d (%s)", errno, strerror(errno));
        }

        vTaskDelay(pdMS_TO_TICKS(RTP_SEND_DELAY));
        data_index += chunk_size;
    }
}

/**
 * RTP send task
 */
static void rtp_send_task(void* pvParameters) {
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
            local.sin_port = PP_HTONS(INADDR_ANY);
            local.sin_addr.s_addr = PP_HTONL(INADDR_ANY);

            /* bind to local address */
            if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == 0) {
                /* prepare RTP stream address */
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_port = PP_HTONS(RTP_STREAM_PORT);
                to.sin_addr.s_addr = rtp_stream_address;

                /* send RTP packets */
                memset(rtp_send_packet, 0, sizeof(rtp_send_packet));

                while (1) {
                    camera_fb_t* fb = esp_camera_fb_get();
                    if (fb) {
                        rtp_send_packets(sock, &to, fb->buf, fb->len);
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
    xTaskCreate(rtp_send_task, "rtp_send_task", DEFAULT_THREAD_STACKSIZE, NULL, DEFAULT_THREAD_PRIO, NULL);
}