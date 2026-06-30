/*
  ============================================================================
  CcTalkMultiSniffer.ino
  ============================================================================

  Scopo del file:
  orchestrare l'intera applicazione: bootstrap, sniffing ccTalk, servizi di
  rete, persistenza, dashboard web e sincronizzazione dello stato globale.

  Questo sketch realizza un "multi sniffer" per bus ccTalk su ESP32.
  Il file ha quattro responsabilita principali:

  1. Inizializzare l'hardware e i servizi di supporto
     - seriale di log
     - rete Wi-Fi / web UI
     - pubblicazione cloud / mesh
     - persistenza FRAM

  2. Sniffare il traffico ccTalk
     - intercettare richieste e risposte sul bus
     - distinguere frame completi e messaggi speciali MDCES
     - aggiornare lo stato interno dei dispositivi osservati

  3. Mantenere uno stato applicativo coerente
     - stato runtime dei dispositivi (hopper, bill validator, coin acceptor)
     - stato economico aggregato
     - contatori persistenti ripristinati da FRAM

  4. Esporre le informazioni raccolte
     - log seriale per debug/manual testing
     - dashboard web
     - servizi cloud e mesh

  Le modifiche introdotte in questa versione sono volutamente conservative:
  l'obiettivo e rendere il file piu leggibile e didattico senza cambiare
  il comportamento funzionale del firmware.
*/

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <Preferences.h>
#endif

#include "CcTalkBusSniffer.h"
#include "CcTalkRouter.h"

#include "CcTalkCoinAcceptorNriFalcon.h"
#include "CcTalkHopperAlbericiDiscriminator.h"
#include "CcTalkHopperAlbericiHopperCd.h"
#include "CcTalkHopperAzkoyenDiscriminator.h"
#include "CcTalkHopperSuzoEvolution.h"
#include "CcTalkBillValidatorIPRO.h"
#include "CcTalkBillValidatorMD100.h"
#include "CcTalkBillValidatorSmartPayout.h"
#include "Config.h"
#include "AppSettingsStore.h"
#include "status/FramPersistence.h"
#include "status/SystemStatus.h"
#include "net/WifiService.h"
#include "net/CloudPublisherService.h"
#include "net/RemoteRegistroEventiService.h"
#include "mesh/EspNowMasterService.h"
#include "web/WebServerService.h"

static_assert(appconfig::TASK_PRIORITY_SNIFFER >= appconfig::TASK_PRIORITY_NET,
              "ccTalk sniffer priority must be at least auxiliary services priority");

// Velocita della seriale usata per il log locale e per i comandi da console.
static const uint32_t LOG_BAUD = 115200;

// Test manuali sintetici del bill validator.
// Rimane disabilitato in produzione per non iniettare traffico/log non richiesti.
static const bool RUN_BV_MANUAL_TESTS = false;
#ifndef ENABLE_SERIAL_LOG
#define ENABLE_SERIAL_LOG 1
#endif

// Modalita di visualizzazione del monitor seriale.
// Ogni modalita cambia il livello di dettaglio stampato, ma non il modo
// in cui lo stato interno viene aggiornato.
enum ViewMode : uint8_t {
  VIEW_ECONOMIC_COUNTERS    = 1,
  VIEW_ALL_RAW              = 2,
  VIEW_INFO_AND_COUNTER_INC = 3,
  VIEW_ANOMALIES            = 4   // solo frame con checksum errato o indirizzi non noti
};

// Modalita di boot del firmware:
// - RUN: esecuzione normale con servizi attivi
// - PROG: modalita configurazione, con access point WiFi
// - PROG_NO_AP: modalita configurazione, WiFi STA (senza access point)
enum BootMode : uint8_t {
  BOOT_MODE_RUN = 0,
  BOOT_MODE_PROG = 1,
  BOOT_MODE_PROG_NO_AP = 2
};

// Stream "muto": implementa l'interfaccia Stream ma scarta tutto l'output.
// Viene usato quando vogliamo aggiornare lo stato dei parser senza produrre log.
class NullStream : public Stream {
public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t b) override { (void)b; return 1; }
};

// Stream di log runtime.
// Duplica opzionalmente l'output verso la seriale e, in parallelo, accumula
// ogni riga completa per inoltrarla allo stato di sistema.
class RuntimeLogStream : public Stream {
public:
  RuntimeLogStream(ccms::SystemStatus& status, Stream* serialOut)
    : _status(status), _serialOut(serialOut) {}

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override { flushPendingLine(); }

  size_t write(uint8_t b) override {
#if ENABLE_SERIAL_LOG
    if (_serialOut) _serialOut->write(b);
#endif

    // '\r' viene ignorato per uniformare la gestione delle righe.
    if (b == '\r') return 1;
    if (b == '\n') {
      flushPendingLine();
      return 1;
    }

    // Accoda i caratteri fino alla dimensione massima prevista dal ring log.
    if (_lineLen < (sizeof(_line) - 1)) {
      _line[_lineLen++] = (char)b;
      _line[_lineLen] = '\0';
    }
    return 1;
  }

  void flushPendingLine() {
    if (_lineLen == 0) return;

    // Ogni riga completa viene salvata nel log di sistema e classificata
    // come evento decodificato, cosi la dashboard puo riutilizzarla.
    _status.logLine(_line, false);
    _status.noteDecodedEvent(_line);
    _lineLen = 0;
    _line[0] = '\0';
  }

private:
  ccms::SystemStatus& _status;
  Stream* _serialOut = nullptr;
  char _line[ccms::RingLog::kLineSize] = {0};
  size_t _lineLen = 0;
};

// Tiene traccia dell'ultimo contatore osservato per decidere se un evento
// rappresenta un avanzamento reale o una semplice ripetizione del polling.
struct CounterTracker {
  bool valid = false;
  uint8_t last = 0;
};

// Cache per i comandi di polling hopper che trasportano piu grandezze:
// rimanente, pagato e non pagato.
struct HopperPollTracker {
  bool valid = false;
  uint16_t remaining = 0;
  uint16_t paid = 0;
  uint16_t unpaid = 0;
};

// Variante compatta del tracker A6 per hopper che usano il pacchetto status
// come contatore pagamenti + stato type1 dell'ultimo payout.
struct HopperStatusTracker {
  bool valid = false;
  uint8_t remaining = 0;
  uint8_t paid = 0;
  uint8_t unpaid = 0;
};

// Cache minimale per confrontare payload di status per indirizzo senza
// dipendere dal parser/configurazione del device.
struct PayloadTracker {
  bool valid = false;
  uint8_t len = 0;
  uint8_t data[16] = {0};
};

// Aggregato economico espresso interamente in centesimi.
// L'uso dei centesimi evita errori dovuti a floating point e rende piu
// prevedibile la persistenza dei valori.
struct EconomicTotals {
  uint32_t cntotBanconoteInCents = 0;
  uint32_t cntotMoneteOutCents = 0;
  uint32_t cntotMoneteInCents = 0;
  uint32_t cntotBanconoteOutCents = 0;
  uint32_t cassaCents = 0;
  uint32_t recyclerInventoryCents = 0;
};

// ============================================================================
// Stato globale applicativo
// ============================================================================
// In un firmware Arduino il file .ino agisce spesso da "composition root":
// qui vengono istanziati i servizi principali e il loro stato condiviso.

static NullStream g_nullOut;
static CounterTracker g_coinEventTrack;
static CounterTracker g_hopperEventTrack[8];   // addr 3..10
static HopperPollTracker g_hopperPollTrack[8]; // addr 3..10
static HopperStatusTracker g_hopperStatusTrack[8]; // addr 3..10, A6 Azkoyen
static CounterTracker g_bvEventTrack[11];      // addr 40..50
static PayloadTracker g_bvStatusTrack[11];     // addr 40..50
static bool g_mdcesSeen[256] = {false};
static bool g_infoSeen[256][15] = {{false}};
static bool g_deviceSeen[256] = {false};
static uint16_t g_detectedDeviceCount = 0;
static ViewMode g_viewMode = VIEW_INFO_AND_COUNTER_INC;
static BootMode g_bootMode = BOOT_MODE_RUN;
static inline bool isProgMode() {
  return g_bootMode == BOOT_MODE_PROG || g_bootMode == BOOT_MODE_PROG_NO_AP;
}

static bool g_economicTotalsValid = false;
static EconomicTotals g_lastEconomicTotals;
static EconomicTotals g_persistentBaseTotals;
static EconomicTotals g_sessionOffsetTotals;
// Accumulatore crediti banconote JCM push-mode: il BV JCM (checksumOk=false)
// bypassa il routing normale. Viene aggiunto direttamente in collectSessionEconomicRaw().
static uint32_t g_jcmBillCreditCents = 0;
static uint32_t g_coinLevelBaseCents = 0;
static ccms::AppSettingsStore g_settingsStore;
static ccms::AppSettings g_appSettings;
static ccms::AppSettings g_runtimeDeviceSettings;
static bool g_settingsLoaded = false;
static ccms::SystemStatus g_systemStatus;
static ccms::WifiService g_wifi;
static ccms::WebServerService g_web(g_systemStatus, g_wifi);
static ccms::CloudPublisherService g_cloud(g_systemStatus, g_wifi);
static ccms::RemoteRegistroEventiService g_remoteRegistro(g_systemStatus, g_wifi);
static ccms::EspNowMasterService g_mesh;
static RuntimeLogStream g_runtimeOut(g_systemStatus, &Serial);
static ccms::FramPersistence g_framStore;
static ccms::SystemStatus::RecyclerInventoryEntry g_persistentRecycler[ccms::SystemStatus::kMaxRecyclerEntries];
static uint8_t g_persistentRecyclerCount = 0;
static char g_remoteMasterRuntimeCommand[32] = {0};
static char g_remoteMasterRuntimePayload[256] = {0};
static uint32_t g_remoteMasterRuntimeAppliedMs = 0;
static bool g_framReady = false;
static bool g_framDirty = false;
static volatile uint32_t g_framDirtyGeneration = 0;
static bool g_framSaveErrorLatched = false;
static bool g_framCongruenceMismatchLatched = false;
static bool g_framCongruencePendingLatched = false;
static uint32_t g_lastFramWriteMs = 0;
static uint32_t g_lastFramCongruenceCheckMs = 0;
static uint32_t g_lastSnifferCycleStartUs = 0;
static uint32_t g_lastAuxServiceMs = 0;
static uint32_t g_lastPersistServiceMs = 0;
static uint32_t g_wifiStableSinceMs = 0;
static uint32_t g_lastMeshStartAttemptMs = 0;
static volatile uint32_t g_lastCcTalkActivityMs = 0;
static bool g_progButtonPrevPressed = false;
static bool g_meshStartupDeferredLogged = false;
static uint8_t g_gpioExpanderPort = 0xFF; // stato porta PCF8574: tutti i pin alti (LED off, button input)
// Override del boot mode: persistito su NVS per sopravvivere a qualsiasi tipo
// di reset (incluso SW_CPU_RESET su ESP32-C6 dove RTC_DATA_ATTR non e affidabile).
static int8_t g_bootModeOverride = -1;

#if defined(ARDUINO_ARCH_ESP32)
static TaskHandle_t g_snifferTaskHandle = nullptr;
static TaskHandle_t g_auxTaskHandle = nullptr;
static bool g_snifferTaskRunning = false;
static bool g_auxTaskRunning = false;
static portMUX_TYPE g_framDirtyMux = portMUX_INITIALIZER_UNLOCKED;
#endif

// ============================================================================
// Istanze device ccTalk
// ============================================================================
CcTalkCoinAcceptorNriFalcon g_coin;
CcTalkHopperAlbericiDiscriminator g_hopperAlbericiDiscriminator;
CcTalkHopperAlbericiHopperCd g_hopperAlbericiHopperCd;
CcTalkHopperAzkoyenDiscriminator g_hopperAzkoyenDiscriminator;
CcTalkHopperSuzoEvolution g_hopperSuzoEvolution;
CcTalkBillValidatorIPRO g_billValidatorIpro;
CcTalkBillValidatorMD100 g_billValidatorMd100;
CcTalkBillValidatorSmartPayout g_billValidatorSmartPayout;

// Il router riceve ogni transazione sniffata e la inoltra al parser/device
// corretto in base al tipo di periferica coinvolta.
CcTalkRouter g_router;

// Sniffer del bus fisico ccTalk.
CcTalkBusSniffer::Config cfg;
CcTalkBusSniffer g_sniffer(cfg);

// Helper di test/composizione: costruisce un frame ccTalk senza dover
// ripetere l'inizializzazione campo per campo.
static CcTalkFrame makeFrame(uint8_t dest, uint8_t src, uint8_t hdr,
                             const uint8_t* data, uint8_t dataLen) {
  CcTalkFrame f;
  f.dest = dest;
  f.src = src;
  f.hdr = hdr;
  f.len = dataLen;
  f.data = data;
  f.dataLen = dataLen;
  return f;
}

// ============================================================================
// Forward declarations
// ============================================================================
// Il file e volutamente organizzato per blocchi tematici; queste dichiarazioni
// anticipano le funzioni richiamate prima della loro definizione.

static void logRuntimeLine(const char* line, bool decoded = false);
static bool onWebResetCounters(String& message, void* userData);
static bool onWebSetCoinBase(int64_t coinLevelBaseCents, String& message, void* userData);
static bool onWebSetBillRecyclerBase(int64_t cassette10Count,
                                     int64_t cassette20Count,
                                     int64_t cassette50Count,
                                     String& message,
                                     void* userData);
static bool onWebSaveRemoteSnapshot(String& message, void* userData);
static bool onWebGetSettings(ccms::AppSettings& out, String& message, void* userData);
static bool onWebGetPresentPeripheralCatalog(bool& coinAcceptorPresent,
                                             uint8_t& hopperMask,
                                             uint16_t& billValidatorMask,
                                             String& unknownDevicesCsv,
                                             String& detectedDevicesJson,
                                             void* userData);
static bool onWebSaveSettings(const ccms::AppSettings& in, String& message, void* userData);
static bool onWebTestConnection(const ccms::AppSettings& in, String& message, void* userData);
static bool onWebWifiTest(const char* ssid, const char* pass, String& message, void* userData);
static bool onRemoteMasterRequest(const char* command,
                                  const char* requestPayload,
                                  String& responseMessage,
                                  void* userData);
static int64_t currentCoinLevelCentsFromTotals(const EconomicTotals& totals);
static bool parseRemoteCoinLevelBasePayload(const char* payload,
                                            int64_t& coinLevelBaseCents,
                                            bool& useCurrentCoinLevel,
                                            String& message);
static bool applyCoinBaseResetAction(uint32_t coinLevelBaseCents,
                                     const char* successLogLine,
                                     const char* successMessage,
                                     String& message);
static bool applyBillRecyclerManualAction(uint16_t cassette10Count,
                                          uint16_t cassette20Count,
                                          uint16_t cassette50Count,
                                          const char* successLogLine,
                                          const char* successMessage,
                                          String& message);
static bool applyCurrentCoinLevelAsBase(const char* successLogLine,
                                        const char* successMessage,
                                        String& message);
static void clearRemoteSettingsFromSerial();
static void factoryResetSettingsFromSerial();
static void detectBootMode();
static void pollProgrammingModeButton();
static void applyRuntimeNetworkSettings();
static void requestBootModeRestart(BootMode targetMode, const char* reason);
static void enterProgrammingMode();
static void enterProgrammingModeNoAp();
static void enterRunMode();
static void loadAppSettings();
static void selectDeviceModelsFromSettings();
static void normalizeDeviceModelSettings(ccms::AppSettings& settings);
static void normalizeCounterRoutingSettings(ccms::AppSettings& settings);
static void applyConfiguredHopperCoinValues(const ccms::AppSettings& settings);
static const char* hopperModelLabel(uint8_t model);
static const char* billValidatorModelLabel(uint8_t model);
static CcTalkHopper* hopperParserForAddress(uint8_t addr);
static const CcTalkHopper::HopperState* hopperStateForAddress(uint8_t addr);
static CcTalkBillValidator* billValidatorParserForAddress(uint8_t addr);
static const CcTalkBillValidator::BillValidatorState* billValidatorStateForAddress(uint8_t addr);
static uint8_t configuredHopperModelForAddress(const ccms::AppSettings& settings, uint8_t addr);
static uint8_t configuredBillValidatorModelForAddress(const ccms::AppSettings& settings, uint8_t addr);
static uint16_t configuredBillValidatorMask();
static void initTelemetryAndMeshServices();
static void initNetworkServices();
static void initFramPersistence();
static void initCcTalkSniffer();
static void initLevelShifterEnable();
static void initStatusLeds();
static void printStartupBanner();
static void initializeApplication();
static void refreshEconomicStatusForRemoteSave();
static void markCcTalkActivity();
static void serviceDeferredMeshStartup();
static void serviceSnifferOnce();
static void serviceAuxOnce();
static void serviceAuxIfDue();
static void updateStatusLeds();
static bool saveFramNow(String& message);
static void flushFramBeforeRestart();
#if defined(ARDUINO_ARCH_ESP32)
static void runSnifferTask(void* userData);
static void runAuxTask(void* userData);
static void startRuntimeTasks();
#endif

static const uint32_t kWebTestWifiTimeoutMs = 15000;
static const uint16_t kWebTestHttpTimeoutMs = 8000;

bool startsWithText(const char* value, const char* prefix) {
  if (!value || !prefix) return false;
  return strncmp(value, prefix, strlen(prefix)) == 0;
}

String extractJsonMessageField(const String& body) {
  const int keyPos = body.indexOf("\"message\"");
  if (keyPos < 0) return String("");

  const int colonPos = body.indexOf(':', keyPos);
  if (colonPos < 0) return String("");

  int quotePos = body.indexOf('"', colonPos + 1);
  if (quotePos < 0) return String("");

  String message;
  for (int i = quotePos + 1; i < body.length(); i++) {
    const char c = body[i];
    if (c == '\\' && i + 1 < body.length()) {
      const char escaped = body[++i];
      switch (escaped) {
        case 'n': message += '\n'; break;
        case 'r': message += '\r'; break;
        case 't': message += '\t'; break;
        case '\\': message += '\\'; break;
        case '"': message += '"'; break;
        default: message += escaped; break;
      }
      continue;
    }

    if (c == '"') {
      return message;
    }
    message += c;
  }

  return String("");
}

bool buildServerTestUrl(const char* serverUrl, String& out, String& message) {
  out = "";
  if (!serverUrl || serverUrl[0] == '\0') {
    message = "endpoint remoto non configurato";
    return false;
  }
  if (!startsWithText(serverUrl, "http://") && !startsWithText(serverUrl, "https://")) {
    message = "endpoint remoto non valido";
    return false;
  }

  String base = serverUrl;
  base.trim();
  if (base.endsWith("/test_connection.php")) {
    out = base;
    return true;
  }

  const int queryPos = base.indexOf('?');
  if (queryPos >= 0) {
    base = base.substring(0, queryPos);
  }

  if (base.endsWith(".php")) {
    const int slashPos = base.lastIndexOf('/');
    if (slashPos >= 0) {
      out = base.substring(0, slashPos + 1);
      out += "test_connection.php";
      return true;
    }
  }

  if (!base.endsWith("/")) base += "/";
  out = base + "test_connection.php";
  return true;
}

bool ensureWifiConnectedForWebTest(const ccms::AppSettings& settings, String& message) {
  if (g_wifi.isConnected()) return true;
  if (settings.wifiSsid[0] == '\0') {
    message = "WiFi non connesso e SSID non configurato";
    return false;
  }

  g_wifi.setCredentials(settings.wifiSsid, settings.wifiPass);
  g_wifi.reconnect();

  const uint32_t startMs = millis();
  while ((uint32_t)(millis() - startMs) < kWebTestWifiTimeoutMs) {
    g_wifi.loop();
    if (g_wifi.isConnected()) {
      return true;
    }
    delay(100);
  }

  message = "timeout connessione WiFi verso ";
  message += settings.wifiSsid;
  return false;
}

