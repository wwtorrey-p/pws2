#include "auth.h"

#include <cstring>

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
    // Keep username after the legacy credential blob so it can be prefix-migrated.
    uint8_t reserved[3];
    char username[33];
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

bool validUsername(const std::string& user) {
    if (user.empty() || user.size() > 32) return false;
    for (unsigned char c : user) {
        if (c < 0x20 || c > 0x7e || c == ':') return false;
    }
    return true;
}

}  // namespace

void init() {
    g_lock = xSemaphoreCreateMutex();
    if (!storage::loadPrefix("cred", g_cred)) {
        make(g_cred, "admin", true);
        memcpy(g_cred.username, "admin", sizeof("admin"));
        if (!storage::save("cred", g_cred)) ESP_LOGE(TAG, "could not save default credentials");
    } else if (g_cred.username[0] == '\0') {
        memcpy(g_cred.username, "admin", sizeof("admin"));
        if (!storage::save("cred", g_cred)) ESP_LOGE(TAG, "could not migrate saved credentials");
    }
}

bool check(const std::string& user, const std::string& pass) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Credentials c = g_cred;
    xSemaphoreGive(g_lock);
    uint8_t h[32];
    hashPassword(c.salt, pass, h);
    // Compare everything without early exit so timing does not reveal how much matched.
    uint8_t diff = user == c.username ? 0 : 1;
    for (int i = 0; i < 32; i++) diff |= h[i] ^ c.hash[i];
    return diff == 0;
}

SetResult set(const std::string& user, const std::string& pass) {
    if (!validUsername(user)) return SetResult::InvalidUsername;
    if (pass.size() < 6 || pass.size() > 64 || pass == "admin") return SetResult::InvalidPassword;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    Credentials updated = g_cred;
    make(updated, pass, false);
    memset(updated.username, 0, sizeof(updated.username));
    memcpy(updated.username, user.data(), user.size());
    bool ok = storage::save("cred", updated);
    if (ok) g_cred = updated;
    xSemaphoreGive(g_lock);
    if (!ok) {
        ESP_LOGE(TAG, "could not save credentials");
        return SetResult::StorageError;
    }
    ESP_LOGI(TAG, "credentials changed");
    return SetResult::Ok;
}

std::string username() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    std::string user(g_cred.username);
    xSemaphoreGive(g_lock);
    return user;
}

bool isDefault() {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool d = g_cred.isDefault;
    xSemaphoreGive(g_lock);
    return d;
}

}  // namespace auth
