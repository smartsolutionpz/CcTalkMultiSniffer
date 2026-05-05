// Scopo del file:
// implementa gli helper condivisi della classe base `CcTalkDevice`.
#include "CcTalkDevice.h"

void CcTalkDevice::printRawIf(Stream& out, bool en, const CcTalkFrame& f, const char* tag) {
  // Separare questa logica evita duplicazioni in tutti i decoder concreti.
  if (!en || !f.raw || !f.rawLen) return;

  out.print(tag);
  out.print(F(": "));
  for (uint8_t i = 0; i < f.rawLen; i++) {
    out.print(F("0x"));
    if (f.raw[i] < 16) out.print('0');
    out.print(f.raw[i], HEX);
    out.print(' ');
  }
  out.println();
}
