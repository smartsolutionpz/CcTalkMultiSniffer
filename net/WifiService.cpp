// Scopo del file:
// implementa una gestione Wi-Fi volutamente semplice e non bloccante.
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

const char* onOffText(bool enabled) {
  return enabled ? "on" : "off";
}

const char* yesNoText(bool value) {
  return value ? "yes" : "no";
}

const char* wifiModeText(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_OFF: return "OFF";
    case WIFI_STA: return "STA";
    case WIFI_AP: return "AP";
    case WIFI_AP_STA: return "AP_STA";
    default: return "UNKNOWN";
  }
}

const char* authModeText(uint8_t authMode) {
  switch ((wifi_auth_mode_t)authMode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "UNKNOWN";
  }
}

const char* wlStatusText(wl_status_t st) {
  switch (st) {
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

void handleArduinoWifiEvent(arduino_event_t* event) {
  if (!g_wifiEventTarget || !event) return;
  if (event->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    g_wifiEventTarget->noteDisconnectReason(event->event_info.wifi_sta_disconnected.reason);
  }
}
} // namespace

WifiService::WifiService() {
  _ssid = appconfig::WIFI_SSID;
  _pass = appconfig::WIFI_PASS;
  _hostname = appconfig::WIFI_HOSTNAME;
  _connectTimeoutMs = appconfig::WIFI_CONNECT_TIMEOUT_MS;
  _retryIntervalMs = appconfig::WIFI_RETRY_INTERVAL_MS;
}

void WifiService::setCredentials(const char* ssid, const char* pass) {
  _ssid = ssid;
  _pass = pass;
  char line[160] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] credentials set: ssid=\"%s\" pass=%s started=%s apOnly=%s",
           safeText(_ssid),
           (_pass && _pass[0] != '\0') ? "set" : "empty",
           yesNoText(_started),
           yesNoText(_apOnlyMode));
  logLine(line);
  if (_started && _enabled && !_apOnlyMode) {
    reconnect();
  }
}

void WifiService::setHostname(const char* hostname) {
  _hostname = hostname;
  char line[96] = {0};
  snprintf(line, sizeof(line), "[WIFI] hostname set: %s", safeText(_hostname));
  logLine(line);
}

void WifiService::setTimings(uint32_t connectTimeoutMs, uint32_t retryIntervalMs) {
  _connectTimeoutMs = connectTimeoutMs;
  _retryIntervalMs = retryIntervalMs;
  char line[128] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] timings set: connectTimeout=%lums retryInterval=%lums",
           (unsigned long)_connectTimeoutMs,
           (unsigned long)_retryIntervalMs);
  logLine(line);
}

void WifiService::setLogHook(LogHook hook, void* userData) {
  _logHook = hook;
  _logHookUserData = userData;
}

