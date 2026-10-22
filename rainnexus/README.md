# RainNexus 接入状态

本目录是“精灵头 - RainNexus”项目后续的平台业务开发入口。

当前阶段为 M5：

1. 复用精灵头项目原有的 Wi-Fi、DHCP 和 NTP 流程；
2. 网络和时间就绪后向 RainNexus 登录接口发送 HTTPS POST；
3. 校验并保存 `token`、`expires_in`、`wss_url` 和 `xiaozhi_url`；
4. 使用 `xiaozhi_url`，通过 URL 的 `token`、`device-id` 和 `client-id` 查询参数建立 WSS；
5. 发送小智兼容协议客户端 `hello`；
6. 校验服务端 `hello`、音频参数和 `session_id`；
7. hello 通过后保持有限时长的协议会话，启动 SDK 心跳并解析后续 JSON 事件；
8. 按 `IO_PORTC_06`（板级 `KEY_OK`）第一次点按后发送 `listen/start`，采集 16 kHz 单声道麦克风；
9. 使用 SDK `audio_server` 编码为 60 ms 裸 Opus 包并通过 WebSocket 二进制帧上传；
10. 按键第二次点按后停止编码并发送 `listen/stop`，同时接收服务端 JSON 和下行 Opus 并写入 DAC；
11. 会话结束时按 `AUDIO_ENC_STOP`、`AUDIO_ENC_CLOSE` 顺序释放编码器。

北觅文档将 `device-id` 和 `client-id` 标记为可选，但 2026-10-17
实测只传 `token` 时服务端返回端口测试提示，补齐两个 ID 后才返回
协议 `hello`。因此设备端用当前 Wi-Fi MAC 生成稳定 ID，并对查询参数
进行 URL 编码。

构建入口：

- `rainnexus.mk`：Makefile 构建接入；
- `apps/wifi_story_machine/board/wl82/AC791N_WIFI_STORY_MACHINE.cbp`：主 CodeBlocks 工程接入；
- `apps/wifi_story_machine/app_main.c`：启动 RainNexus 任务，并屏蔽原云平台/音频业务启动；
- `apps/wifi_story_machine/multiple_ethernetif.c`：RainNexus 启用时不启动旧 QYAI MQTT。

本次迁移没有覆盖或修改以下目标项目基线：

- `apps/wifi_story_machine/board/wl82/board_config.h`；
- `apps/wifi_story_machine/wifi_app_task.c`；
- 板级、射频和 Wi-Fi 配置；
- 精灵头产品已有的 UI、按键和硬件初始化。

M5 当前采用 `KEY_OK` 点按控制录音：WSS hello 成功后等待第一次点按，第一次点按启动
编码和上行，第二次点按停止编码并发送 `listen/stop`；等待期间仍处理服务端下行 JSON
和音频；录音阶段不会因为固定时长自动结束，token 过期时最多重新登录一次。
串口应看到 `polling KEY_OK on IO_PORTC_06`、`listen/start`、
`PCM microphone + virtual Opus encoder started`、
多条 `TX Opus` 以及第二次点按后的 `listen/stop`。

Code::Blocks 下载前请执行 `Build -> Rebuild`，避免旧的 M4 对象文件参与链接。

