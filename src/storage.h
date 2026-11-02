#pragma once
#include <cstddef>
#include <cstring>

#include "nvs.h"

namespace storage {

inline bool loadBlob(const char* key, void* out, size_t size) {
    nvs_handle_t h;
    if (nvs_open("wx", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = size;
    bool ok = nvs_get_blob(h, key, out, &len) == ESP_OK && len == size;
    nvs_close(h);
    return ok;
}

inline bool saveBlob(const char* key, const void* data, size_t size) {
    nvs_handle_t h;
    if (nvs_open("wx", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key, data, size) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

template <typename T>
bool load(const char* key, T& v) { return loadBlob(key, &v, sizeof(T)); }

// Like load(), but accepts a blob saved by an older build with fewer fields; the rest of v is left as is.
template <typename T>
bool loadPrefix(const char* key, T& v) {
    nvs_handle_t h;
    if (nvs_open("wx", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, key, nullptr, &len) == ESP_OK && len > 0 && len <= sizeof(T) &&
              nvs_get_blob(h, key, &v, &len) == ESP_OK;
    nvs_close(h);
    return ok;
}

template <typename T>
bool save(const char* key, const T& v) { return saveBlob(key, &v, sizeof(T)); }

inline void erase(const char* key) {
    nvs_handle_t h;
    if (nvs_open("wx", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

}  // namespace storage
