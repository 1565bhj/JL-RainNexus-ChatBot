#include "system/includes.h"
#include "asm/gpio.h"
#include "app_config.h"
#include "os/os_api.h"
#include "chatbot/ai_uart_ctrol.h"

#ifdef TCFG_TOUCH_KEY_PORT
#if (TCFG_TOUCH_KEY_PORT != -1)

#define TOUCH_KEY_SCAN_TIME     20
#define TOUCH_KEY_DEBOUNCE_CNT  3

extern int music_play_res_file(const char *name);
extern int play_face_emoji(int emoji_index);

static u8 touch_low_count;
static u8 touch_pressed;

static void tm_touch_key_scan(void)
{
    if (!gpio_read(TCFG_TOUCH_KEY_PORT)) {
        if (touch_low_count < TOUCH_KEY_DEBOUNCE_CNT) {
            touch_low_count++;
        }

        if (!touch_pressed && touch_low_count == TOUCH_KEY_DEBOUNCE_CNT) {
            touch_pressed = 1;
            printf("V8T04B touch\n");
#ifdef CONFIG_UI_PLAY_EMOJI
            play_face_emoji(AI_UART_CMD_EMOJI_HAPPY);
#endif
            music_play_res_file("happy.mp3");
        }
    } else {
        touch_low_count = 0;
        touch_pressed = 0;
    }
}
static void tm_touch_key_scan_init(void)
{
    gpio_direction_input(TCFG_TOUCH_KEY_PORT);
    gpio_set_die(TCFG_TOUCH_KEY_PORT, 1);
    gpio_set_pull_down(TCFG_TOUCH_KEY_PORT, 0);
    gpio_set_pull_up(TCFG_TOUCH_KEY_PORT, 0);
    sys_timer_add_to_task("sys_timer", NULL, tm_touch_key_scan, TOUCH_KEY_SCAN_TIME);
}
static void tm_touch_key_init(void)
{
    if (production_test_io_get()) {
        return;
    }
    sys_timeout_add_to_task("sys_timer", NULL, tm_touch_key_scan_init, 1000);//1秒后再检测按键
}
late_initcall(tm_touch_key_init);
#endif
#endif