bool testDatabaseViaRemoteEndpoint(const ccms::AppSettings& settings, String& message) {
  String testUrl;
  const char* remoteBase =
      (settings.remoteEventUrl[0] != '\0') ? settings.remoteEventUrl : settings.serverUrl;
  if (!buildServerTestUrl(remoteBase, testUrl, message)) return false;

  const bool https = testUrl.startsWith("https://");
  int httpCode = -1;
  String responseBody;

  if (https) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, testUrl)) {
      message = "impossibile aprire connessione HTTPS verso endpoint test";
      return false;
    }
    http.setTimeout(kWebTestHttpTimeoutMs);
    if (settings.apiKey[0] != '\0') http.addHeader("X-API-Key", settings.apiKey);
    httpCode = http.GET();
    responseBody = http.getString();
    http.end();
  } else {
    HTTPClient http;
    WiFiClient client;
    if (!http.begin(client, testUrl)) {
      message = "impossibile aprire connessione HTTP verso endpoint test";
      return false;
    }
    http.setTimeout(kWebTestHttpTimeoutMs);
    if (settings.apiKey[0] != '\0') http.addHeader("X-API-Key", settings.apiKey);
    httpCode = http.GET();
    responseBody = http.getString();
    http.end();
  }

  if (httpCode < 0) {
    message = "errore HTTP durante il test DB";
    return false;
  }

  const String remoteMessage = extractJsonMessageField(responseBody);
  if (httpCode >= 200 && httpCode < 300) {
    message = (remoteMessage.length() > 0) ? remoteMessage : String("connessione DB verificata via endpoint remoto");
    return true;
  }

  message = "HTTP ";
  message += String(httpCode);
  if (remoteMessage.length() > 0) {
    message += " - ";
    message += remoteMessage;
  }
  return false;
}

#if defined(ARDUINO_ARCH_ESP32)
inline void lockFramDirtyState() { portENTER_CRITICAL(&g_framDirtyMux); }
inline void unlockFramDirtyState() { portEXIT_CRITICAL(&g_framDirtyMux); }
#endif

void resetFramDirtyTracking() {
#if defined(ARDUINO_ARCH_ESP32)
  lockFramDirtyState();
#endif
  g_framDirty = false;
  g_framDirtyGeneration = 0;
#if defined(ARDUINO_ARCH_ESP32)
  unlockFramDirtyState();
#endif
}

void markFramStateChanged() {
#if defined(ARDUINO_ARCH_ESP32)
  lockFramDirtyState();
#endif
  g_framDirty = true;
  g_framDirtyGeneration++;
#if defined(ARDUINO_ARCH_ESP32)
  unlockFramDirtyState();
#endif
}

void ensureFramDirtyPending() {
#if defined(ARDUINO_ARCH_ESP32)
  lockFramDirtyState();
#endif
  g_framDirty = true;
#if defined(ARDUINO_ARCH_ESP32)
  unlockFramDirtyState();
#endif
}

void readFramDirtyState(bool& dirty, uint32_t& generation) {
#if defined(ARDUINO_ARCH_ESP32)
  lockFramDirtyState();
#endif
  dirty = g_framDirty;
  generation = g_framDirtyGeneration;
#if defined(ARDUINO_ARCH_ESP32)
  unlockFramDirtyState();
#endif
}

bool clearFramDirtyIfGenerationUnchanged(uint32_t expectedGeneration) {
  bool cleared = false;
#if defined(ARDUINO_ARCH_ESP32)
  lockFramDirtyState();
#endif
  if (g_framDirty && g_framDirtyGeneration == expectedGeneration) {
    g_framDirty = false;
    cleared = true;
  }
#if defined(ARDUINO_ARCH_ESP32)
  unlockFramDirtyState();
#endif
  return cleared;
}

static void onWifiLog(const char* line, void* user) {
  (void)user;
  // Evita doppia stampa su seriale: WifiService stampa gia su Serial.
  // Qui replichiamo solo l'integrazione con il log di sistema e la dashboard.
  g_systemStatus.logLine(line, false);
  g_systemStatus.noteDecodedEvent(line);
  g_remoteRegistro.noteEvent(line, false);
}

static void onServiceLog(const char* line, void* user) {
  (void)user;
  // Cloud e mesh passano da qui per avere un punto unico di registrazione.
  logRuntimeLine(line, true);
}

#if defined(ARDUINO_ARCH_ESP32)
static const char* const kBootNs  = "ccms_boot";
static const char* const kBootKey = "ovr";

static void saveBootOverride(int8_t mode) {
  Preferences prefs;
  if (prefs.begin(kBootNs, false)) {
    prefs.putChar(kBootKey, mode);
    prefs.end();
  }
}

static int8_t loadAndClearBootOverride() {
  Preferences prefs;
  int8_t val = -1;
  if (prefs.begin(kBootNs, false)) {
    val = prefs.getChar(kBootKey, -1);
    if (val != -1) prefs.remove(kBootKey);
    prefs.end();
  }
  return val;
}
#endif

static void detectBootMode() {
  // Il pin viene letto subito all'avvio, dopo un breve debounce, per decidere
  // se il firmware debba partire in esecuzione normale o in configurazione.
  // Un eventuale override impostato prima di un reboot software ha priorita
  // sul livello del pin per consentire il cambio modo anche a runtime.
  if (!appconfig::GPIO_EXPANDER_ENABLED && appconfig::PROG_MODE_BUTTON_PIN >= 0) {
    pinMode(appconfig::PROG_MODE_BUTTON_PIN, INPUT_PULLUP);
  }
  delay(appconfig::PROG_MODE_DEBOUNCE_MS);
  const bool buttonPressed = readProgButtonRaw();

#if defined(ARDUINO_ARCH_ESP32)
  g_bootModeOverride = loadAndClearBootOverride();
#endif

  if (g_bootModeOverride == (int8_t)BOOT_MODE_PROG) {
    g_bootMode = BOOT_MODE_PROG;
  } else if (g_bootModeOverride == (int8_t)BOOT_MODE_PROG_NO_AP) {
    g_bootMode = BOOT_MODE_PROG_NO_AP;
  } else if (g_bootModeOverride == (int8_t)BOOT_MODE_RUN) {
    g_bootMode = BOOT_MODE_RUN;
  } else {
    g_bootMode = buttonPressed ? BOOT_MODE_PROG : BOOT_MODE_RUN;
  }

  g_bootModeOverride = -1;
  g_progButtonPrevPressed = buttonPressed;
}

static void applyRuntimeNetworkSettings() {
  g_wifi.setCredentials(g_appSettings.wifiSsid, g_appSettings.wifiPass);
  g_cloud.setEndpointUrl(g_appSettings.serverUrl);
  g_remoteRegistro.setEndpointUrl(g_appSettings.remoteEventUrl);
  g_remoteRegistro.setLocationCode(g_appSettings.locationCode);
  g_remoteRegistro.setApiKey(g_appSettings.apiKey);
  if (g_appSettings.mqttEnabled && g_appSettings.mqttBrokerHost[0] != '\0') {
    g_remoteRegistro.setMqttEnabled(true);
    g_remoteRegistro.setMqttConfig(g_appSettings.mqttBrokerHost,
                                    g_appSettings.mqttBrokerPort,
                                    g_appSettings.mqttUsername,
                                    g_appSettings.mqttPassword);
  } else {
    g_remoteRegistro.setMqttEnabled(false);
  }
}

static void requestBootModeRestart(BootMode targetMode, const char* reason) {
  if (g_bootMode == targetMode) return;

  g_bootModeOverride = (int8_t)targetMode;
#if defined(ARDUINO_ARCH_ESP32)
  saveBootOverride(g_bootModeOverride);
#endif
  if (reason && reason[0] != '\0') {
    logRuntimeLine(reason, true);
  }

  flushFramBeforeRestart();
  delay(200);

#if defined(ARDUINO_ARCH_ESP32)
  ESP.restart();
#else
  g_bootMode = targetMode;
  if (targetMode == BOOT_MODE_PROG) {
    g_web.setUiMode(ccms::WebServerService::UI_MODE_PROG);
    g_wifi.beginApOnly();
    g_cloud.setEnabled(false);
    g_remoteRegistro.setEnabled(false);
    g_mesh.begin(false);
  } else if (targetMode == BOOT_MODE_PROG_NO_AP) {
    g_web.setUiMode(ccms::WebServerService::UI_MODE_PROG);
    g_wifi.begin();
    g_cloud.setEnabled(false);
    g_remoteRegistro.setEnabled(false);
    g_mesh.begin(false);
  } else {
    applyRuntimeNetworkSettings();
    g_web.setUiMode(ccms::WebServerService::UI_MODE_STATUS);
    g_wifi.begin();
    g_cloud.setEnabled(appconfig::CLOUD_PUBLISH_ENABLED);
    if (appconfig::CLOUD_PUBLISH_ENABLED) {
      g_cloud.begin();
    }
    g_remoteRegistro.setEnabled(true);
    g_remoteRegistro.begin();
    if (!appconfig::REMOTE_DB_AUTO_POLL_ENABLED) {
      logRuntimeLine("[REMOTE_DB] polling automatico disabilitato", true);
    }
  }

  g_wifiStableSinceMs = 0;
  g_lastMeshStartAttemptMs = 0;
  g_meshStartupDeferredLogged = false;
#endif
}

static void enterProgrammingMode() {
  requestBootModeRestart(BOOT_MODE_PROG,
                         "[MODE] riavvio richiesto dal pulsante: accesso modalita PROG");
}

static void enterProgrammingModeNoAp() {
  requestBootModeRestart(BOOT_MODE_PROG_NO_AP,
                         "[MODE] riavvio richiesto dalla console: accesso modalita PROG (no AP)");
}

static void enterRunMode() {
  requestBootModeRestart(BOOT_MODE_RUN,
                         "[MODE] riavvio richiesto dal pulsante: ritorno a modalita RUN");
}

static void pollProgrammingModeButton() {
  bool buttonPressed = readProgButtonRaw();
  if (buttonPressed != g_progButtonPrevPressed) {
    delay(appconfig::PROG_MODE_DEBOUNCE_MS);
    buttonPressed = readProgButtonRaw();
    if (buttonPressed != g_progButtonPrevPressed) {
      g_progButtonPrevPressed = buttonPressed;
      if (buttonPressed) {
        if (isProgMode()) enterRunMode();
        else enterProgrammingMode();
      }
    }
  }
}

static void initLevelShifterEnable() {
  if (appconfig::LEVEL_SHIFTER_OE_PIN < 0) return;

  const uint8_t activeLevel = appconfig::LEVEL_SHIFTER_OE_ACTIVE_HIGH ? HIGH : LOW;
  digitalWrite(appconfig::LEVEL_SHIFTER_OE_PIN, activeLevel);
  pinMode(appconfig::LEVEL_SHIFTER_OE_PIN, OUTPUT);
}

static void writeStatusLed(int pin, bool on) {
  if (pin < 0) return;
  const uint8_t activeLevel = appconfig::STATUS_LED_ACTIVE_LOW ? LOW : HIGH;
  const uint8_t idleLevel = (activeLevel == LOW) ? HIGH : LOW;
  digitalWrite(pin, on ? activeLevel : idleLevel);
}

// --- PCF8574MT GPIO expander (I2C, stessa linea di FRAM) ---

static void pcf8574WritePort() {
  Wire.beginTransmission(appconfig::GPIO_EXPANDER_I2C_ADDR);
  Wire.write(g_gpioExpanderPort);
  Wire.endTransmission();
}

static void pcf8574SetPin(uint8_t pin, bool high) {
  if (high) g_gpioExpanderPort |=  (1u << pin);
  else      g_gpioExpanderPort &= ~(1u << pin);
  pcf8574WritePort();
}

static bool pcf8574ReadPin(uint8_t pin) {
  pcf8574SetPin(pin, true); // forza il pin in input (pullup quasi-bidirezionale)
  Wire.requestFrom((uint8_t)appconfig::GPIO_EXPANDER_I2C_ADDR, (uint8_t)1);
  if (!Wire.available()) return true; // default: non premuto
  return ((Wire.read() >> pin) & 1u) != 0; // true = alto (non premuto)
}

// Accende/spegne il LED WiFi (PCF8574 source: 1 = acceso)
static void writeLedWifi(bool on) {
  if (appconfig::GPIO_EXPANDER_ENABLED) {
    pcf8574SetPin(appconfig::GPIO_EXPANDER_PIN_LED_WIFI, on);
    return;
  }
  writeStatusLed(appconfig::WIFI_STATUS_LED_PIN, on);
}

// Accende/spegne il LED CCTalk (PCF8574 source: 1 = acceso)
static void writeLedCcTalk(bool on) {
  if (appconfig::GPIO_EXPANDER_ENABLED) {
    pcf8574SetPin(appconfig::GPIO_EXPANDER_PIN_LED_CCTALK, on);
    return;
  }
  writeStatusLed(appconfig::CCTALK_STATUS_LED_PIN, on);
}

// Legge il pulsante PROG: restituisce true se premuto (pin basso)
static bool readProgButtonRaw() {
  if (appconfig::GPIO_EXPANDER_ENABLED) {
    return !pcf8574ReadPin(appconfig::GPIO_EXPANDER_PIN_BUTTON_PROG);
  }
  if (appconfig::PROG_MODE_BUTTON_PIN < 0) return false;
  return digitalRead(appconfig::PROG_MODE_BUTTON_PIN) == LOW;
}

// --- fine PCF8574MT ---

static void initStatusLeds() {
  if (appconfig::GPIO_EXPANDER_ENABLED) {
    Wire.begin(appconfig::FRAM_I2C_SDA_PIN, appconfig::FRAM_I2C_SCL_PIN);
    g_gpioExpanderPort = 0xFF;
    pcf8574WritePort(); // tutti i pin alti: LED off, P3 in pullup input
    writeLedCcTalk(false);
    writeLedWifi(false);
    return;
  }
  if (appconfig::CCTALK_STATUS_LED_PIN >= 0) {
    pinMode(appconfig::CCTALK_STATUS_LED_PIN, OUTPUT);
  }
  if (appconfig::WIFI_STATUS_LED_PIN >= 0) {
    pinMode(appconfig::WIFI_STATUS_LED_PIN, OUTPUT);
  }
  writeStatusLed(appconfig::CCTALK_STATUS_LED_PIN, false);
  writeStatusLed(appconfig::WIFI_STATUS_LED_PIN, false);
}

static void markCcTalkActivity() {
  g_lastCcTalkActivityMs = millis();
}

static void updateStatusLeds() {
  const uint32_t now = millis();
  const uint32_t lastCcTalkActivityMs = g_lastCcTalkActivityMs;
  const bool ccTalkPresent =
      (lastCcTalkActivityMs != 0) &&
      ((uint32_t)(now - lastCcTalkActivityMs) <= appconfig::CCTALK_STATUS_LED_HOLD_MS);
  const bool wifiConnected = g_wifi.isConnected();
  const bool wifiApActive = g_wifi.isApFallbackActive();

  writeLedCcTalk(ccTalkPresent);

  if (wifiApActive) {
    const bool blinkOn = ((now / 300U) % 2U) == 0U;
    writeLedWifi(blinkOn);
  } else {
    writeLedWifi(wifiConnected);
  }
}

static const char* hopperModelLabel(uint8_t model) {
  switch (model) {
    case ccms::HOPPER_MODEL_ALBERICI_DISCRIMINATOR:
      return "AlbericiDiscriminator";
    case ccms::HOPPER_MODEL_ALBERICI_HOPPERCD:
      return "AlbericiHopperCD";
    case ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR:
      return "AzkoyenDiscriminator";
    case ccms::HOPPER_MODEL_SUZO_EVOLUTION:
      return "SuzoEvolution";
    default:
      return "UNKNOWN";
  }
}

static const char* billValidatorModelLabel(uint8_t model) {
  switch (model) {
    case ccms::BILL_VALIDATOR_MODEL_MD100:
      return "MD100";
    case ccms::BILL_VALIDATOR_MODEL_IPRO:
      return "IPRO";
    case ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT:
      return "SMART_Payout";
    default:
      return "UNKNOWN";
  }
}

static uint8_t configuredHopperModelForAddress(const ccms::AppSettings& settings, uint8_t addr) {
  if (!ccms::isValidHopperAddress(addr)) return 0;
  const uint8_t bit = ccms::hopperAddressBit(addr);
  if ((settings.hopperAlbericiDiscriminatorMask & bit) != 0) {
    return ccms::HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
  }
  if ((settings.hopperAlbericiHopperCdMask & bit) != 0) {
    return ccms::HOPPER_MODEL_ALBERICI_HOPPERCD;
  }
  if ((settings.hopperAzkoyenDiscriminatorMask & bit) != 0) {
    return ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR;
  }
  if ((settings.hopperSuzoEvolutionMask & bit) != 0) {
    return ccms::HOPPER_MODEL_SUZO_EVOLUTION;
  }
  return 0;
}

static uint8_t configuredBillValidatorModelForAddress(const ccms::AppSettings& settings, uint8_t addr) {
  if (!ccms::isValidBillValidatorAddress(addr)) return 0;
  const uint16_t bit = ccms::billValidatorAddressBit(addr);
  if ((settings.billValidatorMd100Mask & bit) != 0) {
    return ccms::BILL_VALIDATOR_MODEL_MD100;
  }
  if ((settings.billValidatorSmartPayoutMask & bit) != 0) {
    return ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT;
  }
  if ((settings.billValidatorIproMask & bit) != 0) {
    return ccms::BILL_VALIDATOR_MODEL_IPRO;
  }
  return 0;
}

static CcTalkHopper* hopperParserForAddress(uint8_t addr) {
  switch (configuredHopperModelForAddress(g_runtimeDeviceSettings, addr)) {
    case ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR:
      return &g_hopperAzkoyenDiscriminator;
    case ccms::HOPPER_MODEL_SUZO_EVOLUTION:
      return &g_hopperSuzoEvolution;
    case ccms::HOPPER_MODEL_ALBERICI_HOPPERCD:
      return &g_hopperAlbericiHopperCd;
    case ccms::HOPPER_MODEL_ALBERICI_DISCRIMINATOR:
      return &g_hopperAlbericiDiscriminator;
    default:
      return nullptr;
  }
}

static const CcTalkHopper::HopperState* hopperStateForAddress(uint8_t addr) {
  CcTalkHopper* parser = hopperParserForAddress(addr);
  return parser ? parser->stateFor(addr) : nullptr;
}

static CcTalkBillValidator* billValidatorParserForAddress(uint8_t addr) {
  switch (configuredBillValidatorModelForAddress(g_runtimeDeviceSettings, addr)) {
    case ccms::BILL_VALIDATOR_MODEL_IPRO:
      return &g_billValidatorIpro;
    case ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT:
      return &g_billValidatorSmartPayout;
    case ccms::BILL_VALIDATOR_MODEL_MD100:
      return &g_billValidatorMd100;
    default:
      return nullptr;
  }
}

static const CcTalkBillValidator::BillValidatorState* billValidatorStateForAddress(uint8_t addr) {
  CcTalkBillValidator* parser = billValidatorParserForAddress(addr);
  return parser ? parser->stateFor(addr) : nullptr;
}

