#include "esp_camera.h"
#include "esp_log.h"

#include "common.h"

#define MAX_QUANT_TABLES 4
#define QUANT_TABLE_SIZE 64

#define min(a, b) ((a) < (b) ? (a) : (b))

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

static int extract_quant_tables_refs(const uint8_t* buf, size_t size, const uint8_t** tables) {
    size_t pos = 0;
    int count = 0;
    while (pos < size - 4) {
        if (buf[pos] == 0xFF && buf[pos + 1] == 0xDB) {
            pos += 2;

            if (pos + 2 > size)
                break;
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
            tables[count++] = buf + pos;
            pos += QUANT_TABLE_SIZE;
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
    if (jpeg_data == NULL) {
        ESP_LOGE(TAG, "empty jpeg payload");
        return;
    }

    const uint8_t* quant_tables[MAX_QUANT_TABLES];
    memset(quant_tables, 0, sizeof(const uint8_t*) * MAX_QUANT_TABLES);

    int quant_tables_count = extract_quant_tables_refs(fb->buf, fb->len, quant_tables);

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
    header->seqNum = htons(esp_random() & 0xFFFF); // RFC 3550

    jpeg_header = (struct rtp_jpeg_header*)(buf + sizeof(struct rtp_header));
    jpeg_header->type_specific = 0;
    jpeg_header->type = 0; // YUV 4:2:2
    jpeg_header->q = 255;  // Default quantization table
    jpeg_header->width = fb->width / 8;
    jpeg_header->height = fb->height / 8;

    // Fragment and send
    while (data_index < jpeg_size) {
        size_t tables_size =
            (data_index == 0) ? (quant_tables_count * QUANT_TABLE_SIZE) + sizeof(struct jpeg_quant_header) : 0;
        payload = buf + sizeof(struct rtp_header) + sizeof(struct rtp_jpeg_header);

        if (tables_size > 0) {

            struct jpeg_quant_header* qh = (struct jpeg_quant_header*)payload;
            qh->mbz = 0;
            qh->precision = 0; // 8-bit tables
            qh->length = htons(quant_tables_count * QUANT_TABLE_SIZE);
            payload += sizeof(*qh);

            for (int i = 0; i < quant_tables_count; i++) {
                memcpy(payload, quant_tables[i], QUANT_TABLE_SIZE);
                payload += QUANT_TABLE_SIZE;
            }
        }

        size_t chunk_size = min(RTP_PAYLOAD_SIZE - tables_size, jpeg_size - data_index);

        set_fragment_offset(jpeg_header->fragment_offset, data_index);
        memcpy(payload, jpeg_data + data_index, chunk_size);

        header->payloadtype = RTP_JPEG_PAYLOADTYPE | (((data_index + chunk_size) >= jpeg_size) ? RTP_MARKER_MASK : 0);
        header->seqNum = htons(ntohs(header->seqNum) + 1);

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

        vTaskDelay(pdMS_TO_TICKS(RTP_SEND_DELAY));
        data_index += chunk_size;
    }
}