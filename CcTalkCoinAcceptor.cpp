// Scopo del file:
// implementa la decodifica testuale delle richieste e risposte della gettoniera.
#include "CcTalkCoinAcceptor.h"
#include "CcTalkMaster.h"
#include "CcTalkUtils.h"
#include <string.h>

namespace {
void printByteHex(Stream& out, uint8_t value) {
  out.print(F("0x"));
  if (value < 16) out.print('0');
  out.print(value, HEX);
}

void printValueAsEuro(Stream& out, uint32_t cents) {
  const uint32_t eur = cents / 100;
  const uint8_t rem = (uint8_t)(cents % 100);
  out.print(eur);
  out.print('.');
  if (rem < 10) out.print('0');
  out.print(rem);
  out.print(F(" EUR"));
}

void copyAsciiSanitized(char* out, size_t outLen, const uint8_t* data, uint8_t len) {
  if (!out || outLen == 0) return;

  size_t n = len;
  if (n > outLen - 1) n = outLen - 1;
  for (size_t i = 0; i < n; i++) {
    const char c = (char)data[i];
    out[i] = (c >= 32 && c <= 126) ? c : '.';
  }
  out[n] = '\0';
}
}

CcTalkCoinAcceptor::CcTalkCoinAcceptor(const CoinAcceptorDataset& dataset)
  : _dataset(dataset) {
  resetState();
}

void CcTalkCoinAcceptor::resetState() {
  const uint8_t activeValueProfile = _state.activeValueProfile;
  _state = CoinAcceptorState();
  _state.activeValueProfile = activeValueProfile;
}

const CcTalkCoinAcceptor::CoinAcceptorState* CcTalkCoinAcceptor::stateFor(uint8_t addr) const {
  if (!matches(addr)) return nullptr;
  return &_state;
}

void CcTalkCoinAcceptor::setValueProfile(uint8_t profileId) {
  _state.activeValueProfile = profileId;
  for (uint8_t coinType = 1; coinType <= (sizeof(_state.coinIds) / sizeof(_state.coinIds[0])); coinType++) {
    applyPendingCreditsForCoinType(coinType);
  }
}

const __FlashStringHelper* CcTalkCoinAcceptor::cmdDesc(uint8_t hdr) const {
  // Per i comandi noti della gettoniera preferiamo una descrizione dedicata;
  // per il resto ricadiamo sul catalogo generico del master.
  switch (hdr) {
    case 0xF9: return F("request polling priority");
    case 0xF8: return F("request status");
    case 0xF0: return F("test solenoids");
    case 0xEE: return F("test output lines");
    case 0xEC: return F("read opto states");
    case 0xE8: return F("perform self test");
    case 0xE7: return F("modify inhibit status");
    case 0xE6: return F("request inhibit status");
    case 0xE5: return F("read buffered credit/error codes");
    case 0xE4: return F("modify master inhibit status");
    case 0xE3: return F("request master inhibit status");
    case 0xDE: return F("modify sorter override status");
    case 0xDD: return F("request sorter override status");
    case 0xD5: return F("request option flags");
    case 0xD2: return F("modify sorter paths");
    case 0xD1: return F("request sorter paths");
    case 0xCA: return F("teach mode control");
    case 0xC9: return F("request teach status");
    case 0xBD: return F("modify default sorter path");
    case 0xBC: return F("request default sorter path");
    case 0xB9: return F("modify coin ID");
    case 0xB8: return F("request coin ID");
    default:   return CcTalkMaster::headerDesc(hdr);
  }
}

const char* CcTalkCoinAcceptor::statusLabel(uint8_t code) const {
  const char* label = coinAcceptorLookupLabel(_dataset.statusLabels, _dataset.statusLabelCount, code);
  return label ? label : "status non mappato";
}

const char* CcTalkCoinAcceptor::errorLabel(uint8_t code) const {
  const char* label = coinAcceptorLookupLabel(_dataset.errorLabels, _dataset.errorLabelCount, code);
  return label ? label : "errore non mappato";
}

