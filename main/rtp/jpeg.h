#include "esp_camera.h"
#include "esp_log.h"

#include "common.h"

#define MAX_QUANT_TABLES 4
#define QUANT_TABLE_SIZE 64

#define JPEG_TYPE_YUV422 0U
#define JPEG_Q_DEFAULT 255U

static const char* const TAG = "rtp_sender";

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

static inline void set_fragment_offset(uint8_t* buf, const size_t offset) {
    buf[0] = (offset >> 16) & 0xFF;
    buf[1] = (offset >> 8) & 0xFF;
    buf[2] = offset & 0xFF;
}

static inline size_t min(size_t a, size_t b) {
    return (a < b) ? a : b;
}

static const uint8_t* get_jpeg_data(const uint8_t* buf, size_t size, size_t* out_size) {
    if (unlikely(!buf || !out_size || size < 4)) {
        if (out_size)
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

    *out_size = pos - data_start;
    return buf + data_start;
}

static size_t extract_quant_tables_refs(const uint8_t* buf, size_t size, const uint8_t** tables) {
    if (unlikely((buf == NULL) || (tables == NULL))) {
        return 0;
    }

    size_t pos = 0;
    size_t count = 0;

    while ((pos + 4) <= size) {
        if ((buf[pos] == 0xFF) && (buf[pos + 1] == 0xDB)) {
            pos += 2;

            /* Read DQT segment length */
            if ((pos + 2) > size) {
                break;
            }

            uint16_t seg_len = ((uint16_t)buf[pos] << 8) | (uint16_t)buf[pos + 1];
            pos += 2;

            if ((seg_len < 2) || ((pos + seg_len - 2) > size)) {
                break; /* invalid segment */
            }

            size_t seg_end = pos + (size_t)seg_len - 2;

            /* Parse all tables inside this DQT segment */
            while ((pos < seg_end) && (count < MAX_QUANT_TABLES)) {
                uint8_t table_info = buf[pos++];
                uint8_t table_id = table_info & 0x0F;
                uint8_t precision = (table_info >> 4) & 0x0F;

                size_t table_size = (precision == 0) ? 64 : 128;

                if ((pos + table_size) > seg_end) {
                    break;
                }

                if ((precision == 0) && (table_id < MAX_QUANT_TABLES)) {
                    tables[count++] = &buf[pos];
                }

                pos += table_size;
            }

            /* Jump to end of DQT segment */
            pos = seg_end;
        } else {
            pos++;
        }
    }

    return count;
}

/**
 * RTP send packets (fragmented for full JPEG)
 */
static void rtp_send_jpeg_packets(int sock, const struct sockaddr_in* to, uint8_t* buf, const camera_fb_t* fb) {

    size_t jpeg_size;
    const uint8_t* jpeg_data = get_jpeg_data(fb->buf, fb->len, &jpeg_size);
    if (unlikely(jpeg_data == NULL)) {
        ESP_LOGE(TAG, "empty jpeg payload");
        return;
    }

    const uint8_t* quant_tables[MAX_QUANT_TABLES];
    size_t quant_tables_count = extract_quant_tables_refs(fb->buf, fb->len, quant_tables);
    configASSERT(quant_tables_count <= MAX_QUANT_TABLES);

    struct rtp_header* header;
    struct rtp_jpeg_header* jpeg_header;
    uint8_t* payload;
    size_t data_index = 0;

    // Prepare common headers
    header = (struct rtp_header*)buf;
    header->version = RTP_VERSION;
    header->ssrc = htonl(RTP_JPEG_SSRC);
    // Use camera timestamp converted to RTP units (90kHz)
    uint32_t rtp_ts = (uint32_t)(fb->timestamp.tv_sec * 90000ULL + fb->timestamp.tv_usec * 90ULL / 1000ULL);
    header->timestamp = htonl(rtp_ts);

    uint16_t seq = esp_random() & 0xFFFF;

    jpeg_header = (struct rtp_jpeg_header*)(buf + sizeof(struct rtp_header));
    jpeg_header->type_specific = 0;
    jpeg_header->type = JPEG_TYPE_YUV422; // YUV 4:2:2
    jpeg_header->q = JPEG_Q_DEFAULT;      // Default quantization table
    jpeg_header->width = fb->width / 8;
    jpeg_header->height = fb->height / 8;

    // Fragment and send
    while (data_index < jpeg_size) {
        header->seqNum = htons(seq++); // RFC 3550

        size_t tables_size =
            (data_index == 0) ? (quant_tables_count * QUANT_TABLE_SIZE) + sizeof(struct jpeg_quant_header) : 0;
        payload = buf + sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header);

        if (tables_size > 0) {

            struct jpeg_quant_header* qh = (struct jpeg_quant_header*)payload;
            qh->mbz = 0;
            qh->precision = 0; // 8-bit tables
            qh->length = htons(quant_tables_count * QUANT_TABLE_SIZE);
            payload += sizeof(*qh);

            for (size_t i = 0; i < quant_tables_count; i++) {
                memcpy(payload, quant_tables[i], QUANT_TABLE_SIZE);
                payload += QUANT_TABLE_SIZE;
            }
        }

        size_t chunk_size = min(RTP_PAYLOAD_SIZE - tables_size, jpeg_size - data_index);

        set_fragment_offset(jpeg_header->fragment_offset, data_index);
        memcpy(payload, jpeg_data + data_index, chunk_size);

        uint8_t marker = ((data_index + chunk_size) >= jpeg_size) ? RTP_MARKER_MASK : 0U;
        header->payloadtype = (uint8_t)(RTP_JPEG_PAYLOADTYPE | marker);

        size_t packet_size = sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header) + tables_size + chunk_size;
        if (unlikely(packet_size > RTP_PACKET_SIZE)) {
            ESP_LOGE(TAG, "Packet size %zu exceeds RTP_PACKET_SIZE %d", packet_size, RTP_PACKET_SIZE);
            break;
        }

        int res = sendto(sock, buf, packet_size, 0, (struct sockaddr*)to, sizeof(struct sockaddr));
        if (unlikely(res < 0)) {
            ESP_LOGE(TAG, "sendto error: %d (%s), skipping this packet", errno, strerror(errno));
            break; // TODO: я чет не уверен что надо весь кадр скипать
        }

        /* Throttle RTP packets to avoid network congestion */
        vTaskDelay(pdMS_TO_TICKS(RTP_SEND_DELAY));
        data_index += chunk_size;
    }
}