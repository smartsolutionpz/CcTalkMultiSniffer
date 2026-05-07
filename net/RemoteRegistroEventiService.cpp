// Scopo del file:
// implementa il protocollo request/response su DB remoto per il master.
#include "RemoteRegistroEventiService.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdio.h>
#include <string.h>

#include "../Config.h"

namespace ccms {

namespace {
bool startsWith(const char* text, const char* prefix) {
  if (!text || !prefix) return false;
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

bool isSupportedUrl(const char* url) {
  return startsWith(url, "http://") || startsWith(url, "https://");
}

int32_t clampToInt32(int64_t value) {
  if (value > 2147483647LL) return 2147483647L;
  if (value < -2147483647LL - 1LL) return -2147483647L - 1L;
  return (int32_t)value;
}

String compactPreview(const String& body, size_t maxLen = 120) {
  String out = body;
  out.replace('\r', ' ');
  out.replace('\n', ' ');
  out.trim();
  if (out.length() > (int)maxLen) {
    out = out.substring(0, (int)maxLen);
    out += "...";
  }
  return out;
}

bool isJsonWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int findJsonValueStart(const String& body, const char* key) {
  if (!key || key[0] == '\0') return -1;

  String pattern = "\"";
  pattern += key;
  pattern += "\"";

  const int keyPos = body.indexOf(pattern);
  if (keyPos < 0) return -1;

  const int colonPos = body.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return -1;

  int valuePos = colonPos + 1;
  while (valuePos < body.length() && isJsonWhitespace(body[valuePos])) valuePos++;
  return (valuePos < body.length()) ? valuePos : -1;
}

bool isUrlUnreserved(char c) {
  return ((c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.' || c == '~');
}
} // namespace

RemoteRegistroEventiService::RemoteRegistroEventiService(SystemStatus& status, WifiService& wifi)
    : _status(status), _wifi(wifi) {}

void RemoteRegistroEventiService::copyBounded(const char* in, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  const char* src = in ? in : "";
  strncpy(out, src, outLen - 1);
  out[outLen - 1] = '\0';
}

void RemoteRegistroEventiService::copyBounded(const String& in, char* out, size_t outLen) {
  copyBounded(in.c_str(), out, outLen);
}

void RemoteRegistroEventiService::appendJsonEscaped(String& out, const char* value) {
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

void RemoteRegistroEventiService::appendJsonNullableString(String& out,
                                                           const char* value,
                                                           bool isNull) {
  if (isNull) {
    out += "null";
    return;
  }

  out += "\"";
  appendJsonEscaped(out, value);
  out += "\"";
}

void RemoteRegistroEventiService::appendUrlEncoded(String& out, const char* value) {
  static const char* const kHex = "0123456789ABCDEF";
  const char* s = value ? value : "";
  while (*s) {
    const uint8_t c = (uint8_t)(*s++);
    if (isUrlUnreserved((char)c)) {
      out += (char)c;
      continue;
    }

    out += '%';
    out += kHex[(c >> 4) & 0x0F];
    out += kHex[c & 0x0F];
  }
}

bool RemoteRegistroEventiService::responseHasBoolTrue(const String& body, const char* key) {
  const int valuePos = findJsonValueStart(body, key);
  if (valuePos < 0) return false;
  return body.substring(valuePos).startsWith("true");
}

bool RemoteRegistroEventiService::responseHasBoolFalse(const String& body, const char* key) {
  const int valuePos = findJsonValueStart(body, key);
  if (valuePos < 0) return false;
  return body.substring(valuePos).startsWith("false");
}

bool RemoteRegistroEventiService::responseHasNullValue(const String& body, const char* key) {
  const int valuePos = findJsonValueStart(body, key);
  if (valuePos < 0) return false;
  return body.substring(valuePos).startsWith("null");
}

long RemoteRegistroEventiService::responseLongValue(const String& body,
                                                    const char* key,
                                                    long defaultValue) {
  const int valuePos = findJsonValueStart(body, key);
  if (valuePos < 0) return defaultValue;

  int end = valuePos;
  if (body[end] == '-') end++;
  while (end < body.length() && body[end] >= '0' && body[end] <= '9') end++;
  if (end <= valuePos || (end == valuePos + 1 && body[valuePos] == '-')) return defaultValue;
  return body.substring(valuePos, end).toInt();
}

String RemoteRegistroEventiService::responseStringValue(const String& body, const char* key) {
  const int valuePos = findJsonValueStart(body, key);
  if (valuePos < 0 || valuePos >= body.length() || body[valuePos] != '"') return String("");

  String value;
  for (int i = valuePos + 1; i < body.length(); i++) {
    const char c = body[i];
    if (c == '\\' && (i + 1) < body.length()) {
      const char escaped = body[++i];
      switch (escaped) {
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case '\\': value += '\\'; break;
        case '"': value += '"'; break;
        default: value += escaped; break;
      }
      continue;
    }
    if (c == '"') return value;
    value += c;
  }

  return String("");
}

bool RemoteRegistroEventiService::responseHasSuccessTrue(const String& body) {
  return responseHasBoolTrue(body, "success");
}

long RemoteRegistroEventiService::responseInsertId(const String& body) {
  return responseLongValue(body, "id", -1);
}

String RemoteRegistroEventiService::responseMessageText(const String& body) {
  return responseStringValue(body, "message");
}

bool RemoteRegistroEventiService::shouldQueueEvent(const char* line, bool decoded) {
  (void)line;
  (void)decoded;
  return false;
}

void RemoteRegistroEventiService::classifyOperation(const char* line,
                                                    bool decoded,
                                                    char* out,
                                                    size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';

  if (!line || line[0] == '\0') {
    copyBounded(decoded ? "CCTALK" : "SYSTEM", out, outLen);
    return;
  }

  if (line[0] == '[') {
    const char* end = strchr(line, ']');
    if (end && end > line + 1) {
      size_t len = (size_t)(end - (line + 1));
      if (len > outLen - 1) len = outLen - 1;
      memcpy(out, line + 1, len);
      out[len] = '\0';
      return;
    }
  }

  copyBounded(decoded ? "CCTALK" : "SYSTEM", out, outLen);
}

void RemoteRegistroEventiService::buildDeviceLabel(char* out, size_t outLen) {
  if (!out || outLen == 0) return;
#if defined(ARDUINO_ARCH_ESP32)
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(out, outLen, "esp32-%08lX", (unsigned long)(mac & 0xFFFFFFFFUL));
#else
  copyBounded("device-unknown", out, outLen);
#endif
}

void RemoteRegistroEventiService::setEndpointUrl(const char* url) {
  copyBounded(url, _endpoint, sizeof(_endpoint));
  updateActiveFlag();
}

void RemoteRegistroEventiService::setLocationCode(const char* code) {
  copyBounded(code, _locationCode, sizeof(_locationCode));
  _lastHandledRequestId = 0;
  _lastHandledRequestSignature[0] = '\0';
  _pendingAppliedRequestSignature[0] = '\0';
  _pendingAppliedRequestOk = false;
  _pendingAppliedResponseMessage[0] = '\0';
  _requestState = REQUEST_STATE_IDLE;
  updateActiveFlag();
}

void RemoteRegistroEventiService::setApiKey(const char* key) {
  copyBounded(key, _apiKey, sizeof(_apiKey));
}

void RemoteRegistroEventiService::setEnabled(bool enabled) {
  _enabled = enabled;
}

void RemoteRegistroEventiService::setRequestTimeoutMs(uint16_t timeoutMs) {
  _timeoutMs = (timeoutMs == 0) ? 1200 : timeoutMs;
}

void RemoteRegistroEventiService::setRetryIntervalMs(uint32_t retryIntervalMs) {
  _retryIntervalMs = (retryIntervalMs == 0) ? 5000 : retryIntervalMs;
}

void RemoteRegistroEventiService::setDbPollIntervalMs(uint32_t pollIntervalMs) {
  _dbPollIntervalMs = (pollIntervalMs == 0) ? 10000 : pollIntervalMs;
}

void RemoteRegistroEventiService::setLogHook(LogHook hook, void* userData) {
  _logHook = hook;
  _logHookUserData = userData;
}

void RemoteRegistroEventiService::setRequestApplyHook(RequestApplyHook hook, void* userData) {
  _requestApplyHook = hook;
  _requestApplyUserData = userData;
}

void RemoteRegistroEventiService::updateActiveFlag() {
  _active = (_endpoint[0] != '\0' &&
             _locationCode[0] != '\0' &&
             isSupportedUrl(_endpoint));
}

bool RemoteRegistroEventiService::begin() {
  _started = true;
  _successCount = 0;
  _failureCount = 0;
  _droppedCount = 0;
  _lastAttemptMs = millis();
  _lastDbPollMs = _lastAttemptMs - _dbPollIntervalMs;
  _wifiConnectedSinceMs = 0;
  _remoteBackoffUntilMs = 0;
  _lastWifiConnected = false;
  _consecutiveFailures = 0;
  _remoteBackoffLogged = false;
  _requestState = REQUEST_STATE_IDLE;
  _lastHandledRequestId = 0;
  _lastHandledRequestSignature[0] = '\0';
  _pendingAppliedRequestSignature[0] = '\0';
  _pendingAppliedRequestOk = false;
  _pendingAppliedResponseMessage[0] = '\0';

  updateActiveFlag();
  if (!_active) {
    logLine("[REMOTE_DB] disattivato: URL non valido o codice ubicazione mancante");
    return false;
  }

  logLine("[REMOTE_DB] servizio request/response remoto attivo");
  return true;
}

void RemoteRegistroEventiService::logLine(const char* line) {
  if (_logHook) _logHook(line ? line : "", _logHookUserData);
}

uint8_t RemoteRegistroEventiService::queuedCount() const {
  return 0;
}

void RemoteRegistroEventiService::buildQueuedEvent(const char* line, bool decoded, QueuedEvent& out) const {
  out = QueuedEvent();

  classifyOperation(line, decoded, out.operation, sizeof(out.operation));
  copyBounded(line, out.description, sizeof(out.description));
  buildDeviceLabel(out.device, sizeof(out.device));

  const SystemStatus::EconomicFields econ = _status.economicCopy();
  const SystemStatus::CcTalkFields cc = _status.cctalkCopy();

  out.value1 = clampToInt32(econ.saldoCents);
  out.value2 = clampToInt32((int64_t)econ.cassaCents);
  out.value3 = clampToInt32(econ.coinCurrentCents);
  out.value4 = clampToInt32((int64_t)cc.txFrames);
  out.value5 = clampToInt32((int64_t)cc.rxFrames);
  out.value6 = clampToInt32((int64_t)cc.transactions);

  copyBounded(cc.lastTxFrame, out.note1, sizeof(out.note1));
  copyBounded(cc.lastRxFrame, out.note2, sizeof(out.note2));
  copyBounded(_wifi.connectedSsid(), out.note3, sizeof(out.note3));
  copyBounded(_wifi.ip(), out.note4, sizeof(out.note4));
}

void RemoteRegistroEventiService::buildRecyclerInventoryNote(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;
  out[0] = '\0';

  const uint8_t count = _status.recyclerEntryCount();
  for (uint8_t i = 0; i < count; i++) {
    SystemStatus::RecyclerInventoryEntry entry;
    if (!_status.recyclerEntryAt(i, entry) || !entry.valid) continue;

    const size_t used = strlen(out);
    if (used >= (outLen - 1)) break;

    const int written = snprintf(out + used,
                                 outLen - used,
                                 "%s%u:%u/%u/%u",
                                 (used > 0) ? ";" : "",
                                 (unsigned)entry.addr,
                                 (unsigned)entry.count10,
                                 (unsigned)entry.count20,
                                 (unsigned)entry.count50);
    if (written < 0 || (size_t)written >= (outLen - used)) {
      out[outLen - 1] = '\0';
      break;
    }
  }
}

void RemoteRegistroEventiService::buildChangeEvent(QueuedEvent& out) const {
  out = QueuedEvent();

  copyBounded("CHANGE", out.operation, sizeof(out.operation));
  out.descriptionIsNull = true;
  buildDeviceLabel(out.device, sizeof(out.device));

  const SystemStatus::EconomicFields econ = _status.economicCopy();
  out.value1 = clampToInt32((int64_t)econ.cntotBanconoteInCents);
  out.value2 = clampToInt32((int64_t)econ.cntotMoneteOutCents);
  out.value3 = clampToInt32((int64_t)econ.cntotMoneteInCents);
  out.value4 = clampToInt32((int64_t)econ.cntotBanconoteOutCents);
  out.value5 = clampToInt32((int64_t)econ.cassaCents);
  out.value6 = clampToInt32(econ.coinCurrentCents);

  buildRecyclerInventoryNote(out.note1, sizeof(out.note1));
  out.note1IsNull = (out.note1[0] == '\0');
  out.note2IsNull = true;
  out.note3IsNull = true;
  out.note4IsNull = true;
}

void RemoteRegistroEventiService::buildRequestSignature(const PendingRequest& request,
                                                        char* out,
                                                        size_t outLen) const {
  if (!out || outLen == 0) return;
  snprintf(out,
           outLen,
           "%lu|%s|%s|%s|%s",
           (unsigned long)request.requestId,
           request.requestTime,
           request.command,
           request.requestPayload,
           request.requestState);
  out[outLen - 1] = '\0';
}

void RemoteRegistroEventiService::noteEvent(const char* line, bool decoded) {
  (void)line;
  (void)decoded;
}

bool RemoteRegistroEventiService::noteChangeEvent() {
  return false;
}

bool RemoteRegistroEventiService::saveChangeEventNow(String& message) {
  return saveChangeEventInternal("manuale", message);
}

bool RemoteRegistroEventiService::saveChangeEventInternal(const char* trigger, String& message) {
  if (!_started) {
    _started = true;
    updateActiveFlag();
  }
  if (!_enabled) {
    message = "servizio remoto disabilitato";
    return false;
  }
  if (!_active) {
    message = "endpoint remoto o codice ubicazione mancanti";
    return false;
  }

  QueuedEvent event;
  buildChangeEvent(event);

  {
    char line[196] = {0};
    snprintf(line, sizeof(line),
             "[REMOTE_DB] salvataggio %s CHANGE verso %s (ubicazione=%s)",
             (trigger && trigger[0] != '\0') ? trigger : "richiesto",
             _endpoint,
             _locationCode);
    logLine(line);
  }

  if (!_wifi.isConnected()) {
    message = "WiFi non connesso";
    logLine("[REMOTE_DB] salvataggio CHANGE non inviato: WiFi assente");
    return false;
  }

  _lastAttemptMs = millis();
  String serverMessage;
  if (postEvent(event, &serverMessage)) {
    _successCount++;
    message = (serverMessage.length() > 0) ? serverMessage : String("record salvato su DB server");
    return true;
  }

  _failureCount++;
  message = (serverMessage.length() > 0) ? (String("server: ") + serverMessage)
                                         : String("errore invio record");
  return false;
}

bool RemoteRegistroEventiService::fetchPendingRequest(PendingRequest& out, String& responseMessage) {
  out = PendingRequest();
  responseMessage = "";

  if (_endpoint[0] == '\0' || _locationCode[0] == '\0') {
    responseMessage = "endpoint remoto o codice ubicazione mancanti";
    return false;
  }

  String statusUrl = _endpoint;
  statusUrl += (statusUrl.indexOf('?') >= 0) ? '&' : '?';
  statusUrl += "codiceUbicazione=";
  appendUrlEncoded(statusUrl, _locationCode);
  statusUrl += "&pending=1";

  const bool https = (strncmp(_endpoint, "https://", 8) == 0);
  int httpCode = -1;
  String body;

  if (https) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, statusUrl)) {
      responseMessage = "impossibile aprire connessione HTTPS";
      return false;
    }
    http.setTimeout(_timeoutMs);
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.GET();
    body = http.getString();
    http.end();
  } else {
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, statusUrl)) {
      responseMessage = "impossibile aprire connessione HTTP";
      return false;
    }
    http.setTimeout(_timeoutMs);
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.GET();
    body = http.getString();
    http.end();
  }