uint8_t CcTalkCoinAcceptor::eventCounterDelta(uint8_t previous, uint8_t current) const {
  if (current == previous) return 0;
  if (current > previous) return (uint8_t)(current - previous);
  if (previous > 220 && current < 32) {
    // I counter buffered ccTalk della gettoniera sono tipicamente 1..255:
    // dopo 255 il device torna a 1, non passa per 0 come evento valido.
    return (uint8_t)(current + (uint8_t)(255 - previous));
  }
  return 0;
}

bool CcTalkCoinAcceptor::parseCoinIdValueCents(const uint8_t* data, uint8_t len, uint16_t& valueCents) const {
  valueCents = 0;
  if (!data || len == 0) return false;

  // ccTalk coin ID standard: CCVVVI
  // esempio dal manuale Falcon: "EU200A" = 2 EUR = 200 cent.
  if (len >= 5) {
    const char c0 = (char)data[0];
    const char c1 = (char)data[1];
    const char v0 = (char)data[2];
    const char v1 = (char)data[3];
    const char v2 = (char)data[4];
    const bool hasCountryCode =
        ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) &&
        ((c1 >= 'A' && c1 <= 'Z') || (c1 >= 'a' && c1 <= 'z'));
    const bool hasValueCode =
        (v0 >= '0' && v0 <= '9') &&
        (v1 >= '0' && v1 <= '9') &&
        (v2 >= '0' && v2 <= '9');
    if (hasCountryCode && hasValueCode) {
      valueCents = (uint16_t)(((uint16_t)(v0 - '0') * 100U) +
                              ((uint16_t)(v1 - '0') * 10U) +
                              (uint16_t)(v2 - '0'));
      return valueCents > 0;
    }
  }

  uint32_t integerPart = 0;
  uint32_t fractionPart = 0;
  uint8_t fractionDigits = 0;
  bool inNumber = false;
  bool seenDecimal = false;
  bool seenDigit = false;

  for (uint8_t i = 0; i < len; i++) {
    const char c = (char)data[i];
    if (c >= '0' && c <= '9') {
      seenDigit = true;
      inNumber = true;
      if (!seenDecimal) {
        integerPart = (integerPart * 10UL) + (uint32_t)(c - '0');
      } else if (fractionDigits < 2) {
        fractionPart = (fractionPart * 10UL) + (uint32_t)(c - '0');
        fractionDigits++;
      }
      continue;
    }

    if ((c == '.' || c == ',') && inNumber && !seenDecimal) {
      seenDecimal = true;
      continue;
    }

    if (inNumber) break;
  }

  if (!seenDigit) return false;

  if (fractionDigits == 1) fractionPart *= 10UL;
  const uint32_t totalCents = integerPart * 100UL + fractionPart;
  if (totalCents > 65535UL) return false;

  valueCents = (uint16_t)totalCents;
  return true;
}

void CcTalkCoinAcceptor::applyPendingCreditsForCoinType(uint8_t coinType) {
  if (coinType < 1 || coinType > (sizeof(_state.coinIds) / sizeof(_state.coinIds[0]))) return;

  const uint8_t idx = (uint8_t)(coinType - 1);
  const uint8_t pendingCount = _state.pendingUnknownCreditCount[idx];
  uint16_t valueCents = 0;
  if (pendingCount == 0 || !resolveCoinValueCents(coinType, valueCents) || valueCents == 0) return;

  _state.acceptedTotalCents += (uint32_t)pendingCount * (uint32_t)valueCents;
  _state.lastAcceptedValid = true;
  _state.lastAcceptedCoinType = coinType;
  _state.lastAcceptedValueCents = valueCents;
  _state.pendingUnknownCreditCount[idx] = 0;
}

bool CcTalkCoinAcceptor::resolveCoinValueCents(uint8_t coinType, uint16_t& valueCents) const {
  valueCents = 0;
  if (coinType < 1 || coinType > (sizeof(_state.coinIds) / sizeof(_state.coinIds[0]))) return false;

  const CoinIdState& coin = _state.coinIds[coinType - 1];
  if (coin.valid && coin.valueCents > 0) {
    valueCents = coin.valueCents;
    return true;
  }

  return coinAcceptorLookupStaticValueCents(_dataset,
                                            _state.activeValueProfile,
                                            coinType,
                                            valueCents);
}

