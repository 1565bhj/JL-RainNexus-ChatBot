/**
 * @file rainnexus_config.h
 * @brief RainNexus 平台选择和分阶段接入参数
 */

#ifndef __RAINNEXUS_CONFIG_H__
#define __RAINNEXUS_CONFIG_H__

/* Keep deployment credentials in the ignored local override when available. */
#if defined(__has_include)
#if __has_include("rainnexus_config.local.h")
#include "rainnexus_config.local.h"
#endif
#endif

#define VOICE_PLATFORM_JOYINSIDE       1
#define VOICE_PLATFORM_RAINNEXUS       2

#ifndef VOICE_PLATFORM
#define VOICE_PLATFORM                 VOICE_PLATFORM_RAINNEXUS
#endif

#ifndef RAINNEXUS_BRINGUP_STAGE
#define RAINNEXUS_BRINGUP_STAGE        4
#endif

#if VOICE_PLATFORM != VOICE_PLATFORM_RAINNEXUS
#error "The RainNexus project must select VOICE_PLATFORM_RAINNEXUS"
#endif

#if RAINNEXUS_BRINGUP_STAGE != 4
#error "This branch intentionally implements RainNexus M4 only"
#endif

#ifndef RAINNEXUS_AUTH_URL
#define RAINNEXUS_AUTH_URL             "https://nexus.raindroprule.com/api/auth/login"
#endif

#ifndef RAINNEXUS_APP_KEY
#define RAINNEXUS_APP_KEY              "YOUR_RAINNEXUS_APP_KEY"
#endif

#ifndef RAINNEXUS_APP_SECRET
#define RAINNEXUS_APP_SECRET           "YOUR_RAINNEXUS_APP_SECRET"
#endif

#ifndef RAINNEXUS_AUTH_TIMEOUT_MS
#define RAINNEXUS_AUTH_TIMEOUT_MS      10000
#endif

#ifndef RAINNEXUS_AUTH_MAX_RETRIES
#define RAINNEXUS_AUTH_MAX_RETRIES     2
#endif

#ifndef RAINNEXUS_AUTH_RETRY_BASE_MS
#define RAINNEXUS_AUTH_RETRY_BASE_MS   500
#endif

#ifndef RAINNEXUS_WIFI_WAIT_LOG_SEC
#define RAINNEXUS_WIFI_WAIT_LOG_SEC    30
#endif

#ifndef RAINNEXUS_NTP_WAIT_LOG_SEC
#define RAINNEXUS_NTP_WAIT_LOG_SEC     30
#endif

#ifndef RAINNEXUS_M2_CONNECT_TIMEOUT_MS
#define RAINNEXUS_M2_CONNECT_TIMEOUT_MS 10000
#endif

#ifndef RAINNEXUS_M2_IO_TIMEOUT_MS
#define RAINNEXUS_M2_IO_TIMEOUT_MS     5000
#endif

#ifndef RAINNEXUS_M2_RECV_POLL_MS
#define RAINNEXUS_M2_RECV_POLL_MS      1000
#endif

#ifndef RAINNEXUS_M2_HELLO_TIMEOUT_MS
#define RAINNEXUS_M2_HELLO_TIMEOUT_MS  10000
#endif

#ifndef RAINNEXUS_M4_SESSION_TIMEOUT_MS
#define RAINNEXUS_M4_SESSION_TIMEOUT_MS 30000
#endif

#ifndef RAINNEXUS_M3_MAX_RECONNECTS
#define RAINNEXUS_M3_MAX_RECONNECTS 1
#endif

#ifndef RAINNEXUS_M4_AUDIO_QUEUE_DEPTH
#define RAINNEXUS_M4_AUDIO_QUEUE_DEPTH 8
#endif

#ifndef RAINNEXUS_M4_AUDIO_PACKET_CAPACITY
#define RAINNEXUS_M4_AUDIO_PACKET_CAPACITY 2048
#endif

#ifndef RAINNEXUS_M4_AUDIO_READ_WAIT_TICKS
#define RAINNEXUS_M4_AUDIO_READ_WAIT_TICKS 10
#endif

/* M4 bring-up stimulus; disable after downlink audio is verified. */
#ifndef RAINNEXUS_M4_TEXT_PROBE_ENABLE
#define RAINNEXUS_M4_TEXT_PROBE_ENABLE 1
#endif

/* The SDK Opus file decoder does not accept raw Xiaozhi frames directly. */
#ifndef RAINNEXUS_M4_AUDIO_DECODER_ENABLE
#define RAINNEXUS_M4_AUDIO_DECODER_ENABLE 0
#endif

#endif /* __RAINNEXUS_CONFIG_H__ */