static void normalizeDeviceModelSettings(ccms::AppSettings& settings) {
  settings.coinAcceptorFalconProfile =
      ccms::sanitizeCoinAcceptorFalconProfile(settings.coinAcceptorFalconProfile);
  settings.hopperAlbericiDiscriminatorMask =
      ccms::sanitizeHopperModelAssignmentMask(settings.hopperAlbericiDiscriminatorMask);
  settings.hopperAlbericiHopperCdMask =
      ccms::sanitizeHopperModelAssignmentMask(settings.hopperAlbericiHopperCdMask);
  settings.hopperAzkoyenDiscriminatorMask =
      ccms::sanitizeHopperModelAssignmentMask(settings.hopperAzkoyenDiscriminatorMask);
  settings.hopperSuzoEvolutionMask =
      ccms::sanitizeHopperModelAssignmentMask(settings.hopperSuzoEvolutionMask);
  settings.billValidatorMd100Mask =
      ccms::sanitizeBillValidatorModelAssignmentMask(settings.billValidatorMd100Mask);
  settings.billValidatorSmartPayoutMask =
      ccms::sanitizeBillValidatorModelAssignmentMask(settings.billValidatorSmartPayoutMask);
  settings.billValidatorIproMask =
      ccms::sanitizeBillValidatorModelAssignmentMask(settings.billValidatorIproMask);

  uint8_t assignedHopperMask = 0;
  settings.hopperAlbericiDiscriminatorMask =
      (uint8_t)(settings.hopperAlbericiDiscriminatorMask & (uint8_t)~assignedHopperMask);
  assignedHopperMask |= settings.hopperAlbericiDiscriminatorMask;
  settings.hopperAlbericiHopperCdMask =
      (uint8_t)(settings.hopperAlbericiHopperCdMask & (uint8_t)~assignedHopperMask);
  assignedHopperMask |= settings.hopperAlbericiHopperCdMask;
  settings.hopperAzkoyenDiscriminatorMask =
      (uint8_t)(settings.hopperAzkoyenDiscriminatorMask & (uint8_t)~assignedHopperMask);
  assignedHopperMask |= settings.hopperAzkoyenDiscriminatorMask;
  settings.hopperSuzoEvolutionMask =
      (uint8_t)(settings.hopperSuzoEvolutionMask & (uint8_t)~assignedHopperMask);
  assignedHopperMask |= settings.hopperSuzoEvolutionMask;

  const uint16_t billValidatorOverlap =
      (uint16_t)(settings.billValidatorMd100Mask & settings.billValidatorSmartPayoutMask);
  if (billValidatorOverlap != 0) {
    settings.billValidatorSmartPayoutMask =
        (uint16_t)(settings.billValidatorSmartPayoutMask & (uint16_t)~billValidatorOverlap);
  }
  const uint16_t billValidatorIproVsMd100Overlap =
      (uint16_t)(settings.billValidatorIproMask & settings.billValidatorMd100Mask);
  if (billValidatorIproVsMd100Overlap != 0) {
    settings.billValidatorIproMask =
        (uint16_t)(settings.billValidatorIproMask & (uint16_t)~billValidatorIproVsMd100Overlap);
  }
  const uint16_t billValidatorIproVsSmartOverlap =
      (uint16_t)(settings.billValidatorIproMask & settings.billValidatorSmartPayoutMask);
  if (billValidatorIproVsSmartOverlap != 0) {
    settings.billValidatorIproMask =
        (uint16_t)(settings.billValidatorIproMask & (uint16_t)~billValidatorIproVsSmartOverlap);
  }

  if (settings.hopperAzkoyenDiscriminatorMask != 0 &&
      settings.hopperAlbericiDiscriminatorMask == 0 &&
      settings.hopperAlbericiHopperCdMask == 0 &&
      settings.hopperSuzoEvolutionMask == 0) {
    settings.hopperModel = ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR;
  } else if (settings.hopperSuzoEvolutionMask != 0 &&
      settings.hopperAlbericiDiscriminatorMask == 0 &&
      settings.hopperAlbericiHopperCdMask == 0 &&
      settings.hopperAzkoyenDiscriminatorMask == 0) {
    settings.hopperModel = ccms::HOPPER_MODEL_SUZO_EVOLUTION;
  } else if (settings.hopperAlbericiHopperCdMask != 0 &&
             settings.hopperAlbericiDiscriminatorMask == 0 &&
             settings.hopperAzkoyenDiscriminatorMask == 0) {
    settings.hopperModel = ccms::HOPPER_MODEL_ALBERICI_HOPPERCD;
  } else {
    settings.hopperModel = ccms::HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
  }
  if (settings.billValidatorIproMask != 0 &&
      settings.billValidatorMd100Mask == 0 &&
      settings.billValidatorSmartPayoutMask == 0) {
    settings.billValidatorModel = ccms::BILL_VALIDATOR_MODEL_IPRO;
  } else if (settings.billValidatorSmartPayoutMask != 0 &&
             settings.billValidatorMd100Mask == 0) {
    settings.billValidatorModel = ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT;
  } else {
    settings.billValidatorModel = ccms::BILL_VALIDATOR_MODEL_MD100;
  }
}

static void selectDeviceModelsFromSettings() {
  // Le assegnazioni modello-per-indirizzo vengono sempre sanitizzate e rese
  // mutuamente esclusive prima di toccare i decoder runtime.
  normalizeDeviceModelSettings(g_appSettings);
  normalizeCounterRoutingSettings(g_appSettings);
  g_runtimeDeviceSettings = g_appSettings;
  applyConfiguredHopperCoinValues(g_runtimeDeviceSettings);
  g_hopperAlbericiDiscriminator.setAddressMask(g_runtimeDeviceSettings.hopperAlbericiDiscriminatorMask);
  g_hopperAlbericiHopperCd.setAddressMask(g_runtimeDeviceSettings.hopperAlbericiHopperCdMask);
  g_hopperAzkoyenDiscriminator.setAddressMask(g_runtimeDeviceSettings.hopperAzkoyenDiscriminatorMask);
  g_hopperSuzoEvolution.setAddressMask(g_runtimeDeviceSettings.hopperSuzoEvolutionMask);
  g_billValidatorIpro.setAddressMask(g_runtimeDeviceSettings.billValidatorIproMask);
  g_billValidatorMd100.setAddressMask(g_runtimeDeviceSettings.billValidatorMd100Mask);
  g_billValidatorSmartPayout.setAddressMask(g_runtimeDeviceSettings.billValidatorSmartPayoutMask);
  g_coin.setValueProfile(g_runtimeDeviceSettings.coinAcceptorFalconProfile);
}

static void applyConfiguredHopperCoinValues(const ccms::AppSettings& settings) {
  for (uint8_t addr = ccms::kHopperAddressMin; addr <= ccms::kHopperAddressMax; addr++) {
    const uint8_t idx = ccms::hopperAddressIndex(addr);
    const uint16_t valueCents =
        (idx < ccms::kHopperAddressCount) ? settings.hopperCoinValueCents[idx] : 0;
    g_hopperAlbericiDiscriminator.setConfiguredCoinValueCents(addr, valueCents);
    g_hopperAlbericiHopperCd.setConfiguredCoinValueCents(addr, valueCents);
    g_hopperAzkoyenDiscriminator.setConfiguredCoinValueCents(addr, valueCents);
    g_hopperSuzoEvolution.setConfiguredCoinValueCents(addr, valueCents);
  }
}

static bool hopperContributes(uint8_t mask, uint8_t addr) {
  return ccms::isValidHopperAddress(addr) && (mask & ccms::hopperAddressBit(addr)) != 0;
}

static bool billValidatorContributes(uint16_t mask, uint8_t addr) {
  return ccms::isValidBillValidatorAddress(addr) && (mask & ccms::billValidatorAddressBit(addr)) != 0;
}

static void normalizeCounterRoutingSettings(ccms::AppSettings& settings) {
  settings.coinInHopperMask = ccms::sanitizeHopperContributionMask(settings.coinInHopperMask);
  settings.coinOutHopperMask = ccms::sanitizeHopperContributionMask(settings.coinOutHopperMask);
  settings.billInValidatorMask =
      ccms::sanitizeBillValidatorContributionMask(settings.billInValidatorMask);
  settings.billOutValidatorMask =
      ccms::sanitizeBillValidatorContributionMask(settings.billOutValidatorMask);

  const uint8_t configuredHopperMask =
      (uint8_t)(settings.hopperAlbericiDiscriminatorMask |
                settings.hopperAlbericiHopperCdMask |
                settings.hopperAzkoyenDiscriminatorMask |
                settings.hopperSuzoEvolutionMask);
  const uint16_t configuredBillValidatorMask =
      (uint16_t)(settings.billValidatorMd100Mask |
                 settings.billValidatorSmartPayoutMask |
                 settings.billValidatorIproMask);
  settings.coinInHopperMask = (uint8_t)(settings.coinInHopperMask & configuredHopperMask);
  settings.coinOutHopperMask = (uint8_t)(settings.coinOutHopperMask & configuredHopperMask);
  settings.billInValidatorMask = (uint16_t)(settings.billInValidatorMask & configuredBillValidatorMask);
  settings.billOutValidatorMask = (uint16_t)(settings.billOutValidatorMask & configuredBillValidatorMask);

  // Uno stesso hopper espone un solo totalizzatore monetario (erogato): se
  // assegnato sia a IN sia a OUT produrrebbe doppio conteggio.
  const uint8_t overlap = (uint8_t)(settings.coinInHopperMask & settings.coinOutHopperMask);
  if (overlap != 0) {
    settings.coinInHopperMask = (uint8_t)(settings.coinInHopperMask & (uint8_t)~overlap);
  }
}

static uint16_t configuredBillValidatorMask() {
  return ccms::sanitizeBillValidatorContributionMask(
      (uint16_t)(g_runtimeDeviceSettings.billValidatorMd100Mask |
                 g_runtimeDeviceSettings.billValidatorSmartPayoutMask |
                 g_runtimeDeviceSettings.billValidatorIproMask));
}

static uint32_t coinAcceptorAcceptedCents() {
  const CcTalkCoinAcceptor::CoinAcceptorState* s = g_coin.stateFor(2);
  if (!s || !s->present) return 0;
  return s->acceptedTotalCents;
}

static void loadAppSettings() {
  // Si parte sempre da una configurazione "pulita".
  // Se il caricamento da NVS fallisce, il firmware continua usando i default.
  g_appSettings.clear();
  g_settingsLoaded = false;

  if (!g_settingsStore.begin()) {
    logRuntimeLine("[CFG] NVS non disponibile", true);
    selectDeviceModelsFromSettings();
    return;
  }

  g_settingsLoaded = g_settingsStore.load(g_appSettings);
  if (!g_settingsLoaded) {
    logRuntimeLine("[CFG] nessuna configurazione utente salvata", true);
    selectDeviceModelsFromSettings();
    return;
  }

  applyRuntimeNetworkSettings();
  selectDeviceModelsFromSettings();

  logRuntimeLine("[CFG] assegnazioni modello-per-indirizzo applicate", true);
  logRuntimeLine("[CFG] configurazione utente caricata", true);
}

static bool onWebGetSettings(ccms::AppSettings& out, String& message, void* userData) {
  (void)userData;
  out = g_appSettings;
  message = g_settingsLoaded ? "ok" : "valori di default";
  return true;
}

static bool onWebGetPresentPeripheralCatalog(bool& coinAcceptorPresent,
                                             uint8_t& hopperMask,
                                             uint16_t& billValidatorMask,
                                             String& unknownDevicesCsv,
                                             String& detectedDevicesJson,
                                             void* userData) {
  (void)userData;
  const CcTalkCoinAcceptor::CoinAcceptorState* coinState = g_coin.stateFor(2);
  coinAcceptorPresent = coinState && coinState->present;
  hopperMask = 0;
  billValidatorMask = 0;
  unknownDevicesCsv = "";
  detectedDevicesJson = "[";
  bool firstDetected = true;

  for (uint8_t addr = ccms::kHopperAddressMin; addr <= ccms::kHopperAddressMax; addr++) {
    const CcTalkHopper::HopperState* s = hopperStateForAddress(addr);
    if (!s || !s->present) continue;
    hopperMask |= ccms::hopperAddressBit(addr);
  }

  for (uint8_t addr = ccms::kBillValidatorAddressMin; addr <= ccms::kBillValidatorAddressMax; addr++) {
    const CcTalkBillValidator::BillValidatorState* s = billValidatorStateForAddress(addr);
    if (!s || !s->present) continue;
    billValidatorMask |= ccms::billValidatorAddressBit(addr);
  }

  for (uint16_t addr = 0; addr < 256; addr++) {
    if (!g_deviceSeen[addr]) continue;
    const uint8_t deviceAddr = (uint8_t)addr;
    const bool supportedCoin = (deviceAddr == 2);
    const bool supportedHopper = ccms::isValidHopperAddress(deviceAddr);
    const bool supportedBillValidator = ccms::isValidBillValidatorAddress(deviceAddr);
    const bool supported = supportedCoin || supportedHopper || supportedBillValidator;

    if (!firstDetected) detectedDevicesJson += ",";
    firstDetected = false;
    detectedDevicesJson += "{";
    detectedDevicesJson += "\"addr\":";
    detectedDevicesJson += String((unsigned)deviceAddr);
    detectedDevicesJson += ",\"supported\":";
    detectedDevicesJson += supported ? "true" : "false";
    detectedDevicesJson += ",\"family\":\"";
    if (supportedCoin) {
      detectedDevicesJson += "coin";
    } else if (supportedHopper) {
      detectedDevicesJson += "hopper";
    } else if (supportedBillValidator) {
      detectedDevicesJson += "bill_validator";
    } else {
      detectedDevicesJson += "unknown";
    }
    detectedDevicesJson += "\",\"selectedModel\":";
    if (supportedCoin) {
      detectedDevicesJson += "1";
    } else if (supportedHopper) {
      detectedDevicesJson += String((unsigned)configuredHopperModelForAddress(g_appSettings, deviceAddr));
    } else if (supportedBillValidator) {
      detectedDevicesJson += String((unsigned)configuredBillValidatorModelForAddress(g_appSettings, deviceAddr));
    } else {
      detectedDevicesJson += "0";
    }
    detectedDevicesJson += "}";

    if (supported) continue;
    if (unknownDevicesCsv.length() > 0) unknownDevicesCsv += ",";
    unknownDevicesCsv += String((unsigned)deviceAddr);
  }

  detectedDevicesJson += "]";

  return true;
}

static bool onWebSaveSettings(const ccms::AppSettings& in, String& message, void* userData) {
  (void)userData;
  // Si lavora su una copia temporanea, cosi il sistema applica la nuova
  // configurazione solo se il salvataggio su store va a buon fine.
  ccms::AppSettings next = in;
  normalizeDeviceModelSettings(next);
  normalizeCounterRoutingSettings(next);
  next.valid = true;
  const bool modelChanged =
      (next.hopperAlbericiDiscriminatorMask != g_appSettings.hopperAlbericiDiscriminatorMask) ||
      (next.hopperAlbericiHopperCdMask != g_appSettings.hopperAlbericiHopperCdMask) ||
      (next.hopperAzkoyenDiscriminatorMask != g_appSettings.hopperAzkoyenDiscriminatorMask) ||
      (next.hopperSuzoEvolutionMask != g_appSettings.hopperSuzoEvolutionMask) ||
      (next.billValidatorMd100Mask != g_appSettings.billValidatorMd100Mask) ||
      (next.billValidatorSmartPayoutMask != g_appSettings.billValidatorSmartPayoutMask) ||
      (next.billValidatorIproMask != g_appSettings.billValidatorIproMask);
  const bool counterRoutingChanged = (next.coinAcceptorInEnabled != g_appSettings.coinAcceptorInEnabled) ||
                                     (next.coinAcceptorFalconProfile != g_appSettings.coinAcceptorFalconProfile) ||
                                     (next.coinInHopperMask != g_appSettings.coinInHopperMask) ||
                                     (next.coinOutHopperMask != g_appSettings.coinOutHopperMask) ||
                                     (memcmp(next.hopperCoinValueCents,
                                             g_appSettings.hopperCoinValueCents,
                                             sizeof(next.hopperCoinValueCents)) != 0) ||
                                     (next.billInValidatorMask != g_appSettings.billInValidatorMask) ||
                                     (next.billOutValidatorMask != g_appSettings.billOutValidatorMask);

  if (!g_settingsStore.save(next)) {
    message = "errore salvataggio configurazione";
    return false;
  }

  g_appSettings = next;
  g_settingsLoaded = true;
  selectDeviceModelsFromSettings();

  if (!isProgMode()) {
    applyRuntimeNetworkSettings();
  }

  if (modelChanged) {
    message = "impostazioni salvate; nuovi modelli periferiche applicati subito";
  } else if (isProgMode()) {
    message = counterRoutingChanged
                  ? "impostazioni salvate; instradamento contatori applicato subito; riavvia il dispositivo per applicare rete e registro remoto"
                  : "impostazioni salvate; riavvia il dispositivo per applicare rete e registro remoto";
  } else if (next.wifiSsid[0] == '\0') {
    message = "impostazioni salvate; WiFi disattivato";
  } else if (!next.saveWifiCredentials) {
    message = "impostazioni salvate; WiFi applicato ora ma non memorizzato";
  } else {
    message = "impostazioni salvate; connessione WiFi automatica abilitata";
  }
  return true;
}

static bool onWebTestConnection(const ccms::AppSettings& in, String& message, void* userData) {
  (void)userData;

  if (!ensureWifiConnectedForWebTest(in, message)) {
    return false;
  }

  if (in.remoteEventUrl[0] != '\0' || in.serverUrl[0] != '\0') {
    if (testDatabaseViaRemoteEndpoint(in, message)) {
      logRuntimeLine("[WEB] test endpoint remoto riuscito", true);
      return true;
    }

    logRuntimeLine("[WEB] test endpoint remoto fallito", true);
    return false;
  }

  message = "Server Web URL non configurato";
  logRuntimeLine("[WEB] test endpoint remoto non eseguito: URL mancante", true);
  return false;
}

static bool onWebWifiTest(const char* ssid, const char* pass, String& message, void* userData) {
  (void)userData;
  if (!ssid || ssid[0] == '\0') {
    message = "SSID non specificato";
    return false;
  }
  g_wifi.setCredentials(ssid, pass ? pass : "");
  g_wifi.reconnect();

  const uint32_t startMs = millis();
  while ((uint32_t)(millis() - startMs) < kWebTestWifiTimeoutMs) {
    g_wifi.loop();
    if (g_wifi.isConnected()) {
      message = "connesso a \"";
      message += ssid;
      message += "\"  IP: ";
      message += g_wifi.ip();
      message += "  RSSI: ";
      message += String(g_wifi.rssi());
      message += " dBm";
      logRuntimeLine("[WEB] test WiFi riuscito", true);
      return true;
    }
    delay(100);
  }

  message = "timeout — rete non raggiunta: \"";
  message += ssid;
  message += "\"";
  logRuntimeLine("[WEB] test WiFi fallito (timeout)", true);
  return false;
}

static void runBillValidatorManualTests() {
  logRuntimeLine("");
  logRuntimeLine("=== BILL VALIDATOR MANUAL TEST START ===", true);
  CcTalkBillValidator* manualBv = billValidatorParserForAddress(40);
  if (!manualBv) manualBv = &g_billValidatorMd100;

  {
    static const uint8_t respData[] = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CcTalkTransaction t;
    t.hasReq = true;
    t.hasResp = true;
    t.req = makeFrame(40, 1, 0x9F, nullptr, 0);
    t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
    logRuntimeLine("[TEST] 0x9F A=2 B=0 (credit stacker/cashbox)", true);
    manualBv->onTransaction(t, g_runtimeOut, false);
    g_runtimeOut.flushPendingLine();
  }

  {
    static const uint8_t respData[] = {2, 3, 18, 0, 0, 0, 0, 0, 0, 0, 0};
    CcTalkTransaction t;
    t.hasReq = true;
    t.hasResp = true;
    t.req = makeFrame(40, 1, 0x9F, nullptr, 0);
    t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
    logRuntimeLine("[TEST] 0x9F A=3 B=18 (stored recycler 10EUR)", true);
    manualBv->onTransaction(t, g_runtimeOut, false);
    g_runtimeOut.flushPendingLine();
  }

  if (configuredBillValidatorModelForAddress(g_appSettings, 40) == ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT) {
    {
      static const uint8_t respData[] = {0x15, 0xF4, 0x01, 0x00, 0x00};
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = true;
      t.req = makeFrame(40, 1, 0x1D, nullptr, 0);
      t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
      logRuntimeLine("[TEST] 0x1D note credit 5EUR", true);
      manualBv->onTransaction(t, g_runtimeOut, false);
      g_runtimeOut.flushPendingLine();
    }

    {
      static const uint8_t respData[] = {0x19};
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = true;
      t.req = makeFrame(40, 1, 0x1D, nullptr, 0);
      t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
      logRuntimeLine("[TEST] 0x1D stacked -> cashbox route confirm", true);
      manualBv->onTransaction(t, g_runtimeOut, false);
      g_runtimeOut.flushPendingLine();
    }

    {
      static const uint8_t reqData[] = {0xD0, 0x07, 0x00, 0x00};
      static const uint8_t respData[] = {0x01};
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = true;
      t.req = makeFrame(40, 1, 0x1A, reqData, sizeof(reqData));
      t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
      logRuntimeLine("[TEST] 0x1A note amount 20EUR=1", true);
      manualBv->onTransaction(t, g_runtimeOut, false);
      g_runtimeOut.flushPendingLine();
    }

    {
      static const uint8_t reqData[] = {0x88, 0x13, 0x00, 0x00};
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = true;
      t.req = makeFrame(40, 1, 0x16, reqData, sizeof(reqData));
      t.resp = makeFrame(1, 40, 0x00, nullptr, 0);
      logRuntimeLine("[TEST] 0x16 payout amount 50EUR", true);
      manualBv->onTransaction(t, g_runtimeOut, false);
      g_runtimeOut.flushPendingLine();
    }
  } else {
    static uint8_t respData[41] = {0};
    respData[35] = 0x02; respData[36] = 0x00;
    respData[37] = 0x01; respData[38] = 0x00;
    respData[39] = 0x01; respData[40] = 0x00;
    CcTalkTransaction t;
    t.hasReq = true;
    t.hasResp = true;
    t.req = makeFrame(40, 1, 0x5E, nullptr, 0);
    t.resp = makeFrame(1, 40, 0x00, respData, sizeof(respData));
    logRuntimeLine("[TEST] 0x5E inventory c10=2 c20=1 c50=1", true);
    manualBv->onTransaction(t, g_runtimeOut, false);
    g_runtimeOut.flushPendingLine();
  }

  logRuntimeLine("=== BILL VALIDATOR MANUAL TEST END ===", true);
  logRuntimeLine("");
}