bool WifiService::begin() {
  logLine("[WIFI] begin RUN requested");
  _started = true;
  _enabled = true;
  _apOnlyMode = false;
  _connecting = false;
  _wasConnected = false;
  _apActive = false;
  _clockSyncRequested = false;
  _clockSyncedFromInternet = false;
  _attemptStartMs = 0;
  _lastAttemptMs = 0;
  _lastClockSyncRequestMs = 0;
  _lastPerformanceApplyMs = 0;
  _lastLoggedStatusValid = false;
  _disconnectReasonPending = false;

#if defined(CONFIG_IDF_TARGET_ESP32C6)
  if (appconfig::WIFI_USE_EXTERNAL_ANTENNA) {
    pinMode(3, OUTPUT);
    digitalWrite(3, LOW);    // alimenta RF switch FM8625H
    pinMode(14, OUTPUT);
    digitalWrite(14, HIGH);  // seleziona antenna esterna
    logLine("[WIFI] antenna: esterna (GPIO3=LOW GPIO14=HIGH)");
  } else {
    logLine("[WIFI] antenna: interna PCB");
  }
#endif
  WiFi.persistent(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  configureCommonRadio();
  enforcePerformanceMode(true);
  attachWifiEvents();
  logRuntimeConfig("RUN");
  logStatusIfChanged(WiFi.status(), true);

  if (!_ssid || _ssid[0] == '\0') {
    logLine("[WIFI] RUN idle: nessuna credenziale salvata");
    return true;
  }

  startConnectAttempt();
  return true;
}

bool WifiService::beginApOnly() {
  logLine("[WIFI] begin PROG requested");
  _started = true;
  _enabled = true;
  _apOnlyMode = true;
  _connecting = false;
  _wasConnected = false;
  _apActive = false;
  _clockSyncRequested = false;
  _clockSyncedFromInternet = false;
  _attemptStartMs = 0;
  _lastAttemptMs = 0;
  _lastClockSyncRequestMs = 0;
  _lastPerformanceApplyMs = 0;
  _lastLoggedStatusValid = false;
  _disconnectReasonPending = false;

  WiFi.persistent(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  configureCommonRadio();
  enforcePerformanceMode(true);
  attachWifiEvents();
  logRuntimeConfig("PROG");
  logStatusIfChanged(WiFi.status(), true);

  return startProgAccessPoint();
}

void WifiService::loop() {
  if (!_started || !_enabled) return;

  const wl_status_t st = WiFi.status();
  const bool connected = (st == WL_CONNECTED);
  const uint32_t now = millis();
  logStatusIfChanged(st, false);
  logPendingDisconnectReason();

  if ((uint32_t)(now - _lastPerformanceApplyMs) >= appconfig::WIFI_PERFORMANCE_REAPPLY_INTERVAL_MS) {
    enforcePerformanceMode(false);
  }

  if (connected) {
    if (!_wasConnected) {
      _wasConnected = true;
      _connecting = false;
      enforcePerformanceMode(true);
      logConnectedInfo();
    }
    requestClockSync();
    return;
  }

  if (_wasConnected) {
    _wasConnected = false;
    _connecting = false;
    _lastAttemptMs = now;
    logLine("[WIFI] disconnected after established connection");
  }

  if (_connecting && (uint32_t)(now - _attemptStartMs) >= _connectTimeoutMs) {
    _connecting = false;
    WiFi.disconnect(false, false);
    char line[160] = {0};
    const uint32_t elapsed = (uint32_t)(now - _attemptStartMs);
    const uint32_t sinceAttempt = (uint32_t)(now - _lastAttemptMs);
    const uint32_t retryLeft = (sinceAttempt >= _retryIntervalMs) ? 0 : (_retryIntervalMs - sinceAttempt);
    snprintf(line, sizeof(line),
             "[WIFI] connect timeout: elapsed=%lums timeout=%lums nextRetryIn=%lums status=%s",
             (unsigned long)elapsed,
             (unsigned long)_connectTimeoutMs,
             (unsigned long)retryLeft,
             wlStatusText(WiFi.status()));
    logLine(line);
  }

  if (_apOnlyMode) return;
  if (_connecting) return;
  if (!_ssid || _ssid[0] == '\0') return;
  if ((uint32_t)(now - _lastAttemptMs) < _retryIntervalMs) return;

  startConnectAttempt();
}

bool WifiService::isConnected() const {
  return (WiFi.status() == WL_CONNECTED);
}

String WifiService::ip() const {
  if (isConnected()) return WiFi.localIP().toString();
  if (_apActive) return WiFi.softAPIP().toString();
  return String("");
}

int WifiService::rssi() const {
  if (!isConnected()) return 0;
  return (int)WiFi.RSSI();
}

bool WifiService::isApFallbackActive() const {
  return _apActive;
}

bool WifiService::enabled() const {
  return _enabled;
}

wl_status_t WifiService::status() const {
  return WiFi.status();
}

const char* WifiService::statusText() const {
  if (_apActive && !isConnected()) return "AP_PROG";
  return wlStatusText(WiFi.status());
}

void WifiService::ipToString(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;
  const String s = ip();
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

  const wifi_mode_t restoreMode = WiFi.getMode();
  char line[192] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] scan start: restoreMode=%s apActive=%s connected=%s maxCount=%u",
           wifiModeText(restoreMode),
           yesNoText(_apActive),
           yesNoText(isConnected()),
           (unsigned)maxCount);
  logLine(line);

  if (_apActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  configureCommonRadio();
  snprintf(line, sizeof(line), "[WIFI] scan radio mode: %s", wifiModeText(WiFi.getMode()));
  logLine(line);

  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    snprintf(line, sizeof(line), "[WIFI] scan complete: rawFound=%d used=0", found);
    logLine(line);
    WiFi.scanDelete();
    if (_apOnlyMode && !isConnected()) WiFi.mode(WIFI_AP);
    else WiFi.mode(restoreMode);
    configureCommonRadio();
    snprintf(line, sizeof(line), "[WIFI] scan restore mode: %s", wifiModeText(WiFi.getMode()));
    logLine(line);
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

    const int32_t rssiValue = WiFi.RSSI(i);
    const wifi_auth_mode_t enc = WiFi.encryptionType(i);

    if (existing >= 0) {
      if (rssiValue > out[existing].rssi) {
        strncpy(out[existing].ssid, ssid.c_str(), sizeof(out[existing].ssid) - 1);
        out[existing].ssid[sizeof(out[existing].ssid) - 1] = '\0';
        out[existing].rssi = rssiValue;
        out[existing].encryption = (uint8_t)enc;
        out[existing].hidden = false;
      }
      continue;
    }

    strncpy(out[used].ssid, ssid.c_str(), sizeof(out[used].ssid) - 1);
    out[used].ssid[sizeof(out[used].ssid) - 1] = '\0';
    out[used].rssi = rssiValue;
    out[used].encryption = (uint8_t)enc;
    out[used].hidden = false;
    used++;
  }

  snprintf(line, sizeof(line), "[WIFI] scan complete: rawFound=%d used=%u", found, (unsigned)used);
  logLine(line);
  for (uint8_t i = 0; i < used; i++) {
    snprintf(line, sizeof(line),
             "[WIFI] scan result %u: ssid=\"%s\" rssi=%ld auth=%s hidden=%s",
             (unsigned)(i + 1),
             out[i].ssid,
             (long)out[i].rssi,
             authModeText(out[i].encryption),
             yesNoText(out[i].hidden));
    logLine(line);
  }

  WiFi.scanDelete();
  if (_apOnlyMode && !isConnected()) WiFi.mode(WIFI_AP);
  else WiFi.mode(restoreMode);
  configureCommonRadio();
  snprintf(line, sizeof(line), "[WIFI] scan restore mode: %s", wifiModeText(WiFi.getMode()));
  logLine(line);
  return used;
}

