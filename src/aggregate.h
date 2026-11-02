#pragma once
#include <cstdint>

// One Tempest obs_st observation, in the units the hub broadcasts.
struct Obs {
    uint32_t epoch = 0;  // UTC seconds
    float lull = 0, avg = 0, gust = 0;  // m/s
    float dir = 0;                      // degrees
    float pressure = 0;                 // mb
    float temp = 0;                     // C
    float hum = 0;                      // %
    float lux = 0, uv = 0, solar = 0;
    float rain = 0;                     // mm over the reporting interval
    int precipType = 0;
    float lightDist = 0;                // km
    int lightCount = 0;
    float battery = 0;                  // V
    int interval = 1;                   // minutes
};

// Compact record kept in RAM and the flash ring. Fixed point so 5 minutes of data fit in 32 bytes.
struct Sample {
    uint32_t epoch;      // start of the 5-minute bucket; 0xFFFFFFFF = unused slot
    uint32_t lux;
    int16_t temp100;     // 0.01 C
    uint16_t hum10;      // 0.1 %
    uint16_t press10;    // 0.1 mb
    uint16_t wind100;    // 0.01 m/s, average
    uint16_t gust100;    // 0.01 m/s, maximum
    uint16_t dir;        // degrees
    uint16_t rain100;    // 0.01 mm, total
    uint16_t solar;      // W/m2
    uint16_t uv10;       // 0.1 index
    uint16_t batteryMv;
    uint8_t lightning;   // strikes in the bucket, saturating
    uint8_t lightDistKm;
    uint16_t reserved;
};
static_assert(sizeof(Sample) == 32, "Sample must stay 32 bytes so records never straddle a flash sector");

constexpr uint32_t kEmptyEpoch = 0xFFFFFFFFu;
constexpr uint32_t kBucketSeconds = 300;

// Averages observations into 5-minute samples.
class Aggregator {
public:
    // Returns true and fills out when obs starts a new bucket and the previous one is complete.
    bool add(const Obs& obs, Sample& out);

private:
    Sample finish() const;
    void reset();

    uint32_t bucket_ = 0;
    int n_ = 0;
    double temp_ = 0, hum_ = 0, press_ = 0, wind_ = 0, solar_ = 0, uv_ = 0, lux_ = 0, rain_ = 0;
    double sinSum_ = 0, cosSum_ = 0;
    float gust_ = 0, battery_ = 0, lightDist_ = 0;
    int lightning_ = 0;
};
