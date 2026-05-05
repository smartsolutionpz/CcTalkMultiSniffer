// Scopo del file:
// implementa `RingLog`, il contenitore compatto dei log recenti.
#include "RingLog.h"

#include <string.h>

namespace ccms {

RingLog::RingLog() {
  clear();
}

void RingLog::clear() {
  // Azzerare i buffer semplifica il debug e impedisce di esporre dati sporchi.
  memset(_lines, 0, sizeof(_lines));
  _writeIndex = 0;
  _count = 0;
}

void RingLog::push(const char* line) {
  // Il ring buffer mantiene solo le ultime `kCapacity` righe.
  const char* safe = line ? line : "";
  strncpy(_lines[_writeIndex], safe, (size_t)(kLineSize - 1));
  _lines[_writeIndex][kLineSize - 1] = '\0';

  _writeIndex = (uint8_t)((_writeIndex + 1) % kCapacity);
  if (_count < kCapacity) _count++;
}

uint8_t RingLog::count() const {
  return _count;
}

bool RingLog::lineAt(uint8_t oldestIndex, char* out, size_t outLen) const {
  if (!out || outLen == 0) return false;
  if (oldestIndex >= _count) return false;

  // `_writeIndex` punta sempre al prossimo slot di scrittura; da qui si ricava
  // la posizione dell'elemento piu vecchio ancora valido.
  const uint8_t oldestPos = (uint8_t)((_writeIndex + kCapacity - _count) % kCapacity);
  const uint8_t idx = (uint8_t)((oldestPos + oldestIndex) % kCapacity);

  strncpy(out, _lines[idx], outLen - 1);
  out[outLen - 1] = '\0';
  return true;
}

} // namespace ccms
