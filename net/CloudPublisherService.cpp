// Scopo del file:
// implementa il publish periodico dello stato verso un endpoint remoto.
#include "CloudPublisherService.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

namespace ccms {

namespace {
static const uint32_t kErrorLogIntervalMs = 10000;
static const uint32_t kStatusLogIntervalMs = 60000;

// Copia bounded per endpoint e altri buffer C fixed-size.
void copyBounded(const char* in, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  const char* src = in ? in : "";
  strncpy(out, src, outLen - 1);
  out[outLen - 1] = '\0';
}

void appendInt64(String& out, int64_t value) {
  char buf[24] = {0};
  snprintf(buf, sizeof(buf), "%lld", (long long)value);
  out += buf;
}
} // namespace

CloudPublisherService::CloudPublisherService(SystemStatus& status, WifiService& wifi)
    : _status(status), _wifi(wifi) {}

void CloudPublisherService::setEndpointUrl(const char* url) {
  copyBounded(url, _endpoint, sizeof(_endpoint));
  _active = (_endpoint[0] != '\0');
}

void CloudPublisherService::setPublishIntervalMs(uint32_t publishIntervalMs) {
  _publishIntervalMs = (publishIntervalMs == 0) ? 1000 : publishIntervalMs;
}

void CloudPublisherService::setRequestTimeoutMs(uint16_t timeoutMs) {
  _timeoutMs = (timeoutMs == 0) ? 300 : timeoutMs;
}

void CloudPublisherService::setEnabled(bool enabled) {
  _enabled = enabled;
}

void CloudPublisherService::setLogHook(LogHook hook, void* userData) {
  _logHook = hook;
  _logHookUserData = userData;
}

bool CloudPublisherService::begin() {
  // `begin()` non apre socket persistenti: inizializza solo stato e logging.
  _lastPublishMs = millis();
  _successCount = 0;
  _failureCount = 0;
  _lastErrorLogMs = 0;
  _lastStatusLogMs = 0;

  _active = (_endpoint[0] != '\0');
  if (!_active) {
    logLine("[CLOUD] disattivato: URL server non configurato");
    return false;
  }

  logLine("[CLOUD] publisher attivo");
  return true;
}

void CloudPublisherService::loop() {
  // Il servizio pubblica solo quando:
  // - e abilitato
  // - ha un endpoint configurato
  // - il Wi-Fi e connesso
  // - e scaduto l'intervallo di publish
  if (!_enabled || !_active) return;
  if (!_wifi.isConnected()) return;

  const uint32_t now = millis();
  if ((uint32_t)(now - _lastPublishMs) < _publishIntervalMs) return;
  _lastPublishMs = now;

  const bool ok = publishNow();
  if (ok) {
    _successCount++;

    if ((uint32_t)(now - _lastStatusLogMs) >= kStatusLogIntervalMs) {
      _lastStatusLogMs = now;
      char line[96] = {0};
      snprintf(line, sizeof(line), "[CLOUD] ok=%lu fail=%lu",
               (unsigned long)_successCount,
               (unsigned long)_failureCount);
      logLine(line);
    }
  } else {
    _failureCount++;
    if ((uint32_t)(now - _lastErrorLogMs) >= kErrorLogIntervalMs) {
      _lastErrorLogMs = now;
      char line[96] = {0};
      snprintf(line, sizeof(line), "[CLOUD] errore publish (ok=%lu fail=%lu)",
               (unsigned long)_successCount,
               (unsigned long)_failureCount);
      logLine(line);
    }
  }
}

bool CloudPublisherService::publishNow() {
  // Il payload viene costruito prima di aprire la connessione per ridurre il
  // tempo in cui la risorsa di rete resta impegnata.
  if (_endpoint[0] == '\0') return false;

  String payload;
  payload.reserve(768);
  buildPayload(payload);

  if (isHttpsEndpoint()) {
    // `setInsecure()` evita la gestione di CA su microcontrollore, accettando
    // pero il compromesso di non validare il certificato server.
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, _endpoint)) return false;
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    const int httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    http.end();
    return (httpCode >= 200 && httpCode < 300);
  } else {
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, _endpoint)) return false;
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    const int httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    http.end();
    return (httpCode >= 200 && httpCode < 300);
  }
}

void CloudPublisherService::buildPayload(String& out) {
  // Viene serializzato solo il subset di stato utile al backend remoto.
  const SystemStatus::EconomicFields econ = _status.economicCopy();
  const SystemStatus::CcTalkFields cc = _status.cctalkCopy();
  char ip[24] = {0};
  _wifi.ipToString(ip, sizeof(ip));

#if defined(ARDUINO_ARCH_ESP32)
  char deviceId[17] = {0};
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(deviceId, sizeof(deviceId), "%08lX", (unsigned long)(chip & 0xFFFFFFFFUL));
#else
  const char* deviceId = "unknown";
#endif

  out += "{";
  out += "\"deviceId\":\"";
  appendJsonEscaped(out, deviceId);
  out += "\",\"timestampMs\":";
  out += String((unsigned long)millis());
  out += ",\"wifi\":{";
  out += "\"ip\":\"";
  appendJsonEscaped(out, ip);
  out += "\",\"rssi\":";
  out += String(_wifi.rssi());
  out += "},\"cctalk\":{";
  out += "\"devices\":";
  out += String((unsigned)cc.detectedDevices);
  out += ",\"transactions\":";
  out += String((unsigned long)cc.transactions);
  out += ",\"txFrames\":";
  out += String((unsigned long)cc.txFrames);
  out += ",\"rxFrames\":";
  out += String((unsigned long)cc.rxFrames);
  out += ",\"lastEvent\":\"";
  appendJsonEscaped(out, cc.lastEventDecoded);
  out += "\"},\"economic\":{";
  out += "\"saldoCents\":";
  appendInt64(out, econ.saldoCents);
  out += ",\"cassaCents\":";
  out += String((unsigned long)econ.cassaCents);
  out += ",\"coinCurrentCents\":";
  appendInt64(out, econ.coinCurrentCents);
  out += "}}";
}

void CloudPublisherService::appendJsonEscaped(String& out, const char* value) {
  // Escape minimo sufficiente per stringhe JSON generate manualmente.
  const char* s = value ? value : "";
  while (*s) {
    const char c = *s++;
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += c; break;
    }
  }
}

bool CloudPublisherService::isHttpsEndpoint() const {
  return strncmp(_endpoint, "https://", 8) == 0;
}

void CloudPublisherService::logLine(const char* line) {
  if (_logHook) _logHook(line ? line : "", _logHookUserData);
}

} // namespace ccms
