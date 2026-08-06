#include "sd_logger.h"

#include <SD.h>
#include <SPI.h>
#include <esp_timer.h>

#include <bin_record_serializer.h>

#include "app_config.h"

namespace sd_logging {
namespace {

constexpr uint16_t kInboundLogFlag = 0x4000;
constexpr uint16_t kOutboundLogFlag = 0x8000;
constexpr uint16_t kDirectionLogFlags = kInboundLogFlag | kOutboundLogFlag;
constexpr uint16_t kQueueDepth = 64;
constexpr size_t kWriteBufferBytes = 8192;
constexpr size_t kWriteChunkBytes = 512;
constexpr uint32_t kFlushPeriodMs = 1000;

struct QueuedRecord {
  boat::Header header{};
  uint8_t payload[boat::kMaxPayload]{};
  uint64_t loggedUs = 0;
};

File logFile;
QueuedRecord recordQueue[kQueueDepth];
uint8_t writeBuffer[kWriteBufferBytes];
size_t writeUsed = 0;
uint16_t queueHead = 0;
uint16_t queueTail = 0;
uint16_t queueUsed = 0;
Status loggerStatus{};
portMUX_TYPE loggerMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t writerTaskHandle = nullptr;
volatile bool flushRequested = false;
uint32_t lastFlushMs = 0;

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

void setFault() {
  portENTER_CRITICAL(&loggerMux);
  loggerStatus.active = false;
  loggerStatus.fault = true;
  ++loggerStatus.writeErrors;
  portEXIT_CRITICAL(&loggerMux);
}

bool writeAll(const uint8_t* data, size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = min(kWriteChunkBytes, length - offset);
    const size_t written = logFile.write(data + offset, chunk);
    if (written != chunk) {
      setFault();
      return false;
    }
    offset += written;
  }
  return true;
}

bool commitBuffer() {
  if (!writeUsed) return true;
  const bool ok = writeAll(writeBuffer, writeUsed);
  writeUsed = 0;
  return ok;
}

bool appendBytes(const uint8_t* data, size_t length) {
  while (length) {
    const size_t available = sizeof(writeBuffer) - writeUsed;
    const size_t copied = min(available, length);
    memcpy(writeBuffer + writeUsed, data, copied);
    writeUsed += copied;
    data += copied;
    length -= copied;
    if (writeUsed == sizeof(writeBuffer) && !commitBuffer()) return false;
  }
  return true;
}

bool enqueue(const boat::Header& sourceHeader, const uint8_t* payload,
             uint64_t loggedUs, uint16_t directionFlag) {
  if (sourceHeader.length > boat::kMaxPayload || (!payload && sourceHeader.length)) {
    return false;
  }

  portENTER_CRITICAL(&loggerMux);
  if (!loggerStatus.active) {
    portEXIT_CRITICAL(&loggerMux);
    return false;
  }
  if (queueUsed >= kQueueDepth) {
    ++loggerStatus.dropped;
    portEXIT_CRITICAL(&loggerMux);
    return false;
  }

  QueuedRecord& queued = recordQueue[queueHead];
  queued.header = sourceHeader;
  queued.header.flags = static_cast<uint16_t>(
      (queued.header.flags & ~kDirectionLogFlags) | directionFlag);
  queued.loggedUs = loggedUs;
  if (queued.header.length) memcpy(queued.payload, payload, queued.header.length);
  queueHead = static_cast<uint16_t>((queueHead + 1) % kQueueDepth);
  ++queueUsed;
  loggerStatus.queueDepth = queueUsed;
  if (queueUsed > loggerStatus.queueHighWater) loggerStatus.queueHighWater = queueUsed;
  portEXIT_CRITICAL(&loggerMux);

  if (writerTaskHandle) xTaskNotifyGive(writerTaskHandle);
  return true;
}

bool dequeue(QueuedRecord& queued) {
  portENTER_CRITICAL(&loggerMux);
  if (!queueUsed) {
    portEXIT_CRITICAL(&loggerMux);
    return false;
  }
  queued = recordQueue[queueTail];
  queueTail = static_cast<uint16_t>((queueTail + 1) % kQueueDepth);
  --queueUsed;
  loggerStatus.queueDepth = queueUsed;
  portEXIT_CRITICAL(&loggerMux);
  return true;
}

bool appendRecord(const QueuedRecord& queued) {
  uint8_t encoded[boat_bin::kMaxRecordBytes]{};
  size_t encodedBytes = 0;
  if (!boat_bin::serializeRecord(queued.header, queued.loggedUs, queued.payload,
                                 queued.header.length, encoded, sizeof(encoded),
                                 encodedBytes)) {
    setFault();
    return false;
  }
  if (!appendBytes(encoded, encodedBytes)) return false;
  portENTER_CRITICAL(&loggerMux);
  ++loggerStatus.records;
  portEXIT_CRITICAL(&loggerMux);
  return true;
}

void flushFile() {
  if (!loggerStatus.active || !logFile) return;
  if (!commitBuffer()) return;
  logFile.flush();
  lastFlushMs = millis();
}

void writerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    QueuedRecord queued{};
    uint16_t processed = 0;
    while (processed < 32 && dequeue(queued)) {
      if (!appendRecord(queued)) break;
      ++processed;
    }
    const uint32_t current = millis();
    if (flushRequested || current - lastFlushMs >= kFlushPeriodMs) {
      flushRequested = false;
      flushFile();
    }
    if (queueUsed) xTaskNotifyGive(writerTaskHandle);
  }
}

