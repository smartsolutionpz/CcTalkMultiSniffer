// Scopo del file:
// implementa la macchina a stati Wi-Fi non bloccante del firmware.
#include "WifiService.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../Config.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_wifi.h>
#endif

namespace ccms {

namespace {
WifiService* g_wifiEventTarget = nullptr;

// Evita controlli ripetuti su stringhe nullable nei punti di log/formattazione.
const char* safeText(const char* v) {
  return v ? v : "";
}

bool sameSsid(const char* a, const char* b) {
  if (!a || !b) return false;
  return strcmp(a, b) == 0;
}

bool isValidEpochTime(time_t epoch) {
  return epoch >= 1704067200; // 2024-01-01T00:00:00Z
}

void handleArduinoWifiEvent(arduino_event_t* event) {
  if (!g_wifiEventTarget || !event) return;
  if (event->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_wifiEventTarget->noteDisconnectReason(event->event_info.wifi_sta_disconnected.reason);
  }
}
} // namespace

WifiService::WifiService() {
  // I default vengono letti da Config.h ma possono essere sovrascritti a runtime
  // dalle impostazioni utente caricate da NVS.
  _ssid = appconfig::WIFI_SSID;
  _pass = appconfig::WIFI_PASS;
  _hostname = appconfig::WIFI_HOSTNAME;
  _connectTimeoutMs = appconfig::WIFI_CONNECT_TIMEOUT_MS;
  _retryIntervalMs = appconfig::WIFI_RETRY_INTERVAL_MS;
}

void WifiService::setCredentials(const char* ssid, const char* pass) {
  _ssid = ssid;
  _pass = pass;
  if (_started && _enabled) {
    reconnect();
  }
}

void WifiService::setRunApEnabled(bool enabled) {
  _runApEnabled = enabled;
  if (_started && _enabled) {
    reconnect();
  }
}

void WifiService::setHostname(const char* hostname) {
  _hostname = hostname;
}

void WifiService::setTimings(uint32_t connectTimeoutMs, uint32_t retryIntervalMs) {
  _connectTimeoutMs = connectTimeoutMs;
  _retryIntervalMs = retryIntervalMs;
}

void WifiService::setLogHook(LogHook hook, void* userData) {
  _logHook = hook;
  _logHookUserData = userData;
}

bool WifiService::begin() {
  // Modalita RUN: privilegiamo la station verso la rete salvata. L'AP locale
  // resta riservato alla modalita PROG o all'opzione AP RUN esplicita.
  _started = true;
  _enabled = true;
  _apOnlyMode = false;
  _apFallbackActive = false;
  _clockSyncRequested = false;
  _lastClockSyncRequestMs = 0;

  WiFi.persistent(false);
  resetRadioStackForStart();
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  attachWifiEvents();

  if (_hostname && _hostname[0] != '\0') {
    WiFi.setHostname(_hostname);
  }

  if (appconfig::WIFI_RUN_AP_ALWAYS_ON || _runApEnabled) {
    startApFallback();
  }

  if (!_ssid || _ssid[0] == '\0') {
    logLine("WiFi credentials missing");
    return true;
  }

  startConnectAttempt();
  return true;
}

bool WifiService::beginApOnly() {
  // Modalita PROG: l'AP locale e sempre disponibile. Questa modalita si attiva
  // solo tramite pulsante premuto all'avvio.
  _started = true;
  _enabled = true;
  _apOnlyMode = true;
  _apFallbackActive = false;
  _connecting = false;
  _wasConnected = false;
  _clockSyncRequested = false;
  _lastClockSyncRequestMs = 0;

  WiFi.persistent(false);
  resetRadioStackForStart();
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  attachWifiEvents();

  if (_hostname && _hostname[0] != '\0') {
    WiFi.setHostname(_hostname);
  }

  startApFallback();
  return _apFallbackActive;
}

void WifiService::loop() {
  // Loop volutamente non bloccante: valuta stato, timeout e retry senza delay.
  if (!_started || !_enabled) return;

  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    if (!_wasConnected) {
      _wasConnected = true;
      _connecting = false;
      logConnectedInfo();
    }
    stopApFallbackIfNotNeeded();
    requestClockSync();
    return;
  }

  const uint32_t now = millis();

  if (_wasConnected) {
    _wasConnected = false;
    _connecting = false;
    _lastAttemptMs = now;
    logLine("WiFi disconnected");
    logPendingDisconnectReason();
  }

  if (_connecting && (uint32_t)(now - _attemptStartMs) >= _connectTimeoutMs) {
    _connecting = false;
    WiFi.disconnect(false, false);
    _lastAttemptMs = now;
    logLine("WiFi connect timeout");
  }

  if (!_connecting && _ssid && _ssid[0] != '\0' &&
      (uint32_t)(now - _lastAttemptMs) >= _retryIntervalMs) {
    startConnectAttempt();
  }
}

bool WifiService::isConnected() const {
  return (WiFi.status() == WL_CONNECTED);
}

