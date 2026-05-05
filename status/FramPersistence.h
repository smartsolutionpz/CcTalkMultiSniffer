// Scopo del file:
// dichiara `FramPersistence`, la classe che serializza e ripristina snapshot
// economici e recycler su memoria FRAM I2C.
#ifndef CCTALK_MULTI_SNIFFER_STATUS_FRAM_PERSISTENCE_H
#define CCTALK_MULTI_SNIFFER_STATUS_FRAM_PERSISTENCE_H

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_FRAM_I2C.h>

#include "SystemStatus.h"

namespace ccms {

// Persistenza compatta su FRAM I2C per il baseline economico dell'applicazione.
// Incapsula layout binario, versioning e checksum, cosi il resto del codice
// lavora solo con snapshot ad alto livello.
class FramPersistence {
public:
  struct Snapshot {
    SystemStatus::EconomicFields economic;
    SystemStatus::RecyclerInventoryEntry recycler[SystemStatus::kMaxRecyclerEntries];
    uint8_t recyclerCount = 0;
  };

  bool begin(TwoWire& wire = Wire, uint8_t i2cAddress = 0x50);
  bool isReady() const { return _ready; }

  // Carica uno snapshot validando magic, versione, dimensione e checksum.
  bool load(Snapshot& out);
  // Salva uno snapshot convertendolo nel layout binario persistito.
  bool save(const Snapshot& in);

private:
  static const uint32_t kMagic = 0x43434652UL; // "CCFR"
  static const uint16_t kVersion = 2;
  static const uint16_t kBaseAddress = 0;

  struct __attribute__((packed)) StoredRecyclerEntry {
    uint8_t valid;
    uint8_t addr;
    uint16_t count10;
    uint16_t count20;
    uint16_t count50;
    uint32_t totalCents;
  };

  // Rappresentazione binaria effettivamente scritta in FRAM.
  struct __attribute__((packed)) StoredLayout {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t cntotBanconoteInCents;
    uint32_t cntotMoneteOutCents;
    uint32_t cntotMoneteInCents;
    uint32_t cntotBanconoteOutCents;
    uint32_t cassaCents;
    uint32_t recyclerInventoryTotaleCents;
    uint32_t coinLevelBaseCents;
    uint8_t recyclerCount;
    StoredRecyclerEntry recycler[SystemStatus::kMaxRecyclerEntries];
    uint32_t checksum;
  };

  static uint32_t computeChecksum(const uint8_t* data, size_t len);

  bool readBytes(uint16_t address, uint8_t* out, size_t len);
  bool writeBytes(uint16_t address, const uint8_t* data, size_t len);

  static void snapshotToStored(const Snapshot& in, StoredLayout& out);
  static void storedToSnapshot(const StoredLayout& in, Snapshot& out);

  Adafruit_FRAM_I2C _fram;
  bool _ready = false;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_STATUS_FRAM_PERSISTENCE_H
