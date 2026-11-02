#include "weather.h"

#include <cstring>
#include <memory>
#include <vector>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace weather {
namespace {

const char* TAG = "weather";
constexpr uint16_t kUdpPort = 50222;
constexpr size_t kSectorBytes = 4096;
constexpr size_t kSlotsPerSector = kSectorBytes / sizeof(Sample);
constexpr size_t kBatch = 32;

SemaphoreHandle_t g_lock;
const esp_partition_t* g_part;
size_t g_slots;  // ring capacity, from the partition size
std::vector<Sample> g_ring;
size_t g_head;  // next slot to write

Current g_cur;
int64_t g_obsMs = -1, g_windMs = -1, g_anyMs = -1;
Aggregator g_agg;

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

uint32_t ageS(int64_t ms) {
    if (ms < 0) return UINT32_MAX;
    return static_cast<uint32_t>((esp_timer_get_time() / 1000 - ms) / 1000);
}

bool validSample(const Sample& s) { return s.epoch != kEmptyEpoch && s.epoch > 1'700'000'000u; }

bool loadRing() {
    g_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "history");
    if (!g_part) {
        ESP_LOGE(TAG, "history partition missing");
        return false;
    }
    g_slots = g_part->size / kSectorBytes * kSlotsPerSector;
    g_ring.assign(g_slots, Sample{});
    if (esp_partition_read(g_part, 0, g_ring.data(), g_slots * sizeof(Sample)) != ESP_OK) return false;

    // Slots written within one sector are in time order, so the newest record marks where the ring continues.
    uint32_t newest = 0;
    g_head = 0;
    size_t valid = 0;
    for (size_t i = 0; i < g_slots; i++) {
        if (!validSample(g_ring[i])) {
            g_ring[i].epoch = kEmptyEpoch;
            continue;
        }
        valid++;
        if (g_ring[i].epoch >= newest) {
            newest = g_ring[i].epoch;
            g_head = (i + 1) % g_slots;
        }
    }
    ESP_LOGI(TAG, "history: %u samples, %u slots", (unsigned)valid, (unsigned)g_slots);
    return true;
}

void storeSample(const Sample& s) {
    size_t slot;
    {
        Lock l;
        slot = g_head;
        g_head = (g_head + 1) % g_slots;
    }
    // Crossing into a new sector reclaims it, which drops that sector's oldest samples.
    if (slot % kSlotsPerSector == 0) {
        esp_partition_erase_range(g_part, slot * sizeof(Sample), kSectorBytes);
        Lock l;
        for (size_t i = 0; i < kSlotsPerSector; i++) g_ring[slot + i].epoch = kEmptyEpoch;
    }
    esp_partition_write(g_part, slot * sizeof(Sample), &s, sizeof s);
    Lock l;
    g_ring[slot] = s;
}

double at(cJSON* arr, int i, double def = 0) {
    cJSON* v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

void handleObs(cJSON* root) {
    cJSON* obsArr = cJSON_GetObjectItem(root, "obs");
    cJSON* a = cJSON_IsArray(obsArr) ? cJSON_GetArrayItem(obsArr, 0) : nullptr;
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) < 18) return;
    Obs o;
    o.epoch = static_cast<uint32_t>(at(a, 0));
    o.lull = at(a, 1);
    o.avg = at(a, 2);
    o.gust = at(a, 3);
    o.dir = at(a, 4);
    o.pressure = at(a, 6);
    o.temp = at(a, 7);
    o.hum = at(a, 8);
    o.lux = at(a, 9);
    o.uv = at(a, 10);
    o.solar = at(a, 11);
    o.rain = at(a, 12);
    o.precipType = static_cast<int>(at(a, 13));
    o.lightDist = at(a, 14);
    o.lightCount = static_cast<int>(at(a, 15));
    o.battery = at(a, 16);
    o.interval = static_cast<int>(at(a, 17, 1));

