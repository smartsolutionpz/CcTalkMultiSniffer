// Scopo del file:
// dichiara la versione legacy di `SystemStatus`, usata come stato economico
// e log compatto nelle parti piu vecchie del progetto.
#pragma once

#include <Arduino.h>
#include <stdint.h>

// Versione legacy/autonoma di uno stato economico + log eventi.
// Convive con `status/SystemStatus` per compatibilita con parti piu vecchie
// del progetto e come riferimento didattico di una versione piu semplice.
class SystemStatus {
public:
  static const uint8_t kMaxRecyclerEntries = 11; // BV addr 40..50
  static const uint8_t kMaxEvents = 64;
  static const uint16_t kMaxEventLen = 192;

  // Sottoinsieme economico pubblicato dalla versione legacy.
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
  };

  struct RecyclerInventoryEntry {
    bool valid = false;
    uint8_t addr = 0;
    uint16_t count10 = 0;
    uint16_t count20 = 0;
    uint16_t count50 = 0;
    uint32_t totalCents = 0;
  };

  SystemStatus();

  void reset();
  // Collega uno stream seriale opzionale usato da `logLine()`.
  void attachSerial(Stream* serialOut);

  void updateEconomicTotals(uint32_t cntotBanconoteInCents,
                            uint32_t cntotMoneteOutCents,
                            uint32_t cntotMoneteInCents,
                            uint32_t cntotBanconoteOutCents,
                            uint32_t cassaCents,
                            uint32_t recyclerInventoryTotaleCents);

  void clearRecyclerEntries();
  bool addRecyclerEntry(uint8_t addr, uint16_t c10, uint16_t c20, uint16_t c50, uint32_t totalCents);

  const EconomicFields& economic() const { return _economic; }
  const RecyclerInventoryEntry* recyclerEntries() const { return _recyclerEntries; }
  uint8_t recyclerEntryCount() const { return _recyclerEntryCount; }

  void formatEconomicLine1(char* out, size_t outLen) const;
  void formatEconomicLine2(char* out, size_t outLen) const;
  void formatEconomicLine3(char* out, size_t outLen) const;
  bool formatRecyclerLine(uint8_t entryIndex, char* out, size_t outLen) const;

  void logLine(const char* line, bool printToSerial = true);
  uint8_t eventCount() const { return _eventCount; }
  const char* eventAt(uint8_t oldestIndex) const;
  void clearEvents();

private:
  // Helper condivisi di formattazione monetaria.
  static void formatUnsignedEuro(uint32_t cents, char* out, size_t outLen);
  static void formatSignedEuro(int64_t cents, char* out, size_t outLen);

  Stream* _serial = nullptr;
  EconomicFields _economic;
  RecyclerInventoryEntry _recyclerEntries[kMaxRecyclerEntries];
  uint8_t _recyclerEntryCount = 0;

  char _events[kMaxEvents][kMaxEventLen];
  uint8_t _eventWriteIndex = 0;
  uint8_t _eventCount = 0;
};