void WifiService::reconnect() {
  if (!_started || !_enabled) return;

  char line[160] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] reconnect requested: ssid=\"%s\" currentStatus=%s currentMode=%s",
           safeText(_ssid),
           wlStatusText(WiFi.status()),
           wifiModeText(WiFi.getMode()));
  logLine(line);

  _connecting = false;
  _wasConnected = false;
  _clockSyncRequested = false;
  _clockSyncedFromInternet = false;
  _lastClockSyncRequestMs = 0;
  _lastLoggedStatusValid = false;
  _disconnectReasonPending = false;

  if (_apOnlyMode && _apActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  configureCommonRadio();
  WiFi.disconnect(false, false);
  logStatusIfChanged(WiFi.status(), true);

  if (!_ssid || _ssid[0] == '\0') {
    logLine("[WIFI] reconnect stopped: credentials cleared");
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

void WifiService::noteDisconnectReason(uint16_t reason) {
  _lastDisconnectReason = reason;
  _disconnectReasonPending = true;
}

void WifiService::attachWifiEvents() {
#if defined(ARDUINO_ARCH_ESP32)
  if (_eventsAttached) return;
  g_wifiEventTarget = this;
  WiFi.onEvent(handleArduinoWifiEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  _eventsAttached = true;
  logLine("[WIFI] disconnect event handler attached");
#endif
}

void WifiService::configureCommonRadio() {
  WiFi.setAutoReconnect(false);
  enforcePerformanceMode(false);
  if (_hostname && _hostname[0] != '\0') {
    WiFi.setHostname(_hostname);
  }
}

void WifiService::enforcePerformanceMode(bool logResult) {
  const uint32_t now = millis();
  _lastPerformanceApplyMs = now;

  const bool sleepOk = WiFi.setSleep(WIFI_PS_NONE);
  const bool txOk = WiFi.setTxPower((wifi_power_t)appconfig::WIFI_TX_POWER);
  bool psOk = true;
  bool legacyOk = true;
#if defined(ARDUINO_ARCH_ESP32)
  psOk = (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK);
  if (appconfig::WIFI_FORCE_LEGACY_24G) {
    // ESP32C6 e' un dispositivo WiFi 6 (802.11ax): includere 11AX nel mask
    // evita che la restrizione di protocollo impedisca la connessione su AP moderni.
#if defined(CONFIG_IDF_TARGET_ESP32C6)
    const uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
#else
    const uint8_t protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
#endif
    const wifi_mode_t currentMode = WiFi.getMode();
    legacyOk = (esp_wifi_set_protocol(WIFI_IF_STA, protocol) == ESP_OK);
    legacyOk = legacyOk && (esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20) == ESP_OK);
    // Impostare WIFI_IF_AP in modalita' STA-only causa ESP_ERR_WIFI_IF su ESP32C6
    // e puo' corrompere lo stato dello stack WiFi.
    if (currentMode == WIFI_AP || currentMode == WIFI_AP_STA) {
      legacyOk = legacyOk && (esp_wifi_set_protocol(WIFI_IF_AP, protocol) == ESP_OK);
      legacyOk = legacyOk && (esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20) == ESP_OK);
    }
  }
#endif

  if (logResult) {
    char line[192] = {0};
    snprintf(line, sizeof(line),
             "[WIFI] performance mode: sleepOff=%s espPsOff=%s txPower=%d txOk=%s legacy24=%s legacyOk=%s",
             yesNoText(sleepOk),
             yesNoText(psOk),
             (int)WiFi.getTxPower(),
             yesNoText(txOk),
             yesNoText(appconfig::WIFI_FORCE_LEGACY_24G),
             yesNoText(legacyOk));
    logLine(line);
  }
}

void WifiService::logRuntimeConfig(const char* mode) {
  char line[192] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] config: mode=%s hostname=\"%s\" ssid=\"%s\" pass=%s timeout=%lums retry=%lums persistent=off autoReconnect=off sleep=off txPower=%d legacy24=%s radioMode=%s",
           safeText(mode),
           safeText(_hostname),
           safeText(_ssid),
           (_pass && _pass[0] != '\0') ? "set" : "empty",
           (unsigned long)_connectTimeoutMs,
           (unsigned long)_retryIntervalMs,
           (int)WiFi.getTxPower(),
           yesNoText(appconfig::WIFI_FORCE_LEGACY_24G),
           wifiModeText(WiFi.getMode()));
  logLine(line);
}

