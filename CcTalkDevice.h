// Scopo del file:
// dichiara `CcTalkDevice`, l'interfaccia astratta comune a tutti i decoder
// di periferica osservati sul bus ccTalk.
#pragma once
#include <Arduino.h>
#include "CcTalkTypes.h"

// Interfaccia base per tutti i decoder di periferica ccTalk.
// Ogni device concreto sa:
// - riconoscere se un indirizzo gli appartiene
// - descrivere se stesso
// - decodificare una transazione in output umano e/o stato interno
class CcTalkDevice {
public:
  virtual ~CcTalkDevice() = default;

  // Ritorna true se l'indirizzo ccTalk appartiene alla famiglia di device.
  virtual bool matches(uint8_t addr) const = 0;
  virtual const char* name() const = 0;

  // Decodifica la transazione osservata.
  // `printRaw` e tenuto nell'interfaccia per compatibilita con l'orchestrazione,
  // anche se alcuni decoder oggi non lo usano direttamente.
  virtual void onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) = 0;

  // Helper condiviso: stampa il frame raw in modo uniforme.
  static void printRawIf(Stream& out, bool en, const CcTalkFrame& f, const char* tag);
};