void CcTalkCoinAcceptor::updateState(const CcTalkTransaction& t) {
  if (!t.hasReq && !t.hasResp) return;

  _state.present = true;
  _state.addr = 2;

  if (!t.hasReq || !t.hasResp) return;
  if (t.resp.hdr != 0x00) return;

  const CcTalkFrame& req = t.req;
  const CcTalkFrame& resp = t.resp;

  switch (req.hdr) {
    case 0x01:
      {
        const uint8_t activeValueProfile = _state.activeValueProfile;
        const uint32_t acceptedTotalCents = _state.acceptedTotalCents;
        const bool lastAcceptedValid = _state.lastAcceptedValid;
        const uint8_t lastAcceptedCoinType = _state.lastAcceptedCoinType;
        const uint16_t lastAcceptedValueCents = _state.lastAcceptedValueCents;
        uint8_t pendingUnknownCreditCount[16];
        CoinIdState coinIds[16];
        memcpy(pendingUnknownCreditCount,
               _state.pendingUnknownCreditCount,
               sizeof(pendingUnknownCreditCount));
        memcpy(coinIds, _state.coinIds, sizeof(coinIds));

        _state = CoinAcceptorState();
        _state.activeValueProfile = activeValueProfile;
        memcpy(_state.pendingUnknownCreditCount,
               pendingUnknownCreditCount,
               sizeof(_state.pendingUnknownCreditCount));
        memcpy(_state.coinIds, coinIds, sizeof(_state.coinIds));
        _state.acceptedTotalCents = acceptedTotalCents;
        _state.lastAcceptedValid = lastAcceptedValid;
        _state.lastAcceptedCoinType = lastAcceptedCoinType;
        _state.lastAcceptedValueCents = lastAcceptedValueCents;
      }
      _state.present = true;
      _state.addr = 2;
      return;

    case 0xB8:
      if (req.dataLen == 1 && resp.dataLen == 6) {
        const uint8_t idx = req.data[0];
        if (idx >= 1 && idx <= (sizeof(_state.coinIds) / sizeof(_state.coinIds[0]))) {
          CoinIdState& entry = _state.coinIds[idx - 1];
          copyAsciiSanitized(entry.id, sizeof(entry.id), resp.data, resp.dataLen);
          entry.valid = parseCoinIdValueCents(resp.data, resp.dataLen, entry.valueCents);
          applyPendingCreditsForCoinType(idx);
        }
      }
      return;

    case 0xB9:
      if (req.dataLen == 7) {
        const uint8_t idx = req.data[0];
        if (idx >= 1 && idx <= (sizeof(_state.coinIds) / sizeof(_state.coinIds[0]))) {
          CoinIdState& entry = _state.coinIds[idx - 1];
          copyAsciiSanitized(entry.id, sizeof(entry.id), &req.data[1], 6);
          entry.valid = parseCoinIdValueCents(&req.data[1], 6, entry.valueCents);
          applyPendingCreditsForCoinType(idx);
        }
      }
      return;

    case 0xE5:
      if (resp.dataLen == 11) {
        _state.eventCounter = resp.data[0];
        _state.eventCounterValid = true;
        _state.lastBufferedPollDiagnosticsValid = true;
        _state.lastBufferedPollDelta = 0;
        for (uint8_t i = 0; i < 5; i++) {
          _state.lastBufferedCredits[i] = BufferedCreditTrace();
        }

        if (_state.eventCounterSeen) {
          uint8_t delta = eventCounterDelta(_state.lastProcessedEventCounter, resp.data[0]);
          if (delta > 5) delta = 5;
          _state.lastBufferedPollDelta = delta;

          for (uint8_t i = 0; i < delta; i++) {
            const uint8_t a = resp.data[1 + (i * 2)];
            const uint8_t b = resp.data[2 + (i * 2)];
            if (a == 0) continue;

            BufferedCreditTrace& trace = _state.lastBufferedCredits[i];
            trace.valid = true;
            trace.isCredit = true;
            trace.coinType = a;
            trace.sorterPathOrError = b;

            if (a > (sizeof(_state.coinIds) / sizeof(_state.coinIds[0]))) {
              trace.accountingCode = BUFFERED_ACCOUNTING_SKIPPED_TYPE_OUT_OF_RANGE;
              continue;
            }

            uint16_t valueCents = 0;
            if (!resolveCoinValueCents(a, valueCents) || valueCents == 0) {
              if (_state.pendingUnknownCreditCount[a - 1] < 255) {
                _state.pendingUnknownCreditCount[a - 1]++;
              }
              trace.accountingCode = BUFFERED_ACCOUNTING_SKIPPED_UNKNOWN_COIN_ID;
              continue;
            }

            _state.acceptedTotalCents += valueCents;
            _state.lastAcceptedValid = true;
            _state.lastAcceptedCoinType = a;
            _state.lastAcceptedValueCents = valueCents;
            trace.accountingCode = BUFFERED_ACCOUNTING_COUNTED;
            trace.valueCents = valueCents;
          }
        } else {
          _state.lastBufferedPollDelta = 5;
          for (uint8_t i = 0; i < 5; i++) {
            const uint8_t a = resp.data[1 + (i * 2)];
            const uint8_t b = resp.data[2 + (i * 2)];
            if (a == 0) continue;

            BufferedCreditTrace& trace = _state.lastBufferedCredits[i];
            trace.valid = true;
            trace.isCredit = true;
            trace.coinType = a;
            trace.sorterPathOrError = b;
            trace.accountingCode = BUFFERED_ACCOUNTING_SKIPPED_FIRST_POLL;
          }
        }

        _state.lastProcessedEventCounter = resp.data[0];
        _state.eventCounterSeen = true;
      }
      return;

    default:
      return;
  }
}

