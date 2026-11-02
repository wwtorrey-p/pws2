#include "auth.h"

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "storage.h"

namespace auth {
namespace {

struct Credentials {
    uint8_t salt[16];
    uint8_t hash[32];
    bool isDefault;
};

const char* TAG = "auth";
Credentials g_cred;
SemaphoreHandle_t g_lock;

void hashPassword(const uint8_t salt[16], const std::string& pass, uint8_t out[32]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, 16);
    mbedtls_sha256_update(&c, reinterpret_cast<const unsigned char*>(pass.data()), pass.size());
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
}

void make(Credentials& c, const std::string& pass, bool isDefault) {
    esp_fill_random(c.salt, sizeof c.salt);
    hashPassword(c.salt, pass, c.hash);
    c.isDefault = isDefault;
}

}  // namespace

void init() {
    g_lock = xSemaphoreCreateMutex();
    if (!storage::load("cred", g_cred)) {
        make(g_cred, "admin", true);
        storage::save("cred", g_cred);
    }
}

bool check(const std::string& user, const std::string& pass) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Credentials c = g_cred;
    xSemaphoreGive(g_lock);
    uint8_t h[32];
    hashPassword(c.salt, pass, h);
    // Compare everything without early exit so timing does not reveal how much matched.
    uint8_t diff = user == "admin" ? 0 : 1;
    for (int i = 0; i < 32; i++) diff |= h[i] ^ c.hash[i];
    return diff == 0;
}

bool set(const std::string& pass) {
    if (pass.size() < 6 || pass == "admin") return false;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    make(g_cred, pass, false);
    bool ok = storage::save("cred", g_cred);
    xSemaphoreGive(g_lock);
    ESP_LOGI(TAG, "password changed");
    return ok;
}

bool isDefault() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool d = g_cred.isDefault;
    xSemaphoreGive(g_lock);
    return d;
}

}  // namespace auth
