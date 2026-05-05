// Scopo del file:
// dichiara `RingLog`, il buffer circolare che conserva le ultime righe di log
// prodotte dal firmware.
#ifndef CCTALK_MULTI_SNIFFER_STATUS_RING_LOG_H
#define CCTALK_MULTI_SNIFFER_STATUS_RING_LOG_H

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

namespace ccms {

// Ring buffer di righe testuali.
// Viene usato come storico compatto e non bloccante dei log recenti esposti
// a seriale, web UI e altri servizi.
class RingLog {
public:
  static const uint8_t kCapacity = 64;
  static const uint16_t kLineSize = 192;

  RingLog();

  // Azzera completamente contenuto e indici.
  void clear();
  // Inserisce una nuova riga; se il buffer e pieno sovrascrive la piu vecchia.
  void push(const char* line);
  uint8_t count() const;
  // Recupera la riga per indice "dal piu vecchio al piu nuovo".
  bool lineAt(uint8_t oldestIndex, char* out, size_t outLen) const;

private:
  char _lines[kCapacity][kLineSize];
  uint8_t _writeIndex = 0;
  uint8_t _count = 0;
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_STATUS_RING_LOG_H
