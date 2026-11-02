#include <cstring>

#include "auth.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led.h"
#include "nvs_flash.h"
#include "weather.h"
#include "web.h"
#include "wifi.h"

constexpr gpio_num_t kPinBoot = GPIO_NUM_0;  // BOOT button: hold to re-enter the Wi-Fi network
constexpr int kPinLed = 48;                  // on-board WS2812
constexpr int kBootHoldTicks = 20;           // 2 s at the 100 ms loop rate
constexpr uint32_t kFreshS = 180;            // an observation is expected every minute

static const char* TAG = "main";
static bool g_webOk = false;

// A freshly updated image must prove itself or the bootloader reverts to the previous one.
static void checkOtaHealth() {
    constexpr int64_t kConfirmUs = 30LL * 1000 * 1000;
    constexpr int64_t kGiveUpUs = 5LL * 60 * 1000 * 1000;
    static bool done = false;
    if (done) return;

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        done = true;
        return;
    }
    const int64_t up = esp_timer_get_time();
    // With no saved network the image is fine but waiting for setup, so only a saved one must get online.
    if ((wifi::connected() || !wifi::hasCredentials()) && g_webOk && up > kConfirmUs) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed");
        done = true;
    } else if (up > kGiveUpUs) {
        ESP_LOGE(TAG, "new firmware never came up, rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

extern "C" void app_main() {
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << kPinBoot;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&in);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi::init();
    // Hub broadcasts are only delivered at DTIM intervals when power save is on, so packets would be missed.
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Credentials already saved: join without asking. Otherwise ask over the serial port.
    if (wifi::hasCredentials()) wifi::connectStored();
    else wifi::startProvisioning();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    ledInit(kPinLed);
    auth::init();
    if (!weather::begin()) ESP_LOGE(TAG, "weather store failed to start");
    g_webOk = startWebServer();
    if (!g_webOk) ESP_LOGE(TAG, "web server failed to start");

    TickType_t last = xTaskGetTickCount();
    int bootHeld = 0;
    for (;;) {
        if (gpio_get_level(kPinBoot) == 0) {
            if (++bootHeld == kBootHoldTicks) wifi::startProvisioning();
        } else {
            bootHeld = 0;
        }

        // Dim colours: purple Wi-Fi setup, red offline, amber waiting for or lost the station, green receiving.
        if (wifi::provisioning()) ledSet(24, 0, 24);
        else if (!wifi::connected()) ledSet(32, 0, 0);
        else if (weather::lastPacketAgeS() > kFreshS) ledSet(32, 16, 0);
        else ledSet(0, 16, 0);

        checkOtaHealth();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}
