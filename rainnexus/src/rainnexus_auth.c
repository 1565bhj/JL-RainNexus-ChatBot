/**
 * @file rainnexus_auth.c
 * @brief RainNexus M1 登录鉴权实现
 */

#include "system/includes.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "rainnexus_auth.h"
#include "rainnexus_config.h"
#include "rainnexus_http.h"

#define RAINNEXUS_STR_EMPTY(s) ((s) == NULL || (s)[0] == '\0')

static rainnexus_auth_result_t g_auth_result;

static char *rainnexus_strdup(const char *source)
{
    size_t length;
    char *copy;

    if (source == NULL) {
        return NULL;
    }
    length = strlen(source);
    copy = (char *)malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, source, length + 1U);
    }
    return copy;
}

void rainnexus_auth_clear(void)
{
    free((void *)g_auth_result.token);
    free((void *)g_auth_result.wss_url);
    free((void *)g_auth_result.xiaozhi_url);
    memset(&g_auth_result, 0, sizeof(g_auth_result));
}

const rainnexus_auth_result_t *rainnexus_auth_get_result(void)
{
    return &g_auth_result;
}

static int rainnexus_is_wss_url(const cJSON *node)
{
    static const char prefix[] = "wss://";

    return cJSON_IsString(node) && node->valuestring != NULL &&
           strncmp(node->valuestring, prefix, sizeof(prefix) - 1U) == 0 &&
           node->valuestring[sizeof(prefix) - 1U] != '\0';
}

static int rainnexus_is_nonempty_string(const cJSON *node)
{
    return cJSON_IsString(node) && !RAINNEXUS_STR_EMPTY(node->valuestring);
}

/* 只输出字段结构和校验状态，不输出 token、URL 或服务端异常值。 */
static void rainnexus_log_invalid_data_fields(const cJSON *token,
                                              const cJSON *expires_in,
                                              const cJSON *wss_url,
                                              const cJSON *xiaozhi_url)
{
    int token_is_string = cJSON_IsString(token);
    int expires_is_number = cJSON_IsNumber(expires_in);
    int wss_is_string = cJSON_IsString(wss_url);
    int xiaozhi_is_string = cJSON_IsString(xiaozhi_url);
    int expires_positive = 0;
    int expires_in_range = 0;
    int expires_is_integer = 0;

    if (expires_is_number) {
        expires_positive = expires_in->valuedouble > 0.0;
        expires_in_range = expires_positive &&
            expires_in->valuedouble <= (double)UINT_MAX;
        expires_is_integer = expires_in_range &&
            expires_in->valuedouble ==
                (double)(unsigned int)expires_in->valuedouble;
    }

    printf("[RN-M1] auth response field diagnostics (values hidden)\n");
    printf("[RN-M1] field token: present=%d string=%d nonempty=%d\n",
           token != NULL, token_is_string,
           token_is_string && !RAINNEXUS_STR_EMPTY(token->valuestring));
    printf("[RN-M1] field expires_in: present=%d number=%d positive=%d range=%d integer=%d\n",
           expires_in != NULL, expires_is_number, expires_positive,
           expires_in_range, expires_is_integer);
    printf("[RN-M1] field wss_url: present=%d string=%d scheme_wss=%d\n",
           wss_url != NULL, wss_is_string, rainnexus_is_wss_url(wss_url));
    printf("[RN-M1] field xiaozhi_url: present=%d string=%d scheme_wss=%d\n",
           xiaozhi_url != NULL, xiaozhi_is_string,
           rainnexus_is_wss_url(xiaozhi_url));
}