bool openNextRunFile() {
  for (uint16_t number = 1; number < 10000; ++number) {
    char fileName[16];
    char path[40];
    snprintf(fileName, sizeof(fileName), "RUN%04u.BIN", number);
    snprintf(path, sizeof(path), "%s/%s", app_config::kLogDirectory, fileName);
    if (SD.exists(path)) continue;
    logFile = SD.open(path, FILE_WRITE);
    if (!logFile) return false;
    portENTER_CRITICAL(&loggerMux);
    snprintf(loggerStatus.fileName, sizeof(loggerStatus.fileName), "%s", fileName);
    portEXIT_CRITICAL(&loggerMux);
    return true;
  }
  return false;
}

}  // namespace

bool begin() {
  SPI.begin(app_config::kSdSckPin, app_config::kSdMisoPin,
            app_config::kSdMosiPin, app_config::kSdCsPin);
  const bool cardReady = SD.begin(app_config::kSdCsPin, SPI, app_config::kSdFrequencyHz);
  portENTER_CRITICAL(&loggerMux);
  loggerStatus = {};
  loggerStatus.cardReady = cardReady;
  portEXIT_CRITICAL(&loggerMux);
  if (!cardReady) return false;
  if (!SD.exists(app_config::kLogDirectory) && !SD.mkdir(app_config::kLogDirectory)) {
    setFault();
    return false;
  }
  if (!openNextRunFile()) {
    setFault();
    return false;
  }

  portENTER_CRITICAL(&loggerMux);
  loggerStatus.active = true;
  loggerStatus.fault = false;
  portEXIT_CRITICAL(&loggerMux);
  lastFlushMs = millis();
  const BaseType_t created = xTaskCreatePinnedToCore(
      writerTask, "SdWriter", 8192, nullptr, 1, &writerTaskHandle, 1);
  if (created != pdPASS) {
    setFault();
    logFile.close();
    return false;
  }
  return true;
}

bool recordInbound(const boat::Frame& frame, uint64_t receivedUs) {
  return enqueue(frame.header, frame.payload, receivedUs, kInboundLogFlag);
}

bool recordOutbound(const boat::Header& header, const void* payload, uint64_t loggedUs) {
  return enqueue(header, static_cast<const uint8_t*>(payload), loggedUs, kOutboundLogFlag);
}

void requestFlush() {
  flushRequested = true;
  if (writerTaskHandle) xTaskNotifyGive(writerTaskHandle);
}

Status status() {
  Status copy{};
  portENTER_CRITICAL(&loggerMux);
  copy = loggerStatus;
  portEXIT_CRITICAL(&loggerMux);
  return copy;
}

}  // namespace sd_logging
