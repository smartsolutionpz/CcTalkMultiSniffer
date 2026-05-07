// Scopo del file:
// dichiara `WifiService`, il servizio che gestisce la connettivita Wi-Fi.
// RUN usa solo la station verso il router; PROG usa solo l'AP locale.
#ifndef CCTALK_MULTI_SNIFFER_NET_WIFI_SERVICE_H
#define CCTALK_MULTI_SNIFFER_NET_WIFI_SERVICE_H

#include <Arduino.h>
#include <WiFi.h>
#include <stdint.h>
#include <time.h>

#include "../status/RingLog.h"

namespace ccms {

class WifiService {
public:
  typedef void (*LogHook)(const char* line, void* userData);
  static const uint8_t kMaxScannedNetworks = 16;

  struct ScannedNetwork {
    char ssid[33];
    int32_t rssi;
    uint8_t encryption;
    bool hidden;
  };

  struct ClockInfo {
    bool valid = false;
    bool syncedFromInternet = false;
    uint64_t unixTime = 0;
    char iso8601[32] = {0};
    char display[24] = {0};
  };

  WifiService();

  void setCredentials(const char* ssid, const char* pass);
  void setHostname(const char* hostname);
  void setTimings(uint32_t connectTimeoutMs, uint32_t retryIntervalMs);

  bool begin();
  bool beginApOnly();
  bool isConnected() const;
  String ip() const;
  int rssi() const;
  bool isApFallbackActive() const;

  void loop();

  bool enabled() const;
  wl_status_t status() const;
  const char* statusText() const;
  void ipToString(char* out, size_t outLen) const;
  String apSsid() const;
  String connectedSsid() const;
  uint8_t scanNetworks(ScannedNetwork* out, uint8_t maxCount);
  void reconnect();
  bool getClockInfo(ClockInfo& out) const;
  void noteDisconnectReason(uint16_t reason);

  void setLogHook(LogHook hook, void* userData);

  const RingLog& ringLog() const { return _ringLog; }

private:
  void attachWifiEvents();
  void configureCommonRadio();
  void enforcePerformanceMode(bool logResult);
  void logRuntimeConfig(const char* mode);
  void logStatusIfChanged(wl_status_t st, bool force);
  void logPendingDisconnectReason();
  void startConnectAttempt();
  bool startProgAccessPoint();
  void requestClockSync();
  void logLine(const char* line);
  void logConnectedInfo();

  const char* _ssid = nullptr;
  const char* _pass = nullptr;
  const char* _hostname = nullptr;
  uint32_t _connectTimeoutMs = 15000;
  uint32_t _retryIntervalMs = 30000;

  bool _enabled = false;
  bool _started = false;
  bool _apOnlyMode = false;
  bool _connecting = false;
  bool _wasConnected = false;
  bool _apActive = false;
  bool _clockSyncRequested = false;
  bool _clockSyncedFromInternet = false;
  bool _eventsAttached = false;
  bool _lastLoggedStatusValid = false;
  wl_status_t _lastLoggedStatus = WL_IDLE_STATUS;
  uint32_t _attemptStartMs = 0;
  uint32_t _lastAttemptMs = 0;
  uint32_t _lastClockSyncRequestMs = 0;
  uint32_t _lastPerformanceApplyMs = 0;
  volatile uint16_t _lastDisconnectReason = 0;
  volatile bool _disconnectReasonPending = false;

  LogHook _logHook = nullptr;
  void* _logHookUserData = nullptr;

  RingLog _ringLog;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_NET_WIFI_SERVICE_H