  const bool success = responseHasSuccessTrue(body);
  responseMessage = responseMessageText(body);
  if (httpCode < 200 || httpCode >= 300 || !success) {
    if (responseMessage.length() == 0) {
      responseMessage = (httpCode >= 0)
                            ? (String("HTTP ") + String(httpCode) + " - " + compactPreview(body))
                            : String("errore HTTP durante lettura richiesta pendente");
    }
    return false;
  }

  bool found = false;
  if (responseHasBoolTrue(body, "found")) {
    found = true;
  } else if (responseHasBoolFalse(body, "found")) {
    found = false;
  } else {
    responseMessage = "risposta pending senza campo found valido";
    return false;
  }

  if (!found) {
    if (responseMessage.length() == 0) responseMessage = "nessuna richiesta pendente";
    return true;
  }

  const long requestId = responseLongValue(body, "id", -1);
  if (requestId <= 0) {
    responseMessage = "risposta pending senza id valido";
    return false;
  }

  out.requestId = (uint32_t)requestId;
  copyBounded(responseStringValue(body, "command"), out.command, sizeof(out.command));
  copyBounded(responseStringValue(body, "requestPayload"), out.requestPayload, sizeof(out.requestPayload));
  copyBounded(responseStringValue(body, "state"), out.requestState, sizeof(out.requestState));
  copyBounded(responseStringValue(body, "requestTime"), out.requestTime, sizeof(out.requestTime));

