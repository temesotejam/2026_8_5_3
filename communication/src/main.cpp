#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "production_page_ja.h"
#include <boat_protocol.h>

using namespace app_config;

namespace {

constexpr uint32_t kLinkFreshMs = 1000;
constexpr uint32_t kHeartbeatPeriodMs = 100;
constexpr uint32_t kManualRefreshMs = 200;
constexpr uint32_t kCommandRetryMs = 100;
constexpr uint32_t kCommandTimeoutMs = 1200;
constexpr uint32_t kSafetyRetryMs = 150;
constexpr uint32_t kSafetyTimeoutMs = 2000;
constexpr uint32_t kScreenPeriodMs = 200;
constexpr size_t kRxChunkBytes = 512;

enum class Stage : uint8_t {
  Idle,
  EnsureDisarmed,
  WaitModeAck,
  WaitManualAck,
  WaitArmed,
  WaitRunning,
  Running,
  Stopping,
  Emergency,
  ClearingEmergency,
  Error,
};

struct LinkCache {
  boat::ControlSnapshotPayload snapshot{};
  boat::ControlOutputPayload output{};
  boat::ActuatorStatePayload actuators{};
  boat::SystemHealthPayload health{};
  boat::ControlCommandAckPayload commandAck{};
  uint64_t lastFrameUs = 0;
  uint64_t lastSnapshotUs = 0;
  uint64_t lastOutputUs = 0;
  uint64_t lastActuatorUs = 0;
  uint64_t lastAckUs = 0;
  uint32_t frames = 0;
  bool hasSnapshot = false;
  bool hasOutput = false;
  bool hasActuators = false;
  bool hasHealth = false;
  bool hasCommandAck = false;
};

struct PendingCommand {
  boat::Type type = boat::Type::ControlModeCommand;
  uint8_t payload[sizeof(boat::ManualCommandPayload)]{};
  uint16_t length = 0;
  uint32_t requestId = 0;
  uint32_t commandSequence = 0;
  uint32_t startedMs = 0;
  uint32_t lastSendMs = 0;
  uint8_t attempts = 0;
  bool active = false;
};

HardwareSerial controlUart(1);
WebServer web(kHttpPort);
boat::Decoder controlDecoder;
TaskHandle_t rxTaskHandle = nullptr;
portMUX_TYPE cacheMux = portMUX_INITIALIZER_UNLOCKED;
LinkCache linkCache{};

uint32_t bootId = 0;
uint32_t frameSequence = 0;
uint32_t requestIdNext = 1;
uint32_t commandSequenceNext = 1;
uint32_t safetyCommandId = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastManualMs = 0;
uint32_t lastSafetyMs = 0;
uint32_t lastScreenMs = 0;
uint32_t crcErrors = 0;
uint32_t cobsErrors = 0;
uint32_t lengthErrors = 0;

Stage stage = Stage::Idle;
PendingCommand pending{};
uint32_t stageStartedMs = 0;
uint8_t selectedChannel = 0;
float selectedValue = 0.0f;
char operationMessage[96] = "停止中です。";

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

uint32_t ageMs(uint64_t timestampUs, uint64_t currentUs) {
  if (!timestampUs || currentUs < timestampUs) return UINT32_MAX;
  const uint64_t age = (currentUs - timestampUs) / 1000ULL;
  return age > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(age);
}

const char* stageName(Stage value) {
  switch (value) {
    case Stage::Idle: return "idle";
    case Stage::EnsureDisarmed: return "ensure_disarmed";
    case Stage::WaitModeAck: return "wait_mode_ack";
    case Stage::WaitManualAck: return "wait_manual_ack";
    case Stage::WaitArmed: return "wait_armed";
    case Stage::WaitRunning: return "wait_running";
    case Stage::Running: return "running";
    case Stage::Stopping: return "stopping";
    case Stage::Emergency: return "emergency";
    case Stage::ClearingEmergency: return "clearing_emergency";
    case Stage::Error: return "error";
  }
  return "unknown";
}

const char* safetyName(uint8_t value) {
  switch (value) {
    case 0: return "BOOT";
    case 1: return "DISARMED";
    case 2: return "ARMED";
    case 3: return "RUNNING";
    case 4: return "E-STOP";
    case 5: return "FAULT";
    default: return "UNKNOWN";
  }
}

void setMessage(const char* text) {
  snprintf(operationMessage, sizeof(operationMessage), "%s", text ? text : "");
}

void setStage(Stage next, const char* message) {
  stage = next;
  stageStartedMs = millis();
  lastSafetyMs = 0;
  if (message) setMessage(message);
}

LinkCache cacheSnapshot() {
  LinkCache copy{};
  portENTER_CRITICAL(&cacheMux);
  copy = linkCache;
  portEXIT_CRITICAL(&cacheMux);
  return copy;
}

bool linkConnected(const LinkCache& cache) {
  return cache.lastFrameUs && ageMs(cache.lastFrameUs, nowUs()) <= kLinkFreshMs;
}

uint8_t currentSafety(const LinkCache& cache) {
  if (cache.hasActuators) return cache.actuators.safetyState;
  if (cache.hasOutput) return cache.output.safety;
  if (cache.hasHealth) return cache.health.safetyState;
  return 0;
}

bool sendFrame(boat::Type type, const void* payload, uint16_t length) {
  boat::Header header{boat::kVersion, static_cast<uint8_t>(type), length,
                      ++frameSequence, bootId, nowUs(), 0};
  uint8_t encoded[boat::kMaxEncoded]{};
  const size_t bytes = boat::encode(
      header, static_cast<const uint8_t*>(payload), encoded, sizeof(encoded));
  return bytes && controlUart.write(encoded, bytes) == bytes;
}

void initializePersistentCounters() {
  Preferences preferences;
  preferences.begin("boatcmd2", false);
  uint32_t request = preferences.getUInt("request", 0);
  uint32_t sequence = preferences.getUInt("sequence", 0);
  if (!request) request = esp_random() | 1U;
  if (!sequence) sequence = esp_random() | 1U;
  constexpr uint32_t kReservation = 0x01000000UL;
  preferences.putUInt("request", request + kReservation);
  preferences.putUInt("sequence", sequence + kReservation);
  preferences.end();
  requestIdNext = request;
  commandSequenceNext = sequence;
}

uint8_t selectedMask() {
  if (selectedChannel == 0) return boat::ManualLeft;
  if (selectedChannel == 1) return boat::ManualRight;
  return boat::ManualRear;
}

boat::ManualCommandPayload makeManual(uint8_t mask, float value) {
  boat::ManualCommandPayload command{};
  command.protocolVersion = boat::kVersion;
  command.reserved[0] = mask;
  command.requestId = requestIdNext++;
  command.commandSequence = commandSequenceNext++;
  command.sourceUs = nowUs();
  if (mask & boat::ManualLeft) command.leftFrontWing = value;
  if (mask & boat::ManualRight) command.rightFrontWing = value;
  if (mask & boat::ManualRear) command.rearYaw = value;
  command.propulsion = 0.0f;
  command.canonicalCrc = boat::canonicalCrc(
      &command, offsetof(boat::ManualCommandPayload, canonicalCrc));
  return command;
}

void beginPending(boat::Type type, const void* payload, uint16_t length,
                  uint32_t requestId, uint32_t commandSequence) {
  pending = {};
  pending.type = type;
  pending.length = length;
  pending.requestId = requestId;
  pending.commandSequence = commandSequence;
  pending.startedMs = millis();
  pending.active = true;
  memcpy(pending.payload, payload, length);
  if (sendFrame(type, payload, length)) {
    pending.lastSendMs = millis();
    pending.attempts = 1;
  }
}

void beginModeCommand() {
  boat::ControlModeCommandPayload command{};
  command.protocolVersion = boat::kVersion;
  command.mode = 0;
  command.requestId = requestIdNext++;
  command.commandSequence = commandSequenceNext++;
  command.sourceUs = nowUs();
  command.canonicalCrc = boat::canonicalCrc(
      &command, offsetof(boat::ControlModeCommandPayload, canonicalCrc));
  beginPending(boat::Type::ControlModeCommand, &command, sizeof(command),
               command.requestId, command.commandSequence);
}

void beginManualCommand() {
  const boat::ManualCommandPayload command = makeManual(selectedMask(), selectedValue);
  beginPending(boat::Type::ManualCommand, &command, sizeof(command),
               command.requestId, command.commandSequence);
}

void sendManualRefresh() {
  const boat::ManualCommandPayload command = makeManual(selectedMask(), selectedValue);
  if (sendFrame(boat::Type::ManualCommand, &command, sizeof(command))) {
    lastManualMs = millis();
  }
}

void sendManualOff() {
  const boat::ManualCommandPayload command = makeManual(0, 0.0f);
  sendFrame(boat::Type::ManualCommand, &command, sizeof(command));
}

void sendSafety(boat::Type type) {
  boat::CommandPayload command{++safetyCommandId, static_cast<uint8_t>(type), {0, 0, 0}};
  if (sendFrame(type, &command, sizeof(command))) lastSafetyMs = millis();
}

// Returns 1 when accepted, -1 when rejected/timed out, and 0 while waiting.
int servicePending(const LinkCache& cache) {
  if (!pending.active) return -1;
  if (cache.hasCommandAck && cache.commandAck.requestId == pending.requestId &&
      cache.commandAck.commandSequence == pending.commandSequence &&
      cache.commandAck.commandType == static_cast<uint8_t>(pending.type)) {
    pending.active = false;
    if (cache.commandAck.disposition == 0 ||
        (cache.commandAck.disposition == 2 && cache.commandAck.reason == 0)) {
      return 1;
    }
    char message[96];
    snprintf(message, sizeof(message), "XIAOが指令を拒否しました（理由%u）。",
             static_cast<unsigned>(cache.commandAck.reason));
    setMessage(message);
    return -1;
  }

  const uint32_t current = millis();
  if (current - pending.startedMs > kCommandTimeoutMs) {
    pending.active = false;
    setMessage("XIAOから指令ACKが返りませんでした。");
    return -1;
  }
  if ((!pending.lastSendMs || current - pending.lastSendMs >= kCommandRetryMs) &&
      pending.attempts < 8) {
    if (sendFrame(pending.type, pending.payload, pending.length)) {
      pending.lastSendMs = current;
      ++pending.attempts;
    }
  }
  return 0;
}

void failOperation(const char* message) {
  char savedMessage[sizeof(operationMessage)];
  snprintf(savedMessage, sizeof(savedMessage), "%s", message ? message : operationMessage);
  pending.active = false;
  sendSafety(boat::Type::Stop);
  sendManualOff();
  setStage(Stage::Error, savedMessage);
}

void keepManualFresh() {
  if (!lastManualMs || millis() - lastManualMs >= kManualRefreshMs) {
    sendManualRefresh();
  }
}

void serviceOperation() {
  const LinkCache cache = cacheSnapshot();
  const bool connected = linkConnected(cache);
  const uint8_t safety = currentSafety(cache);
  const uint32_t current = millis();

  if (stage != Stage::Idle && stage != Stage::Error && !connected) {
    failOperation("XIAOとの通信が途切れたため停止指令を送りました。");
    return;
  }

  switch (stage) {
    case Stage::Idle:
      return;

    case Stage::Error:
      if (connected && safety != 1 && safety != 4 &&
          (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs)) {
        sendSafety(boat::Type::Stop);
        sendManualOff();
      }
      return;

    case Stage::EnsureDisarmed:
      if (safety == 4) {
        failOperation("緊急停止中です。解除してから開始してください。");
      } else if (safety == 1) {
        beginModeCommand();
        setStage(Stage::WaitModeAck, "手動モードを設定しています。");
      } else if (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs) {
        sendSafety(boat::Type::Stop);
        if (current - stageStartedMs > kSafetyTimeoutMs) {
          failOperation("XIAOをDISARMEDにできませんでした。");
        }
      }
      return;

    case Stage::WaitModeAck: {
      const int result = servicePending(cache);
      if (result > 0) {
        beginManualCommand();
        setStage(Stage::WaitManualAck, "手動出力値を設定しています。");
      } else if (result < 0) {
        failOperation(operationMessage);
      }
      return;
    }

    case Stage::WaitManualAck: {
      const int result = servicePending(cache);
      if (result > 0) {
        lastManualMs = current;
        sendSafety(boat::Type::Arm);
        setStage(Stage::WaitArmed, "ARMの成立を待っています。");
      } else if (result < 0) {
        failOperation(operationMessage);
      }
      return;
    }

    case Stage::WaitArmed:
      keepManualFresh();
      if (safety == 2) {
        sendSafety(boat::Type::StartTest);
        setStage(Stage::WaitRunning, "STARTの成立を待っています。");
      } else if (safety == 4) {
        failOperation("ARM中に緊急停止になりました。");
      } else if (current - stageStartedMs > kSafetyTimeoutMs) {
        failOperation("ARMできませんでした。PCA9685と配線を確認してください。");
      } else if (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs) {
        sendSafety(boat::Type::Arm);
      }
      return;

    case Stage::WaitRunning:
      keepManualFresh();
      if (safety == 3) {
        setStage(Stage::Running, "選択した1チャンネルだけを出力しています。");
      } else if (safety == 4 || safety == 5) {
        failOperation("START中に安全停止しました。");
      } else if (current - stageStartedMs > kSafetyTimeoutMs) {
        failOperation("STARTできませんでした。XIAOの状態を確認してください。");
      } else if (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs) {
        sendSafety(boat::Type::StartTest);
      }
      return;

    case Stage::Running:
      keepManualFresh();
      if (safety != 3) {
        failOperation("XIAOがRUNNINGを解除したため停止しました。");
      }
      return;

    case Stage::Stopping:
      if (safety == 1) {
        sendManualOff();
        setStage(Stage::Idle, "停止しました。すべての出力はOFFです。");
      } else if (safety == 4) {
        setStage(Stage::Emergency, "緊急停止中です。すべての出力はOFFです。");
      } else if (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs) {
        sendSafety(boat::Type::Stop);
      }
      return;

    case Stage::Emergency:
      if (safety != 4 && current - stageStartedMs > kSafetyTimeoutMs) {
        setMessage("緊急停止状態を確認できません。物理的に電源を切ってください。");
      } else if (safety != 4 && (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs)) {
        sendSafety(boat::Type::Estop);
      }
      return;

    case Stage::ClearingEmergency:
      if (safety == 1) {
        setStage(Stage::Idle, "緊急停止を解除しました。出力はOFFです。");
      } else if (current - stageStartedMs > kSafetyTimeoutMs) {
        failOperation("緊急停止を解除できませんでした。");
      } else if (!lastSafetyMs || current - lastSafetyMs >= kSafetyRetryMs) {
        sendSafety(boat::Type::ClearEstop);
      }
      return;
  }
}

void processFrame(const boat::Frame& frame) {
  const boat::Type type = static_cast<boat::Type>(frame.header.type);
  const uint64_t receivedUs = nowUs();
  portENTER_CRITICAL(&cacheMux);
  linkCache.lastFrameUs = receivedUs;
  ++linkCache.frames;
  if (type == boat::Type::ControlSnapshot &&
      frame.header.length == sizeof(linkCache.snapshot)) {
    memcpy(&linkCache.snapshot, frame.payload, sizeof(linkCache.snapshot));
    linkCache.lastSnapshotUs = receivedUs;
    linkCache.hasSnapshot = true;
  } else if (type == boat::Type::ControlOutput &&
             frame.header.length == sizeof(linkCache.output)) {
    memcpy(&linkCache.output, frame.payload, sizeof(linkCache.output));
    linkCache.lastOutputUs = receivedUs;
    linkCache.hasOutput = true;
  } else if (type == boat::Type::ActuatorState &&
             frame.header.length == sizeof(linkCache.actuators)) {
    memcpy(&linkCache.actuators, frame.payload, sizeof(linkCache.actuators));
    linkCache.lastActuatorUs = receivedUs;
    linkCache.hasActuators = true;
  } else if (type == boat::Type::SystemHealth &&
             frame.header.length == sizeof(linkCache.health)) {
    memcpy(&linkCache.health, frame.payload, sizeof(linkCache.health));
    linkCache.hasHealth = true;
  } else if (type == boat::Type::ControlCommandAck &&
             frame.header.length == sizeof(linkCache.commandAck)) {
    memcpy(&linkCache.commandAck, frame.payload, sizeof(linkCache.commandAck));
    linkCache.lastAckUs = receivedUs;
    linkCache.hasCommandAck = true;
  }
  portEXIT_CRITICAL(&cacheMux);
}

void controlRxTask(void*) {
  uint8_t bytes[kRxChunkBytes];
  for (;;) {
    const int available = controlUart.available();
    if (available <= 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    const size_t wanted = min<size_t>(static_cast<size_t>(available), sizeof(bytes));
    const size_t count = controlUart.read(bytes, wanted);
    for (size_t index = 0; index < count; ++index) {
      boat::Frame frame{};
      if (controlDecoder.feed(bytes[index], frame)) processFrame(frame);
    }
    crcErrors = controlDecoder.crcErrors;
    cobsErrors = controlDecoder.cobsErrors;
    lengthErrors = controlDecoder.lengthErrors;
  }
}

void sendHeartbeat() {
  const uint32_t current = millis();
  if (current - lastHeartbeatMs < kHeartbeatPeriodMs) return;
  const LinkCache cache = cacheSnapshot();
  // A heartbeat is proof of a healthy bidirectional link.  If CoreS3 can no
  // longer receive XIAO telemetry, stop heartbeats so XIAO's own link timeout
  // also forces every physical output off.
  if (!linkConnected(cache)) return;
  lastHeartbeatMs = current;
  boat::HeartbeatPayload heartbeat{current, frameSequence, currentSafety(cache), 0, 0};
  sendFrame(boat::Type::Heartbeat, &heartbeat, sizeof(heartbeat));
}

bool parseFloatArgument(const char* name, float& value) {
  const String source = web.arg(name);
  if (!source.length()) return false;
  char* end = nullptr;
  value = strtof(source.c_str(), &end);
  return end && *end == 0 && isfinite(value);
}

bool parseChannel(uint8_t& channel) {
  const String source = web.arg("channel");
  if (!source.length()) return false;
  char* end = nullptr;
  const unsigned long value = strtoul(source.c_str(), &end, 10);
  if (!end || *end != 0 || value > 2) return false;
  channel = static_cast<uint8_t>(value);
  return true;
}

void sendJsonResult(int code, bool accepted, const char* message) {
  char body[180];
  snprintf(body, sizeof(body), "{\"accepted\":%s,\"message\":\"%s\"}",
           accepted ? "true" : "false", message);
  web.send(code, "application/json; charset=utf-8", body);
}

void apiStart() {
  uint8_t channel = 0;
  float value = 0.0f;
  if (!parseChannel(channel) || !parseFloatArgument("value", value) ||
      value < -1.0f || value > 1.0f) {
    sendJsonResult(400, false, "CHまたは出力値が不正です。");
    return;
  }
  const LinkCache cache = cacheSnapshot();
  if (!linkConnected(cache)) {
    sendJsonResult(503, false, "XIAOと通信できていません。");
    return;
  }
  if (!cache.hasActuators || !cache.actuators.pcaReady) {
    sendJsonResult(409, false, "PCA9685が準備できていません。");
    return;
  }
  if (currentSafety(cache) == 4) {
    sendJsonResult(409, false, "緊急停止を解除してください。");
    return;
  }
  if (stage != Stage::Idle && stage != Stage::Error) {
    sendJsonResult(409, false, "別の操作を処理中です。先に停止してください。");
    return;
  }
  selectedChannel = channel;
  selectedValue = value;
  pending.active = false;
  lastManualMs = 0;
  setStage(Stage::EnsureDisarmed, "XIAOを停止状態にそろえています。");
  sendJsonResult(202, true, "開始手順をCoreS3側で実行します。");
}

void apiValue() {
  float value = 0.0f;
  if (!parseFloatArgument("value", value) || value < -1.0f || value > 1.0f) {
    sendJsonResult(400, false, "出力値が不正です。");
    return;
  }
  if (stage != Stage::Running) {
    sendJsonResult(409, false, "動作中ではありません。");
    return;
  }
  selectedValue = value;
  sendManualRefresh();
  sendJsonResult(202, true, "出力値を更新しました。");
}

void apiStop() {
  pending.active = false;
  sendSafety(boat::Type::Stop);
  sendManualOff();
  setStage(Stage::Stopping, "停止を確認しています。");
  sendJsonResult(202, true, "停止指令を送りました。");
}

void apiEstop() {
  pending.active = false;
  sendSafety(boat::Type::Estop);
  sendManualOff();
  setStage(Stage::Emergency, "緊急停止指令を送りました。");
  sendJsonResult(202, true, "緊急停止指令を送りました。");
}

void apiClearEstop() {
  const LinkCache cache = cacheSnapshot();
  if (currentSafety(cache) != 4) {
    sendJsonResult(409, false, "XIAOは緊急停止状態ではありません。");
    return;
  }
  sendSafety(boat::Type::ClearEstop);
  setStage(Stage::ClearingEmergency, "緊急停止の解除を確認しています。");
  sendJsonResult(202, true, "緊急停止解除指令を送りました。");
}

void apiStatus() {
  const LinkCache cache = cacheSnapshot();
  const bool connected = linkConnected(cache);
  const uint32_t linkAge = ageMs(cache.lastFrameUs, nowUs());
  const uint8_t safety = currentSafety(cache);
  char body[1300];
  snprintf(
      body, sizeof(body),
      "{\"connected\":%s,\"ever_received\":%s,\"age_ms\":%lu,"
      "\"operation\":\"%s\",\"message\":\"%s\",\"selected_channel\":%u,"
      "\"selected_value\":%.3f,\"control\":{\"safety\":%u,\"safety_name\":\"%s\","
      "\"mode\":%u,\"stop_reason\":%u},\"actuators\":{\"pca_ready\":%u,"
      "\"outputs_enabled\":%u,\"enabled_mask\":%u,\"left_us\":%u,"
      "\"right_us\":%u,\"rear_us\":%u,\"relay\":%u},"
      "\"sensors\":{\"imu_valid\":%u,\"roll_rad\":%.4f,\"pitch_rad\":%.4f,"
      "\"yaw_rad\":%.4f,\"tof_valid\":%u,\"tof_m\":%.3f},"
      "\"link\":{\"frames\":%lu,\"crc_errors\":%lu,\"cobs_errors\":%lu,"
      "\"length_errors\":%lu}}",
      connected ? "true" : "false", cache.lastFrameUs ? "true" : "false",
      static_cast<unsigned long>(linkAge), stageName(stage), operationMessage,
      static_cast<unsigned>(selectedChannel), selectedValue,
      static_cast<unsigned>(safety), safetyName(safety),
      static_cast<unsigned>(cache.hasSnapshot ? cache.snapshot.mode : 0),
      static_cast<unsigned>(cache.hasOutput ? cache.output.stopReason : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.pcaReady : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.outputsEnabled : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.enabledMask : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.leftPulseUs : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.rightPulseUs : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.rearPulseUs : 0),
      static_cast<unsigned>(cache.hasActuators ? cache.actuators.motorRelayEnabled : 0),
      static_cast<unsigned>(cache.hasSnapshot ? cache.snapshot.imuValid : 0),
      cache.hasSnapshot ? cache.snapshot.rollRad : 0.0f,
      cache.hasSnapshot ? cache.snapshot.pitchRad : 0.0f,
      cache.hasSnapshot ? cache.snapshot.yawRad : 0.0f,
      static_cast<unsigned>(cache.hasSnapshot ? cache.snapshot.tofValid : 0),
      cache.hasSnapshot ? cache.snapshot.tofFilteredM : 0.0f,
      static_cast<unsigned long>(cache.frames), static_cast<unsigned long>(crcErrors),
      static_cast<unsigned long>(cobsErrors), static_cast<unsigned long>(lengthErrors));
  web.send(200, "application/json; charset=utf-8", body);
}

void startWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(kApSsid, kApPassword);
  web.on("/", HTTP_GET, [] { web.send(200, "text/html; charset=utf-8", productionPageJapanese); });
  web.on("/api/status", HTTP_GET, apiStatus);
  web.on("/api/start", HTTP_POST, apiStart);
  web.on("/api/value", HTTP_POST, apiValue);
  web.on("/api/stop", HTTP_POST, apiStop);
  web.on("/api/estop", HTTP_POST, apiEstop);
  web.on("/api/clear-estop", HTTP_POST, apiClearEstop);
  web.onNotFound([] { web.send(404, "application/json", "{\"error\":\"not_found\"}"); });
  web.begin();
}

void drawScreen() {
  const LinkCache cache = cacheSnapshot();
  const bool connected = linkConnected(cache);
  const uint8_t safety = currentSafety(cache);
  M5.Display.fillScreen(0x0000);
  M5.Display.setTextColor(0xFFFF, 0x0000);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(6, 6);
  M5.Display.printf("CORES3 MANUAL 2.0\n");
  M5.Display.printf("LINK %s  age %lu ms  frames %lu\n",
                    connected ? "OK" : "WAIT",
                    static_cast<unsigned long>(ageMs(cache.lastFrameUs, nowUs())),
                    static_cast<unsigned long>(cache.frames));
  M5.Display.printf("XIAO %s  CORE %s\n", safetyName(safety), stageName(stage));
  M5.Display.printf("PCA %s  mask 0x%02X  relay %u\n",
                    cache.hasActuators && cache.actuators.pcaReady ? "OK" : "WAIT",
                    cache.hasActuators ? cache.actuators.enabledMask : 0,
                    cache.hasActuators ? cache.actuators.motorRelayEnabled : 0);
  M5.Display.printf("PWM %u / %u / %u us\n",
                    cache.hasActuators ? cache.actuators.leftPulseUs : 0,
                    cache.hasActuators ? cache.actuators.rightPulseUs : 0,
                    cache.hasActuators ? cache.actuators.rearPulseUs : 0);
  M5.Display.printf("RPY %.2f %.2f %.2f rad\n",
                    cache.hasSnapshot ? cache.snapshot.rollRad : 0.0f,
                    cache.hasSnapshot ? cache.snapshot.pitchRad : 0.0f,
                    cache.hasSnapshot ? cache.snapshot.yawRad : 0.0f);
  M5.Display.printf("ToF %.3f m  AP %s\n",
                    cache.hasSnapshot ? cache.snapshot.tofFilteredM : 0.0f,
                    WiFi.softAPIP().toString().c_str());
  M5.Display.printf("UART err C/C/L %lu/%lu/%lu\n",
                    static_cast<unsigned long>(crcErrors),
                    static_cast<unsigned long>(cobsErrors),
                    static_cast<unsigned long>(lengthErrors));
}

}  // namespace

void setup() {
  auto config = M5.config();
  config.serial_baudrate = 115200;
  M5.begin(config);
  bootId = esp_random();
  if (!bootId) bootId = 1;
  frameSequence = esp_random();
  safetyCommandId = esp_random();
  initializePersistentCounters();

  controlUart.setRxBufferSize(16384);
  controlUart.setTimeout(2);
  controlUart.begin(kControlUartBaud, SERIAL_8N1, kControlUartRxPin, kControlUartTxPin);
  xTaskCreatePinnedToCore(controlRxTask, "ControlRx", 6144, nullptr, 3, &rxTaskHandle, 1);
  startWeb();
  drawScreen();

  Serial.printf("%s %s RX=%d TX=%d baud=%lu AP=%s URL=http://%s/\n",
                kFirmwareName, kFirmwareVersion, kControlUartRxPin, kControlUartTxPin,
                static_cast<unsigned long>(kControlUartBaud), kApSsid,
                WiFi.softAPIP().toString().c_str());
}

void loop() {
  M5.update();
  web.handleClient();
  sendHeartbeat();
  serviceOperation();
  if (millis() - lastScreenMs >= kScreenPeriodMs) {
    lastScreenMs = millis();
    drawScreen();
  }
  delay(1);
}