void WifiService::logStatusIfChanged(wl_status_t st, bool force) {
  if (!force && _lastLoggedStatusValid && st == _lastLoggedStatus) return;
  _lastLoggedStatus = st;
  _lastLoggedStatusValid = true;

  char line[160] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] status: %s (%d) mode=%s connecting=%s connected=%s ap=%s",
           wlStatusText(st),
           (int)st,
           wifiModeText(WiFi.getMode()),
           yesNoText(_connecting),
           yesNoText(st == WL_CONNECTED),
           yesNoText(_apActive));
  logLine(line);
}

void WifiService::logPendingDisconnectReason() {
  if (!_disconnectReasonPending) return;
  const uint16_t reason = _lastDisconnectReason;
  _disconnectReasonPending = false;

  char line[192] = {0};
#if defined(ARDUINO_ARCH_ESP32)
  snprintf(line, sizeof(line),
           "[WIFI] disconnect event: reason=%u name=%s status=%s mode=%s",
           (unsigned)reason,
           safeText(WiFi.disconnectReasonName((wifi_err_reason_t)reason)),
           wlStatusText(WiFi.status()),
           wifiModeText(WiFi.getMode()));
#else
  snprintf(line, sizeof(line),
           "[WIFI] disconnect event: reason=%u status=%s mode=%s",
           (unsigned)reason,
           wlStatusText(WiFi.status()),
           wifiModeText(WiFi.getMode()));
#endif
  logLine(line);
}

void WifiService::startConnectAttempt() {
  if (!_enabled) return;
  if (!_ssid || _ssid[0] == '\0') return;

  if (_apOnlyMode && _apActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  configureCommonRadio();

  _connecting = true;
  _attemptStartMs = millis();
  _lastAttemptMs = _attemptStartMs;

  char line[192] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] connect attempt: ssid=\"%s\" pass=%s mode=%s timeout=%lums retry=%lums",
           safeText(_ssid),
           (_pass && _pass[0] != '\0') ? "set" : "empty",
           wifiModeText(WiFi.getMode()),
           (unsigned long)_connectTimeoutMs,
           (unsigned long)_retryIntervalMs);
  logLine(line);

  wl_status_t beginStatus = WL_IDLE_STATUS;
  if (_pass && _pass[0] != '\0') {
    beginStatus = WiFi.begin(_ssid, _pass);
  } else {
    beginStatus = WiFi.begin(_ssid);
  }
  snprintf(line, sizeof(line), "[WIFI] WiFi.begin returned: %s (%d)",
           wlStatusText(beginStatus),
           (int)beginStatus);
  logLine(line);
  logStatusIfChanged(WiFi.status(), true);
}