static void printConsoleHelp() {
#if ENABLE_SERIAL_LOG
  // Help minimale pensato per il monitor seriale dell'IDE/PlatformIO.
  Serial.println(F("Comandi monitor seriale:"));
  Serial.println(F("  1 -> totali cambiamonete"));
  Serial.println(F("  2 -> tutti i messaggi (con RAW REQ/RESP)"));
  Serial.println(F("  3 -> info periferiche + counter incrementato"));
  Serial.println(F("  4 -> solo anomalie (checksum errato o indirizzi non noti)"));
  Serial.println(F("  s -> stato RAM periferiche"));
  Serial.println(F("  r -> reset impostazioni registro remoto + reboot"));
  Serial.println(F("  x -> reset totale impostazioni + reboot"));
  Serial.println(F("  p -> entra in modalita PROG (WiFi STA, no AP)"));
  Serial.println(F("  h/? -> help"));
#endif
}

static void setViewMode(ViewMode mode) {
  // La view mode modifica solo il livello di output.
  // Lo sniffing e l'aggiornamento dello stato restano attivi in tutti i casi.
  g_viewMode = mode;

  switch (mode) {
    case VIEW_ECONOMIC_COUNTERS:
      g_sniffer.setPrintMode(CcTalkBusSniffer::COMPACT);
      g_economicTotalsValid = false;
#if ENABLE_SERIAL_LOG
      Serial.println(F("View mode 1: totali cambiamonete"));
#endif
      return;
    case VIEW_ALL_RAW:
      g_sniffer.setPrintMode(CcTalkBusSniffer::FULL);
#if ENABLE_SERIAL_LOG
      Serial.println(F("View mode 2: tutti i messaggi (con RAW REQ/RESP)"));
#endif
      return;
    case VIEW_INFO_AND_COUNTER_INC:
    default:
      g_sniffer.setPrintMode(CcTalkBusSniffer::COMPACT);
#if ENABLE_SERIAL_LOG
      Serial.println(F("View mode 3: info periferiche + counter incrementato"));
#endif
      return;
    case VIEW_ANOMALIES:
      g_sniffer.setPrintMode(CcTalkBusSniffer::FULL);
#if ENABLE_SERIAL_LOG
      Serial.println(F("View mode 4: anomalie (checksum errato / indirizzi non noti)"));
#endif
      return;
  }
}

static void printRuntimeState() {
#if ENABLE_SERIAL_LOG
  // Dump diagnostico volatile: utile per capire lo stato dei parser durante
  // la sessione corrente, ma non rappresenta un dato persistente.
  Serial.println();
  Serial.println(F("=== RUNTIME STATE (RAM, reset on boot) ==="));
  g_coin.dumpState(Serial);
  g_hopperAlbericiDiscriminator.dumpState(Serial);
  g_hopperAlbericiHopperCd.dumpState(Serial);
  g_hopperAzkoyenDiscriminator.dumpState(Serial);
  g_hopperSuzoEvolution.dumpState(Serial);
  g_billValidatorIpro.dumpState(Serial);
  g_billValidatorMd100.dumpState(Serial);
  g_billValidatorSmartPayout.dumpState(Serial);
  Serial.println(F("=== END RUNTIME STATE ==="));
#endif
}

static void logRuntimeLine(const char* line, bool decoded) {
  // Punto unico di emissione log dell'applicazione:
  // - opzionalmente inoltra su seriale
  // - aggiorna lo storico consultabile via SystemStatus
  g_systemStatus.logLine(line, ENABLE_SERIAL_LOG != 0);
  if (decoded) g_systemStatus.noteDecodedEvent(line);
  g_remoteRegistro.noteEvent(line, decoded);
}

static void clearRemoteSettingsFromSerial() {
  ccms::AppSettings next = g_appSettings;
  memset(next.remoteEventUrl, 0, sizeof(next.remoteEventUrl));
  memset(next.locationCode, 0, sizeof(next.locationCode));
  memset(next.apiKey, 0, sizeof(next.apiKey));
  next.valid = true;

  if (!g_settingsStore.save(next)) {
#if ENABLE_SERIAL_LOG
    Serial.println(F("[SERIAL] errore reset impostazioni registro remoto"));
#endif
    return;
  }

#if ENABLE_SERIAL_LOG
  Serial.println(F("[SERIAL] impostazioni registro remoto azzerate; riavvio"));
#endif
  flushFramBeforeRestart();
  delay(200);
#if defined(ARDUINO_ARCH_ESP32)
  ESP.restart();
#endif
}

static void factoryResetSettingsFromSerial() {
  if (!g_settingsStore.clearAll()) {
#if ENABLE_SERIAL_LOG
    Serial.println(F("[SERIAL] errore reset totale configurazione"));
#endif
    return;
  }

#if ENABLE_SERIAL_LOG
  Serial.println(F("[SERIAL] configurazione cancellata; riavvio"));
#endif
  flushFramBeforeRestart();
  delay(200);
#if defined(ARDUINO_ARCH_ESP32)
  ESP.restart();
#endif
}

static void markDeviceSeen(uint8_t addr) {
  // Conta i dispositivi visti almeno una volta.
  // Serve alla dashboard per mostrare una misura sintetica della topologia.
  if (g_deviceSeen[addr]) return;
  g_deviceSeen[addr] = true;
  g_detectedDeviceCount++;
  g_systemStatus.setDetectedDevices(g_detectedDeviceCount);
}

static void formatFrameSummary(const char* prefix,
                               uint8_t src,
                               uint8_t dest,
                               uint8_t hdr,
                               uint8_t len,
                               char* out,
                               size_t outLen) {
  if (!out || outLen == 0) return;
  snprintf(out, outLen, "%s s=%u d=%u h=0x%02X len=%u",
           prefix ? prefix : "FRAME",
           (unsigned)src,
           (unsigned)dest,
           (unsigned)hdr,
           (unsigned)len);
}

static void updateCcTalkBusStatus(const CcTalkTransaction& t) {
  // Ogni transazione aggiorna un riepilogo sintetico TX/RX nel SystemStatus.
  // Questo consente alla web UI di mostrare attivita recente senza rileggere
  // l'intero log testuale.
  if (t.hasReq) {
    markDeviceSeen(t.req.dest);
    char frame[96] = {0};
    formatFrameSummary("TX", t.req.src, t.req.dest, t.req.hdr, t.req.len, frame, sizeof(frame));
    g_systemStatus.noteTxFrame(frame);
  }

  if (t.hasResp) {
    markDeviceSeen(t.resp.src);
    char frame[96] = {0};
    formatFrameSummary("RX", t.resp.src, t.resp.dest, t.resp.hdr, t.resp.len, frame, sizeof(frame));
    g_systemStatus.noteRxFrame(frame);
  }

  if (t.hasReq || t.hasResp) {
    g_systemStatus.noteTransaction();
  }
}

static bool isCounterIncrement(uint8_t previous, uint8_t current) {
  if (current == previous) return false;
  if (current > previous) return true;

  // Rollover plausibile di un contatore a 8 bit (es. 254 -> 0..31).
  // La soglia e empirica ma evita di perdere un evento reale al riavvolgimento.
  if (previous > 220 && current < 32) return true;
  return false;
}

static bool shouldPrintStartupInfo(uint8_t hdr) {
  switch (hdr) {
    case 0xF6: // manufacturer id
    case 0xF5: // category id
    case 0xF4: // product code
    case 0xF2: // serial number
    case 0xF1: // software revision
    case 0xC0: // build code
    case 0x04: // comms revision
    case 0xFF: // extended id
    case 0x01: // reset
    case 0x83: // hopper coin value table
    case 0xD9: // hopper hi/low status
    case 0x9D: // bill id
      return true;
    default:
      return false;
  }
}

static bool shouldPrintAzkoyenStartupInfo(uint8_t addr, uint8_t hdr) {
  if (!ccms::isValidHopperAddress(addr)) return false;
  if (configuredHopperModelForAddress(g_runtimeDeviceSettings, addr) !=
      ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) {
    return false;
  }

  switch (hdr) {
    case 0x10: // about
    case 0x29: // diameters
    case 0x32: // coin values
      return true;
    default:
      return false;
  }
}

static bool isAzkoyenDatasetCommandHeader(uint8_t hdr) {
  switch (hdr) {
    case 0x10: // about
    case 0x13: // request discriminator status
    case 0x15: // cancel current command
    case 0x19: // multiple emptying
    case 0x20: // multiple payout
    case 0x23: // request last command status
    case 0x25: // program diameters
    case 0x29: // request diameters
    case 0x31: // program coin values
    case 0x32: // request coin values
    case 0x34: // multiple emptying
    case 0x35: // intelligent payout
      return true;
    default:
      return false;
  }
}

static bool isBillValidatorDatasetSpecificHeader(uint8_t hdr) {
  switch (hdr) {
    case 0x61: // MD100 payout/transfer bill
    case 0x5E: // MD100 recycler inventory
    case 0x14: // SmartPayout routing
    case 0x15: // SmartPayout get routing
    case 0x16: // SmartPayout payout amount
    case 0x17: // SmartPayout float amount
    case 0x18: // SmartPayout empty payout store
    case 0x19: // SmartPayout get minimum payout
    case 0x1A: // SmartPayout get note amount
    case 0x1D: // SmartPayout request status
      return true;
    default:
      return false;
  }
}

static bool configuredBillValidatorModelSupportsHeader(uint8_t addr, uint8_t hdr) {
  if (!ccms::isValidBillValidatorAddress(addr)) return false;

  switch (configuredBillValidatorModelForAddress(g_runtimeDeviceSettings, addr)) {
    case ccms::BILL_VALIDATOR_MODEL_MD100:
      return (hdr == 0x61 || hdr == 0x5E);
    case ccms::BILL_VALIDATOR_MODEL_SMART_PAYOUT:
      return (hdr == 0x14 || hdr == 0x15 || hdr == 0x16 || hdr == 0x17 ||
              hdr == 0x18 || hdr == 0x19 || hdr == 0x1A || hdr == 0x1D);
    case ccms::BILL_VALIDATOR_MODEL_IPRO:
    default:
      return false;
  }
}

static uint8_t transactionAddress(const CcTalkTransaction& t) {
  if (t.hasReq) return t.req.dest;
  if (t.hasResp) return t.resp.src;
  return 0;
}

// Restituisce true se l'indirizzo appartiene a un dispositivo ccTalk noto
// (master, hopper, bill validator, coin acceptor). Usato dal filtro anomalie.
static bool isCctalkKnownDeviceAddress(uint8_t addr) {
  if (addr == CCTALK_ADDR_MASTER) return true;
  if (addr == 2) return true;                            // coin acceptor NRI Falcon
  if (ccms::isValidHopperAddress(addr)) return true;     // 3-10
  if (ccms::isValidBillValidatorAddress(addr)) return true; // 40-50
  return false;
}

static bool shouldUseRawOnlyOutput(const CcTalkTransaction& t) {
  const uint8_t addr = transactionAddress(t);
  const uint8_t hdr = t.hasReq ? t.req.hdr : 0;

  if (addr == 2) return false;

  if (ccms::isValidHopperAddress(addr)) {
    const uint8_t model = configuredHopperModelForAddress(g_runtimeDeviceSettings, addr);
    if (model == 0) return true;
    if (t.hasReq && isAzkoyenDatasetCommandHeader(hdr) &&
        model != ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) {
      return true;
    }
    return false;
  }

  if (ccms::isValidBillValidatorAddress(addr)) {
    const uint8_t model = configuredBillValidatorModelForAddress(g_runtimeDeviceSettings, addr);
    if (model == 0) return true;
    if (t.hasReq && isBillValidatorDatasetSpecificHeader(hdr) &&
        !configuredBillValidatorModelSupportsHeader(addr, hdr)) {
      return true;
    }
    return false;
  }

  return true;
}

static bool isIproCrcBillValidatorTx(const CcTalkTransaction& t) {
  if (!t.hasReq || !ccms::isValidBillValidatorAddress(t.req.dest)) return false;
  if (!t.req.crc16) return false;
  if (t.hasResp && !t.resp.crc16) return false;
  return true;
}

static void autoAssignIproRuntimeAddress(uint8_t addr) {
  if (!ccms::isValidBillValidatorAddress(addr)) return;
  const uint16_t bit = ccms::billValidatorAddressBit(addr);
  if ((g_runtimeDeviceSettings.billValidatorIproMask & bit) != 0) return;

  // L'iPRO/JCM usa i frame ccTalk CRC-16 osservati nei log. Se la UI non ha
  // ancora assegnato quel BV al modello IPRO, lo facciamo solo in RAM per
  // permettere al decoder economico di aggiornarsi.
  g_runtimeDeviceSettings.billValidatorMd100Mask =
      (uint16_t)(g_runtimeDeviceSettings.billValidatorMd100Mask & (uint16_t)~bit);
  g_runtimeDeviceSettings.billValidatorSmartPayoutMask =
      (uint16_t)(g_runtimeDeviceSettings.billValidatorSmartPayoutMask & (uint16_t)~bit);
  g_runtimeDeviceSettings.billValidatorIproMask =
      (uint16_t)(g_runtimeDeviceSettings.billValidatorIproMask | bit);
  g_runtimeDeviceSettings.billInValidatorMask =
      (uint16_t)(g_runtimeDeviceSettings.billInValidatorMask | bit);
  g_runtimeDeviceSettings.billValidatorModel = ccms::BILL_VALIDATOR_MODEL_IPRO;

  g_billValidatorMd100.setAddressMask(g_runtimeDeviceSettings.billValidatorMd100Mask);
  g_billValidatorSmartPayout.setAddressMask(g_runtimeDeviceSettings.billValidatorSmartPayoutMask);
  g_billValidatorIpro.setAddressMask(g_runtimeDeviceSettings.billValidatorIproMask);
}

static bool shouldPrintDatasetSpecificByAddress(uint8_t addr, uint8_t hdr) {
  if (ccms::isValidHopperAddress(addr)) return isAzkoyenDatasetCommandHeader(hdr);
  if (ccms::isValidBillValidatorAddress(addr)) {
    return isBillValidatorDatasetSpecificHeader(hdr) && hdr != 0x1D;
  }
  return false;
}

static int8_t startupInfoSlot(uint8_t addr, uint8_t hdr) {
  switch (hdr) {
    case 0xF6: return 0;
    case 0xF5: return 1;
    case 0xF4: return 2;
    case 0xF2: return 3;
    case 0xF1: return 4;
    case 0xC0: return 5;
    case 0x04: return 6;
    case 0xFF: return 7;
    case 0x01: return 8;
    case 0x83: return 9;
    case 0xD9: return 10;
    case 0x9D: return 11;
    case 0x10:
      return shouldPrintAzkoyenStartupInfo(addr, hdr) ? 12 : -1;
    case 0x29:
      return shouldPrintAzkoyenStartupInfo(addr, hdr) ? 13 : -1;
    case 0x32:
      return shouldPrintAzkoyenStartupInfo(addr, hdr) ? 14 : -1;
    default: return -1;
  }
}

static bool shouldPrintStartupInfoOnce(const CcTalkTransaction& t) {
  // Alcune informazioni identificative del device cambiano raramente.
  // In modalita "informativa" le stampiamo solo la prima volta per evitare
  // rumore eccessivo su seriale.
  if (!t.hasReq || !t.hasResp) return false;
  if (t.resp.hdr != 0x00) return false;
  if (!shouldPrintStartupInfo(t.req.hdr) &&
      !shouldPrintAzkoyenStartupInfo(t.req.dest, t.req.hdr)) {
    return false;
  }

  const int8_t slot = startupInfoSlot(t.req.dest, t.req.hdr);
  if (slot < 0) return false;

  const uint8_t addr = t.req.dest;
  if (g_infoSeen[addr][slot]) return false;
  g_infoSeen[addr][slot] = true;
  return true;
}

static bool shouldPrintByCounter(const CcTalkTransaction& t) {
  // Questa funzione implementa una logica di "debounce semantico":
  // il bus viene interrogato spesso, ma stampiamo solo quando il contenuto
  // suggerisce un evento nuovo o un cambiamento operativo significativo.
  if (!t.hasReq || !t.hasResp || t.resp.hdr != 0x00) return false;

  const uint8_t hdr = t.req.hdr;
  const uint8_t addr = t.req.dest;

  // Coin acceptor buffered events counter.
  if (hdr == 0xE5 && addr == 2 && t.resp.dataLen == 11) {
    const uint8_t current = t.resp.data[0];
    const bool print = (!g_coinEventTrack.valid) || isCounterIncrement(g_coinEventTrack.last, current);
    g_coinEventTrack.valid = true;
    g_coinEventTrack.last = current;
    return print;
  }

  // Bill validator buffered events counter
  if (hdr == 0x9F && addr >= 40 && addr <= 50 && t.resp.dataLen == 11) {
    CounterTracker& tr = g_bvEventTrack[(uint8_t)(addr - 40)];
    const uint8_t current = t.resp.data[0];
    const bool print = (!tr.valid) || isCounterIncrement(tr.last, current);
    tr.valid = true;
    tr.last = current;
    return print;
  }

  // Smart Payout usa Request Status (0x1D) per gli eventi reali di banconote.
  // In view mode 3 stampiamo solo i payload non idle e solo quando cambiano,
  // cosi il poll continuo non inonda il terminale.
  if (hdr == 0x1D &&
      addr >= 40 &&
      addr <= 50) {
    if (t.resp.dataLen == 0) return false;

    const bool idleStatus = (t.resp.dataLen == 1 && t.resp.data[0] == 0x00);
    const bool isCreditStatus = (t.resp.data[0] == 0x15 || t.resp.data[0] == 0x1D || t.resp.data[0] == 0x3C);
    PayloadTracker& tr = g_bvStatusTrack[(uint8_t)(addr - 40)];
    const uint8_t len = (t.resp.dataLen > sizeof(tr.data)) ? (uint8_t)sizeof(tr.data) : t.resp.dataLen;
    const bool sameAsPrevious = tr.valid &&
                                tr.len == len &&
                                memcmp(tr.data, t.resp.data, len) == 0;

    tr.valid = true;
    tr.len = len;
    if (len > 0) {
      memcpy(tr.data, t.resp.data, len);
    }

    if (sameAsPrevious && !isCreditStatus) return false;
    return !idleStatus;
  }

  // Hopper event counter (A6), byte 0 della risposta.
  if (hdr == 0xA6 && t.resp.dataLen == 4) {
    if (addr >= 3 && addr <= 10) {
      CounterTracker& tr = g_hopperEventTrack[(uint8_t)(addr - 3)];
      const uint8_t current = t.resp.data[0];
      const bool counterPrint = (!tr.valid) || isCounterIncrement(tr.last, current);
      tr.valid = true;
      tr.last = current;

      if (configuredHopperModelForAddress(g_runtimeDeviceSettings, addr) !=
          ccms::HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) {
        return counterPrint;
      }

      HopperStatusTracker& st = g_hopperStatusTrack[(uint8_t)(addr - 3)];
      const bool payloadChanged = (!st.valid) ||
                                  (st.remaining != t.resp.data[1]) ||
                                  (st.paid != t.resp.data[2]) ||
                                  (st.unpaid != t.resp.data[3]);
      st.valid = true;
      st.remaining = t.resp.data[1];
      st.paid = t.resp.data[2];
      st.unpaid = t.resp.data[3];
      return counterPrint || payloadChanged;
    }
  }

  // Hopper polling value (85 / AB): in alcuni modelli il byte[0] non basta
  // per capire se lo stato e cambiato, quindi confrontiamo anche i valori
  // numerici del payload.
  if ((hdr == 0x85 || hdr == 0xAB) && t.resp.dataLen == 7 && addr >= 3 && addr <= 10) {
    CounterTracker& ctr = g_hopperEventTrack[(uint8_t)(addr - 3)];
    const uint8_t eventByte = t.resp.data[0];
    const bool counterPrint = (!ctr.valid) || isCounterIncrement(ctr.last, eventByte);
    ctr.valid = true;
    ctr.last = eventByte;

    HopperPollTracker& pv = g_hopperPollTrack[(uint8_t)(addr - 3)];
    const uint16_t remaining = (uint16_t)t.resp.data[1] | ((uint16_t)t.resp.data[2] << 8);
    const uint16_t paid = (uint16_t)t.resp.data[3] | ((uint16_t)t.resp.data[4] << 8);
    const uint16_t unpaid = (uint16_t)t.resp.data[5] | ((uint16_t)t.resp.data[6] << 8);
    const bool valueChanged = (!pv.valid) ||
                              (pv.remaining != remaining) ||
                              (pv.paid != paid) ||
                              (pv.unpaid != unpaid);
    pv.valid = true;
    pv.remaining = remaining;
    pv.paid = paid;
    pv.unpaid = unpaid;

    return counterPrint || valueChanged;
  }

  return false;
}