void CcTalkCoinAcceptor::dumpState(Stream& out) const {
  if (!_state.present) return;

  out.println(F("[STATE] GETTONIERA"));
  out.print(F("  addr="));
  out.print(_state.addr);
  out.print(F(" eventCounter="));
  if (_state.eventCounterValid) out.println(_state.eventCounter);
  else out.println(F("n/a"));
  out.print(F("    eventCounterSeen="));
  out.println(_state.eventCounterSeen ? F("yes") : F("no"));
  out.print(F("    valueProfile="));
  const CoinAcceptorValueProfile* profile =
      coinAcceptorLookupValueProfile(_dataset, _state.activeValueProfile);
  if (profile) {
    out.print(profile->label);
    out.print(F(" (id="));
    out.print(profile->id);
    out.println(F(")"));
  } else {
    out.println(F("n/a"));
  }
  out.print(F("    lastProcessedEventCounter="));
  out.println(_state.lastProcessedEventCounter);
  out.print(F("    acceptedTotalCents="));
  out.print(_state.acceptedTotalCents);
  out.print(F(" ("));
  printValueAsEuro(out, _state.acceptedTotalCents);
  out.println(F(")"));
  if (_state.lastAcceptedValid) {
    out.print(F("    lastAccepted coinType="));
    out.print(_state.lastAcceptedCoinType);
    out.print(F(" valueCents="));
    out.print(_state.lastAcceptedValueCents);
    out.print(F(" ("));
    printValueAsEuro(out, _state.lastAcceptedValueCents);
    out.println(F(")"));
  }
  if (_state.lastBufferedPollDiagnosticsValid) {
    out.print(F("    lastBufferedPollDelta="));
    out.println(_state.lastBufferedPollDelta);
    for (uint8_t i = 0; i < 5; i++) {
      const BufferedCreditTrace& trace = _state.lastBufferedCredits[i];
      if (!trace.valid) continue;
      out.print(F("    buffered["));
      out.print((uint8_t)(i + 1));
      out.print(F("] coinType="));
      out.print(trace.coinType);
      out.print(F(" sorterPath="));
      out.print(trace.sorterPathOrError);
      out.print(F(" -> "));
      switch (trace.accountingCode) {
        case BUFFERED_ACCOUNTING_COUNTED:
          out.print(F("credit counted"));
          out.print(F(" valueCents="));
          out.print(trace.valueCents);
          out.print(F(" ("));
          printValueAsEuro(out, trace.valueCents);
          out.println(F(")"));
          break;
        case BUFFERED_ACCOUNTING_SKIPPED_FIRST_POLL:
          out.println(F("credit skipped: first buffered poll (baseline)"));
          break;
        case BUFFERED_ACCOUNTING_SKIPPED_TYPE_OUT_OF_RANGE:
          out.println(F("credit skipped: coinType out of range"));
          break;
        case BUFFERED_ACCOUNTING_SKIPPED_UNKNOWN_COIN_ID:
          out.println(F("credit pending: unknown coin id/value"));
          break;
        default:
          out.println(F("credit skipped"));
          break;
      }
    }
  }
  for (uint8_t i = 0; i < (sizeof(_state.coinIds) / sizeof(_state.coinIds[0])); i++) {
    if (_state.pendingUnknownCreditCount[i] > 0) {
      out.print(F("    pendingCredits coinType="));
      out.print((uint8_t)(i + 1));
      out.print(F(" count="));
      out.println(_state.pendingUnknownCreditCount[i]);
    }
    const CoinIdState& coin = _state.coinIds[i];
    if (!coin.valid) continue;
    out.print(F("    coin["));
    out.print((uint8_t)(i + 1));
    out.print(F("] id=\""));
    out.print(coin.id);
    out.print(F("\" valueCents="));
    out.print(coin.valueCents);
    out.print(F(" ("));
    printValueAsEuro(out, coin.valueCents);
    out.println(F(")"));
  }
}

