// Scopo del file:
// dichiara `CcTalkMaster`, il catalogo dei comandi noti del master ccTalk
// usato come supporto testuale dai decoder.
#pragma once
#include <Arduino.h>

// Catalogo descrittivo degli header emessi dal master ccTalk.
// Non rappresenta un device reale: e un helper usato dai decoder per fornire
// una descrizione di fallback quando non hanno una mappa piu specifica.
class CcTalkMaster {
public:
  static const __FlashStringHelper* headerDesc(uint8_t hdr);

  // In modalita RX-only non inviamo questi comandi: il catalogo esiste solo
  // come supporto semantico alla decodifica del traffico osservato.
};
