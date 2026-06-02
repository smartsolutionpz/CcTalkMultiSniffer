// Scopo del file:
// dichiara `SystemStatus`, la classe centrale che espone stato economico,
// telemetria ccTalk, inventario recycler e log recenti.
#ifndef CCTALK_MULTI_SNIFFER_STATUS_SYSTEM_STATUS_H
#define CCTALK_MULTI_SNIFFER_STATUS_SYSTEM_STATUS_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "RingLog.h"

namespace ccms {

// Stato applicativo condiviso tra sniffer, servizi di rete, persistenza e UI.
// Funziona come "single source of truth" per tutto cio che deve essere
// pubblicato all'esterno del parser ccTalk.
class SystemStatus {
public:
  static const uint8_t kMaxRecyclerEntries = 11; // BV addr 40..50
  static const uint16_t kFormattedLineSize = 192;

  // Campi economici gia normalizzati e pronti per la pubblicazione.
  struct EconomicFields {
    uint32_t cntotBanconoteInCents = 0;
    uint32_t cntotMoneteOutCents = 0;
    uint32_t cntotMoneteInCents = 0;
    uint32_t cntotBanconoteOutCents = 0;
    int64_t cntotBanconoteCents = 0;
    int64_t cntotMoneteCents = 0;
    int64_t saldoCents = 0;
    uint32_t cassaCents = 0;
    uint32_t recyclerInventoryTotaleCents = 0;
    uint32_t coinLevelBaseCents = 0;
    int64_t coinCurrentCents = 0;
  };

  struct RecyclerInventoryEntry {
    bool valid = false;
    uint8_t addr = 0;
    uint16_t count10 = 0;
    uint16_t count20 = 0;
    uint16_t count50 = 0;
    uint32_t totalCents = 0;
  };

  // Telemetria sintetica del bus/sniffer.
  struct CcTalkFields {
    uint16_t detectedDevices = 0;
    uint32_t txFrames = 0;
    uint32_t rxFrames = 0;
    uint32_t transactions = 0;
    uint32_t snifferLoops = 0;
    uint32_t snifferLoopLastUs = 0;
    uint32_t snifferLoopMaxUs = 0;
    uint32_t snifferLoopGapMaxUs = 0;
    uint32_t snifferOverBudgetLoops = 0;
    char lastTxFrame[96] = {0};
    char lastRxFrame[96] = {0};
    char lastEventDecoded[RingLog::kLineSize] = {0};
  };

  SystemStatus();

  void reset();
  // Collega uno stream opzionale per il mirroring dei log.
  void attachLogOutput(Stream* out);

  // Aggiorna in un colpo solo tutti i campi economici derivati.
  void updateEconomicTotals(uint32_t cntotBanconoteInCents,
                            uint32_t cntotMoneteOutCents,
                            uint32_t cntotMoneteInCents,
                            uint32_t cntotBanconoteOutCents,
                            uint32_t cassaCents,
                            uint32_t recyclerInventoryTotaleCents,
                            uint32_t coinLevelBaseCents);
  void replaceEconomicAndRecycler(uint32_t cntotBanconoteInCents,
                                  uint32_t cntotMoneteOutCents,
                                  uint32_t cntotMoneteInCents,
                                  uint32_t cntotBanconoteOutCents,
                                  uint32_t cassaCents,
                                  uint32_t recyclerInventoryTotaleCents,
                                  uint32_t coinLevelBaseCents,
                                  const RecyclerInventoryEntry* entries,
                                  uint8_t count);
  void snapshotForPersistence(EconomicFields& economic,
                              RecyclerInventoryEntry* recycler,
                              uint8_t& count,
                              uint8_t maxCount) const;

  EconomicFields economicCopy() const;
  CcTalkFields cctalkCopy() const;

  // Gestione inventario recycler pubblicato alla UI.
  void clearRecyclerEntries();
  bool addRecyclerEntry(uint8_t addr, uint16_t c10, uint16_t c20, uint16_t c50, uint32_t totalCents);
  uint8_t recyclerEntryCount() const;
  bool recyclerEntryAt(uint8_t index, RecyclerInventoryEntry& out) const;

  void setDetectedDevices(uint16_t count);
  void noteTransaction();
  void noteTxFrame(const char* frameText);
  void noteRxFrame(const char* frameText);
  void noteDecodedEvent(const char* eventLine);
  void noteSnifferLoop(uint32_t durationUs, uint32_t gapUs, uint32_t budgetUs);

  // Formatter testuali condivisi da seriale, web e persistenza.
  void formatEconomicLine1(char* out, size_t outLen) const;
  void formatEconomicLine2(char* out, size_t outLen) const;
  void formatEconomicLine3(char* out, size_t outLen) const;
  bool formatRecyclerLine(uint8_t entryIndex, char* out, size_t outLen) const;

  void logLine(const char* line, bool printToSerial = true);
  uint8_t logCount() const;
  bool logLineAt(uint8_t oldestIndex, char* out, size_t outLen) const;
  void clearLogs();

  void setMqttConnected(bool connected);
  bool mqttConnected() const;

private:
  // Helper di formattazione monetaria.
  static void formatUnsignedEuro(uint32_t cents, char* out, size_t outLen);
  static void formatSignedEuro(int64_t cents, char* out, size_t outLen);

#if defined(ARDUINO_ARCH_ESP32)
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
#endif

  Stream* _logOutput = nullptr;
  EconomicFields _economic;
  CcTalkFields _cctalk;
  bool _mqttConnected = false;
  RecyclerInventoryEntry _recycler[kMaxRecyclerEntries];
  uint8_t _recyclerCount = 0;
  RingLog _ringLog;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_STATUS_SYSTEM_STATUS_H
