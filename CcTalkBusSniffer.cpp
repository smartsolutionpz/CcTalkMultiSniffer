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
  _buf           = (uint8_t*)malloc(_cfg.maxFrame);
  _pendingReq    = (uint8_t*)malloc(_cfg.maxFrame);
  _lastBvReqBuf  = (uint8_t*)malloc(_cfg.maxFrame);

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

  // Appena il frame e completo, prova prima il checksum standard (1 byte),
  // poi il layout CRC-16 ccTalk. Il CRC non allunga il pacchetto: sostituisce
  // il source byte con il CRC LSB e usa l'ultimo byte come CRC MSB.
  if (_expected && _pos == _expected) {
    if (cctalkChecksumOk(_buf, _expected)) {
      handleFrame(_buf, _expected, false);
      resetParser();
    } else if (cctalkCrc16Ok(_buf, _expected)) {
      handleFrame(_buf, _expected, true);
      resetParser();
    } else {
      handleBadFrame(_buf, _expected);
      resetParser();
    }
    return;
  }
}

void CcTalkBusSniffer::handleFrame(const uint8_t* raw, uint8_t n, bool crc16) {
  if (!raw || n < 5) return;

  const uint8_t dest = raw[0];
  const uint8_t len  = raw[1];
  const uint8_t src  = raw[2];
  const uint8_t hdr  = raw[3];
  const bool reqFromMaster = crc16 ? (dest != CCTALK_ADDR_MASTER) : isReqFromMaster(raw);
  const bool respToMaster  = crc16 ? (dest == CCTALK_ADDR_MASTER) : isRespToMaster(raw);

  // MDCES: alcuni poll non ricevono un frame classico ma un singolo byte indirizzo.
  if (!crc16 && src == CCTALK_ADDR_MASTER && (hdr == 0xFD || hdr == 0xFC) && len == 0) {
    _expectMdcesByte = true;
    _mdcesSinceMs = millis();
    _mdcesHostHdr = hdr;
  }

  // Se è richiesta del master: la salvo e attendo risposta
  if (reqFromMaster) {
    // Se l'indirizzo di destinazione è un bill validator (40-50), salva anche
    // nello slot BV dedicato. I BV JCM rispondono in ritardo: la slot principale
    // viene sovrascritta dagli hopper prima che il BV risponda.
    if (_lastBvReqBuf && raw[0] >= 40 && raw[0] <= 50) {
      memcpy(_lastBvReqBuf, raw, n);
      _lastBvReqLen = n;
      _lastBvReqMs  = millis();
      _havePendingBvReq = true;
      _lastBvReqCrc16 = crc16;
    }
    if (!_pendingReq) return;
    memcpy(_pendingReq, raw, n);
    _pendingReqLen = n;
    _pendingReqMs  = millis();
    _havePendingReq = true;
    _pendingReqCrc16 = crc16;
    return;
  }

  // Se è risposta al master: abbina con richiesta pending (address match)
  if (respToMaster && _havePendingReq) {
    const uint8_t reqDest = _pendingReq[0];
    const uint8_t respSrc = crc16 ? reqDest : src;
    const bool pendingCompatible = crc16 ? _pendingReqCrc16 : true;
    const bool addrMatch = pendingCompatible && (crc16 || (respSrc == reqDest));

    if ((millis() - _pendingReqMs) <= _cfg.pairWindowMs && addrMatch) {
      uint16_t crc = 0xFFFF;
      crc = crc16_frame(crc, _pendingReq, _pendingReqLen);
      crc = crc16_frame(crc, raw, n);

      if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
        CcTalkTransaction t;
        t.hasReq = true;
        t.hasResp = true;
        if (_pendingReqCrc16) t.req.setFromRawCrc16(_pendingReq, _pendingReqLen, CCTALK_ADDR_MASTER);
        else                  t.req.setFromRaw(_pendingReq, _pendingReqLen);
        if (crc16) t.resp.setFromRawCrc16(raw, n, respSrc);
        else       t.resp.setFromRaw(raw, n);

        if (_onTx) _onTx(t, crc, _onTxUser);
      }

      _lastCrcValid = true;
      _lastCrc = crc;

      _havePendingReq = false;
      _pendingReqLen = 0;
      _pendingReqCrc16 = false;
      if (_havePendingBvReq && _lastBvReqBuf && _lastBvReqBuf[0] == reqDest) {
        _havePendingBvReq = false;
        _lastBvReqLen = 0;
        _lastBvReqCrc16 = false;
      }
      return;
    }

    // Mismatch o timeout: scarta la pending solo se appartiene allo stesso
    // formato della risposta. Una risposta CRC del BV non deve cancellare una
    // richiesta standard appena inviata a un hopper.
    if (!crc16 || _pendingReqCrc16) {
      _havePendingReq = false;
      _pendingReqLen = 0;
      _pendingReqCrc16 = false;
    }
  }

  if (respToMaster && crc16 && _havePendingBvReq && _lastBvReqCrc16 &&
      (millis() - _lastBvReqMs) <= (uint32_t)_cfg.pairWindowMs * 10) {
    const uint8_t respSrc = _lastBvReqBuf[0];
    uint16_t crc = 0xFFFF;
    crc = crc16_frame(crc, _lastBvReqBuf, _lastBvReqLen);
    crc = crc16_frame(crc, raw, n);

    if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
      CcTalkTransaction t;
      t.hasReq = true;
      t.hasResp = true;
      t.req.setFromRawCrc16(_lastBvReqBuf, _lastBvReqLen, CCTALK_ADDR_MASTER);
      t.resp.setFromRawCrc16(raw, n, respSrc);

      if (_onTx) _onTx(t, crc, _onTxUser);
    }

    _lastCrcValid = true;
    _lastCrc = crc;

    _havePendingBvReq = false;
    _lastBvReqLen = 0;
    _lastBvReqCrc16 = false;
    return;
  }

  // Frame orfano: puo capitare per perdita byte o traffico osservato a meta.
  // Viene comunque emesso come sola risposta per non perdere informazione.
  uint16_t crc = 0xFFFF;
  crc = crc16_frame(crc, raw, n);

  if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
    CcTalkTransaction t;
    t.hasReq = false;
    t.hasResp = true;
    if (crc16) t.resp.setFromRawCrc16(raw, n, 0);
    else       t.resp.setFromRaw(raw, n);

    if (_onTx) _onTx(t, crc, _onTxUser);
  }

  _lastCrcValid = true;
  _lastCrc = crc;
}

