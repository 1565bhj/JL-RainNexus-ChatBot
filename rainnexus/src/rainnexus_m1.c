/**
 * @file rainnexus_m1.c
 * @brief 精灵头项目的 RainNexus M1/M2 分阶段验证任务
 */

#include "system/includes.h"
#include "os/os_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "wifi/wifi_connect.h"
#include "ntp/ntp.h"

#include "rainnexus.h"
#include "rainnexus_auth.h"
#include "rainnexus_config.h"
#include "rainnexus_m2.h"

extern u32 MALLOC_SIZE[];
extern u32 get_malloc_remain_heap_size(void);

static int g_rainnexus_m1_pid;
static int g_rainnexus_m1_started;

static int rainnexus_wifi_ready(void)
{
    return wifi_get_sta_connect_state() == WIFI_STA_NETWORK_STACK_DHCP_SUCC;
}

static void rainnexus_init_cjson_hooks(void)
{
    cJSON_Hooks hooks;

    hooks.malloc_fn = malloc;
    hooks.free_fn = free;
    cJSON_InitHooks(&hooks);
}

static void rainnexus_log_boot_baseline(void)
{
    printf("[RN-M4] platform ready: active=RainNexus stage=%d\n",
           RAINNEXUS_BRINGUP_STAGE);
    printf("[RN-M4] heap baseline: free=%u capacity=%u bytes\n",
           (unsigned int)get_malloc_remain_heap_size(),
           (unsigned int)MALLOC_SIZE);
    printf("[RN-M4] login, WebSocket protocol session, heartbeat and downlink audio enabled\n");
}

static void rainnexus_wait_for_wifi(void)
{
    unsigned int ticks = 0;

    printf("[RN-M1] waiting for existing WiFi/DHCP flow\n");
    while (!rainnexus_wifi_ready()) {
        os_time_dly(10);
        ticks++;
        if (ticks >= RAINNEXUS_WIFI_WAIT_LOG_SEC * 10U) {
            ticks = 0;
            printf("[RN-M1] WiFi/DHCP not ready; continue waiting\n");
        }
    }
    printf("[RN-M1] WiFi DHCP ready\n");
}

static void rainnexus_wait_for_ntp(void)
{
    unsigned int ticks = 0;
    int invalid_epoch_logged = 0;

    printf("[RN-M1] waiting for NTP time sync\n");
    for (;;) {
        time_t now;

        if (!rainnexus_wifi_ready()) {
            printf("[RN-M1] WiFi lost while waiting NTP; wait DHCP again\n");
            rainnexus_wait_for_wifi();
        }
        if (ntp_client_get_time_status() != 0) {
            now = time(NULL);
            if ((long)now >= 1700000000L) {
                printf("[RN-M1] NTP sync OK: epoch=%ld\n", (long)now);
                return;
            }
            if (!invalid_epoch_logged) {
                invalid_epoch_logged = 1;
                printf("[RN-M1] NTP status ready but UTC epoch invalid: %ld\n",
                       (long)now);
            }
        }
        os_time_dly(10);
        ticks++;
        if (ticks >= RAINNEXUS_NTP_WAIT_LOG_SEC * 10U) {
            ticks = 0;
            printf("[RN-M1] NTP not ready; continue waiting\n");
        }
    }
}

static void rainnexus_m1_task(void *priv)
{
    const rainnexus_auth_result_t *auth;
    unsigned int reconnects = 0U;
    int m2_ret;
    int ret;

    (void)priv;
    rainnexus_init_cjson_hooks();
    rainnexus_log_boot_baseline();
    rainnexus_wait_for_wifi();
    rainnexus_wait_for_ntp();

    for (;;) {
        printf("[RN-M1] WiFi and NTP ready; starting login authentication\n");
        ret = rainnexus_auth_login();
        if (ret != RAINNEXUS_AUTH_OK) {
            printf("[RN-M1] auth failed: error=%d\n", ret);
            printf("[RN-M4] skipped because M1 authentication failed\n");
            break;
        }

        auth = rainnexus_auth_get_result();
        printf("[RN-M1] auth success (token stored, not printed)\n");
        printf("[RN-M1] expires_in=%u seconds\n", auth->expires_in);
        printf("[RN-M1] wss_url=%s\n", auth->wss_url);
        printf("[RN-M1] xiaozhi_url=%s\n", auth->xiaozhi_url);
        printf("[RN-M4] M1 complete; starting WSS protocol session\n");
        m2_ret = rainnexus_m2_run(auth);
        if (m2_ret == RAINNEXUS_M2_ERR_TOKEN_EXPIRED &&
            reconnects < RAINNEXUS_M3_MAX_RECONNECTS) {
            reconnects++;
            printf("[RN-M4] token expired; returning to M1 (%u/%u)\n",
                   reconnects, RAINNEXUS_M3_MAX_RECONNECTS);
            rainnexus_auth_clear();
            os_time_dly(100);
            continue;
        }
        if (m2_ret == RAINNEXUS_M2_OK) {
            printf("[RN-M4] bounded downlink audio session complete\n");
        } else if (m2_ret == RAINNEXUS_M2_ERR_TOKEN_EXPIRED) {
            printf("[RN-M4] token expired; reconnect limit reached\n");
        } else {
            printf("[RN-M4] protocol/audio session failed: error=%d\n",
                   m2_ret);
        }
        break;
    }

    printf("[RN-M4] protocol state; microphone/upload remain disabled\n");
    for (;;) {
        os_time_dly(100);
    }
}

int rainnexus_start(void)
{
    int ret;

    if (g_rainnexus_m1_started) {
        return 0;
    }
    /* M2 keeps JSON, TLS and audio-session state on this call chain. */
    ret = thread_fork("rainnexus_m1", 10, 8192, 0,
                      &g_rainnexus_m1_pid, rainnexus_m1_task, NULL);
    if (ret != OS_NO_ERR) {
        printf("[RN-M3] failed to create RainNexus task: %d\n", ret);
        return -1;
    }
    g_rainnexus_m1_started = 1;
    return 0;
}

