// Scopo del file:
// implementa il parser del flusso seriale ccTalk e il pairing richiesta/risposta.
#include "CcTalkBusSniffer.h"
#include "CcTalkUtils.h"

CcTalkBusSniffer::CcTalkBusSniffer(const Config& cfg) : _cfg(cfg) {}

void CcTalkBusSniffer::begin() {
  if (!_cfg.port) return;

  // Buffer separati:
  // - `_buf` per il frame attualmente in parsing
  // - `_pendingReq` per la richiesta in attesa di risposta
  _buf = (uint8_t*)malloc(_cfg.maxFrame);
  _pendingReq = (uint8_t*)malloc(_cfg.maxFrame);

  _cfg.port->begin(_cfg.baud, SERIAL_8N1, _cfg.rxPin, _cfg.txPin);
  resetParser();
}

void CcTalkBusSniffer::onTransaction(OnTransactionFn fn, void* user) {
  _onTx = fn;
  _onTxUser = user;
}

void CcTalkBusSniffer::onMdces(OnMdcesFn fn, void* user) {
  _onMdces = fn;
  _onMdcesUser = user;
}

bool CcTalkBusSniffer::isReqFromMaster(const uint8_t* raw) const {
  return raw && raw[2] == CCTALK_ADDR_MASTER; // src==1
}

bool CcTalkBusSniffer::isRespToMaster(const uint8_t* raw) const {
  return raw && raw[0] == CCTALK_ADDR_MASTER; // dest==1
}

static bool shouldSuppressDuplicate(CcTalkBusSniffer::PrintMode mode,
                                    bool lastCrcValid,
                                    uint16_t lastCrc,
                                    uint16_t currentCrc) {
  // In FULL mode la view 2 deve essere lossless: ogni frame letto sul bus
  // va inoltrato anche se identico al precedente.
  if (mode == CcTalkBusSniffer::FULL) return false;
  return lastCrcValid && currentCrc == lastCrc;
}

void CcTalkBusSniffer::resetParser() {
  // Non azzera il buffer: basta riportare gli indici allo stato iniziale.
  _pos = 0;
  _expected = 0;
}

void CcTalkBusSniffer::feedByte(uint8_t x) {
  if (!_buf) return;

  // Protezione contro frame corrotti o troppo lunghi.
  if (_pos >= _cfg.maxFrame) {
    resetParser();
    return;
  }

  _buf[_pos++] = x;

  // Validazione LEN quando arriva il 2° byte
  if (_pos == 2) {
    if (_buf[1] > (_cfg.maxFrame - 5)) { // 4 header + data + checksum
      resetParser();
      return;
    }
  }

  // expectedLen quando ho Dest/Len/Src/Hdr
  if (_pos == 4) {
    _expected = (uint8_t)(4 + _buf[1] + 1);
    if (_expected > _cfg.maxFrame) {
      resetParser();
      return;
    }
  }

  // Appena il frame e completo, il checksum ccTalk decide se l'evento va emesso.
  if (_expected && _pos == _expected) {
    if (cctalkChecksumOk(_buf, _expected)) {
      handleFrame(_buf, _expected);
    }
    resetParser();
  }
}

void CcTalkBusSniffer::handleFrame(const uint8_t* raw, uint8_t n) {
  if (!raw || n < 5) return;

  const uint8_t dest = raw[0];
  const uint8_t len  = raw[1];
  const uint8_t src  = raw[2];
  const uint8_t hdr  = raw[3];

  // MDCES: alcuni poll non ricevono un frame classico ma un singolo byte indirizzo.
  if (src == CCTALK_ADDR_MASTER && (hdr == 0xFD || hdr == 0xFC) && len == 0) {
    _expectMdcesByte = true;
    _mdcesSinceMs = millis();
    _mdcesHostHdr = hdr;
  }

  // Se è richiesta del master: la salvo e attendo risposta
  if (isReqFromMaster(raw)) {
    if (!_pendingReq) return;
    memcpy(_pendingReq, raw, n);
    _pendingReqLen = n;
    _pendingReqMs  = millis();
    _havePendingReq = true;
    return;
  }

  // Se è risposta al master: abbina con richiesta pending (address match)
  if (isRespToMaster(raw) && _havePendingReq) {
    const uint8_t reqDest = _pendingReq[0];
    const uint8_t respSrc = src;
    const bool addrMatch = (respSrc == reqDest);

    if ((millis() - _pendingReqMs) <= _cfg.pairWindowMs && addrMatch) {
      uint16_t crc = 0xFFFF;
      crc = crc16_frame(crc, _pendingReq, _pendingReqLen);
      crc = crc16_frame(crc, raw, n);

      if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
        CcTalkTransaction t;
        t.hasReq = true;
        t.hasResp = true;
        t.req.setFromRaw(_pendingReq, _pendingReqLen);
        t.resp.setFromRaw(raw, n);

        if (_onTx) _onTx(t, crc, _onTxUser);
      }

      _lastCrcValid = true;
      _lastCrc = crc;

      _havePendingReq = false;
      _pendingReqLen = 0;
      return;
    }

    // mismatch o timeout: scarta pending
    _havePendingReq = false;
    _pendingReqLen = 0;
  }

  // Frame orfano: puo capitare per perdita byte o traffico osservato a meta.
  // Viene comunque emesso come sola risposta per non perdere informazione.
  uint16_t crc = 0xFFFF;
  crc = crc16_frame(crc, raw, n);

  if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
    CcTalkTransaction t;
    t.hasReq = false;
    t.hasResp = true;
    t.resp.setFromRaw(raw, n);

    if (_onTx) _onTx(t, crc, _onTxUser);
  }

  _lastCrcValid = true;
  _lastCrc = crc;
}

void CcTalkBusSniffer::loop() {
  if (!_cfg.port) return;

  uint32_t now = millis();

  // Se il frame si interrompe troppo a lungo, il parser riparte da zero.
  if (_pos > 0 && (now - _lastByteMs) > _cfg.interByteTimeoutMs) {
    resetParser();
  }

  // Se il byte MDCES non arriva entro il timeout, si abbandona lo stato speciale.
  if (_expectMdcesByte && (now - _mdcesSinceMs) > _cfg.mdcesTimeoutMs) {
    _expectMdcesByte = false;
  }

  // Una richiesta senza risposta entro la finestra viene comunque emessa:
  // e un'informazione utile per debug di timeout o periferica assente.
  if (_havePendingReq && (now - _pendingReqMs) > _cfg.pairWindowMs) {
    uint16_t crc = 0xFFFF;
    crc = crc16_frame(crc, _pendingReq, _pendingReqLen);

    if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = false;
      t.req.setFromRaw(_pendingReq, _pendingReqLen);

      if (_onTx) _onTx(t, crc, _onTxUser);
    }

    _lastCrcValid = true;
    _lastCrc = crc;

    _havePendingReq = false;
    _pendingReqLen = 0;
  }

  while (_cfg.port->available()) {
    // risposta MDCES 1 byte
    if (_expectMdcesByte) {
      uint8_t a = (uint8_t)_cfg.port->read();
      _expectMdcesByte = false;
      resetParser();

      if (_onMdces) _onMdces(_mdcesHostHdr, a, _onMdcesUser);

      _lastByteMs = millis();
      continue;
    }

    uint8_t b = (uint8_t)_cfg.port->read();
    _lastByteMs = millis();
    feedByte(b);
  }
}
