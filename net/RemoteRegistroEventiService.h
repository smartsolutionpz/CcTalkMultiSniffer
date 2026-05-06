// Scopo del file:
// dichiara `RemoteRegistroEventiService`, il servizio che accoda eventi locali
// e li invia a una API HTTP remota che persiste su MySQL Aruba.
#ifndef CCTALK_MULTI_SNIFFER_NET_REMOTE_REGISTRO_EVENTI_SERVICE_H
#define CCTALK_MULTI_SNIFFER_NET_REMOTE_REGISTRO_EVENTI_SERVICE_H

#include <Arduino.h>
#include <stdint.h>

#include "../AppSettings.h"
#include "../status/RingLog.h"
#include "../status/SystemStatus.h"
#include "WifiService.h"

namespace ccms {

class RemoteRegistroEventiService {
public:
  typedef void (*LogHook)(const char* line, void* userData);
  typedef bool (*RequestApplyHook)(const char* command,
                                   const char* requestPayload,
                                   String& responseMessage,
                                   void* userData);
  static const uint8_t kQueueSize = 8;

  RemoteRegistroEventiService(SystemStatus& status, WifiService& wifi);

  void setEndpointUrl(const char* url);
  void setLocationCode(const char* code);
  void setApiKey(const char* key);
  void setEnabled(bool enabled);
  void setRequestTimeoutMs(uint16_t timeoutMs);
  void setRetryIntervalMs(uint32_t retryIntervalMs);
  void setDbPollIntervalMs(uint32_t pollIntervalMs);
  void setLogHook(LogHook hook, void* userData);
  void setRequestApplyHook(RequestApplyHook hook, void* userData);

  bool begin();
  void loop();
  void noteEvent(const char* line, bool decoded);
  bool noteChangeEvent();
  bool saveChangeEventNow(String& message);

  bool enabled() const { return _enabled; }
  bool active() const { return _active; }
  uint8_t queuedCount() const;
  uint32_t successCount() const { return _successCount; }
  uint32_t failureCount() const { return _failureCount; }
  uint32_t droppedCount() const { return _droppedCount; }

private:
  enum RequestProtocolState : uint8_t {
    REQUEST_STATE_IDLE = 0,
    REQUEST_STATE_REQUEST_SEEN = 1,
    REQUEST_STATE_RESPONSE_POSTED = 2
  };

  struct QueuedEvent {
    char operation[32];
    char description[RingLog::kLineSize];
    bool descriptionIsNull = false;
    int32_t value1 = 0;
    int32_t value2 = 0;
    int32_t value3 = 0;
    int32_t value4 = 0;
    int32_t value5 = 0;
    int32_t value6 = 0;
    char note1[256];
    bool note1IsNull = false;
    char note2[96];
    bool note2IsNull = false;
    char note3[96];
    bool note3IsNull = false;
    char note4[96];
    bool note4IsNull = false;
    char device[96];
  };

  struct PendingRequest {
    uint32_t requestId = 0;
    char command[32];
    char requestPayload[256];
    char requestState[24];
    char requestTime[32];

    PendingRequest() {
      command[0] = '\0';
      requestPayload[0] = '\0';
      requestState[0] = '\0';
      requestTime[0] = '\0';
    }
  };

  void updateActiveFlag();
  void logLine(const char* line);
  void buildQueuedEvent(const char* line, bool decoded, QueuedEvent& out) const;
  void buildChangeEvent(QueuedEvent& out) const;
  void buildRecyclerInventoryNote(char* out, size_t outLen) const;
  bool postEvent(const QueuedEvent& event, String* responseMessage = nullptr);
  bool postResponse(const PendingRequest& request,
                    bool requestApplied,
                    const char* responseMessage,
                    String* serverResponseMessage = nullptr);
  bool saveChangeEventInternal(const char* trigger, String& message);
  bool fetchPendingRequest(PendingRequest& out, String& responseMessage);
  void buildJsonPayload(const QueuedEvent& event, String& out) const;
  void buildResponseJsonPayload(const PendingRequest& request,
                                bool requestApplied,
                                const char* responseMessage,
                                String& out) const;
  void buildRequestSignature(const PendingRequest& request, char* out, size_t outLen) const;
  static void copyBounded(const char* in, char* out, size_t outLen);
  static void copyBounded(const String& in, char* out, size_t outLen);
  static void appendJsonEscaped(String& out, const char* value);
  static void appendJsonNullableString(String& out, const char* value, bool isNull);
  static void appendUrlEncoded(String& out, const char* value);
  static bool responseHasBoolTrue(const String& body, const char* key);
  static bool responseHasBoolFalse(const String& body, const char* key);
  static bool responseHasNullValue(const String& body, const char* key);
  static long responseLongValue(const String& body, const char* key, long defaultValue = -1);
  static String responseStringValue(const String& body, const char* key);
  static bool responseHasSuccessTrue(const String& body);
  static long responseInsertId(const String& body);
  static String responseMessageText(const String& body);
  static bool shouldQueueEvent(const char* line, bool decoded);
  static void classifyOperation(const char* line, bool decoded, char* out, size_t outLen);
  static void buildDeviceLabel(char* out, size_t outLen);

  SystemStatus& _status;
  WifiService& _wifi;
  char _endpoint[AppSettings::kRemoteEventUrlSize] = {0};
  char _locationCode[AppSettings::kLocationCodeSize] = {0};
  char _apiKey[AppSettings::kApiKeySize] = {0};
  uint16_t _timeoutMs = 1200;
  uint32_t _retryIntervalMs = 5000;
  bool _started = false;
  bool _enabled = true;
  bool _active = false;
  uint32_t _lastAttemptMs = 0;
  uint32_t _lastDbPollMs = 0;
  uint32_t _dbPollIntervalMs = 10000;
  uint32_t _successCount = 0;
  uint32_t _failureCount = 0;
  uint32_t _droppedCount = 0;
  bool _lastWifiConnected = false;
  RequestProtocolState _requestState = REQUEST_STATE_IDLE;
  uint32_t _lastHandledRequestId = 0;
  char _lastHandledRequestSignature[384] = {0};
  char _pendingAppliedRequestSignature[384] = {0};
  bool _pendingAppliedRequestOk = false;
  char _pendingAppliedResponseMessage[160] = {0};

  LogHook _logHook = nullptr;
  void* _logHookUserData = nullptr;
  RequestApplyHook _requestApplyHook = nullptr;
  void* _requestApplyUserData = nullptr;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_NET_REMOTE_REGISTRO_EVENTI_SERVICE_H
