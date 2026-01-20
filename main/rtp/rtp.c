#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "esp_log.h"

#include "rtpdata.h"

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
#define RTP_PAYLOADTYPE 96
#define RTP_MARKER_MASK 0x80

struct rtp_hdr {
    uint8_t version;
    uint8_t payloadtype;
    uint16_t seqNum;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));

static uint8_t rtp_send_packet[RTP_PACKET_SIZE];

static const char* TAG = "rtp_sender";

/**
 * RTP send packets
 */
static void rtp_send_packets(int sock, const struct sockaddr_in* to, const char* rtp_data,
                             const unsigned long rtp_data_size) {
    struct rtp_hdr* rtphdr;
    uint8_t* rtp_payload;
    int rtp_payload_size;
    int rtp_data_index;

    /* prepare RTP packet */
    rtphdr = (struct rtp_hdr*)rtp_send_packet;
    rtphdr->version = RTP_VERSION;
    rtphdr->payloadtype = 0;
    rtphdr->ssrc = PP_HTONL(RTP_SSRC);
    rtphdr->timestamp = htonl(ntohl(rtphdr->timestamp) + RTP_TIMESTAMP_INCREMENT);
    ESP_LOGD(TAG, "RTP payload type: %d", RTP_PAYLOADTYPE);

    /* send RTP stream packets */
    rtp_data_index = 0;
    do {
        rtp_payload = rtp_send_packet + sizeof(struct rtp_hdr);
        rtp_payload_size = min(RTP_PAYLOAD_SIZE, (rtp_data_size - rtp_data_index));

        memcpy(rtp_payload, rtp_data + rtp_data_index, rtp_payload_size);

        /* set MARKER bit in RTP header on the last packet of an image */
        rtphdr->payloadtype =
            RTP_PAYLOADTYPE | (((rtp_data_index + rtp_payload_size) >= rtp_data_size) ? RTP_MARKER_MASK : 0);

        /* send RTP stream packet */
        vTaskDelay(pdMS_TO_TICKS(RTP_SEND_DELAY)); // clean buf
        if (sendto(sock, rtp_send_packet, sizeof(struct rtp_hdr) + rtp_payload_size, 0, (struct sockaddr*)to,
                   sizeof(struct sockaddr)) >= 0) {
            rtphdr->seqNum = htons(ntohs(rtphdr->seqNum) + 1);
            rtp_data_index += rtp_payload_size;
        } else {
            ESP_LOGE(TAG, "sendto error: %d (%s)", errno, strerror(errno));
        }

    } while (rtp_data_index < rtp_data_size);
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
                    rtp_send_packets(sock, &to, rtp_data_test, sizeof(rtp_data_test));
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