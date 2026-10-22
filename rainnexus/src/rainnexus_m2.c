/**
 * @file rainnexus_m2.c
 * @brief RainNexus M2/M5：WSS 建连、会话、音频上行和下行播放
 */

#include "system/includes.h"
#include "os/os_api.h"
#include "web_socket/websocket_api.h"
#include "web_socket/websocket_ssl.h"
#include "wifi/wifi_connect.h"
#include "fs/fs.h"
#include "server/audio_server.h"
#include "server/server_core.h"
#include "asm/gpio.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "rainnexus_config.h"
#include "rainnexus_m2.h"

#define RAINNEXUS_M2_RX_JSON_CAPACITY  1024U
#define RAINNEXUS_M3_SESSION_ID_CAPACITY 128U
#define RAINNEXUS_M4_AUDIO_PACKET_CAPACITY 2048U
#define RAINNEXUS_M4_OGG_STREAM_CAPACITY (128U * 1024U)
#define RAINNEXUS_M4_OGG_OPEN_PREROLL 1024U
/* The server may pause between tts/sentence_start segments.  Only tts/stop
 * is an authoritative end marker; this is a lost-stop fallback. */
#define RAINNEXUS_M4_STREAM_COMPLETE_IDLE_MS 5000U
#define RAINNEXUS_LOG_SERVER_RAW_JSON 1
#define RAINNEXUS_LOG_SERVER_OPUS_HEX 0
#define RAINNEXUS_LOG_HEX_BYTES_PER_LINE 16U
#define RAINNEXUS_DEVICE_ID "00:ff:03:39:29:3b"
#define RAINNEXUS_M4_OGG_PAGE_CAPACITY \
    (27U + 1U + (RAINNEXUS_M4_AUDIO_PACKET_CAPACITY / 255U) + 1U + \
     RAINNEXUS_M4_AUDIO_PACKET_CAPACITY)
#define RAINNEXUS_M4_OGG_SERIAL 0x524e4f47UL

#ifndef RAINNEXUS_M5_SESSION_TIMEOUT_MS
#define RAINNEXUS_M5_SESSION_TIMEOUT_MS 30000U
#endif
#ifndef RAINNEXUS_M5_AUDIO_QUEUE_DEPTH
#define RAINNEXUS_M5_AUDIO_QUEUE_DEPTH 8U
#endif
#ifndef RAINNEXUS_M5_AUDIO_PACKET_CAPACITY
#define RAINNEXUS_M5_AUDIO_PACKET_CAPACITY 2048U
#endif
#ifndef RAINNEXUS_M5_SAMPLE_RATE
#define RAINNEXUS_M5_SAMPLE_RATE 16000U
#endif
#ifndef RAINNEXUS_M5_AUDIO_FRAME_MS
#define RAINNEXUS_M5_AUDIO_FRAME_MS 60U
#endif
#ifndef RAINNEXUS_M5_AUDIO_BITRATE
#define RAINNEXUS_M5_AUDIO_BITRATE 16000U
#endif
#ifndef RAINNEXUS_M5_MIC_CHANNEL
#define RAINNEXUS_M5_MIC_CHANNEL 1U
#endif
#ifndef RAINNEXUS_M5_MIC_GAIN
#define RAINNEXUS_M5_MIC_GAIN 90U
#endif
#ifndef RAINNEXUS_M5_PCM_RING_CAPACITY
#define RAINNEXUS_M5_PCM_RING_CAPACITY (32U * 1024U)
#endif
#define RAINNEXUS_M5_PCM_FRAME_BYTES \
    ((RAINNEXUS_M5_SAMPLE_RATE / 100U) * 2U * RAINNEXUS_M5_MIC_CHANNEL)
/* The SDK's virtual Opus path is known-good with a 20 ms PCM frame. */
#define RAINNEXUS_M5_OPUS_FRAME_BYTES 640U
#define RAINNEXUS_M5_KEY_PORT IO_PORTC_06

#ifdef CONFIG_AISP_MIC0_ADC_CHANNEL
#define RAINNEXUS_M5_EFFECTIVE_MIC_CHANNEL CONFIG_AISP_MIC0_ADC_CHANNEL
#else
#define RAINNEXUS_M5_EFFECTIVE_MIC_CHANNEL RAINNEXUS_M5_MIC_CHANNEL
#endif

enum {
    RAINNEXUS_M3_DISCONNECT_NONE = 0,
    RAINNEXUS_M3_DISCONNECT_PEER = 1,
    RAINNEXUS_M3_DISCONNECT_CLOSE = 2,
    RAINNEXUS_M3_DISCONNECT_TOKEN = 3,
};

extern u32 get_malloc_remain_heap_size(void);
/* The active AC7916AB development board uses PC7 as active-low PA mute. */
extern void dac_mute_control(char enable, char force);

typedef struct rainnexus_m2_context rainnexus_m2_context_t;

struct rainnexus_m2_context {
    struct websocket_struct *ws;
    char *url;
    char rx_json[RAINNEXUS_M2_RX_JSON_CAPACITY];
    unsigned int rx_json_len;
    volatile int hello_result;
    volatile int hello_validated;
    volatile int disconnected;
    volatile int disconnect_reason;
    volatile int token_expired;
    unsigned int event_count;
    unsigned int pong_count;
    char session_id[RAINNEXUS_M3_SESSION_ID_CAPACITY];
    unsigned int audio_sample_rate;
    unsigned int audio_frame_duration;
    unsigned int audio_frame_samples;
    volatile int audio_active;
    volatile int audio_start_failed;
    void *audio_decoder;
    OS_SEM audio_data_sem;
    int audio_sem_initialized;
    volatile int audio_decoder_open_requested;
    u8 *audio_ring_buffer;
    u8 *audio_page;
    u8 *audio_fragment;
    unsigned int audio_fragment_len;
    int audio_binary_fragment_active;
    unsigned int audio_frames_received;
    unsigned int audio_frames_queued;
    unsigned int audio_frames_dropped;
    unsigned int audio_decode_errors;
    unsigned int audio_vfs_read_calls;
    unsigned int audio_vfs_read_bytes;
    unsigned int audio_vfs_waits;
    volatile unsigned int audio_vfs_offset;
    unsigned int audio_vfs_seek_calls;
    volatile unsigned int audio_ogg_stream_length;
    unsigned int audio_tts_start_events;
    unsigned int audio_tts_stop_events;
    unsigned int audio_ogg_sequence;
    u64 audio_ogg_granule;
    int audio_ogg_headers_written;
    volatile int audio_stream_eof;
    u32 audio_last_packet_ms;
    int ws_initialized;
    void *audio_mic_encoder;
    void *audio_opus_encoder;
    OS_MUTEX uplink_mutex;
    int uplink_mutex_initialized;
    OS_SEM pcm_data_sem;
    int pcm_sem_initialized;
    cbuffer_t pcm_ring;
    u8 *pcm_ring_storage;
    volatile int pcm_encoder_active;
    volatile int opus_encoder_active;
    u8 *uplink_packet_storage;
    u16 *uplink_packet_lengths;
    u8 *uplink_tx_buffer;
    unsigned int uplink_queue_head;
    unsigned int uplink_queue_tail;
    unsigned int uplink_queue_count;
    volatile int uplink_encoder_active;
    unsigned int uplink_frames_encoded;
    unsigned int uplink_frames_sent;
    unsigned int uplink_frames_dropped;
    unsigned int uplink_send_errors;
    unsigned int uplink_encoder_errors;
    volatile int recording_start_requested;
    volatile int recording_stop_requested;
    volatile int recording_active;
    volatile int recording_starting;
    int key_poll_initialized;
    int key_poll_last_pressed;
    int key_poll_reported;
    unsigned int key_poll_stable_count;
};

/* 目标 SDK 的接收回调没有 priv 参数；M2 只允许存在一个验证连接。 */
static rainnexus_m2_context_t *g_m2_context;

static void rainnexus_m5_key_click(rainnexus_m2_context_t *ctx)
{
    if (ctx == NULL || !ctx->hello_validated || ctx->disconnected ||
        ctx->token_expired) {
        return;
    }
    if (ctx->recording_active || ctx->recording_starting ||
        ctx->recording_start_requested) {
        ctx->recording_stop_requested = 1;
        printf("[RN-M5] key click: recording stop requested\n");
    } else if (!ctx->recording_start_requested) {
        ctx->recording_start_requested = 1;
        printf("[RN-M5] key click: recording start requested\n");
    }
}

static void rainnexus_m5_poll_key(rainnexus_m2_context_t *ctx)
{
    int pressed;

    if (ctx == NULL) {
        return;
    }
    /* The 7916A IO key is ONE_PORT_TO_LOW, so a low level means pressed. */
    pressed = !gpio_read(RAINNEXUS_M5_KEY_PORT);
    if (!ctx->key_poll_initialized) {
        ctx->key_poll_initialized = 1;
        ctx->key_poll_last_pressed = pressed;
        ctx->key_poll_reported = pressed;
        ctx->key_poll_stable_count = 0U;
        return;
    }
    if (pressed != ctx->key_poll_last_pressed) {
        ctx->key_poll_last_pressed = pressed;
        ctx->key_poll_stable_count = 0U;
        if (!pressed) {
            ctx->key_poll_reported = 0;
        }
        return;
    }
    if (ctx->key_poll_stable_count < 3U) {
        ctx->key_poll_stable_count++;
    }
    if (pressed && ctx->key_poll_stable_count >= 3U &&
        !ctx->key_poll_reported) {
        ctx->key_poll_reported = 1;
        rainnexus_m5_key_click(ctx);
    }
}

static u32 rainnexus_m4_ogg_crc(const u8 *data, unsigned int len)
{
    u32 crc = 0;
    unsigned int i;
    unsigned int bit;

    for (i = 0U; i < len; i++) {
        crc ^= (u32)data[i] << 24;
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x80000000UL) ?
                  (crc << 1) ^ 0x04c11db7UL : crc << 1;
        }
    }
    return crc;
}

static void rainnexus_m4_ogg_write_le32(u8 *dst, u32 value)
{
    dst[0] = (u8)value;
    dst[1] = (u8)(value >> 8);
    dst[2] = (u8)(value >> 16);
    dst[3] = (u8)(value >> 24);
}

static void rainnexus_m4_ogg_write_le64(u8 *dst, u64 value)
{
    unsigned int i;

    for (i = 0U; i < 8U; i++) {
        dst[i] = (u8)(value >> (i * 8U));
    }
}

/* One Ogg page carries exactly one Opus packet. */
static int rainnexus_m4_ogg_write_page(rainnexus_m2_context_t *ctx,
                                        const u8 *packet,
                                        unsigned int packet_len,
                                        u8 header_type,
                                        u64 granule)
{
    unsigned int segments;
    unsigned int page_len;
    unsigned int index;
    unsigned int remaining;
    u32 crc;

    if (ctx == NULL || (packet == NULL && packet_len != 0U) ||
        packet_len > RAINNEXUS_M4_AUDIO_PACKET_CAPACITY ||
        ctx->audio_page == NULL || ctx->audio_ring_buffer == NULL) {
        return -1;
    }
    segments = packet_len / 255U + 1U;
    page_len = 27U + segments + packet_len;
    if (page_len > RAINNEXUS_M4_OGG_PAGE_CAPACITY ||
        ctx->audio_ogg_stream_length >
        RAINNEXUS_M4_OGG_STREAM_CAPACITY - page_len) {
        return -1;
    }

    memset(ctx->audio_page, 0, page_len);
    memcpy(ctx->audio_page, "OggS", 4U);
    ctx->audio_page[4] = 0U;
    ctx->audio_page[5] = header_type;
    rainnexus_m4_ogg_write_le64(ctx->audio_page + 6U, granule);
    rainnexus_m4_ogg_write_le32(ctx->audio_page + 14U, RAINNEXUS_M4_OGG_SERIAL);
    rainnexus_m4_ogg_write_le32(ctx->audio_page + 18U,
                                 ctx->audio_ogg_sequence);
    ctx->audio_page[26] = (u8)segments;
    remaining = packet_len;
    for (index = 0U; index < segments; index++) {
        unsigned int lace = remaining > 255U ? 255U : remaining;

        ctx->audio_page[27U + index] = (u8)lace;
        remaining -= lace;
    }
    if (packet_len != 0U) {
        memcpy(ctx->audio_page + 27U + segments, packet, packet_len);
    }
    crc = rainnexus_m4_ogg_crc(ctx->audio_page, page_len);
    rainnexus_m4_ogg_write_le32(ctx->audio_page + 22U, crc);
    memcpy(ctx->audio_ring_buffer + ctx->audio_ogg_stream_length,
           ctx->audio_page, page_len);
    ctx->audio_ogg_stream_length += page_len;
    ctx->audio_ogg_sequence++;
    return 0;
}

