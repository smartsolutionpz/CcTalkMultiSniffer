// Scopo del file:
// implementa la decodifica e la memoria runtime degli hopper osservati sul bus.
#include "CcTalkHopper.h"
#include "CcTalkMaster.h"
#include "CcTalkUtils.h"
#include <string.h>

namespace {
// Copia testo proveniente dal device rendendo innocui i byte non stampabili.
void copyAsciiSanitized(char* dst, size_t dstSize, const uint8_t* src, uint8_t srcLen) {
  if (!dst || dstSize == 0) return;

  size_t n = srcLen;
  if (n > (dstSize - 1)) n = (dstSize - 1);

  for (size_t i = 0; i < n; i++) {
    const uint8_t c = src[i];
    dst[i] = (c >= 32 && c <= 126) ? (char)c : '.';
  }
  dst[n] = '\0';
}

uint16_t readU16BE(const uint8_t* d) {
  if (!d) return 0;
  return (uint16_t)(((uint16_t)d[0] << 8) | (uint16_t)d[1]);
}
}

CcTalkHopper::CcTalkHopper(const HopperDataset& dataset)
  : _dataset(dataset) {
  resetState();
}

bool CcTalkHopper::addressEnabled(uint8_t addr) const {
  if (addr < kAddrMin || addr > kAddrMax) return false;
  return (_addressMask & (uint8_t)(1u << (addr - kAddrMin))) != 0;
}

bool CcTalkHopper::matches(uint8_t addr) const {
  return addressEnabled(addr);
}

void CcTalkHopper::setAddressMask(uint8_t mask) {
  _addressMask = (uint8_t)(mask & ((1u << kStateCount) - 1u));
}

void CcTalkHopper::setConfiguredCoinValueCents(uint8_t addr, uint16_t valueCents) {
  if (addr < kAddrMin || addr > kAddrMax) return;
  _configuredCoinValueCents[(uint8_t)(addr - kAddrMin)] = valueCents;
}

void CcTalkHopper::resetState() {
  // Inizializzazione deterministica degli slot hopper 3..10.
  memset(_states, 0, sizeof(_states));
  for (uint8_t i = 0; i < kStateCount; i++) {
    _states[i].addr = (uint8_t)(kAddrMin + i);
  }
}

CcTalkHopper::HopperState* CcTalkHopper::mutableStateFor(uint8_t addr) {
  if (!addressEnabled(addr)) return nullptr;
  return &_states[(uint8_t)(addr - kAddrMin)];
}

const CcTalkHopper::HopperState* CcTalkHopper::stateFor(uint8_t addr) const {
  if (!addressEnabled(addr)) return nullptr;
  return &_states[(uint8_t)(addr - kAddrMin)];
}

