/**
 * @file rainnexus.h
 * @brief RainNexus 分阶段接入入口
 */

#ifndef __RAINNEXUS_H__
#define __RAINNEXUS_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 RainNexus 接入任务。
 *
 * M2 流程复用产品现有 Wi-Fi/DHCP/NTP，完成登录、WSS 建连和
 * hello 校验后停在验证态，不启动音频采集或播放。
 */
int rainnexus_start(void);

#ifdef __cplusplus
}
#endif

#endif /* __RAINNEXUS_H__ */