static uint32_t hopperDispensedCents(uint8_t addr) {
  // Lo stato hopper e mantenuto dai parser di device; qui lo leggiamo in sola
  // consultazione per ricavare un totale monetario consistente.
  const CcTalkHopper::HopperState* s = hopperStateForAddress(addr);
  if (!s || !s->present) return 0;
  return s->dispensedTotalValue;
}

static uint32_t recyclerEntryTotalFromCountsCents(uint16_t c10, uint16_t c20, uint16_t c50) {
  return (uint32_t)c10 * 1000UL + (uint32_t)c20 * 2000UL + (uint32_t)c50 * 5000UL;
}

static bool selectManualRecyclerTargetAddress(uint8_t& targetAddr) {
  for (uint8_t i = 0; i < g_persistentRecyclerCount; i++) {
    if (!g_persistentRecycler[i].valid) continue;
    targetAddr = g_persistentRecycler[i].addr;
    return true;
  }

  const uint16_t mask = configuredBillValidatorMask();
  for (uint8_t addr = ccms::kBillValidatorAddressMin; addr <= ccms::kBillValidatorAddressMax; addr++) {
    if ((mask & ccms::billValidatorAddressBit(addr)) == 0) continue;
    targetAddr = addr;
    return true;
  }

  return false;
}

static bool recyclerEntryEqual(const ccms::SystemStatus::RecyclerInventoryEntry& a,
                               const ccms::SystemStatus::RecyclerInventoryEntry& b) {
  return (a.valid == b.valid) &&
         (a.addr == b.addr) &&
         (a.count10 == b.count10) &&
         (a.count20 == b.count20) &&
         (a.count50 == b.count50) &&
         (a.totalCents == b.totalCents);
}

static uint32_t recyclerTotalFromEntriesCents(const ccms::SystemStatus::RecyclerInventoryEntry* entries,
                                              uint8_t count) {
  if (!entries || count == 0) return 0;

  uint32_t total = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (!entries[i].valid) continue;
    total += entries[i].totalCents;
  }
  return total;
}

static bool updatePersistentRecyclerCache(const ccms::SystemStatus::RecyclerInventoryEntry* entries,
                                          uint8_t count,
                                          uint32_t fallbackTotalCents) {
  // Mantiene una copia locale dell'inventario recycler che sopravvive anche
  // quando, in un dato istante, i device live non stanno esponendo il dato.
  const uint8_t maxEntries = ccms::SystemStatus::kMaxRecyclerEntries;
  if (count > maxEntries) count = maxEntries;

  const uint32_t newTotalCents =
      (count > 0 && entries) ? recyclerTotalFromEntriesCents(entries, count) : fallbackTotalCents;

  bool changed = (count != g_persistentRecyclerCount) ||
                 (newTotalCents != g_persistentBaseTotals.recyclerInventoryCents);

  if (!changed) {
    for (uint8_t i = 0; i < count; i++) {
      if (!recyclerEntryEqual(g_persistentRecycler[i], entries[i])) {
        changed = true;
        break;
      }
    }
  }

  if (!changed) return false;

  memset(g_persistentRecycler, 0, sizeof(g_persistentRecycler));
  if (entries && count > 0) {
    for (uint8_t i = 0; i < count; i++) {
      g_persistentRecycler[i] = entries[i];
    }
  }

  g_persistentRecyclerCount = count;
  g_persistentBaseTotals.recyclerInventoryCents = newTotalCents;
  return true;
}

static void publishPersistentStateToSystemStatus(const EconomicTotals& totals) {
  // Pubblica economici e recycler come uno snapshot unico per evitare che
  // il task di persistenza legga combinazioni miste durante un update.
  g_systemStatus.replaceEconomicAndRecycler(totals.cntotBanconoteInCents,
                                            totals.cntotMoneteOutCents,
                                            totals.cntotMoneteInCents,
                                            totals.cntotBanconoteOutCents,
                                            totals.cassaCents,
                                            totals.recyclerInventoryCents,
                                            g_coinLevelBaseCents,
                                            g_persistentRecycler,
                                            g_persistentRecyclerCount);
}

static bool refreshRecyclerCacheFromLiveStates() {
  // Raccoglie lo stato recycler "live" dai bill validator attualmente presenti.
  // Se almeno un dato valido e disponibile, aggiorna la cache persistente.
  const uint16_t recyclerMask = configuredBillValidatorMask();
  ccms::SystemStatus::RecyclerInventoryEntry live[ccms::SystemStatus::kMaxRecyclerEntries] = {};
  ccms::SystemStatus::RecyclerInventoryEntry filteredCached[ccms::SystemStatus::kMaxRecyclerEntries] = {};
  uint8_t liveCount = 0;
  uint8_t filteredCachedCount = 0;
  uint32_t liveTotalCents = 0;

  for (uint8_t addr = ccms::kBillValidatorAddressMin; addr <= ccms::kBillValidatorAddressMax; addr++) {
    if (!billValidatorContributes(recyclerMask, addr)) continue;
    const CcTalkBillValidator::BillValidatorState* s = billValidatorStateForAddress(addr);
    if (!s || !s->present || !s->recyclerInventoryValid) continue;

    const uint32_t totalCents = (uint32_t)(s->recyclerInventoryTotalEuro * 100UL);
    if (liveCount < ccms::SystemStatus::kMaxRecyclerEntries) {
      ccms::SystemStatus::RecyclerInventoryEntry& e = live[liveCount++];
      e.valid = true;
      e.addr = addr;
      e.count10 = s->recyclerCount10;
      e.count20 = s->recyclerCount20;
      e.count50 = s->recyclerCount50;
      e.totalCents = totalCents;
    }
    liveTotalCents += totalCents;
  }

  if (liveCount > 0) {
    return updatePersistentRecyclerCache(live, liveCount, liveTotalCents);
  }

  for (uint8_t i = 0; i < g_persistentRecyclerCount; i++) {
    const ccms::SystemStatus::RecyclerInventoryEntry& cached = g_persistentRecycler[i];
    if (!cached.valid) continue;
    if (!billValidatorContributes(recyclerMask, cached.addr)) continue;
    if (filteredCachedCount >= ccms::SystemStatus::kMaxRecyclerEntries) break;
    filteredCached[filteredCachedCount++] = cached;
  }

  const uint32_t filteredTotalCents = recyclerTotalFromEntriesCents(filteredCached, filteredCachedCount);
  return updatePersistentRecyclerCache(
      (filteredCachedCount > 0) ? filteredCached : nullptr,
      filteredCachedCount,
      filteredTotalCents);
}

static void applyPersistentSnapshot(const ccms::FramPersistence::Snapshot& snapshot) {
  // Snapshot FRAM -> stato runtime:
  // 1. ripristina la base persistente dei contatori
  // 2. azzera gli offset di sessione
  // 3. ricostruisce la cache recycler
  // 4. pubblica il tutto in SystemStatus
  g_persistentBaseTotals = EconomicTotals();
  g_sessionOffsetTotals = EconomicTotals();
  g_persistentBaseTotals.cntotBanconoteInCents = snapshot.economic.cntotBanconoteInCents;
  g_persistentBaseTotals.cntotMoneteOutCents = snapshot.economic.cntotMoneteOutCents;
  g_persistentBaseTotals.cntotMoneteInCents = snapshot.economic.cntotMoneteInCents;
  g_persistentBaseTotals.cntotBanconoteOutCents = snapshot.economic.cntotBanconoteOutCents;
  g_persistentBaseTotals.cassaCents = snapshot.economic.cassaCents;
  g_persistentBaseTotals.recyclerInventoryCents = snapshot.economic.recyclerInventoryTotaleCents;
  g_coinLevelBaseCents = snapshot.economic.coinLevelBaseCents;

  ccms::SystemStatus::RecyclerInventoryEntry restored[ccms::SystemStatus::kMaxRecyclerEntries] = {};
  uint8_t restoredCount = 0;
  const uint8_t maxEntries = ccms::SystemStatus::kMaxRecyclerEntries;
  uint8_t count = snapshot.recyclerCount;
  if (count > maxEntries) count = maxEntries;
  for (uint8_t i = 0; i < count; i++) {
    const ccms::SystemStatus::RecyclerInventoryEntry& src = snapshot.recycler[i];
    if (!src.valid) continue;
    ccms::SystemStatus::RecyclerInventoryEntry& dst = restored[restoredCount++];
    dst = src;
    CcTalkBillValidator* parser = billValidatorParserForAddress(dst.addr);
    if (parser) {
      parser->preloadRecyclerInventory(dst.addr, dst.count10, dst.count20, dst.count50);
    }
    if (dst.totalCents == 0) {
      dst.totalCents = recyclerEntryTotalFromCountsCents(dst.count10, dst.count20, dst.count50);
    }
    if (restoredCount >= maxEntries) break;
  }

  updatePersistentRecyclerCache(restored, restoredCount, snapshot.economic.recyclerInventoryTotaleCents);

  publishPersistentStateToSystemStatus(g_persistentBaseTotals);
}

static uint32_t subtractSaturate(uint32_t value, uint32_t offset) {
  // Sottrazione "safe": evita underflow quando il contatore live e minore
  // dell'offset memorizzato (per reset hardware, sostituzione device, ecc.).
  if (value < offset) return 0;
  return (uint32_t)(value - offset);
}

static EconomicTotals collectSessionEconomicRaw() {
  // Legge i totalizzatori live dai parser device senza considerare ancora
  // persistenza o offset di sessione.
  EconomicTotals session;
  bool recyclerLiveAvailable = false;
  uint32_t recyclerLiveCents = 0;
  const bool coinAcceptorInEnabled = g_runtimeDeviceSettings.coinAcceptorInEnabled;
  ccms::AppSettings normalizedSettings = g_runtimeDeviceSettings;
  normalizeCounterRoutingSettings(normalizedSettings);
  const uint16_t recyclerMask = configuredBillValidatorMask();
  const uint8_t coinInHopperMask = normalizedSettings.coinInHopperMask;
  const uint8_t coinOutHopperMask = normalizedSettings.coinOutHopperMask;
  const uint16_t billInValidatorMask = normalizedSettings.billInValidatorMask;
  const uint16_t billOutValidatorMask = normalizedSettings.billOutValidatorMask;

  // BV: totale banconote in/out (valori in EUR convertiti in centesimi).
  for (uint8_t addr = ccms::kBillValidatorAddressMin; addr <= ccms::kBillValidatorAddressMax; addr++) {
    const CcTalkBillValidator::BillValidatorState* s = billValidatorStateForAddress(addr);
    if (!s || !s->present) continue;
    const bool contributesBillIn = billValidatorContributes(billInValidatorMask, addr);
    const bool contributesBillOut = billValidatorContributes(billOutValidatorMask, addr);
    if (contributesBillIn) {
      session.cntotBanconoteInCents += (uint32_t)(s->acceptedTotalEuro * 100UL);
    }
    if (contributesBillOut) {
      session.cntotBanconoteOutCents += (uint32_t)(s->dispensedTotalEuro * 100UL);
    }
    if (contributesBillIn) {
      session.cassaCents += (uint32_t)(s->cashboxTotalEuro * 100UL);
    }
    if (billValidatorContributes(recyclerMask, addr) && s->recyclerInventoryValid) {
      recyclerLiveAvailable = true;
      recyclerLiveCents += (uint32_t)(s->recyclerInventoryTotalEuro * 100UL);
    }
  }

  // Crediti banconote JCM push-mode: il BV JCM ha checksum non standard e
  // bypassa il routing normale. L'importo viene ricavato dal comando A7 che
  // il master invia all'hopper subito dopo l'accettazione della banconota.
  session.cntotBanconoteInCents += g_jcmBillCreditCents;
  session.cassaCents            += g_jcmBillCreditCents;

  for (uint8_t addr = ccms::kHopperAddressMin; addr <= ccms::kHopperAddressMax; addr++) {
    const uint32_t dispensedCents = hopperDispensedCents(addr);
    if (hopperContributes(coinOutHopperMask, addr)) {
      session.cntotMoneteOutCents += dispensedCents;
    }
    if (hopperContributes(coinInHopperMask, addr)) {
      session.cntotMoneteInCents += dispensedCents;
    }
  }

  if (coinAcceptorInEnabled) {
    session.cntotMoneteInCents += coinAcceptorAcceptedCents();
  }

  session.recyclerInventoryCents = recyclerLiveAvailable ? recyclerLiveCents : g_persistentBaseTotals.recyclerInventoryCents;

  return session;
}

static EconomicTotals collectEconomicTotals() {
  // Costruisce il totale "pubblicabile" in tre passi:
  // 1. legge i valori runtime correnti
  // 2. sottrae gli offset della sessione attuale
  // 3. somma la base persistente ripristinata dalla FRAM
  const EconomicTotals sessionRaw = collectSessionEconomicRaw();
  EconomicTotals session = sessionRaw;
  session.cntotBanconoteInCents = subtractSaturate(sessionRaw.cntotBanconoteInCents, g_sessionOffsetTotals.cntotBanconoteInCents);
  session.cntotMoneteOutCents = subtractSaturate(sessionRaw.cntotMoneteOutCents, g_sessionOffsetTotals.cntotMoneteOutCents);
  session.cntotMoneteInCents = subtractSaturate(sessionRaw.cntotMoneteInCents, g_sessionOffsetTotals.cntotMoneteInCents);
  session.cntotBanconoteOutCents = subtractSaturate(sessionRaw.cntotBanconoteOutCents, g_sessionOffsetTotals.cntotBanconoteOutCents);
  session.cassaCents = subtractSaturate(sessionRaw.cassaCents, g_sessionOffsetTotals.cassaCents);
  session.recyclerInventoryCents = sessionRaw.recyclerInventoryCents;

  EconomicTotals t = g_persistentBaseTotals;
  t.cntotBanconoteInCents += session.cntotBanconoteInCents;
  t.cntotMoneteOutCents += session.cntotMoneteOutCents;
  t.cntotMoneteInCents += session.cntotMoneteInCents;
  t.cntotBanconoteOutCents += session.cntotBanconoteOutCents;
  t.cassaCents += session.cassaCents;
  t.recyclerInventoryCents = session.recyclerInventoryCents;

  return t;
}

static bool economicTotalsEqual(const EconomicTotals& a, const EconomicTotals& b) {
  return (a.cntotBanconoteInCents == b.cntotBanconoteInCents) &&
         (a.cntotMoneteOutCents == b.cntotMoneteOutCents) &&
         (a.cntotMoneteInCents == b.cntotMoneteInCents) &&
         (a.cntotBanconoteOutCents == b.cntotBanconoteOutCents) &&
         (a.cassaCents == b.cassaCents) &&
         (a.recyclerInventoryCents == b.recyclerInventoryCents);
}

static void updateEconomicStatus(const EconomicTotals& t) {
  // SystemStatus e il "modello" letto da web UI / cloud / diagnostica.
  // Tutte le viste esterne devono essere aggiornate tramite questo oggetto.
  publishPersistentStateToSystemStatus(t);
}

static void refreshEconomicStatusForRemoteSave() {
  const bool recyclerChanged = refreshRecyclerCacheFromLiveStates();
  const EconomicTotals current = collectEconomicTotals();
  const bool totalsChanged = !g_economicTotalsValid || !economicTotalsEqual(current, g_lastEconomicTotals);

  g_lastEconomicTotals = current;
  g_economicTotalsValid = true;
  updateEconomicStatus(current);

  if (g_framReady && (totalsChanged || recyclerChanged)) {
    markFramStateChanged();
  }
}

static void rememberRemoteMasterRuntimeRequest(const char* command, const char* payload) {
  const char* reqCommand = (command && command[0] != '\0') ? command : "SNAPSHOT";
  const char* reqPayload = payload ? payload : "";

  strncpy(g_remoteMasterRuntimeCommand, reqCommand, sizeof(g_remoteMasterRuntimeCommand) - 1);
  g_remoteMasterRuntimeCommand[sizeof(g_remoteMasterRuntimeCommand) - 1] = '\0';
  strncpy(g_remoteMasterRuntimePayload, reqPayload, sizeof(g_remoteMasterRuntimePayload) - 1);
  g_remoteMasterRuntimePayload[sizeof(g_remoteMasterRuntimePayload) - 1] = '\0';
  g_remoteMasterRuntimeAppliedMs = millis();
}

static void normalizeRemoteCommandName(const char* value, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  out[0] = '\0';
  if (!value) return;

  size_t writeIndex = 0;
  bool lastWasSeparator = true;
  for (size_t i = 0; value[i] != '\0' && writeIndex + 1 < outLen; ++i) {
    char ch = value[i];
    if (ch >= 'a' && ch <= 'z') ch = (char)('A' + (ch - 'a'));

    const bool isAlphaNum =
      ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'));
    if (isAlphaNum) {
      out[writeIndex++] = ch;
      lastWasSeparator = false;
      continue;
    }

    const bool isSeparator =
      (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
       ch == '-' || ch == '_' || ch == '/' || ch == '.');
    if (isSeparator && !lastWasSeparator && writeIndex + 1 < outLen) {
      out[writeIndex++] = '_';
      lastWasSeparator = true;
    }
  }

  if (writeIndex > 0 && out[writeIndex - 1] == '_') {
    --writeIndex;
  }
  out[writeIndex] = '\0';
}