int rainnexus_auth_login(void)
{
    rainnexus_http_response_t raw = {-1, 0, NULL};
    cJSON *request_root = NULL;
    cJSON *response_root = NULL;
    cJSON *code = NULL;
    cJSON *data = NULL;
    cJSON *token = NULL;
    cJSON *expires_in = NULL;
    cJSON *wss_url = NULL;
    cJSON *xiaozhi_url = NULL;
    char *payload = NULL;
    char *new_token = NULL;
    char *new_wss_url = NULL;
    char *new_xiaozhi_url = NULL;
    int ret = RAINNEXUS_AUTH_ERR_RESPONSE;

    rainnexus_auth_clear();

    if (RAINNEXUS_STR_EMPTY(RAINNEXUS_APP_KEY) ||
        RAINNEXUS_STR_EMPTY(RAINNEXUS_APP_SECRET)) {
        printf("[RN-M1] credentials not configured\n");
        return RAINNEXUS_AUTH_ERR_PARAM;
    }

    request_root = cJSON_CreateObject();
    if (request_root == NULL ||
        cJSON_AddStringToObject(request_root, "app_key", RAINNEXUS_APP_KEY) == NULL ||
        cJSON_AddStringToObject(request_root, "app_secret", RAINNEXUS_APP_SECRET) == NULL) {
        ret = RAINNEXUS_AUTH_ERR_NO_MEM;
        goto cleanup;
    }

    payload = cJSON_PrintUnformatted(request_root);
    if (payload == NULL) {
        ret = RAINNEXUS_AUTH_ERR_NO_MEM;
        goto cleanup;
    }

    printf("[RN-M1] auth request: POST %s\n", RAINNEXUS_AUTH_URL);
    raw = rainnexus_http_post_json_with_retry(
        RAINNEXUS_AUTH_URL,
        payload,
        RAINNEXUS_AUTH_TIMEOUT_MS,
        RAINNEXUS_AUTH_MAX_RETRIES,
        RAINNEXUS_AUTH_RETRY_BASE_MS);
    if (raw.error != 0 || raw.status_code != 200 || raw.body == NULL) {
        printf("[RN-M1] auth HTTP failed: err=%d status=%d\n",
               raw.error, raw.status_code);
        ret = RAINNEXUS_AUTH_ERR_HTTP;
        goto cleanup;
    }

    response_root = cJSON_Parse(raw.body);
    if (!cJSON_IsObject(response_root)) {
        printf("[RN-M1] auth response is not valid JSON object\n");
        ret = RAINNEXUS_AUTH_ERR_JSON;
        goto cleanup;
    }

    code = cJSON_GetObjectItemCaseSensitive(response_root, "code");
    if (!cJSON_IsNumber(code)) {
        printf("[RN-M1] auth response missing numeric code\n");
        ret = RAINNEXUS_AUTH_ERR_RESPONSE;
        goto cleanup;
    }
    if (code->valueint != 0) {
        printf("[RN-M1] auth rejected: code=%d\n", code->valueint);
        ret = RAINNEXUS_AUTH_ERR_SERVER;
        goto cleanup;
    }

    data = cJSON_GetObjectItemCaseSensitive(response_root, "data");
    if (!cJSON_IsObject(data)) {
        printf("[RN-M1] auth response missing data object\n");
        ret = RAINNEXUS_AUTH_ERR_RESPONSE;
        goto cleanup;
    }

    token = cJSON_GetObjectItemCaseSensitive(data, "token");
    expires_in = cJSON_GetObjectItemCaseSensitive(data, "expires_in");
    wss_url = cJSON_GetObjectItemCaseSensitive(data, "wss_url");
    xiaozhi_url = cJSON_GetObjectItemCaseSensitive(data, "xiaozhi_url");
    if (!rainnexus_is_nonempty_string(token) ||
        !cJSON_IsNumber(expires_in) || expires_in->valuedouble <= 0.0 ||
        expires_in->valuedouble > (double)UINT_MAX ||
        expires_in->valuedouble != (double)(unsigned int)expires_in->valuedouble ||
        !rainnexus_is_nonempty_string(wss_url) ||
        !rainnexus_is_wss_url(xiaozhi_url)) {
        printf("[RN-M1] auth response data fields invalid\n");
        rainnexus_log_invalid_data_fields(token, expires_in, wss_url,
                                          xiaozhi_url);
        ret = RAINNEXUS_AUTH_ERR_RESPONSE;
        goto cleanup;
    }

    /* 部分账号的 wss_url 是非 WSS 基础地址；M2 以 xiaozhi_url 为准。 */
    if (!rainnexus_is_wss_url(wss_url)) {
        printf("[RN-M1] auth warning: wss_url is not a WSS endpoint; "
               "xiaozhi_url will be used\n");
    }

    new_token = rainnexus_strdup(token->valuestring);
    new_wss_url = rainnexus_strdup(wss_url->valuestring);
    new_xiaozhi_url = rainnexus_strdup(xiaozhi_url->valuestring);
    if (new_token == NULL || new_wss_url == NULL || new_xiaozhi_url == NULL) {
        ret = RAINNEXUS_AUTH_ERR_NO_MEM;
        goto cleanup;
    }

    g_auth_result.token = new_token;
    g_auth_result.expires_in = (unsigned int)expires_in->valuedouble;
    g_auth_result.wss_url = new_wss_url;
    g_auth_result.xiaozhi_url = new_xiaozhi_url;
    new_token = NULL;
    new_wss_url = NULL;
    new_xiaozhi_url = NULL;
    ret = RAINNEXUS_AUTH_OK;

cleanup:
    free(new_token);
    free(new_wss_url);
    free(new_xiaozhi_url);
    if (payload != NULL) {
        cJSON_free(payload);
    }
    cJSON_Delete(request_root);
    cJSON_Delete(response_root);
    rainnexus_http_response_deinit(&raw);
    return ret;
}

