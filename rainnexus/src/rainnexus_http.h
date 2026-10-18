/**
 * @file rainnexus_http.h
 * @brief RainNexus M1 HTTPS POST 工具
 */

#ifndef __RAINNEXUS_HTTP_H__
#define __RAINNEXUS_HTTP_H__

typedef struct {
    int error;
    int status_code;
    char *body;
} rainnexus_http_response_t;

rainnexus_http_response_t rainnexus_http_post_json_with_retry(
    const char *url,
    const char *json,
    int timeout_ms,
    int max_retries,
    int base_delay_ms);

void rainnexus_http_response_deinit(rainnexus_http_response_t *response);

#endif /* __RAINNEXUS_HTTP_H__ */

