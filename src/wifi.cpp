#include "wifi.h"

#include <cstdio>
#include <cstring>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "storage.h"

namespace wifi {
namespace {

const char* TAG = "wifi";
constexpr EventBits_t kGotIp = BIT0;
constexpr uint32_t kTrialMs = 30000;       // how long a new network gets to hand out an address
constexpr uint32_t kIdleCancelMs = 60000;  // a prompt opened by the BOOT button gives up after this
constexpr uint32_t kReprintMs = 10000;     // repeat an unanswered prompt so it is not lost in the log output

struct Saved {
    char ssid[33];
    char pass[65];
};

EventGroupHandle_t g_events;
volatile bool g_autoReconnect = false;
volatile int g_lastReason = 0;
volatile bool g_provisioning = false;
bool g_uartReady = false;
bool g_lastWasCr = false;

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_autoReconnect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_lastReason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        xEventGroupClearBits(g_events, kGotIp);
        ESP_LOGW(TAG, "disconnected, reason %d", g_lastReason);
        if (g_autoReconnect) esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        xEventGroupSetBits(g_events, kGotIp);
        ESP_LOGI(TAG, "connected, https://" IPSTR "/", IP2STR(&ev->ip_info.ip));
    }
}

void apply(const char* ssid, const char* pass) {
    g_autoReconnect = false;  // the disconnect below is intentional
    esp_wifi_disconnect();
    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char*>(cfg.sta.ssid), ssid, sizeof cfg.sta.ssid);
    strlcpy(reinterpret_cast<char*>(cfg.sta.password), pass, sizeof cfg.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    xEventGroupClearBits(g_events, kGotIp);
    g_lastReason = 0;
    g_autoReconnect = true;
    esp_wifi_connect();
}

bool tryConnect(const char* ssid, const char* pass) {
    apply(ssid, pass);
    return xEventGroupWaitBits(g_events, kGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(kTrialMs)) & kGotIp;
}

const char* hint(int reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return " (network not found; check the name, and note the ESP32 needs 2.4 GHz)";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return " (wrong password?)";
        default:
            return "";
    }
}

void uartInit() {
    if (g_uartReady) return;
    // Receive only: log output keeps using the console's own transmit path.
    uart_driver_install(UART_NUM_0, 256, 0, 0, nullptr, 0);
    g_uartReady = true;
}

// Returns false if idleMs (0 = wait forever) passes without a key press.
bool readLine(char* out, size_t cap, bool mask, uint32_t idleMs, const char* prompt) {
    size_t n = 0;
    uint32_t idle = 0, quiet = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) <= 0) {
            idle += 100;
            quiet += 100;
            if (idleMs && idle >= idleMs) return false;
            if (n == 0 && quiet >= kReprintMs) {
                printf("\n%s", prompt);
                fflush(stdout);
                quiet = 0;
            }
            continue;
        }
        idle = 0;
        quiet = 0;
        if (c == '\n' && n == 0 && g_lastWasCr) {  // second half of a CR+LF pair
            g_lastWasCr = false;
            continue;
        }
        g_lastWasCr = (c == '\r');
        if (c == '\r' || c == '\n') {
            out[n] = 0;
            printf("\n");
            fflush(stdout);
            return true;
        }
        if (c == 0x08 || c == 0x7f) {
            if (n > 0) {
                n--;
                printf("\b \b");
            }
        } else if (c >= 0x20 && c < 0x7f && n + 1 < cap) {
            out[n++] = c;
            putchar(mask ? '*' : c);
        }
        fflush(stdout);
    }
}

void provisionTask(void*) {
    uartInit();
    Saved current{};
    bool have = storage::load("wifi", current);
    const uint32_t idle = have ? kIdleCancelMs : 0;
    bool done = false;

    printf("\n=== Wi-Fi setup ===\n");
    if (have) printf("Current network: %s. Leave the name empty to cancel.\n", current.ssid);

    while (!done) {
        char ssid[33], pass[65];
        uart_flush_input(UART_NUM_0);
        printf("Network name: ");
        fflush(stdout);
        if (!readLine(ssid, sizeof ssid, false, idle, "Network name: ")) {
            printf("Timed out. Keeping the current network.\n");
            break;
        }
        if (!ssid[0]) {
            if (have) {
                printf("Cancelled.\n");
                break;
            }
            printf("A network name is required.\n");
            continue;
        }
        printf("Password (shown as *, empty for an open network): ");
        fflush(stdout);
        if (!readLine(pass, sizeof pass, true, idle, "Password (shown as *, empty for an open network): ")) {
            printf("Timed out. Keeping the current network.\n");
            break;
        }
        if (pass[0] && strlen(pass) < 8) {
            printf("A WPA2 password needs at least 8 characters.\n");
            continue;
        }

        printf("Testing %s (up to %u s)...\n", ssid, (unsigned)(kTrialMs / 1000));
        if (tryConnect(ssid, pass)) {
            Saved s{};
            strlcpy(s.ssid, ssid, sizeof s.ssid);
            strlcpy(s.pass, pass, sizeof s.pass);
            printf(storage::save("wifi", s) ? "Connected and saved.\n" : "Connected, but saving failed.\n");
            done = true;
            have = true;
        } else {
            printf("Could not connect%s. Nothing was saved.\n", hint(g_lastReason));
            if (have) apply(current.ssid, current.pass);  // keep the device reachable while retrying
        }
    }

    if (!done) {
        if (have) {
            apply(current.ssid, current.pass);
        } else {
            g_autoReconnect = false;
            esp_wifi_disconnect();
        }
    }
    g_provisioning = false;
    vTaskDelete(nullptr);
}

}  // namespace

void init() {
    g_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // Keep the driver from saving networks itself, so a failed trial is never stored.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool hasCredentials() {
    Saved s;
    return storage::load("wifi", s) && s.ssid[0];
}

void connectStored() {
    Saved s;
    if (!storage::load("wifi", s)) return;
    s.ssid[sizeof s.ssid - 1] = 0;
    s.pass[sizeof s.pass - 1] = 0;
    apply(s.ssid, s.pass);
}

bool connected() { return xEventGroupGetBits(g_events) & kGotIp; }

bool provisioning() { return g_provisioning; }

void startProvisioning() {
    if (g_provisioning) return;
    g_provisioning = true;
    if (xTaskCreate(provisionTask, "wifi_setup", 5120, nullptr, 3, nullptr) != pdPASS) g_provisioning = false;
}

}  // namespace wifi
