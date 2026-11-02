#include "aggregate.h"

#include <algorithm>
#include <cmath>

namespace {

template <typename T>
T clampTo(double v, double lo, double hi) {
    return static_cast<T>(std::lround(std::min(std::max(v, lo), hi)));
}

}  // namespace

void Aggregator::reset() {
    n_ = 0;
    temp_ = hum_ = press_ = wind_ = solar_ = uv_ = lux_ = rain_ = 0;
    sinSum_ = cosSum_ = 0;
    gust_ = battery_ = lightDist_ = 0;
    lightning_ = 0;
}

bool Aggregator::add(const Obs& o, Sample& out) {
    const uint32_t bucket = o.epoch / kBucketSeconds;
    bool flushed = false;
    if (n_ > 0) {
        if (bucket < bucket_) return false;  // late or duplicate packet
        if (bucket != bucket_) {
            out = finish();
            flushed = true;
            reset();
        }
    }
    bucket_ = bucket;
    n_++;
    temp_ += o.temp;
    hum_ += o.hum;
    press_ += o.pressure;
    wind_ += o.avg;
    solar_ += o.solar;
    uv_ += o.uv;
    lux_ += o.lux;
    rain_ += o.rain;
    const double rad = o.dir * M_PI / 180.0;
    sinSum_ += std::sin(rad) * o.avg;  // weight by speed so calm periods do not swing the direction
    cosSum_ += std::cos(rad) * o.avg;
    gust_ = std::max(gust_, o.gust);
    battery_ = o.battery;
    lightning_ += o.lightCount;
    if (o.lightCount > 0) lightDist_ = o.lightDist;
    return flushed;
}

Sample Aggregator::finish() const {
    Sample s{};
    const double n = n_ > 0 ? n_ : 1;
    s.epoch = bucket_ * kBucketSeconds;
    s.lux = static_cast<uint32_t>(std::max(0.0, std::round(lux_ / n)));
    s.temp100 = clampTo<int16_t>(temp_ / n * 100, -32768, 32767);
    s.hum10 = clampTo<uint16_t>(hum_ / n * 10, 0, 65535);
    s.press10 = clampTo<uint16_t>(press_ / n * 10, 0, 65535);
    s.wind100 = clampTo<uint16_t>(wind_ / n * 100, 0, 65535);
    s.gust100 = clampTo<uint16_t>(gust_ * 100, 0, 65535);
    double deg = std::atan2(sinSum_, cosSum_) * 180.0 / M_PI;
    if (deg < 0) deg += 360.0;
    s.dir = clampTo<uint16_t>(deg, 0, 359);
    s.rain100 = clampTo<uint16_t>(rain_ * 100, 0, 65535);
    s.solar = clampTo<uint16_t>(solar_ / n, 0, 65535);
    s.uv10 = clampTo<uint16_t>(uv_ / n * 10, 0, 65535);
    s.batteryMv = clampTo<uint16_t>(battery_ * 1000, 0, 65535);
    s.lightning = clampTo<uint8_t>(lightning_, 0, 255);
    s.lightDistKm = clampTo<uint8_t>(lightDist_, 0, 255);
    s.reserved = 0;
    return s;
}