void CcTalkCoinAcceptor::printRequest(Stream& out, const CcTalkFrame& req) {
  out.print(F("MASTER -> GETTONIERA: "));
  out.print(cmdDesc(req.hdr));
  out.print(F(" (0x"));
  if (req.hdr < 16) out.print('0');
  out.print(req.hdr, HEX);
  out.println(F(")"));
  printRequestPayload(out, req);
}

void CcTalkCoinAcceptor::printRequestPayload(Stream& out, const CcTalkFrame& req) {
  switch (req.hdr) {
    case 0xE7:
      if (req.dataLen == 2) {
        out.print(F("  inhibitMask low="));
        printByteHex(out, req.data[0]);
        out.print(F(" high="));
        printByteHex(out, req.data[1]);
        out.println();
      }
      return;

    case 0xE4:
    case 0xDE:
    case 0xBD:
    case 0xCA:
    case 0xF0:
      if (req.dataLen == 1) {
        out.print(F("  payload="));
        printByteHex(out, req.data[0]);
        out.println();
      }
      return;

    case 0xD2:
      out.print(F("  sorter paths: "));
      dumpHex(out, req.data, req.dataLen);
      out.println();
      return;

    case 0xD1:
    case 0xB8:
      if (req.dataLen == 1) {
        out.print(F("  coin position="));
        out.println(req.data[0]);
      }
      return;

    case 0xB9:
      if (req.dataLen == 7) {
        out.print(F("  coin position="));
        out.print(req.data[0]);
        out.print(F(" id=\""));
        dumpAscii(out, &req.data[1], 6);
        out.println('"');
      }
      return;

    default:
      return;
  }
}

void CcTalkCoinAcceptor::printBufferedEvent(Stream& out, uint8_t idx, uint8_t a, uint8_t b) const {
  out.print(F("  ev"));
  out.print(idx);
  out.print(F(": A="));
  out.print(a);
  out.print(F(" B="));
  out.print(b);

  if (a == 0) {
    out.print(F(" -> "));
    out.print(errorLabel(b));
  } else {
    out.print(F(" -> credit coinType="));
    out.print(a);
    out.print(F(" sorterPath="));
    out.print(b);
  }

  out.println();
  printBufferedAccounting(out, idx);
}