  if (responseMessage.length() == 0) responseMessage = "richiesta pendente acquisita";
  return true;
}

void RemoteRegistroEventiService::buildJsonPayload(const QueuedEvent& event, String& out) const {
  out.reserve(1024);
  out += "{";
  out += "\"codiceUbicazione\":\"";
  appendJsonEscaped(out, _locationCode);
  out += "\",\"operazione\":";
  appendJsonNullableString(out, event.operation, false);
  out += ",\"descrizione\":";
  appendJsonNullableString(out, event.description, event.descriptionIsNull);
  out += ",\"valore1\":";
  out += String(event.value1);
  out += ",\"valore2\":";
  out += String(event.value2);
  out += ",\"valore3\":";
  out += String(event.value3);
  out += ",\"valore4\":";
  out += String(event.value4);
  out += ",\"valore5\":";
  out += String(event.value5);
  out += ",\"valore6\":";
  out += String(event.value6);
  out += ",\"note1\":";
  appendJsonNullableString(out, event.note1, event.note1IsNull);
  out += ",\"note2\":";
  appendJsonNullableString(out, event.note2, event.note2IsNull);
  out += ",\"note3\":";
  appendJsonNullableString(out, event.note3, event.note3IsNull);
  out += ",\"note4\":";
  appendJsonNullableString(out, event.note4, event.note4IsNull);
  out += ",\"dispositivo\":\"";
  appendJsonEscaped(out, event.device);
  out += "\"}";
}

