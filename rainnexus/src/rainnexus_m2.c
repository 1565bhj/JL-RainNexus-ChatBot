/**
 * @file rainnexus_m2.c
 * @brief RainNexus M2/M4：WSS 建连、会话和下行音频播放
 */

#include "system/includes.h"
#include "os/os_api.h"
#include "web_socket/websocket_api.h"
#include "web_socket/websocket_ssl.h"
#include "wifi/wifi_connect.h"
#include "server/audio_server.h"
#include "server/server_core.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "rainnexus_config.h"
#include "rainnexus_m2.h"

#define RAINNEXUS_M2_RX_JSON_CAPACITY  1024U
#define RAINNEXUS_M3_SESSION_ID_CAPACITY 128U

enum {
    RAINNEXUS_M3_DISCONNECT_NONE = 0,
    RAINNEXUS_M3_DISCONNECT_PEER = 1,
    RAINNEXUS_M3_DISCONNECT_CLOSE = 2,
    RAINNEXUS_M3_DISCONNECT_TOKEN = 3,
};

extern u32 get_malloc_remain_heap_size(void);

typedef struct {
    u8 data[RAINNEXUS_M4_AUDIO_PACKET_CAPACITY];
    u16 len;
    u16 offset;
} rainnexus_m4_audio_packet_t;

typedef struct rainnexus_m2_context rainnexus_m2_context_t;

static int rainnexus_m4_audio_vfs_fread(void *file, void *data, u32 len);
static int rainnexus_m4_audio_decoder_start(rainnexus_m2_context_t *ctx);

static const struct audio_vfs_ops rainnexus_m4_audio_vfs_ops = {
    .fread = rainnexus_m4_audio_vfs_fread,
};

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
    rainnexus_m4_audio_packet_t *audio_packets;
    u8 audio_read_index;
    u8 audio_write_index;
    volatile u8 audio_packet_count;
    volatile int audio_active;
    volatile int audio_start_requested;
    volatile int audio_start_failed;
    OS_SEM audio_data_sem;
    int audio_sem_initialized;
    void *audio_decoder;
    u8 *audio_fragment;
    unsigned int audio_fragment_len;
    int audio_fragment_active;
    unsigned int audio_frames_received;
    unsigned int audio_frames_queued;
    unsigned int audio_frames_dropped;
    unsigned int audio_decode_errors;
    int ws_initialized;
};

/* 目标 SDK 的接收回调没有 priv 参数；M2 只允许存在一个验证连接。 */
static rainnexus_m2_context_t *g_m2_context;

static int rainnexus_m4_audio_enqueue(rainnexus_m2_context_t *ctx,
                                      const u8 *data, unsigned int len)
{
    rainnexus_m4_audio_packet_t *packet;
    u8 next_index;

#if !RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    /* Raw Opus is intentionally sinked until a safe decoder path is verified. */
    if (ctx != NULL && data != NULL && len != 0U) {
        ctx->audio_frames_dropped++;
    }
    return 0;
#endif

    if (ctx == NULL || data == NULL || len == 0U ||
        len > RAINNEXUS_M4_AUDIO_PACKET_CAPACITY ||
        !ctx->audio_active || !ctx->audio_sem_initialized) {
        return 0;
    }
    if (ctx->audio_packet_count >= RAINNEXUS_M4_AUDIO_QUEUE_DEPTH) {
        ctx->audio_frames_dropped++;
        if (ctx->audio_frames_dropped == 1U ||
            (ctx->audio_frames_dropped % 10U) == 0U) {
            printf("[RN-M4] audio queue full; dropped=%u\n",
                   ctx->audio_frames_dropped);
        }
        return 0;
    }

    packet = &ctx->audio_packets[ctx->audio_write_index];
    memcpy(packet->data, data, len);
    packet->len = (u16)len;
    packet->offset = 0U;
    next_index = (u8)(ctx->audio_write_index + 1U);
    if (next_index >= RAINNEXUS_M4_AUDIO_QUEUE_DEPTH) {
        next_index = 0U;
    }
    ctx->audio_write_index = next_index;
    ctx->audio_packet_count++;
    ctx->audio_frames_queued++;
    os_sem_post(&ctx->audio_data_sem);

    /* Let the session task start the decoder outside the WebSocket callback. */
#if RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    if (ctx->audio_decoder == NULL && !ctx->audio_start_failed) {
        ctx->audio_start_requested = 1;
    }
