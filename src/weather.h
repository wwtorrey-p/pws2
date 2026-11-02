#pragma once
#include <cstdint>
#include <functional>

#include "aggregate.h"

namespace weather {

struct Current {
    bool haveObs = false;
    Obs obs;
    uint32_t obsAgeS = 0;
    bool haveWind = false;
    float windSpeed = 0, windDir = 0;  // rapid_wind, m/s and degrees
    uint32_t windAgeS = 0;
    bool haveHub = false;
    int hubRssi = 0;
    char serial[24] = {};
};

// Opens the history partition and starts the UDP listener task.
bool begin();
Current current();
// Seconds since the last packet of any kind; UINT32_MAX if none has arrived.
uint32_t lastPacketAgeS();

// Calls fn for each stored sample at or after sinceEpoch, oldest first. The lock is dropped between batches.
void forEachSample(uint32_t sinceEpoch, const std::function<bool(const Sample&)>& fn);

}  // namespace weather
