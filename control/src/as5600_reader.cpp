#include "as5600_reader.h"

#include <Wire.h>
#include <esp_timer.h>

#include "app_config.h"

namespace {
constexpr uint8_t kStatus = 0x0B;
constexpr uint8_t kRawAngleHigh = 0x0C;
constexpr float kTwoPi = 6.28318530717958647692f;
}

bool As5600Reader::read8(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(app_config::kAs5600Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(static_cast<int>(app_config::kAs5600Address), 1) != 1) {
    ++errors_;
    return false;
  }
  value = Wire.read();
  return true;
}

bool As5600Reader::read16(uint8_t reg, uint16_t& value) {
  Wire.beginTransmission(app_config::kAs5600Address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(static_cast<int>(app_config::kAs5600Address), 2) != 2) {
    ++errors_;
    return false;
  }
  value = (static_cast<uint16_t>(Wire.read()) << 8) | Wire.read();
  return true;
}

bool As5600Reader::begin() {
  uint8_t status = 0;
  uint16_t raw = 0;
  if (!read8(kStatus, status) || !read16(kRawAngleHigh, raw)) return false;
  if ((status & 0x20u) == 0) return false;
  previousRaw_ = raw & 0x0FFFu;
  previousUs_ = static_cast<uint64_t>(esp_timer_get_time());
  filteredRpm_ = 0.0f;
  havePrevious_ = true;
  return true;
}

bool As5600Reader::read(As5600Sample& sample) {
  sample = {};
  uint8_t status = 0;
  uint16_t raw = 0;
  const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
  if (!read8(kStatus, status) || !read16(kRawAngleHigh, raw)) return false;
  raw &= 0x0FFFu;
  sample.timestampUs = now;
  sample.rawAngle = raw;
  sample.angleRad = raw * (kTwoPi / 4096.0f);
  sample.magnetDetected = (status & 0x20u) != 0;
  sample.valid = sample.magnetDetected;
  if (havePrevious_ && now > previousUs_) {
    int32_t delta = static_cast<int32_t>(raw) - static_cast<int32_t>(previousRaw_);
    if (delta > 2048) delta -= 4096;
    if (delta < -2048) delta += 4096;
    const float dt = (now - previousUs_) * 1.0e-6f;
    const float rpm = (static_cast<float>(delta) / 4096.0f) * 60.0f /
                      dt / app_config::kAs5600GearRatio;
    filteredRpm_ += 0.25f * (rpm - filteredRpm_);
    sample.rpm = filteredRpm_;
  }
  previousRaw_ = raw;
  previousUs_ = now;
  havePrevious_ = true;
  return sample.valid;
}