static bool onRemoteMasterRequest(const char* command,
                                  const char* requestPayload,
                                  String& responseMessage,
                                  void* userData) {
  (void)userData;

  const char* reqCommand = (command && command[0] != '\0') ? command : "SNAPSHOT";
  const char* payload = requestPayload ? requestPayload : "";

  refreshEconomicStatusForRemoteSave();

  char normalizedCommand[48] = {0};
  normalizeRemoteCommandName(reqCommand, normalizedCommand, sizeof(normalizedCommand));

  if (strcmp(normalizedCommand, "CLEAR_RUNTIME") == 0) {
    g_remoteMasterRuntimeCommand[0] = '\0';
    g_remoteMasterRuntimePayload[0] = '\0';
    g_remoteMasterRuntimeAppliedMs = millis();
    responseMessage = "payload runtime cancellato";
    logRuntimeLine("[REMOTE_DB] richiesta master applicata: CLEAR_RUNTIME", true);
    return true;
  }

  rememberRemoteMasterRuntimeRequest(reqCommand, payload);

  if (strcmp(normalizedCommand, "AZZERAMENTO") == 0 ||
      strcmp(normalizedCommand, "AZZERA") == 0 ||
      strcmp(normalizedCommand, "RESET_COUNTERS") == 0) {
    return applyCurrentCoinLevelAsBase(
      "[REMOTE_DB] contatori e cassa azzerati da richiesta remota",
      "contatori e cassa azzerati; livello monete iniziale allineato al valore attuale",
      responseMessage);
  }

  if (strcmp(normalizedCommand, "IMPOSTA_VALORE_ATTUALE") == 0 ||
      strcmp(normalizedCommand, "SET_CURRENT_VALUE") == 0 ||
      strcmp(normalizedCommand, "SET_COIN_BASE") == 0 ||
      strcmp(normalizedCommand, "IMPOSTA_LIVELLO_INIZIALE") == 0) {
    {
      char dbg[192] = {0};
      snprintf(dbg, sizeof(dbg),
               "[REMOTE_DB] IMPOSTA_VALORE_ATTUALE payload_raw='%s' (len=%u)",
               payload, (unsigned)strlen(payload));
      logRuntimeLine(dbg, true);
    }
    int64_t coinLevelBaseCents = 0;
    bool useCurrentCoinLevel = false;
    if (!parseRemoteCoinLevelBasePayload(payload,
                                         coinLevelBaseCents,
                                         useCurrentCoinLevel,
                                         responseMessage)) {
      char dbg[128] = {0};
      snprintf(dbg, sizeof(dbg),
               "[REMOTE_DB] IMPOSTA_VALORE_ATTUALE parse FALLITO: %s",
               responseMessage.c_str());
      logRuntimeLine(dbg, true);
      return false;
    }
    {
      char dbg[128] = {0};
      snprintf(dbg, sizeof(dbg),
               "[REMOTE_DB] IMPOSTA_VALORE_ATTUALE parse OK: useCurrentLevel=%d coinLevelBaseCents=%ld",
               (int)useCurrentCoinLevel, (long)coinLevelBaseCents);
      logRuntimeLine(dbg, true);
    }

    if (useCurrentCoinLevel) {
      return applyCurrentCoinLevelAsBase(
        "[REMOTE_DB] livello monete iniziale riallineato al valore attuale da richiesta remota",
        "livello monete iniziale riallineato al valore attuale; contatori e cassa azzerati",
        responseMessage);
    }

    return applyCoinBaseResetAction(
      (uint32_t)coinLevelBaseCents,
      "[REMOTE_DB] livello monete iniziale impostato da richiesta remota",
      "livello monete iniziale impostato; contatori e cassa azzerati",
      responseMessage);
  }

  char line[256] = {0};
  if (g_remoteMasterRuntimePayload[0] != '\0') {
    snprintf(line, sizeof(line),
             "[REMOTE_DB] richiesta master cmd=%s payload=%s",
             g_remoteMasterRuntimeCommand,
             g_remoteMasterRuntimePayload);
    responseMessage = "payload applicato in RAM";
  } else {
    snprintf(line, sizeof(line),
             "[REMOTE_DB] richiesta master cmd=%s senza payload",
             g_remoteMasterRuntimeCommand);
    // Costruisce il JSON con i contatori economici attuali (valori in centesimi).
    // Il prefisso '{' viene riconosciuto da mqttPublishResponse per includerlo
    // come campo "data" invece di "message" stringa.
    const int64_t coinLevelCents =
        currentCoinLevelCentsFromTotals(g_lastEconomicTotals);
    char dataJson[384] = {0};
    snprintf(dataJson, sizeof(dataJson),
             "{\"banconoteInCents\":%lu"
             ",\"moneteInCents\":%lu"
             ",\"moneteOutCents\":%lu"
             ",\"banconoteOutCents\":%lu"
             ",\"cassaCents\":%lu"
             ",\"coinLevelCents\":%ld"
             ",\"recyclerCents\":%lu}",
             (unsigned long)g_lastEconomicTotals.cntotBanconoteInCents,
             (unsigned long)g_lastEconomicTotals.cntotMoneteInCents,
             (unsigned long)g_lastEconomicTotals.cntotMoneteOutCents,
             (unsigned long)g_lastEconomicTotals.cntotBanconoteOutCents,
             (unsigned long)g_lastEconomicTotals.cassaCents,
             (long)coinLevelCents,
             (unsigned long)g_lastEconomicTotals.recyclerInventoryCents);
    responseMessage = dataJson;
  }
  logRuntimeLine(line, true);
  return true;
}

static void logEconomicTotalsFromStatus() {
  // Le righe vengono formattate da SystemStatus per garantire coerenza tra
  // rappresentazione seriale e rappresentazione esposta ad altri servizi.
  char line[ccms::SystemStatus::kFormattedLineSize] = {0};
  logRuntimeLine("");
  g_systemStatus.formatEconomicLine1(line, sizeof(line));
  logRuntimeLine(line);
  g_systemStatus.formatEconomicLine2(line, sizeof(line));
  logRuntimeLine(line);
  g_systemStatus.formatEconomicLine3(line, sizeof(line));
  logRuntimeLine(line);

  for (uint8_t i = 0; i < g_systemStatus.recyclerEntryCount(); i++) {
    if (g_systemStatus.formatRecyclerLine(i, line, sizeof(line))) {
      logRuntimeLine(line);
    }
  }
}

static void captureFramSnapshot(ccms::FramPersistence::Snapshot& snapshot) {
  // Estrae dallo stato pubblicato tutti i dati necessari alla persistenza.
  // Scegliamo SystemStatus come sorgente per evitare duplicazioni di formato.
  // La copia deve essere atomica per non serializzare economici/recycler di
  // generazioni diverse mentre sniffer e task servizi lavorano in parallelo.
  snapshot = ccms::FramPersistence::Snapshot();
  g_systemStatus.snapshotForPersistence(snapshot.economic,
                                        snapshot.recycler,
                                        snapshot.recyclerCount,
                                        ccms::SystemStatus::kMaxRecyclerEntries);
}

static bool economicFieldsEqual(const ccms::SystemStatus::EconomicFields& a,
                                const ccms::SystemStatus::EconomicFields& b) {
  return (a.cntotBanconoteInCents == b.cntotBanconoteInCents) &&
         (a.cntotMoneteOutCents == b.cntotMoneteOutCents) &&
         (a.cntotMoneteInCents == b.cntotMoneteInCents) &&
         (a.cntotBanconoteOutCents == b.cntotBanconoteOutCents) &&
         (a.cntotBanconoteCents == b.cntotBanconoteCents) &&
         (a.cntotMoneteCents == b.cntotMoneteCents) &&
         (a.saldoCents == b.saldoCents) &&
         (a.cassaCents == b.cassaCents) &&
         (a.recyclerInventoryTotaleCents == b.recyclerInventoryTotaleCents) &&
         (a.coinLevelBaseCents == b.coinLevelBaseCents) &&
         (a.coinCurrentCents == b.coinCurrentCents);
}

static bool framSnapshotsEqual(const ccms::FramPersistence::Snapshot& a,
                               const ccms::FramPersistence::Snapshot& b) {
  if (!economicFieldsEqual(a.economic, b.economic)) return false;
  if (a.recyclerCount != b.recyclerCount) return false;

  for (uint8_t i = 0; i < a.recyclerCount; i++) {
    if (!recyclerEntryEqual(a.recycler[i], b.recycler[i])) return false;
  }
  return true;
}

static void serviceFramCongruenceCheck() {
  // Verifica a bassa frequenza che lo snapshot persistito in FRAM corrisponda
  // a quello pubblicato runtime. Se `g_framDirty` e attivo, una differenza e
  // normale e viene riportata come "salvataggio pendente", non come errore.
  if (!g_framReady) return;

  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastFramCongruenceCheckMs) < appconfig::FRAM_CONGRUENCE_CHECK_INTERVAL_MS) return;
  g_lastFramCongruenceCheckMs = now;

  ccms::FramPersistence::Snapshot runtimeSnapshot;
  captureFramSnapshot(runtimeSnapshot);

  ccms::FramPersistence::Snapshot framSnapshot;
  if (!g_framStore.load(framSnapshot)) {
    bool dirty = false;
    uint32_t generation = 0;
    readFramDirtyState(dirty, generation);
    (void)generation;
    g_framCongruencePendingLatched = false;
    if (!g_framCongruenceMismatchLatched && !dirty) {
      logRuntimeLine("[FRAM] verifica congruenza fallita: snapshot persistito non leggibile", true);
    }
    g_framCongruenceMismatchLatched = !dirty;
    return;
  }

  if (framSnapshotsEqual(runtimeSnapshot, framSnapshot)) {
    if (g_framCongruenceMismatchLatched || g_framCongruencePendingLatched) {
      logRuntimeLine("[FRAM] verifica congruenza OK", true);
    }
    g_framCongruenceMismatchLatched = false;
    g_framCongruencePendingLatched = false;
    return;
  }

  bool dirty = false;
  uint32_t generation = 0;
  readFramDirtyState(dirty, generation);
  (void)generation;
  if (dirty) {
    if (!g_framCongruencePendingLatched) {
      logRuntimeLine("[FRAM] verifica congruenza: differenze attese, salvataggio pendente", true);
    }
    g_framCongruencePendingLatched = true;
    g_framCongruenceMismatchLatched = false;
    return;
  }

  g_framCongruencePendingLatched = false;
  if (!g_framCongruenceMismatchLatched) {
    logRuntimeLine("[FRAM] incongruenza: runtime e snapshot FRAM divergono con dirty=0", true);
  }
  g_framCongruenceMismatchLatched = true;
}

static void flushFramIfDue() {
  // La scrittura viene rate-limited per non stressare il supporto e per
  // evitare di salvare troppe volte durante burst ravvicinati di eventi.
  bool dirty = false;
  uint32_t snapshotGeneration = 0;
  readFramDirtyState(dirty, snapshotGeneration);
  if (!g_framReady || !dirty) return;

  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastFramWriteMs) < appconfig::FRAM_WRITE_MIN_INTERVAL_MS) return;

  ccms::FramPersistence::Snapshot snapshot;
  captureFramSnapshot(snapshot);
  readFramDirtyState(dirty, snapshotGeneration);
  if (!dirty) return;

  g_lastFramWriteMs = now;
  if (!g_framStore.save(snapshot)) {
    if (!g_framSaveErrorLatched) {
      logRuntimeLine("[FRAM] errore scrittura", true);
      g_framSaveErrorLatched = true;
    }
    return;
  }

  clearFramDirtyIfGenerationUnchanged(snapshotGeneration);
  if (g_framSaveErrorLatched) {
    logRuntimeLine("[FRAM] scrittura ripristinata", true);
    g_framSaveErrorLatched = false;
  }
}

static bool saveFramNow(String& message) {
  // Variante sincrona usata da azioni esplicite (tipicamente web).
  // In questi casi l'utente si aspetta un feedback immediato di successo/errore.
  if (!g_framReady) {
    message = "FRAM non disponibile";
    return false;
  }

  ccms::FramPersistence::Snapshot snapshot;
  captureFramSnapshot(snapshot);
  bool dirty = false;
  uint32_t snapshotGeneration = 0;
  readFramDirtyState(dirty, snapshotGeneration);
  (void)dirty;

  g_lastFramWriteMs = millis();
  if (!g_framStore.save(snapshot)) {
    ensureFramDirtyPending();
    g_framSaveErrorLatched = true;
    message = "errore scrittura FRAM";
    return false;
  }

  clearFramDirtyIfGenerationUnchanged(snapshotGeneration);
  g_framSaveErrorLatched = false;
  message = "valori salvati su FRAM";
  return true;
}

static void flushFramBeforeRestart() {
  bool dirty = false;
  uint32_t generation = 0;
  readFramDirtyState(dirty, generation);
  (void)generation;
  if (!g_framReady || !dirty) return;

  String message;
  if (saveFramNow(message)) {
    logRuntimeLine("[FRAM] snapshot salvata prima del riavvio", true);
    return;
  }

  char line[96] = {0};
  snprintf(line, sizeof(line), "[FRAM] riavvio con snapshot non salvata: %s", message.c_str());
  logRuntimeLine(line, true);
}

static int64_t currentCoinLevelCentsFromTotals(const EconomicTotals& totals) {
  return (int64_t)g_coinLevelBaseCents +
         (int64_t)totals.cntotMoneteInCents -
         (int64_t)totals.cntotMoneteOutCents;
}

static bool tryExtractNamedNumericToken(const String& payload,
                                        const char* key,
                                        String& token) {
  if (!key || key[0] == '\0') return false;

  const String quotedKey = String("\"") + key + "\"";
  int keyPos = payload.indexOf(quotedKey);
  int advance = quotedKey.length();
  if (keyPos < 0) {
    keyPos = payload.indexOf(key);
    advance = strlen(key);
  }
  if (keyPos < 0) return false;

  int pos = keyPos + advance;
  while (pos < payload.length()) {
    const char ch = payload.charAt(pos);
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
        ch == ':' || ch == '=' || ch == '"' || ch == '\'') {
      ++pos;
      continue;
    }
    break;
  }

  const int start = pos;
  if (pos < payload.length()) {
    const char sign = payload.charAt(pos);
    if (sign == '+' || sign == '-') ++pos;
  }

  bool hasDigit = false;
  while (pos < payload.length()) {
    const char ch = payload.charAt(pos);
    if (ch >= '0' && ch <= '9') {
      hasDigit = true;
      ++pos;
      continue;
    }
    if (ch == '.' || ch == ',') {
      ++pos;
      continue;
    }
    break;
  }

  if (!hasDigit) return false;
  token = payload.substring(start, pos);
  token.trim();
  return token.length() > 0;
}

static bool parseIntegerToken(const String& token, int64_t& outValue) {
  String normalized = token;
  normalized.trim();
  if (normalized.length() == 0 || normalized.length() >= 40) return false;

  char buffer[40] = {0};
  normalized.toCharArray(buffer, sizeof(buffer));
  char* endPtr = nullptr;
  const long long parsed = strtoll(buffer, &endPtr, 10);
  if (endPtr == buffer || (endPtr && *endPtr != '\0')) return false;
  outValue = (int64_t)parsed;
  return true;
}

static bool parseEuroTokenToCents(const String& token, int64_t& outValue) {
  String normalized = token;
  normalized.trim();
  normalized.replace(',', '.');
  if (normalized.length() == 0 || normalized.length() >= 40) return false;

  char buffer[40] = {0};
  normalized.toCharArray(buffer, sizeof(buffer));
  char* endPtr = nullptr;
  const double parsed = strtod(buffer, &endPtr);
  if (endPtr == buffer || (endPtr && *endPtr != '\0')) return false;
  if (parsed < 0.0) return false;

  outValue = (int64_t)(parsed * 100.0 + 0.5);
  return true;
}

static bool parseRemoteCoinLevelBasePayload(const char* payload,
                                            int64_t& coinLevelBaseCents,
                                            bool& useCurrentCoinLevel,
                                            String& message) {
  useCurrentCoinLevel = false;
  coinLevelBaseCents = 0;
  message = "";

  String rawPayload = payload ? String(payload) : String("");
  rawPayload.trim();
  if (rawPayload.length() == 0) {
    useCurrentCoinLevel = true;
    return true;
  }

  String token;
  if (tryExtractNamedNumericToken(rawPayload, "coinLevelBaseCents", token) ||
      tryExtractNamedNumericToken(rawPayload, "valueCents", token) ||
      tryExtractNamedNumericToken(rawPayload, "valoreCentesimi", token)) {
    if (!parseIntegerToken(token, coinLevelBaseCents)) {
      message = "payload remoto con coinLevelBaseCents non valido";
      return false;
    }
  } else if (tryExtractNamedNumericToken(rawPayload, "coinLevelBaseEur", token) ||
             tryExtractNamedNumericToken(rawPayload, "valueEur", token) ||
             tryExtractNamedNumericToken(rawPayload, "valoreEuro", token) ||
             tryExtractNamedNumericToken(rawPayload, "valoreAttuale", token)) {
    if (!parseEuroTokenToCents(token, coinLevelBaseCents)) {
      message = "payload remoto con coinLevelBaseEur non valido";
      return false;
    }
  } else if (rawPayload.indexOf('.') >= 0 || rawPayload.indexOf(',') >= 0) {
    if (!parseEuroTokenToCents(rawPayload, coinLevelBaseCents)) {
      message = "payload remoto non valido: atteso EUR o centesimi";
      return false;
    }
  } else {
    if (!parseIntegerToken(rawPayload, coinLevelBaseCents)) {
      message = "payload remoto non valido: atteso EUR o centesimi";
      return false;
    }
  }

  if (coinLevelBaseCents < 0 || coinLevelBaseCents > 0xFFFFFFFFLL) {
    message = "valore monete iniziale remoto fuori range";
    return false;
  }

  return true;
}

static void resetCountersRuntimeState(uint32_t coinLevelBaseCents, bool resetCassa) {
  // "Reset" logico dei contatori:
  // - memorizza gli attuali valori live come nuovo offset di sessione
  // - ricostruisce la base persistente solo con i campi da conservare
  // - rigenera lo stato economico pubblicato
  refreshRecyclerCacheFromLiveStates();
  const EconomicTotals currentTotals = collectEconomicTotals();
  const EconomicTotals sessionRaw = collectSessionEconomicRaw();

  EconomicTotals newBase;
  if (!resetCassa) {
    newBase.cassaCents = currentTotals.cassaCents;
  }
  newBase.recyclerInventoryCents = sessionRaw.recyclerInventoryCents;

  g_persistentBaseTotals = newBase;
  g_sessionOffsetTotals = sessionRaw;
  g_coinLevelBaseCents = coinLevelBaseCents;
  g_economicTotalsValid = false;

  const EconomicTotals current = collectEconomicTotals();
  g_lastEconomicTotals = current;
  g_economicTotalsValid = true;
  updateEconomicStatus(current);
  g_remoteRegistro.noteChangeEvent();
}

static bool applyCoinBaseResetAction(uint32_t coinLevelBaseCents,
                                     const char* successLogLine,
                                     const char* successMessage,
                                     String& message) {
  resetCountersRuntimeState(coinLevelBaseCents, true);
  markFramStateChanged();

  const bool saved = saveFramNow(message);
  if (saved) {
    if (successLogLine && successLogLine[0] != '\0') {
      logRuntimeLine(successLogLine, true);
    }
    if (successMessage && successMessage[0] != '\0') {
      message = successMessage;
    }
  }
  return saved;
}