void RemoteRegistroEventiService::buildResponseJsonPayload(const PendingRequest& request,
                                                           bool requestApplied,
                                                           const char* responseMessage,
                                                           String& out) const {
  QueuedEvent event;
  buildChangeEvent(event);

  out.reserve(1024);
  out += "{";
  out += "\"mode\":\"response\"";
  out += ",\"id\":";
  out += String((unsigned long)request.requestId);
  out += ",\"codiceUbicazione\":\"";
  appendJsonEscaped(out, _locationCode);
  out += "\",\"status\":\"";
  appendJsonEscaped(out, requestApplied ? "served" : "error");
  out += "\",\"responseMessage\":\"";
  appendJsonEscaped(out, responseMessage ? responseMessage : "");
  out += "\",\"note1\":";
  appendJsonNullableString(out, event.note1, event.note1IsNull);
  out += ",\"valore1\":";
  out += String(event.value1);
  out += ",\"valore2\":";
  out += String(event.value2);
  out += ",\"valore3\":";
  out += String(event.value3);
  out += ",\"valore4\":";
  out += String(event.value4);
  out += ",\"valore5\":";
  out += String(event.value5);
  out += ",\"valore6\":";
  out += String(event.value6);
  out += ",\"dispositivo\":\"";
  appendJsonEscaped(out, event.device);
  out += "\"}";
}

