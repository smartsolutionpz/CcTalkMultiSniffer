// Scopo del file:
// dichiara `CcTalkBusSniffer`, il parser incrementale che ascolta la UART
// e ricostruisce frame e transazioni ccTalk.
#pragma once
#include <Arduino.h>
#include "CcTalkTypes.h"

// Parser incrementale del traffico ccTalk osservato su UART.
// Questa classe:
// - ricostruisce i frame a partire dal flusso di byte
// - abbina richiesta e risposta quando possibile
// - gestisce il caso speciale MDCES a 1 byte
// - deduplica gli eventi sniffati tramite CRC interno
class CcTalkBusSniffer {
public:
  enum PrintMode : uint8_t { COMPACT = 0, FULL = 1 };

  void setPrintMode(PrintMode m) { _printMode = m; }
  PrintMode printMode() const { return _printMode; }
  bool isFullMode() const { return _printMode == FULL; }

  struct Config {
    HardwareSerial* port = nullptr;
    int rxPin = 16;
    int txPin = -1; // RX-only
    uint32_t baud = 9600;

    // Timeout di parsing/pairing espressi in millisecondi.
    uint16_t interByteTimeoutMs = 50;
    uint16_t pairWindowMs = 200;
    uint16_t mdcesTimeoutMs = 1200;

    // Lunghezza massima ammessa per i frame catturati.
    uint8_t maxFrame = 64;
  };

  typedef void (*OnTransactionFn)(const CcTalkTransaction& t, uint16_t txCrc, void* user);
  typedef void (*OnMdcesFn)(uint8_t hostHdr, uint8_t addrByte, void* user);

  explicit CcTalkBusSniffer(const Config& cfg);

  // Alloca i buffer runtime e inizializza la UART di sniffing.
  void begin();
  // Poll non bloccante del parser.
  void loop();

  void onTransaction(OnTransactionFn fn, void* user);
  void onMdces(OnMdcesFn fn, void* user);

private:
  PrintMode _printMode = COMPACT;
  Config _cfg;

  uint8_t* _buf = nullptr;
  uint8_t  _pos = 0;
  uint8_t  _expected = 0;
  uint32_t _lastByteMs = 0;

  // Pairing richiesta/risposta.
  bool _havePendingReq = false;
  uint8_t* _pendingReq = nullptr;
  uint8_t  _pendingReqLen = 0;
  uint32_t _pendingReqMs = 0;
  bool     _pendingReqCrc16 = false;

  // Dedup dell'ultimo evento emesso.
  bool _lastCrcValid = false;
  uint16_t _lastCrc = 0;

  // Slot dedicato per richieste verso bill validator (addr 40-50).
  // I BV JCM rispondono con ritardo: la pending principale viene sovrascritta
  // dalle richieste agli hopper intermedie prima che il BV risponda.
  uint8_t* _lastBvReqBuf = nullptr;
  uint8_t  _lastBvReqLen = 0;
  uint32_t _lastBvReqMs  = 0;
  bool     _havePendingBvReq = false;
  bool     _lastBvReqCrc16 = false;

  // Stato temporaneo per il caso MDCES (richiesta frame + risposta 1 byte).
  bool _expectMdcesByte = false;
  uint32_t _mdcesSinceMs = 0;
  uint8_t _mdcesHostHdr = 0;

  // Callback utente.
  OnTransactionFn _onTx = nullptr;
  void* _onTxUser = nullptr;

  OnMdcesFn _onMdces = nullptr;
  void* _onMdcesUser = nullptr;

  void resetParser();
  void feedByte(uint8_t b);
  void handleFrame(const uint8_t* raw, uint8_t n, bool crc16);
  // Emette frame con checksum non valido come transazione anomala (checksumOk=false).
  void handleBadFrame(const uint8_t* raw, uint8_t n);

  // Heuristic semplici per distinguere richieste del master e risposte verso il master.
  bool isReqFromMaster(const uint8_t* raw) const;
  bool isRespToMaster(const uint8_t* raw) const;
};