void CcTalkHopper::updateState(const CcTalkTransaction& t) {
  // Come nel BV, il decoder hopper usa il traffico sniffato per costruire
  // una memoria runtime incrementale del device.
  if (!t.hasReq && !t.hasResp) return;

  const uint8_t addr = t.hasReq ? t.req.dest : t.resp.src;
  HopperState* state = mutableStateFor(addr);
  if (!state) return;

  state->present = true;

  // Lo stato viene aggiornato solo se abbiamo una transazione completa con ACK.
  if (!t.hasReq || !t.hasResp) return;
  if (t.resp.hdr != 0x00) return;

  const CcTalkFrame& req = t.req;
  const CcTalkFrame& resp = t.resp;

  switch (req.hdr) {
    case 0xF6:
      copyAsciiSanitized(state->manufacturer, sizeof(state->manufacturer), resp.data, resp.dataLen);
      state->manufacturerValid = true;
      return;

    case 0xF4:
      copyAsciiSanitized(state->productCode, sizeof(state->productCode), resp.data, resp.dataLen);
      state->productCodeValid = true;
      return;

    case 0xF2:
      if (resp.dataLen == 3) {
        state->serial = readU24LE(resp.data);
        state->serialValid = true;
      }
      return;

    case 0xFF:
      copyAsciiSanitized(state->extendedId, sizeof(state->extendedId), resp.data, resp.dataLen);
      state->extendedIdValid = true;
      return;

    case 0x01:
      state->resetCount++;
      state->eventCounterValid = false;
      state->azkoyenHopperStatusValid = false;
      state->azkoyenHopperStatusCounterSeen = false;
      state->azkoyenType1Remaining = 0;
      state->azkoyenLastType1PaidByA6 = 0;
      state->azkoyenLastType1UnpaidByA6 = 0;
      state->azkoyenProgressAccountedValid = false;
      state->azkoyenRequestBaseUnits = 0;
      state->pollSnapshotValid = false;
      state->lastDispenseStepValid = false;
      return;

    case 0x10:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
          resp.dataLen == 5) {
        state->azkoyenDeviceType = resp.data[0];
        state->azkoyenSoftwareMajor = resp.data[1];
        state->azkoyenSoftwareMinor = resp.data[2];
        state->azkoyenCommsMajor = resp.data[3];
        state->azkoyenCommsMinor = resp.data[4];
        state->azkoyenAboutValid = true;
      }
      return;

    case 0xAA:
    case 0x86:
      if (req.dataLen == 5) {
        state->payoutRequestSerial = readU24LE(req.data);
        state->payoutRequestValue = readU16LE(&req.data[3]);
        state->payoutRequestValid = true;
        state->pollSnapshotValid = false;
      }
      return;

    case 0xA7:
      if (req.dataLen >= 1) {
        state->payoutRequestSerial = (req.dataLen >= 4) ? readU24LE(req.data) : 0;
        const uint8_t coinCount = req.data[(uint8_t)(req.dataLen - 1)];
        const uint16_t coinValue = knownCoinValue(*state);
        state->payoutRequestValue = (coinValue > 0) ? (uint16_t)(coinCount * coinValue) : coinCount;
        state->payoutRequestValid = true;
        state->pollSnapshotValid = false;
        state->azkoyenProgressAccountedValid = false;
      }
      return;

    case 0x20:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
          req.dataLen >= 5) {
        state->payoutRequestSerial = readU24LE(req.data);
        state->azkoyenRequestBaseUnits = 0;
        if (azkoyenHasValueConfig(*state)) {
          state->azkoyenRequestBaseUnits =
              (uint16_t)(((uint16_t)req.data[3] * azkoyenType1ValueUnits(*state)) +
                         ((uint16_t)req.data[4] * azkoyenType2ValueUnits(*state)));
        }
        const uint16_t baseCoinValueCents = azkoyenBaseCoinValueCents(*state);
        state->payoutRequestValue =
            (baseCoinValueCents > 0)
                ? (uint16_t)((uint32_t)state->azkoyenRequestBaseUnits * (uint32_t)baseCoinValueCents)
                : state->azkoyenRequestBaseUnits;
        state->payoutRequestValid = true;
        state->pollSnapshotValid = false;
        state->azkoyenProgressAccountedValid = false;
      }
      return;

    case 0x35:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
          req.dataLen >= 5) {
        state->payoutRequestSerial = readU24LE(req.data);
        state->azkoyenRequestBaseUnits = readU16BE(&req.data[3]);
        const uint16_t baseCoinValueCents = azkoyenBaseCoinValueCents(*state);
        state->payoutRequestValue =
            (baseCoinValueCents > 0)
                ? (uint16_t)((uint32_t)state->azkoyenRequestBaseUnits * (uint32_t)baseCoinValueCents)
                : state->azkoyenRequestBaseUnits;
        state->payoutRequestValid = true;
        state->pollSnapshotValid = false;
        state->azkoyenProgressAccountedValid = false;
      }
      return;

    case 0x83:
      if (req.dataLen == 1 && resp.dataLen == 8) {
        const uint8_t coinIndex = req.data[0];
        if (coinIndex >= 1 && coinIndex <= 16) {
          CoinValueState& coin = state->coinValues[(uint8_t)(coinIndex - 1)];
          copyAsciiSanitized(coin.coin, sizeof(coin.coin), resp.data, 6);
          coin.value = readU16LE(&resp.data[6]);
          coin.valid = true;
        }
      }
      return;

    case 0xD9:
      if (resp.dataLen == 1) {
        state->payoutHiLow = resp.data[0];
        state->payoutHiLowValid = true;
      }
      return;

    case 0xA3:
      if (resp.dataLen == 2) {
        state->testErr1 = resp.data[0];
        state->testErr2 = resp.data[1];
        state->testResultValid = true;
      } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
                 resp.dataLen == 1) {
        state->testErr1 = resp.data[0];
        state->testErr2 = 0;
        state->testResultValid = true;
      }
      return;

    case 0xA6:
      if (resp.dataLen == 4) {
        state->eventCounter = resp.data[0];
        state->eventCounterValid = true;
        if (_dataset.statusMode == HOPPER_STATUS_MODE_AZKOYEN_TYPE1_PAYOUT_COUNTER) {
          updateAzkoyenDispensedFromHopperStatus(*state,
                                                resp.data[0],
                                                resp.data[1],
                                                resp.data[2],
                                                resp.data[3]);
        } else {
          const uint16_t coinValue = knownCoinValue(*state);
          if (coinValue > 0) {
            updateDispensedFromPoll(*state,
                                    (uint16_t)(resp.data[1] * coinValue),
                                    (uint16_t)(resp.data[2] * coinValue),
                                    (uint16_t)(resp.data[3] * coinValue));
          }
        }
      }
      return;

    case 0xAB:
    case 0x85:
      if (resp.dataLen == 7) {
        // Il polling hopper contiene abbastanza informazione per stimare il
        // valore effettivamente erogato tra un campionamento e il successivo.
        state->eventCounter = resp.data[0];
        state->eventCounterValid = true;
        const uint16_t remaining = readU16LE(&resp.data[1]);
        const uint16_t paid = readU16LE(&resp.data[3]);
        const uint16_t unpaid = readU16LE(&resp.data[5]);
        updateDispensedFromPoll(*state, remaining, paid, unpaid);
      }
      return;

    case 0x13:
    case 0x15:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        uint8_t code = 0;
        uint16_t type1Paid = 0, type1Unpaid = 0, type2Paid = 0, type2Unpaid = 0;
        if (parseAzkoyenStatusPayload(resp.data, resp.dataLen,
                                     code,
                                     type1Paid,
                                     type1Unpaid,
                                     type2Paid,
                                     type2Unpaid)) {
          state->azkoyenCurrentStatusCode = code;
          state->azkoyenCurrentType1Paid = type1Paid;
          state->azkoyenCurrentType1Unpaid = type1Unpaid;
          state->azkoyenCurrentType2Paid = type2Paid;
          state->azkoyenCurrentType2Unpaid = type2Unpaid;
          state->azkoyenCurrentStatusValid = true;
          updateAzkoyenDispensedValue(*state, code, type1Paid, type2Paid);
        }
      }
      return;

    case 0x23:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        uint8_t code = 0;
        uint16_t type1Paid = 0, type1Unpaid = 0, type2Paid = 0, type2Unpaid = 0;
        if (parseAzkoyenStatusPayload(resp.data, resp.dataLen,
                                     code,
                                     type1Paid,
                                     type1Unpaid,
                                     type2Paid,
                                     type2Unpaid)) {
          updateAzkoyenDispensedValue(*state, code, type1Paid, type2Paid);

          state->azkoyenLastCommandCode = code;
          state->azkoyenLastType1Paid = type1Paid;
          state->azkoyenLastType1Unpaid = type1Unpaid;
          state->azkoyenLastType2Paid = type2Paid;
          state->azkoyenLastType2Unpaid = type2Unpaid;
          state->azkoyenLastCommandValid = true;
          state->azkoyenLastCommandAccountedValid = true;
          state->azkoyenLastCommandAccountedCode = code;
          state->azkoyenLastCommandAccountedPaidCoins = (uint16_t)(type1Paid + type2Paid);
        }
      }
      return;

    case 0x29:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
          resp.dataLen == 4) {
        state->azkoyenType1Diameter = readU16BE(resp.data);
        state->azkoyenType2Diameter = readU16BE(&resp.data[2]);
        state->azkoyenDiameterValid = true;
      }
      return;

    case 0x32:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
          resp.dataLen == 4) {
        state->azkoyenType1ValueUnits = readU16BE(resp.data);
        state->azkoyenType2ValueUnits = readU16BE(&resp.data[2]);
        state->azkoyenValueUnitsValid = true;
      }
      return;

    default:
      return;
  }
}

