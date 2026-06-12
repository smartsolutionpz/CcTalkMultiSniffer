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

static uint16_t crc16CcittUpdate(uint16_t crc, uint8_t x) {
  crc ^= ((uint16_t)x << 8);
  for (uint8_t j = 0; j < 8; j++) {
    if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021);
    else              crc = (uint16_t)(crc << 1);
  }
  return crc;
}

static uint16_t cctalkFrameCrc16(const uint8_t* b, uint8_t n) {
  if (!b || n < 5) return 0;

  // Layout ccTalk CRC:
  //   [Dest][Len][CRC LSB][Hdr][Data...][CRC MSB]
  // Il CRC si calcola sul comando logico [Dest][Len][Hdr][Data...].
  uint16_t crc = 0x0000;
  crc = crc16CcittUpdate(crc, b[0]);
  crc = crc16CcittUpdate(crc, b[1]);
  crc = crc16CcittUpdate(crc, b[3]);
  const uint8_t dataLen = b[1];
  for (uint8_t i = 0; i < dataLen && (uint16_t)(4u + i) < (uint16_t)(n - 1u); i++) {
    crc = crc16CcittUpdate(crc, b[4 + i]);
  }
  return crc;
}

bool cctalkCrc16Ok(const uint8_t* b, uint8_t n) {
  // Il CRC-16 ccTalk non aggiunge un byte al frame: sostituisce il source
  // con il CRC LSB e mette il CRC MSB nell'ultimo byte.
  if (!b || n < 5) return false;
  const uint8_t dataLen = b[1];
  if ((uint16_t)n != (uint16_t)(5u + dataLen)) return false;

  const uint16_t expected = cctalkFrameCrc16(b, n);
  const uint16_t got = (uint16_t)b[2] | ((uint16_t)b[n - 1] << 8);
  return got == expected;
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
