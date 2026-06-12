// Scopo del file:
// definisce i tipi base del protocollo ccTalk usati da tutto il progetto,
// in particolare `CcTalkFrame` e `CcTalkTransaction`.
#pragma once
#include <Arduino.h>

static const uint8_t CCTALK_ADDR_MASTER = 1;

// Rappresentazione "decodificata" di un frame ccTalk.
// Il parser conserva sia i campi interpretati sia il puntatore al buffer raw:
// questo permette ai decoder di lavorare sul protocollo senza perdere
// la possibilita di stampare il frame originale per debug.
struct CcTalkFrame {
  uint8_t dest = 0;
  uint8_t len  = 0;
  uint8_t src  = 0;
  uint8_t hdr  = 0;

  const uint8_t* data = nullptr;
  uint8_t dataLen = 0;

  const uint8_t* raw = nullptr;
  uint8_t rawLen = 0;
  bool crc16 = false;

  // Popola la vista strutturata a partire da un buffer raw gia validato.
  // Non copia i dati: i puntatori rimangono riferiti al buffer sorgente.
  void setFromRaw(const uint8_t* r, uint8_t rlen) {
    raw = r;
    rawLen = rlen;
    crc16 = false;

    if (!r || rlen < 5) { // minimo: Dest Len Src Hdr Chk
      dest = len = src = hdr = 0;
      data = nullptr;
      dataLen = 0;
      return;
    }

    dest = r[0];
    len  = r[1];
    src  = r[2];
    hdr  = r[3];

    data = &r[4];
    dataLen = (uint8_t)len;
  }

  // Popola la vista strutturata per pacchetti ccTalk con CRC-16:
  // [Dest][Len][CRC LSB][Hdr][Data...][CRC MSB].
  // In questo formato non c'e un source byte; il parser lo ricostruisce
  // dal contesto single-master o dalla richiesta pending.
  void setFromRawCrc16(const uint8_t* r, uint8_t rlen, uint8_t logicalSrc) {
    raw = r;
    rawLen = rlen;
    crc16 = true;

    if (!r || rlen < 5) {
      dest = len = src = hdr = 0;
      data = nullptr;
      dataLen = 0;
      return;
    }

    dest = r[0];
    len  = r[1];
    src  = logicalSrc;
    hdr  = r[3];

    data = &r[4];
    dataLen = (uint8_t)len;
  }
};

// Una transazione e composta da:
// - sola richiesta
// - sola risposta
// - coppia richiesta/risposta
// Questa struttura evita di forzare tutti i casi nel modello "frame singolo".
struct CcTalkTransaction {
  bool hasReq     = false;
  bool hasResp    = false;
  bool checksumOk = true;  // false = frame ricevuto con checksum non valido

  CcTalkFrame req;
  CcTalkFrame resp;
};