void CcTalkHopper::dumpState(Stream& out) const {
  // Dump diagnostico completo dello stato runtime degli hopper.
  out.println(F("[STATE] HOPPER"));
  for (uint8_t i = 0; i < kStateCount; i++) {
    const HopperState& s = _states[i];
    if (!s.present) continue;

    out.print(F("  addr="));
    out.print(s.addr);
    out.print(F(" resetCount="));
    out.print(s.resetCount);
    if (_dataset.statusMode == HOPPER_STATUS_MODE_AZKOYEN_TYPE1_PAYOUT_COUNTER) {
      out.print(F(" payoutCounter="));
    } else {
      out.print(F(" eventCounter="));
    }
    if (s.eventCounterValid) out.println(s.eventCounter);
    else out.println(F("n/a"));

    if (s.azkoyenHopperStatusValid) {
      out.print(F("    azkA6 type1Remaining="));
      out.print(s.azkoyenType1Remaining);
      out.print(F(" lastType1Paid="));
      out.print(s.azkoyenLastType1PaidByA6);
      out.print(F(" lastType1Unpaid="));
      out.println(s.azkoyenLastType1UnpaidByA6);
    }

    if (s.payoutRequestValid) {
      out.print(F("    lastPayoutRequest serial="));
      out.print(s.payoutRequestSerial);
      out.print(F(" value="));
      out.print(s.payoutRequestValue);
      out.print(F(" ("));
      printValueAsEuro(out, s.payoutRequestValue);
      out.println(F(")"));
    }
    if (s.pollSnapshotValid) {
      out.print(F("    poll remaining="));
      out.print(s.lastRemainingValue);
      out.print(F(" paid="));
      out.print(s.lastPaidValue);
      out.print(F(" unpaid="));
      out.println(s.lastUnpaidValue);
    }
    out.print(F("    dispensedTotal="));
    out.print(s.dispensedTotalValue);
    out.print(F(" ("));
    printValueAsEuro(out, s.dispensedTotalValue);
    out.println(F(")"));
    const uint16_t configuredCoinValue = configuredCoinValueCents(s.addr);
    if (configuredCoinValue > 0) {
      out.print(F("    configuredCoinValue="));
      out.print(configuredCoinValue);
      out.print(F(" ("));
      printValueAsEuro(out, configuredCoinValue);
      out.println(F(")"));
    }
    if (s.lastDispenseStepValid) {
      out.print(F("    lastDispenseStep="));
      out.print(s.lastDispenseStepValue);
      out.print(F(" ("));
      printValueAsEuro(out, s.lastDispenseStepValue);
      out.println(F(")"));
    }

    if (s.manufacturerValid) {
      out.print(F("    manufacturer=\""));
      out.print(s.manufacturer);
      out.println('"');
    }
    if (s.productCodeValid) {
      out.print(F("    product=\""));
      out.print(s.productCode);
      out.println('"');
    }
    if (s.serialValid) {
      out.print(F("    serial="));
      out.println(s.serial);
    }
    if (s.extendedIdValid) {
      out.print(F("    extendedId=\""));
      out.print(s.extendedId);
      out.println('"');
    }
    if (s.payoutHiLowValid) {
      out.print(F("    payoutHiLow=0x"));
      if (s.payoutHiLow < 16) out.print('0');
      out.println(s.payoutHiLow, HEX);
    }
    if (s.testResultValid) {
      out.print(F("    testErr1=0x"));
      if (s.testErr1 < 16) out.print('0');
      out.print(s.testErr1, HEX);
      out.print(F(" testErr2=0x"));
      if (s.testErr2 < 16) out.print('0');
      out.println(s.testErr2, HEX);
    }
    if (s.azkoyenAboutValid) {
      out.print(F("    azkAbout type=0x"));
      if (s.azkoyenDeviceType < 16) out.print('0');
      out.print(s.azkoyenDeviceType, HEX);
      out.print(F(" sw="));
      out.print(s.azkoyenSoftwareMajor);
      out.print('.');
      out.print(s.azkoyenSoftwareMinor);
      out.print(F(" comms="));
      out.print(s.azkoyenCommsMajor);
      out.print('.');
      out.println(s.azkoyenCommsMinor);
    }
    if (s.azkoyenCurrentStatusValid) {
      out.print(F("    azkCurrentStatus="));
      out.print(azkoyenStatusLabel(s.azkoyenCurrentStatusCode));
      out.print(F(" t1Paid="));
      out.print(s.azkoyenCurrentType1Paid);
      out.print(F(" t1Unpaid="));
      out.print(s.azkoyenCurrentType1Unpaid);
      out.print(F(" t2Paid="));
      out.print(s.azkoyenCurrentType2Paid);
      out.print(F(" t2Unpaid="));
      out.print(s.azkoyenCurrentType2Unpaid);
      if (s.azkoyenCurrentStatusCode == 0x35) {
        out.print(F(" lastCoin="));
        out.print(s.azkoyenCurrentType2Paid == 0 ? F("1EUR") : F("2EUR"));
        out.print(F(" nextCoin="));
        out.println(s.azkoyenCurrentType2Unpaid == 0 ? F("1EUR") : F("2EUR"));
      } else {
        out.println();
      }
    }
    if (s.azkoyenLastCommandValid) {
      out.print(F("    azkLastCommand="));
      out.print(azkoyenStatusLabel(s.azkoyenLastCommandCode));
      out.print(F(" t1Paid="));
      out.print(s.azkoyenLastType1Paid);
      out.print(F(" t1Unpaid="));
      out.print(s.azkoyenLastType1Unpaid);
      out.print(F(" t2Paid="));
      out.print(s.azkoyenLastType2Paid);
      out.print(F(" t2Unpaid="));
      out.print(s.azkoyenLastType2Unpaid);
      if (s.azkoyenLastCommandCode == 0x35) {
        out.print(F(" lastCoin="));
        out.print(s.azkoyenLastType2Paid == 0 ? F("1EUR") : F("2EUR"));
        out.print(F(" nextCoin="));
        out.println(s.azkoyenLastType2Unpaid == 0 ? F("1EUR") : F("2EUR"));
      } else {
        out.println();
      }
    }
    if (s.azkoyenDiameterValid) {
      out.print(F("    azkDiameters type1="));
      out.print(s.azkoyenType1Diameter);
      out.print(F(" type2="));
      out.println(s.azkoyenType2Diameter);
    }
    if (s.azkoyenValueUnitsValid) {
      out.print(F("    azkValues baseUnits type1="));
      out.print(s.azkoyenType1ValueUnits);
      out.print(F(" type2="));
      out.println(s.azkoyenType2ValueUnits);
    } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR &&
               (_dataset.defaultType1ValueUnits > 0 || _dataset.defaultType2ValueUnits > 0)) {
      out.print(F("    azkValues(default) baseUnits type1="));
      out.print(_dataset.defaultType1ValueUnits);
      out.print(F(" type2="));
      out.println(_dataset.defaultType2ValueUnits);
    }

    for (uint8_t coinIdx = 0; coinIdx < 16; coinIdx++) {
      const CoinValueState& coin = s.coinValues[coinIdx];
      if (!coin.valid) continue;
      out.print(F("    coin["));
      out.print((uint8_t)(coinIdx + 1));
      out.print(F("] id=\""));
      out.print(coin.coin);
      out.print(F("\" value="));
      out.println(coin.value);
    }
  }
}