    Sample done;
    bool flushed;
    {
        Lock l;
        g_cur.haveObs = true;
        g_cur.obs = o;
        g_obsMs = esp_timer_get_time() / 1000;
        cJSON* sn = cJSON_GetObjectItem(root, "serial_number");
        if (cJSON_IsString(sn)) strlcpy(g_cur.serial, sn->valuestring, sizeof g_cur.serial);
        flushed = g_agg.add(o, done);
    }
    if (flushed) storeSample(done);
}

void handleRapidWind(cJSON* root) {
    cJSON* ob = cJSON_GetObjectItem(root, "ob");
    if (!cJSON_IsArray(ob) || cJSON_GetArraySize(ob) < 3) return;
    Lock l;
    g_cur.haveWind = true;
    g_cur.windSpeed = at(ob, 1);
    g_cur.windDir = at(ob, 2);
    g_windMs = esp_timer_get_time() / 1000;
}

void handleStatus(cJSON* root) {
    cJSON* rssi = cJSON_GetObjectItem(root, "rssi");
    if (!cJSON_IsNumber(rssi)) return;
    Lock l;
    g_cur.haveHub = true;
    g_cur.hubRssi = static_cast<int>(rssi->valuedouble);
}

void handlePacket(const char* data) {
    std::unique_ptr<cJSON, void (*)(cJSON*)> root(cJSON_Parse(data), cJSON_Delete);
    if (!root) return;
    cJSON* type = cJSON_GetObjectItem(root.get(), "type");
    if (!cJSON_IsString(type)) return;
    {
        Lock l;
        g_anyMs = esp_timer_get_time() / 1000;
    }
    if (!strcmp(type->valuestring, "obs_st")) handleObs(root.get());
    else if (!strcmp(type->valuestring, "rapid_wind")) handleRapidWind(root.get());
    else if (!strcmp(type->valuestring, "hub_status") || !strcmp(type->valuestring, "device_status")) handleStatus(root.get());
}

void udpTask(void*) {
    constexpr size_t kBuf = 2048;
    std::unique_ptr<char[]> buf(new char[kBuf]);
    for (;;) {
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(kUdpPort);
        timeval tv = {5, 0};
        if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            ESP_LOGW(TAG, "UDP bind failed, retrying");
            if (sock >= 0) close(sock);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ESP_LOGI(TAG, "listening for Tempest broadcasts on UDP %d", kUdpPort);
        for (;;) {
            int n = recvfrom(sock, buf.get(), kBuf - 1, 0, nullptr, nullptr);
            if (n > 0) {
                buf[n] = 0;
                handlePacket(buf.get());
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;  // socket broke, reopen it
            }
        }
        close(sock);
    }
}

}  // namespace

bool begin() {
    g_lock = xSemaphoreCreateMutex();
    if (!loadRing()) return false;
    return xTaskCreate(udpTask, "udp_rx", 6144, nullptr, 5, nullptr) == pdPASS;
}

Current current() {
    Lock l;
    Current c = g_cur;
    c.obsAgeS = ageS(g_obsMs);
    c.windAgeS = ageS(g_windMs);
    return c;
}

uint32_t lastPacketAgeS() {
    Lock l;
    return ageS(g_anyMs);
}

void forEachSample(uint32_t sinceEpoch, const std::function<bool(const Sample&)>& fn) {
    // Oldest first: the slot after the write head holds the oldest sample.
    size_t pos = 0;
    Sample batch[kBatch];
    while (pos < g_slots) {
        size_t n = 0;
        {
            Lock l;
            for (; pos < g_slots && n < kBatch; pos++) {
                const Sample& s = g_ring[(g_head + pos) % g_slots];
                if (validSample(s) && s.epoch >= sinceEpoch) batch[n++] = s;
            }
        }
        for (size_t i = 0; i < n; i++)
            if (!fn(batch[i])) return;
    }
}

}  // namespace weather
