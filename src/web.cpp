#include "web.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>

#include "auth.h"
#include "cJSON.h"
#include "cert.h"
#include "esp_app_format.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "weather.h"
#include "web_assets.h"

static const char* TAG = "web";
static std::string g_cert, g_key;  // server keeps pointers to these

struct JsonDeleter { void operator()(cJSON* j) const { cJSON_Delete(j); } };
using Json = std::unique_ptr<cJSON, JsonDeleter>;

static bool authorized(httpd_req_t* req) {
    char hdr[256];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char dec[160];
        size_t n = 0;
        if (mbedtls_base64_decode(dec, sizeof dec - 1, &n, (const unsigned char*)hdr + 6, strlen(hdr + 6)) == 0) {
            dec[n] = 0;
            std::string s((char*)dec);
            size_t colon = s.find(':');
            if (colon != std::string::npos && auth::check(s.substr(0, colon), s.substr(colon + 1))) return true;
        }
    }
    // No WWW-Authenticate header: the UI has its own login form, not the browser prompt.
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t sendJson(httpd_req_t* req, cJSON* j) {
    char* s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return r;
}

static esp_err_t sendText(httpd_req_t* req, const char* status, const char* msg) {
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static Json readBody(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 512) return nullptr;
    char buf[513];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return nullptr;
        got += r;
    }
    buf[got] = 0;
    return Json(cJSON_Parse(buf));
}

static uint32_t uptimeS() { return (uint32_t)(esp_timer_get_time() / 1000000); }

static esp_err_t getCurrent(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    const weather::Current c = weather::current();
    cJSON* j = cJSON_CreateObject();
    if (c.haveObs) {
        const Obs& o = c.obs;
        cJSON* ob = cJSON_AddObjectToObject(j, "obs");
        cJSON_AddNumberToObject(ob, "epoch", o.epoch);
        cJSON_AddNumberToObject(ob, "lull", o.lull);
        cJSON_AddNumberToObject(ob, "avg", o.avg);
        cJSON_AddNumberToObject(ob, "gust", o.gust);
        cJSON_AddNumberToObject(ob, "dir", o.dir);
        cJSON_AddNumberToObject(ob, "pressure", o.pressure);
        cJSON_AddNumberToObject(ob, "temp", o.temp);
        cJSON_AddNumberToObject(ob, "hum", o.hum);
        cJSON_AddNumberToObject(ob, "lux", o.lux);
        cJSON_AddNumberToObject(ob, "uv", o.uv);
        cJSON_AddNumberToObject(ob, "solar", o.solar);
        cJSON_AddNumberToObject(ob, "rain", o.rain);
        cJSON_AddNumberToObject(ob, "precip", o.precipType);
        cJSON_AddNumberToObject(ob, "lightDist", o.lightDist);
        cJSON_AddNumberToObject(ob, "lightCount", o.lightCount);
        cJSON_AddNumberToObject(ob, "battery", o.battery);
        cJSON_AddNumberToObject(ob, "interval", o.interval);
        cJSON_AddNumberToObject(ob, "ageS", c.obsAgeS);
    }
    if (c.haveWind) {
        cJSON* w = cJSON_AddObjectToObject(j, "wind");
        cJSON_AddNumberToObject(w, "speed", c.windSpeed);
        cJSON_AddNumberToObject(w, "dir", c.windDir);
        cJSON_AddNumberToObject(w, "ageS", c.windAgeS);
    }
    if (c.haveHub) cJSON_AddNumberToObject(j, "hubRssi", c.hubRssi);
    cJSON_AddStringToObject(j, "serial", c.serial);
    cJSON_AddNumberToObject(j, "packetAgeS", weather::lastPacketAgeS() == UINT32_MAX ? -1 : weather::lastPacketAgeS());

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(j, "rssi", ap.rssi);
    cJSON_AddNumberToObject(j, "uptimeS", uptimeS());
    cJSON_AddBoolToObject(j, "defaultPassword", auth::isDefault());
    const esp_app_desc_t* desc = esp_app_get_description();
    cJSON_AddStringToObject(j, "version", desc->version);
    std::string built = std::string(desc->date) + " " + desc->time;
    cJSON_AddStringToObject(j, "built", built.c_str());
    return sendJson(req, j);
}

