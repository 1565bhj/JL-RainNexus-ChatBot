/**
 * @file rainnexus_m2.h
 * @brief RainNexus M2/M3 WebSocket 建连、hello 验证和协议会话
 */

#ifndef __RAINNEXUS_M2_H__
#define __RAINNEXUS_M2_H__

#include "rainnexus_auth.h"

typedef enum {
    RAINNEXUS_M2_OK = 0,
    RAINNEXUS_M2_ERR_PARAM = -20,
    RAINNEXUS_M2_ERR_NO_MEM = -21,
    RAINNEXUS_M2_ERR_WS_INIT = -22,
    RAINNEXUS_M2_ERR_WS_HANDSHAKE = -23,
    RAINNEXUS_M2_ERR_RECV_TASK = -24,
    RAINNEXUS_M2_ERR_SEND_HELLO = -25,
    RAINNEXUS_M2_ERR_HELLO_TIMEOUT = -26,
    RAINNEXUS_M2_ERR_SERVER_HELLO = -27,
    RAINNEXUS_M2_ERR_DISCONNECTED = -28,
    RAINNEXUS_M2_ERR_PING_TASK = -29,
    RAINNEXUS_M2_ERR_TOKEN_EXPIRED = -30,
} rainnexus_m2_error_t;

int rainnexus_m2_run(const rainnexus_auth_result_t *auth);

#endif /* __RAINNEXUS_M2_H__ */

