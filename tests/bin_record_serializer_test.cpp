#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "bin_record_serializer.h"

namespace {

uint32_t readLe32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8) |
         (static_cast<uint32_t>(source[2]) << 16) |
         (static_cast<uint32_t>(source[3]) << 24);
}

}  // namespace

int main() {
  const uint8_t payload[] = {0x11, 0x22, 0x33, 0x44};
  boat::Header header{boat::kVersion, static_cast<uint8_t>(boat::Type::Heartbeat),
                      sizeof(payload), 123, 456, 789, 0x8000};
  uint8_t output[boat_bin::kMaxRecordBytes]{};
  size_t written = 0;
  assert(boat_bin::serializeRecord(header, 987654321ULL, payload, sizeof(payload),
                                   output, sizeof(output), written));
  assert(written == boat_bin::kPrefixBytes + sizeof(payload));
  assert(readLe32(output) == boat_bin::kMagic);
  assert(output[12] == boat::kVersion);
  assert(output[13] == static_cast<uint8_t>(boat::Type::Heartbeat));
  assert(std::memcmp(output + boat_bin::kPrefixBytes, payload, sizeof(payload)) == 0);

  size_t rejectedBytes = 99;
  assert(!boat_bin::serializeRecord(header, 0, payload, sizeof(payload) - 1,
                                    output, sizeof(output), rejectedBytes));
  assert(rejectedBytes == 0);
  assert(!boat_bin::serializeRecord(header, 0, payload, sizeof(payload), output,
                                    boat_bin::kPrefixBytes, rejectedBytes));
  std::cout << "binary log serializer tests passed\n";
}
