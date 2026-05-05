// Scopo del file:
// implementa la versione legacy di `SystemStatus`.
#include "CcTalkSystemStatus.h"

#include <stdio.h>
#include <string.h>

SystemStatus::SystemStatus() {
  reset();
}

void SystemStatus::reset() {
  // Versione compatta: nessuna sincronizzazione, tutto gira in un singolo contesto.
  _economic = EconomicFields();
  clearRecyclerEntries();
  clearEvents();
}

void SystemStatus::attachSerial(Stream* serialOut) {
  _serial = serialOut;
}

void SystemStatus::updateEconomicTotals(uint32_t cntotBanconoteInCents,
                                        uint32_t cntotMoneteOutCents,
                                        uint32_t cntotMoneteInCents,
                                        uint32_t cntotBanconoteOutCents,
                                        uint32_t cassaCents,
                                        uint32_t recyclerInventoryTotaleCents) {
  // Aggiorna sia i campi base sia quelli derivati (netti e saldo).
  _economic.cntotBanconoteInCents = cntotBanconoteInCents;
  _economic.cntotMoneteOutCents = cntotMoneteOutCents;
  _economic.cntotMoneteInCents = cntotMoneteInCents;
  _economic.cntotBanconoteOutCents = cntotBanconoteOutCents;
  _economic.cntotBanconoteCents = (int64_t)cntotBanconoteInCents - (int64_t)cntotBanconoteOutCents;
  _economic.cntotMoneteCents = (int64_t)cntotMoneteInCents - (int64_t)cntotMoneteOutCents;
  _economic.saldoCents = _economic.cntotBanconoteCents + _economic.cntotMoneteCents;
  _economic.cassaCents = cassaCents;
  _economic.recyclerInventoryTotaleCents = recyclerInventoryTotaleCents;
}

void SystemStatus::clearRecyclerEntries() {
  memset(_recyclerEntries, 0, sizeof(_recyclerEntries));
  _recyclerEntryCount = 0;
}

bool SystemStatus::addRecyclerEntry(uint8_t addr, uint16_t c10, uint16_t c20, uint16_t c50, uint32_t totalCents) {
  // Array a dimensione fissa: in overflow restituisce semplicemente false.
  if (_recyclerEntryCount >= kMaxRecyclerEntries) return false;

  RecyclerInventoryEntry& e = _recyclerEntries[_recyclerEntryCount++];
  e.valid = true;
  e.addr = addr;
  e.count10 = c10;
  e.count20 = c20;
  e.count50 = c50;
  e.totalCents = totalCents;
  return true;
}

void SystemStatus::formatUnsignedEuro(uint32_t cents, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  const uint32_t eur = cents / 100UL;
  const uint32_t cent = cents % 100UL;
  snprintf(out, outLen, "%lu.%02lu EUR", (unsigned long)eur, (unsigned long)cent);
}

void SystemStatus::formatSignedEuro(int64_t cents, char* out, size_t outLen) {
  if (!out || outLen == 0) return;

  bool negative = false;
  uint64_t absCents = 0;
  if (cents < 0) {
    negative = true;
    absCents = (uint64_t)(-cents);
  } else {
    absCents = (uint64_t)cents;
  }

  const unsigned long eur = (unsigned long)(absCents / 100ULL);
  const unsigned long cent = (unsigned long)(absCents % 100ULL);
  snprintf(out, outLen, "%s%lu.%02lu EUR", negative ? "-" : "", eur, cent);
}

void SystemStatus::formatEconomicLine1(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;

  char bIn[24] = {0};
  char mOut[24] = {0};
  char mIn[24] = {0};
  char bOut[24] = {0};
  formatUnsignedEuro(_economic.cntotBanconoteInCents, bIn, sizeof(bIn));
  formatUnsignedEuro(_economic.cntotMoneteOutCents, mOut, sizeof(mOut));
  formatUnsignedEuro(_economic.cntotMoneteInCents, mIn, sizeof(mIn));
  formatUnsignedEuro(_economic.cntotBanconoteOutCents, bOut, sizeof(bOut));

  snprintf(out, outLen,
           "CntotBanconoteIN=%s | CntotMoneteOut=%s | CntotMoneteIn=%s | CntotBanconoteOUT=%s",
           bIn, mOut, mIn, bOut);
}

void SystemStatus::formatEconomicLine2(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;

  char banconote[24] = {0};
  char monete[24] = {0};
  char saldo[24] = {0};
  formatSignedEuro(_economic.cntotBanconoteCents, banconote, sizeof(banconote));
  formatSignedEuro(_economic.cntotMoneteCents, monete, sizeof(monete));
  formatSignedEuro(_economic.saldoCents, saldo, sizeof(saldo));

  snprintf(out, outLen,
           "CntotBanconote=%s | CntotMonete=%s | Saldo=%s",
           banconote, monete, saldo);
}

void SystemStatus::formatEconomicLine3(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;

  char cassa[24] = {0};
  char recycler[24] = {0};
  formatUnsignedEuro(_economic.cassaCents, cassa, sizeof(cassa));
  formatUnsignedEuro(_economic.recyclerInventoryTotaleCents, recycler, sizeof(recycler));

  snprintf(out, outLen,
           "Cassa R=%s | RecyclerInventoryTotale=%s",
           cassa, recycler);
}

bool SystemStatus::formatRecyclerLine(uint8_t entryIndex, char* out, size_t outLen) const {
  if (!out || outLen == 0) return false;
  if (entryIndex >= _recyclerEntryCount) return false;

  const RecyclerInventoryEntry& e = _recyclerEntries[entryIndex];
  if (!e.valid) return false;

  snprintf(out, outLen,
           "BV[%u] RecyclerInventory: 10EUR=%u 20EUR=%u 50EUR=%u",
           (unsigned)e.addr,
           (unsigned)e.count10,
           (unsigned)e.count20,
           (unsigned)e.count50);
  return true;
}

void SystemStatus::logLine(const char* line, bool printToSerial) {
  // Ring buffer manuale degli eventi, mantenuto in ordine cronologico relativo.
  const char* safe = line ? line : "";

  strncpy(_events[_eventWriteIndex], safe, (size_t)(kMaxEventLen - 1));
  _events[_eventWriteIndex][kMaxEventLen - 1] = '\0';

  _eventWriteIndex = (uint8_t)((_eventWriteIndex + 1) % kMaxEvents);
  if (_eventCount < kMaxEvents) _eventCount++;

  if (printToSerial && _serial) {
    _serial->println(safe);
  }
}

const char* SystemStatus::eventAt(uint8_t oldestIndex) const {
  if (oldestIndex >= _eventCount) return nullptr;

  const uint8_t oldestPos = (uint8_t)((_eventWriteIndex + kMaxEvents - _eventCount) % kMaxEvents);
  const uint8_t idx = (uint8_t)((oldestPos + oldestIndex) % kMaxEvents);
  return _events[idx];
}

void SystemStatus::clearEvents() {
  memset(_events, 0, sizeof(_events));
  _eventWriteIndex = 0;
  _eventCount = 0;
}
