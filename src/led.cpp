#include "led.h"

#include "led_strip.h"

static led_strip_handle_t g_strip;

void ledInit(int gpio) {
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = gpio;
    cfg.max_leds = 1;
    cfg.led_model = LED_MODEL_WS2812;
    cfg.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;
    if (led_strip_new_rmt_device(&cfg, &rmt, &g_strip) != ESP_OK) g_strip = nullptr;
}

void ledSet(uint8_t r, uint8_t g, uint8_t b) {
    if (!g_strip) return;
    led_strip_set_pixel(g_strip, 0, r, g, b);
    led_strip_refresh(g_strip);
}