/* Mark the assembled Ogg stream as finite before opening the decoder. */
static void rainnexus_m4_audio_finish_stream(rainnexus_m2_context_t *ctx,
                                             const char *reason)
{
    if (ctx == NULL || ctx->audio_stream_eof) {
        return;
    }
    if (ctx->audio_ogg_headers_written &&
        rainnexus_m4_ogg_write_page(ctx, NULL, 0U, 0x04U,
                                    ctx->audio_ogg_granule) != 0) {
        printf("[RN-M4] Ogg EOS page unavailable; finishing VFS stream\n");
    }
    ctx->audio_stream_eof = 1;
    if (ctx->audio_frames_received != 0U && ctx->audio_decoder == NULL) {
        ctx->audio_decoder_open_requested = 1;
    }
    if (ctx->audio_sem_initialized) {
        os_sem_post(&ctx->audio_data_sem);
    }
    printf("[RN-M4] downlink stream finished: %s, bytes=%u frames=%u\n",
           reason != NULL ? reason : "unknown", ctx->audio_ogg_stream_length,
           ctx->audio_frames_received);
}

static int rainnexus_m4_ogg_write_headers(rainnexus_m2_context_t *ctx)
{
    static const u8 opus_head[] = {
        'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
        1, 1, 0, 0, 0x80, 0x3e, 0, 0, 0, 0, 0,
    };
    static const u8 opus_tags[] = {
        'O', 'p', 'u', 's', 'T', 'a', 'g', 's',
        9, 0, 0, 0, 'R', 'a', 'i', 'n', 'N', 'e', 'x', 'u', 's',
        0, 0, 0, 0,
    };

    if (ctx->audio_ogg_headers_written) {
        return 0;
    }
    if (rainnexus_m4_ogg_write_page(ctx, opus_head, sizeof(opus_head),
                                     0x02U, 0) != 0 ||
        rainnexus_m4_ogg_write_page(ctx, opus_tags, sizeof(opus_tags),
                                     0x00U, 0) != 0) {
        return -1;
    }
    ctx->audio_ogg_headers_written = 1;
    return 0;
}

static int rainnexus_m4_audio_vfs_fread(void *file, void *data, u32 len)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)file;
    u32 available;

    if (ctx == NULL || data == NULL || len == 0U ||
        !ctx->audio_sem_initialized) {
        return 0;
    }

    /*
     * Ogg parsing seeks back to the start of a page after scanning it, so
     * retain the received stream for this bounded session instead of using a
     * destructive FIFO.  While a reply is streaming, do not return a short
     * read because Ogg Opus treats it as EOF.  Once TTS ends (or the service
     * has been idle long enough to imply a missing tts/stop), an Ogg EOS page
     * closes the logical stream.  The final partial parser read is padded in
     * place below because this SDK drops a short final read.
     */
    while (ctx->audio_active) {
        if (ctx->audio_vfs_offset > ctx->audio_ogg_stream_length) {
            return 0;
        }
        available = ctx->audio_ogg_stream_length - ctx->audio_vfs_offset;
        if (available >= len) {
            int read_len = (int)len;

            memcpy(data, ctx->audio_ring_buffer + ctx->audio_vfs_offset, len);

            ctx->audio_vfs_read_calls++;
            ctx->audio_vfs_read_bytes += (unsigned int)read_len;
            ctx->audio_vfs_offset += (unsigned int)read_len;
            if (ctx->audio_vfs_read_calls <= 3U ||
                (ctx->audio_vfs_read_calls % 32U) == 0U) {
                printf("[RN-M4] Ogg VFS read: count=%u req=%u got=%u "
                       "remain=%u\n", ctx->audio_vfs_read_calls,
                       (unsigned int)len, (unsigned int)read_len,
                       ctx->audio_ogg_stream_length - ctx->audio_vfs_offset);
            }
            return read_len;
        }
        if (ctx->audio_stream_eof) {
            if (available != 0U) {
                /*
                 * The SDK's Ogg reader drops a short final fread.  After an
                 * EOS page it is safe to complete this parser-only read with
                 * zeroes; the following read returns real EOF.  Do not grow
                 * stream_length: seeks must still see the real Ogg length.
                 */
                memcpy(data, ctx->audio_ring_buffer + ctx->audio_vfs_offset,
                       available);
                memset((u8 *)data + available, 0, len - available);
                ctx->audio_vfs_read_calls++;
                ctx->audio_vfs_read_bytes += len;
                ctx->audio_vfs_offset += len;
                printf("[RN-M4] Ogg VFS final read padded: req=%u data=%u\n",
                       (unsigned int)len, available);
                return (int)len;
            }
            return 0;
        }
        ctx->audio_vfs_waits++;
        os_sem_pend(&ctx->audio_data_sem, 0);
    }
    return 0;
}

static int rainnexus_m4_audio_vfs_fseek(void *file, u32 offset, int seek_mode)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)file;
    unsigned int target;

    if (ctx == NULL || !ctx->audio_sem_initialized) {
        return -1;
    }
    if (seek_mode == SEEK_SET) {
        target = offset;
    } else if (seek_mode == SEEK_CUR) {
        target = ctx->audio_vfs_offset + offset;
    } else if (seek_mode == SEEK_END) {
        target = ctx->audio_ogg_stream_length + offset;
    } else {
        printf("[RN-M4] Ogg VFS invalid seek mode=%d\n", seek_mode);
        return -1;
    }
    ctx->audio_vfs_seek_calls++;
    while (ctx->audio_active && target > ctx->audio_ogg_stream_length &&
           !ctx->audio_stream_eof) {
        ctx->audio_vfs_waits++;
        os_sem_pend(&ctx->audio_data_sem, 0);
    }
    if (!ctx->audio_active || target > ctx->audio_ogg_stream_length) {
        return -1;
    }
    ctx->audio_vfs_offset = target;
    if (ctx->audio_vfs_seek_calls <= 3U) {
        printf("[RN-M4] Ogg VFS seek: target=%u stream=%u\n", target,
               ctx->audio_ogg_stream_length);
    }
    return 0;
}

static int rainnexus_m4_audio_vfs_ftell(void *file)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)file;

    return ctx != NULL ? (int)ctx->audio_vfs_offset : -1;
}

static int rainnexus_m4_audio_vfs_flen(void *file)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)file;

    return ctx != NULL ? (int)ctx->audio_ogg_stream_length : -1;
}

static const struct audio_vfs_ops rainnexus_m4_audio_vfs_ops = {
    .fread = rainnexus_m4_audio_vfs_fread,
    .fseek = rainnexus_m4_audio_vfs_fseek,
    .ftell = rainnexus_m4_audio_vfs_ftell,
    .flen = rainnexus_m4_audio_vfs_flen,
};

static void rainnexus_m4_audio_server_event(void *priv, int argc, int *argv)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)priv;

    if (ctx == NULL || argc < 1 || argv == NULL) {
        return;
    }
    if (argv[0] == AUDIO_SERVER_EVENT_ERR) {
        ctx->audio_decode_errors++;
    }
    if (argv[0] == AUDIO_SERVER_EVENT_ERR ||
        argv[0] == AUDIO_SERVER_EVENT_END) {
        printf("[RN-M4] audio_server event=%d reads=%u bytes=%u waits=%u "
               "seeks=%u buffered=%u frames=%u queued=%u dropped=%u\n",
               argv[0], ctx->audio_vfs_read_calls,
               ctx->audio_vfs_read_bytes, ctx->audio_vfs_waits,
               ctx->audio_vfs_seek_calls,
               ctx->audio_ogg_stream_length - ctx->audio_vfs_offset,
               ctx->audio_frames_received, ctx->audio_frames_queued,
               ctx->audio_frames_dropped);
    }
}

static int rainnexus_m4_audio_decoder_start(rainnexus_m2_context_t *ctx)
{
    union audio_req req = {0};
    int err;

    if (ctx == NULL || !ctx->audio_active || ctx->audio_decoder != NULL) {
        return -1;
    }
    ctx->audio_decoder = server_open("audio_server", "dec");
    if (ctx->audio_decoder == NULL) {
        printf("[RN-M4] Ogg Opus audio_server open failed\n");
        return -1;
    }
    /*
     * The normal application un-mutes the PA at boot, but make this audio
     * path self-contained as a locked/latching mute GPIO would otherwise
     * leave a healthy decoder and DAC completely silent.
     */
    dac_mute_control(0, 1);
    printf("[RN-M4] speaker PA force-unmuted (PC7=high)\n");
    server_register_event_handler_to_task(ctx->audio_decoder, ctx,
                                          rainnexus_m4_audio_server_event,
                                          "sys_timer");
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.volume = 80;
    req.dec.output_buf_len = 8U * 1024U;
    req.dec.priority = 1;
    req.dec.channel = 0;
    req.dec.sample_rate = 0;
    req.dec.dec_type = "opus";
    req.dec.sample_source = "dac";
    req.dec.attr = AUDIO_ATTR_OPUS_OGG_TYPE;
    req.dec.file = (FILE *)ctx;
    req.dec.vfs_ops = &rainnexus_m4_audio_vfs_ops;
    printf("[RN-M4] opening Ogg Opus decoder with buffered=%u bytes\n",
           ctx->audio_ogg_stream_length - ctx->audio_vfs_offset);
    err = server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
    if (err != 0) {
        printf("[RN-M4] Ogg Opus AUDIO_DEC_OPEN failed: %d\n", err);
        goto fail;
    }
    req.dec.cmd = AUDIO_DEC_START;
    err = server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
    if (err != 0) {
        printf("[RN-M4] Ogg Opus AUDIO_DEC_START failed: %d\n", err);
        goto fail;
    }
    printf("[RN-M4] Ogg Opus decoder started: variable-length frames -> DAC\n");
    return 0;

fail:
    req.dec.cmd = AUDIO_DEC_STOP;
    server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
    server_close(ctx->audio_decoder);
    ctx->audio_decoder = NULL;
    return -1;
}

