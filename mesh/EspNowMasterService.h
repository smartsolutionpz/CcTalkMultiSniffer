// Scopo del file:
// dichiara `EspNowMasterService`, il servizio mesh locale che invia heartbeat
// broadcast tramite ESP-NOW.
#ifndef CCTALK_MULTI_SNIFFER_MESH_ESP_NOW_MASTER_SERVICE_H
#define CCTALK_MULTI_SNIFFER_MESH_ESP_NOW_MASTER_SERVICE_H

#include <Arduino.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_now.h>
#endif

namespace ccms {

// Servizio ESP-NOW in ruolo "master broadcaster".
// In questa fase del progetto invia heartbeat broadcast per telemetria locale.
class EspNowMasterService {
public:
  typedef void (*LogHook)(const char* line, void* userData);

  bool begin(bool enabled);
  void loop();
  void setHeartbeatIntervalMs(uint32_t heartbeatIntervalMs);
  void setLogHook(LogHook hook, void* userData);

  bool enabled() const { return _enabled; }
  bool ready() const { return _ready; }
  uint32_t rxCount() const { return _rxCount; }
  uint32_t txCount() const { return _txCount; }
  uint32_t txFailCount() const { return _txFailCount; }

private:
  // Pacchetto broadcast minimo inviato periodicamente.
  struct __attribute__((packed)) HeartbeatPacket {
    uint8_t version;
    uint8_t msgType;
    uint16_t reserved;
    uint32_t seq;
    uint32_t uptimeMs;
  };

  void logLine(const char* line);
  bool sendHeartbeat();
#if defined(ARDUINO_ARCH_ESP32)
  void onSendInternal(const uint8_t* macAddr, esp_now_send_status_t status);
  static void onSendStatic(const uint8_t* macAddr, esp_now_send_status_t status);
#else
  void onSendInternal(const uint8_t* macAddr, int status);
  static void onSendStatic(const uint8_t* macAddr, int status);
#endif

  static EspNowMasterService* s_instance;

  bool _enabled = false;
  bool _started = false;
  bool _ready = false;
  uint32_t _heartbeatIntervalMs = 1000;
  uint32_t _lastHeartbeatMs = 0;
  uint32_t _txSeq = 0;
  uint32_t _rxCount = 0;
  uint32_t _txCount = 0;
  uint32_t _txFailCount = 0;
  uint32_t _lastTxErrorLogMs = 0;

  LogHook _logHook = nullptr;
  void* _logHookUserData = nullptr;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_MESH_ESP_NOW_MASTER_SERVICE_H
