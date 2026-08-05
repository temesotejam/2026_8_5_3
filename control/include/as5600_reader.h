#pragma once

#include <Arduino.h>

struct As5600Sample {
  uint64_t timestampUs = 0;
  uint16_t rawAngle = 0;
  float angleRad = NAN;
  float rpm = NAN;
  bool magnetDetected = false;
  bool valid = false;
};

class As5600Reader {
 public:
  bool begin();
  bool read(As5600Sample& sample);
  uint32_t errors() const { return errors_; }

 private:
  bool read8(uint8_t reg, uint8_t& value);
  bool read16(uint8_t reg, uint16_t& value);

  bool havePrevious_ = false;
  uint16_t previousRaw_ = 0;
  uint64_t previousUs_ = 0;
  float filteredRpm_ = 0.0f;
  uint32_t errors_ = 0;
};
