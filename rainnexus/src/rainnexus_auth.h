/**
 * @file rainnexus_auth.h
 * @brief RainNexus M1 登录鉴权接口
 */

#ifndef __RAINNEXUS_AUTH_H__
#define __RAINNEXUS_AUTH_H__

typedef enum {
    RAINNEXUS_AUTH_OK = 0,
    RAINNEXUS_AUTH_ERR_PARAM = -2,
    RAINNEXUS_AUTH_ERR_NO_MEM = -3,
    RAINNEXUS_AUTH_ERR_HTTP = -4,
    RAINNEXUS_AUTH_ERR_JSON = -5,
    RAINNEXUS_AUTH_ERR_SERVER = -6,
    RAINNEXUS_AUTH_ERR_RESPONSE = -7,
} rainnexus_auth_error_t;

typedef struct {
    const char *token;
    unsigned int expires_in;
    const char *wss_url;
    const char *xiaozhi_url;
} rainnexus_auth_result_t;

int rainnexus_auth_login(void);
const rainnexus_auth_result_t *rainnexus_auth_get_result(void);
void rainnexus_auth_clear(void);

#endif /* __RAINNEXUS_AUTH_H__ */

