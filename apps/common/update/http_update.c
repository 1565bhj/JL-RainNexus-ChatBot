#include "system/includes.h"
#include "app_config.h"
#include "update/update.h"
#include "fs/fs.h"
#include "update/update_loader_download.h"
#include "update/net_update.h"
#include "json_c/json.h"
#include "json_c/json_tokener.h"

#ifdef CONFIG_NET_ENABLE
#ifndef CONFIG_NO_SDRAM_ENABLE
#include "http/http_cli.h"
#include "http_chunck_stream_api.h"

/* 临时固定 HTTP OTA 地址：下载到 net_update 的系统固件升级通道 */
#define HTTP_OTA_FIXED_URL "http://www.dokidokikirakira.icu:3000/fw/update-ota.ufw"

//#define SD_FILE_OTA_ENABLE //sd卡升级

static char http_res_ota = 0;
static char http_update_status = 0;
static char http_update_task_init = 0;
static uint64_t http_download_percent = 0;
static void *update_fd = NULL;

int http_chunck_stream_client_socket_request_thread(HTTP_CHUNCK_STREAM_INFO *HTTP,
        char *url,
        int (*buf_callbak)(void *http, char *buf, int len, int payloadlen, char fin),
        char *static_buf, int static_buf_size);

int http_is_update(void)
{
    return http_update_status;
}

static int http_ota_is_text_response(char *buf, int len, int payloadlen)
{
    int i;

    if (update_fd || payloadlen >= 4096) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        if (buf[i] == ' ' || buf[i] == '\r' || buf[i] == '\n' || buf[i] == '\t') {
            continue;
        }
        return (buf[i] == '{' || buf[i] == '[' || buf[i] == '<');
    }

    return 0;
}

static int http_data_callback(void *http, char *buf, int len, int payloadlen, char fin)
{
    int ret = 0;
    HTTP_CHUNCK_STREAM_INFO *phttp = http;
    if (!buf || len <= 0) {
        printf("OTA err : %d\n", len);
        goto exit;
    }
    if (http_ota_is_text_response(buf, len, payloadlen)) {
        printf("OTA recv %s %d : \n%s\n", (phttp && phttp->chunked) ? "Chunked" : "Content-Length", len, buf);
        json_object *new_obj = json_tokener_parse(buf);
        if (!new_obj) {
            goto exit;
        }
        json_object *message = json_object_object_get(new_obj, "message");
        if (message) {
            char *str = json_object_get_string(message);
            if (str) {
                printf("message : \n%s\n", str);
            }
        }
        json_object_put(new_obj);
        goto exit;
    }
    if (http_res_ota && !update_fd) {
#ifdef SD_FILE_OTA_ENABLE
        update_fd = fopen(CONFIG_ROOT_PATH CONFIG_UPGRADE_OTA_FILE_NAME_RES, "w+");
#else
        update_fd = net_fopen(CONFIG_UPGRADE_OTA_FILE_NAME_RES, "w+");
#endif
    } else if (!http_res_ota && !update_fd) {
#ifdef SD_FILE_OTA_ENABLE
        update_fd = fopen(CONFIG_ROOT_PATH CONFIG_UPGRADE_OTA_FILE_NAME, "w+");
#else
        update_fd = net_fopen(CONFIG_UPGRADE_OTA_FILE_NAME, "w+");
#endif
    }
    if (!update_fd) {
        printf("OTA open file err\n");
        goto exit;
    }
    http_update_status = true;
#ifdef SD_FILE_OTA_ENABLE
    ret = fwrite(buf, 1, len, update_fd);
#else
    ret = net_fwrite(update_fd, buf, len, 0);
#endif
    if (ret != len) {
        printf("OTA write error : 0x%x\n", ret);
        goto exit;
    }
    http_download_percent += len;
    if (payloadlen > 0) {
        printf("OTA len:%d, upload : %llu, total:%d (%ld%%)\n", len, http_download_percent, payloadlen, http_download_percent * 100 / payloadlen);
    } else {
        printf("OTA len:%d, upload : %llu\n", len, http_download_percent);
    }
    if (fin) {
#ifdef SD_FILE_OTA_ENABLE
        fclose(update_fd);
#else
        net_fclose(update_fd, (char)0);
#endif
        update_fd = NULL;
        http_update_status = false;
        printf("OTA write file OK\n");
    }
    return HTTP_OK;

exit:
    if (update_fd) {
#ifdef SD_FILE_OTA_ENABLE
        fclose(update_fd);
#else
        net_fclose(update_fd, (char) -1);
#endif
        update_fd = NULL;
    }
    http_update_status = false;
    return HTTP_ERR;
}

static int http_ota_task(void *priv)
{
    char *url = malloc(sizeof(HTTP_OTA_FIXED_URL));
    http_update_task_init = true;
    if (url) {
        extern int sys_connect_net_success(void);
        int to = 60 * 60;
        while (!sys_connect_net_success() && --to) {
            os_time_dly(100);
        }
#if (defined TCFG_VBAT_CHECK_EN && TCFG_VBAT_CHECK_EN == 1)
        extern int vbat_check_percent(void);
        int vbat_perc = vbat_check_percent();
        if (vbat_perc < 40) { //电压过低
            os_time_dly(10);
            vbat_perc = vbat_check_percent();
            if (vbat_perc < 40) { //电压过低不升级
                goto exit;
            }
        }
#endif
        if (priv == NULL) {
            os_time_dly(500);//非MQTT触发，5秒钟后升级
        }
        if (to) {
            strcpy(url, HTTP_OTA_FIXED_URL);
            printf("-> HTTP OTA : %s\n", url);
            http_res_ota = 0;
            http_download_percent = 0;
            http_chunck_stream_client_socket_request_thread(NULL, url, http_data_callback, NULL, 0);
        }
        free(url);
    }
exit:
    http_update_task_init = 0;
    return 0;
}

int http_ota_init(void *priv)
{
    priv = priv == 1 ? 1 : NULL;
    if (production_io_is_enter() || is_production_test_enter(0) || system_is_alarm_wakeup() || http_update_task_init) {
        return -1;
    }
    http_update_task_init = true;
    if (thread_fork("http_ota", 10, 2400, 0, 0, http_ota_task, priv)) {
        http_update_task_init = false;
        return -2;
    }
    return 0;
}

#endif
#endif
