// Scopo del file:
// [ANOMALY_DEBUG] implementazione del modulo di debug temporaneo per la
// cattura dell'anomalia IN/OUT. Vedi AnomalyDebugLog.h per il contratto
// pubblico e le istruzioni di rimozione a fine indagine.
#include "AnomalyDebugLog.h"

#include <Adafruit_FRAM_I2C.h>
#include <string.h>
#include <time.h>

namespace anomalydebug {

namespace {

// Sotto questa soglia l'orologio di sistema non e' mai stato sincronizzato
// via NTP (stesso criterio usato da WifiService::getClockInfo): un time_t
// minore di questa soglia non e' un'ora reale valida, e' solo il default
// dell'RTC interno all'avvio.
const int64_t kMinValidEpoch = 1704067200; // 2024-01-01T00:00:00Z

struct AnomalyLogEntry {
  uint32_t tsMs;
  uint8_t cmdHeader;
  uint8_t eventCounter;
  uint16_t deltaCents;
};

struct Channel {
  DeviceKind kind = DEV_NONE;
  uint8_t addr = 0;
  AnomalyLogEntry entries[kEntriesPerChannel];
  uint16_t writeIndex = 0;
  uint16_t count = 0;
};

struct __attribute__((packed)) FramChannelMeta {
  uint8_t kind;
  uint8_t addr;
  uint16_t entryCount;
};

// Layout persistito, replicato identico in due slot (vedi kSlotCount): la
// prima occorrenza mai vista (protetta, mai sovrascritta finche' non arriva
// clearCapturedAnomaly()) e la piu' recente (risalvata per intero ad ogni
// nuova occorrenza). Ogni slot ha la sua profondita' per canale limitata a
// kFramEntriesPerChannel (inferiore a kEntriesPerChannel del ring RAM live)
// per poter tenere DUE catture complete nei 32KB del chip MB85RC256V — vedi
// static_assert piu' sotto per il conto esatto dei byte occupati. Regione
// separata dallo snapshot economico esistente (status/FramPersistence.cpp),
// che occupa i primi 173 byte a partire dall'indirizzo 0.
struct __attribute__((packed)) FramHeader {
  uint32_t magic;
  uint16_t version;
  uint8_t captured; // 0 = slot vuoto, 1 = contiene una cattura valida
  uint8_t channelCount;
  uint32_t triggerTsMs;
  int64_t triggerWallClockEpoch; // 0 se non disponibile (NTP non sincronizzato al trigger)
  int32_t imbalanceCentsAtTrigger;
  uint32_t occurrenceCount; // condiviso: tenuto sincronizzato in entrambi gli slot
  FramChannelMeta channels[kChannelCount];
  uint32_t checksum;
};

const uint32_t kFramMagic = 0x43434144UL; // "CCAD"
const uint16_t kFramVersion = 2;          // v2: due slot (v1 era singolo, non retrocompatibile)
const uint16_t kFramBaseAddress = 512;

// Profondita' per canale salvata in ciascuno slot FRAM. Il ring RAM live
// (kEntriesPerChannel, vedi header) resta a 1000 eventi/canale per
// l'ispezione dal vivo via /debug/anomalylog; al momento della cattura si
// serializzano solo i kFramEntriesPerChannel eventi piu' recenti presenti
// nel ring, cosi' da poter tenere due catture complete nel budget del chip.
const uint16_t kFramEntriesPerChannel = 500;

enum SlotIndex : uint8_t { SLOT_FIRST = 0, SLOT_LATEST = 1, kSlotCount = 2 };

const uint32_t kSlotSize = sizeof(FramHeader) +
                           (uint32_t)kChannelCount * kFramEntriesPerChannel * sizeof(AnomalyLogEntry);

// Guardia di sicurezza: se in futuro kChannelCount/kFramEntriesPerChannel (o
// le struct sopra) cambiano dimensione, il build si rompe qui invece di
// scrivere silenziosamente oltre la fine del chip FRAM (MB85RC256V, 32768
// byte) o sopra la regione dello snapshot economico esistente.
static_assert((uint32_t)kFramBaseAddress + (uint32_t)kSlotCount * kSlotSize <= 32768UL,
              "[ANOMALY_DEBUG] la regione FRAM di debug (2 slot) supera la capacita del chip (32KB)");

const uint32_t kFramI2cHz = 400000;
const size_t kFramWriteChunk = 64;

Channel g_channels[kChannelCount];

uint32_t g_lastActivityMs = 0;
bool g_hasActivity = false;
bool g_settleEvaluated = false;
bool g_firstCaptured = false;
uint32_t g_occurrenceCount = 0;

Adafruit_FRAM_I2C g_fram;
TwoWire* g_wire = nullptr;
uint8_t g_i2cAddress = 0;
bool g_framReady = false;

uint32_t computeChecksum(const uint8_t* data, size_t len) {
  // FNV-1a 32-bit, stesso algoritmo di status/FramPersistence.cpp (reimplementato
  // qui per non creare una dipendenza fra i due moduli: sono regioni diverse
  // dello stesso chip e devono restare completamente indipendenti).
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool framReadBytes(uint16_t address, uint8_t* out, size_t len) {
  if (!out) return false;
  size_t remaining = len;
  uint16_t addr = address;
  uint8_t* dst = out;
  while (remaining > 0) {
    const uint16_t chunk = (remaining > 65535UL) ? 65535U : (uint16_t)remaining;
    if (!g_fram.read(addr, dst, chunk)) return false;
    addr = (uint16_t)(addr + chunk);
    dst += chunk;
    remaining -= chunk;
  }
  return true;
}

bool framWriteBytes(uint16_t address, const uint8_t* data, size_t len) {
  if (!data || !g_wire) return false;
  for (size_t offset = 0; offset < len; offset += kFramWriteChunk) {
    const size_t chunk = (len - offset > kFramWriteChunk) ? kFramWriteChunk : (len - offset);
    const uint16_t addr = (uint16_t)(address + offset);
    g_wire->beginTransmission(g_i2cAddress);
    g_wire->write((uint8_t)(addr >> 8));
    g_wire->write((uint8_t)(addr & 0xFF));
    if (g_wire->write(data + offset, chunk) != chunk) return false;
    if (g_wire->endTransmission() != 0) return false;
  }
  return true;
}

// Il bus I2C e' condiviso con un espansore PCF8574 garantito solo a 100kHz:
// come in FramPersistence, si alza il clock solo per la durata dell'accesso
// FRAM e si ripristina subito dopo.
uint32_t beginFramFastClock() {
  if (!g_wire) return 100000UL;
  const uint32_t prev = g_wire->getClock();
  g_wire->setClock(kFramI2cHz);
  return prev;
}

void endFramFastClock(uint32_t prevHz) {
  if (g_wire) g_wire->setClock(prevHz);
}

uint16_t slotBaseAddress(uint8_t slot) {
  return (uint16_t)(kFramBaseAddress + (uint32_t)slot * kSlotSize);
}

uint16_t channelPayloadAddress(uint8_t slot, uint8_t channelIndex) {
  return (uint16_t)(slotBaseAddress(slot) + sizeof(FramHeader) +
                     (uint32_t)channelIndex * kFramEntriesPerChannel * sizeof(AnomalyLogEntry));
}

bool loadFramHeader(uint8_t slot, FramHeader& out) {
  if (!g_framReady) return false;
  const uint32_t prevHz = beginFramFastClock();
  const bool ok = framReadBytes(slotBaseAddress(slot), reinterpret_cast<uint8_t*>(&out), sizeof(out));
  endFramFastClock(prevHz);
  if (!ok) return false;
  if (out.magic != kFramMagic || out.version != kFramVersion) return false;
  const uint32_t expected = computeChecksum(reinterpret_cast<const uint8_t*>(&out), offsetof(FramHeader, checksum));
  return out.checksum == expected;
}

bool writeFramHeader(uint8_t slot, const FramHeader& hdr) {
  const uint32_t prevHz = beginFramFastClock();
  const bool ok = framWriteBytes(slotBaseAddress(slot), reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
  endFramFastClock(prevHz);
  return ok;
}

int8_t findOrAssignChannel(DeviceKind kind, uint8_t addr) {
  for (uint8_t i = 0; i < kChannelCount; i++) {
    if (g_channels[i].kind == kind && g_channels[i].addr == addr) return (int8_t)i;
  }
  for (uint8_t i = 0; i < kChannelCount; i++) {
    if (g_channels[i].kind == DEV_NONE) {
      g_channels[i].kind = kind;
      g_channels[i].addr = addr;
      return (int8_t)i;
    }
  }
  // Tutti i canali gia' occupati da altre periferiche: caso non previsto per
  // l'impianto attuale (1 BV + 2 hopper + 1 gettoniera = 4). L'evento non
  // viene tracciato ma il timer di quiete resta comunque aggiornato dal
  // chiamante (logEvent), quindi il rilevamento anomalia non ne risente.
  return -1;
}

bool saveToSlot(uint8_t slot, int32_t imbalanceCents, uint32_t nowMs) {
  if (!g_framReady) return false;

  FramHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.magic = kFramMagic;
  hdr.version = kFramVersion;
  hdr.captured = 1;
  hdr.channelCount = kChannelCount;
  hdr.triggerTsMs = nowMs;
  const time_t nowEpoch = time(nullptr);
  hdr.triggerWallClockEpoch = ((int64_t)nowEpoch >= kMinValidEpoch) ? (int64_t)nowEpoch : 0;
  hdr.imbalanceCentsAtTrigger = imbalanceCents;
  hdr.occurrenceCount = g_occurrenceCount;
  for (uint8_t i = 0; i < kChannelCount; i++) {
    const uint16_t framCount = (g_channels[i].count < kFramEntriesPerChannel)
                                   ? g_channels[i].count
                                   : kFramEntriesPerChannel;
    hdr.channels[i].kind = g_channels[i].kind;
    hdr.channels[i].addr = g_channels[i].addr;
    hdr.channels[i].entryCount = framCount;
  }
  hdr.checksum = computeChecksum(reinterpret_cast<const uint8_t*>(&hdr), offsetof(FramHeader, checksum));

  if (!writeFramHeader(slot, hdr)) return false;

  const uint32_t prevHz = beginFramFastClock();
  bool allOk = true;
  for (uint8_t ch = 0; ch < kChannelCount; ch++) {
    const Channel& c = g_channels[ch];
    const uint16_t framCount = hdr.channels[ch].entryCount;
    if (framCount == 0) continue;

    // Delle c.count entry presenti nel ring RAM (capacita' kEntriesPerChannel),
    // si serializzano solo le framCount piu' recenti: si salta "skip" delle
    // piu' vecchie a partire dall'inizio logico del ring.
    const uint16_t ramOldestOverall = (c.count < kEntriesPerChannel) ? 0 : c.writeIndex;
    const uint16_t skip = (uint16_t)(c.count - framCount);
    const uint16_t startIdx = (uint16_t)((ramOldestOverall + skip) % kEntriesPerChannel);
    const uint16_t payloadAddr = channelPayloadAddress(slot, ch);

    // Scrittura linearizzata (dalla entry piu' vecchia tra quelle scelte):
    // evita di dover gestire il wrap del ring buffer in lettura
    // (visitFramChannel legge semplicemente entryCount entry consecutive
    // dall'inizio del blocco).
    AnomalyLogEntry chunkBuf[8];
    uint16_t written = 0;
    while (allOk && written < framCount) {
      uint8_t n = 0;
      while (n < 8 && written < framCount) {
        chunkBuf[n] = c.entries[(uint16_t)((startIdx + written) % kEntriesPerChannel)];
        n++;
        written++;
      }
      allOk = framWriteBytes((uint16_t)(payloadAddr + (uint32_t)(written - n) * sizeof(AnomalyLogEntry)),
                              reinterpret_cast<const uint8_t*>(chunkBuf),
                              (size_t)n * sizeof(AnomalyLogEntry));
    }
    if (!allOk) break;
  }
  endFramFastClock(prevHz);
  return allOk;
}

void updateOccurrenceCountInSlot(uint8_t slot) {
  FramHeader hdr;
  if (!loadFramHeader(slot, hdr)) return;
  hdr.occurrenceCount = g_occurrenceCount;
  hdr.checksum = computeChecksum(reinterpret_cast<const uint8_t*>(&hdr), offsetof(FramHeader, checksum));
  writeFramHeader(slot, hdr);
}

} // namespace

bool begin(TwoWire& wire, uint8_t i2cAddress) {
  g_wire = &wire;
  g_i2cAddress = i2cAddress;
  g_framReady = g_fram.begin(i2cAddress, &wire);

  if (g_framReady) {
    FramHeader hdr;
    if (loadFramHeader(SLOT_FIRST, hdr)) {
      g_firstCaptured = (hdr.captured != 0);
      g_occurrenceCount = hdr.occurrenceCount;
    }
  }
  return g_framReady;
}

void logEvent(DeviceKind kind, uint8_t addr, uint8_t cmdHeader, uint8_t eventCounter, uint16_t deltaCents) {
  const uint32_t nowMs = millis();
  g_lastActivityMs = nowMs;
  g_hasActivity = true;
  g_settleEvaluated = false;

  const int8_t idx = findOrAssignChannel(kind, addr);
  if (idx < 0) return;

  Channel& ch = g_channels[(uint8_t)idx];
  AnomalyLogEntry& e = ch.entries[ch.writeIndex];
  e.tsMs = nowMs;
  e.cmdHeader = cmdHeader;
  e.eventCounter = eventCounter;
  e.deltaCents = deltaCents;
  ch.writeIndex = (uint16_t)((ch.writeIndex + 1) % kEntriesPerChannel);
  if (ch.count < kEntriesPerChannel) ch.count++;
}

void tick(int32_t currentSaldoCents, uint32_t nowMs) {
  if (!g_hasActivity || g_settleEvaluated) return;
  if ((uint32_t)(nowMs - g_lastActivityMs) < kSettleMs) return;

  // Un solo controllo per ogni periodo di quiete: evita di ripetere il
  // trigger ad ogni chiamata mentre l'anomalia resta ferma e non cambia.
  g_settleEvaluated = true;
  if (currentSaldoCents == 0) return;

  g_occurrenceCount++;

  // Lo slot "latest" viene sempre risalvato per intero con il dettaglio
  // dell'occorrenza corrente: cosi', anche se nessuno ha ancora analizzato
  // occorrenze precedenti, il dettaglio di quella piu' recente resta
  // comunque disponibile.
  saveToSlot(SLOT_LATEST, currentSaldoCents, nowMs);

  // Lo slot "first" viene scritto una sola volta, alla primissima occorrenza
  // da avvio (o dall'ultimo clearCapturedAnomaly()), e resta protetto.
  if (!g_firstCaptured) {
    g_firstCaptured = true;
    saveToSlot(SLOT_FIRST, currentSaldoCents, nowMs);
  } else {
    // Il contenuto dello slot "first" non cambia, ma il contatore di
    // occorrenze resta sincronizzato anche li', cosi' e' leggibile da
    // entrambi gli slot indipendentemente.
    updateOccurrenceCountInSlot(SLOT_FIRST);
  }
}

bool hasCapturedAnomaly() {
  FramHeader hdr;
  return loadFramHeader(SLOT_FIRST, hdr) && hdr.captured != 0;
}

void clearCapturedAnomaly() {
  for (uint8_t slot = 0; slot < kSlotCount; slot++) {
    FramHeader hdr;
    if (loadFramHeader(slot, hdr)) {
      hdr.captured = 0;
      hdr.checksum = computeChecksum(reinterpret_cast<const uint8_t*>(&hdr), offsetof(FramHeader, checksum));
      writeFramHeader(slot, hdr);
    }
  }
  g_firstCaptured = false;
}

FramCaptureInfo framCaptureInfo() {
  FramCaptureInfo info;

  FramHeader firstHdr;
  if (!loadFramHeader(SLOT_FIRST, firstHdr) || !firstHdr.captured) return info;

  info.present = true;
  info.occurrenceCount = firstHdr.occurrenceCount;
  info.firstTriggerTsMs = firstHdr.triggerTsMs;
  info.firstTriggerWallClockEpoch = firstHdr.triggerWallClockEpoch;
  info.firstImbalanceCents = firstHdr.imbalanceCentsAtTrigger;

  FramHeader latestHdr;
  if (loadFramHeader(SLOT_LATEST, latestHdr) && latestHdr.captured) {
    info.occurrenceCount = latestHdr.occurrenceCount; // piu' aggiornato
    info.latestTriggerTsMs = latestHdr.triggerTsMs;
    info.latestTriggerWallClockEpoch = latestHdr.triggerWallClockEpoch;
    info.latestImbalanceCents = latestHdr.imbalanceCentsAtTrigger;
  } else {
    // Non dovrebbe succedere (latest viene sempre scritto insieme a first),
    // ma per robustezza ricadiamo sui valori di "first" anche per "latest".
    info.latestTriggerTsMs = firstHdr.triggerTsMs;
    info.latestTriggerWallClockEpoch = firstHdr.triggerWallClockEpoch;
    info.latestImbalanceCents = firstHdr.imbalanceCentsAtTrigger;
  }
  return info;
}

uint8_t ramChannelCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kChannelCount; i++) {
    if (g_channels[i].kind != DEV_NONE) n++;
  }
  return n;
}

bool visitRamChannel(uint8_t channelIndex, EntryVisitor visitor, void* ctx) {
  if (channelIndex >= kChannelCount || !visitor) return false;
  const Channel& ch = g_channels[channelIndex];
  if (ch.kind == DEV_NONE) return false;

  const time_t nowEpoch = time(nullptr);
  const bool epochValid = (int64_t)nowEpoch >= kMinValidEpoch;
  const uint32_t nowMs = millis();

  const uint16_t oldestIdx = (ch.count < kEntriesPerChannel) ? 0 : ch.writeIndex;
  for (uint16_t i = 0; i < ch.count; i++) {
    const AnomalyLogEntry& e = ch.entries[(uint16_t)((oldestIdx + i) % kEntriesPerChannel)];
    int64_t wallClockEpoch = 0;
    if (epochValid) {
      const uint32_t agoMs = (uint32_t)(nowMs - e.tsMs);
      wallClockEpoch = (int64_t)nowEpoch - (int64_t)(agoMs / 1000UL);
    }
    visitor(ctx, ch.kind, ch.addr, e.cmdHeader, e.eventCounter, e.deltaCents, e.tsMs, wallClockEpoch);
  }
  return true;
}

bool visitFramChannel(FramSlot slot, uint8_t channelIndex, EntryVisitor visitor, void* ctx) {
  if (channelIndex >= kChannelCount || !visitor) return false;

  FramHeader hdr;
  if (!loadFramHeader((uint8_t)slot, hdr) || !hdr.captured) return false;

  const FramChannelMeta& meta = hdr.channels[channelIndex];
  if (meta.kind == DEV_NONE || meta.entryCount == 0) return false;

  const uint16_t payloadAddr = channelPayloadAddress((uint8_t)slot, channelIndex);
  const uint32_t prevHz = beginFramFastClock();

  AnomalyLogEntry chunkBuf[8];
  bool ok = true;
  for (uint16_t i = 0; ok && i < meta.entryCount; i = (uint16_t)(i + 8)) {
    const uint16_t n = (uint16_t)(((meta.entryCount - i) > 8) ? 8 : (meta.entryCount - i));
    ok = framReadBytes((uint16_t)(payloadAddr + (uint32_t)i * sizeof(AnomalyLogEntry)),
                        reinterpret_cast<uint8_t*>(chunkBuf),
                        (size_t)n * sizeof(AnomalyLogEntry));
    if (!ok) break;

    for (uint16_t k = 0; k < n; k++) {
      const AnomalyLogEntry& e = chunkBuf[k];
      int64_t wallClockEpoch = 0;
      if (hdr.triggerWallClockEpoch != 0) {
        const uint32_t agoMs = (uint32_t)(hdr.triggerTsMs - e.tsMs);
        wallClockEpoch = hdr.triggerWallClockEpoch - (int64_t)(agoMs / 1000UL);
      }
      visitor(ctx, (DeviceKind)meta.kind, meta.addr, e.cmdHeader, e.eventCounter, e.deltaCents, e.tsMs, wallClockEpoch);
    }
  }
  endFramFastClock(prevHz);
  return ok;
}

} // namespace anomalydebug