static int rainnexus_m4_audio_start(rainnexus_m2_context_t *ctx)
{
    if (ctx == NULL || ctx->audio_sample_rate == 0U ||
        ctx->audio_frame_duration != 60U || ctx->audio_frame_samples == 0U) {
        return -1;
    }

    /* A manual session can have several listen/TTS rounds on one WSS link. */
    ctx->audio_decoder_open_requested = 0;
    ctx->audio_fragment_len = 0U;
    ctx->audio_binary_fragment_active = 0;
    ctx->audio_frames_received = 0U;
    ctx->audio_frames_queued = 0U;
    ctx->audio_frames_dropped = 0U;
    ctx->audio_decode_errors = 0U;
    ctx->audio_vfs_read_calls = 0U;
    ctx->audio_vfs_read_bytes = 0U;
    ctx->audio_vfs_waits = 0U;
    ctx->audio_vfs_offset = 0U;
    ctx->audio_vfs_seek_calls = 0U;
    ctx->audio_ogg_stream_length = 0U;
    ctx->audio_ogg_sequence = 0U;
    ctx->audio_ogg_granule = 0U;
    ctx->audio_ogg_headers_written = 0;
    ctx->audio_stream_eof = 0;
    ctx->audio_last_packet_ms = 0U;

    ctx->audio_ring_buffer = (u8 *)malloc(RAINNEXUS_M4_OGG_STREAM_CAPACITY);
    ctx->audio_page = (u8 *)malloc(RAINNEXUS_M4_OGG_PAGE_CAPACITY);
    ctx->audio_fragment = (u8 *)malloc(RAINNEXUS_M4_AUDIO_PACKET_CAPACITY);
    if (ctx->audio_ring_buffer == NULL || ctx->audio_page == NULL ||
        ctx->audio_fragment == NULL) {
        printf("[RN-M4] Ogg Opus buffer allocation failed\n");
        goto fail_buffers;
    }
    if (os_sem_create(&ctx->audio_data_sem, 0) != 0) {
        printf("[RN-M4] Ogg Opus data semaphore create failed\n");
        goto fail_buffers;
    }
    ctx->audio_sem_initialized = 1;
    ctx->audio_active = 1;
    printf("[RN-M4] Ogg Opus adapter ready: waiting for first variable-length frame\n");
    return 0;

fail_buffers:
    free(ctx->audio_ring_buffer);
    free(ctx->audio_page);
    free(ctx->audio_fragment);
    ctx->audio_ring_buffer = NULL;
    ctx->audio_page = NULL;
    ctx->audio_fragment = NULL;
    return -1;
}

static void rainnexus_m4_audio_stop(rainnexus_m2_context_t *ctx);

static int rainnexus_m4_audio_prepare_next_reply(rainnexus_m2_context_t *ctx)
{
    if (ctx == NULL || !ctx->audio_stream_eof) {
        return 0;
    }
    printf("[RN-M4] resetting completed playback for next manual round\n");
    rainnexus_m4_audio_stop(ctx);
    if (rainnexus_m4_audio_start(ctx) != 0) {
        printf("[RN-M4] next-round playback initialization failed\n");
        return -1;
    }
    return 0;
}

static void rainnexus_m4_audio_stop(rainnexus_m2_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->audio_active = 0;
    if (ctx->audio_sem_initialized) {
        os_sem_post(&ctx->audio_data_sem);
    }
    if (ctx->audio_decoder != NULL) {
        union audio_req req = {0};

        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
        server_close(ctx->audio_decoder);
        ctx->audio_decoder = NULL;
    }
    if (ctx->audio_sem_initialized) {
        os_sem_del(&ctx->audio_data_sem, OS_DEL_ALWAYS);
        ctx->audio_sem_initialized = 0;
    }
    free(ctx->audio_ring_buffer);
    free(ctx->audio_page);
    free(ctx->audio_fragment);
    ctx->audio_ring_buffer = NULL;
    ctx->audio_page = NULL;
    ctx->audio_fragment = NULL;
    printf("[RN-M4] playback stopped: received=%u queued=%u dropped=%u "
           "decode_errors=%u reads=%u bytes=%u waits=%u\n",
           ctx->audio_frames_received, ctx->audio_frames_queued,
           ctx->audio_frames_dropped, ctx->audio_decode_errors,
           ctx->audio_vfs_read_calls, ctx->audio_vfs_read_bytes,
           ctx->audio_vfs_waits);
}

static void rainnexus_m4_audio_receive(rainnexus_m2_context_t *ctx,
                                       u8 *buf, u32 len, u32 payload_len,
                                       u8 frame_type, u8 opcode,
                                       u8 end_seq)
{
    unsigned int remaining;
    int page_written = 0;

    if (ctx == NULL || !ctx->hello_validated || !ctx->audio_active ||
        buf == NULL || len == 0U) {
        return;
    }

    /*
     * RainNexus sends bare variable-length Opus packets.  Wrap each complete
     * frame in an Ogg page so the documented Ogg Opus decoder keeps its frame
     * boundaries without relying on the incompatible CBR mode.
     */
    if (opcode == WS_TYPE_BINDATA) {
        if (ctx->audio_stream_eof) {
            printf("[RN-M4] ignoring unexpected audio after stream EOF\n");
            return;
        }
        ctx->audio_fragment_len = 0U;
        ctx->audio_binary_fragment_active = 1;
        if (payload_len > RAINNEXUS_M4_AUDIO_PACKET_CAPACITY) {
            ctx->audio_binary_fragment_active = 0;
            ctx->audio_frames_dropped++;
            printf("[RN-M4] Opus frame exceeds Ogg adapter capacity: %u\n",
                   (unsigned int)payload_len);
            return;
        }
    } else if (opcode != WS_TYPE_SEQ || !ctx->audio_binary_fragment_active) {
        return;
    }
    remaining = RAINNEXUS_M4_AUDIO_PACKET_CAPACITY - ctx->audio_fragment_len;
    if (len > remaining) {
        ctx->audio_binary_fragment_active = 0;
        ctx->audio_fragment_len = 0U;
        ctx->audio_frames_dropped++;
        printf("[RN-M4] Opus frame fragment overflow: %u\n", (unsigned int)len);
        return;
    }
    memcpy(ctx->audio_fragment + ctx->audio_fragment_len, buf, len);
    ctx->audio_fragment_len += len;
    if (!end_seq && opcode == WS_TYPE_SEQ &&
        (payload_len == 0U || ctx->audio_fragment_len < payload_len)) {
        return;
    }
    if (!end_seq && opcode == WS_TYPE_BINDATA &&
        !(frame_type & WS_TYPE_FIN) && payload_len > len) {
        return;
    }

#if RAINNEXUS_LOG_SERVER_OPUS_HEX
    {
        unsigned int offset;

        printf("[RN-M4] RX Opus raw: frame=%u bytes=%u\n",
               ctx->audio_frames_received + 1U,
               ctx->audio_fragment_len);
        for (offset = 0U; offset < ctx->audio_fragment_len;
             offset += RAINNEXUS_LOG_HEX_BYTES_PER_LINE) {
            unsigned int index;
            unsigned int line_len = ctx->audio_fragment_len - offset;

            if (line_len > RAINNEXUS_LOG_HEX_BYTES_PER_LINE) {
                line_len = RAINNEXUS_LOG_HEX_BYTES_PER_LINE;
            }
            printf("[RN-M4] RX Opus raw[%04u]:", offset);
            for (index = 0U; index < line_len; index++) {
                printf(" %02X", ctx->audio_fragment[offset + index]);
            }
            printf("\n");
        }
    }
#endif

    ctx->audio_last_packet_ms = timer_get_ms();
    if (rainnexus_m4_ogg_write_headers(ctx) == 0) {
        u64 next_granule = ctx->audio_ogg_granule +
                           (u64)ctx->audio_frame_duration * 48U;

        if (rainnexus_m4_ogg_write_page(ctx, ctx->audio_fragment,
                                         ctx->audio_fragment_len,
                                         0x00U, next_granule) == 0) {
            ctx->audio_ogg_granule = next_granule;
            page_written = 1;
        }
    }
    ctx->audio_binary_fragment_active = 0;
    ctx->audio_frames_received++;
    /*
     * Log only a tiny format fingerprint.  This distinguishes a bare Opus
     * packet from a service-specific wrapper without exposing audio payload.
     */
    if (ctx->audio_frames_received == 1U ||
        (ctx->audio_frames_received % 20U) == 0U) {
        printf("[RN-M4] Opus packet format: count=%u len=%u toc=%02x "
               "head=%02x%02x%02x%02x\n",
               ctx->audio_frames_received, ctx->audio_fragment_len,
               ctx->audio_fragment[0], ctx->audio_fragment[0],
               ctx->audio_fragment_len > 1U ? ctx->audio_fragment[1] : 0U,
               ctx->audio_fragment_len > 2U ? ctx->audio_fragment[2] : 0U,
               ctx->audio_fragment_len > 3U ? ctx->audio_fragment[3] : 0U);
    }
    if (page_written) {
        ctx->audio_frames_queued++;
        os_sem_post(&ctx->audio_data_sem);
        if (ctx->audio_decoder == NULL &&
            ctx->audio_ogg_stream_length >= RAINNEXUS_M4_OGG_OPEN_PREROLL) {
            ctx->audio_decoder_open_requested = 1;
        }
        if (ctx->audio_frames_received == 1U ||
            (ctx->audio_frames_received % 20U) == 0U) {
            printf("[RN-M4] Ogg Opus page queued: count=%u packet=%u\n",
                   ctx->audio_frames_received, ctx->audio_fragment_len);
        }
    } else {
        ctx->audio_frames_dropped++;
        printf("[RN-M4] Ogg Opus page dropped: packet=%u\n",
               ctx->audio_fragment_len);
    }
    ctx->audio_fragment_len = 0U;
}

static int rainnexus_m4_audio_service(rainnexus_m2_context_t *ctx)
{
    u32 now;

    if (ctx == NULL) {
        return -1;
    }
    if (ctx->audio_active && !ctx->audio_stream_eof &&
        ctx->audio_frames_received != 0U) {
        now = timer_get_ms();
        if ((u32)(now - ctx->audio_last_packet_ms) >=
            RAINNEXUS_M4_STREAM_COMPLETE_IDLE_MS) {
            rainnexus_m4_audio_finish_stream(ctx, "idle timeout");
        }
    }
    if (ctx->audio_decoder_open_requested && ctx->audio_decoder == NULL) {
        ctx->audio_decoder_open_requested = 0;
        if (rainnexus_m4_audio_decoder_start(ctx) != 0) {
            ctx->audio_start_failed = 1;
            ctx->audio_decode_errors++;
            ctx->audio_active = 0;
            return -1;
        }
    }
    if (ctx->audio_start_failed) {
        return -1;
    }
    return 0;
}

static int rainnexus_m5_audio_pcm_vfs_fwrite(void *file, void *data,
                                              u32 len)
{
    rainnexus_m2_context_t *ctx = g_m2_context;
    u32 written;

    (void)file;
    if (ctx == NULL || data == NULL || len == 0U ||
        !ctx->pcm_encoder_active) {
        return 0;
    }
    written = cbuf_write(&ctx->pcm_ring, data, len);
    if (written != len) {
        ctx->uplink_frames_dropped++;
        printf("[RN-M5] PCM ring full: written=%u expected=%u\n",
               (unsigned int)written, (unsigned int)len);
        return -1;
    }
    if (ctx->pcm_sem_initialized) {
        os_sem_post(&ctx->pcm_data_sem);
    }
    return (int)len;
}

