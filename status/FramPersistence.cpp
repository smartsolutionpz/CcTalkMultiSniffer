// Scopo del file:
// implementa la persistenza binaria su FRAM degli snapshot applicativi.
#include "FramPersistence.h"

#include <stddef.h>
#include <string.h>

namespace ccms {

bool FramPersistence::begin(TwoWire& wire, uint8_t i2cAddress) {
  // `begin()` non scrive nulla: verifica solo che il chip FRAM risponda.
  _ready = _fram.begin(i2cAddress, &wire);
  return _ready;
}

bool FramPersistence::load(Snapshot& out) {
  if (!_ready) return false;

  // Tutta la struttura viene letta in RAM e validata prima di esporla al resto
  // del sistema, cosi snapshot corrotti non contaminano lo stato runtime.
  StoredLayout raw;
  if (!readBytes(kBaseAddress, reinterpret_cast<uint8_t*>(&raw), sizeof(raw))) return false;

  if (raw.magic != kMagic) return false;
  if (raw.version != kVersion) return false;
  if (raw.size != sizeof(StoredLayout)) return false;

  const uint32_t expected = computeChecksum(reinterpret_cast<const uint8_t*>(&raw),
                                            offsetof(StoredLayout, checksum));
  if (raw.checksum != expected) return false;

  storedToSnapshot(raw, out);
  return true;
}

bool FramPersistence::save(const Snapshot& in) {
  if (!_ready) return false;

  // Il checksum viene calcolato sul layout serializzato, non sullo snapshot
  // logico, per proteggere esattamente i byte memorizzati.
  StoredLayout raw;
  snapshotToStored(in, raw);
  raw.checksum = computeChecksum(reinterpret_cast<const uint8_t*>(&raw),
                                 offsetof(StoredLayout, checksum));

  return writeBytes(kBaseAddress, reinterpret_cast<const uint8_t*>(&raw), sizeof(raw));
}

uint32_t FramPersistence::computeChecksum(const uint8_t* data, size_t len) {
  if (!data) return 0;

  // FNV-1a 32-bit: semplice, veloce e adeguato per rilevare corruzioni accidentali.
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    hash ^= data[i];
    hash *= 16777619UL;
  }
  return hash;
}

bool FramPersistence::readBytes(uint16_t address, uint8_t* out, size_t len) {
  // Lettura a chunk per compatibilita con l'API della libreria FRAM.
  if (!out) return false;
  size_t remaining = len;
  uint16_t addr = address;
  uint8_t* dst = out;

  while (remaining > 0) {
    uint16_t chunk = (remaining > 65535UL) ? 65535U : (uint16_t)remaining;
    if (!_fram.read(addr, dst, chunk)) return false;
    addr = (uint16_t)(addr + chunk);
    dst += chunk;
    remaining -= chunk;
  }
  return true;
}

bool FramPersistence::writeBytes(uint16_t address, const uint8_t* data, size_t len) {
  // La libreria scrive byte per byte; la FRAM tollera bene questo pattern.
  if (!data) return false;
  for (size_t i = 0; i < len; i++) {
    if (!_fram.write((uint16_t)(address + i), data[i])) return false;
  }
  return true;
}

void FramPersistence::snapshotToStored(const Snapshot& in, StoredLayout& out) {
  // Conversione dal modello logico al layout persistito/versionato.
  memset(&out, 0, sizeof(out));
  out.magic = kMagic;
  out.version = kVersion;
  out.size = sizeof(StoredLayout);

  out.cntotBanconoteInCents = in.economic.cntotBanconoteInCents;
  out.cntotMoneteOutCents = in.economic.cntotMoneteOutCents;
  out.cntotMoneteInCents = in.economic.cntotMoneteInCents;
  out.cntotBanconoteOutCents = in.economic.cntotBanconoteOutCents;
  out.cassaCents = in.economic.cassaCents;
  out.recyclerInventoryTotaleCents = in.economic.recyclerInventoryTotaleCents;
  out.coinLevelBaseCents = in.economic.coinLevelBaseCents;

  const uint8_t maxEntries = SystemStatus::kMaxRecyclerEntries;
  uint8_t count = in.recyclerCount;
  if (count > maxEntries) count = maxEntries;
  out.recyclerCount = count;

  for (uint8_t i = 0; i < count; i++) {
    const SystemStatus::RecyclerInventoryEntry& src = in.recycler[i];
    StoredRecyclerEntry& dst = out.recycler[i];
    dst.valid = src.valid ? 1 : 0;
    dst.addr = src.addr;
    dst.count10 = src.count10;
    dst.count20 = src.count20;
    dst.count50 = src.count50;
    dst.totalCents = src.totalCents;
  }
}

void FramPersistence::storedToSnapshot(const StoredLayout& in, Snapshot& out) {
  // Conversione inversa: oltre ai campi base, ricalcola i valori economici derivati.
  out = Snapshot();
  out.economic.cntotBanconoteInCents = in.cntotBanconoteInCents;
  out.economic.cntotMoneteOutCents = in.cntotMoneteOutCents;
  out.economic.cntotMoneteInCents = in.cntotMoneteInCents;
  out.economic.cntotBanconoteOutCents = in.cntotBanconoteOutCents;
  out.economic.cntotBanconoteCents =
      (int64_t)in.cntotBanconoteInCents - (int64_t)in.cntotBanconoteOutCents;
  out.economic.cntotMoneteCents =
      (int64_t)in.cntotMoneteInCents - (int64_t)in.cntotMoneteOutCents;
  out.economic.saldoCents = out.economic.cntotBanconoteCents + out.economic.cntotMoneteCents;
  out.economic.cassaCents = in.cassaCents;
  out.economic.recyclerInventoryTotaleCents = in.recyclerInventoryTotaleCents;
  out.economic.coinLevelBaseCents = in.coinLevelBaseCents;
  out.economic.coinCurrentCents = (int64_t)in.coinLevelBaseCents + out.economic.cntotMoneteCents;

  const uint8_t maxEntries = SystemStatus::kMaxRecyclerEntries;
  uint8_t count = in.recyclerCount;
  if (count > maxEntries) count = maxEntries;
  out.recyclerCount = count;

  for (uint8_t i = 0; i < count; i++) {
    const StoredRecyclerEntry& src = in.recycler[i];
    SystemStatus::RecyclerInventoryEntry& dst = out.recycler[i];
    dst.valid = (src.valid != 0);
    dst.addr = src.addr;
    dst.count10 = src.count10;
    dst.count20 = src.count20;
    dst.count50 = src.count50;
    dst.totalCents = src.totalCents;
  }
}

} // namespace ccms
