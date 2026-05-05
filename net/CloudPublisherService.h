// Scopo del file:
// dichiara `CloudPublisherService`, il servizio che pubblica via HTTP/HTTPS
// uno snapshot sintetico dello stato del dispositivo.
#ifndef CCTALK_MULTI_SNIFFER_NET_CLOUD_PUBLISHER_SERVICE_H
#define CCTALK_MULTI_SNIFFER_NET_CLOUD_PUBLISHER_SERVICE_H

#include <Arduino.h>

#include "../AppSettings.h"
#include "../status/SystemStatus.h"
#include "WifiService.h"

namespace ccms {

// Publisher periodico dello stato verso un endpoint HTTP/HTTPS.
// Consuma `SystemStatus` come modello sorgente e `WifiService` come prerequisito
// di connettivita, restando non bloccante nel loop principale salvo il tempo
// strettamente necessario alla singola POST.
class CloudPublisherService {
public:
  typedef void (*LogHook)(const char* line, void* userData);

  CloudPublisherService(SystemStatus& status, WifiService& wifi);

  // Parametri di configurazione runtime.
  void setEndpointUrl(const char* url);
  void setPublishIntervalMs(uint32_t publishIntervalMs);
  void setRequestTimeoutMs(uint16_t timeoutMs);
  void setEnabled(bool enabled);
  void setLogHook(LogHook hook, void* userData);

  bool begin();
  void loop();

  bool enabled() const { return _enabled; }
  bool active() const { return _active; }
  uint32_t successCount() const { return _successCount; }
  uint32_t failureCount() const { return _failureCount; }

private:
  void logLine(const char* line);
  // Esegue una singola pubblicazione completa.
  bool publishNow();
  // Serializza il sottoinsieme di stato richiesto dal backend remoto.
  void buildPayload(String& out);
  static void appendJsonEscaped(String& out, const char* value);
  bool isHttpsEndpoint() const;

  SystemStatus& _status;
  WifiService& _wifi;
  char _endpoint[AppSettings::kServerUrlSize] = {0};
  uint32_t _publishIntervalMs = 1000;
  uint16_t _timeoutMs = 300;
  bool _enabled = true;
  bool _active = false;
  uint32_t _lastPublishMs = 0;
  uint32_t _successCount = 0;
  uint32_t _failureCount = 0;
  uint32_t _lastErrorLogMs = 0;
  uint32_t _lastStatusLogMs = 0;

  LogHook _logHook = nullptr;
  void* _logHookUserData = nullptr;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_NET_CLOUD_PUBLISHER_SERVICE_H
