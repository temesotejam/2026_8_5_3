#pragma once

#include <Arduino.h>

#include <boat_protocol.h>

namespace sd_logging {

struct Status {
  bool cardReady = false;
  bool active = false;
  bool fault = false;
  char fileName[16] = "none";
  uint32_t records = 0;
  uint32_t dropped = 0;
  uint32_t writeErrors = 0;
  uint16_t queueDepth = 0;
  uint16_t queueHighWater = 0;
};

// Initializes the CoreS3 built-in microSD and automatically opens RUNnnnn.BIN.
bool begin();
bool recordInbound(const boat::Frame& frame, uint64_t receivedUs);
bool recordOutbound(const boat::Header& header, const void* payload, uint64_t loggedUs);
void requestFlush();
Status status();

}  // namespace sd_logging
