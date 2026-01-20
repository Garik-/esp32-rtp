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
#define RTP_SEND_DELAY 10

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

struct jpeg_quant_header {
    uint8_t mbz;
    uint8_t precision;
    uint16_t length;
} __attribute__((packed));

DRAM_ATTR static uint8_t rtp_send_packet[RTP_PACKET_SIZE];

static const char* const TAG = "rtp_sender";

static inline void set_fragment_offset(uint8_t* buf, const size_t offset) {
    buf[0] = (offset >> 16) & 0xFF;
    buf[1] = (offset >> 8) & 0xFF;
    buf[2] = offset & 0xFF;
}

static const uint8_t* get_jpeg_data(const uint8_t* buf, size_t size, size_t* out_size) {
    if (!buf || size < 4) {
        *out_size = 0;
        return NULL;
    }

    // Найти FF DA (SOS)
    size_t pos = 0;
    while (pos < size - 1) {
        if (buf[pos] == 0xFF && buf[pos + 1] == 0xDA) {
            break;
        }
        pos++;
    }
    if (pos >= size - 1) {
        *out_size = 0;
        return NULL; // SOS не найден
    }

    // Пропустить FF DA
    pos += 2;

    // Прочитать длину SOS-сегмента (2 байта, big-endian)
    if (pos + 2 > size) {
        *out_size = 0;
        return NULL;
    }
    uint16_t sos_length = (buf[pos] << 8) | buf[pos + 1];
    pos += 2;

    // Пропустить параметры SOS (sos_length - 2, поскольку длина включает себя)
    if (sos_length < 2 || pos + (sos_length - 2) > size) {
        *out_size = 0;
        return NULL;
    }
    pos += (sos_length - 2);

    // Теперь pos указывает на начало сжатых данных
    size_t data_start = pos;

    // Найти FF D9 (EOI)
    while (pos < size - 1) {
        if (buf[pos] == 0xFF && buf[pos + 1] == 0xD9) {
            break;
        }
        pos++;
    }
    if (pos >= size - 1) {
        *out_size = 0;
        return NULL; // EOI не найден
    }

    // Копировать всё от data_start до pos (включительно FF D9? Но "до FF D9" значит перед ним)
    // Предполагаю копировать данные до FF D9, не включая его

    *out_size = pos - data_start;
    return buf + data_start;
}
#define MAX_QUANT_TABLES 4
#define QUANT_TABLE_SIZE 64

void extract_quant_tables_refs(const uint8_t* buf, size_t size, const uint8_t** tables) {
    // Инициализировать NULL
    for (int i = 0; i < MAX_QUANT_TABLES; i++) {
        tables[i] = NULL;
    }

    size_t pos = 0;
    while (pos < size - 4) {
        if (buf[pos] == 0xFF && buf[pos + 1] == 0xDB) {
            pos += 2;

            if (pos + 2 > size)
                break;
            // uint16_t length = (buf[pos] << 8) | buf[pos + 1];
            pos += 2;

            if (pos >= size)
                break;
            uint8_t table_info = buf[pos++];
            uint8_t table_id = table_info & 0x0F;
            uint8_t precision = (table_info >> 4) & 0x0F;

            if (precision != 0 || table_id >= MAX_QUANT_TABLES)
                continue;

            if (pos + QUANT_TABLE_SIZE > size)
                continue;
            tables[table_id] = buf + pos;
            pos += QUANT_TABLE_SIZE;
        } else {
            pos++;
        }
    }
}

/**
 * RTP send packets (fragmented for full JPEG)
 */
static void rtp_send_packets(int sock, const struct sockaddr_in* to, const camera_fb_t* fb) {

    size_t jpeg_size;
    const uint8_t* jpeg_data = get_jpeg_data(fb->buf, fb->len, &jpeg_size);
    if (jpeg_data == NULL) {
        ESP_LOGE(TAG, "empty jpeg payload");
        return;
    }

    const uint8_t* quant_tables[MAX_QUANT_TABLES];
    extract_quant_tables_refs(fb->buf, fb->len, quant_tables);

    int quant_tables_count = 0;
    for (; quant_tables_count < MAX_QUANT_TABLES && quant_tables[quant_tables_count] != NULL; quant_tables_count++)
        ;

    struct rtp_header* header;
    struct rtp_jpeg_header* jpeg_header;
    uint8_t* payload;
    size_t data_index = 0;

    // Prepare common headers
    header = (struct rtp_header*)rtp_send_packet;
    header->version = RTP_VERSION;
    header->ssrc = htonl(RTP_SSRC);
    // Use camera timestamp converted to RTP units (90kHz)
    uint32_t rtp_ts = (uint32_t)(fb->timestamp.tv_sec * 90000ULL + fb->timestamp.tv_usec * 90ULL / 1000ULL);
    header->timestamp = htonl(rtp_ts);

    jpeg_header = (struct rtp_jpeg_header*)(rtp_send_packet + sizeof(struct rtp_header));
    jpeg_header->type_specific = 0;
    jpeg_header->type = 0; // YUV 4:2:2
    jpeg_header->q = 255;  // Default quantization table
    jpeg_header->width = fb->width / 8;
    jpeg_header->height = fb->height / 8;

    // Fragment and send
    while (data_index < jpeg_size) {
        size_t tables_size =
            (data_index == 0) ? (quant_tables_count * QUANT_TABLE_SIZE) + sizeof(struct jpeg_quant_header) : 0;
        payload = rtp_send_packet + sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header);

        if (tables_size > 0) {

            struct jpeg_quant_header* qh = (struct jpeg_quant_header*)payload;
            qh->mbz = 0;
            qh->precision = 0; // 8-bit tables
            qh->length = htons(quant_tables_count * 64);
            payload += sizeof(*qh);

            for (int i = 0; i < quant_tables_count; i++) {
                memcpy(payload, quant_tables[i], QUANT_TABLE_SIZE);
                payload += QUANT_TABLE_SIZE;
            }
        }

        size_t chunk_size = min(RTP_PAYLOAD_SIZE - tables_size, jpeg_size - data_index);

        set_fragment_offset(jpeg_header->fragment_offset, data_index);
        memcpy(payload, jpeg_data + data_index, chunk_size);

        header->payloadtype = RTP_PAYLOADTYPE | (((data_index + chunk_size) >= jpeg_size) ? RTP_MARKER_MASK : 0);
        header->seqNum = htons(ntohs(header->seqNum) + 1);

        size_t packet_size = sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header) + tables_size + chunk_size;
        if (unlikely(packet_size > RTP_PACKET_SIZE)) {
            ESP_LOGE(TAG, "Packet size %zu exceeds RTP_PACKET_SIZE %d", packet_size, RTP_PACKET_SIZE);
            return;
        }

        int res = sendto(sock, rtp_send_packet, packet_size, 0, (struct sockaddr*)to, sizeof(struct sockaddr));
        if (unlikely(res < 0)) {
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
                        rtp_send_packets(sock, &to, fb);
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