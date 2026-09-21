// Scopo del file:
// implementa la persistenza binaria su FRAM degli snapshot applicativi.
#include "FramPersistence.h"

#include <stddef.h>
#include <string.h>

namespace ccms {

bool FramPersistence::begin(TwoWire& wire, uint8_t i2cAddress) {
  // `begin()` non scrive nulla: verifica solo che il chip FRAM risponda.
  _wire = &wire;
  _i2cAddress = i2cAddress;
  _ready = _fram.begin(i2cAddress, &wire);
  if (_ready) bootstrapSlots();
  return _ready;
}

bool FramPersistence::readSlot(uint16_t address, StoredLayout& out) {
  // Tutta la struttura viene letta in RAM e validata prima di esporla al resto
  // del sistema, cosi snapshot corrotti non contaminano lo stato runtime.
  const uint32_t prevHz = _wire->getClock();
  _wire->setClock(kFramI2cHz);
  const bool ok = readBytes(address, reinterpret_cast<uint8_t*>(&out), sizeof(out));
  _wire->setClock(prevHz);
  if (!ok) return false;

  if (out.magic != kMagic) return false;
  if (out.version != kVersion) return false;
  if (out.size != sizeof(StoredLayout)) return false;

  const uint32_t expected = computeChecksum(reinterpret_cast<const uint8_t*>(&out),
                                            offsetof(StoredLayout, checksum));
  return out.checksum == expected;
}

void FramPersistence::bootstrapSlots() {
  // Scansiona entrambi gli slot indipendentemente da load(), cosi il
  // bookkeeping (slot da alternare, prossimo sequence number) e pronto anche
  // se il chiamante invoca save()/saveFramNow() prima di un eventuale load().
  StoredLayout a, b;
  const bool aValid = readSlot(kSlotAAddress, a);
  const bool bValid = readSlot(kSlotBAddress, b);

  if (aValid && bValid) {
    const bool bIsNewer = (int32_t)(b.seq - a.seq) > 0;
    _lastGoodSlotAddress = bIsNewer ? kSlotBAddress : kSlotAAddress;
    _nextSeq = (bIsNewer ? b.seq : a.seq) + 1;
  } else if (aValid) {
    _lastGoodSlotAddress = kSlotAAddress;
    _nextSeq = a.seq + 1;
  } else if (bValid) {
    _lastGoodSlotAddress = kSlotBAddress;
    _nextSeq = b.seq + 1;
  } else {
    // Nessuno slot valido: il primo save() scrivera in A (vedi default membri).
    _lastGoodSlotAddress = kSlotBAddress;
    _nextSeq = 1;
  }
}

bool FramPersistence::load(Snapshot& out) {
  if (!_ready || !_wire) return false;

  StoredLayout a, b;
  const bool aValid = readSlot(kSlotAAddress, a);
  const bool bValid = readSlot(kSlotBAddress, b);

  const StoredLayout* chosen = nullptr;
  if (aValid && bValid) {
    chosen = ((int32_t)(b.seq - a.seq) > 0) ? &b : &a;
  } else if (aValid) {
    chosen = &a;
  } else if (bValid) {
    chosen = &b;
  } else {
    return false;
  }

  storedToSnapshot(*chosen, out);
  return true;
}

bool FramPersistence::save(const Snapshot& in) {
  if (!_ready || !_wire) return false;

  // Ping-pong: si scrive sempre nello slot diverso dall'ultimo buono, cosi
  // una perdita di alimentazione a meta scrittura lascia intatto lo stato
  // precedente invece di invalidare l'unico snapshot persistito.
  const uint16_t targetAddress =
      (_lastGoodSlotAddress == kSlotAAddress) ? kSlotBAddress : kSlotAAddress;

  // Il checksum viene calcolato sul layout serializzato, non sullo snapshot
  // logico, per proteggere esattamente i byte memorizzati.
  StoredLayout raw;
  snapshotToStored(in, raw);
  raw.seq = _nextSeq;
  raw.checksum = computeChecksum(reinterpret_cast<const uint8_t*>(&raw),
                                 offsetof(StoredLayout, checksum));

  const uint32_t prevHz = _wire->getClock();
  _wire->setClock(kFramI2cHz);
  const bool ok = writeBytes(targetAddress, reinterpret_cast<const uint8_t*>(&raw), sizeof(raw));
  _wire->setClock(prevHz);
  if (!ok) return false;

  _lastGoodSlotAddress = targetAddress;
  _nextSeq++;
  return true;
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
  // Scrittura a blocco: una transazione I2C ogni kWriteChunk byte invece di una
  // per byte. La FRAM auto-incrementa il puntatore interno, quindi ogni chunk
  // rispedisce solo i 2 byte di indirizzo iniziale. Nessuna attesa di ciclo di
  // scrittura: la FRAM conferma subito con l'ACK di endTransmission().
  if (!data || !_wire) return false;

  for (size_t offset = 0; offset < len; offset += kWriteChunk) {
    const size_t chunk = (len - offset > kWriteChunk) ? kWriteChunk : (len - offset);
    const uint16_t addr = (uint16_t)(address + offset);

    _wire->beginTransmission(_i2cAddress);
    _wire->write((uint8_t)(addr >> 8));
    _wire->write((uint8_t)(addr & 0xFF));
    if (_wire->write(data + offset, chunk) != chunk) return false;
    if (_wire->endTransmission() != 0) return false;
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