static int rainnexus_m5_audio_opus_vfs_fwrite(void *file, void *data,
                                               u32 len)
{
    rainnexus_m2_context_t *ctx = g_m2_context;
    unsigned int index;

    (void)file;
    if (ctx == NULL || data == NULL || len == 0U) {
        return 0;
    }
    ctx->uplink_frames_encoded++;
    if (!ctx->opus_encoder_active ||
        !ctx->uplink_mutex_initialized ||
        len > RAINNEXUS_M5_AUDIO_PACKET_CAPACITY ||
        ctx->uplink_packet_storage == NULL) {
        ctx->uplink_frames_dropped++;
        return (int)len;
    }

    /* The encoder callback must never wait on TLS or a full network queue. */
    if (os_mutex_accept(&ctx->uplink_mutex) != 0) {
        ctx->uplink_frames_dropped++;
        return (int)len;
    }
    if (ctx->uplink_queue_count >= RAINNEXUS_M5_AUDIO_QUEUE_DEPTH) {
        ctx->uplink_frames_dropped++;
        os_mutex_post(&ctx->uplink_mutex);
        return (int)len;
    }
    index = ctx->uplink_queue_tail;
    memcpy(ctx->uplink_packet_storage +
           index * RAINNEXUS_M5_AUDIO_PACKET_CAPACITY, data, len);
    ctx->uplink_packet_lengths[index] = (u16)len;
    ctx->uplink_queue_tail =
        (index + 1U) % RAINNEXUS_M5_AUDIO_QUEUE_DEPTH;
    ctx->uplink_queue_count++;
    os_mutex_post(&ctx->uplink_mutex);
    return (int)len;
}

static int rainnexus_m5_audio_uplink_vfs_fclose(void *file)
{
    (void)file;
    return 0;
}

/*
 * Keep these in writable storage and populate them immediately before each
 * AUDIO_ENC_OPEN.  The audio-server asserts if the callback table it receives
 * has a NULL fwrite member; runtime setup also makes that contract visible in
 * the serial trace.
 */
static struct audio_vfs_ops rainnexus_m5_audio_pcm_vfs_ops;
static struct audio_vfs_ops rainnexus_m5_audio_opus_vfs_ops;

static void rainnexus_m5_audio_vfs_prepare(void)
{
    memset(&rainnexus_m5_audio_pcm_vfs_ops, 0,
           sizeof(rainnexus_m5_audio_pcm_vfs_ops));
    rainnexus_m5_audio_pcm_vfs_ops.fwrite =
        rainnexus_m5_audio_pcm_vfs_fwrite;
    rainnexus_m5_audio_pcm_vfs_ops.fclose =
        rainnexus_m5_audio_uplink_vfs_fclose;

    memset(&rainnexus_m5_audio_opus_vfs_ops, 0,
           sizeof(rainnexus_m5_audio_opus_vfs_ops));
    rainnexus_m5_audio_opus_vfs_ops.fwrite =
        rainnexus_m5_audio_opus_vfs_fwrite;
    rainnexus_m5_audio_opus_vfs_ops.fclose =
        rainnexus_m5_audio_uplink_vfs_fclose;
}

static u32 rainnexus_m5_audio_opus_read_input(u8 *buf, u32 len)
{
    rainnexus_m2_context_t *ctx = g_m2_context;
    u32 read_len;

    if (ctx == NULL || buf == NULL || len == 0U) {
        return (u32)-1;
    }
    for (;;) {
        read_len = cbuf_read(&ctx->pcm_ring, buf, len);
        if (read_len == len) {
            return read_len;
        }
        if (!ctx->opus_encoder_active) {
            return (u32)-1;
        }
        if (ctx->pcm_sem_initialized) {
            os_sem_pend(&ctx->pcm_data_sem, 100);
        } else {
            os_time_dly(1);
        }
    }
}

static void rainnexus_m5_audio_encoder_event(void *priv, int argc, int *argv)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)priv;

    if (ctx == NULL || argc < 1 || argv == NULL) {
        return;
    }
    if (argv[0] == AUDIO_SERVER_EVENT_ERR) {
        ctx->uplink_encoder_errors++;
        printf("[RN-M5] audio encoder event=ERR\n");
    } else if (argv[0] == AUDIO_SERVER_EVENT_END) {
        printf("[RN-M5] audio encoder event=END\n");
    }
}

static int rainnexus_m5_uplink_init(rainnexus_m2_context_t *ctx)
{
    size_t storage_size;

    if (ctx == NULL || RAINNEXUS_M5_AUDIO_QUEUE_DEPTH == 0U ||
        RAINNEXUS_M5_AUDIO_PACKET_CAPACITY == 0U) {
        return -1;
    }
    if (os_mutex_create(&ctx->uplink_mutex) != 0) {
        printf("[RN-M5] uplink mutex create failed\n");
        return -1;
    }
    ctx->uplink_mutex_initialized = 1;
    ctx->pcm_ring_storage = (u8 *)malloc(RAINNEXUS_M5_PCM_RING_CAPACITY);
    if (ctx->pcm_ring_storage == NULL ||
        os_sem_create(&ctx->pcm_data_sem, 0) != 0) {
        printf("[RN-M5] PCM ring initialization failed\n");
        goto fail;
    }
    ctx->pcm_sem_initialized = 1;
    cbuf_init(&ctx->pcm_ring, ctx->pcm_ring_storage,
              RAINNEXUS_M5_PCM_RING_CAPACITY);
    cbuf_clear(&ctx->pcm_ring);
    storage_size = (size_t)RAINNEXUS_M5_AUDIO_QUEUE_DEPTH *
                   RAINNEXUS_M5_AUDIO_PACKET_CAPACITY;
    ctx->uplink_packet_storage = (u8 *)malloc(storage_size);
    ctx->uplink_packet_lengths = (u16 *)malloc(
        sizeof(u16) * RAINNEXUS_M5_AUDIO_QUEUE_DEPTH);
    ctx->uplink_tx_buffer =
        (u8 *)malloc(RAINNEXUS_M5_AUDIO_PACKET_CAPACITY);
    if (ctx->uplink_packet_storage == NULL ||
        ctx->uplink_packet_lengths == NULL || ctx->uplink_tx_buffer == NULL) {
        printf("[RN-M5] uplink queue allocation failed\n");
        goto fail;
    }
    memset(ctx->uplink_packet_lengths, 0,
           sizeof(u16) * RAINNEXUS_M5_AUDIO_QUEUE_DEPTH);
    ctx->uplink_queue_head = 0U;
    ctx->uplink_queue_tail = 0U;
    ctx->uplink_queue_count = 0U;
    return 0;

fail:
    if (ctx->pcm_sem_initialized) {
        os_sem_del(&ctx->pcm_data_sem, OS_DEL_ALWAYS);
        ctx->pcm_sem_initialized = 0;
    }
    free(ctx->pcm_ring_storage);
    ctx->pcm_ring_storage = NULL;
    free(ctx->uplink_packet_storage);
    free(ctx->uplink_packet_lengths);
    free(ctx->uplink_tx_buffer);
    ctx->uplink_packet_storage = NULL;
    ctx->uplink_packet_lengths = NULL;
    ctx->uplink_tx_buffer = NULL;
    if (ctx->uplink_mutex_initialized) {
        os_mutex_del(&ctx->uplink_mutex, OS_DEL_ALWAYS);
        ctx->uplink_mutex_initialized = 0;
    }
    return -1;
}

static void rainnexus_m5_uplink_deinit(rainnexus_m2_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->uplink_encoder_active = 0;
    ctx->pcm_encoder_active = 0;
    ctx->opus_encoder_active = 0;
    if (ctx->pcm_sem_initialized) {
        os_sem_post(&ctx->pcm_data_sem);
    }
    if (ctx->audio_mic_encoder != NULL || ctx->audio_opus_encoder != NULL) {
        printf("[RN-M5] warning: encoder handles remain during uplink deinit\n");
    }
    if (ctx->pcm_sem_initialized) {
        os_sem_del(&ctx->pcm_data_sem, OS_DEL_ALWAYS);
        ctx->pcm_sem_initialized = 0;
    }
    free(ctx->pcm_ring_storage);
    ctx->pcm_ring_storage = NULL;
    free(ctx->uplink_packet_storage);
    free(ctx->uplink_packet_lengths);
    free(ctx->uplink_tx_buffer);
    ctx->uplink_packet_storage = NULL;
    ctx->uplink_packet_lengths = NULL;
    ctx->uplink_tx_buffer = NULL;
    if (ctx->uplink_mutex_initialized) {
        os_mutex_del(&ctx->uplink_mutex, OS_DEL_ALWAYS);
        ctx->uplink_mutex_initialized = 0;
    }
    ctx->uplink_queue_head = 0U;
    ctx->uplink_queue_tail = 0U;
    ctx->uplink_queue_count = 0U;
}

static int rainnexus_m5_audio_encoder_start(rainnexus_m2_context_t *ctx)
{
    union audio_req mic_req = {0};
    union audio_req opus_req = {0};
    int err;

    if (ctx == NULL || ctx->audio_mic_encoder != NULL ||
        ctx->audio_opus_encoder != NULL ||
        !ctx->uplink_mutex_initialized) {
        return -1;
    }
    rainnexus_m5_audio_vfs_prepare();
    printf("[RN-M5] virtual Opus VFS: ops=%x fwrite=%x close=%x\n",
           (unsigned int)&rainnexus_m5_audio_opus_vfs_ops,
           (unsigned int)rainnexus_m5_audio_opus_vfs_ops.fwrite,
           (unsigned int)rainnexus_m5_audio_opus_vfs_ops.fclose);
    ctx->audio_opus_encoder = server_open("audio_server", "enc");
    if (ctx->audio_opus_encoder == NULL) {
        printf("[RN-M5] Opus encoder server open failed\n");
        return -1;
    }
    server_register_event_handler_to_task(ctx->audio_opus_encoder, ctx,
                                          rainnexus_m5_audio_encoder_event,
                                          "app_core");
    opus_req.enc.cmd = AUDIO_ENC_OPEN;
    opus_req.enc.channel = 1;
    opus_req.enc.sample_rate = RAINNEXUS_M5_SAMPLE_RATE;
    opus_req.enc.format = "opus";
    opus_req.enc.no_header = 1;
    opus_req.enc.bitrate = RAINNEXUS_M5_AUDIO_BITRATE;
    opus_req.enc.sample_source = "virtual";
    opus_req.enc.read_input = rainnexus_m5_audio_opus_read_input;
    opus_req.enc.vfs_ops = &rainnexus_m5_audio_opus_vfs_ops;
    opus_req.enc.frame_size = RAINNEXUS_M5_OPUS_FRAME_BYTES;
    opus_req.enc.output_buf_len = 8U * 1024U;
    opus_req.enc.vir_data_wait = 1;
    ctx->opus_encoder_active = 1;
    err = server_request(ctx->audio_opus_encoder, AUDIO_REQ_ENC, &opus_req);
    if (err != 0) {
        printf("[RN-M5] virtual Opus AUDIO_ENC_OPEN failed: %d\n", err);
        ctx->opus_encoder_active = 0;
        server_close(ctx->audio_opus_encoder);
        ctx->audio_opus_encoder = NULL;
        return -1;
    }
    ctx->audio_mic_encoder = server_open("audio_server", "enc");
    if (ctx->audio_mic_encoder == NULL) {
        printf("[RN-M5] PCM microphone encoder server open failed\n");
        goto fail_opus;
    }
    server_register_event_handler_to_task(ctx->audio_mic_encoder, ctx,
                                          rainnexus_m5_audio_encoder_event,
                                          "app_core");
    mic_req.enc.cmd = AUDIO_ENC_OPEN;
    mic_req.enc.channel = RAINNEXUS_M5_MIC_CHANNEL;
    mic_req.enc.channel_bit_map = BIT(RAINNEXUS_M5_EFFECTIVE_MIC_CHANNEL);
    mic_req.enc.volume = RAINNEXUS_M5_MIC_GAIN;
    mic_req.enc.sample_rate = RAINNEXUS_M5_SAMPLE_RATE;
    mic_req.enc.format = "pcm";
    mic_req.enc.sample_source = "mic";
    mic_req.enc.vfs_ops = &rainnexus_m5_audio_pcm_vfs_ops;
    mic_req.enc.frame_size = RAINNEXUS_M5_PCM_FRAME_BYTES;
    mic_req.enc.output_buf_len = RAINNEXUS_M5_PCM_FRAME_BYTES * 3U;
    ctx->pcm_encoder_active = 1;
    err = server_request(ctx->audio_mic_encoder, AUDIO_REQ_ENC, &mic_req);
    if (err != 0) {
        printf("[RN-M5] PCM microphone AUDIO_ENC_OPEN failed: %d\n", err);
        goto fail_mic;
    }
    printf("[RN-M5] PCM microphone + virtual Opus encoder started: "
           "%u Hz mono %u ms bitrate=%u mic_channel=%u\n",
           RAINNEXUS_M5_SAMPLE_RATE, RAINNEXUS_M5_AUDIO_FRAME_MS,
           RAINNEXUS_M5_AUDIO_BITRATE, RAINNEXUS_M5_EFFECTIVE_MIC_CHANNEL);
    return 0;

fail_mic:
    ctx->pcm_encoder_active = 0;
    if (ctx->audio_mic_encoder != NULL) {
        memset(&mic_req, 0, sizeof(mic_req));
        mic_req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(ctx->audio_mic_encoder, AUDIO_REQ_ENC, &mic_req);
        server_close(ctx->audio_mic_encoder);
        ctx->audio_mic_encoder = NULL;
    }
fail_opus:
    ctx->opus_encoder_active = 0;
    if (ctx->pcm_sem_initialized) {
        os_sem_post(&ctx->pcm_data_sem);
    }
    if (ctx->audio_opus_encoder != NULL) {
        memset(&opus_req, 0, sizeof(opus_req));
        opus_req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(ctx->audio_opus_encoder, AUDIO_REQ_ENC, &opus_req);
        server_close(ctx->audio_opus_encoder);
        ctx->audio_opus_encoder = NULL;
    }
    return -1;
}

