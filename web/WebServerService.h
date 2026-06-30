// Scopo del file:
// dichiara `WebServerService`, il server HTTP embedded che espone dashboard,
// API di stato e pagina impostazioni.
#ifndef CCTALK_MULTI_SNIFFER_WEB_WEB_SERVER_SERVICE_H
#define CCTALK_MULTI_SNIFFER_WEB_WEB_SERVER_SERVICE_H

#include <Arduino.h>
#include <WebServer.h>

#include "../status/SystemStatus.h"
#include "../net/WifiService.h"
#include "../AppSettings.h"

namespace ccms {

// Web server embedded che espone:
// - dashboard di stato
// - API JSON per log e metriche
// - pagina impostazioni in modalita PROG
class WebServerService {
public:
  enum UiMode : uint8_t {
    UI_MODE_STATUS = 0,
    UI_MODE_PROG = 1
  };

  typedef bool (*ResetCountersCallback)(String& message, void* userData);
  typedef bool (*SetCoinBaseCallback)(int64_t coinLevelBaseCents, String& message, void* userData);
  typedef bool (*SetBillRecyclerBaseCallback)(int64_t cassette10Count,
                                              int64_t cassette20Count,
                                              int64_t cassette50Count,
                                              String& message,
                                              void* userData);
  typedef bool (*SaveRemoteSnapshotCallback)(String& message, void* userData);
  typedef bool (*GetSettingsCallback)(AppSettings& out, String& message, void* userData);
  typedef bool (*GetPresentPeripheralCatalogCallback)(bool& coinAcceptorPresent,
                                                      uint8_t& hopperMask,
                                                      uint16_t& billValidatorMask,
                                                      String& unknownDevicesCsv,
                                                      String& detectedDevicesJson,
                                                      void* userData);
  typedef bool (*SaveSettingsCallback)(const AppSettings& in, String& message, void* userData);
  typedef bool (*TestConnectionCallback)(const AppSettings& in, String& message, void* userData);
  typedef bool (*WifiTestCallback)(const char* ssid, const char* pass, String& message, void* userData);
  typedef bool (*EnterProgModeCallback)(String& message, void* userData);

  explicit WebServerService(SystemStatus& status, WifiService& wifi, uint16_t port = 80);

  void begin();
  void loop();
  // Cambia il profilo UI esposto all'utente.
  void setUiMode(UiMode mode);
  // Registra le callback applicative per azioni runtime.
  void setActions(ResetCountersCallback onResetCounters,
                  SetCoinBaseCallback onSetCoinBase,
                  SetBillRecyclerBaseCallback onSetBillRecyclerBase,
                  SaveRemoteSnapshotCallback onSaveRemoteSnapshot,
                  void* userData);
  void setSettingsActions(GetSettingsCallback onGetSettings,
                          GetPresentPeripheralCatalogCallback onGetPresentPeripheralCatalog,
                          SaveSettingsCallback onSaveSettings,
                          TestConnectionCallback onTestConnection,
                          void* userData);
  void setEnterProgModeAction(EnterProgModeCallback cb, void* userData);
  void setWifiTestAction(WifiTestCallback cb, void* userData);

private:
  // Handler HTML/API.
  void handleRoot();
  void handleStatusPage();
  void handleSettingsPage();
  void handleAppCss();
  void handleHealth();
  void handleApiStatus();
  void handleApiLogs();
  void handleApiWifiNetworks();
  void handleApiResetCounters();
  void handleApiSetCoinBase();
  void handleApiSetBillRecyclerBase();
  void handleApiSaveRemoteSnapshot();
  void handleApiGetSettings();
  void handleApiSaveSettings();
  void handleApiTestConnection();
  void handleApiWifiTest();
  void handleApiEnterProgMode();
  void appendSettingsJson(String& out,
                          const AppSettings& settings,
                          bool presentCoinAcceptor,
                          uint8_t presentHopperMask,
                          uint16_t presentBillValidatorMask,
                          const String& unknownDevicesCsv,
                          const String& detectedDevicesJson);
  // Estrae e valida i parametri della form impostazioni.
  bool parseSettingsFromRequest(AppSettings& out, String& message);
  void appendLogsArrayJson(String& out, uint16_t limit);
  static void appendJsonEscaped(String& out, const char* value);

  SystemStatus& _status;
  WifiService& _wifi;
  WebServer _server;
  UiMode _uiMode = UI_MODE_STATUS;
  bool _started = false;
  ResetCountersCallback _onResetCounters = nullptr;
  SetCoinBaseCallback _onSetCoinBase = nullptr;
  SetBillRecyclerBaseCallback _onSetBillRecyclerBase = nullptr;
  SaveRemoteSnapshotCallback _onSaveRemoteSnapshot = nullptr;
  void* _actionsUserData = nullptr;
  GetSettingsCallback _onGetSettings = nullptr;
  GetPresentPeripheralCatalogCallback _onGetPresentPeripheralCatalog = nullptr;
  SaveSettingsCallback _onSaveSettings = nullptr;
  TestConnectionCallback _onTestConnection = nullptr;
  void* _settingsUserData = nullptr;
  EnterProgModeCallback _onEnterProgMode = nullptr;
  void* _enterProgModeUserData = nullptr;
  WifiTestCallback _onWifiTest = nullptr;
  void* _wifiTestUserData = nullptr;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_WEB_WEB_SERVER_SERVICE_H