void CcTalkCoinAcceptor::printBufferedAccounting(Stream& out, uint8_t idx) const {
  if (idx < 1 || idx > 5) return;

  const BufferedCreditTrace& trace = _state.lastBufferedCredits[idx - 1];
  if (!trace.valid || !trace.isCredit) return;

  out.print(F("     accounting: "));
  switch (trace.accountingCode) {
    case BUFFERED_ACCOUNTING_COUNTED:
      out.print(F("credit counted"));
      out.print(F(" valueCents="));
      out.print(trace.valueCents);
      out.print(F(" ("));
      printValueAsEuro(out, trace.valueCents);
      out.println(F(")"));
      return;
    case BUFFERED_ACCOUNTING_SKIPPED_FIRST_POLL:
      out.println(F("credit skipped: first buffered poll (baseline)"));
      return;
    case BUFFERED_ACCOUNTING_SKIPPED_TYPE_OUT_OF_RANGE:
      out.println(F("credit skipped: coinType out of range"));
      return;
    case BUFFERED_ACCOUNTING_SKIPPED_UNKNOWN_COIN_ID:
      out.println(F("credit pending: unknown coin id/value"));
      return;
    default:
      out.println(F("credit skipped"));
      return;
  }
}

void CcTalkCoinAcceptor::printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp) {
  out.print(F("GETTONIERA -> MASTER: "));

  if (resp.hdr == 0x05) { out.println(F("NAK")); return; }
  if (resp.hdr == 0x06) { out.println(F("BUSY")); return; }

  if (resp.hdr != 0x00) {
    out.print(F("HDR=0x"));
    if (resp.hdr < 16) out.print('0');
    out.println(resp.hdr, HEX);
    return;
  }

  if (hostHdr == 0xFE || hostHdr == 0x01 || hostHdr == 0xE7 || hostHdr == 0xE4 ||
      hostHdr == 0xDE || hostHdr == 0xD2 || hostHdr == 0xCA || hostHdr == 0xBD ||
      hostHdr == 0xB9 || hostHdr == 0xF0) {
    out.println(F("ACK"));
    return;
  }

  if (hostHdr == 0xF6 || hostHdr == 0xF5 || hostHdr == 0xF4 || hostHdr == 0xF1 ||
      hostHdr == 0xC0 || hostHdr == 0xFF || hostHdr == 0xF3) {
    out.print('"');
    dumpAscii(out, resp.data, resp.dataLen);
    out.println('"');
    return;
  }

  if (hostHdr == 0xF2 && resp.dataLen == 3) {
    out.print(F("Serial="));
    out.println(readU24LE(resp.data));
    return;
  }

  if (hostHdr == 0x04 && resp.dataLen == 3) {
    out.print(F("CommsRevision level="));
    out.print(resp.data[0]);
    out.print(F(" major="));
    out.print(resp.data[1]);
    out.print(F(" minor="));
    out.println(resp.data[2]);
    return;
  }

  if (hostHdr == 0xF9 && resp.dataLen == 2) {
    out.print(F("PollingPriority="));
    out.print(resp.data[0]);
    out.print(F(", "));
    out.print(resp.data[1]);
    if (resp.data[0] == 20 && resp.data[1] == 0) {
      out.print(F(" (~200ms)"));
    }
    out.println();
    return;
  }

  if (hostHdr == 0xF8 && resp.dataLen == 1) {
    out.print(F("Status="));
    out.print(resp.data[0]);
    out.print(F(" ("));
    out.print(statusLabel(resp.data[0]));
    out.println(F(")"));
    return;
  }

  if (hostHdr == 0xEE && resp.dataLen == 1) {
    out.print(F("OutputLines="));
    printByteHex(out, resp.data[0]);
    out.println();
    return;
  }

  if (hostHdr == 0xEC && resp.dataLen == 1) {
    out.print(F("OptoStates="));
    printByteHex(out, resp.data[0]);
    out.println();
    return;
  }

  if (hostHdr == 0xE8 && resp.dataLen == 1) {
    out.print(F("SelfTest="));
    out.print(resp.data[0]);
    if (resp.data[0] == 0) out.print(F(" (no error)"));
    else if (resp.data[0] == 1) out.print(F(" (EEPROM checksum fault)"));
    out.println();
    return;
  }

  if (hostHdr == 0xE6 && resp.dataLen == 2) {
    out.print(F("InhibitMask low="));
    printByteHex(out, resp.data[0]);
    out.print(F(" high="));
    printByteHex(out, resp.data[1]);
    out.println();
    return;
  }

  if (hostHdr == 0xE5 && resp.dataLen == 11) {
    out.print(F("eventCounter="));
    out.println(resp.data[0]);
    for (uint8_t i = 0; i < 5; i++) {
      const uint8_t a = resp.data[1 + (i * 2)];
      const uint8_t b = resp.data[2 + (i * 2)];
      printBufferedEvent(out, (uint8_t)(i + 1), a, b);
    }
    return;
  }

  if (hostHdr == 0xE3 && resp.dataLen == 1) {
    out.print(F("MasterInhibit="));
    out.println((resp.data[0] & 0x01) ? F("OFF (enabled)") : F("ON (blocked)"));
    return;
  }

  if (hostHdr == 0xDD && resp.dataLen == 1) {
    out.print(F("SorterOverride="));
    printByteHex(out, resp.data[0]);
    out.println();
    return;
  }

  if (hostHdr == 0xD5 && resp.dataLen == 1) {
    out.print(F("OptionFlags="));
    printByteHex(out, resp.data[0]);
    out.print(F(" remoteTeach="));
    out.print((resp.data[0] & 0x80) ? F("yes") : F("no"));
    out.print(F(" escrowControl="));
    out.print((resp.data[0] & 0x40) ? F("yes") : F("no"));
    out.print(F(" returnLeverMotor="));
    out.println((resp.data[0] & 0x20) ? F("yes") : F("no"));
    return;
  }

  if (hostHdr == 0xD1) {
    if (resp.dataLen == 4) {
      out.print(F("SorterPaths: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;
    }
    if (resp.dataLen == 1) {
      out.print(F("SorterPath="));
      out.println(resp.data[0]);
      return;
    }
  }

  if (hostHdr == 0xC9 && resp.dataLen == 2) {
    out.print(F("TeachStatus inserted="));
    out.print(resp.data[0]);
    out.print(F(" status="));
    out.print(resp.data[1]);
    switch (resp.data[1]) {
      case 252: out.print(F(" (aborted)")); break;
      case 253: out.print(F(" (error)")); break;
      case 254: out.print(F(" (in process)")); break;
      case 255: out.print(F(" (completed)")); break;
      default: break;
    }
    out.println();
    return;
  }

  if (hostHdr == 0xBC && resp.dataLen == 1) {
    out.print(F("DefaultSorterPath="));
    out.println(resp.data[0]);
    return;
  }

  if (hostHdr == 0xB8 && resp.dataLen == 6) {
    out.print(F("CoinID=\""));
    dumpAscii(out, resp.data, 6);
    out.println('"');
    return;
  }

  out.print(F("ACK data: "));
  dumpHex(out, resp.data, resp.dataLen);
  out.println();
}

void CcTalkCoinAcceptor::onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) {
  (void)printRaw;
  updateState(t);
  // Il decoder aggiorna uno stato runtime minimale e produce una vista
  // testuale coerente della transazione corrente usando il dataset concreto.
  out.println();

  uint8_t hostHdr = 0;
  if (t.hasReq) {
    hostHdr = t.req.hdr;
    printRequest(out, t.req);
  } else {
    out.println(F("MASTER -> GETTONIERA: (richiesta assente)"));
  }

  if (t.hasResp) printResponse(out, hostHdr, t.resp);
  else out.println(F("GETTONIERA -> MASTER: (nessuna risposta)"));
}