static void rainnexus_m5_audio_encoder_stop(rainnexus_m2_context_t *ctx)
{
    union audio_req req = {0};

    if (ctx == NULL) {
        return;
    }
    ctx->pcm_encoder_active = 0;
    if (ctx->audio_mic_encoder != NULL) {
        req.enc.cmd = AUDIO_ENC_STOP;
        server_request(ctx->audio_mic_encoder, AUDIO_REQ_ENC, &req);
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(ctx->audio_mic_encoder, AUDIO_REQ_ENC, &req);
        server_close(ctx->audio_mic_encoder);
        ctx->audio_mic_encoder = NULL;
    }
    ctx->opus_encoder_active = 0;
    if (ctx->pcm_sem_initialized) {
        os_sem_post(&ctx->pcm_data_sem);
    }
    if (ctx->audio_opus_encoder != NULL) {
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_STOP;
        server_request(ctx->audio_opus_encoder, AUDIO_REQ_ENC, &req);
        memset(&req, 0, sizeof(req));
        req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(ctx->audio_opus_encoder, AUDIO_REQ_ENC, &req);
        server_close(ctx->audio_opus_encoder);
        ctx->audio_opus_encoder = NULL;
    }
    printf("[RN-M5] encoders stopped: encoded=%u dropped=%u errors=%u\n",
           ctx->uplink_frames_encoded, ctx->uplink_frames_dropped,
           ctx->uplink_encoder_errors);
}

static int rainnexus_m5_uplink_pop(rainnexus_m2_context_t *ctx,
                                   unsigned int *packet_len)
{
    unsigned int index;
    unsigned int len;

    if (ctx == NULL || packet_len == NULL ||
        !ctx->uplink_mutex_initialized || ctx->uplink_tx_buffer == NULL) {
        return -1;
    }
    if (os_mutex_pend(&ctx->uplink_mutex, 0) != 0) {
        return -1;
    }
    if (ctx->uplink_queue_count == 0U) {
        os_mutex_post(&ctx->uplink_mutex);
        return 0;
    }
    index = ctx->uplink_queue_head;
    len = ctx->uplink_packet_lengths[index];
    memcpy(ctx->uplink_tx_buffer + 0U,
           ctx->uplink_packet_storage +
           index * RAINNEXUS_M5_AUDIO_PACKET_CAPACITY, len);
    ctx->uplink_queue_head =
        (index + 1U) % RAINNEXUS_M5_AUDIO_QUEUE_DEPTH;
    ctx->uplink_queue_count--;
    os_mutex_post(&ctx->uplink_mutex);
    *packet_len = len;
    return 1;
}

static int rainnexus_m5_uplink_send_pending(rainnexus_m2_context_t *ctx)
{
    unsigned int packet_len;
    int pop_ret;
    int ret;

    if (ctx == NULL || ctx->ws == NULL) {
        return -1;
    }
    for (;;) {
        pop_ret = rainnexus_m5_uplink_pop(ctx, &packet_len);
        if (pop_ret < 0) {
            return -1;
        }
        if (pop_ret == 0) {
            return 0;
        }
        ret = ctx->ws->_send(ctx->ws, ctx->uplink_tx_buffer, packet_len,
                             WS_TYPE_BINDATA);
        if (ret <= 0) {
            ctx->uplink_send_errors++;
            printf("[RN-M5] Opus uplink send failed: %d\n", ret);
            return -1;
        }
        ctx->uplink_frames_sent++;
        if (ctx->uplink_frames_sent == 1U ||
            (ctx->uplink_frames_sent % 20U) == 0U) {
            printf("[RN-M5] TX Opus: count=%u bytes=%u queue=%u\n",
                   ctx->uplink_frames_sent, packet_len,
                   ctx->uplink_queue_count);
        }
    }
}

static int rainnexus_m5_send_listen_state(rainnexus_m2_context_t *ctx,
                                           const char *state)
{
    char request[256];
    int request_len;
    int ret;

    if (ctx == NULL || ctx->ws == NULL || ctx->session_id[0] == '\0' ||
        state == NULL) {
        return -1;
    }
    if (strcmp(state, "start") == 0) {
        request_len = snprintf(request, sizeof(request),
                               "{\"session_id\":\"%s\","
                               "\"type\":\"listen\",\"state\":"
                               "\"start\",\"mode\":\"manual\"}",
                               ctx->session_id);
    } else {
        request_len = snprintf(request, sizeof(request),
                               "{\"session_id\":\"%s\","
                               "\"type\":\"listen\",\"state\":"
                               "\"stop\"}", ctx->session_id);
    }
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
        return -1;
    }
    ret = ctx->ws->_send(ctx->ws, (u8 *)request, (u32)request_len,
                         WS_TYPE_TXTDATA);
    if (ret <= 0) {
        printf("[RN-M5] listen/%s send failed: %d\n", state, ret);
        return -1;
    }
    printf("[RN-M5] TX JSON (%u bytes): %s\n",
           (unsigned int)request_len, request);
    return 0;
}

static int rainnexus_m5_run_session(rainnexus_m2_context_t *ctx)
{
    int listen_started = 0;
    int encoder_started = 0;
    int ret = RAINNEXUS_M2_OK;

    if (rainnexus_m5_uplink_init(ctx) != 0) {
        return RAINNEXUS_M2_ERR_NO_MEM;
    }
    ctx->recording_start_requested = 0;
    ctx->recording_stop_requested = 0;
    ctx->recording_active = 0;
    ctx->recording_starting = 0;
    ctx->key_poll_initialized = 0;
    ctx->key_poll_last_pressed = 0;
    ctx->key_poll_reported = 0;
    ctx->key_poll_stable_count = 0U;
    printf("[RN-M5] polling KEY_OK on IO_PORTC_06\n");
    while (!ctx->disconnected && !ctx->token_expired &&
           !ctx->audio_start_failed) {
        rainnexus_m5_poll_key(ctx);
        if (rainnexus_m4_audio_service(ctx) != 0 ||
            rainnexus_m5_uplink_send_pending(ctx) != 0) {
            ret = RAINNEXUS_M2_ERR_RECV_TASK;
            break;
        }
        if (!listen_started && ctx->recording_start_requested) {
            ctx->recording_start_requested = 0;
            ctx->recording_starting = 1;

            /*
             * Keep M5 diagnostics scoped to one manual recording round.
             * The encoder is stopped and the uplink queue drained before a
             * new round can reach here, so no encoder callback can update
             * these counters while they are reset.
             */
            ctx->uplink_frames_encoded = 0U;
            ctx->uplink_frames_sent = 0U;
            ctx->uplink_frames_dropped = 0U;
            ctx->uplink_send_errors = 0U;
            ctx->uplink_encoder_errors = 0U;
            if (rainnexus_m4_audio_prepare_next_reply(ctx) != 0) {
                ctx->recording_starting = 0;
                ret = RAINNEXUS_M2_ERR_RECV_TASK;
                break;
            }
            if (rainnexus_m5_send_listen_state(ctx, "start") != 0) {
                ctx->recording_starting = 0;
                ret = RAINNEXUS_M2_ERR_SEND_HELLO;
                break;
            }
            listen_started = 1;
            if (rainnexus_m5_audio_encoder_start(ctx) != 0) {
                ctx->recording_starting = 0;
                ret = RAINNEXUS_M2_ERR_RECV_TASK;
                break;
            }
            encoder_started = 1;
            ctx->recording_active = 1;
            ctx->recording_starting = 0;
            printf("[RN-M5] capture started by KEY_OK\n");
        }
        if (listen_started && ctx->recording_stop_requested) {
            ctx->recording_stop_requested = 0;
            ctx->recording_active = 0;
            printf("[RN-M5] stopping capture by KEY_OK\n");
            if (encoder_started) {
                rainnexus_m5_audio_encoder_stop(ctx);
                encoder_started = 0;
            }
            /* Drain all encoded frames before telling the server to finalize. */
            while (!ctx->disconnected &&
                   rainnexus_m5_uplink_send_pending(ctx) == 0) {
                unsigned int pending;

                if (os_mutex_pend(&ctx->uplink_mutex, 0) != 0) {
                    ret = RAINNEXUS_M2_ERR_RECV_TASK;
                    break;
                }
                pending = ctx->uplink_queue_count;
                os_mutex_post(&ctx->uplink_mutex);
                if (pending == 0U) {
                    break;
                }
                os_time_dly(1);
            }
            if (ret != RAINNEXUS_M2_OK || ctx->disconnected) {
                break;
            }
            if (rainnexus_m5_send_listen_state(ctx, "stop") != 0) {
                ret = RAINNEXUS_M2_ERR_SEND_HELLO;
                break;
            }
            listen_started = 0;
            printf("[RN-M5] capture stopped: encoded=%u sent=%u dropped=%u "
                   "send_errors=%u; ready for next KEY_OK recording\n",
                   ctx->uplink_frames_encoded, ctx->uplink_frames_sent,
                   ctx->uplink_frames_dropped, ctx->uplink_send_errors);
        }
        os_time_dly(1);
    }
    if (ctx->token_expired) {
        ret = RAINNEXUS_M2_ERR_TOKEN_EXPIRED;
    } else if (ctx->disconnected) {
        ret = RAINNEXUS_M2_ERR_DISCONNECTED;
    }

    ctx->recording_active = 0;
    ctx->recording_starting = 0;
    if (encoder_started) {
        rainnexus_m5_audio_encoder_stop(ctx);
    }
    if (listen_started && !ctx->disconnected) {
        rainnexus_m5_send_listen_state(ctx, "stop");
    }
    rainnexus_m5_uplink_deinit(ctx);
    return ret;
}

static int rainnexus_m2_ascii_lower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (int)(c - 'A' + 'a');
    }
    return (int)c;
}

static int rainnexus_m2_ascii_equal(const char *left, const char *right,
                                    size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (rainnexus_m2_ascii_lower((unsigned char)left[i]) !=
            rainnexus_m2_ascii_lower((unsigned char)right[i])) {
            return 0;
        }
    }
    return 1;
}