bool RemoteRegistroEventiService::postEvent(const QueuedEvent& event, String* responseMessage) {
  if (_endpoint[0] == '\0' || _locationCode[0] == '\0') return false;

  String payload;
  buildJsonPayload(event, payload);

  const bool https = (strncmp(_endpoint, "https://", 8) == 0);
  int httpCode = -1;
  String body;

  if (https) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, _endpoint)) {
      if (responseMessage) *responseMessage = "impossibile aprire connessione HTTPS";
      return false;
    }
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    body = http.getString();
    http.end();
  } else {
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, _endpoint)) {
      if (responseMessage) *responseMessage = "impossibile aprire connessione HTTP";
      return false;
    }
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    body = http.getString();
    http.end();
  }

  const String serverMessage = responseMessageText(body);
  const bool success = responseHasSuccessTrue(body);
  const long insertId = responseInsertId(body);
  if (responseMessage) {
    if (httpCode < 200 || httpCode >= 300) {
      *responseMessage = (serverMessage.length() > 0)
                             ? (String("HTTP ") + String(httpCode) + " - " + serverMessage)
                             : (String("HTTP ") + String(httpCode) + " - " + compactPreview(body));
    } else if (!success) {
      *responseMessage = (serverMessage.length() > 0)
                             ? serverMessage
                             : String("risposta server senza success=true");
    } else if (insertId <= 0) {
      *responseMessage = (serverMessage.length() > 0)
                             ? (serverMessage + " (id mancante o non valido)")
                             : String("risposta server senza id valido");
    } else {
      *responseMessage = (serverMessage.length() > 0) ? serverMessage : String("evento registrato");
    }
  }
  return (httpCode >= 200 && httpCode < 300) && success && (insertId > 0);
}

