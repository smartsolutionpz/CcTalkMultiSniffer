// Scopo del file:
// implementa le utility ccTalk condivise da parser e decoder del progetto.
#include "CcTalkUtils.h"

bool cctalkChecksumOk(const uint8_t* b, uint8_t n) {
  // Nel framing ccTalk la somma di tutti i byte, incluso il checksum finale,
  // deve dare 0 modulo 256.
  uint16_t sum = 0;
  for (uint8_t i = 0; i < n; i++) sum = (sum + b[i]) & 0xFF;
  return (sum == 0);
}

uint16_t crc16_ibm_update(uint16_t crc, uint8_t a) {
  // CRC separato dal checksum ccTalk:
  // qui non valida il protocollo, ma genera una firma piu robusta per dedup.
  crc ^= a;
  for (uint8_t i = 0; i < 8; ++i) {
    if (crc & 1) crc = (crc >> 1) ^ 0xA001;
    else         crc = (crc >> 1);
  }
  return crc;
}

uint16_t crc16_frame(uint16_t crc, const uint8_t* b, uint8_t n) {
  for (uint8_t i = 0; i < n; i++) crc = crc16_ibm_update(crc, b[i]);
  return crc;
}

void dumpHex(Stream& s, const uint8_t* b, uint8_t n) {
  // Stampa in formato leggibile e stabile, utile per confronti da log.
  for (uint8_t i = 0; i < n; i++) {
    s.print(F("0x"));
    if (b[i] < 16) s.print('0');
    s.print(b[i], HEX);
    s.print(' ');
  }
}

void dumpAscii(Stream& s, const uint8_t* b, uint8_t n) {
  // I byte non stampabili vengono sostituiti con '.' per evitare output sporco.
  for (uint8_t i = 0; i < n; i++) {
    char c = (char)b[i];
    if (c >= 32 && c <= 126) s.print(c);
    else s.print('.');
  }
}

uint16_t readU16LE(const uint8_t* d) {
  // ccTalk usa spesso campi little-endian multi-byte.
  return (uint16_t)d[0] | ((uint16_t)d[1] << 8);
}

uint32_t readU24LE(const uint8_t* d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16);
}