bool WifiService::startProgAccessPoint() {
  const IPAddress apIp(192, 168, 4, 1);
  const IPAddress apGateway(192, 168, 4, 1);
  const IPAddress apMask(255, 255, 255, 0);
  char line[192] = {0};
  snprintf(line, sizeof(line),
           "[WIFI] PROG AP config: ip=%s gateway=%s mask=%s channel=%u",
           safeText(apIp.toString().c_str()),
           safeText(apGateway.toString().c_str()),
           safeText(apMask.toString().c_str()),
           (unsigned)appconfig::WIFI_AP_CHANNEL);
  logLine(line);

  const bool configOk = WiFi.softAPConfig(apIp, apGateway, apMask);
  snprintf(line, sizeof(line), "[WIFI] PROG AP softAPConfig: %s", configOk ? "ok" : "failed");
  logLine(line);

  const char* apSsid = appconfig::WIFI_FALLBACK_AP_SSID;
  const char* apPass = appconfig::WIFI_FALLBACK_AP_PASS;
  bool ok = false;
  if (apPass && strlen(apPass) >= 8) {
    ok = WiFi.softAP(apSsid, apPass, appconfig::WIFI_AP_CHANNEL);
  } else {
    ok = WiFi.softAP(apSsid, nullptr, appconfig::WIFI_AP_CHANNEL);
  }

  if (!ok) {
    logLine("[WIFI] PROG AP start failed");
    return false;
  }

  _apActive = true;
  logLine("[WIFI] PROG AP active");

  snprintf(line, sizeof(line), "[WIFI] AP SSID: %s", safeText(apSsid));
  logLine(line);
  snprintf(line, sizeof(line), "[WIFI] AP IP: %s", safeText(WiFi.softAPIP().toString().c_str()));
  logLine(line);
  snprintf(line, sizeof(line), "[WIFI] AP MAC: %s clients=%u mode=%s",
           safeText(WiFi.softAPmacAddress().c_str()),
           (unsigned)WiFi.softAPgetStationNum(),
           wifiModeText(WiFi.getMode()));
  logLine(line);
  return true;
}

void WifiService::requestClockSync() {
  const uint32_t nowMs = millis();
  if (_clockSyncRequested &&
      (uint32_t)(nowMs - _lastClockSyncRequestMs) < appconfig::NTP_RESYNC_INTERVAL_MS) {
    const time_t now = time(nullptr);
    if (isValidEpochTime(now) && !_clockSyncedFromInternet) {
      _clockSyncedFromInternet = true;
      logLine("[WIFI] clock synced from internet");
    }
    return;
  }

  configTzTime(appconfig::TIME_TZ,
               appconfig::NTP_SERVER_1,
               appconfig::NTP_SERVER_2,
               appconfig::NTP_SERVER_3);
  _clockSyncRequested = true;
  _lastClockSyncRequestMs = nowMs;
  logLine("[WIFI] NTP sync requested");

  const time_t now = time(nullptr);
  if (isValidEpochTime(now) && !_clockSyncedFromInternet) {
    _clockSyncedFromInternet = true;
    logLine("[WIFI] clock synced from internet");
  }
}

void WifiService::logLine(const char* line) {
  const char* msg = safeText(line);
  Serial.println(msg);
  _ringLog.push(msg);
  if (_logHook) _logHook(msg, _logHookUserData);
}

void WifiService::logConnectedInfo() {
  logLine("[WIFI] connected");

  char line[192] = {0};
  snprintf(line, sizeof(line), "[WIFI] SSID: %s", safeText(WiFi.SSID().c_str()));
  logLine(line);

  const String ipStr = WiFi.localIP().toString();
  snprintf(line, sizeof(line), "[WIFI] IP: %s", safeText(ipStr.c_str()));
  logLine(line);

  snprintf(line, sizeof(line), "[WIFI] NET: gateway=%s subnet=%s dns=%s",
           safeText(WiFi.gatewayIP().toString().c_str()),
           safeText(WiFi.subnetMask().toString().c_str()),
           safeText(WiFi.dnsIP(0).toString().c_str()));
  logLine(line);

  snprintf(line, sizeof(line), "[WIFI] LINK: rssi=%d channel=%d bssid=%s mac=%s",
           rssi(),
           WiFi.channel(),
           safeText(WiFi.BSSIDstr().c_str()),
           safeText(WiFi.macAddress().c_str()));
  logLine(line);

  snprintf(line, sizeof(line), "[WIFI] STATUS: %s (%d)",
           wlStatusText(WiFi.status()),
           (int)WiFi.status());
  logLine(line);
}

} // namespace ccms
