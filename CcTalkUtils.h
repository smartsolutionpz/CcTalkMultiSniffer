// Scopo del file:
// dichiara utility di basso livello per checksum, CRC, dump e lettura
// di campi little-endian nei payload ccTalk.
#pragma once
#include <Arduino.h>

// Verifica il checksum classico ccTalk a somma modulo 256.
bool cctalkChecksumOk(const uint8_t* b, uint8_t n);
// Verifica il checksum CRC16-CCITT ccTalk:
// [Dest][Len][CRC LSB][Hdr][Data...][CRC MSB].
bool cctalkCrc16Ok(const uint8_t* b, uint8_t n);
// CRC16 IBM usato come firma interna per deduplicare frame/transazioni sniffate.
uint16_t crc16_ibm_update(uint16_t crc, uint8_t a);
uint16_t crc16_frame(uint16_t crc, const uint8_t* b, uint8_t n);

// Utility di dump per log umani.
void dumpHex(Stream& s, const uint8_t* b, uint8_t n);
void dumpAscii(Stream& s, const uint8_t* b, uint8_t n);

// Helper di lettura little-endian, frequenti nei payload ccTalk.
uint16_t readU16LE(const uint8_t* d);
uint32_t readU24LE(const uint8_t* d);
