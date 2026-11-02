#pragma once

namespace wifi {

// Brings up the Wi-Fi driver. Call once, after NVS is initialised.
void init();
bool hasCredentials();
// Starts joining the saved network. Returns at once and retries for as long as the device runs.
void connectStored();
// True while the device has an IP address.
bool connected();
// True while the serial setup prompt is open.
bool provisioning();
// Opens the serial setup prompt in its own task. No-op if it is already open.
void startProvisioning();

}  // namespace wifi
