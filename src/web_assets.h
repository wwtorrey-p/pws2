#pragma once
#include <cstddef>

struct WebAsset {
    const char* path;
    const char* mime;
    const unsigned char* data;
    size_t size;
};

// data holds the gzip-compressed file. Defined in src/generated/web_assets.cpp, which tools/build_web.py writes from web/.
extern const WebAsset kWebAssets[];
extern const size_t kWebAssetCount;
