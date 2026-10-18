/**
 * @file rainnexus_http.c
 * @brief 基于 SDK httpcli_post 的 RainNexus HTTPS POST 工具
 */

#include "system/includes.h"
#include "os/os_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http/http_cli.h"
#include "rainnexus_http.h"

#define RAINNEXUS_HTTP_BODY_BUF_LEN    (4 * 1024)

static int rainnexus_http_error_retryable(int error)
{
    switch (error) {
    case HERROR_SOCK:
    case HERROR_RECV_TIMEOUT:
    case HERROR_SOCKHDL:
    case HERROR_UNKNOWN:
        return 1;
    default:
        return 0;
    }
}

rainnexus_http_response_t rainnexus_http_post_json_with_retry(
    const char *url,
    const char *json,
    int timeout_ms,
    int max_retries,
    int base_delay_ms)
{
    rainnexus_http_response_t result = {-1, 0, NULL};
    http_body_obj body;
    httpcli_ctx *ctx = NULL;
    int error = HERROR_UNKNOWN;
    int attempt;

    if (url == NULL || url[0] == '\0') {
        return result;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 5000;
    }
    if (max_retries < 0) {
        max_retries = 0;
    }
    if (base_delay_ms <= 0) {
        base_delay_ms = 500;
    }

    ctx = (httpcli_ctx *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return result;
    }

    memset(&body, 0, sizeof(body));
    body.buf_len = RAINNEXUS_HTTP_BODY_BUF_LEN;
    body.buf_count = 1;
    body.p = (char *)malloc(body.buf_len);
    if (body.p == NULL) {
        free(ctx);
        return result;
    }

    for (attempt = 0; attempt <= max_retries; ++attempt) {
        if (attempt > 0) {
            int delay_ms = base_delay_ms << (attempt - 1);

            printf("[RN-M1] HTTP retry %d/%d after %dms\n",
                   attempt, max_retries, delay_ms);
            os_time_dly(delay_ms / 10 > 0 ? delay_ms / 10 : 1);
        }

        memset(ctx, 0, sizeof(*ctx));
        body.recv_len = 0;
        ctx->url = url;
        ctx->timeout_millsec = timeout_ms;
        ctx->priv = &body;
        ctx->connection = "close";
        ctx->data_format = "application/json";
        ctx->post_data = json;
        ctx->data_len = json != NULL ? (int)strlen(json) : 0;

        error = httpcli_post(ctx);
        if (error == HERROR_OK) {
            break;
        }
        printf("[RN-M1] HTTP POST failed: error=%d\n", error);
        if (!rainnexus_http_error_retryable(error)) {
            break;
        }
    }

    if (error == HERROR_OK) {
        result.error = 0;
        result.status_code = 200;
        if (body.recv_len > 0) {
            result.body = (char *)malloc(body.recv_len + 1U);
            if (result.body != NULL) {
                memcpy(result.body, body.p, body.recv_len);
                result.body[body.recv_len] = '\0';
            } else {
                result.error = -1;
            }
        }
    } else {
        result.status_code = error == HERROR_RESPOND ? 400 : 0;
    }

    httpcli_close(ctx);
    free(body.p);
    free(ctx);
    return result;
}

void rainnexus_http_response_deinit(rainnexus_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    free(response->body);
    response->body = NULL;
    response->error = 0;
    response->status_code = 0;
}

