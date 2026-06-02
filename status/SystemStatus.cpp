// Scopo del file:
// implementa `SystemStatus`, la sorgente unica di verita per UI, cloud e log.
#include "SystemStatus.h"

#include <stdio.h>
#include <string.h>

namespace ccms {

namespace {
#if defined(ARDUINO_ARCH_ESP32)
// Lock locale per proteggere le strutture condivise tra task sniffer e task servizi.
inline void lock(portMUX_TYPE& mux) { portENTER_CRITICAL(&mux); }
inline void unlock(portMUX_TYPE& mux) { portEXIT_CRITICAL(&mux); }
#endif
} // namespace

SystemStatus::SystemStatus() {
  reset();
}

void SystemStatus::reset() {
  // Riporta tutte le viste pubblicate a uno stato iniziale coerente.
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _economic = EconomicFields();
  _cctalk = CcTalkFields();
  memset(_recycler, 0, sizeof(_recycler));
  _recyclerCount = 0;
  _ringLog.clear();
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::attachLogOutput(Stream* out) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _logOutput = out;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::updateEconomicTotals(uint32_t cntotBanconoteInCents,
                                        uint32_t cntotMoneteOutCents,
                                        uint32_t cntotMoneteInCents,
                                        uint32_t cntotBanconoteOutCents,
                                        uint32_t cassaCents,
                                        uint32_t recyclerInventoryTotaleCents,
                                        uint32_t coinLevelBaseCents) {
  // Oltre ai campi base, qui vengono ricalcolati tutti i valori derivati:
  // totali netti, saldo complessivo e livello monete corrente.
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _economic.cntotBanconoteInCents = cntotBanconoteInCents;
  _economic.cntotMoneteOutCents = cntotMoneteOutCents;
  _economic.cntotMoneteInCents = cntotMoneteInCents;
  _economic.cntotBanconoteOutCents = cntotBanconoteOutCents;
  _economic.cntotBanconoteCents = (int64_t)cntotBanconoteInCents - (int64_t)cntotBanconoteOutCents;
  _economic.cntotMoneteCents = (int64_t)cntotMoneteInCents - (int64_t)cntotMoneteOutCents;
  _economic.saldoCents = _economic.cntotBanconoteCents + _economic.cntotMoneteCents;
  _economic.cassaCents = cassaCents;
  _economic.recyclerInventoryTotaleCents = recyclerInventoryTotaleCents;
  _economic.coinLevelBaseCents = coinLevelBaseCents;
  _economic.coinCurrentCents = (int64_t)coinLevelBaseCents + _economic.cntotMoneteCents;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::replaceEconomicAndRecycler(uint32_t cntotBanconoteInCents,
                                              uint32_t cntotMoneteOutCents,
                                              uint32_t cntotMoneteInCents,
                                              uint32_t cntotBanconoteOutCents,
                                              uint32_t cassaCents,
                                              uint32_t recyclerInventoryTotaleCents,
                                              uint32_t coinLevelBaseCents,
                                              const RecyclerInventoryEntry* entries,
                                              uint8_t count) {
  if (count > kMaxRecyclerEntries) count = kMaxRecyclerEntries;

#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _economic.cntotBanconoteInCents = cntotBanconoteInCents;
  _economic.cntotMoneteOutCents = cntotMoneteOutCents;
  _economic.cntotMoneteInCents = cntotMoneteInCents;
  _economic.cntotBanconoteOutCents = cntotBanconoteOutCents;
  _economic.cntotBanconoteCents = (int64_t)cntotBanconoteInCents - (int64_t)cntotBanconoteOutCents;
  _economic.cntotMoneteCents = (int64_t)cntotMoneteInCents - (int64_t)cntotMoneteOutCents;
  _economic.saldoCents = _economic.cntotBanconoteCents + _economic.cntotMoneteCents;
  _economic.cassaCents = cassaCents;
  _economic.recyclerInventoryTotaleCents = recyclerInventoryTotaleCents;
  _economic.coinLevelBaseCents = coinLevelBaseCents;
  _economic.coinCurrentCents = (int64_t)coinLevelBaseCents + _economic.cntotMoneteCents;

  memset(_recycler, 0, sizeof(_recycler));
  _recyclerCount = 0;
  if (entries) {
    for (uint8_t i = 0; i < count; i++) {
      if (!entries[i].valid) continue;
      _recycler[_recyclerCount++] = entries[i];
      _recycler[_recyclerCount - 1].valid = true;
    }
  }
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::snapshotForPersistence(EconomicFields& economic,
                                          RecyclerInventoryEntry* recycler,
                                          uint8_t& count,
                                          uint8_t maxCount) const {
  count = 0;

#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  economic = _economic;
  if (recycler && maxCount > 0) {
    count = _recyclerCount;
    if (count > maxCount) count = maxCount;
    for (uint8_t i = 0; i < count; i++) {
      recycler[i] = _recycler[i];
    }
  }
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

SystemStatus::EconomicFields SystemStatus::economicCopy() const {
  EconomicFields out;
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  out = _economic;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return out;
}

SystemStatus::CcTalkFields SystemStatus::cctalkCopy() const {
  CcTalkFields out;
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  out = _cctalk;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return out;
}

void SystemStatus::clearRecyclerEntries() {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  memset(_recycler, 0, sizeof(_recycler));
  _recyclerCount = 0;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

bool SystemStatus::addRecyclerEntry(uint8_t addr, uint16_t c10, uint16_t c20, uint16_t c50, uint32_t totalCents) {
  // L'array ha dimensione fissa; in caso di overflow si preferisce rifiutare
  // l'inserimento piuttosto che corrompere memoria.
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  if (_recyclerCount >= kMaxRecyclerEntries) {
#if defined(ARDUINO_ARCH_ESP32)
    unlock(_mux);
#endif
    return false;
  }

  RecyclerInventoryEntry& e = _recycler[_recyclerCount++];
  e.valid = true;
  e.addr = addr;
  e.count10 = c10;
  e.count20 = c20;
  e.count50 = c50;
  e.totalCents = totalCents;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return true;
}

uint8_t SystemStatus::recyclerEntryCount() const {
  uint8_t out = 0;
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  out = _recyclerCount;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return out;
}

bool SystemStatus::recyclerEntryAt(uint8_t index, RecyclerInventoryEntry& out) const {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  if (index >= _recyclerCount || !_recycler[index].valid) {
#if defined(ARDUINO_ARCH_ESP32)
    unlock(_mux);
#endif
    return false;
  }
  out = _recycler[index];
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return true;
}

void SystemStatus::setDetectedDevices(uint16_t count) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _cctalk.detectedDevices = count;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::noteTransaction() {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _cctalk.transactions++;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::noteTxFrame(const char* frameText) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _cctalk.txFrames++;
  strncpy(_cctalk.lastTxFrame, frameText ? frameText : "", sizeof(_cctalk.lastTxFrame) - 1);
  _cctalk.lastTxFrame[sizeof(_cctalk.lastTxFrame) - 1] = '\0';
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::noteRxFrame(const char* frameText) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _cctalk.rxFrames++;
  strncpy(_cctalk.lastRxFrame, frameText ? frameText : "", sizeof(_cctalk.lastRxFrame) - 1);
  _cctalk.lastRxFrame[sizeof(_cctalk.lastRxFrame) - 1] = '\0';
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::noteDecodedEvent(const char* eventLine) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  strncpy(_cctalk.lastEventDecoded, eventLine ? eventLine : "", sizeof(_cctalk.lastEventDecoded) - 1);
  _cctalk.lastEventDecoded[sizeof(_cctalk.lastEventDecoded) - 1] = '\0';
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::noteSnifferLoop(uint32_t durationUs, uint32_t gapUs, uint32_t budgetUs) {
  // Questa metrica permette di capire se il loop di sniffing resta entro il
  // budget temporale previsto, aspetto critico su bus seriale.
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _cctalk.snifferLoops++;
  _cctalk.snifferLoopLastUs = durationUs;
  if (durationUs > _cctalk.snifferLoopMaxUs) _cctalk.snifferLoopMaxUs = durationUs;
  if (gapUs > _cctalk.snifferLoopGapMaxUs) _cctalk.snifferLoopGapMaxUs = gapUs;
  if (budgetUs > 0 && durationUs > budgetUs) _cctalk.snifferOverBudgetLoops++;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::formatUnsignedEuro(uint32_t cents, char* out, size_t outLen) {
  // La formattazione monetaria e centralizzata per evitare incoerenze tra UI e log.
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

  EconomicFields econ = economicCopy();
  char bIn[24] = {0};
  char mOut[24] = {0};
  char mIn[24] = {0};
  char bOut[24] = {0};

  formatUnsignedEuro(econ.cntotBanconoteInCents, bIn, sizeof(bIn));
  formatUnsignedEuro(econ.cntotMoneteOutCents, mOut, sizeof(mOut));
  formatUnsignedEuro(econ.cntotMoneteInCents, mIn, sizeof(mIn));
  formatUnsignedEuro(econ.cntotBanconoteOutCents, bOut, sizeof(bOut));

  snprintf(out, outLen,
           "CntotBanconoteIN=%s | CntotMoneteOut=%s | CntotMoneteIn=%s | CntotBanconoteOUT=%s",
           bIn, mOut, mIn, bOut);
}

void SystemStatus::formatEconomicLine2(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;

  EconomicFields econ = economicCopy();
  char banconote[24] = {0};
  char monete[24] = {0};
  char saldo[24] = {0};

  formatSignedEuro(econ.cntotBanconoteCents, banconote, sizeof(banconote));
  formatSignedEuro(econ.cntotMoneteCents, monete, sizeof(monete));
  formatSignedEuro(econ.saldoCents, saldo, sizeof(saldo));

  snprintf(out, outLen,
           "CntotBanconote=%s | CntotMonete=%s | Saldo=%s",
           banconote, monete, saldo);
}

void SystemStatus::formatEconomicLine3(char* out, size_t outLen) const {
  if (!out || outLen == 0) return;

  EconomicFields econ = economicCopy();
  char cassa[24] = {0};
  char recycler[24] = {0};
  formatUnsignedEuro(econ.cassaCents, cassa, sizeof(cassa));
  formatUnsignedEuro(econ.recyclerInventoryTotaleCents, recycler, sizeof(recycler));

  snprintf(out, outLen,
           "Cassa R=%s | RecyclerInventoryTotale=%s",
           cassa, recycler);
}

bool SystemStatus::formatRecyclerLine(uint8_t entryIndex, char* out, size_t outLen) const {
  if (!out || outLen == 0) return false;

  RecyclerInventoryEntry entry;
  if (!recyclerEntryAt(entryIndex, entry)) return false;

  snprintf(out, outLen,
           "BV[%u] RecyclerInventory: 10EUR=%u 20EUR=%u 50EUR=%u",
           (unsigned)entry.addr,
           (unsigned)entry.count10,
           (unsigned)entry.count20,
           (unsigned)entry.count50);
  return true;
}

void SystemStatus::logLine(const char* line, bool printToSerial) {
  Stream* out = nullptr;

#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  // Il ring log viene aggiornato sempre, anche se la stampa seriale e disabilitata.
  _ringLog.push(line);
  out = _logOutput;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif

  if (printToSerial && out) out->println(line ? line : "");
}

uint8_t SystemStatus::logCount() const {
  uint8_t out = 0;
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  out = _ringLog.count();
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return out;
}

bool SystemStatus::logLineAt(uint8_t oldestIndex, char* out, size_t outLen) const {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  const bool ok = _ringLog.lineAt(oldestIndex, out, outLen);
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return ok;
}

void SystemStatus::clearLogs() {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _ringLog.clear();
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

void SystemStatus::setMqttConnected(bool connected) {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  _mqttConnected = connected;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
}

bool SystemStatus::mqttConnected() const {
#if defined(ARDUINO_ARCH_ESP32)
  lock(_mux);
#endif
  const bool v = _mqttConnected;
#if defined(ARDUINO_ARCH_ESP32)
  unlock(_mux);
#endif
  return v;
}

} // namespace ccms