bool RemoteRegistroEventiService::postResponse(const PendingRequest& request,
                                               bool requestApplied,
                                               const char* responseMessage,
                                               String* serverResponseMessage) {
  if (_endpoint[0] == '\0' || _locationCode[0] == '\0' || request.requestId == 0) return false;

  String payload;
  buildResponseJsonPayload(request, requestApplied, responseMessage, payload);

  const bool https = (strncmp(_endpoint, "https://", 8) == 0);
  int httpCode = -1;
  String body;

  if (https) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, _endpoint)) {
      if (serverResponseMessage) *serverResponseMessage = "impossibile aprire connessione HTTPS";
      return false;
    }
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    body = http.getString();
    http.end();
  } else {
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, _endpoint)) {
      if (serverResponseMessage) *serverResponseMessage = "impossibile aprire connessione HTTP";
      return false;
    }
    http.setTimeout(_timeoutMs);
    http.addHeader("Content-Type", "application/json");
    if (_apiKey[0] != '\0') http.addHeader("X-API-Key", _apiKey);
    httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    body = http.getString();
    http.end();
  }

  const String message = responseMessageText(body);
  const bool success = responseHasSuccessTrue(body);
  const long updatedId = responseInsertId(body);
  if (serverResponseMessage) {
    if (httpCode < 200 || httpCode >= 300) {
      *serverResponseMessage = (message.length() > 0)
                                   ? (String("HTTP ") + String(httpCode) + " - " + message)
                                   : (String("HTTP ") + String(httpCode) + " - " + compactPreview(body));
    } else if (!success) {
      *serverResponseMessage = (message.length() > 0)
                                   ? message
                                   : String("risposta server senza success=true");
    } else if (updatedId <= 0) {
      *serverResponseMessage = (message.length() > 0)
                                   ? (message + " (id mancante o non valido)")
                                   : String("risposta server senza id valido");
    } else {
      *serverResponseMessage = (message.length() > 0) ? message : String("risposta aggiornata");
    }
  }
  return (httpCode >= 200 && httpCode < 300) && success && (updatedId > 0);
}

