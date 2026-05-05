// Scopo del file:
// dichiara `CcTalkRouter`, la classe che sceglie quale decoder deve gestire
// una transazione ccTalk in base all'indirizzo del device.
#pragma once
#include <Arduino.h>
#include "CcTalkDevice.h"

// Router semplice "indirizzo -> decoder".
// Lo sketch registra qui le famiglie di device supportate e, per ogni
// transazione sniffata, il router sceglie quale decoder deve occuparsene.
class CcTalkRouter {
public:
  CcTalkRouter() {}

  // Aggiunge un decoder all'elenco interno in ordine di registrazione.
  void add(CcTalkDevice* d);
  // Instrada la transazione al decoder scelto e, se richiesto, stampa anche
  // i frame raw completi come post-processing uniforme.
  void route(const CcTalkTransaction& t, Stream& out, bool printFull);

private:
  CcTalkDevice* _devs[12];
  uint8_t _count = 0;

  // Risolve il decoder corretto in base all'indirizzo ccTalk osservato.
  CcTalkDevice* pickByAddr(uint8_t addr);
};