// Streams rows of [epoch, temp*100, hum*10, press*10, wind*100, gust*100, dir, rain*100, solar, uv*10, lux, mV, lightning].
static esp_err_t getHistory(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    int hours = 24;
    char query[48], val[12];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK &&
        httpd_query_key_value(query, "hours", val, sizeof val) == ESP_OK) {
        hours = std::clamp(atoi(val), 1, 24 * 14);
    }
    time_t now = time(nullptr);
    // Without a wall clock, fall back to the newest observation's own timestamp.
    uint32_t ref = now > 1'700'000'000 ? (uint32_t)now : weather::current().obs.epoch;
    uint32_t since = ref > (uint32_t)hours * 3600 ? ref - hours * 3600 : 0;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char buf[768];
    size_t len = 0;
    bool first = true, ok = true;
    len += snprintf(buf, sizeof buf, "{\"step\":%u,\"rows\":[", (unsigned)kBucketSeconds);
    weather::forEachSample(since, [&](const Sample& s) {
        if (len > sizeof buf - 128) {
            ok = httpd_resp_send_chunk(req, buf, len) == ESP_OK;
            len = 0;
            if (!ok) return false;
        }
        len += snprintf(buf + len, sizeof buf - len, "%s[%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", first ? "" : ",",
                        (unsigned)s.epoch, s.temp100, s.hum10, s.press10, s.wind100, s.gust100, s.dir, s.rain100,
                        s.solar, s.uv10, (unsigned)s.lux, s.batteryMv, s.lightning);
        first = false;
        return true;
    });
    if (!ok) return ESP_FAIL;
    len += snprintf(buf + len, sizeof buf - len, "]}");
    if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t postPassword(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    Json body = readBody(req);
    cJSON* p = body ? cJSON_GetObjectItem(body.get(), "password") : nullptr;
    if (!cJSON_IsString(p)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "password required");
    if (!auth::set(p->valuestring)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "min 6 chars, not 'admin'");
    return httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t serveStatic(httpd_req_t* req) {
    // The UI shell is public so the login page can load; all data is behind /api auth.
    std::string uri = req->uri;
    uri = uri.substr(0, uri.find('?'));
    for (size_t i = 0; i < kWebAssetCount; i++) {
        const WebAsset& a = kWebAssets[i];
        if (uri != a.path) continue;
        httpd_resp_set_type(req, a.mime);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        return httpd_resp_send(req, (const char*)a.data, a.size);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

static bool recvFull(httpd_req_t* req, char* buf, size_t len) {
    size_t got = 0;
    int idle = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle > 10) return false;
            continue;
        }
        if (n <= 0) return false;
        idle = 0;
        got += n;
    }
    return true;
}

// Rejects images that are not an app build of this project before anything is written.
static bool isOurImage(const char* first) {
    constexpr size_t kDescOffset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if ((uint8_t)first[0] != ESP_IMAGE_HEADER_MAGIC) return false;
    esp_app_desc_t desc;
    memcpy(&desc, first + kDescOffset, sizeof desc);
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    return strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof desc.project_name) == 0;
}

static esp_err_t postFirmware(httpd_req_t* req) {
    constexpr size_t kChunk = 4096;
    constexpr size_t kMinImage = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (!authorized(req)) return ESP_OK;

    const size_t total = req->content_len;
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part || total < kMinImage) return sendText(req, "400 Bad Request", "Not a firmware image");
    if (total > part->size) return sendText(req, "413 Payload Too Large", "Image too large");

    std::unique_ptr<char[]> buf(new (std::nothrow) char[kChunk]);
    if (!buf) return httpd_resp_send_500(req);

    esp_ota_handle_t handle = 0;
    bool begun = false;
    size_t received = 0;
    while (received < total) {
        const size_t n = std::min(kChunk, total - received);
        if (!recvFull(req, buf.get(), n)) {
            if (begun) esp_ota_abort(handle);
            ESP_LOGW(TAG, "firmware upload interrupted at %u of %u", (unsigned)received, (unsigned)total);
            return ESP_FAIL;
        }
        if (!begun) {
            if (!isOurImage(buf.get())) return sendText(req, "400 Bad Request", "Not a firmware image for this device");
            if (esp_ota_begin(part, total, &handle) != ESP_OK) return httpd_resp_send_500(req);
            begun = true;
        }
        if (esp_ota_write(handle, buf.get(), n) != ESP_OK) {
            esp_ota_abort(handle);
            return httpd_resp_send_500(req);
        }
        received += n;
    }
    if (esp_ota_end(handle) != ESP_OK) return sendText(req, "400 Bad Request", "Image failed validation");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return httpd_resp_send_500(req);

    ESP_LOGI(TAG, "firmware installed (%u bytes), restarting", (unsigned)total);
    sendText(req, "200 OK", "ok");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

bool startWebServer() {
    if (!ensureCert(g_cert, g_key)) {
        ESP_LOGE(TAG, "certificate unavailable");
        return false;
    }
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert = (const uint8_t*)g_cert.c_str();
    cfg.servercert_len = g_cert.size() + 1;
    cfg.prvtkey_pem = (const uint8_t*)g_key.c_str();
    cfg.prvtkey_len = g_key.size() + 1;
    cfg.httpd.max_uri_handlers = 12;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.httpd.stack_size = 10240;
    cfg.httpd.recv_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    if (httpd_ssl_start(&server, &cfg) != ESP_OK) return false;

    const httpd_uri_t routes[] = {
        {"/api/current", HTTP_GET, getCurrent, nullptr},
        {"/api/history", HTTP_GET, getHistory, nullptr},
        {"/api/password", HTTP_POST, postPassword, nullptr},
        {"/api/firmware", HTTP_POST, postFirmware, nullptr},
        {"/*", HTTP_GET, serveStatic, nullptr},  // must stay last
    };
    for (const auto& r : routes) httpd_register_uri_handler(server, &r);
    return true;
}
