

#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** RTP packet/payload size */
#define RTP_PACKET_SIZE 1500
#define RTP_PAYLOAD_SIZE 1024

/** RTP header constants */
#define RTP_VERSION 0x80

#define RTP_JPEG_SSRC 0xDEADBEEF
#define RTP_JPEG_PAYLOADTYPE 26

#define RTP_OPUS_SSRC 0xABADBABE
#define RTP_OPUS_PAYLOADTYPE 96

#define RTP_MARKER_MASK 0x80

/** RTP stream port */
#define RTP_STREAM_PORT CONFIG_ESPRTP_UDP_PORT
/** RTP stream multicast address as IPv4 address in "uint32_t" format */
#define RTP_STREAM_ADDRESS inet_addr("192.168.1.78")

/** RTP send delay - in milliseconds */
#define RTP_SEND_DELAY 10

struct rtp_header {
    uint8_t version;
    uint8_t payloadtype;
    uint16_t seqNum;
    uint32_t timestamp;
    uint32_t ssrc;
} __attribute__((packed));