#else
    ctx->audio_frames_dropped++;
#endif
    return 1;
}

static int rainnexus_m4_audio_vfs_fread(void *file, void *data, u32 len)
{
    rainnexus_m2_context_t *ctx = (rainnexus_m2_context_t *)file;
    rainnexus_m4_audio_packet_t *packet;
    u32 available;
    u32 copy_len;

    if (ctx == NULL || data == NULL || len == 0U || !ctx->audio_active ||
        !ctx->audio_sem_initialized) {
        return 0;
    }
    while (ctx->audio_active && ctx->audio_packet_count == 0U) {
        os_sem_pend(&ctx->audio_data_sem, RAINNEXUS_M4_AUDIO_READ_WAIT_TICKS);
    }
    if (!ctx->audio_active || ctx->audio_packet_count == 0U) {
        return 0;
    }
    packet = &ctx->audio_packets[ctx->audio_read_index];
    available = (u32)packet->len - packet->offset;
    copy_len = available < len ? available : len;
    memcpy(data, packet->data + packet->offset, copy_len);
    packet->offset += (u16)copy_len;
    if (packet->offset >= packet->len) {
        packet->len = 0U;
        packet->offset = 0U;
        ctx->audio_read_index++;
        if (ctx->audio_read_index >= RAINNEXUS_M4_AUDIO_QUEUE_DEPTH) {
            ctx->audio_read_index = 0U;
        }
        ctx->audio_packet_count--;
    }
    return (int)copy_len;
}

static int rainnexus_m4_audio_decoder_start(rainnexus_m2_context_t *ctx)
{
    union audio_req req = {0};
    int err;

    if (ctx == NULL || !ctx->audio_active || ctx->audio_decoder != NULL) {
        return -1;
    }
    printf("[RN-M4] opening audio_server decoder\n");
    ctx->audio_decoder = server_open("audio_server", "dec");
    if (ctx->audio_decoder == NULL) {
        printf("[RN-M4] Opus audio server open failed\n");
        return -1;
    }
    printf("[RN-M4] audio_server decoder opened\n");
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.volume = 80;
    req.dec.output_buf_len = 8U * 1024U;
    req.dec.priority = 1;
    req.dec.channel = 1;
    req.dec.sample_rate = ctx->audio_sample_rate;
    req.dec.dec_type = "opus";
    req.dec.sample_source = "dac";
    req.dec.file = (FILE *)ctx;
    req.dec.vfs_ops = &rainnexus_m4_audio_vfs_ops;
    printf("[RN-M4] sending AUDIO_DEC_OPEN: sr=%u channels=1 frame_ms=%u\n",
           ctx->audio_sample_rate, ctx->audio_frame_duration);
    err = server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
    if (err != 0) {
        printf("[RN-M4] Opus audio server open failed: %d\n", err);
        goto fail;
    }
    printf("[RN-M4] AUDIO_DEC_OPEN completed\n");
    req.dec.cmd = AUDIO_DEC_START;
    printf("[RN-M4] sending AUDIO_DEC_START\n");
    err = server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
    if (err != 0) {
        printf("[RN-M4] Opus audio server start failed: %d\n", err);
        goto fail;
    }
    printf("[RN-M4] AUDIO_DEC_START completed\n");
    printf("[RN-M4] downlink playback started: opus %u Hz mono %u ms\n",
           ctx->audio_sample_rate, ctx->audio_frame_duration);
    return 0;

fail:
    ctx->audio_active = 0;
    os_sem_post(&ctx->audio_data_sem);
    if (ctx->audio_decoder != NULL) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
        server_close(ctx->audio_decoder);
        ctx->audio_decoder = NULL;
    }
    return -1;
}

