#pragma once
#include <Arduino.h>
namespace app_config {
constexpr char kFirmwareName[] = "cores3-telemetry-bridge";
constexpr char kFirmwareVersion[] = "3.0.0-auto-select";
// CoreS3 Port C: GNSS TX connects to GPIO18 (CoreS3 RX). GPIO17 is reserved as TX.
constexpr int kGnssRxPin = 18, kGnssTxPin = 17;
// CoreS3 Port B: GPIO8 is receiver and GPIO9 is transmitter for the control XIAO link.
constexpr int kControlUartRxPin = 8, kControlUartTxPin = 9;
constexpr uint32_t kControlUartBaud = 921600UL;
constexpr uint32_t kGnssBaud = 115200UL;
constexpr uint16_t kGnssUartRxBufferBytes = 2048, kGnssReadBudgetBytes = 512;
constexpr uint16_t kGnssInputLineChars = 127, kGnssMaxSentenceChars = 110;
constexpr uint32_t kGnssSentenceTimeoutMs = 500UL, kGnssNoDataTimeoutMs = 1500UL;
constexpr uint32_t kGnssNavIntervalMs = 100UL;
constexpr char kApSsid[] = "BOAT-CONTROL", kApPassword[] = "12345678";
constexpr uint16_t kHttpPort = 80;
}