static bool applyBillRecyclerManualAction(uint16_t cassette10Count,
                                          uint16_t cassette20Count,
                                          uint16_t cassette50Count,
                                          const char* successLogLine,
                                          const char* successMessage,
                                          String& message) {
  uint8_t targetAddr = 0;
  if (!selectManualRecyclerTargetAddress(targetAddr)) {
    message = "nessun bill validator configurato per il recycler";
    return false;
  }

  CcTalkBillValidator* parser = billValidatorParserForAddress(targetAddr);
  if (!parser) {
    message = "bill validator recycler non disponibile";
    return false;
  }

  parser->preloadRecyclerInventory(targetAddr, cassette10Count, cassette20Count, cassette50Count);

  ccms::SystemStatus::RecyclerInventoryEntry updated[ccms::SystemStatus::kMaxRecyclerEntries] = {};
  uint8_t updatedCount = 0;
  bool replaced = false;
  for (uint8_t i = 0; i < g_persistentRecyclerCount; i++) {
    if (!g_persistentRecycler[i].valid) continue;
    ccms::SystemStatus::RecyclerInventoryEntry entry = g_persistentRecycler[i];
    if (entry.addr == targetAddr) {
      entry.count10 = cassette10Count;
      entry.count20 = cassette20Count;
      entry.count50 = cassette50Count;
      entry.totalCents = recyclerEntryTotalFromCountsCents(cassette10Count, cassette20Count, cassette50Count);
      replaced = true;
    }
    if (updatedCount < ccms::SystemStatus::kMaxRecyclerEntries) {
      updated[updatedCount++] = entry;
    }
  }

  if (!replaced && updatedCount < ccms::SystemStatus::kMaxRecyclerEntries) {
    ccms::SystemStatus::RecyclerInventoryEntry& entry = updated[updatedCount++];
    entry.valid = true;
    entry.addr = targetAddr;
    entry.count10 = cassette10Count;
    entry.count20 = cassette20Count;
    entry.count50 = cassette50Count;
    entry.totalCents = recyclerEntryTotalFromCountsCents(cassette10Count, cassette20Count, cassette50Count);
  }

  if (!updatePersistentRecyclerCache(updated, updatedCount, 0)) {
    // Anche quando il cache non cambia, riallineiamo lo stato pubblicato e salviamo.
  }

  const EconomicTotals current = collectEconomicTotals();
  g_lastEconomicTotals = current;
  g_economicTotalsValid = true;
  updateEconomicStatus(current);
  g_remoteRegistro.noteChangeEvent();
  markFramStateChanged();

  const bool saved = saveFramNow(message);
  if (saved) {
    if (successLogLine && successLogLine[0] != '\0') {
      logRuntimeLine(successLogLine, true);
    }
    if (successMessage && successMessage[0] != '\0') {
      message = successMessage;
    }
  }
  return saved;
}

static bool applyCurrentCoinLevelAsBase(const char* successLogLine,
                                        const char* successMessage,
                                        String& message) {
  const EconomicTotals currentTotals = collectEconomicTotals();
  int64_t currentCoinLevelCents = currentCoinLevelCentsFromTotals(currentTotals);
  if (currentCoinLevelCents < 0) currentCoinLevelCents = 0;

  return applyCoinBaseResetAction((uint32_t)currentCoinLevelCents,
                                  successLogLine,
                                  successMessage,
                                  message);
}

static bool onWebEnterProgMode(String& message, void* userData) {
  (void)userData;
  if (g_bootMode != BOOT_MODE_RUN) {
    message = "gia in modalita PROG";
    return false;
  }
  message = "riavvio in modalita PROG (no AP)";
  enterProgrammingModeNoAp();
  return true;
}

static bool onWebResetCounters(String& message, void* userData) {
  (void)userData;
  return applyCurrentCoinLevelAsBase(
    "[WEB] contatori e cassa azzerati, livello monete iniziale aggiornato",
    "contatori e cassa azzerati; livello monete iniziale aggiornato",
    message);
}

static bool onWebSetCoinBase(int64_t coinLevelBaseCents, String& message, void* userData) {
  (void)userData;
  if (coinLevelBaseCents < 0 || coinLevelBaseCents > 0xFFFFFFFFLL) {
    message = "valore monete iniziale non valido";
    return false;
  }

  return applyCoinBaseResetAction(
    (uint32_t)coinLevelBaseCents,
    "[WEB] livello monete iniziale impostato, contatori e cassa azzerati",
    "livello monete iniziale impostato; contatori e cassa azzerati",
    message);
}

static bool onWebSetBillRecyclerBase(int64_t cassette10Count,
                                     int64_t cassette20Count,
                                     int64_t cassette50Count,
                                     String& message,
                                     void* userData) {
  (void)userData;
  if (cassette10Count < 0 || cassette10Count > 0xFFFFLL ||
      cassette20Count < 0 || cassette20Count > 0xFFFFLL ||
      cassette50Count < 0 || cassette50Count > 0xFFFFLL) {
    message = "valori cassette banconote fuori range";
    return false;
  }

  return applyBillRecyclerManualAction((uint16_t)cassette10Count,
                                       (uint16_t)cassette20Count,
                                       (uint16_t)cassette50Count,
                                       "[WEB] valori cassette banconote recycler aggiornati manualmente",
                                       "valori cassette banconote salvati",
                                       message);
}

static bool onWebSaveRemoteSnapshot(String& message, void* userData) {
  (void)userData;
  refreshEconomicStatusForRemoteSave();
  if (!ensureWifiConnectedForWebTest(g_appSettings, message)) {
    return false;
  }
  return g_remoteRegistro.saveChangeEventNow(message);
}

static bool isRecyclerInventoryUpdateTx(const CcTalkTransaction& t) {
  // I modelli BV supportati espongono l'inventario recycler con comandi diversi:
  // - MD100: 0x5E
  // - SMART Payout: 0x1A (Get Note Amount)
  if (!t.hasReq || !t.hasResp) return false;
  if (t.req.dest < 40 || t.req.dest > 50) return false;
  if (t.resp.hdr != 0x00) return false;
  return t.req.hdr == 0x5E || t.req.hdr == 0x1A;
}

static void updateEconomicTotalsAndPrintIfChanged(bool printOnFirst, bool forcePrint, bool emitLog) {
  // Punto centrale di sincronizzazione dei totali economici.
  // Viene richiamato spesso, quindi evita lavoro inutile confrontando lo stato
  // corrente con l'ultimo snapshot gia pubblicato.
  const bool recyclerChanged = refreshRecyclerCacheFromLiveStates();
  const EconomicTotals current = collectEconomicTotals();
  if (!g_economicTotalsValid) {
    g_lastEconomicTotals = current;
    g_economicTotalsValid = true;
    updateEconomicStatus(current);
    if (g_framReady && (!economicTotalsEqual(current, g_persistentBaseTotals) || recyclerChanged)) {
      markFramStateChanged();
    }
    if (emitLog && (printOnFirst || forcePrint)) logEconomicTotalsFromStatus();
    return;
  }

  const bool totalsChanged = !economicTotalsEqual(current, g_lastEconomicTotals);
  if (forcePrint || totalsChanged || recyclerChanged) {
    g_lastEconomicTotals = current;
    updateEconomicStatus(current);
    if (g_framReady && (totalsChanged || recyclerChanged)) markFramStateChanged();
    if (totalsChanged || recyclerChanged) g_remoteRegistro.noteChangeEvent();
    if (emitLog) logEconomicTotalsFromStatus();
  }
}

static bool shouldPrintTransaction(const CcTalkTransaction& t) {
  // Regola di filtro per la modalita 3: si stampa solo cio che aiuta davvero
  // a capire avvio device e cambi di stato significativi.
  if (!t.hasReq) return false;
  if (shouldPrintStartupInfoOnce(t)) return true;
  if (shouldPrintDatasetSpecificByAddress(t.req.dest, t.req.hdr)) return true;
  if (shouldPrintByCounter(t)) return true;

  return false;
}

static bool shouldPrintEconomicModePeripheralInfo(const CcTalkTransaction& t) {
  if (!t.hasReq) return false;
  if (shouldPrintStartupInfoOnce(t)) return true;
  return shouldPrintDatasetSpecificByAddress(t.req.dest, t.req.hdr);
}

static void printTransactionRawOnly(Stream& out, const CcTalkTransaction& t) {
  out.println();
  if (t.hasReq) CcTalkDevice::printRawIf(out, true, t.req, "RAW REQ");
  else out.println(F("RAW REQ: (assente)"));

  if (t.hasResp) CcTalkDevice::printRawIf(out, true, t.resp, "RAW RESP");
  else out.println(F("RAW RESP: (assente)"));
}

// Nomi degli header JCM iPRO-100 / ccTalk BV secondo spec ID-0E3 (pagina 4-5).
// Il JCM echeggia nella risposta lo stesso header della richiesta (non usa HDR=0).
static const __FlashStringHelper* jcmHdrName(uint8_t hdr) {
  switch (hdr) {
    // Core commands
    case 0x01: return F("reset");                  // 1
    case 0x04: return F("req_comms_rev");          // 4
    case 0xC0: return F("req_build_code");         // 192
    case 0xC5: return F("calc_rom_checksum");      // 197
    case 0xF1: return F("req_sw_revision");        // 241
    case 0xF2: return F("req_serial_number");      // 242
    case 0xF4: return F("req_product_code");       // 244
    case 0xF5: return F("req_category_id");        // 245
    case 0xF6: return F("req_manufacturer_id");    // 246
    case 0xF9: return F("req_poll_priority");      // 249
    case 0xFE: return F("simple_poll");            // 254
    // Bill validator commands
    case 0x91: return F("req_currency_rev");       // 145
    case 0x98: return F("req_bill_opmode");        // 152
    case 0x99: return F("mod_bill_opmode");        // 153
    case 0x9A: return F("route_bill");             // 154
    case 0x9B: return F("req_bill_position");      // 155
    case 0x9C: return F("req_scaling_factor");     // 156
    case 0x9D: return F("req_bill_id");            // 157
    case 0x9F: return F("read_bill_events");       // 159
    case 0xAA: return F("req_base_year");          // 170
    case 0xB2: return F("req_bank_select");        // 178
    case 0xB3: return F("mod_bank_select");        // 179
    case 0xC3: return F("req_last_mod_date");      // 195
    case 0xC4: return F("req_creation_date");      // 196
    case 0xD5: return F("req_option_flags");       // 213
    case 0xD8: return F("req_data_storage");       // 216
    case 0xE3: return F("req_master_inhibit");     // 227
    case 0xE4: return F("mod_master_inhibit");     // 228
    case 0xE6: return F("req_inhibit");            // 230
    case 0xE7: return F("mod_inhibit");            // 231
    case 0xF7: return F("req_variable_set");       // 247
    // Recycler commands (iPRO-100-RC, pagina 5 spec)
    case 0x14: return F("mod_recycle_setting");    // 20
    case 0x16: return F("pump_rng");               // 22
    case 0x17: return F("req_cipher_key");         // 23
    case 0x18: return F("req_var_setting");        // 24
    case 0x1A: return F("req_total_count");        // 26
    case 0x1C: return F("dispense_bills");         // 28
    case 0x1D: return F("req_recycler_status");    // 29
    case 0x1E: return F("emergency_stop");         // 30
    case 0x20: return F("mod_recycle_currency");   // 32
    case 0x21: return F("req_recycler_sw_rev");    // 33
    case 0x22: return F("req_recycle_count");      // 34
    case 0x23: return F("mod_recycle_count");      // 35
    case 0x24: return F("req_recycle_current");    // 36
    case 0x34: return F("req_recycle_mode");       // 52
    case 0x35: return F("mod_recycle_mode");       // 53
    case 0x3B: return F("read_recycle_events");    // 59
    default:   return nullptr;
  }
}

// Stampa una transazione con risposta in formato JCM iPRO-100:
//   [ADDR_DEV][LEN][SEQ][HDR_ECHO][DATA...][CHK_JCM][0x01]
// Se la risposta non corrisponde al formato JCM ricade su printTransactionRawOnly.
static void printJcmTransaction(Stream& out, const CcTalkTransaction& t) {
  const bool isJcm = t.hasResp
                     && t.resp.raw
                     && t.resp.rawLen >= 6
                     && t.resp.raw[t.resp.rawLen - 1] == CCTALK_ADDR_MASTER;

  if (!isJcm) {
    printTransactionRawOnly(out, t);
    return;
  }

  const uint8_t* r      = t.resp.raw;
  const uint8_t  rn     = t.resp.rawLen;
  const uint8_t  devAddr = r[0];
  const uint8_t  dataLen = r[1];
  const uint8_t  seq     = r[2];
  const uint8_t  echHdr  = r[3];

  out.println();

  if (t.hasReq) {
    out.print(F("MASTER -> BV["));
    out.print(devAddr);
    out.print(F("]: "));
    const __FlashStringHelper* name = jcmHdrName(echHdr);
    if (name) { out.print(name); out.print(' '); }
    out.print(F("(0x"));
    if (echHdr < 0x10) out.print('0');
    out.print(echHdr, HEX);
    out.println(')');
    CcTalkDevice::printRawIf(out, true, t.req, "RAW REQ");
  }
  // Nessuna richiesta: il JCM e' in modalita auto-push (init: cmd 0xE4 data=0x01).
  // Il BV invia frame spontanei senza essere interrogato dal master.

  out.print(F("BV["));
  out.print(devAddr);
  out.print(F("] PUSH -> MASTER: seq=0x"));
  if (seq < 0x10) out.print('0');
  out.print(seq, HEX);
  out.print(' ');
  const __FlashStringHelper* hdrname = jcmHdrName(echHdr);
  if (hdrname) { out.print(hdrname); out.print(' '); }
  out.print(F("(0x"));
  if (echHdr < 0x10) out.print('0');
  out.print(echHdr, HEX);
  out.print(')');
  if (dataLen == 0) {
    out.println(echHdr == 0x9F ? F(" [no_bill_events]") : F(" [no_data]"));
  } else if (echHdr == 0x9F) {
    // ID-0E3 bank note event codes: 1=EUR5, 2=EUR10, 3=EUR20, 4=EUR50,
    // 5=EUR100, 6=EUR200, 7=EUR500 (spec pagina 3)
    static const uint16_t kBillEur[] = {0, 5, 10, 20, 50, 100, 200, 500};
    bool anyEvent = false;
    for (uint8_t i = 0; i < dataLen && (4 + i) < (rn - 2u); i++) {
      const uint8_t ev = r[4 + i];
      if (ev >= 1 && ev <= 7) {
        out.print(anyEvent ? F(" + EUR") : F(" BILL_ACCEPTED: EUR"));
        out.print(kBillEur[ev]);
        anyEvent = true;
      }
    }
    if (!anyEvent) out.print(F(" [no_bill_events]"));
    out.println();
  } else {
    out.print(F(" data=["));
    for (uint8_t i = 0; i < dataLen && (4 + i) < (rn - 2u); i++) {
      if (i) out.print(' ');
      out.print(F("0x"));
      if (r[4 + i] < 0x10) out.print('0');
      out.print(r[4 + i], HEX);
    }
    out.println(']');
  }
  CcTalkDevice::printRawIf(out, true, t.resp, "RAW RESP");
}

// Riconosce e decodifica il frame "Read buffered bill events" in formato JCM ID-0E3.
// In questa modalita il master usa addr=0x00 (non 0x01 come in ccTalk standard),
// quindi la risposta del BV ha: dest=0x00, src=0x00, HDR=0x00, dati=bill event codes.
// Formato: [0x00][LEN][0x00][0x00][ev0..evN][CHK_JCM] + 1 byte probe CRC16
// Bill event codes (spec pagina 3): 1=EUR5, 2=EUR10, 3=EUR20, 4=EUR50,
//   5=EUR100, 6=EUR200, 7=EUR500.
static bool tryPrintJcmBillEventFrame(Stream& out, const CcTalkTransaction& t) {
  if (!t.hasResp || !t.resp.raw || t.resp.rawLen < 6) return false;
  const uint8_t* r  = t.resp.raw;
  const uint8_t  rn = t.resp.rawLen;
  // Pattern: dest=0x00 (JCM master addr), src=0x00, HDR=0x00 (ACK format)
  if (r[0] != 0x00 || r[2] != 0x00 || r[3] != 0x00) return false;
  // LEN deve essere > 0 e coerente con la dimensione del frame ricevuto
  const uint8_t dataLen = r[1];
  if (dataLen == 0 || (4u + dataLen + 2u) > rn) return false;  // +2: CHK + probe

  static const uint16_t kBillEur[] = {0, 5, 10, 20, 50, 100, 200, 500};
  out.println();
  out.print(F("BV BILL_EVENTS (JCM):"));
  bool anyEvent = false;
  for (uint8_t i = 0; i < dataLen; i++) {
    const uint8_t ev = r[4 + i];
    if (ev >= 1 && ev <= 7) {
      out.print(F(" EUR"));
      out.print(kBillEur[ev]);
      anyEvent = true;
    }
  }
  if (!anyEvent) out.print(F(" [no events]"));
  out.println();
  CcTalkDevice::printRawIf(out, true, t.resp, "RAW RESP");
  return true;
}

static void handleConsoleCommands() {
  // Parser volutamente semplice "carattere per carattere":
  // sufficiente per il monitor seriale e robusto su microcontrollore.
  while (Serial.available()) {
    const char c = (char)Serial.read();
    switch (c) {
      case '1':
        setViewMode(VIEW_ECONOMIC_COUNTERS);
        break;
      case '2':
        setViewMode(VIEW_ALL_RAW);
        break;
      case '3':
        setViewMode(VIEW_INFO_AND_COUNTER_INC);
        break;
      case '4':
        setViewMode(VIEW_ANOMALIES);
        break;
      case 's':
      case 'S':
        printRuntimeState();
        break;
      case 'r':
      case 'R':
        clearRemoteSettingsFromSerial();
        break;
      case 'x':
      case 'X':
        factoryResetSettingsFromSerial();
        break;
      case 'p':
      case 'P':
        if (g_bootMode == BOOT_MODE_RUN) enterProgrammingModeNoAp();
        break;
      case 'h':
      case 'H':
      case '?':
        printConsoleHelp();
        break;
      case '\r':
      case '\n':
      case ' ':
      case '\t':
        break;
      default:
#if ENABLE_SERIAL_LOG
        Serial.print(F("Comando non valido: "));
        Serial.println(c);
        printConsoleHelp();
#endif
        break;
    }
  }
}

// ============================================================================
// Contabilita banconote JCM push-mode
// ============================================================================
// Il BV JCM iPRO-100 opera in auto-push: i frame hanno checksum non standard
// (CRC16-JCM) e vengono emessi con checksumOk=false, bypassando g_router.
// Il segnale di accettazione e route_bill (0x9A) push con data[0]=0x01 (stacker).
// La denominazione viene ricavata dal comando A7 che il master invia all'hopper
// subito dopo: payoutRequestValue (centesimi) = importo della banconota.

static uint8_t  g_jcmBillPendingBvAddr = 0;   // 0 = nessuna banconota pending
static uint32_t g_jcmBillPendingMs     = 0;

// Deve essere chiamata DOPO g_router.route() in modo che payoutRequestValue
// sia gia aggiornato dall'hopper decoder.
static void tryInjectJcmBillCredit(const CcTalkTransaction& t) {
  if (g_jcmBillPendingBvAddr == 0) return;
  if (!t.checksumOk || !t.hasReq || t.req.hdr != 0xA7) return;
  if ((millis() - g_jcmBillPendingMs) > 10000u) {
    g_jcmBillPendingBvAddr = 0;
    return;
  }
  const CcTalkHopper::HopperState* hs = hopperStateForAddress(t.req.dest);
  if (!hs || !hs->payoutRequestValid || hs->payoutRequestValue == 0) return;
  // payoutRequestValue e' gia in centesimi (es. 1000 = EUR10)
  g_jcmBillCreditCents += hs->payoutRequestValue;
  g_jcmBillPendingBvAddr = 0;
}

// ============================================================================
// Callback di sniffing
// ============================================================================
// Queste funzioni vengono invocate dal bus sniffer quando ricostruisce un
// evento significativo dal flusso seriale grezzo.