static const char *rainnexus_m2_header_value(const char *response,
                                             size_t response_len,
                                             const char *name,
                                             size_t *value_len)
{
    const char *line = response;
    const char *end = response + response_len;
    size_t name_len = strlen(name);

    while (line < end) {
        const char *line_end = line;
        const char *value;

        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }
        if ((size_t)(line_end - line) > name_len &&
            rainnexus_m2_ascii_equal(line, name, name_len) &&
            line[name_len] == ':') {
            value = line + name_len + 1U;
            while (value < line_end && (*value == ' ' || *value == '\t')) {
                value++;
            }
            while (line_end > value &&
                   (line_end[-1] == ' ' || line_end[-1] == '\t')) {
                line_end--;
            }
            *value_len = (size_t)(line_end - value);
            return value;
        }
        while (line_end < end && (*line_end == '\r' || *line_end == '\n')) {
            line_end++;
        }
        line = line_end;
    }
    *value_len = 0U;
    return NULL;
}

static int rainnexus_m2_make_ws_key(struct websocket_struct *ws,
                                    char *accept_key,
                                    size_t accept_key_size)
{
    static const char base64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char guid[] =
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char nonce[16];
    unsigned char digest[20];
    char source[64];
    SHA1Context sha1;
    unsigned int i;
    unsigned int out = 0U;

    if (accept_key_size < 29U) {
        return 0;
    }
    for (i = 0U; i < sizeof(nonce); i++) {
        nonce[i] = (unsigned char)(rand() & 0xff);
    }
    for (i = 0U; i < sizeof(nonce); i += 3U) {
        unsigned int block = (unsigned int)nonce[i] << 16;

        if (i + 1U < sizeof(nonce)) {
            block |= (unsigned int)nonce[i + 1U] << 8;
        }
        if (i + 2U < sizeof(nonce)) {
            block |= nonce[i + 2U];
        }
        ws->key[out++] = (u8)base64[(block >> 18) & 0x3fU];
        ws->key[out++] = (u8)base64[(block >> 12) & 0x3fU];
        ws->key[out++] = (u8)(i + 1U < sizeof(nonce) ?
                              base64[(block >> 6) & 0x3fU] : '=');
        ws->key[out++] = (u8)(i + 2U < sizeof(nonce) ?
                              base64[block & 0x3fU] : '=');
    }
    ws->key[out] = '\0';
    ws->key_len = (u8)out;

    snprintf(source, sizeof(source), "%s%s", (char *)ws->key, guid);
    SHA1Reset(&sha1);
    SHA1Input(&sha1, source, (unsigned int)strlen(source));
    if (SHA1Result(&sha1) == 0) {
        return 0;
    }
    for (i = 0U; i < 5U; i++) {
        digest[i * 4U] = (unsigned char)(sha1.Message_Digest[i] >> 24);
        digest[i * 4U + 1U] = (unsigned char)(sha1.Message_Digest[i] >> 16);
        digest[i * 4U + 2U] = (unsigned char)(sha1.Message_Digest[i] >> 8);
        digest[i * 4U + 3U] = (unsigned char)sha1.Message_Digest[i];
    }

    out = 0U;
    for (i = 0U; i < sizeof(digest); i += 3U) {
        unsigned int block = (unsigned int)digest[i] << 16;

        if (i + 1U < sizeof(digest)) {
            block |= (unsigned int)digest[i + 1U] << 8;
        }
        if (i + 2U < sizeof(digest)) {
            block |= digest[i + 2U];
        }
        accept_key[out++] = base64[(block >> 18) & 0x3fU];
        accept_key[out++] = base64[(block >> 12) & 0x3fU];
        accept_key[out++] = (char)(i + 1U < sizeof(digest) ?
                                   base64[(block >> 6) & 0x3fU] : '=');
        accept_key[out++] = (char)(i + 2U < sizeof(digest) ?
                                   base64[block & 0x3fU] : '=');
    }
    accept_key[out] = '\0';
    return 1;
}

static int rainnexus_m2_set_request_path(struct websocket_struct *ws,
                                         const char *url)
{
    const char *scheme_end;
    const char *path;
    size_t path_len;

    scheme_end = strstr(url, "://");
    if (scheme_end == NULL) {
        return 0;
    }
    path = strchr(scheme_end + 3U, '/');
    if (path == NULL) {
        path = "/";
    }
    path_len = strlen(path);
    if (path_len >= sizeof(ws->req_head.file)) {
        return 0;
    }
    memcpy(ws->req_head.file, path, path_len + 1U);
    return 1;
}

static int rainnexus_m4_send_text_probe(rainnexus_m2_context_t *ctx)
{
    char request[256];
    int request_len;
    int ret;

    if (ctx == NULL || ctx->ws == NULL || ctx->session_id[0] == '\0') {
        return -1;
    }
    request_len = snprintf(
        request, sizeof(request),
        "{\"session_id\":\"%s\",\"type\":\"listen\","
        "\"state\":\"detect\",\"text\":\"M4 audio test\"}",
        ctx->session_id);
    if (request_len <= 0 || (size_t)request_len >= sizeof(request)) {
        return -1;
    }
    ret = ctx->ws->_send(ctx->ws, (u8 *)request, (u32)request_len,
                         WS_TYPE_TXTDATA);
    if (ret <= 0) {
        printf("[RN-M4] text probe send failed: %d\n", ret);
        return -1;
    }
    printf("[RN-M4] TX JSON (%u bytes): %s\n",
           (unsigned int)request_len, request);
    return 0;
}

/*
 * The SDK handshake parser compares response header names case-sensitively.
 * Envoy legally lowercases the header names, so keep the SDK transport/TLS
 * pieces and perform the HTTP Upgrade validation here.
 */
static int rainnexus_m2_websocket_handshake(struct websocket_struct *ws)
{
    static const char request_format[] =
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    char request[1400];
    char response[2048];
    char expected_accept[32];
    const char *accept;
    size_t accept_len;
    size_t response_len = 0U;
    size_t request_len;
    int ret;
    int sent;

    ret = websockets_mbtls_client_connect(&ws->websockets_mbtls_info,
                                          ws->req_head.host, ws->port);
    if (ret == FALSE) {
        printf("[RN-M2] WSS TCP connect failed\n");
        return FALSE;
    }
    ret = websockets_mbtls_client_handshack(&ws->websockets_mbtls_info);
    if (ret == FALSE) {
        printf("[RN-M2] WSS TLS handshake failed\n");
        return FALSE;
    }
    if (!rainnexus_m2_make_ws_key(ws, expected_accept,
                                  sizeof(expected_accept))) {
        printf("[RN-M2] WSS handshake key generation failed\n");
        return FALSE;
    }
    request_len = (size_t)snprintf(request, sizeof(request), request_format,
                                   ws->req_head.file, ws->req_head.host,
                                   ws->key);
    if (request_len >= sizeof(request)) {
        printf("[RN-M2] WSS handshake request is too large\n");
        return FALSE;
    }
    sent = ssl_write(ws, &ws->websockets_mbtls_info.ssl, request,
                     (int)request_len);
    if (sent != (int)request_len) {
        printf("[RN-M2] WSS handshake request send failed: %d\n", sent);
        return FALSE;
    }

    while (response_len < sizeof(response) - 1U) {
        ret = ssl_read(ws, &ws->websockets_mbtls_info.ssl,
                       response + response_len,
                       (int)(sizeof(response) - 1U - response_len));
        if (ret <= 0) {
            break;
        }
        response_len += (size_t)ret;
        response[response_len] = '\0';
        if (strstr(response, "\r\n\r\n") != NULL) {
            break;
        }
    }
    response[response_len] = '\0';
    if (strncmp(response, "HTTP/1.1 101 ", 13U) != 0 &&
        strncmp(response, "HTTP/1.0 101 ", 13U) != 0) {
        printf("[RN-M2] WSS Upgrade status invalid\n");
        return FALSE;
    }
    accept = rainnexus_m2_header_value(response, response_len,
                                       "Sec-WebSocket-Accept", &accept_len);
    if (accept == NULL || accept_len != strlen(expected_accept) ||
        memcmp(accept, expected_accept, accept_len) != 0) {
        printf("[RN-M2] WSS Upgrade accept validation failed\n");
        return FALSE;
    }
    ws->websocket_valid = ESTABLISHED;
    return TRUE;
}

static int rainnexus_m2_nonempty_json_string(const cJSON *node)
{
    return cJSON_IsString(node) && node->valuestring != NULL &&
           node->valuestring[0] != '\0';
}

static int rainnexus_m2_opus_sample_rate_valid(const cJSON *sample_rate)
{
    if (!cJSON_IsNumber(sample_rate) ||
        sample_rate->valuedouble != (double)sample_rate->valueint) {
        return 0;
    }
    return sample_rate->valueint == 8000 ||
           sample_rate->valueint == 12000 ||
           sample_rate->valueint == 16000 ||
           sample_rate->valueint == 24000 ||
           sample_rate->valueint == 48000;
}

static int rainnexus_m2_server_hello_valid(const cJSON *root)
{
    const cJSON *type;
    const cJSON *version;
    const cJSON *transport;
    const cJSON *audio_params;
    const cJSON *format;
    const cJSON *sample_rate;
    const cJSON *channels;
    const cJSON *frame_duration;
    const cJSON *session_id;

    if (!cJSON_IsObject(root)) {
        return 0;
    }

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (!rainnexus_m2_nonempty_json_string(type) ||
        strcmp(type->valuestring, "hello") != 0 ||
        !cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !rainnexus_m2_nonempty_json_string(transport) ||
        strcmp(transport->valuestring, "websocket") != 0 ||
        !cJSON_IsObject(audio_params) ||
        !rainnexus_m2_nonempty_json_string(session_id)) {
        return 0;
    }

    format = cJSON_GetObjectItemCaseSensitive(audio_params, "format");
    sample_rate = cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate");
    channels = cJSON_GetObjectItemCaseSensitive(audio_params, "channels");
    frame_duration = cJSON_GetObjectItemCaseSensitive(audio_params,
                                                       "frame_duration");
    return rainnexus_m2_nonempty_json_string(format) &&
           strcmp(format->valuestring, "opus") == 0 &&
           rainnexus_m2_opus_sample_rate_valid(sample_rate) &&
           cJSON_IsNumber(channels) && channels->valuedouble == 1.0 &&
           cJSON_IsNumber(frame_duration) &&
           frame_duration->valuedouble == 60.0;
}

static int rainnexus_m2_string_equal_ci(const char *left, const char *right)
{
    size_t left_len;

    if (left == NULL || right == NULL) {
        return 0;
    }
    left_len = strlen(left);
    return left_len == strlen(right) &&
           rainnexus_m2_ascii_equal(left, right, left_len);
}

static int rainnexus_m2_json_key_is_sensitive(const char *key)
{
    return key != NULL &&
           (rainnexus_m2_string_equal_ci(key, "token") ||
            rainnexus_m2_string_equal_ci(key, "secret") ||
            rainnexus_m2_string_equal_ci(key, "password") ||
            rainnexus_m2_string_equal_ci(key, "authorization"));
}

static int rainnexus_m2_redact_json(cJSON *node)
{
    cJSON *child;

    if (node == NULL || (!cJSON_IsObject(node) && !cJSON_IsArray(node))) {
        return 1;
    }
    for (child = node->child; child != NULL; child = child->next) {
        if (rainnexus_m2_json_key_is_sensitive(child->string) &&
            cJSON_IsString(child)) {
            if (cJSON_SetValuestring(child, "[REDACTED]") == NULL) {
                return 0;
            }
        } else if (!rainnexus_m2_redact_json(child)) {
            return 0;
        }
    }
    return 1;
}

