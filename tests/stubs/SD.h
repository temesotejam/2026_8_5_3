#pragma once

#include <stddef.h>
#include <stdint.h>

#include "SPI.h"

constexpr const char* FILE_WRITE = "w";

class File {
 public:
  explicit operator bool() const { return true; }
  size_t write(const uint8_t*, size_t length) { return length; }
  void flush() {}
  void close() {}
};

class SDClass {
 public:
  bool begin(int, SPIClass&, uint32_t) { return true; }
  bool exists(const char*) const { return false; }
  bool mkdir(const char*) { return true; }
  File open(const char*, const char*) { return {}; }
};

extern SDClass SD;