static int rainnexus_m4_audio_start(rainnexus_m2_context_t *ctx)
{
#if RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    int err;
#endif

    if (ctx == NULL || ctx->audio_sample_rate == 0U ||
        ctx->audio_frame_duration != 60U || ctx->audio_frame_samples == 0U) {
        return -1;
    }

#if !RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    ctx->audio_active = 1;
    printf("[RN-M4] audio sink ready; raw Opus decoder disabled for SDK safety\n");
    return 0;
#else
    ctx->audio_packets = (rainnexus_m4_audio_packet_t *)malloc(
        sizeof(rainnexus_m4_audio_packet_t) * RAINNEXUS_M4_AUDIO_QUEUE_DEPTH);
    ctx->audio_fragment = (u8 *)malloc(RAINNEXUS_M4_AUDIO_PACKET_CAPACITY);
    if (ctx->audio_packets == NULL || ctx->audio_fragment == NULL) {
        printf("[RN-M4] audio buffer allocation failed\n");
        goto fail_buffers;
    }
    printf("[RN-M4] audio buffers allocated: heap=%u\n",
           (unsigned int)get_malloc_remain_heap_size());
    memset(ctx->audio_packets, 0,
           sizeof(rainnexus_m4_audio_packet_t) * RAINNEXUS_M4_AUDIO_QUEUE_DEPTH);
    printf("[RN-M4] creating audio queue semaphore\n");
    err = os_sem_create(&ctx->audio_data_sem, 0);
    if (err != 0) {
        printf("[RN-M4] audio queue semaphore create failed: %d\n", err);
        goto fail_buffers;
    }
    ctx->audio_sem_initialized = 1;
    ctx->audio_active = 1;
#if RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    printf("[RN-M4] audio queue semaphore ready; decoder waits for first Opus frame\n");
#else
    printf("[RN-M4] audio queue ready; raw Opus decoder disabled for SDK safety\n");
#endif
    return 0;

fail_buffers:
    free(ctx->audio_packets);
    free(ctx->audio_fragment);
    ctx->audio_packets = NULL;
    ctx->audio_fragment = NULL;
    return -1;
#endif
}

static void rainnexus_m4_audio_stop(rainnexus_m2_context_t *ctx)
{
    union audio_req req = {0};

    if (ctx == NULL) {
        return;
    }
    ctx->audio_active = 0;
    if (!ctx->audio_sem_initialized) {
        return;
    }
    os_sem_post(&ctx->audio_data_sem);
    if (ctx->audio_decoder != NULL) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(ctx->audio_decoder, AUDIO_REQ_DEC, &req);
        server_close(ctx->audio_decoder);
        ctx->audio_decoder = NULL;
    }
    os_sem_del(&ctx->audio_data_sem, OS_DEL_ALWAYS);
    ctx->audio_sem_initialized = 0;
    free(ctx->audio_packets);
    free(ctx->audio_fragment);
    ctx->audio_packets = NULL;
    ctx->audio_fragment = NULL;
    printf("[RN-M4] playback stopped: received=%u queued=%u dropped=%u "
           "decode_errors=%u\n",
           ctx->audio_frames_received, ctx->audio_frames_queued,
           ctx->audio_frames_dropped, ctx->audio_decode_errors);
}

