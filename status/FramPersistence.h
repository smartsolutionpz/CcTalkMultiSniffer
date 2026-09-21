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
  // v3: doppio slot con sequence number (v2 era singolo slot, non
  // retrocompatibile: al primo boot post-aggiornamento lo snapshot v2
  // viene scartato per mismatch di version/size, stesso fallback "avvio da
  // zero" gia' esistente).
  static const uint16_t kVersion = 3;

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
    uint32_t seq; // sequence number monotono, usato per scegliere lo slot piu recente
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

  // Doppio slot (ping-pong): ogni save() scrive nello slot diverso
  // dall'ultimo buono, cosi una perdita di alimentazione a meta scrittura
  // lascia intatto lo slot precedente invece di invalidare l'unico stato
  // persistito. Stesso pattern gia' usato da AnomalyDebugLog.cpp (che occupa
  // la regione a partire da indirizzo 512).
  static const uint16_t kSlotAAddress = 0;
  static const uint16_t kSlotBAddress = sizeof(StoredLayout);
  static_assert((uint32_t)kSlotBAddress + sizeof(StoredLayout) <= 512UL,
                "FramPersistence: i due slot economici superano lo spazio riservato prima della regione AnomalyDebugLog (indirizzo 512)");

  // Clock I2C usato solo durante le operazioni FRAM. Il bus torna al valore
  // precedente subito dopo: il PCF8574 sulla stessa linea e garantito solo a
  // 100 kHz.
  static const uint32_t kFramI2cHz = 400000;
  // Byte dati per transazione a blocco (piu i 2 byte di indirizzo restano sotto
  // il buffer Wire di default).
  static const size_t kWriteChunk = 64;

  static uint32_t computeChecksum(const uint8_t* data, size_t len);

  bool readBytes(uint16_t address, uint8_t* out, size_t len);
  bool writeBytes(uint16_t address, const uint8_t* data, size_t len);

  static void snapshotToStored(const Snapshot& in, StoredLayout& out);
  static void storedToSnapshot(const StoredLayout& in, Snapshot& out);

  // Legge e valida (magic/version/size/checksum) un singolo slot, senza
  // toccare lo stato di bookkeeping (_lastGoodSlotAddress/_nextSeq).
  bool readSlot(uint16_t address, StoredLayout& out);
  // Scansiona entrambi gli slot e inizializza _lastGoodSlotAddress/_nextSeq
  // in base al piu recente valido trovato (o ai default se nessuno lo e).
  // Chiamato da begin() cosi il bookkeeping e pronto anche se il chiamante
  // non invoca mai load().
  void bootstrapSlots();

  Adafruit_FRAM_I2C _fram;
  TwoWire* _wire = nullptr;
  uint8_t _i2cAddress = 0x50;
  bool _ready = false;

  uint16_t _lastGoodSlotAddress = kSlotBAddress; // cosi il primo save() scrive lo slot A
  uint32_t _nextSeq = 1;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_STATUS_FRAM_PERSISTENCE_H