void CcTalkBusSniffer::handleBadFrame(const uint8_t* raw, uint8_t n) {
  if (!raw || n < 5 || !_onTx) return;

  // Formato JCM (iPRO-100 e derivati):
  //   [ADDR_DEV][LEN][SEQ][HDR_ECHO][DATA...][CHK_JCM][0x01]
  // La risposta mette l'indirizzo del device come primo byte e il marker
  // 0x01 (CCTALK_ADDR_MASTER) come ultimo, invertendo la convenzione standard.
  const bool isJcmResp = (n >= 6) && (raw[n - 1] == CCTALK_ADDR_MASTER);

  if (isJcmResp) {
    // 1° tentativo: slot principale (BV ha risposto prima che il master interrogasse altri)
    const uint8_t* matchBuf = nullptr;
    uint8_t        matchLen = 0;
    bool*          matchFlag = nullptr;
    uint8_t*       matchLenPtr = nullptr;
    bool           matchCrc16 = false;

    if (_havePendingReq && _pendingReq && _pendingReq[0] == raw[0]
        && (millis() - _pendingReqMs) <= _cfg.pairWindowMs) {
      matchBuf    = _pendingReq;
      matchLen    = _pendingReqLen;
      matchFlag   = &_havePendingReq;
      matchLenPtr = &_pendingReqLen;
      matchCrc16  = _pendingReqCrc16;
    }
    // 2° tentativo: slot BV dedicato (BV ha risposto in ritardo, dopo che la slot
    // principale era già stata sovrascritta dalle richieste agli hopper intermedi)
    else if (_havePendingBvReq && _lastBvReqBuf && _lastBvReqBuf[0] == raw[0]
             && (millis() - _lastBvReqMs) <= (uint32_t)_cfg.pairWindowMs * 10) {
      matchBuf    = _lastBvReqBuf;
      matchLen    = _lastBvReqLen;
      matchFlag   = &_havePendingBvReq;
      matchLenPtr = &_lastBvReqLen;
      matchCrc16  = _lastBvReqCrc16;
    }

    if (matchBuf) {
      uint16_t crc = 0xFFFF;
      crc = crc16_frame(crc, matchBuf, matchLen);
      crc = crc16_frame(crc, raw, n);

      if (!shouldSuppressDuplicate(_printMode, _lastCrcValid, _lastCrc, crc)) {
        CcTalkTransaction t;
        t.hasReq     = true;
        t.hasResp    = true;
        t.checksumOk = false;
        if (matchCrc16) t.req.setFromRawCrc16(matchBuf, matchLen, CCTALK_ADDR_MASTER);
        else            t.req.setFromRaw(matchBuf, matchLen);
        t.resp.setFromRaw(raw, n);
        _onTx(t, crc, _onTxUser);
      }

      _lastCrcValid  = true;
      _lastCrc       = crc;
      *matchFlag     = false;
      *matchLenPtr   = 0;
      if (matchFlag == &_havePendingReq) _pendingReqCrc16 = false;
      if (matchFlag == &_havePendingBvReq) _lastBvReqCrc16 = false;
      return;
    }
  }

  // Frame orfano: non JCM, o non abbinabile a nessuna richiesta pendente.
  CcTalkTransaction t;
  t.hasReq     = false;
  t.hasResp    = true;
  t.checksumOk = false;
  t.resp.setFromRaw(raw, n);
  _onTx(t, 0, _onTxUser);
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
      if (_pendingReqCrc16) t.req.setFromRawCrc16(_pendingReq, _pendingReqLen, CCTALK_ADDR_MASTER);
      else                  t.req.setFromRaw(_pendingReq, _pendingReqLen);

      if (_onTx) _onTx(t, crc, _onTxUser);
    }

    _lastCrcValid = true;
    _lastCrc = crc;

    _havePendingReq = false;
    _pendingReqLen = 0;
    _pendingReqCrc16 = false;
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