static void rainnexus_m2_log_rx_json(const rainnexus_m2_context_t *ctx,
                                     const cJSON *root)
{
    cJSON *safe_root;
    char *safe_json;

    if (RAINNEXUS_LOG_SERVER_RAW_JSON) {
        printf("[RN-M2] RX JSON RAW (%u bytes): %s\n",
               ctx->rx_json_len, ctx->rx_json);
        return;
    }

    safe_root = cJSON_Duplicate(root, 1);
    if (safe_root == NULL) {
        printf("[RN-M2] RX JSON (%u bytes): log copy unavailable\n",
               ctx->rx_json_len);
        return;
    }
    if (!rainnexus_m2_redact_json(safe_root)) {
        printf("[RN-M2] RX JSON (%u bytes): redaction failed; content omitted\n",
               ctx->rx_json_len);
        cJSON_Delete(safe_root);
        return;
    }
    safe_json = cJSON_PrintUnformatted(safe_root);
    if (safe_json != NULL) {
        printf("[RN-M2] RX JSON (%u bytes, credentials redacted): %s\n",
               ctx->rx_json_len, safe_json);
        cJSON_free(safe_json);
    } else {
        printf("[RN-M2] RX JSON (%u bytes): sanitized log unavailable\n",
               ctx->rx_json_len);
    }
    cJSON_Delete(safe_root);
}

static int rainnexus_m2_error_is_token_expired(const cJSON *root)
{
    const cJSON *code;
    const cJSON *error;
    const cJSON *message;

    code = cJSON_GetObjectItemCaseSensitive(root, "code");
    error = cJSON_GetObjectItemCaseSensitive(root, "error");
    message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsNumber(code) &&
        (code->valueint == 401 || code->valueint == 403 ||
         code->valueint == 419)) {
        return 1;
    }
    if (cJSON_IsString(code) &&
        (rainnexus_m2_string_equal_ci(code->valuestring, "token_expired") ||
         rainnexus_m2_string_equal_ci(code->valuestring, "invalid_token") ||
         rainnexus_m2_string_equal_ci(code->valuestring, "unauthorized"))) {
        return 1;
    }
    if (cJSON_IsString(error) &&
        (rainnexus_m2_string_equal_ci(error->valuestring, "token_expired") ||
         rainnexus_m2_string_equal_ci(error->valuestring, "invalid_token") ||
         rainnexus_m2_string_equal_ci(error->valuestring, "unauthorized"))) {
        return 1;
    }
    return cJSON_IsString(message) &&
           (rainnexus_m2_string_equal_ci(message->valuestring, "token_expired") ||
            rainnexus_m2_string_equal_ci(message->valuestring, "invalid_token"));
}

static void rainnexus_m2_process_json(rainnexus_m2_context_t *ctx)
{
    cJSON *root;
    const cJSON *type;
    const cJSON *session_id;
    const cJSON *tts_state;

    ctx->rx_json[ctx->rx_json_len] = '\0';
    root = cJSON_Parse(ctx->rx_json);
    if (root == NULL) {
        return;
    }
    rainnexus_m2_log_rx_json(ctx, root);

    type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (rainnexus_m2_nonempty_json_string(type) &&
        strcmp(type->valuestring, "hello") == 0) {
        if (rainnexus_m2_server_hello_valid(root)) {
            session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            if (session_id->valuestring == NULL ||
                strlen(session_id->valuestring) >=
                    sizeof(ctx->session_id)) {
                ctx->hello_result = RAINNEXUS_M2_ERR_SERVER_HELLO;
            } else {
                memcpy(ctx->session_id, session_id->valuestring,
                       strlen(session_id->valuestring) + 1U);
                ctx->audio_sample_rate =
                    (unsigned int)cJSON_GetObjectItemCaseSensitive(
                        cJSON_GetObjectItemCaseSensitive(root, "audio_params"),
                        "sample_rate")->valueint;
                ctx->audio_frame_duration = (unsigned int)
                    cJSON_GetObjectItemCaseSensitive(
                        cJSON_GetObjectItemCaseSensitive(root, "audio_params"),
                        "frame_duration")->valueint;
                ctx->audio_frame_samples =
                    ctx->audio_sample_rate * ctx->audio_frame_duration / 1000U;
                ctx->hello_validated = 1;
                ctx->hello_result = RAINNEXUS_M2_OK;
            }
        } else {
            ctx->hello_result = RAINNEXUS_M2_ERR_SERVER_HELLO;
        }
    } else if (ctx->hello_validated) {
        ctx->event_count++;
        if (rainnexus_m2_nonempty_json_string(type) &&
            strcmp(type->valuestring, "error") == 0 &&
            rainnexus_m2_error_is_token_expired(root)) {
            ctx->token_expired = 1;
            ctx->disconnect_reason = RAINNEXUS_M3_DISCONNECT_TOKEN;
        }
        if (rainnexus_m2_nonempty_json_string(type)) {
            printf("[RN-M4] server JSON event received: type=%s\n",
                   type->valuestring);
        } else {
            printf("[RN-M4] server JSON event received: type=unknown\n");
        }
        if (rainnexus_m2_nonempty_json_string(type) &&
            strcmp(type->valuestring, "tts") == 0) {
            tts_state = cJSON_GetObjectItemCaseSensitive(root, "state");
            if (rainnexus_m2_nonempty_json_string(tts_state) &&
                strcmp(tts_state->valuestring, "start") == 0) {
                ctx->audio_tts_start_events++;
                ctx->audio_last_packet_ms = timer_get_ms();
                printf("[RN-M4] TTS started\n");
            } else if (rainnexus_m2_nonempty_json_string(tts_state) &&
                       strcmp(tts_state->valuestring, "sentence_start") == 0) {
                /* Sentence text can arrive before its matching Opus frames. */
                ctx->audio_last_packet_ms = timer_get_ms();
                printf("[RN-M4] TTS sentence started\n");
            } else if (rainnexus_m2_nonempty_json_string(tts_state) &&
                       strcmp(tts_state->valuestring, "stop") == 0) {
                ctx->audio_tts_stop_events++;
                rainnexus_m4_audio_finish_stream(ctx, "server tts/stop");
                printf("[RN-M4] TTS stopped: frames=%u queued=%u dropped=%u "
                       "nominal_ms=%u\n", ctx->audio_frames_received,
                       ctx->audio_frames_queued, ctx->audio_frames_dropped,
                       ctx->audio_frames_received *
                       ctx->audio_frame_duration);
            }
        }
        ctx->rx_json_len = 0U;
    } else {
        printf("[RN-M2] received non-hello JSON; continue waiting\n");
        ctx->rx_json_len = 0;
    }
    if (ctx->hello_validated &&
        rainnexus_m2_nonempty_json_string(type) &&
        strcmp(type->valuestring, "hello") == 0) {
        ctx->rx_json_len = 0;
    }
    cJSON_Delete(root);
}

static void rainnexus_m2_ws_callback(u8 *buf, u32 len, u32 payload_len,
                                     u8 type, u16 seq_enable, u8 end_seq)
{
    rainnexus_m2_context_t *ctx = g_m2_context;
    u8 opcode = type & 0x0fU;
    unsigned int available;

    (void)seq_enable;
    if (ctx == NULL || ctx->disconnected || ctx->token_expired ||
        (!ctx->hello_validated &&
         ctx->hello_result != RAINNEXUS_M2_ERR_HELLO_TIMEOUT)) {
        return;
    }

    if (opcode == WS_TYPE_DISCONN || type == WS_TYPE_CLOSE_OK) {
        ctx->disconnected = 1;
        ctx->disconnect_reason = type == WS_TYPE_CLOSE_OK ?
                                 RAINNEXUS_M3_DISCONNECT_CLOSE :
                                 RAINNEXUS_M3_DISCONNECT_PEER;
        return;
    }
    if (opcode == WS_TYPE_PONG) {
        ctx->pong_count++;
        return;
    }
    if (opcode == WS_TYPE_BINDATA ||
        (opcode == WS_TYPE_SEQ && ctx->audio_binary_fragment_active)) {
        rainnexus_m4_audio_receive(ctx, buf, len, payload_len,
                                   type, opcode, end_seq);
        return;
    }
    if (opcode != WS_TYPE_TXTDATA && opcode != WS_TYPE_SEQ) {
        return;
    }
    if (buf == NULL || len == 0U) {
        return;
    }

    available = RAINNEXUS_M2_RX_JSON_CAPACITY - 1U - ctx->rx_json_len;
    if (len > available) {
        printf("[RN-M4] server JSON event exceeds receive buffer\n");
        if (ctx->hello_validated) {
            ctx->rx_json_len = 0U;
        } else {
            ctx->hello_result = RAINNEXUS_M2_ERR_SERVER_HELLO;
        }
        return;
    }

    memcpy(ctx->rx_json + ctx->rx_json_len, buf, len);
    ctx->rx_json_len += len;
    rainnexus_m2_process_json(ctx);

    if ((payload_len != 0U && ctx->rx_json_len >= payload_len) || end_seq) {
        if (!ctx->hello_validated &&
            ctx->hello_result == RAINNEXUS_M2_ERR_HELLO_TIMEOUT) {
            printf("[RN-M2] RX invalid JSON (%u bytes); content omitted\n",
                   ctx->rx_json_len);
            ctx->hello_result = RAINNEXUS_M2_ERR_SERVER_HELLO;
        } else if (ctx->hello_validated && ctx->rx_json_len != 0U) {
            printf("[RN-M4] RX invalid JSON (%u bytes); content omitted\n",
                   ctx->rx_json_len);
            ctx->rx_json_len = 0U;
        }
    }
}

static int rainnexus_m2_url_char_is_unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

static char rainnexus_m2_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789ABCDEF";

    return digits[value & 0x0fU];
}

static size_t rainnexus_m2_append_encoded(char *output, size_t offset,
                                          const char *value)
{
    size_t i;

    for (i = 0; value[i] != '\0'; i++) {
        unsigned char c = (unsigned char)value[i];

        if (rainnexus_m2_url_char_is_unreserved(c)) {
            output[offset++] = (char)c;
        } else {
            output[offset++] = '%';
            output[offset++] = rainnexus_m2_hex_digit(c >> 4);
            output[offset++] = rainnexus_m2_hex_digit(c);
        }
    }
    return offset;
}

static int rainnexus_m2_mac_valid(const u8 mac[6])
{
    unsigned int i;
    int all_zero = 1;
    int all_ff = 1;

    for (i = 0; i < 6U; i++) {
        if (mac[i] != 0U) {
            all_zero = 0;
        }
        if (mac[i] != 0xffU) {
            all_ff = 0;
        }
    }
    return !all_zero && !all_ff;
}