static void onTx(const CcTalkTransaction& t, uint16_t txCrc, void* user) {
  (void)txCrc; (void)user;
  markCcTalkActivity();
  updateCcTalkBusStatus(t);

  if (isIproCrcBillValidatorTx(t)) {
    autoAssignIproRuntimeAddress(t.req.dest);
  }

  // Rilevamento accettazione banconota JCM: route_bill (0x9A) push con
  // data[0]=0x01 (stacker) indica banconota accettata in auto-push mode.
  // La denominazione verra' ricavata dal successivo comando A7 all'hopper.
  if (!t.checksumOk && t.hasResp && t.resp.raw && t.resp.rawLen >= 7
      && t.resp.raw[t.resp.rawLen - 1] == CCTALK_ADDR_MASTER
      && t.resp.raw[3] == 0x9A
      && t.resp.raw[0] >= 40 && t.resp.raw[0] <= 50
      && t.resp.raw[1] >= 1 && t.resp.raw[4] == 0x01) {
    g_jcmBillPendingBvAddr = t.resp.raw[0];
    g_jcmBillPendingMs     = millis();
  }

  const bool rawOnly = shouldUseRawOnlyOutput(t);
  switch (g_viewMode) {
    case VIEW_ECONOMIC_COUNTERS:
      // La vista 1 resta centrata sui totali, ma mostra una sola volta
      // le info periferica utili per riconoscere i device configurati.
      if (shouldPrintEconomicModePeripheralInfo(t)) {
        if (rawOnly) {
          printTransactionRawOnly(g_runtimeOut, t);
        } else {
          g_router.route(t, g_runtimeOut, false);
          g_runtimeOut.flushPendingLine();
        }
      } else if (!rawOnly) {
        g_router.route(t, g_nullOut, false);
      }
      tryInjectJcmBillCredit(t);
      updateEconomicTotalsAndPrintIfChanged(false, isRecyclerInventoryUpdateTx(t), true);
      return;
    case VIEW_ALL_RAW:
      if (!t.checksumOk || rawOnly) {
        if (!t.checksumOk) {
          g_runtimeOut.print(F("[?] "));
          if (!tryPrintJcmBillEventFrame(g_runtimeOut, t))
            printJcmTransaction(g_runtimeOut, t);
        } else {
          printTransactionRawOnly(g_runtimeOut, t);
        }
      } else {
        g_router.route(t, g_runtimeOut, true);
        g_runtimeOut.flushPendingLine();
      }
      tryInjectJcmBillCredit(t);
      updateEconomicTotalsAndPrintIfChanged(false, isRecyclerInventoryUpdateTx(t), true);
      return;
    case VIEW_INFO_AND_COUNTER_INC:
    default:
      if (shouldPrintTransaction(t)) {
        if (rawOnly) {
          printTransactionRawOnly(g_runtimeOut, t);
        } else {
          g_router.route(t, g_runtimeOut, false);
          g_runtimeOut.flushPendingLine();
        }
      } else if (!rawOnly) {
        // Mantiene aggiornata la cache di stato senza generare rumore in output.
        g_router.route(t, g_nullOut, false);
      }
      tryInjectJcmBillCredit(t);
      // In mode 3 i totali devono restare freschi per la dashboard web,
      // anche se non stiamo stampando nulla su seriale.
      updateEconomicTotalsAndPrintIfChanged(false, false, false);
      return;
    case VIEW_ANOMALIES: {
      // Mostra SOLO frame anomali: checksum errato oppure indirizzo non noto.
      // Tutto il traffico ordinario dei dispositivi configurati viene soppresso.
      const bool badChecksum  = !t.checksumOk;
      const bool unknownAddr  = !isCctalkKnownDeviceAddress(transactionAddress(t));
      if (badChecksum || unknownAddr) {
        if (badChecksum) {
          g_runtimeOut.print(F("[?] "));
          if (!tryPrintJcmBillEventFrame(g_runtimeOut, t))
            printJcmTransaction(g_runtimeOut, t);
        } else {
          printTransactionRawOnly(g_runtimeOut, t);
        }
      } else if (!rawOnly) {
        g_router.route(t, g_nullOut, false);
      }
      tryInjectJcmBillCredit(t);
      return;
    }
  }
}

// Callback per messaggi speciali MDCES a 1 byte.
static void onMdces(uint8_t hostHdr, uint8_t addrByte, void* user) {
  (void)user;
  markCcTalkActivity();
  markDeviceSeen(addrByte);
  if (g_viewMode == VIEW_ECONOMIC_COUNTERS) return;

  if (g_viewMode == VIEW_INFO_AND_COUNTER_INC) {
    if (g_mdcesSeen[addrByte]) return;
    g_mdcesSeen[addrByte] = true;
  }

  logRuntimeLine("");
  if (hostHdr == 0xFD) logRuntimeLine("MASTER: address poll (MDCES)", true);
  else if (hostHdr == 0xFC) logRuntimeLine("MASTER: address clash (MDCES)", true);
  else logRuntimeLine("MASTER: MDCES", true);

  char line[64] = {0};
  snprintf(line, sizeof(line), "DEVICE: address = %u", (unsigned)addrByte);
  logRuntimeLine(line, true);

  if (g_viewMode == VIEW_ALL_RAW) {
    snprintf(line, sizeof(line), "RAW MDCES BYTE: 0x%02X", (unsigned)addrByte);
    logRuntimeLine(line, true);
  }
}

static void initNetworkServices() {
  // Inizializzazione dei servizi visibili all'utente:
  // - SystemStatus alimenta la UI
  // - WifiService gestisce STA/AP
  // - WebServerService espone configurazione e monitoraggio
  g_systemStatus.attachLogOutput(&Serial);
  g_wifi.setLogHook(onWifiLog, nullptr);
  g_web.setUiMode(isProgMode()
                      ? ccms::WebServerService::UI_MODE_PROG
                      : ccms::WebServerService::UI_MODE_STATUS);
  g_web.setActions(onWebResetCounters,
                   onWebSetCoinBase,
                   onWebSetBillRecyclerBase,
                   onWebSaveRemoteSnapshot,
                   nullptr);
  g_web.setSettingsActions(onWebGetSettings,
                           onWebGetPresentPeripheralCatalog,
                           onWebSaveSettings,
                           onWebTestConnection,
                           nullptr);
  g_web.setEnterProgModeAction(onWebEnterProgMode, nullptr);
  g_web.setWifiTestAction(onWebWifiTest, nullptr);

  if (g_bootMode == BOOT_MODE_PROG) {
    g_wifi.beginApOnly();
  } else {
    g_wifi.begin();  // RUN e PROG_NO_AP usano entrambi STA
  }

  g_web.begin();
}

static void initTelemetryAndMeshServices() {
  // Servizi "secondari" rispetto alla raccolta dati locale.
  // In modalita PROG restano disabilitati o in forma ridotta per non
  // interferire con la configurazione iniziale del dispositivo.
  g_cloud.setLogHook(onServiceLog, nullptr);
  g_cloud.setPublishIntervalMs(appconfig::CLOUD_PUBLISH_INTERVAL_MS);
  g_cloud.setRequestTimeoutMs(appconfig::CLOUD_HTTP_TIMEOUT_MS);
  g_remoteRegistro.setLogHook(onServiceLog, nullptr);
  g_remoteRegistro.setRequestApplyHook(onRemoteMasterRequest, nullptr);
  g_remoteRegistro.setRequestTimeoutMs(appconfig::REMOTE_DB_HTTP_TIMEOUT_MS);
  g_remoteRegistro.setRetryIntervalMs(appconfig::REMOTE_DB_RETRY_INTERVAL_MS);
  g_remoteRegistro.setDbPollIntervalMs(appconfig::REMOTE_DB_POLL_INTERVAL_MS);

  g_mesh.setLogHook(onServiceLog, nullptr);
  g_mesh.setHeartbeatIntervalMs(appconfig::MESH_HEARTBEAT_INTERVAL_MS);

  if (isProgMode()) {
    g_cloud.setEnabled(false);
    g_remoteRegistro.setEnabled(false);
    g_mesh.begin(false);
    return;
  }

  g_cloud.setEnabled(appconfig::CLOUD_PUBLISH_ENABLED);
  if (appconfig::CLOUD_PUBLISH_ENABLED) {
    g_cloud.begin();
  }
  g_remoteRegistro.setEnabled(true);
  g_remoteRegistro.begin();
  if (!appconfig::REMOTE_DB_AUTO_POLL_ENABLED) {
    logRuntimeLine("[REMOTE_DB] polling automatico disabilitato", true);
  }
  g_mesh.begin(appconfig::MESH_ENABLED);
}

static void initFramPersistence() {
  // La FRAM contiene il baseline persistente dei contatori economici.
  // Se non e presente o non contiene uno snapshot valido, il sistema continua
  // comunque a funzionare usando solo i dati live della sessione.
  Wire.begin(appconfig::FRAM_I2C_SDA_PIN, appconfig::FRAM_I2C_SCL_PIN);
  g_framReady = g_framStore.begin(Wire, appconfig::FRAM_I2C_ADDR);
  resetFramDirtyTracking();
  g_framSaveErrorLatched = false;
  g_framCongruenceMismatchLatched = false;
  g_framCongruencePendingLatched = false;
  g_lastFramWriteMs = millis();
  g_lastFramCongruenceCheckMs = millis();

  if (!g_framReady) {
    logRuntimeLine("[FRAM] non rilevata, persistenza disabilitata", true);
    return;
  }

  ccms::FramPersistence::Snapshot snapshot;
  if (!g_framStore.load(snapshot)) {
    logRuntimeLine("[FRAM] nessun dato valido trovato, avvio da zero", true);
    return;
  }

  applyPersistentSnapshot(snapshot);
  g_lastEconomicTotals = g_persistentBaseTotals;
  g_economicTotalsValid = true;
  logRuntimeLine("[FRAM] valori economici ripristinati", true);
  logEconomicTotalsFromStatus();
}

static void initCcTalkSniffer() {
  // Il router viene ricreato e popolato a ogni boot per partire da uno stato
  // noto e per legarlo ai modelli device selezionati dalla configurazione.
  g_router = CcTalkRouter();
  g_coin.resetState();
  g_hopperAlbericiDiscriminator.resetState();
  g_hopperAlbericiHopperCd.resetState();
  g_hopperAzkoyenDiscriminator.resetState();
  g_hopperSuzoEvolution.resetState();
  g_billValidatorIpro.resetState();
  g_billValidatorMd100.resetState();
  g_billValidatorSmartPayout.resetState();

  // Router: registrazione dei parser/device handlers.
  g_router.add(&g_coin);
  g_router.add(&g_hopperAlbericiDiscriminator);
  g_router.add(&g_hopperAlbericiHopperCd);
  g_router.add(&g_hopperAzkoyenDiscriminator);
  g_router.add(&g_hopperSuzoEvolution);
  g_router.add(&g_billValidatorIpro);
  g_router.add(&g_billValidatorMd100);
  g_router.add(&g_billValidatorSmartPayout);

  // Configurazione fisica dello sniffer UART ccTalk.
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
  cfg.port = &Serial1;
#else
  cfg.port = &Serial2;
#endif
  cfg.rxPin = appconfig::CCTALK_UART_RX_PIN;
  cfg.txPin = appconfig::CCTALK_UART_TX_PIN;
  cfg.baud = 9600;
  cfg.interByteTimeoutMs = 50;
  cfg.pairWindowMs = 200;
  cfg.mdcesTimeoutMs = 1200;
  cfg.maxFrame = 64;

  // Ricrea lo sniffer con la configurazione finale appena definita.
  g_sniffer = CcTalkBusSniffer(cfg);

  setViewMode(VIEW_INFO_AND_COUNTER_INC);
  g_sniffer.onTransaction(onTx, nullptr);
  g_sniffer.onMdces(onMdces, nullptr);
  g_sniffer.begin();
}

static void serviceSnifferOnce() {
  // Questo ciclo deve essere il piu reattivo possibile: gestisce seriale,
  // sniffing e misura il proprio tempo di esecuzione per telemetria interna.
  handleConsoleCommands();

  const uint32_t startUs = micros();
  const uint32_t gapUs =
      (g_lastSnifferCycleStartUs == 0) ? 0 : (uint32_t)(startUs - g_lastSnifferCycleStartUs);
  g_lastSnifferCycleStartUs = startUs;

  g_sniffer.loop();

  const uint32_t durationUs = (uint32_t)(micros() - startUs);
  g_systemStatus.noteSnifferLoop(durationUs, gapUs, appconfig::SNIFFER_LOOP_BUDGET_US);
}

static void serviceAuxOnce() {
  // Task/loop dei servizi non critici per il timing del bus ccTalk.
  pollProgrammingModeButton();
  g_wifi.loop();
  updateStatusLeds();
  serviceDeferredMeshStartup();
  g_web.loop();
  g_cloud.loop();
  // Il polling DB remoto puo fare HTTP bloccante: lo manteniamo fuori dal
  // fallback single-threaded in cui lo sniffer non ha un task dedicato.
#if defined(ARDUINO_ARCH_ESP32)
  static bool remotePollingSuspendedLogged = false;
  if (g_snifferTaskRunning && appconfig::REMOTE_DB_AUTO_POLL_ENABLED) {
    remotePollingSuspendedLogged = false;
    g_remoteRegistro.loop();
  } else if (!remotePollingSuspendedLogged && appconfig::REMOTE_DB_AUTO_POLL_ENABLED) {
    logRuntimeLine("[REMOTE_DB] polling automatico sospeso: task sniffer non attivo", true);
    remotePollingSuspendedLogged = true;
  }
#else
  if (appconfig::REMOTE_DB_AUTO_POLL_ENABLED) g_remoteRegistro.loop();
#endif
  g_mesh.loop();

  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastPersistServiceMs) >= appconfig::PERSIST_LOOP_INTERVAL_MS) {
    g_lastPersistServiceMs = now;
    flushFramIfDue();
    serviceFramCongruenceCheck();
  }
}

static void serviceDeferredMeshStartup() {
  if (isProgMode()) return;
  if (!appconfig::MESH_ENABLED) return;
  if (g_mesh.ready()) return;

  const uint32_t now = millis();

  if (!g_wifi.isConnected() && !g_wifi.isApFallbackActive()) {
    g_wifiStableSinceMs = 0;
    return;
  }

  if (g_wifiStableSinceMs == 0) {
    g_wifiStableSinceMs = now;
    g_meshStartupDeferredLogged = false;
    return;
  }

  const uint32_t stableForMs = (uint32_t)(now - g_wifiStableSinceMs);
  if (stableForMs < appconfig::MESH_START_DELAY_MS) {
    if (!g_meshStartupDeferredLogged) {
      logRuntimeLine("[MESH] avvio rinviato finche WiFi/AP non restano stabili", true);
      g_meshStartupDeferredLogged = true;
    }
    return;
  }

  if (g_lastMeshStartAttemptMs != 0 &&
      (uint32_t)(now - g_lastMeshStartAttemptMs) < appconfig::MESH_RETRY_INTERVAL_MS) {
    return;
  }

  g_lastMeshStartAttemptMs = now;
  if (!g_mesh.begin(true)) {
    logRuntimeLine("[MESH] avvio rimandato: init fallita, nuovo tentativo piu tardi", true);
  }
}

static void serviceAuxIfDue() {
  const uint32_t now = millis();
  if (g_lastAuxServiceMs != 0 &&
      (uint32_t)(now - g_lastAuxServiceMs) < appconfig::NET_SERVICE_LOOP_INTERVAL_MS) {
    return;
  }

  g_lastAuxServiceMs = now;
  serviceAuxOnce();
}

#if defined(ARDUINO_ARCH_ESP32)
static void runSnifferTask(void* userData) {
  (void)userData;
  uint32_t lastCoopYieldUs = micros();

  for (;;) {
    serviceSnifferOnce();

    // Yield cooperativo: lascia spazio al scheduler senza introdurre ritardi
    // ad ogni singola iterazione del loop sniffer.
    const uint32_t nowUs = micros();
    if ((uint32_t)(nowUs - lastCoopYieldUs) >= appconfig::SNIFFER_COOP_YIELD_INTERVAL_US) {
      vTaskDelay(1);
      lastCoopYieldUs = micros();
    }
  }
}

static void runAuxTask(void* userData) {
  (void)userData;
  for (;;) {
    serviceAuxOnce();
    vTaskDelay(pdMS_TO_TICKS(appconfig::NET_SERVICE_LOOP_INTERVAL_MS));
  }
}

static void startRuntimeTasks() {
  // Su ESP32 proviamo a separare il lavoro "time sensitive" del bus da quello
  // dei servizi di rete. Se la creazione fallisce, il loop principale fara da
  // fallback single-threaded.
  if (g_snifferTaskRunning || g_auxTaskRunning) return;

  BaseType_t snifferCore = tskNO_AFFINITY;
  BaseType_t auxCore = tskNO_AFFINITY;
#if defined(CONFIG_FREERTOS_UNICORE) && (CONFIG_FREERTOS_UNICORE == 0)
  snifferCore = 1;
  auxCore = 0;
#endif

  if (xTaskCreatePinnedToCore(runSnifferTask,
                              "ccms-sniffer",
                              appconfig::TASK_STACK_BYTES_SNIFFER,
                              nullptr,
                              (UBaseType_t)appconfig::TASK_PRIORITY_SNIFFER,
                              &g_snifferTaskHandle,
                              snifferCore) == pdPASS) {
    g_snifferTaskRunning = true;
  } else {
    logRuntimeLine("[RT] errore creazione task sniffer", true);
  }

  if (xTaskCreatePinnedToCore(runAuxTask,
                              "ccms-aux",
                              appconfig::TASK_STACK_BYTES_NET,
                              nullptr,
                              (UBaseType_t)appconfig::TASK_PRIORITY_NET,
                              &g_auxTaskHandle,
                              auxCore) == pdPASS) {
    g_auxTaskRunning = true;
  } else {
    logRuntimeLine("[RT] errore creazione task servizi", true);
  }

  if (g_snifferTaskRunning || g_auxTaskRunning) {
    logRuntimeLine("[RT] scheduler Fase 1 attivo", true);
  }
}
#endif

// ============================================================================
// Bootstrap applicativo
// ============================================================================

static void printStartupBanner() {
#if ENABLE_SERIAL_LOG
#if CONFIG_IDF_TARGET_ESP32C6
  Serial.println(F("ccTalk MULTI sniffer ESP32-C6 RX-only (Serial1)"));
#elif CONFIG_IDF_TARGET_ESP32C3
  Serial.println(F("ccTalk MULTI sniffer ESP32-C3 RX-only (Serial1)"));
#else
  Serial.println(F("ccTalk MULTI sniffer ESP32 RX-only (Serial2)"));
#endif
  Serial.println(F("Gestione: GETTONIERA addr=2, HOPPER addr=3..10, BV addr=40..50"));
  Serial.println(F("Modelli Hopper: configurazione per indirizzo da modalita PROG"));
  Serial.println(F("Modelli Bill Validator: configurazione per indirizzo da modalita PROG"));
  if (g_bootMode == BOOT_MODE_PROG) {
    Serial.println(F("Boot mode: PROG (AP attivo)"));
    Serial.print(F("Accesso PROG: GPIO"));
    Serial.print(appconfig::PROG_MODE_BUTTON_PIN);
    Serial.println(F(" LOW al boot o pulsante premuto in RUN"));
    Serial.println(F("Apri http://192.168.4.1 per Impostazioni/Stato"));
  } else if (g_bootMode == BOOT_MODE_PROG_NO_AP) {
    Serial.println(F("Boot mode: PROG (WiFi STA, no AP)"));
    Serial.println(F("Apri http://<IP_dispositivo> per Impostazioni/Stato"));
  } else {
    Serial.println(F("Boot mode: RUN"));
  }
#endif
}

static void initializeApplication() {
  // Sequenza di bootstrap in ordine logico:
  // 1. rileva il contesto di boot
  // 2. carica la configurazione persistente
  // 3. attiva servizi utente/rete
  // 4. ripristina eventuale baseline economica da FRAM
  // 5. avvia lo sniffer ccTalk
  initLevelShifterEnable();
  initStatusLeds();
  detectBootMode();
  loadAppSettings();
  initNetworkServices();
  initTelemetryAndMeshServices();
  initFramPersistence();
  initCcTalkSniffer();
}

void setup() {
  Serial.begin(LOG_BAUD);
#if defined(ARDUINO_ARCH_ESP32)
  // NetworkClient su ESP32-C6 / pioarduino chiama setSocketOption() anche
  // quando il socket non e ancora aperto (fd=0), generando un falso [E].
  // Il comportamento funzionale e corretto; silenzio solo questo tag.
  esp_log_level_set("NetworkClient", ESP_LOG_NONE);
#endif
  initializeApplication();
  printStartupBanner();
  printConsoleHelp();

  if (RUN_BV_MANUAL_TESTS) runBillValidatorManualTests();

#if defined(ARDUINO_ARCH_ESP32)
  startRuntimeTasks();
#endif
}

void loop() {
#if defined(ARDUINO_ARCH_ESP32)
  // Fallback: se uno dei task FreeRTOS non parte, il loop principale evita che
  // la relativa funzione rimanga completamente ferma.
  if (!g_snifferTaskRunning) serviceSnifferOnce();
  if (!g_auxTaskRunning) serviceAuxIfDue();
  vTaskDelay(1);
#else
  serviceSnifferOnce();
  serviceAuxIfDue();
#endif
}
