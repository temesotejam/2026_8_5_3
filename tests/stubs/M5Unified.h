#pragma once

#include <Arduino.h>

struct M5Config { uint32_t serial_baudrate=0; };
class M5Display {
 public:
  void fillScreen(uint32_t){}
  void setTextColor(uint32_t,uint32_t){}
  void setTextSize(int){}
  void setCursor(int,int){}
  int printf(const char*,...) __attribute__((format(printf,2,3)));
};
class M5UnifiedStub {
 public:
  M5Display Display{};
  M5Config config(){return {};}
  void begin(const M5Config&){}
  void update(){}
};
extern M5UnifiedStub M5;
