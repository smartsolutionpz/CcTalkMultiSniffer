// Scopo del file:
// implementa il servizio ESP-NOW master usato per heartbeat e telemetria locale.
#include "EspNowMasterService.h"

#include <stdio.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <esp_wifi.h>
#endif

namespace ccms {

EspNowMasterService* EspNowMasterService::s_instance = nullptr;

bool EspNowMasterService::begin(bool enabled) {
  // Resetta sempre lo stato interno, anche quando il servizio viene disabilitato.
  _enabled = enabled;
  _started = true;
  _ready = false;
  _rxCount = 0;
  _txCount = 0;
  _txFailCount = 0;
  _txSeq = 0;
  _lastHeartbeatMs = millis();
  _lastTxErrorLogMs = 0;

  if (!_enabled) {
    logLine("[MESH] ESP-NOW disabilitato");
    return false;
  }

#if defined(ARDUINO_ARCH_ESP32)
  // ESP-NOW richiede stack Wi-Fi attivo lato ESP32.
  if (esp_now_init() != ESP_OK) {
    logLine("[MESH] init ESP-NOW fallita");
    return false;
  }

  esp_now_register_send_cb(onSendStatic);

  // Viene registrato un peer broadcast, cosi ogni heartbeat puo essere inviato
  // senza conoscere in anticipo i destinatari.
  const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastMac, sizeof(broadcastMac));
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  const esp_err_t addRes = esp_now_add_peer(&peerInfo);
  if (addRes != ESP_OK && addRes != ESP_ERR_ESPNOW_EXIST) {
    logLine("[MESH] add peer broadcast fallito");
    return false;
  }

  s_instance = this;
  _ready = true;

  uint8_t channel = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&channel, &second) == ESP_OK) {
    char line[64] = {0};
    snprintf(line, sizeof(line), "[MESH] ESP-NOW pronto su canale %u", (unsigned)channel);
    logLine(line);
  } else {
    logLine("[MESH] ESP-NOW pronto");
  }
  return true;
#else
  logLine("[MESH] ESP-NOW non supportato su questo target");
  return false;
#endif
}

void EspNowMasterService::setHeartbeatIntervalMs(uint32_t heartbeatIntervalMs) {
  _heartbeatIntervalMs = (heartbeatIntervalMs == 0) ? 1000 : heartbeatIntervalMs;
}

void EspNowMasterService::setLogHook(LogHook hook, void* userData) {
  _logHook = hook;
  _logHookUserData = userData;
}

void EspNowMasterService::loop() {
  // Il servizio e volutamente leggero: decide solo quando e il momento di
  // spedire un nuovo heartbeat.
  if (!_enabled || !_started || !_ready) return;

  const uint32_t now = millis();
  if ((uint32_t)(now - _lastHeartbeatMs) < _heartbeatIntervalMs) return;
  _lastHeartbeatMs = now;
  sendHeartbeat();
}

bool EspNowMasterService::sendHeartbeat() {
#if defined(ARDUINO_ARCH_ESP32)
  // Heartbeat broadcast con numero di sequenza e uptime per debug/monitoring.
  const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  HeartbeatPacket pkt = {};
  pkt.version = 1;
  pkt.msgType = 1;
  pkt.reserved = 0;
  pkt.seq = ++_txSeq;
  pkt.uptimeMs = millis();

  const esp_err_t err = esp_now_send(broadcastMac, (const uint8_t*)&pkt, sizeof(pkt));
  if (err != ESP_OK) {
    _txFailCount++;
    const uint32_t now = millis();
    if ((uint32_t)(now - _lastTxErrorLogMs) >= 10000) {
      _lastTxErrorLogMs = now;
      logLine("[MESH] heartbeat TX fallito");
    }
    return false;
  }

  _txCount++;
  return true;
#else
  return false;
#endif
}

#if defined(ARDUINO_ARCH_ESP32)
void EspNowMasterService::onSendInternal(const uint8_t* macAddr, esp_now_send_status_t status) {
  (void)macAddr;
  // Il callback segnala solo l'esito di trasmissione al livello ESP-NOW.
  if (status != ESP_NOW_SEND_SUCCESS) {
    _txFailCount++;
  }
}

void EspNowMasterService::onSendStatic(const uint8_t* macAddr, esp_now_send_status_t status) {
  if (!s_instance) return;
  s_instance->onSendInternal(macAddr, status);
}
#else
void EspNowMasterService::onSendInternal(const uint8_t* macAddr, int status) {
  (void)macAddr;
  (void)status;
}

void EspNowMasterService::onSendStatic(const uint8_t* macAddr, int status) {
  if (!s_instance) return;
  s_instance->onSendInternal(macAddr, status);
}
#endif

void EspNowMasterService::logLine(const char* line) {
  if (_logHook) _logHook(line ? line : "", _logHookUserData);
}

} // namespace ccms