static char *rainnexus_m2_build_url(const char *base_url, const char *token,
                                    const char *device_id,
                                    const char *client_id)
{
    static const char device_key[] = "device-id=";
    static const char client_key[] = "&client-id=";
    static const char codec_and_token_key[] = "&codec=opus&token=";
    size_t base_len;
    size_t token_len;
    size_t device_id_len;
    size_t client_id_len;
    size_t capacity;
    size_t out;
    char separator;
    char *url;

    if (base_url == NULL || base_url[0] == '\0' ||
        token == NULL || token[0] == '\0' ||
        device_id == NULL || device_id[0] == '\0' ||
        client_id == NULL || client_id[0] == '\0') {
        return NULL;
    }
    base_len = strlen(base_url);
    token_len = strlen(token);
    device_id_len = strlen(device_id);
    client_id_len = strlen(client_id);
    if (token_len > (SIZE_MAX - base_len) / 3U ||
        device_id_len > (SIZE_MAX - base_len - token_len * 3U) / 3U ||
        client_id_len > (SIZE_MAX - base_len - token_len * 3U -
                         device_id_len * 3U) / 3U) {
        return NULL;
    }
    capacity = base_len + 1U + sizeof(device_key) - 1U +
               device_id_len * 3U + sizeof(client_key) - 1U +
               client_id_len * 3U + sizeof(codec_and_token_key) - 1U +
               token_len * 3U + 1U;
    url = (char *)malloc(capacity);
    if (url == NULL) {
        return NULL;
    }

    memcpy(url, base_url, base_len);
    out = base_len;
    if (base_len > 0U &&
        (base_url[base_len - 1U] == '?' || base_url[base_len - 1U] == '&')) {
        separator = '\0';
    } else {
        separator = strchr(base_url, '?') == NULL ? '?' : '&';
    }
    if (separator != '\0') {
        url[out++] = separator;
    }
    memcpy(url + out, device_key, sizeof(device_key) - 1U);
    out += sizeof(device_key) - 1U;
    out = rainnexus_m2_append_encoded(url, out, device_id);
    memcpy(url + out, client_key, sizeof(client_key) - 1U);
    out += sizeof(client_key) - 1U;
    out = rainnexus_m2_append_encoded(url, out, client_id);
    memcpy(url + out, codec_and_token_key, sizeof(codec_and_token_key) - 1U);
    out += sizeof(codec_and_token_key) - 1U;
    out = rainnexus_m2_append_encoded(url, out, token);
    url[out] = '\0';
    return url;
}

static void rainnexus_m2_register_ws(struct websocket_struct *ws)
{
    memset(ws, 0, sizeof(*ws));
    /*
     * 当前目标 SDK 的 websocket.a 中，websockets_client_socket_init()
     * 是示例对象里保留的空桩（固定返回 FALSE）。真正完成 URL
     * 解析、缓冲区分配和 TLS/socket 初始化的入口是 websockets1_* 版本。
     */
    ws->_init = websockets1_client_socket_init;
    ws->_exit = websockets_client_socket_exit;
    ws->_handshack = rainnexus_m2_websocket_handshake;
    ws->_send = websockets_client_socket_send;
    ws->_recv_thread = websockets_client_socket_recv_thread;
    ws->_heart_thread = websockets_client_socket_heart_thread;
    ws->_recv_cb = rainnexus_m2_ws_callback;
    ws->websocket_mode = WEBSOCKETS_MODE;
}

static void rainnexus_m2_cleanup(rainnexus_m2_context_t *ctx)
{
    rainnexus_m5_audio_encoder_stop(ctx);
    rainnexus_m5_uplink_deinit(ctx);
    /* Stop accepting frames before waiting for the WebSocket task. */
    ctx->audio_active = 0;
    if (ctx->ws != NULL &&
        (ctx->ws->recv_thread_id != 0 || ctx->ws->ping_thread_id != 0)) {
        /* Invalidate and wake both SDK tasks before waiting for them. */
        ctx->ws->websocket_valid = INVALID_ESTABLISHED;
        if (ctx->ws->websockets_mbtls_info.server_fd.hdl != NULL) {
            mbedtls_net_free(&ctx->ws->websockets_mbtls_info.server_fd);
        }
        if (ctx->ws->ping_thread_id != 0) {
            thread_kill(&ctx->ws->ping_thread_id, KILL_WAIT);
        }
        if (ctx->ws->recv_thread_id != 0) {
            thread_kill(&ctx->ws->recv_thread_id, KILL_WAIT);
        }
    }
    rainnexus_m4_audio_stop(ctx);
    if (ctx->ws_initialized && ctx->ws != NULL) {
        ctx->ws->_exit(ctx->ws);
        ctx->ws_initialized = 0;
    }
    g_m2_context = NULL;
    free(ctx->ws);
    ctx->ws = NULL;
    free(ctx->url);
    ctx->url = NULL;
}

int rainnexus_m2_run(const rainnexus_auth_result_t *auth)
{
    static const char client_hello[] =
        "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":false},"
        "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
        "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
    rainnexus_m2_context_t ctx;
    u8 mac[6] = {0};
    char client_id[18];
    unsigned int waited_ms = 0;
    int init_ret;
    int ret;

    memset(&ctx, 0, sizeof(ctx));
    ctx.hello_result = RAINNEXUS_M2_ERR_HELLO_TIMEOUT;
    if (auth == NULL || auth->token == NULL || auth->token[0] == '\0' ||
        auth->xiaozhi_url == NULL ||
        strncmp(auth->xiaozhi_url, "wss://", 6U) != 0) {
        return RAINNEXUS_M2_ERR_PARAM;
    }

    wifi_get_mac(mac);
    if (!rainnexus_m2_mac_valid(mac)) {
        printf("[RN-M2] cannot build device identity from WiFi MAC\n");
        return RAINNEXUS_M2_ERR_PARAM;
    }
    snprintf(client_id, sizeof(client_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ctx.url = rainnexus_m2_build_url(auth->xiaozhi_url, auth->token,
                                     RAINNEXUS_DEVICE_ID, client_id);
    ctx.ws = (struct websocket_struct *)malloc(sizeof(*ctx.ws));
    if (ctx.url == NULL || ctx.ws == NULL) {
        ret = RAINNEXUS_M2_ERR_NO_MEM;
        goto cleanup;
    }

    printf("[RN-M2] connecting to xiaozhi WSS: device-id=%s client-id=%s "
           "(token hidden)\n", RAINNEXUS_DEVICE_ID, client_id);
    rainnexus_m2_register_ws(ctx.ws);
    /*
     * SDK init 会把输入 URL 的 path 打印到串口。初始化只需要 scheme、
     * host 和 port，因此先使用不含 token 的基础 URL；完成后再恢复完整
     * URL，HTTP Upgrade 仍会使用完整 query。
     */
    ctx.ws->ip_or_url = (u8 *)auth->xiaozhi_url;
    ctx.ws->recv_time_out = RAINNEXUS_M2_RECV_POLL_MS;
    ctx.ws->send_time_out = RAINNEXUS_M2_IO_TIMEOUT_MS;
    ctx.ws->connect_time_out = RAINNEXUS_M2_CONNECT_TIMEOUT_MS;

    if (websockets_struct_check(sizeof(*ctx.ws)) == FALSE) {
        printf("[RN-M2] WSS SDK structure version mismatch\n");
        ret = RAINNEXUS_M2_ERR_WS_INIT;
        goto cleanup;
    }
    /* SDK 示例要求 init 失败时也调用 exit 释放部分初始化资源。 */
    ctx.ws_initialized = 1;
    printf("[RN-M2] WSS init parameters: url_len=%u struct_size=%u "
           "mode=%d free_heap=%u\n",
           (unsigned int)strlen(ctx.url), (unsigned int)sizeof(*ctx.ws),
           (int)ctx.ws->websocket_mode,
           (unsigned int)get_malloc_remain_heap_size());
    init_ret = ctx.ws->_init(ctx.ws);
    if (init_ret == FALSE) {
        printf("[RN-M2] WSS transport init failed: ret=%d\n", init_ret);
        ret = RAINNEXUS_M2_ERR_WS_INIT;
        goto cleanup;
    }
    ctx.ws->ip_or_url = (u8 *)ctx.url;
    if (!rainnexus_m2_set_request_path(ctx.ws, ctx.url)) {
        printf("[RN-M2] WSS request path is invalid\n");
        ret = RAINNEXUS_M2_ERR_WS_HANDSHAKE;
        goto cleanup;
    }
    printf("[RN-M2] WSS transport init success: ret=%d\n", init_ret);
    if (ctx.ws->_handshack(ctx.ws) == FALSE) {
        printf("[RN-M2] WSS handshake failed\n");
        ret = RAINNEXUS_M2_ERR_WS_HANDSHAKE;
        goto cleanup;
    }
    printf("[RN-M2] WSS handshake success\n");

    g_m2_context = &ctx;
    if (thread_fork("rnx_m3_heart", 19, 1024, 0,
                    &ctx.ws->ping_thread_id,
                    ctx.ws->_heart_thread, ctx.ws) != OS_NO_ERR) {
        printf("[RN-M5] failed to create heartbeat task\n");
        ret = RAINNEXUS_M2_ERR_PING_TASK;
        goto cleanup;
    }
    /* JSON parsing and downlink-frame accounting run from this callback task. */
    if (thread_fork("rnx_m2_recv", 18, 4096, 0, &ctx.ws->recv_thread_id,
                    ctx.ws->_recv_thread, ctx.ws) != OS_NO_ERR) {
        printf("[RN-M2] failed to create receive task\n");
        ret = RAINNEXUS_M2_ERR_RECV_TASK;
        goto cleanup;
    }

    ret = ctx.ws->_send(ctx.ws, (u8 *)client_hello,
                        sizeof(client_hello) - 1U, WS_TYPE_TXTDATA);
    if (ret <= 0) {
        printf("[RN-M2] client hello send failed: %d\n", ret);
        ret = RAINNEXUS_M2_ERR_SEND_HELLO;
        goto cleanup;
    }
    printf("[RN-M2] TX JSON (%u bytes): %s\n",
           (unsigned int)(sizeof(client_hello) - 1U), client_hello);
    printf("[RN-M5] client hello sent; uplink and downlink audio enabled\n");

    while (!ctx.hello_validated &&
           ctx.hello_result == RAINNEXUS_M2_ERR_HELLO_TIMEOUT &&
           !ctx.disconnected && waited_ms < RAINNEXUS_M2_HELLO_TIMEOUT_MS) {
        os_time_dly(1);
        waited_ms += 10U;
    }

    if (ctx.disconnected) {
        printf("[RN-M2] disconnected before valid server hello: reason=%d\n",
               ctx.disconnect_reason);
        ret = RAINNEXUS_M2_ERR_DISCONNECTED;
    } else if (ctx.hello_validated && ctx.hello_result == RAINNEXUS_M2_OK) {
        printf("[RN-M2] server hello validated (session_id received, hidden)\n");
        printf("[RN-M5] session established; session_id stored (hidden), "
               "heartbeat active, downlink audio=%u Hz mono %u ms\n",
               ctx.audio_sample_rate, ctx.audio_frame_duration);
        if (rainnexus_m4_audio_start(&ctx) != 0) {
            printf("[RN-M5] downlink audio initialization failed\n");
            ret = RAINNEXUS_M2_ERR_RECV_TASK;
            goto cleanup;
        }
        ret = rainnexus_m5_run_session(&ctx);
        if (ctx.audio_start_failed) {
            printf("[RN-M5] downlink audio decoder failed after first frame\n");
        } else if (ctx.token_expired) {
            printf("[RN-M5] token expiry event received; session stopping\n");
        } else if (ctx.disconnected) {
            printf("[RN-M5] session disconnected: reason=%d events=%u "
                   "audio_frames=%u\n", ctx.disconnect_reason,
                   ctx.event_count, ctx.audio_frames_received);
        } else {
            printf("[RN-M5] session complete: events=%u audio_frames=%u\n",
                   ctx.event_count, ctx.audio_frames_received);
        }
    } else if (ctx.hello_result == RAINNEXUS_M2_ERR_SERVER_HELLO) {
        printf("[RN-M2] server hello validation failed\n");
        ret = RAINNEXUS_M2_ERR_SERVER_HELLO;
    } else {
        printf("[RN-M2] server hello timeout after %u ms\n",
               RAINNEXUS_M2_HELLO_TIMEOUT_MS);
        ret = RAINNEXUS_M2_ERR_HELLO_TIMEOUT;
    }

cleanup:
    rainnexus_m2_cleanup(&ctx);
    return ret;
}