const __FlashStringHelper* CcTalkHopper::cmdDesc(uint8_t hdr) const {
  // Catalogo specifico hopper.
  switch (hdr) {
    case 0x29:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("request diameters (29)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x32:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("request coin values (32)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x31:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("program coin values (31)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x25:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("program diameters (25)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x23:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("request last command status (23)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x34:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("multiple emptying (34)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x35:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("intelligent payout (35)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x20:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("multiple payout (20)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x19:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("multiple emptying (19)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x15:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("cancel current command (15)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x13:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("request discriminator status (13)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x10:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        return F("about discriminator (10)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0xD9: return F("request payout hi/low status (D9)");
    case 0xD2: return F("modify sorter path (D2)");
    case 0xD1: return F("request sorter path (D1)");
    case 0xC5: return F("calculate ROM checksum (C5)");
    case 0xEC: return F("read opto states (EC)");
    case 0xA4: return F("enable hopper (A4)");
    case 0xA3: return F("test hopper (A3)");
    case 0xAA: case 0x86: return F("dispense hopper value (AA/86)");
    case 0xAB: case 0x85: return F("request hopper polling value (AB/85)");
    case 0xAC: case 0x84: return F("emergency stop value (AC/84)");
    case 0x83: return F("request hopper coin value (83)");
    case 0x82: return F("request indexed hopper dispense count (82)");
    case 0xA9: return F("request address mode (A9)");
    case 0xA8: return F("request dispense count total (A8)");
    case 0xA7: return F("dispense coin (A7)");
    case 0xA6: return F("request hopper status (A6)");
    case 0xD8: return F("request data storage availability (D8)");
    default:   return CcTalkMaster::headerDesc(hdr);
  }
}

void CcTalkHopper::printRequestPayload(Stream& out, const CcTalkFrame& req) {
  // Interpreta i payload che aggiungono significato operativo ai comandi.
  switch (req.hdr) {
    case 0xA4:
      if (req.dataLen == 1) {
        out.print(F("  payload: mode=0x"));
        if (req.data[0] < 16) out.print('0');
        out.print(req.data[0], HEX);
        if (req.data[0] == 0xA5) out.println(F(" (ENABLE)"));
        else out.println(F(" (DISABLE)"));
      } else if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0xAA: case 0x86:
      if (req.dataLen == 5) {
        const uint32_t serial = readU24LE(req.data);
        const uint16_t value  = readU16LE(&req.data[3]);
        out.print(F("  payload: serial="));
        out.print(serial);
        out.print(F(" value="));
        out.print(value);
        out.println(F(" (unita minime)"));
      } else if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0xA7:
      if (req.dataLen >= 1) {
        out.print(F("  payload: coins="));
        out.print(req.data[(uint8_t)(req.dataLen - 1)]);
        if (req.dataLen >= 4) {
          out.print(F(" serial="));
          out.print(readU24LE(req.data));
        }
        out.println();
      } else if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x83:
      if (req.dataLen == 1) {
        out.print(F("  payload: coinIndex="));
        out.println(req.data[0]);
      } else if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x82:
      if (req.dataLen == 1) {
        out.print(F("  payload: counterIndex="));
        out.println(req.data[0]);
      } else if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x20:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && req.dataLen >= 5) {
        out.print(F("  payload: serial="));
        out.print(readU24LE(req.data));
        out.print(F(" type1Coins="));
        out.print(req.data[3]);
        out.print(F(" type2Coins="));
        out.print(req.data[4]);
        if (req.dataLen > 5) {
          out.print(F(" extra="));
          dumpHex(out, &req.data[5], (uint8_t)(req.dataLen - 5));
        }
        out.println();
      } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x35:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && req.dataLen >= 5) {
        out.print(F("  payload: serial="));
        out.print(readU24LE(req.data));
        out.print(F(" amountBaseUnits="));
        out.print(readU16BE(&req.data[3]));
        const HopperState* state = stateFor(req.dest);
        if (state) {
          const uint16_t baseCoinValueCents = azkoyenBaseCoinValueCents(*state);
          if (baseCoinValueCents > 0) {
            out.print(F(" approxValue="));
            printValueAsEuro(out, (uint32_t)readU16BE(&req.data[3]) * (uint32_t)baseCoinValueCents);
          }
        }
        out.println();
      } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x25:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && req.dataLen == 4) {
        out.print(F("  payload: type1Diameter="));
        out.print(readU16BE(req.data));
        out.print(F(" type2Diameter="));
        out.println(readU16BE(&req.data[2]));
      } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0x31:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && req.dataLen == 4) {
        out.print(F("  payload: type1BaseUnits="));
        out.print(readU16BE(req.data));
        out.print(F(" type2BaseUnits="));
        out.println(readU16BE(&req.data[2]));
      } else if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    default:
      if (req.dataLen) {
        out.print(F("  payload raw: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;
  }
}

void CcTalkHopper::printRequest(Stream& out, const CcTalkFrame& req) {
  out.print(F("MASTER -> HOPPER["));
  out.print(req.dest);
  out.print(F("]: "));
  out.print(cmdDesc(req.hdr));
  out.print(F(" (0x"));
  if (req.hdr < 16) out.print('0');
  out.print(req.hdr, HEX);
  out.println(F(")"));
  printRequestPayload(out, req);
}

void CcTalkHopper::printPayoutHiLowStatus(Stream& out, uint8_t status) {
  // Decodifica a bit di uno status byte molto compatto.
  out.print(F("status=0x"));
  if (status < 16) out.print('0');
  out.print(status, HEX);
  out.print(F(" lowLevel="));
  out.print((status & 0x01) ? F("LOW") : F("OK"));
  out.print(F(" highLevel="));
  out.print((status & 0x02) ? F("HIGH") : F("OK"));
  out.print(F(" lowSensor="));
  out.print((status & 0x10) ? F("present") : F("n/a"));
  out.print(F(" highSensor="));
  out.println((status & 0x20) ? F("present") : F("n/a"));
}

void CcTalkHopper::printTestHopperErrors(Stream& out, uint8_t err1, uint8_t err2) {
  // I due byte di errore sono maschere bitwise: qui vengono espansi in label.
  out.print(F("err1=0x"));
  if (err1 < 16) out.print('0');
  out.print(err1, HEX);
  out.print(F(" err2=0x"));
  if (err2 < 16) out.print('0');
  out.print(err2, HEX);

  bool any = false;
  out.print(F(" flags="));

  if (err1 & 0x01) { out.print(F("I_MAX ")); any = true; }
  if (err1 & 0x02) { out.print(F("TIMEOUT ")); any = true; }
  if (err1 & 0x04) { out.print(F("MOTOR_REV ")); any = true; }
  if (err1 & 0x08) { out.print(F("OPTO_BLOCK_IDLE ")); any = true; }
  if (err1 & 0x10) { out.print(F("OPTO_SHORT_IDLE ")); any = true; }
  if (err1 & 0x20) { out.print(F("OPTO_BLOCK_PAY ")); any = true; }
  if (err1 & 0x40) { out.print(F("POWER_UP ")); any = true; }
  if (err1 & 0x80) { out.print(F("DISABLED ")); any = true; }

  if (err2 & 0x01) { out.print(F("OPTO_SHORT_PAY ")); any = true; }
  if (err2 & 0x02) { out.print(F("FLASH_CRC ")); any = true; }
  if (err2 & 0x04) { out.print(F("USE_OTHER_HOPPER ")); any = true; }
  if (err2 & 0x08) { out.print(F("NU_READ0 ")); any = true; }
  if (err2 & 0x10) { out.print(F("MOTOR_REV_LIMIT ")); any = true; }
  if (err2 & 0x20) { out.print(F("UNRECOG_COIN_REV_LIMIT ")); any = true; }
  if (err2 & 0x40) { out.print(F("SORTER_BLOCKED ")); any = true; }
  if (err2 & 0x80) { out.print(F("PIN_ACTIVE ")); any = true; }

  if (!any) out.print(F("none"));
  out.println();
}

const __FlashStringHelper* CcTalkHopper::azkoyenStatusLabel(uint8_t code) const {
  switch (code) {
    case 0x01: return F("idle");
    case 0x24: return F("multiple emptying");
    case 0x25: return F("multiple payout");
    case 0x34: return F("multiple emptying");
    case 0x35: return F("intelligent payout");
    default:   return F("status non mappato");
  }
}

bool CcTalkHopper::parseAzkoyenStatusPayload(const uint8_t* data, uint8_t len,
                                             uint8_t& code,
                                             uint16_t& type1Paid,
                                             uint16_t& type1Unpaid,
                                             uint16_t& type2Paid,
                                             uint16_t& type2Unpaid) const {
  code = 0;
  type1Paid = 0;
  type1Unpaid = 0;
  type2Paid = 0;
  type2Unpaid = 0;
  if (_dataset.customCommandMode != HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) return false;
  if (!data || len == 0) return false;

  code = data[0];
  if (len == 1 && code == 0x01) return true;

  if (len >= 5 && (code == 0x24 || code == 0x34)) {
    type1Paid = readU16BE(&data[1]);
    type2Paid = readU16BE(&data[3]);
    return true;
  }

  if (len >= 9 && (code == 0x25 || code == 0x35)) {
    type1Paid = readU16BE(&data[1]);
    type1Unpaid = readU16BE(&data[3]);
    type2Paid = readU16BE(&data[5]);
    type2Unpaid = readU16BE(&data[7]);
    return true;
  }

  return false;
}

void CcTalkHopper::printAzkoyenStatusPayload(Stream& out, const uint8_t* data, uint8_t len) const {
  uint8_t code = 0;
  uint16_t type1Paid = 0, type1Unpaid = 0, type2Paid = 0, type2Unpaid = 0;
  if (!parseAzkoyenStatusPayload(data, len, code, type1Paid, type1Unpaid, type2Paid, type2Unpaid)) {
    out.print(F("Azkoyen raw: "));
    dumpHex(out, data, len);
    out.println();
    return;
  }

  out.print(F("Status="));
  out.print(azkoyenStatusLabel(code));
  out.print(F(" (0x"));
  if (code < 16) out.print('0');
  out.print(code, HEX);
  out.print(')');

  if (len >= 5) {
    out.print(F(" type1Paid="));
    out.print(type1Paid);
    out.print(F(" type2Paid="));
    out.print(type2Paid);
    if (code == 0x35) {
      out.print(F(" [lastCoin="));
      out.print(type2Paid == 0 ? F("1EUR") : F("2EUR"));
      out.print(']');
    }
  }
  if (len >= 9) {
    out.print(F(" type1Unpaid="));
    out.print(type1Unpaid);
    out.print(F(" type2Unpaid="));
    out.print(type2Unpaid);
    if (code == 0x35) {
      out.print(F(" [nextCoin="));
      out.print(type2Unpaid == 0 ? F("1EUR") : F("2EUR"));
      out.print(']');
    }
  }
  if (len > 9) {
    out.print(F(" extra="));
    dumpHex(out, &data[9], (uint8_t)(len - 9));
  }
  out.println();
}

void CcTalkHopper::printValueAsEuro(Stream& out, uint32_t units) const {
  // `units` e gia espresso in centesimi/unita minime del sistema.
  const uint32_t eur = units / 100;
  const uint8_t cents = (uint8_t)(units % 100);
  out.print(eur);
  out.print('.');
  if (cents < 10) out.print('0');
  out.print(cents);
  out.print(F(" EUR"));
}

uint16_t CcTalkHopper::configuredCoinValueCents(uint8_t addr) const {
  if (addr < kAddrMin || addr > kAddrMax) return 0;
  return _configuredCoinValueCents[(uint8_t)(addr - kAddrMin)];
}

uint16_t CcTalkHopper::azkoyenBaseCoinValueCents(const HopperState& state) const {
  const uint16_t configured = configuredCoinValueCents(state.addr);
  if (configured > 0) return configured;
  return _dataset.defaultBaseCoinValueCents;
}

bool CcTalkHopper::azkoyenHasBaseCoinConfig(const HopperState& state) const {
  return azkoyenBaseCoinValueCents(state) > 0;
}

uint16_t CcTalkHopper::azkoyenType1CoinValueCents(const HopperState& state) const {
  const uint16_t baseCoinValueCents = azkoyenBaseCoinValueCents(state);
  const uint16_t type1Units = azkoyenType1ValueUnits(state);
  if (baseCoinValueCents == 0 || type1Units == 0) return 0;

  const uint32_t valueCents = (uint32_t)baseCoinValueCents * (uint32_t)type1Units;
  return (valueCents > 0xFFFFu) ? 0xFFFFu : (uint16_t)valueCents;
}

bool CcTalkHopper::azkoyenHasType1CoinValue(const HopperState& state) const {
  return azkoyenType1CoinValueCents(state) > 0;
}

uint16_t CcTalkHopper::azkoyenType1ValueUnits(const HopperState& state) const {
  if (state.azkoyenValueUnitsValid && state.azkoyenType1ValueUnits > 0) {
    return state.azkoyenType1ValueUnits;
  }
  return _dataset.defaultType1ValueUnits;
}

uint16_t CcTalkHopper::azkoyenType2ValueUnits(const HopperState& state) const {
  if (state.azkoyenValueUnitsValid && state.azkoyenType2ValueUnits > 0) {
    return state.azkoyenType2ValueUnits;
  }
  return _dataset.defaultType2ValueUnits;
}

uint16_t CcTalkHopper::knownCoinValue(const HopperState& state) const {
  const uint16_t configuredValue = configuredCoinValueCents(state.addr);
  if (configuredValue > 0) return configuredValue;
  for (uint8_t i = 0; i < 16; i++) {
    if (!state.coinValues[i].valid) continue;
    if (state.coinValues[i].value == 0) continue;
    return state.coinValues[i].value;
  }
  return 0;
}

bool CcTalkHopper::azkoyenHasValueConfig(const HopperState& state) const {
  return (azkoyenType1ValueUnits(state) > 0 || azkoyenType2ValueUnits(state) > 0) &&
         azkoyenHasBaseCoinConfig(state);
}

uint8_t CcTalkHopper::azkoyenHopperStatusCounterDelta(uint8_t previous, uint8_t current) const {
  if (current == previous) return 0;
  if (previous == 255 && current == 1) return 1;
  if (current > previous) return (uint8_t)(current - previous);
  return 0xFFu;
}

void CcTalkHopper::updateAzkoyenDispensedFromHopperStatus(HopperState& state,
                                                          uint8_t payoutCounter,
                                                          uint8_t type1Remaining,
                                                          uint8_t type1Paid,
                                                          uint8_t type1Unpaid) {
  state.azkoyenHopperStatusValid = true;
  state.azkoyenType1Remaining = type1Remaining;
  state.azkoyenLastType1PaidByA6 = type1Paid;
  state.azkoyenLastType1UnpaidByA6 = type1Unpaid;

  if (!state.azkoyenHopperStatusCounterSeen) {
    state.azkoyenHopperStatusCounterSeen = true;
    state.azkoyenLastHopperStatusCounter = payoutCounter;
    state.lastDispenseStepValid = false;
    return;
  }

  const uint8_t deltaCounter =
      azkoyenHopperStatusCounterDelta(state.azkoyenLastHopperStatusCounter, payoutCounter);
  state.azkoyenHopperStatusCounterSeen = true;
  state.azkoyenLastHopperStatusCounter = payoutCounter;

  if (deltaCounter != 1 || !azkoyenHasType1CoinValue(state) || type1Paid == 0) {
    state.lastDispenseStepValid = false;
    return;
  }

  const uint32_t deltaValue =
      (uint32_t)type1Paid * (uint32_t)azkoyenType1CoinValueCents(state);
  state.dispensedTotalValue += deltaValue;
  state.lastDispenseStepValue = (deltaValue > 0xFFFFu) ? 0xFFFFu : (uint16_t)deltaValue;
  state.lastDispenseStepValid = true;
}

uint32_t CcTalkHopper::azkoyenPaidBaseUnits(const HopperState& state,
                                            uint8_t code,
                                            uint16_t type1Paid,
                                            uint16_t type2Paid) const {
  (void)type2Paid;
  if (code == 0x35) {
    if (!azkoyenHasBaseCoinConfig(state)) return 0;
    return (uint32_t)type1Paid;
  }
  if (!azkoyenHasValueConfig(state)) return 0;
  return ((uint32_t)type1Paid * (uint32_t)azkoyenType1ValueUnits(state)) +
         ((uint32_t)type2Paid * (uint32_t)azkoyenType2ValueUnits(state));
}

uint32_t CcTalkHopper::azkoyenUnpaidBaseUnits(const HopperState& state,
                                              uint8_t code,
                                              uint16_t type1Unpaid,
                                              uint16_t type2Unpaid) const {
  (void)type2Unpaid;
  if (code == 0x35) {
    if (!azkoyenHasBaseCoinConfig(state)) return 0;
    return (uint32_t)type1Unpaid;
  }
  if (!azkoyenHasValueConfig(state)) return 0;
  return ((uint32_t)type1Unpaid * (uint32_t)azkoyenType1ValueUnits(state)) +
         ((uint32_t)type2Unpaid * (uint32_t)azkoyenType2ValueUnits(state));
}

void CcTalkHopper::updateAzkoyenDispensedValue(HopperState& state,
                                               uint8_t code,
                                               uint16_t type1Paid,
                                               uint16_t type2Paid) {
  const bool payoutLikeCommand =
      (code == 0x24 || code == 0x25 || code == 0x34 || code == 0x35);
  if (!payoutLikeCommand) {
    // Non azzerare il progresso sul semplice ritorno a idle: il master puo
    // interrogare subito dopo 0x23 (last command status) con i valori finali
    // dello stesso payout, e dobbiamo continuare il delta dall'ultimo 0x13.
    state.lastDispenseStepValid = false;
    return;
  }

  const bool hasEconomicConfig =
      (code == 0x35) ? azkoyenHasBaseCoinConfig(state) : azkoyenHasValueConfig(state);
  if (!hasEconomicConfig) {
    state.lastDispenseStepValid = false;
    return;
  }

  const uint32_t paidBaseUnits = azkoyenPaidBaseUnits(state, code, type1Paid, type2Paid);
  uint32_t deltaBaseUnits = paidBaseUnits;
  if (state.azkoyenProgressAccountedValid &&
      state.azkoyenProgressAccountedCode == code &&
      paidBaseUnits >= state.azkoyenProgressPaidBaseUnits) {
    deltaBaseUnits -= state.azkoyenProgressPaidBaseUnits;
  }

  state.azkoyenProgressAccountedValid = true;
  state.azkoyenProgressAccountedCode = code;
  state.azkoyenProgressPaidBaseUnits = paidBaseUnits;

  const uint16_t baseCoinValueCents = azkoyenBaseCoinValueCents(state);
  const uint32_t deltaValue = deltaBaseUnits * (uint32_t)baseCoinValueCents;
  if (deltaValue > 0) {
    state.dispensedTotalValue += deltaValue;
    state.lastDispenseStepValue = (uint16_t)deltaValue;
    state.lastDispenseStepValid = true;
  } else {
    state.lastDispenseStepValid = false;
  }
}

void CcTalkHopper::updateDispensedFromPoll(HopperState& state,
                                           uint16_t remaining,
                                           uint16_t paid,
                                           uint16_t unpaid) {
  // Algoritmo conservativo di ricostruzione dell'erogato:
  // usa principalmente il delta del valore pagato, ma controlla anche
  // l'andamento del valore rimanente per evitare sovrastime.
  if (!state.pollSnapshotValid) {
    state.lastRemainingValue = remaining;
    state.lastPaidValue = paid;
    state.lastUnpaidValue = unpaid;
    state.pollSnapshotValid = true;
    state.lastDispenseStepValid = false;
    return;
  }

  // Se i valori "tornano indietro", riallinea snapshot (nuovo ciclo o reset logico).
  if (paid < state.lastPaidValue || remaining > state.lastRemainingValue) {
    state.lastRemainingValue = remaining;
    state.lastPaidValue = paid;
    state.lastUnpaidValue = unpaid;
    state.lastDispenseStepValid = false;
    return;
  }

  const uint16_t deltaPaid = (uint16_t)(paid - state.lastPaidValue);
  const uint16_t deltaRemaining = (uint16_t)(state.lastRemainingValue - remaining);

  // Fonte principale: incremento di lastPaidValue ad ogni poll.
  uint16_t deltaDispensed = deltaPaid;

  // Correlazione con remainingValue: se mismatch verso il basso, usa valore conservativo.
  // Esempio atteso: 500/0 -> 300/200 -> 200/300 => step 200 poi 100.
  if (deltaPaid > 0 && deltaRemaining > 0 && deltaRemaining < deltaPaid) {
    deltaDispensed = deltaRemaining;
  }

  if (deltaDispensed > 0) {
    state.dispensedTotalValue += deltaDispensed;
    state.lastDispenseStepValue = deltaDispensed;
    state.lastDispenseStepValid = true;
  } else {
    state.lastDispenseStepValid = false;
  }

  state.lastRemainingValue = remaining;
  state.lastPaidValue = paid;
  state.lastUnpaidValue = unpaid;
}

void CcTalkHopper::printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp) {
  // Anche per l'hopper la risposta va letta nel contesto del comando richiesto.
  out.print(F("HOPPER["));
  out.print(resp.src);
  out.print(F("] -> MASTER: "));

  if (resp.hdr == 0x05) { out.println(F("NAK")); return; }
  if (resp.hdr == 0x06) { out.println(F("BUSY")); return; }

  if (resp.hdr != 0x00) {
    out.print(F("HDR=0x")); if (resp.hdr < 16) out.print('0'); out.println(resp.hdr, HEX);
    return;
  }

  switch (hostHdr) {
    case 0xFE: out.println(F("ACK (presente)")); return;
    case 0x01: out.println(F("ACK (reset eseguito)")); return;
    case 0xA4: out.println(F("ACK (stato hopper aggiornato)")); return;
    case 0xA7:
      out.println(F("ACK (comando payout monete accettato)"));
      {
        const HopperState* state = stateFor(resp.src);
        if (state && state->payoutRequestValid) {
          out.print(F("  [MEM] payoutRequest="));
          out.print(state->payoutRequestValue);
          if (knownCoinValue(*state) > 0) {
            out.print(F(" ("));
            printValueAsEuro(out, state->payoutRequestValue);
            out.print(')');
          } else {
            out.print(F(" (conteggio monete / valore non ancora noto)"));
          }
          out.println();
        }
      }
      return;
    case 0xAA:
    case 0x86:
      out.println(F("ACK (comando payout accettato)"));
      {
        const HopperState* state = stateFor(resp.src);
        if (state && state->payoutRequestValid) {
          out.print(F("  [MEM] payoutRequest value="));
          out.print(state->payoutRequestValue);
          out.print(F(" ("));
          printValueAsEuro(out, state->payoutRequestValue);
          out.println(F(")"));
        }
      }
      return;
    case 0xD2:
      if (resp.dataLen == 0) out.println(F("ACK (percorso sorter aggiornato)"));
      else {
        out.print(F("ACK data D2: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0xF6: case 0xF5: case 0xF4: case 0xC0: case 0xFF:
      out.print('"'); dumpAscii(out, resp.data, resp.dataLen); out.println('"'); return;

    case 0xF1:
      if (resp.dataLen == 2) {
        out.print(F("SoftwareRevision="));
        out.print(resp.data[0]);
        out.print('.');
        out.println(resp.data[1]);
      } else {
        out.print('"'); dumpAscii(out, resp.data, resp.dataLen); out.println('"');
      }
      return;

    case 0xF2:
      if (resp.dataLen == 3) { out.print(F("Serial=")); out.println(readU24LE(resp.data)); }
      else { out.print(F("Serial raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0x04:
      if (resp.dataLen == 3) {
        out.print(F("Level=")); out.print(resp.data[0]);
        out.print(F(" Major=")); out.print(resp.data[1]);
        out.print(F(" Minor=")); out.println(resp.data[2]);
      } else {
        out.print(F("Comms raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println();
      }
      return;

    case 0xEC:
      if (resp.dataLen == 1) {
        out.print(F("OptoStates=0x"));
        if (resp.data[0] < 16) out.print('0');
        out.println(resp.data[0], HEX);
      } else {
        out.print(F("EC raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0xD9:
      if (resp.dataLen == 1) printPayoutHiLowStatus(out, resp.data[0]);
      else { out.print(F("D9 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0xC5:
      if (resp.dataLen == 3) {
        out.print(F("ROM checksum: monitor=0x"));
        if (resp.data[0] < 16) out.print('0');
        out.print(resp.data[0], HEX);
        out.print(F(" program=0x"));
        if (resp.data[1] < 16) out.print('0');
        out.print(resp.data[1], HEX);
        out.print(F(" ram=0x"));
        if (resp.data[2] < 16) out.print('0');
        out.println(resp.data[2], HEX);
      } else { out.print(F("C5 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0xA3:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && resp.dataLen == 1) {
        out.print(F("TestResult=0x"));
        if (resp.data[0] < 16) out.print('0');
        out.println(resp.data[0], HEX);
      } else if (resp.dataLen == 2) {
        printTestHopperErrors(out, resp.data[0], resp.data[1]);
      }
      else { out.print(F("A3 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0xA8:
      if (resp.dataLen == 3) {
        const uint32_t count = readU24LE(resp.data);
        out.print(F("DispenseTotCount="));
        out.print(count);
        const HopperState* state = stateFor(resp.src);
        if (state) {
          const uint16_t coinValue = knownCoinValue(*state);
          if (coinValue > 0) {
            out.print(F(" approxValue="));
            printValueAsEuro(out, count * (uint32_t)coinValue);
          }
        }
        out.println();
      }
      else { out.print(F("A8 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0x10:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && resp.dataLen == 5) {
        out.print(F("About: type=0x"));
        if (resp.data[0] < 16) out.print('0');
        out.print(resp.data[0], HEX);
        out.print(F(" sw="));
        out.print(resp.data[1]);
        out.print('.');
        out.print(resp.data[2]);
        out.print(F(" comms="));
        out.print(resp.data[3]);
        out.print('.');
        out.println(resp.data[4]);
      } else {
        out.print(F("10 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x13:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        printAzkoyenStatusPayload(out, resp.data, resp.dataLen);
        const HopperState* state = stateFor(resp.src);
        if (state) {
          const bool intelligentPayout = state->azkoyenCurrentStatusCode == 0x35;
          if ((intelligentPayout && azkoyenHasBaseCoinConfig(*state)) ||
              (!intelligentPayout && azkoyenHasValueConfig(*state))) {
            out.print(F("  [CFG] "));
            if (!intelligentPayout) {
              out.print(F("type1BaseUnits="));
              out.print(azkoyenType1ValueUnits(*state));
              out.print(F(" type2BaseUnits="));
              out.print(azkoyenType2ValueUnits(*state));
              out.print(F(" "));
            }
            out.print(F("baseCoinValue="));
            out.print(azkoyenBaseCoinValueCents(*state));
            out.print(F(" ("));
            printValueAsEuro(out, azkoyenBaseCoinValueCents(*state));
            out.println(F(")"));
          } else if (intelligentPayout && !azkoyenHasBaseCoinConfig(*state)) {
            out.println(F("  [CFG] valore unita base discriminatore non configurato"));
          } else if (!intelligentPayout && !azkoyenHasValueConfig(*state)) {
            out.println(F("  [CFG] valori type1/type2 non ancora acquisiti (serve comando 0x32 o 0x31)"));
          } else {
            out.println(F("  [CFG] valore unita base discriminatore non configurato"));
          }
          if (state->lastDispenseStepValid) {
            out.print(F("  [MEM] erogatoStep="));
            out.print(state->lastDispenseStepValue);
            out.print(F(" ("));
            printValueAsEuro(out, state->lastDispenseStepValue);
            out.println(F(")"));
          }
        }
      } else {
        out.print(F("13 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x15:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        printAzkoyenStatusPayload(out, resp.data, resp.dataLen);
      } else {
        out.print(F("15 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x34:
    case 0x19:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.println(F("ACK (svuotamento multiplo avviato)"));
      } else {
        out.print(F("19 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x35:
    case 0x20:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        if (hostHdr == 0x35) out.println(F("ACK (intelligent payout avviato)"));
        else out.println(F("ACK (pagamento multiplo avviato)"));
      } else {
        out.print(F("20 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x23:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.print(F("LastCommand "));
        printAzkoyenStatusPayload(out, resp.data, resp.dataLen);
        const HopperState* state = stateFor(resp.src);
        if (state) {
          const bool intelligentPayout = state->azkoyenLastCommandCode == 0x35;
          if ((intelligentPayout && azkoyenHasBaseCoinConfig(*state)) ||
              (!intelligentPayout && azkoyenHasValueConfig(*state))) {
            out.print(F("  [CFG] "));
            if (!intelligentPayout) {
              out.print(F("type1BaseUnits="));
              out.print(azkoyenType1ValueUnits(*state));
              out.print(F(" type2BaseUnits="));
              out.print(azkoyenType2ValueUnits(*state));
              out.print(F(" "));
            }
            out.print(F("baseCoinValue="));
            out.print(azkoyenBaseCoinValueCents(*state));
            out.print(F(" ("));
            printValueAsEuro(out, azkoyenBaseCoinValueCents(*state));
            out.println(F(")"));
          } else if (intelligentPayout && !azkoyenHasBaseCoinConfig(*state)) {
            out.println(F("  [CFG] valore unita base discriminatore non configurato"));
          } else if (!intelligentPayout && !state->azkoyenValueUnitsValid) {
            out.println(F("  [CFG] valori type1/type2 non ancora acquisiti (serve comando 0x32 o 0x31)"));
          } else {
            out.println(F("  [CFG] valore unita base discriminatore non configurato"));
          }
          if (state->lastDispenseStepValid) {
            out.print(F("  [MEM] erogatoStep="));
            out.print(state->lastDispenseStepValue);
            out.print(F(" ("));
            printValueAsEuro(out, state->lastDispenseStepValue);
            out.println(F(")"));
          }
          out.print(F("  [MEM] erogatoTotale="));
          out.print(state->dispensedTotalValue);
          out.print(F(" ("));
          printValueAsEuro(out, state->dispensedTotalValue);
          out.println(F(")"));
        }
      } else {
        out.print(F("23 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x25:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.println(F("ACK (diametri programmati)"));
      } else {
        out.print(F("25 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x29:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && resp.dataLen == 4) {
        out.print(F("Diameters: type1="));
        out.print(readU16BE(resp.data));
        out.print(F(" type2="));
        out.println(readU16BE(&resp.data[2]));
      } else {
        out.print(F("29 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x32:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR && resp.dataLen == 4) {
        out.print(F("Values: type1BaseUnits="));
        out.print(readU16BE(resp.data));
        out.print(F(" type2BaseUnits="));
        out.println(readU16BE(&resp.data[2]));
      } else {
        out.print(F("32 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x31:
      if (_dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR) {
        out.println(F("ACK (valori type1/type2 programmati)"));
      } else {
        out.print(F("31 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0xA6:
      if (resp.dataLen == 4) {
        const bool azkoyenType1Status =
            (_dataset.statusMode == HOPPER_STATUS_MODE_AZKOYEN_TYPE1_PAYOUT_COUNTER);
        if (azkoyenType1Status) {
          out.print(F("payoutCounter=")); out.print(resp.data[0]);
          out.print(F(" type1CoinsRemaining=")); out.print(resp.data[1]);
          out.print(F(" lastType1Paid=")); out.print(resp.data[2]);
          out.print(F(" lastType1Unpaid=")); out.println(resp.data[3]);
        } else {
          out.print(F("event=")); out.print(resp.data[0]);
          out.print(F(" payoutCoinsRemaining=")); out.print(resp.data[1]);
          out.print(F(" lastPayoutPaid=")); out.print(resp.data[2]);
          out.print(F(" lastPayoutUnpaid=")); out.println(resp.data[3]);
        }
        const HopperState* state = stateFor(resp.src);
        if (state) {
          if (azkoyenType1Status) {
            if (azkoyenHasType1CoinValue(*state)) {
              out.print(F("  [CFG] type1CoinValue="));
              out.print(azkoyenType1CoinValueCents(*state));
              out.print(F(" ("));
              printValueAsEuro(out, azkoyenType1CoinValueCents(*state));
              out.println(F(")"));
            } else {
              out.println(F("  [CFG] valore moneta type1 non configurato"));
            }
          }
          if (state->lastDispenseStepValid) {
            out.print(F("  [MEM] erogatoStep="));
            out.print(state->lastDispenseStepValue);
            out.print(F(" ("));
            printValueAsEuro(out, state->lastDispenseStepValue);
            out.println(F(")"));
          }
        }
      } else { out.print(F("A6 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0xAB: case 0x85:
      if (resp.dataLen == 7) {
        uint16_t remaining = readU16LE(&resp.data[1]);
        uint16_t paid      = readU16LE(&resp.data[3]);
        uint16_t unpaid    = readU16LE(&resp.data[5]);
        out.print(F("event=")); out.print(resp.data[0]);
        out.print(F(" remainingValue=")); out.print(remaining);
        out.print(F(" lastPaidValue=")); out.print(paid);
        out.print(F(" lastUnpaidValue=")); out.println(unpaid);
        const HopperState* state = stateFor(resp.src);
        if (state) {
          if (state->lastDispenseStepValid) {
            out.print(F("  [MEM] erogatoStep="));
            out.print(state->lastDispenseStepValue);
            out.print(F(" ("));
            printValueAsEuro(out, state->lastDispenseStepValue);
            out.println(F(")"));
          }
          out.print(F("  [MEM] erogatoTotale="));
          out.print(state->dispensedTotalValue);
          out.print(F(" ("));
          printValueAsEuro(out, state->dispensedTotalValue);
          out.println(F(")"));
        }
      } else { out.print(F("AB raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0xAC: case 0x84:
      if (resp.dataLen == 2) {
        out.print(F("UnpaidValue="));
        out.println(readU16LE(resp.data));
      } else { out.print(F("84 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0x83:
      if (resp.dataLen == 8) {
        out.print(F("Coin=\""));
        dumpAscii(out, resp.data, 6);
        out.print(F("\" Value="));
        out.println(readU16LE(&resp.data[6]));
      } else { out.print(F("83 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    case 0x82:
      if (resp.dataLen == 3) {
        out.print(F("DispenseCount="));
        out.println(readU24LE(resp.data));
      } else { out.print(F("82 raw: ")); dumpHex(out, resp.data, resp.dataLen); out.println(); }
      return;

    default:
      out.print(F("ACK data: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;
  }
}

void CcTalkHopper::onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) {
  (void)printRaw;
  // Aggiorna prima il modello runtime, poi emette la vista testuale.
  updateState(t);
  out.println();

  uint8_t hostHdr = 0;
  if (t.hasReq) { hostHdr = t.req.hdr; printRequest(out, t.req); }
  else out.println(F("MASTER -> HOPPER: (richiesta assente)"));

  if (t.hasResp) printResponse(out, hostHdr, t.resp);
  else out.println(F("HOPPER -> MASTER: (nessuna risposta)"));
}