static void rainnexus_m4_audio_receive(rainnexus_m2_context_t *ctx,
                                       u8 *buf, u32 len, u32 payload_len,
                                       u8 frame_type, u8 opcode,
                                       u8 end_seq)
{
    unsigned int remaining;

    if (ctx == NULL || !ctx->hello_validated || !ctx->audio_active ||
        buf == NULL || len == 0U) {
        return;
    }

#if !RAINNEXUS_M4_AUDIO_DECODER_ENABLE
    /* The safe bring-up sink has no audio buffer; never copy into it. */
    if (opcode == WS_TYPE_BINDATA) {
        ctx->audio_frames_received++;
        ctx->audio_frames_dropped++;
        if (ctx->audio_frames_received == 1U ||
            (ctx->audio_frames_received % 20U) == 0U) {
            printf("[RN-M4] binary Opus frame received and dropped: "
                   "count=%u len=%u\n",
                   ctx->audio_frames_received, (unsigned int)len);
        }
    }
    return;
#endif

    if (opcode == WS_TYPE_BINDATA) {
        ctx->audio_fragment_len = 0U;
        ctx->audio_fragment_active = 1;
        if (payload_len > RAINNEXUS_M4_AUDIO_PACKET_CAPACITY) {
            ctx->audio_fragment_active = 0;
            ctx->audio_frames_dropped++;
            printf("[RN-M4] binary Opus frame too large: %u\n",
                   (unsigned int)payload_len);
            return;
        }
    } else if (opcode != WS_TYPE_SEQ || !ctx->audio_fragment_active) {
        return;
    }

    remaining = RAINNEXUS_M4_AUDIO_PACKET_CAPACITY -
                ctx->audio_fragment_len;
    if (len > remaining) {
        ctx->audio_fragment_active = 0;
        ctx->audio_fragment_len = 0U;
        ctx->audio_frames_dropped++;
        printf("[RN-M4] binary Opus fragment overflow: %u\n",
               (unsigned int)len);
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

    ctx->audio_fragment_active = 0;
    ctx->audio_frames_received++;
    if (ctx->audio_frames_received == 1U ||
        (ctx->audio_frames_received % 20U) == 0U) {
        printf("[RN-M4] binary Opus frame received: count=%u len=%u\n",
               ctx->audio_frames_received, ctx->audio_fragment_len);
    }
    rainnexus_m4_audio_enqueue(ctx, ctx->audio_fragment,
                               ctx->audio_fragment_len);
    ctx->audio_fragment_len = 0U;
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
    printf("[RN-M4] text probe sent: bytes=%u\n",
           (unsigned int)request_len);
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

    ctx->rx_json[ctx->rx_json_len] = '\0';
    root = cJSON_Parse(ctx->rx_json);
    if (root == NULL) {
        return;
    }

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
        (opcode == WS_TYPE_SEQ && ctx->audio_fragment_active)) {
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
            printf("[RN-M2] server hello is not valid JSON\n");
            ctx->hello_result = RAINNEXUS_M2_ERR_SERVER_HELLO;
        } else if (ctx->hello_validated && ctx->rx_json_len != 0U) {
            printf("[RN-M4] server JSON event is not valid JSON\n");
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
                                    const char *device_id)
{
    static const char device_key[] = "device-id=";
    static const char client_key[] = "&client-id=";
    static const char codec_and_token_key[] = "&codec=opus&token=";
    size_t base_len;
    size_t token_len;
    size_t device_id_len;
    size_t capacity;
    size_t out;
    char separator;
    char *url;

    if (base_url == NULL || base_url[0] == '\0' ||
        token == NULL || token[0] == '\0' ||
        device_id == NULL || device_id[0] == '\0') {
        return NULL;
    }
    base_len = strlen(base_url);
    token_len = strlen(token);
    device_id_len = strlen(device_id);
    if (token_len > (SIZE_MAX - base_len) / 3U ||
        device_id_len > (SIZE_MAX - base_len - token_len * 3U) / 6U) {
        return NULL;
    }
    capacity = base_len + 1U + sizeof(device_key) - 1U +
               device_id_len * 3U + sizeof(client_key) - 1U +
               device_id_len * 3U + sizeof(codec_and_token_key) - 1U +
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
    out = rainnexus_m2_append_encoded(url, out, device_id);
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
    if (ctx->audio_sem_initialized) {
        /* Stop accepting frames before waiting for the WebSocket task. */
        ctx->audio_active = 0;
        os_sem_post(&ctx->audio_data_sem);
    }
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
        "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
        "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
        "\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}";
    rainnexus_m2_context_t ctx;
    u8 mac[6] = {0};
    char device_id[18];
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
    snprintf(device_id, sizeof(device_id),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ctx.url = rainnexus_m2_build_url(auth->xiaozhi_url, auth->token,
                                     device_id);
    ctx.ws = (struct websocket_struct *)malloc(sizeof(*ctx.ws));
    if (ctx.url == NULL || ctx.ws == NULL) {
        ret = RAINNEXUS_M2_ERR_NO_MEM;
        goto cleanup;
    }

    printf("[RN-M2] connecting to xiaozhi WSS (token hidden)\n");
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
        printf("[RN-M4] failed to create heartbeat task\n");
        ret = RAINNEXUS_M2_ERR_PING_TASK;
        goto cleanup;
    }
    if (thread_fork("rnx_m2_recv", 18, 2048, 0, &ctx.ws->recv_thread_id,
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
    printf("[RN-M4] client hello sent; downlink audio enabled\n");

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
        printf("[RN-M4] session established; session_id stored (hidden), "
               "heartbeat active, downlink audio=%u Hz mono %u ms\n",
               ctx.audio_sample_rate, ctx.audio_frame_duration);
        if (rainnexus_m4_audio_start(&ctx) != 0) {
            printf("[RN-M4] downlink audio initialization failed\n");
            ret = RAINNEXUS_M2_ERR_RECV_TASK;
            goto cleanup;
        }
#if RAINNEXUS_M4_TEXT_PROBE_ENABLE
        if (rainnexus_m4_send_text_probe(&ctx) != 0) {
            ret = RAINNEXUS_M2_ERR_SEND_HELLO;
            goto cleanup;
        }
#endif
        waited_ms = 0U;
        while (!ctx.disconnected && !ctx.token_expired &&
               !ctx.audio_start_failed &&
               waited_ms < RAINNEXUS_M4_SESSION_TIMEOUT_MS) {
            if (ctx.audio_start_requested && ctx.audio_decoder == NULL) {
#if RAINNEXUS_M4_AUDIO_DECODER_ENABLE
                ctx.audio_start_requested = 0;
                if (rainnexus_m4_audio_decoder_start(&ctx) != 0) {
                    ctx.audio_start_failed = 1;
                    ctx.audio_decode_errors++;
                    ctx.audio_active = 0;
                    os_sem_post(&ctx.audio_data_sem);
                }
#else
                ctx.audio_start_requested = 0;
#endif
            }
            os_time_dly(1);
            waited_ms += 10U;
        }
        if (ctx.audio_start_failed) {
            printf("[RN-M4] downlink audio decoder failed after first frame\n");
            ret = RAINNEXUS_M2_ERR_RECV_TASK;
        } else if (ctx.token_expired) {
            printf("[RN-M4] token expiry event received; session stopping\n");
            ret = RAINNEXUS_M2_ERR_TOKEN_EXPIRED;
        } else if (ctx.disconnected) {
            printf("[RN-M4] session disconnected: reason=%d events=%u "
                   "audio_frames=%u\n", ctx.disconnect_reason,
                   ctx.event_count, ctx.audio_frames_received);
            ret = RAINNEXUS_M2_ERR_DISCONNECTED;
        } else {
            printf("[RN-M4] session timeout after %u ms; events=%u "
                   "audio_frames=%u\n", RAINNEXUS_M4_SESSION_TIMEOUT_MS,
                   ctx.event_count, ctx.audio_frames_received);
            ret = RAINNEXUS_M2_OK;
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

