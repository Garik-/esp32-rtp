#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"

#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"

#include "freertos/FreeRTOS.h"

#include "include/pdm_mic.h"

#define SAMPLE_RATE 8000
#define I2S_PORT I2S_NUM_0
#define PDM_DATA GPIO_NUM_41
#define PDM_CLK GPIO_NUM_42

#define READ_TIMEOUT_MS 100

static const char* TAG = "pdm_mic";

// https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/pcm_tablegen.h

#define QUANT_MASK (0xf) /* Quantization field mask. */
#define SEG_SHIFT (4)    /* Left shift for segment number. */
#define SEG_MASK (0x70)  /* Segment field mask. */
#define SIGN_BIT (0x80)  /* Sign bit for a A-law byte. */
#define BIAS (0x84)      /* Bias for linear code. */

#ifdef CONFIG_ESPRTP_AUDIO_SUPPORT
static DRAM_ATTR uint8_t linear_to_ulaw[16384];
static i2s_chan_handle_t rx_chan;
#endif

static
    __attribute__((cold)) void build_xlaw_table(uint8_t* linear_to_xlaw, int (*xlaw2linear)(unsigned char), int mask) {
    int i, j, v, v1, v2;

    j = 1;
    linear_to_xlaw[8192] = mask;
    for (i = 0; i < 127; i++) {
        v1 = xlaw2linear(i ^ mask);
        v2 = xlaw2linear((i + 1) ^ mask);
        v = (v1 + v2 + 4) >> 3;
        for (; j < v; j += 1) {
            linear_to_xlaw[8192 - j] = (i ^ (mask ^ 0x80));
            linear_to_xlaw[8192 + j] = (i ^ mask);
        }
    }
    for (; j < 8192; j++) {
        linear_to_xlaw[8192 - j] = (127 ^ (mask ^ 0x80));
        linear_to_xlaw[8192 + j] = (127 ^ mask);
    }
    linear_to_xlaw[0] = linear_to_xlaw[1];
}

static __attribute__((cold)) int ulaw2linear(unsigned char u_val) {
    int t;

    /* Complement to obtain normal u-law value. */
    u_val = ~u_val;

    /*
     * Extract and bias the quantization bits. Then
     * shift up by the segment number and subtract out the bias.
     */
    t = ((u_val & QUANT_MASK) << 3) + BIAS;
    t <<= ((unsigned)u_val & SEG_MASK) >> SEG_SHIFT;

    return (u_val & SIGN_BIT) ? (BIAS - t) : (t - BIAS);
}

static void pcm_ulaw_tableinit(void) {
    build_xlaw_table(linear_to_ulaw, ulaw2linear, 0xff);
}

esp_err_t __attribute__((cold)) pdm_mic_init() {

    pcm_ulaw_tableinit();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx_chan), TAG, "i2s_new_channel");

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {
                .clk = PDM_CLK,
                .din = PDM_DATA,
                .invert_flags =
                    {
                        .clk_inv = false,
                    },
            },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_cfg), TAG, "i2s_channel_init_pdm_rx_mode");
    return i2s_channel_enable(rx_chan);
}

#define NOISE_RMS_THRESH 500U // RMS порог шума, подбирать
#define ATTACK_FACTOR 0.2f    // скорость открытия noise gate
#define RELEASE_FACTOR 0.05f  // скорость закрытия noise gate

// плавная регулировка громкости (0.0 = тихо, 1.0 = обычная, 2.0 = +100%)
#define VOLUME_GAIN 2.5f;

static inline int16_t abs16(int16_t x) {
    int16_t mask = x >> 15;
    return (x + mask) ^ mask;
}

esp_err_t pdm_mic_read(uint8_t* ulaw_buffer, size_t* ulaw_size) {
    size_t bytes_read = 0;
    int16_t pcm8k[FRAME_8K];

    ESP_RETURN_ON_ERROR(i2s_channel_read(rx_chan, pcm8k, sizeof(pcm8k), &bytes_read, pdMS_TO_TICKS(READ_TIMEOUT_MS)),
                        TAG, "i2s_channel_read");

    size_t samples_read = bytes_read / sizeof(int16_t);

#ifdef NOISE_GATE
    // --- RMS через среднее абсолютное значение ---
    static float gate_gain = 0.0f; // плавный gain noise gate
    uint32_t sum_abs = 0;
    for (size_t i = 0; i < samples_read; i++) {
        sum_abs += abs16(pcm8k[i]);
    }
    const float rms = sum_abs / (float)samples_read;

    // --- плавный noise gate ---
    const bool open = __builtin_expect(rms > NOISE_RMS_THRESH, 0);
    gate_gain += open ? ATTACK_FACTOR * (1.0f - gate_gain) : RELEASE_FACTOR * (0.0f - gate_gain);

    if (gate_gain < 0.0f)
        gate_gain = 0.0f;
    if (gate_gain > 1.0f)
        gate_gain = 1.0f;
#endif

    for (size_t i = 0; i < samples_read; i++) {
#ifdef NOISE_GATE
        float sample = pcm8k[i] * gate_gain * VOLUME_GAIN;
#else
        float sample = pcm8k[i] * VOLUME_GAIN;
#endif
        ulaw_buffer[i] = linear_to_ulaw[((int16_t)sample + 32768) >> 2];
    }

    *ulaw_size = samples_read;

    return ESP_OK;
}