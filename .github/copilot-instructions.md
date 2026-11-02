# Copilot instructions

ESP32-S3 (ESP-IDF via PlatformIO) weather station for a Tempest hub. Read `README.md` first: it has the architecture, UDP packet fields, history ring layout, HTTP API and open items.

- Build `pio run -e esp32s3`; flash `pio run -e esp32s3 -t upload`; host tests `pio test -e native` (needs g++).
- The UI is plain HTML/CSS/JS in `web/` (no framework, no Node). It is gzipped into `src/generated/web_assets.cpp` by `tools/build_web.py`. Never edit the generated file.
- Keep `src/aggregate.*` free of ESP-IDF includes so it stays host-testable.
- `Sample` must stay 32 bytes. Changing it invalidates stored history.
- Reference projects: `..\water-pump-monitor` (build environment, Wi-Fi, HTTPS, OTA) and `..\pws` (Go app that the UI look and feel is copied from).
- Code comments only where something needs clarification.
