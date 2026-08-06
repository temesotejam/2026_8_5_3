#pragma once
#include <Arduino.h>
namespace app_config {
constexpr char kFirmwareName[] = "cores3-telemetry-bridge";
constexpr char kFirmwareVersion[] = "2.0.0-manual-minimal";
// CoreS3 Port B: GPIO8 is receiver and GPIO9 is transmitter for the control XIAO link.
constexpr int kControlUartRxPin = 8, kControlUartTxPin = 9;
constexpr uint32_t kControlUartBaud = 921600UL;
constexpr char kApSsid[] = "BOAT-CONTROL", kApPassword[] = "12345678";
constexpr uint16_t kHttpPort = 80;
}