void RemoteRegistroEventiService::loop() {
  if (!_started || !_enabled || !_active) return;

  const bool wifiConnected = _wifi.isConnected();
  const bool justConnected = (wifiConnected && !_lastWifiConnected);
  _lastWifiConnected = wifiConnected;

  const uint32_t now = millis();
  if (!wifiConnected) {
    _wifiConnectedSinceMs = 0;
    return;
  }

  if (justConnected || _wifiConnectedSinceMs == 0) {
    _wifiConnectedSinceMs = now;
    _lastAttemptMs = now;
    _lastDbPollMs = now;
    return;
  }

  if ((uint32_t)(now - _wifiConnectedSinceMs) < appconfig::REMOTE_DB_WIFI_SETTLE_MS) return;

  if (_remoteBackoffUntilMs != 0 && (int32_t)(now - _remoteBackoffUntilMs) < 0) {
    if (!_remoteBackoffLogged) {
      char line[160] = {0};
      snprintf(line, sizeof(line),
               "[REMOTE_DB] polling remoto in backoff per %lums",
               (unsigned long)(_remoteBackoffUntilMs - now));
      logLine(line);
      _remoteBackoffLogged = true;
    }
    return;
  }
  _remoteBackoffUntilMs = 0;
  _remoteBackoffLogged = false;

  if (!justConnected && (uint32_t)(now - _lastDbPollMs) < _dbPollIntervalMs) return;
  if (!justConnected && (uint32_t)(now - _lastAttemptMs) < _retryIntervalMs) return;
  _lastDbPollMs = now;
  _lastAttemptMs = now;

  PendingRequest request;
  String statusMessage;
  if (!fetchPendingRequest(request, statusMessage)) {
    _failureCount++;
    if (_consecutiveFailures < 10) _consecutiveFailures++;
    uint32_t backoffMs = appconfig::REMOTE_DB_FAILURE_BACKOFF_BASE_MS * (uint32_t)_consecutiveFailures;
    if (backoffMs > appconfig::REMOTE_DB_FAILURE_BACKOFF_MAX_MS) {
      backoffMs = appconfig::REMOTE_DB_FAILURE_BACKOFF_MAX_MS;
    }
    _remoteBackoffUntilMs = now + backoffMs;
    _remoteBackoffLogged = false;
    char line[224] = {0};
    snprintf(line, sizeof(line),
             "[REMOTE_DB] lettura richiesta pendente fallita: %s; backoff=%lums",
             (statusMessage.length() > 0) ? statusMessage.c_str() : "errore sconosciuto",
             (unsigned long)backoffMs);
    logLine(line);
    return;
  }

  _consecutiveFailures = 0;
  _remoteBackoffUntilMs = 0;
  _remoteBackoffLogged = false;

  if (request.requestId == 0) {
    _requestState = REQUEST_STATE_IDLE;
    _pendingAppliedRequestSignature[0] = '\0';
    _pendingAppliedRequestOk = false;
    _pendingAppliedResponseMessage[0] = '\0';
    return;
  }

  char signature[sizeof(_lastHandledRequestSignature)] = {0};
  buildRequestSignature(request, signature, sizeof(signature));

  _requestState = REQUEST_STATE_REQUEST_SEEN;
  _lastAttemptMs = now;

  String applyMessage;
  bool applyOk = true;
  if (_pendingAppliedRequestSignature[0] != '\0' &&
      strcmp(signature, _pendingAppliedRequestSignature) == 0) {
    applyOk = _pendingAppliedRequestOk;
    applyMessage = _pendingAppliedResponseMessage;
  } else if (_requestApplyHook) {
    applyOk = _requestApplyHook(request.command,
                                request.requestPayload,
                                applyMessage,
                                _requestApplyUserData);
  } else if (request.command[0] == '\0' || strcmp(request.command, "SNAPSHOT") == 0) {
    applyMessage = "snapshot corrente acquisita";
  } else {
    applyOk = false;
    applyMessage = "nessun gestore richieste configurato";
  }
  copyBounded(signature, _pendingAppliedRequestSignature, sizeof(_pendingAppliedRequestSignature));
  _pendingAppliedRequestOk = applyOk;
  copyBounded(applyMessage, _pendingAppliedResponseMessage, sizeof(_pendingAppliedResponseMessage));

  String serverMessage;
  if (postResponse(request, applyOk, applyMessage.c_str(), &serverMessage)) {
    _successCount++;
    _lastHandledRequestId = request.requestId;
    copyBounded(signature, _lastHandledRequestSignature, sizeof(_lastHandledRequestSignature));
    _pendingAppliedRequestSignature[0] = '\0';
    _pendingAppliedRequestOk = false;
    _pendingAppliedResponseMessage[0] = '\0';
    _requestState = REQUEST_STATE_RESPONSE_POSTED;

    char line[256] = {0};
    snprintf(line, sizeof(line),
             "[REMOTE_DB] risposta request id=%lu cmd=%s esito=%s",
             (unsigned long)request.requestId,
             (request.command[0] != '\0') ? request.command : "SNAPSHOT",
             applyOk ? "served" : "error");
    logLine(line);
    return;
  }

  _failureCount++;
  char line[256] = {0};
  snprintf(line, sizeof(line),
           "[REMOTE_DB] update risposta request id=%lu fallito: %s",
           (unsigned long)request.requestId,
           (serverMessage.length() > 0) ? serverMessage.c_str() : "errore sconosciuto");
  logLine(line);
}

} // namespace ccms
