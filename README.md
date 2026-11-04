# Weather Station

ESP32-S3 firmware and web UI that shows a WeatherFlow Tempest station's data, with no cloud involved. The device listens for the hub's local UDP broadcasts, keeps recent history in flash, and serves a dashboard over HTTPS with a login.

The build environment is copied from `..\water-pump-monitor`. The look and feel of the UI is copied from `..\pws\cmd\udp-dashboard` (a Go + React/Grommet app), reimplemented in plain HTML/CSS/JS.

Status: the firmware builds. It has not yet been flashed or run against a real hub, and the host unit tests have not been run because no `g++` was installed on the dev machine.

## Hardware

Lonely Binary ESP32-S3 N16R8, 16 MB flash. No external I/O. Status LED is the on-board WS2812 on GPIO 48 (dim): green = receiving, amber = packets delayed (>180 s), red = Wi-Fi offline, purple = Wi-Fi setup. The BOOT button (GPIO 0), held for 2 s, re-opens Wi-Fi setup.

## Build, flash, test

```
pio run -e esp32s3                 # build
pio run -e esp32s3 -t upload       # flash over the CH340 port (COM5)
pio device monitor                 # serial log and Wi-Fi setup prompt
pio test -e native                 # host unit tests (needs g++, e.g. MinGW)
```

`tools/build_web.py` runs before every firmware build. It gzips `web/index.html`, `web/app.js` and `web/style.css` into `src/generated/web_assets.cpp` (git-ignored), which holds them as C arrays. If local TLS files are present, it also embeds those into generated C++ assets. The server sends the web assets with `Content-Encoding: gzip`. There is no Node step.

Wi-Fi setup: on first boot the device prompts for the network name and password on the serial port. Credentials are only saved once a trial connection gets an IP address.

### Trusted local HTTPS

The default certificate is self-signed and causes a browser warning. To use a certificate trusted by your browser for `https://192.168.1.73/`, first reserve that address for the station in your router, then create and trust a local CA certificate on Windows:

```powershell
go run .\tools\create_local_cert.go -ip 192.168.1.73
certutil -user -addstore Root .\certs\local-ca.crt
pio run -e esp32s3
pio run -e esp32s3 -t upload
```

The build embeds `certs/server.crt` and `certs/server.key` when both files exist; otherwise it retains the default generated certificate. The generated `certs/` files are ignored by Git. The local CA private key stays in your Windows user configuration directory, while the firmware contains the server's private key. To trust the station from another device, securely copy `certs/local-ca.crt` to it and install that certificate as a trusted root there. Do not share the CA private key.

## Architecture

| File | Role |
| --- | --- |
| `src/main.cpp` | Startup, status LED, BOOT button, OTA rollback confirmation |
| `src/wifi.*` | Wi-Fi station and serial provisioning (from water-pump-monitor) |
| `src/weather.*` | UDP listener task, current-conditions state, flash history ring |
| `src/aggregate.*` | Pure logic: averages observations into 5-minute `Sample`s (unit-tested) |
| `src/auth.*` | Configurable single account, salted SHA-256 password, stored in NVS |
| `src/web.*` | HTTPS server, JSON API, static assets, firmware upload |
| `src/cert.*` | Embedded local-trust certificate, or self-signed ECDSA certificate generated on first boot and kept in NVS |
| `src/storage.h` | NVS blob helpers (namespace `wx`) |
| `web/` | The UI |

### Tempest UDP packets (port 50222, JSON)

- `obs_st`: `obs[0]` is an array. Indexes: 0 epoch, 1 lull, 2 avg wind, 3 gust (m/s), 4 direction, 6 pressure (mb), 7 temperature (C), 8 humidity (%), 9 lux, 10 UV, 11 solar (W/m2), 12 rain over the interval (mm), 13 precipitation type, 14 lightning distance (km), 15 lightning count, 16 battery (V), 17 interval (min).
- `rapid_wind`: `ob` is `[epoch, speed m/s, direction]`, sent about every 3 s.
- `hub_status` / `device_status`: only `rssi` is used.

### History ring

`Aggregator` closes a 5-minute bucket when an observation from a later bucket arrives, and `weather.cpp` appends the 32-byte `Sample` to the `history` partition (80 KB, about 2560 slots, about 9 days). Writing into the first slot of a 4 KB sector erases that sector, which drops its 128 oldest samples. On boot the ring is read back and the write head is placed after the newest valid sample. Records with a time before 2023-11 (or erased) are treated as empty.

If `Sample` changes size or layout, bump the data format, because old records would be misread. The `static_assert` keeps it at 32 bytes so a record never straddles a sector.

### HTTP API (all except static files need HTTP Basic authentication)

- `GET /api/current`: latest observation, live wind, hub RSSI, device info, `defaultPassword` flag.
- `GET /api/history?hours=N` (1 to 336, default 24): `{"step":300,"rows":[[epoch, temp*100, hum*10, press*10, wind*100, gust*100, dir, rain*100, solar, uv*10, lux, mV, lightning], ...]}`, oldest first, chunked.
- `POST /api/password` with `{"username": "...", "password": "..."}`. Usernames must be 1-32 printable ASCII characters without `:`; passwords must be 6-64 characters and cannot be `admin`. For compatibility, omitting `username` keeps the current username.
- `POST /api/firmware` with the raw `.bin` as the body. The image is checked against this project's name and the device restarts into it. A new image must be online and serving within 30 s to be confirmed, or it rolls back.

## Open items

- Flash it and verify against a real hub (check that the broadcasts are received with power save off, and that the history survives a reboot).
- Run `pio test -e native` once a C++ compiler is installed.
- The UI has not been viewed in a browser. Check the charts, tooltips and unit switching, especially layout on a phone.
- The first 24 hours after a fresh flash show partial charts; the history is empty until the first 5-minute bucket closes.
- Possible additions: a per-day min/max summary, a rain-rate display, an option to keep the clock-less fallback timestamps, and bigger or longer history by growing the `history` partition (there is free flash after 0x634000).