String WifiService::ip() const {
  if (isConnected()) return WiFi.localIP().toString();
  if (_apFallbackActive) return WiFi.softAPIP().toString();
  return String("");
}

int WifiService::rssi() const {
  if (!isConnected()) return 0;
  return (int)WiFi.RSSI();
}

bool WifiService::isApFallbackActive() const {
  return _apFallbackActive;
}

bool WifiService::enabled() const {
  return _enabled;
}

wl_status_t WifiService::status() const {
  return WiFi.status();
}

const char* WifiService::statusText() const {
  if (_apFallbackActive && !isConnected()) return "AP_FALLBACK";

  switch (WiFi.status()) {
    case WL_CONNECTED: return "CONNECTED";
    case WL_NO_SHIELD: return "NO_SHIELD";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void WifiService::ipToString(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;
  String s = ip();
  strncpy(out, safeText(s.c_str()), outLen - 1);
  out[outLen - 1] = '\0';
}

String WifiService::apSsid() const {
  return String(appconfig::WIFI_FALLBACK_AP_SSID);
}

String WifiService::connectedSsid() const {
  if (isConnected()) return WiFi.SSID();
  return String("");
}

uint8_t WifiService::scanNetworks(ScannedNetwork* out, uint8_t maxCount) {
  if (!out || maxCount == 0) return 0;

  memset(out, 0, sizeof(ScannedNetwork) * maxCount);
  WiFi.mode((_apFallbackActive || _apOnlyMode) ? WIFI_AP_STA : WIFI_STA);
  applyRadioStabilitySettings();

  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    WiFi.scanDelete();
    return 0;
  }

  uint8_t used = 0;
  for (int i = 0; i < found && used < maxCount; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;

    int existing = -1;
    for (uint8_t j = 0; j < used; j++) {
      if (sameSsid(out[j].ssid, ssid.c_str())) {
        existing = (int)j;
        break;
      }
    }

    const int32_t rssi = WiFi.RSSI(i);
    const wifi_auth_mode_t enc = WiFi.encryptionType(i);
    const bool hidden = false;

    if (existing >= 0) {
      if (rssi > out[existing].rssi) {
        strncpy(out[existing].ssid, ssid.c_str(), sizeof(out[existing].ssid) - 1);
        out[existing].ssid[sizeof(out[existing].ssid) - 1] = '\0';
        out[existing].rssi = rssi;
        out[existing].encryption = (uint8_t)enc;
        out[existing].hidden = hidden;
      }
      continue;
    }

    strncpy(out[used].ssid, ssid.c_str(), sizeof(out[used].ssid) - 1);
    out[used].ssid[sizeof(out[used].ssid) - 1] = '\0';
    out[used].rssi = rssi;
    out[used].encryption = (uint8_t)enc;
    out[used].hidden = hidden;
    used++;
  }

  WiFi.scanDelete();
  return used;
}

void WifiService::reconnect() {
  if (!_started || !_enabled) return;

  _connecting = false;
  _wasConnected = false;
  _attemptStartMs = 0;
  _lastAttemptMs = 0;
  _clockSyncRequested = false;
  _lastClockSyncRequestMs = 0;

  WiFi.disconnect(false, false);

  if (!_ssid || _ssid[0] == '\0') {
    logLine("WiFi credentials cleared");
    return;
  }

  startConnectAttempt();
}

bool WifiService::getClockInfo(ClockInfo& out) const {
  out = ClockInfo();

  const time_t now = time(nullptr);
  if (!isValidEpochTime(now)) return false;

  struct tm tmNow;
  if (!localtime_r(&now, &tmNow)) return false;

  out.valid = true;
  out.syncedFromInternet = _clockSyncedFromInternet;
  out.unixTime = (uint64_t)now;
  strftime(out.display, sizeof(out.display), "%d/%m/%Y %H:%M:%S", &tmNow);
  strftime(out.iso8601, sizeof(out.iso8601), "%Y-%m-%dT%H:%M:%S%z", &tmNow);
  return true;
}

void WifiService::attachWifiEvents() {
#if defined(ARDUINO_ARCH_ESP32)
  if (_eventsAttached) return;
  g_wifiEventTarget = this;
  WiFi.onEvent(handleArduinoWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  _eventsAttached = true;
#endif
}

void WifiService::noteDisconnectReason(uint16_t reason) {
  _lastDisconnectReason = reason;
  _disconnectReasonPending = true;
}

void WifiService::logPendingDisconnectReason() {
  if (!_disconnectReasonPending) return;
  const uint16_t reason = _lastDisconnectReason;
  _disconnectReasonPending = false;

  char line[128] = {0};
#if defined(ARDUINO_ARCH_ESP32)
  snprintf(line, sizeof(line), "WiFi disconnect reason: %u (%s)",
           (unsigned)reason,
           safeText(WiFi.disconnectReasonName((wifi_err_reason_t)reason)));
#else
  snprintf(line, sizeof(line), "WiFi disconnect reason: %u", (unsigned)reason);
#endif
  logLine(line);
}

void WifiService::startConnectAttempt() {
  if (!_enabled) return;
  if (!_ssid || _ssid[0] == '\0') return;

  if (_apFallbackActive || appconfig::WIFI_RUN_AP_ALWAYS_ON || _runApEnabled) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  applyRadioStabilitySettings();

  _connecting = true;
  _attemptStartMs = millis();
  _lastAttemptMs = _attemptStartMs;

  char line[128] = {0};
  snprintf(line, sizeof(line), "WiFi connecting to: %s", safeText(_ssid));
  logLine(line);
  WiFi.begin(_ssid, _pass);
}

void WifiService::startApFallback() {
  if (_apFallbackActive) return;

  // L'AP di fallback espone un IP locale noto, cosi la UI e sempre raggiungibile
  // anche senza DHCP o rete esterna.
  const bool hasStaCredentials = _ssid && _ssid[0] != '\0';
  WiFi.mode((_apOnlyMode || !hasStaCredentials) ? WIFI_AP : WIFI_AP_STA);
  applyRadioStabilitySettings();

  IPAddress apIp(192, 168, 4, 1);
  IPAddress apGateway(192, 168, 4, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIp, apGateway, apMask);

  const char* apSsid = appconfig::WIFI_FALLBACK_AP_SSID;
  const char* apPass = appconfig::WIFI_FALLBACK_AP_PASS;
  bool ok = false;
  if (apPass && strlen(apPass) >= 8) {
    ok = WiFi.softAP(apSsid, apPass, appconfig::WIFI_AP_CHANNEL);
  } else {
    ok = WiFi.softAP(apSsid, nullptr, appconfig::WIFI_AP_CHANNEL);
  }

  if (!ok) {
    logLine("AP fallback start failed");
    return;
  }

  _apFallbackActive = true;
  logLine("AP fallback active");

  char line[96] = {0};
  snprintf(line, sizeof(line), "AP SSID: %s", safeText(apSsid));
  logLine(line);
  snprintf(line, sizeof(line), "AP IP: %s", safeText(WiFi.softAPIP().toString().c_str()));
  logLine(line);
}

void WifiService::stopApFallbackIfNotNeeded() {
  if (!_apFallbackActive) return;
  if (_apOnlyMode || appconfig::WIFI_RUN_AP_ALWAYS_ON || _runApEnabled) return;

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  applyRadioStabilitySettings();
  _apFallbackActive = false;
  logLine("AP fallback stopped after STA connection");
}

void WifiService::resetRadioStackForStart() {
  if (!appconfig::WIFI_RESET_RADIO_ON_BEGIN) return;

  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  delay(500);
}

void WifiService::applyRadioStabilitySettings() {
  WiFi.setSleep(false);
#if defined(ARDUINO_ARCH_ESP32)
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(appconfig::WIFI_MAX_TX_POWER);
  if (appconfig::WIFI_FORCE_LEGACY_24G) {
    const uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    esp_wifi_set_protocol(WIFI_IF_STA, protocol);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_protocol(WIFI_IF_AP, protocol);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
  }
#endif
}

void WifiService::requestClockSync() {
  const uint32_t nowMs = millis();
  if (_clockSyncRequested &&
      (uint32_t)(nowMs - _lastClockSyncRequestMs) < appconfig::NTP_RESYNC_INTERVAL_MS) {
    const time_t now = time(nullptr);
    if (isValidEpochTime(now) && !_clockSyncedFromInternet) {
      _clockSyncedFromInternet = true;
      logLine("Clock synced from internet");
    }
    return;
  }

  configTzTime(appconfig::TIME_TZ,
               appconfig::NTP_SERVER_1,
               appconfig::NTP_SERVER_2,
               appconfig::NTP_SERVER_3);
  _clockSyncRequested = true;
  _lastClockSyncRequestMs = nowMs;
  logLine("NTP sync requested");

  const time_t now = time(nullptr);
  if (isValidEpochTime(now) && !_clockSyncedFromInternet) {
    _clockSyncedFromInternet = true;
    logLine("Clock synced from internet");
  }
}

void WifiService::logLine(const char* line) {
  // La seriale resta il canale primario di debug locale; in parallelo aggiorniamo
  // il ring log e l'eventuale hook di integrazione.
  const char* msg = safeText(line);
  Serial.println(msg);
  _ringLog.push(msg);
  if (_logHook) _logHook(msg, _logHookUserData);
}

void WifiService::logConnectedInfo() {
  logLine("Connected");

  char line[96] = {0};
  snprintf(line, sizeof(line), "SSID: %s", safeText(WiFi.SSID().c_str()));
  logLine(line);

  memset(line, 0, sizeof(line));
  const String ipStr = WiFi.localIP().toString();
  snprintf(line, sizeof(line), "IP: %s", safeText(ipStr.c_str()));
  logLine(line);

  snprintf(line, sizeof(line), "RSSI: %d", rssi());
  logLine(line);
}

} // namespace ccms
