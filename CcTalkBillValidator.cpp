// Scopo del file:
// implementa la logica di decodifica e aggiornamento stato dei bill validator.
/*
 * CcTalkBillValidator decode notes
 *
 * Standard commands used:
 * - 0x99 modify bill operating mode
 * - 0x9A route bill
 * - 0x9F read buffered bill events
 * - 0xE4 modify master inhibit status
 *
 * OEM commands:
 * - 0x5E recycler inventory status
 * - 0x61 payout/transfer bill
 *
 * Event rules:
 * - escrow: B = 1
 * - credit stacker/cashbox: B = 0
 * - stored recycler: B = billType + 15
 */
#include "CcTalkBillValidator.h"
#include "CcTalkMaster.h"
#include "CcTalkUtils.h"
#include <string.h>

namespace {
// Copia testo proveniente dal bus in un buffer C rendendo espliciti i byte
// non stampabili. Questo evita stringhe "sporche" nel log e nello stato.
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

uint32_t readU32LE(const uint8_t* d) {
  if (!d) return 0;
  return (uint32_t)d[0] |
         ((uint32_t)d[1] << 8) |
         ((uint32_t)d[2] << 16) |
         ((uint32_t)d[3] << 24);
}

void printMoneyCents(Stream& out, uint32_t cents) {
  out.print(cents / 100UL);
  out.print('.');
  const uint8_t rem = (uint8_t)(cents % 100UL);
  if (rem < 10) out.print('0');
  out.print(rem);
  out.print(F(" EUR"));
}

void printSmartPayoutRoute(Stream& out, uint8_t route) {
  if (route == 0) {
    out.print(F("cashbox"));
  } else if (route == 1) {
    out.print(F("payout"));
  } else {
    out.print(F("code "));
    out.print(route);
  }
}

const __FlashStringHelper* smartPayoutStatusLabel(uint8_t code) {
  switch (code) {
    case 0x00: return F("idle");
    case 0x01: return F("dispensing");
    case 0x02: return F("dispensed");
    case 0x07: return F("floating");
    case 0x08: return F("floated");
    case 0x0A: return F("incomplete payout");
    case 0x0B: return F("incomplete float");
    case 0x11: return F("disabled");
    case 0x12: return F("note stored");
    case 0x13: return F("slave reset");
    case 0x14: return F("note read");
    case 0x15: return F("note credit");
    case 0x16: return F("note rejecting");
    case 0x17: return F("rejected");
    case 0x18: return F("stacking");
    case 0x19: return F("stacked");
    case 0x1A: return F("note path jam");
    case 0x1B: return F("note stack jam");
    case 0x1C: return F("bill from front at start");
    case 0x1D: return F("bill stacked at start");
    case 0x1E: return F("cashbox full");
    case 0x1F: return F("cashbox removed");
    case 0x20: return F("cashbox replaced");
    case 0x27: return F("smart emptying");
    case 0x28: return F("smart emptied");
    case 0x34: return F("barcode escrow");
    case 0x35: return F("barcode stacked");
    case 0x39: return F("bill held in bezel");
    case 0x3C: return F("bill stored at startup");
    default:   return F("status non mappato");
  }
}

bool smartPayoutValueCentsToEuro(uint32_t valueCents, uint8_t& euro) {
  euro = 0;
  if (valueCents == 0 || (valueCents % 100UL) != 0) return false;

  const uint32_t valueEuro = valueCents / 100UL;
  if (valueEuro == 0 || valueEuro > 255UL) return false;

  euro = (uint8_t)valueEuro;
  return true;
}

bool smartPayoutIsCreditEvent(uint8_t code) {
  return (code == 0x15 || code == 0x1D || code == 0x3C);
}
}

CcTalkBillValidator::CcTalkBillValidator(const BillValidatorDataset& dataset)
  : _dataset(dataset) {
  resetState();
}

bool CcTalkBillValidator::addressEnabled(uint8_t addr) const {
  if (addr < kAddrMin || addr > kAddrMax) return false;
  return (_addressMask & (uint16_t)(1u << (addr - kAddrMin))) != 0;
}

bool CcTalkBillValidator::matches(uint8_t addr) const {
  return addressEnabled(addr);
}

void CcTalkBillValidator::setAddressMask(uint16_t mask) {
  _addressMask = (uint16_t)(mask & ((1u << kStateCount) - 1u));
}

bool CcTalkBillValidator::preloadRecyclerInventory(uint8_t addr,
                                                   uint16_t c10,
                                                   uint16_t c20,
                                                   uint16_t c50) {
  BillValidatorState* state = mutableStateFor(addr);
  if (!state) return false;

  state->recyclerCount10 = c10;
  state->recyclerCount20 = c20;
  state->recyclerCount50 = c50;
  refreshRecyclerTotals(*state);
  state->recyclerInventoryValid = true;
  return true;
}

void CcTalkBillValidator::injectAcceptedEuro(uint8_t addr, uint32_t euros) {
  BillValidatorState* s = mutableStateFor(addr);
  if (!s || euros == 0) return;
  s->present = true;
  s->acceptedTotalEuro += euros;
}

void CcTalkBillValidator::resetState() {
  // L'array viene azzerato integralmente e poi ogni slot riceve il proprio
  // indirizzo logico, cosi le lookup restano O(1) senza strutture dinamiche.
  memset(_states, 0, sizeof(_states));
  for (uint8_t i = 0; i < kStateCount; i++) {
    _states[i].addr = (uint8_t)(kAddrMin + i);
  }
}

CcTalkBillValidator::BillValidatorState* CcTalkBillValidator::mutableStateFor(uint8_t addr) {
  if (!addressEnabled(addr)) return nullptr;
  return &_states[(uint8_t)(addr - kAddrMin)];
}

const CcTalkBillValidator::BillValidatorState* CcTalkBillValidator::stateFor(uint8_t addr) const {
  if (!addressEnabled(addr)) return nullptr;
  return &_states[(uint8_t)(addr - kAddrMin)];
}

void CcTalkBillValidator::updateState(const CcTalkTransaction& t) {
  // Qui avviene la vera "memoria" del decoder:
  // a partire dai frame sniffati, la classe ricostruisce un modello runtime
  // del bill validator per ogni indirizzo osservato.
  if (!t.hasReq && !t.hasResp) return;

  const uint8_t addr = t.hasReq ? t.req.dest : t.resp.src;
  BillValidatorState* state = mutableStateFor(addr);
  if (!state) return;

  state->present = true;

  if (!t.hasReq || !t.hasResp) return;

  const CcTalkFrame& req = t.req;
  const CcTalkFrame& resp = t.resp;

  // Aggiorniamo lo stato solo su ACK con payload semanticamente valido.
  if (resp.hdr == 0x00) {
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
        state->lastStatusPayloadValid = false;
        state->lastStatusPayloadLen = 0;
        state->pendingAcceptedRouteValid = false;
        state->pendingAcceptedRouteEuro = 0;
        return;

      case 0x99:
        if (req.dataLen == 1) {
          state->operatingMode = req.data[0];
          state->operatingModeValid = true;
        }
        return;

      case 0x98:
        if (resp.dataLen == 1) {
          state->operatingMode = resp.data[0];
          state->operatingModeValid = true;
        }
        return;

      case 0xE4:
        if (req.dataLen == 1) {
          state->masterInhibitOff = ((req.data[0] & 0x01) != 0);
          state->masterInhibitValid = true;
        }
        return;

      case 0xE3:
        if (resp.dataLen == 1) {
          state->masterInhibitOff = ((resp.data[0] & 0x01) != 0);
          state->masterInhibitValid = true;
        }
        return;

      case 0xE7:
        if (req.dataLen > 0) {
          uint8_t n = req.dataLen;
          if (n > sizeof(state->inhibitMask)) n = sizeof(state->inhibitMask);
          memcpy(state->inhibitMask, req.data, n);
          state->inhibitMaskLen = n;
          state->inhibitMaskValid = true;
        }
        return;

      case 0xE6:
        if (resp.dataLen > 0) {
          uint8_t n = resp.dataLen;
          if (n > sizeof(state->inhibitMask)) n = sizeof(state->inhibitMask);
          memcpy(state->inhibitMask, resp.data, n);
          state->inhibitMaskLen = n;
          state->inhibitMaskValid = true;
        }
        return;

      case 0x9D:
        if (req.dataLen == 1) {
          const uint8_t idx = req.data[0];
          if (idx < (sizeof(state->billIds) / sizeof(state->billIds[0]))) {
            copyAsciiSanitized(state->billIds[idx].id, sizeof(state->billIds[idx].id), resp.data, resp.dataLen);
            state->billIds[idx].valid = true;
          }
        }
        return;

      case 0x61:
        accumulateMd100Dispense(*state, req);
        return;

      case 0x16:
        accumulateSmartPayoutDispense(*state, req);
        return;

      case 0x5E:
        applyMd100RecyclerInventory(*state, resp.data, resp.dataLen);
        return;

      case 0x1A:
        applySmartPayoutRecyclerInventory(*state, req, resp);
        return;

      case 0x20:
        applyIproRecycleCurrencySetting(*state, req.data, req.dataLen);
        return;

      case 0x24:
        applyIproRecyclerCurrent(*state, resp.data, resp.dataLen);
        return;

      case 0x9F:
        if (resp.dataLen == 11) {
          // 0x9F restituisce un event counter e fino a 5 coppie evento.
          // Il counter permette di capire quanti eventi nuovi processare.
          state->eventCounter = resp.data[0];
          state->eventCounterValid = true;
          state->lastFaultValid = false;

          for (uint8_t i = 0; i < 5; i++) {
            const uint8_t a = resp.data[1 + (i * 2)];
            const uint8_t b = resp.data[2 + (i * 2)];
            state->events[i].a = a;
            state->events[i].b = b;
            if (!state->lastFaultValid && a == 0 && b > 21) {
              state->lastFaultCode = b;
              state->lastFaultValid = true;
            }
          }

          accumulateAcceptedBills(*state, resp);
          state->lastProcessedEventCounter = resp.data[0];
          state->eventCounterSeen = true;
        }
        return;

      case 0x1D:
        if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
          accumulateSmartPayoutStatus(*state, resp.data, resp.dataLen);
        }
        return;

      default:
        return;
    }
  }
}

void CcTalkBillValidator::dumpState(Stream& out) const {
  // Dump completo del modello runtime, pensato per diagnostica dal monitor seriale.
  out.println(F("[STATE] BILL_VALIDATOR"));
  for (uint8_t i = 0; i < kStateCount; i++) {
    const BillValidatorState& s = _states[i];
    if (!s.present) continue;

    out.print(F("  addr="));
    out.print(s.addr);
    out.print(F(" resetCount="));
    out.print(s.resetCount);
    out.print(F(" eventCounter="));
    if (s.eventCounterValid) out.println(s.eventCounter);
    else out.println(F("n/a"));

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
    if (s.operatingModeValid) {
      out.print(F("    operatingMode=0x"));
      if (s.operatingMode < 16) out.print('0');
      out.println(s.operatingMode, HEX);
    }
    if (s.masterInhibitValid) {
      out.print(F("    masterInhibit="));
      out.println(s.masterInhibitOff ? F("OFF(enabled)") : F("ON(blocked)"));
    }
    if (s.inhibitMaskValid) {
      out.print(F("    inhibitMask: "));
      dumpHex(out, s.inhibitMask, s.inhibitMaskLen);
      out.println();
    }
    if (s.iproRecycleBoxMapValid) {
      out.print(F("    iproRecycleBox1="));
      out.print(s.iproRecycleBoxEuro[0]);
      out.print(F(" EUR iproRecycleBox2="));
      out.print(s.iproRecycleBoxEuro[1]);
      out.println(F(" EUR"));
    }
    if (s.recyclerInventoryValid) {
      out.print(F("    recycler10="));
      out.print(s.recyclerCount10);
      out.print(F(" recycler20="));
      out.print(s.recyclerCount20);
      out.print(F(" recycler50="));
      out.print(s.recyclerCount50);
      out.print(F(" recyclerTotal="));
      out.print(s.recyclerInventoryTotalEuro);
      out.println(F(" EUR"));
    }
    out.print(F("    acceptedTotal="));
    out.print(s.acceptedTotalEuro);
    out.println(F(" EUR"));
    out.print(F("    cassa="));
    out.print(s.cashboxTotalEuro);
    out.println(F(" EUR"));
    out.print(F("    dispensedTotal="));
    out.print(s.dispensedTotalEuro);
    out.println(F(" EUR"));
    out.print(F("    netBalance="));
    const int32_t net = (int32_t)s.acceptedTotalEuro - (int32_t)s.dispensedTotalEuro;
    out.print(net);
    out.println(F(" EUR"));
    if (s.lastAcceptedValid) {
      out.print(F("    lastAccepted billType="));
      out.print(s.lastAcceptedBillType);
      out.print(F(" value="));
      out.print(s.lastAcceptedEuro);
      out.println(F(" EUR"));
    }
    if (s.lastCashboxValid) {
      out.print(F("    lastCashbox value="));
      out.print(s.lastCashboxEuro);
      out.println(F(" EUR"));
    }
    if (s.lastDispensedValid) {
      out.print(F("    lastDispensed value="));
      out.print(s.lastDispensedEuro);
      out.println(F(" EUR"));
    }
    if (s.lastFaultValid) {
      out.print(F("    lastFaultCode="));
      out.println(s.lastFaultCode);
    }

    for (uint8_t e = 0; e < 5; e++) {
      if (s.events[e].a == 0 && s.events[e].b == 0) continue;
      out.print(F("    ev"));
      out.print((uint8_t)(e + 1));
      out.print(F(": A="));
      out.print(s.events[e].a);
      out.print(F(" B="));
      out.println(s.events[e].b);
    }

    for (uint8_t idx = 0; idx < (sizeof(s.billIds) / sizeof(s.billIds[0])); idx++) {
      if (!s.billIds[idx].valid) continue;
      out.print(F("    billId["));
      out.print(idx);
      out.print(F("]=\""));
      out.print(s.billIds[idx].id);
      out.println('"');
    }
  }
}

const __FlashStringHelper* CcTalkBillValidator::cmdDesc(uint8_t hdr) const {
  // Catalogo specifico BV: piu dettagliato del fallback generico del master.
  switch (hdr) {
    case 0xE7: return F("modify inhibit status (231/E7)");
    case 0xE6: return F("request inhibit status (230/E6)");
    case 0xE4: return F("modify master inhibit status (228/E4)");
    case 0xE3: return F("request master inhibit status (227/E3)");
    case 0x9F: return F("read buffered bill events (159/9F)");
    case 0x9E: return F("modify bill id (158/9E)");
    case 0x9D: return F("request bill id (157/9D)");
    case 0x9C: return F("request country scaling factor (156/9C)");
    case 0x9B: return F("request bill position (155/9B)");
    case 0x9A: return F("route bill (154/9A)");
    case 0x99: return F("modify bill operating mode (153/99)");
    case 0x98: return F("request bill operating mode (152/98)");
    case 0x97: return F("test lamps (151/97)");
    case 0x96: return F("request individual accept counter (150/96)");
    case 0x95: return F("request individual error counter (149/95)");
    case 0x94: return F("read opto voltages (148/94)");
    case 0x93: return F("perform stacker cycle (147/93)");
    case 0x92: return F("operate bi-directional motors (146/92)");
    case 0x91: return F("request currency revision (145/91)");
    case 0x90: return F("upload bill tables (144/90)");
    case 0x8F: return F("begin bill table upgrade (143/8F)");
    case 0x8E: return F("finish bill table upgrade (142/8E)");
    case 0x8D: return F("request firmware upgrade capability (141/8D)");
    case 0x8C: return F("upload firmware (140/8C)");
    case 0x8B: return F("begin firmware upgrade (139/8B)");
    case 0x8A: return F("finish firmware upgrade (138/8A)");
    case 0x89: return F("switch encryption code (137/89)");
    case 0x88: return F("store encryption code (136/88)");
    case 0x87: return F("set accept limit (135/87)");
    case 0x81: return F("read barcode data (129/81)");
    case 0x6B: return F("operate escrow (107/6B)");
    case 0x6A: return F("request escrow status (106/6A)");
    case 0x61:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE) {
        return F("payout/transfer bill (OEM 0x61)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x5E:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_MD100_PAYLOAD) {
        return F("recycler inventory status (OEM 0x5E)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x14:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("set routing (020/14)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x15:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("get routing (021/15)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x16:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("payout amount (022/16)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x17:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("float amount (023/17)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x18:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("empty payout store (024/18)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x19:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("get minimum payout (025/19)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x1A:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_NOTE_AMOUNT) {
        return F("get note amount (026/1A)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x1D:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
        return F("request status (029/1D)");
      }
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT) {
        return F("request recycler status (029/1D)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x20:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT) {
        return F("modify recycle currency setting (032/20)");
      }
      return CcTalkMaster::headerDesc(hdr);
    case 0x24:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT) {
        return F("request recycle current count (036/24)");
      }
      return CcTalkMaster::headerDesc(hdr);
    default:   return CcTalkMaster::headerDesc(hdr);
  }
}

void CcTalkBillValidator::printRequest(Stream& out, const CcTalkFrame& req) {
  out.print(F("MASTER -> BV["));
  out.print(req.dest);
  out.print(F("]: "));
  out.print(cmdDesc(req.hdr));
  out.print(F(" (0x"));
  if (req.hdr < 16) out.print('0');
  out.print(req.hdr, HEX);
  out.println(F(")"));
  printRequestPayload(out, req);
}

void CcTalkBillValidator::printRequestPayload(Stream& out, const CcTalkFrame& req) {
  // Interpreta solo i payload che aggiungono contesto utile al log.
  switch (req.hdr) {
    case 0x61:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE) return;
      if (req.dataLen > 0) {
        uint8_t code = 0;
        const int16_t denom = payoutDenomFromRequest(req, code);
        if (denom > 0) {
          out.print(F("  payload: denom="));
          out.print(denom);
          out.print(F(" EUR (code=0x"));
          if (code < 16) out.print('0');
          out.print(code, HEX);
          out.println(F(")"));
        } else {
          out.print(F("  payload raw: "));
          dumpHex(out, req.data, req.dataLen);
          out.println();
        }
      }
      return;

    case 0x14:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32 && req.dataLen >= 5) {
        out.print(F("  payload route="));
        printSmartPayoutRoute(out, req.data[0]);
        out.print(F("  payload value="));
        printMoneyCents(out, readU32LE(&req.data[1]));
        out.println();
      }
      return;

    case 0x15:
    case 0x16:
    case 0x1A:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32 && req.dataLen >= 4) {
        out.print(F("  payload value="));
        printMoneyCents(out, readU32LE(req.data));
        out.println();
      }
      return;

    case 0x17:
      if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32 && req.dataLen >= 8) {
        out.print(F("  payload minPayout="));
        printMoneyCents(out, readU32LE(req.data));
        out.print(F(" target="));
        printMoneyCents(out, readU32LE(&req.data[4]));
        out.println();
      }
      return;

    case 0x20:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT &&
          req.data && req.dataLen >= 4) {
        const uint8_t start = ((req.dataLen > 1) && (((uint8_t)(req.dataLen - 1) % 3u) == 0)) ? 1 : 0;
        out.print(F("  payload recycleCurrency:"));
        for (uint8_t pos = start; (uint8_t)(pos + 2u) < req.dataLen; pos = (uint8_t)(pos + 3u)) {
          const uint16_t billType = readU16LE(&req.data[pos]);
          const uint8_t box = req.data[pos + 2u];
          uint8_t euro = 0;
          out.print(F(" box"));
          out.print(box);
          out.print('=');
          if (billType <= 255u && billTypeToEuro((uint8_t)billType, euro)) {
            out.print(euro);
            out.print(F("EUR"));
          } else {
            out.print(F("billType"));
            out.print(billType);
          }
        }
        out.println();
      }
      return;

    case 0x24:
      if (_dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT &&
          req.dataLen == 1) {
        out.print(F("  payload boxSelector="));
        out.println(req.data[0]);
      }
      return;

    case 0xE7:
      if (req.dataLen >= 2) {
        out.print(F("  payload inhibit: "));
        dumpHex(out, req.data, req.dataLen);
        out.println();
      }
      return;

    case 0xE4:
      if (req.dataLen == 1) {
        out.print(F("  payload: masterInhibit="));
        out.println((req.data[0] & 0x01) ? F("OFF (enabled)") : F("ON (blocked)"));
      }
      return;

    case 0x9A:
      if (req.dataLen == 1) {
        out.print(F("  payload route="));
        if (req.data[0] == 0) out.println(F("return bill"));
        else if (req.data[0] == 1) out.println(F("stack/cashbox"));
        else if (req.data[0] == 255) out.println(F("extend escrow timeout"));
        else { out.print(F("code ")); out.println(req.data[0]); }
      }
      return;

    case 0x99:
      if (req.dataLen == 1) {
        const uint8_t mode = req.data[0];
        out.print(F("  payload mode=0x"));
        if (mode < 16) out.print('0');
        out.print(mode, HEX);
        out.print(F(" stacker="));
        out.print((mode & 0x01) ? F("on") : F("off"));
        out.print(F(" escrow="));
        out.println((mode & 0x02) ? F("on") : F("off"));
      }
      return;

    case 0x97:
      if (req.dataLen == 2) {
        out.print(F("  payload lamp="));
        out.print(req.data[0]);
        out.print(F(" control="));
        out.println(req.data[1]);
      }
      return;

    case 0x96: case 0x95: case 0x9D:
      if (req.dataLen == 1) {
        out.print(F("  payload index="));
        out.println(req.data[0]);
      }
      return;

    case 0x9E:
      if (req.dataLen >= 2) {
        out.print(F("  payload billType="));
        out.print(req.data[0]);
        out.print(F(" id=\""));
        dumpAscii(out, &req.data[1], (uint8_t)(req.dataLen - 1));
        out.println(F("\""));
      }
      return;

    case 0x9C: case 0x9B: case 0x91:
      if (req.dataLen == 2) {
        out.print(F("  payload country=\""));
        out.print((char)req.data[0]);
        out.print((char)req.data[1]);
        out.println(F("\""));
      }
      return;

    case 0x92:
      if (req.dataLen == 3) {
        out.print(F("  payload motors mask=0x"));
        if (req.data[0] < 16) out.print('0');
        out.print(req.data[0], HEX);
        out.print(F(" dir=0x"));
        if (req.data[1] < 16) out.print('0');
        out.print(req.data[1], HEX);
        out.print(F(" speed="));
        out.println(req.data[2]);
      }
      return;

    case 0x87:
      if (req.dataLen == 1) {
        out.print(F("  payload acceptLimit="));
        out.println(req.data[0]);
      }
      return;

    default:
      return;
  }
}

const __FlashStringHelper* CcTalkBillValidator::billEventLabel(uint8_t a, uint8_t b) const {
  // Nel protocollo BV la coppia (A,B) ha semantica dipendente dal contesto:
  // A=tipo banconota / categoria evento, B=esito o sottocodice.
  if (isRecyclerStoredEvent(a, b)) return F("stored in recycler");
  if (b == 0 && a >= 1) return F("accepted credit");
  if (b == 1 && a >= 1) return F("pending credit (escrow)");

  if (a != 0) return F("event non standard");

  switch (b) {
    case 0:  return F("master inhibit active / slot empty");
    case 1:  return F("bill returned from escrow");
    case 2:  return F("invalid bill (validation fail)");
    case 3:  return F("invalid bill (transport problem)");
    case 4:  return F("inhibited bill (serial)");
    case 5:  return F("inhibited bill (DIP)");
    case 6:  return F("bill jammed in transport (unsafe)");
    case 7:  return F("bill jammed in stacker");
    case 8:  return F("bill pulled backwards");
    case 9:  return F("bill tamper");
    case 10: return F("stacker OK");
    case 11: return F("stacker removed");
    case 12: return F("stacker inserted");
    case 13: return F("stacker faulty");
    case 14: return F("stacker full");
    case 15: return F("stacker jammed");
    case 16: return F("bill jammed in transport (safe)");
    case 17: return F("opto fraud detected");
    case 18: return F("string fraud detected");
    case 19: return F("anti-string mechanism faulty");
    case 20: return F("barcode detected");
    case 21: return F("unknown bill type stacked");
    default: return billFaultLabel(b);
  }
}

const __FlashStringHelper* CcTalkBillValidator::billEventClass(uint8_t a, uint8_t b) const {
  // Raggruppamento didattico degli eventi in categorie leggibili a colpo d'occhio.
  if (isRecyclerStoredEvent(a, b)) return F("Credit (Recycler)");
  if (b == 0 && a >= 1) return F("Credit (Accepted)");
  if (b == 1 && a >= 1) return F("Pending Credit");
  if (a != 0) return F("Unknown");

  switch (b) {
    case 0:  return F("Status");
    case 1:  return F("Status");
    case 2:  return F("Reject");
    case 3:  return F("Reject");
    case 4:  return F("Status");
    case 5:  return F("Status");
    case 6:  return F("Fatal Error");
    case 7:  return F("Fatal Error");
    case 8:  return F("Fraud Attempt");
    case 9:  return F("Fraud Attempt");
    case 10: return F("Status");
    case 11: return F("Status");
    case 12: return F("Status");
    case 13: return F("Fatal Error");
    case 14: return F("Status");
    case 15: return F("Fatal Error");
    case 16: return F("Fatal Error");
    case 17: return F("Fraud Attempt");
    case 18: return F("Fraud Attempt");
    case 19: return F("Fatal Error");
    case 20: return F("Status");
    case 21: return F("Status");
    default: return F("Fault");
  }
}

const __FlashStringHelper* CcTalkBillValidator::billFaultLabel(uint8_t code) const {
  switch (code) {
    case 22: return F("fault code 22: fault on thermistor");
    case 23: return F("fault code 23: payout motor fault");
    case 24: return F("fault code 24: payout timeout");
    case 25: return F("fault code 25: payout jammed");
    case 26: return F("fault code 26: payout sensor fault");
    case 27: return F("fault code 27: level sensor error");
    case 28: return F("fault code 28: personality module not fitted");
    case 29: return F("fault code 29: personality checksum corrupted");
    case 30: return F("fault code 30: ROM checksum mismatch");
    case 31: return F("fault code 31: missing slave device");
    case 32: return F("fault code 32: internal comms bad");
    case 33: return F("fault code 33: supply voltage out of limits");
    case 34: return F("fault code 34: temperature out of limits");
    case 35: return F("fault code 35: D.C.E. fault");
    case 36: return F("fault code 36: bill validation sensor fault");
    case 37: return F("fault code 37: bill transport motor fault");
    case 38: return F("fault code 38: stacker fault");
    case 39: return F("fault code 39: bill jammed");
    case 40: return F("fault code 40: RAM test fail");
    case 41: return F("fault code 41: string sensor fault");
    case 42: return F("fault code 42: accept gate failed open");
    case 43: return F("fault code 43: accept gate failed closed");
    case 44: return F("fault code 44: stacker missing");
    case 45: return F("fault code 45: stacker full");
    case 46: return F("fault code 46: flash erase fail");
    case 47: return F("fault code 47: flash write fail");
    case 48: return F("fault code 48: slave device not responding");
    case 49: return F("fault code 49: opto sensor fault");
    case 50: return F("fault code 50: battery fault");
    case 51: return F("fault code 51: door open");
    case 52: return F("fault code 52: microswitch fault");
    case 53: return F("fault code 53: RTC fault");
    case 54: return F("fault code 54: firmware error");
    case 55: return F("fault code 55: initialisation error");
    case 56: return F("fault code 56: supply current out of limits");
    case 57: return F("fault code 57: forced bootloader mode");
    case 255: return F("fault code 255: unspecified");
    default: return F("event/fault non mappato");
  }
}

bool CcTalkBillValidator::isAcceptedCreditEvent(uint8_t a, uint8_t b) const {
  if (a == 0) return false;
  return (b == 0) || isRecyclerStoredEvent(a, b);
}

bool CcTalkBillValidator::isCashboxCreditEvent(uint8_t a, uint8_t b) const {
  return (a != 0 && b == 0);
}

bool CcTalkBillValidator::isRecyclerStoredEvent(uint8_t a, uint8_t b) const {
  return (a != 0 && b == (uint8_t)(a + 15));
}

bool CcTalkBillValidator::billTypeToEuro(uint8_t billType, uint8_t& euro) const {
  uint16_t value = 0;
  if (!billValidatorDatasetLookupEuro(_dataset.billTypeMap, _dataset.billTypeMapCount, billType, value)) {
    return false;
  }
  euro = (uint8_t)value;
  return true;
}

bool CcTalkBillValidator::billIdToEuro(const char* billId, uint16_t& euro) const {
  euro = 0;
  if (!billId || billId[0] == '\0') return false;

  uint32_t value = 0;
  uint8_t digitCount = 0;
  for (const char* p = billId; *p; ++p) {
    const char c = *p;
    if (c >= '0' && c <= '9') {
      if (digitCount >= 4) break;
      value = (value * 10UL) + (uint32_t)(c - '0');
      digitCount++;
      continue;
    }
    if (digitCount > 0) break;
  }

  if (digitCount == 0 || value == 0 || value > 255UL) return false;
  euro = (uint16_t)value;
  return true;
}

bool CcTalkBillValidator::billTypeToEuro(const BillValidatorState& state,
                                         uint8_t billType,
                                         uint8_t& euro) const {
  if (billTypeToEuro(billType, euro)) return true;

  uint16_t value = 0;
  const uint8_t maxIds = (uint8_t)(sizeof(state.billIds) / sizeof(state.billIds[0]));

  if (billType < maxIds &&
      state.billIds[billType].valid &&
      billIdToEuro(state.billIds[billType].id, value)) {
    euro = (uint8_t)value;
    return true;
  }

  if (billType > 0 &&
      (uint8_t)(billType - 1) < maxIds &&
      state.billIds[(uint8_t)(billType - 1)].valid &&
      billIdToEuro(state.billIds[(uint8_t)(billType - 1)].id, value)) {
    euro = (uint8_t)value;
    return true;
  }

  return false;
}

int16_t CcTalkBillValidator::payoutDenomFromRequest(const CcTalkFrame& req, uint8_t& code) const {
  // Alcuni modelli usano codici OEM, altri valori monetari a 32 bit.
  code = 0;
  if (req.dataLen == 0 || !req.data) return -1;

  if (_dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) {
    uint32_t valueCents = 0;
    if (!requestValueCents(req, valueCents)) return -1;
    const int16_t denom = recyclerDenomFromKey(valueCents);
    if (denom > 0) return denom;
    if ((valueCents % 100UL) != 0) return -1;
    const uint32_t euro = valueCents / 100UL;
    return (euro <= 32767UL) ? (int16_t)euro : -1;
  }

  for (uint8_t i = 0; i < req.dataLen; i++) {
    const int16_t denom = recyclerDenomFromKey(req.data[i]);
    if (denom > 0) {
      code = req.data[i];
      return denom;
    }
  }

  return -1;
}

bool CcTalkBillValidator::requestValueCents(const CcTalkFrame& req, uint32_t& valueCents) const {
  valueCents = 0;
  if (!req.data || req.dataLen < 4) return false;

  if (req.hdr == 0x14) {
    if (req.dataLen < 5) return false;
    valueCents = readU32LE(&req.data[1]);
    return true;
  }

  valueCents = readU32LE(req.data);
  return true;
}

bool CcTalkBillValidator::responseCountValue(const uint8_t* data, uint8_t len, uint16_t& count) const {
  count = 0;
  if (!data || len == 0) return false;

  uint32_t raw = 0;
  uint8_t bytes = len;
  if (bytes > 4) bytes = 4;
  for (uint8_t i = 0; i < bytes; i++) {
    raw |= ((uint32_t)data[i] << (8 * i));
  }

  if (raw > 65535UL) raw = 65535UL;
  count = (uint16_t)raw;
  return true;
}

uint8_t CcTalkBillValidator::eventCounterDelta(uint8_t previous, uint8_t current) const {
  // Calcola quanti eventi nuovi sono comparsi, gestendo anche il rollover del
  // contatore a 8 bit senza considerare come validi i salti anomali.
  if (current == previous) return 0;
  if (current > previous) return (uint8_t)(current - previous);

  // Rollover plausibile del counter a 8 bit.
  if (previous > 220 && current < 32) {
    // I validator ccTalk usano normalmente il range 1..255: il rollover reale
    // e 255 -> 1, quindi 255 non va contato due volte.
    return (uint8_t)(current + (uint8_t)(255 - previous));
  }

  // Salto anomalo (es. reset BV): riallinea senza accumulare.
  return 0;
}

void CcTalkBillValidator::accumulateAcceptedBills(BillValidatorState& state, const CcTalkFrame& resp) const {
  // Converte gli ultimi eventi bufferizzati in variazioni dei totalizzatori.
  // Il contatore evita di sommare piu volte gli stessi eventi in poll ripetuti.
  if (!state.eventCounterSeen) return;

  const uint8_t delta = eventCounterDelta(state.lastProcessedEventCounter, resp.data[0]);
  if (delta == 0) return;

  uint8_t eventsToProcess = delta;
  if (eventsToProcess > 5) eventsToProcess = 5;

  for (uint8_t i = 0; i < eventsToProcess; i++) {
    const uint8_t a = resp.data[1 + (i * 2)];
    const uint8_t b = resp.data[2 + (i * 2)];
    if (!isAcceptedCreditEvent(a, b)) continue;

    uint8_t denom = 0;
    if (!billTypeToEuro(state, a, denom)) continue;

    uint8_t iproRecyclerEuro = 0;
    const bool iproRecyclerCredit =
        isCashboxCreditEvent(a, b) && iproBillTypeToRecyclerEuro(state, a, iproRecyclerEuro);

    state.acceptedTotalEuro += denom;
    state.lastAcceptedValid = true;
    state.lastAcceptedBillType = a;
    state.lastAcceptedEuro = denom;

    if ((usesMd100RecyclerInventory() && isRecyclerStoredEvent(a, b)) ||
        iproRecyclerCredit) {
      applyRecyclerDelta(state, iproRecyclerCredit ? iproRecyclerEuro : denom, +1);
    }

    if (isCashboxCreditEvent(a, b) && !iproRecyclerCredit) {
      state.cashboxTotalEuro += denom;
      state.lastCashboxValid = true;
      state.lastCashboxEuro = denom;
    }
  }
}

uint8_t CcTalkBillValidator::smartPayoutStatusDataLen(uint8_t code) const {
  switch (code) {
    case 0x00:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1E:
    case 0x1F:
    case 0x20:
    case 0x34:
    case 0x35:
      return 0;

    case 0x01:
    case 0x02:
    case 0x07:
    case 0x08:
    case 0x0A:
    case 0x0B:
    case 0x14:
    case 0x15:
    case 0x1C:
    case 0x1D:
    case 0x27:
    case 0x28:
    case 0x39:
    case 0x3C:
      return 4;

    default:
      return 0xFF;
  }
}

bool CcTalkBillValidator::usesMd100PayoutCommands() const {
  return _dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE;
}

bool CcTalkBillValidator::usesSmartPayoutValueCommands() const {
  return _dataset.payoutCommandMode == BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32;
}

bool CcTalkBillValidator::usesMd100RecyclerInventory() const {
  return _dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_MD100_PAYLOAD;
}

bool CcTalkBillValidator::usesSmartPayoutRecyclerInventory() const {
  return _dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_NOTE_AMOUNT;
}

bool CcTalkBillValidator::usesIproRecyclerInventory() const {
  return _dataset.recyclerInventoryMode == BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT;
}

void CcTalkBillValidator::accumulateMd100Dispense(BillValidatorState& state,
                                                  const CcTalkFrame& req) const {
  if (!usesMd100PayoutCommands()) return;

  uint8_t code = 0;
  const int16_t denom = payoutDenomFromRequest(req, code);
  if (denom <= 0) return;

  state.dispensedTotalEuro += (uint32_t)denom;
  state.lastDispensedValid = true;
  state.lastDispensedEuro = (uint8_t)denom;
  applyRecyclerDelta(state, (uint8_t)denom, -1);
}

void CcTalkBillValidator::accumulateSmartPayoutDispense(BillValidatorState& state,
                                                        const CcTalkFrame& req) const {
  if (!usesSmartPayoutValueCommands()) return;

  uint8_t code = 0;
  const int16_t denom = payoutDenomFromRequest(req, code);
  if (denom <= 0) return;

  state.dispensedTotalEuro += (uint32_t)denom;
  state.lastDispensedValid = true;
  state.lastDispensedEuro = (uint8_t)denom;
  applyRecyclerDelta(state, (uint8_t)denom, -1);
}

bool CcTalkBillValidator::applyRecyclerDelta(BillValidatorState& state,
                                             uint8_t euro,
                                             int8_t delta) const {
  if (delta == 0) return false;

  if (!state.recyclerInventoryValid) {
    if (!usesMd100RecyclerInventory() &&
        !usesSmartPayoutRecyclerInventory() &&
        !usesIproRecyclerInventory()) {
      return false;
    }
    state.recyclerCount10 = 0;
    state.recyclerCount20 = 0;
    state.recyclerCount50 = 0;
    state.recyclerInventoryTotalEuro = 0;
    state.recyclerInventoryValid = true;
  }

  uint16_t* count = nullptr;
  switch (euro) {
    case 10:
      count = &state.recyclerCount10;
      break;
    case 20:
      count = &state.recyclerCount20;
      break;
    case 50:
      count = &state.recyclerCount50;
      break;
    default:
      return false;
  }

  if (delta > 0) {
    const uint16_t add = (uint16_t)delta;
    const uint32_t next = (uint32_t)(*count) + add;
    *count = (next > 65535UL) ? 65535u : (uint16_t)next;
  } else {
    const uint16_t sub = (uint16_t)(-delta);
    *count = (*count > sub) ? (uint16_t)(*count - sub) : 0;
  }

  refreshRecyclerTotals(state);
  return true;
}

bool CcTalkBillValidator::applyIproRecycleCurrencySetting(BillValidatorState& state,
                                                          const uint8_t* data,
                                                          uint8_t len) const {
  if (!usesIproRecyclerInventory() || !data || len < 3) return false;

  uint8_t nextBoxEuro[2] = {0, 0};
  bool any = false;
  uint8_t pos = ((len > 1) && (((uint8_t)(len - 1) % 3u) == 0)) ? 1 : 0;

  while ((uint8_t)(pos + 2u) < len) {
    const uint16_t billType = readU16LE(&data[pos]);
    const uint8_t box = data[pos + 2u];
    uint8_t euro = 0;
    if (box >= 1 && box <= 2 && billType <= 255u && billTypeToEuro((uint8_t)billType, euro)) {
      nextBoxEuro[(uint8_t)(box - 1u)] = euro;
      any = true;
    }
    pos = (uint8_t)(pos + 3u);
  }

  if (!any) return false;
  state.iproRecycleBoxEuro[0] = nextBoxEuro[0];
  state.iproRecycleBoxEuro[1] = nextBoxEuro[1];
  state.iproRecycleBoxMapValid = true;
  return true;
}

bool CcTalkBillValidator::applyIproRecyclerCurrent(BillValidatorState& state,
                                                   const uint8_t* data,
                                                   uint8_t len) const {
  if (!usesIproRecyclerInventory() || !data || len < 4 || !state.iproRecycleBoxMapValid) {
    return false;
  }

  const uint16_t countBox1 = readU16LE(&data[0]);
  const uint16_t countBox2 = readU16LE(&data[2]);
  const uint8_t euros[2] = {state.iproRecycleBoxEuro[0], state.iproRecycleBoxEuro[1]};
  const uint16_t counts[2] = {countBox1, countBox2};

  for (uint8_t i = 0; i < 2; i++) {
    switch (euros[i]) {
      case 10:
        state.recyclerCount10 = counts[i];
        break;
      case 20:
        state.recyclerCount20 = counts[i];
        break;
      case 50:
        state.recyclerCount50 = counts[i];
        break;
      default:
        break;
    }
  }

  refreshRecyclerTotals(state);
  state.recyclerInventoryValid = true;
  return true;
}

bool CcTalkBillValidator::iproBillTypeToRecyclerEuro(const BillValidatorState& state,
                                                     uint8_t billType,
                                                     uint8_t& euro) const {
  euro = 0;
  if (!usesIproRecyclerInventory() || !state.iproRecycleBoxMapValid) return false;

  uint8_t candidate = 0;
  if (!billTypeToEuro(state, billType, candidate)) return false;

  for (uint8_t i = 0; i < 2; i++) {
    if (state.iproRecycleBoxEuro[i] == candidate) {
      euro = candidate;
      return true;
    }
  }
  return false;
}

void CcTalkBillValidator::accumulateSmartPayoutStatus(BillValidatorState& state,
                                                      const uint8_t* data,
                                                      uint8_t len) const {
  if (!data || len == 0) return;

  const uint8_t firstCode = data[0];
  if (state.lastStatusPayloadValid &&
      state.lastStatusPayloadLen == len &&
      memcmp(state.lastStatusPayload, data, len) == 0 &&
      !smartPayoutIsCreditEvent(firstCode)) {
    return;
  }

  uint8_t pos = 0;
  while (pos < len) {
    const uint8_t code = data[pos++];
    const uint8_t extraLen = smartPayoutStatusDataLen(code);
    if (extraLen == 0xFF || (uint8_t)(len - pos) < extraLen) break;

    uint32_t valueCents = 0;
    if (extraLen >= 4) valueCents = readU32LE(&data[pos]);

    uint8_t euro = 0;
    const bool hasEuroValue = smartPayoutValueCentsToEuro(valueCents, euro);

    switch (code) {
      case 0x13:
        state.lastStatusPayloadValid = false;
        state.lastStatusPayloadLen = 0;
        state.pendingAcceptedRouteValid = false;
        state.pendingAcceptedRouteEuro = 0;
        break;

      case 0x15:
        if (hasEuroValue) {
          state.acceptedTotalEuro += euro;
          state.lastAcceptedValid = true;
          state.lastAcceptedBillType = 0;
          state.lastAcceptedEuro = euro;
          state.pendingAcceptedRouteValid = true;
          state.pendingAcceptedRouteEuro = euro;
        }
        break;

      case 0x12:
        // La nota e stata immagazzinata nel payout store/recycler.
        if (state.pendingAcceptedRouteValid) {
          applyRecyclerDelta(state, state.pendingAcceptedRouteEuro, +1);
        }
        state.pendingAcceptedRouteValid = false;
        state.pendingAcceptedRouteEuro = 0;
        break;

      case 0x19:
        // La nota accettata e stata effettivamente inviata allo stacker/cashbox.
        if (state.pendingAcceptedRouteValid) {
          state.cashboxTotalEuro += state.pendingAcceptedRouteEuro;
          state.lastCashboxValid = true;
          state.lastCashboxEuro = state.pendingAcceptedRouteEuro;
          state.pendingAcceptedRouteValid = false;
          state.pendingAcceptedRouteEuro = 0;
        }
        break;

      case 0x1D:
        // Recovery all'avvio: nota gia finita nello stacker prima del reboot.
        if (hasEuroValue) {
          state.acceptedTotalEuro += euro;
          state.lastAcceptedValid = true;
          state.lastAcceptedBillType = 0;
          state.lastAcceptedEuro = euro;
          state.cashboxTotalEuro += euro;
          state.lastCashboxValid = true;
          state.lastCashboxEuro = euro;
        }
        state.pendingAcceptedRouteValid = false;
        state.pendingAcceptedRouteEuro = 0;
        break;

      case 0x3C:
        // Recovery all'avvio: nota gia memorizzata nel payout store.
        if (hasEuroValue) {
          state.acceptedTotalEuro += euro;
          state.lastAcceptedValid = true;
          state.lastAcceptedBillType = 0;
          state.lastAcceptedEuro = euro;
        }
        state.pendingAcceptedRouteValid = false;
        state.pendingAcceptedRouteEuro = 0;
        break;

      case 0x16:
      case 0x17:
      case 0x1A:
      case 0x1B:
        // La nota non ha completato un percorso utile per la contabilita.
        state.pendingAcceptedRouteValid = false;
        state.pendingAcceptedRouteEuro = 0;
        break;

      default:
        break;
    }

    pos = (uint8_t)(pos + extraLen);
  }

  uint8_t copyLen = len;
  if (copyLen > sizeof(state.lastStatusPayload)) {
    copyLen = sizeof(state.lastStatusPayload);
  }
  memcpy(state.lastStatusPayload, data, copyLen);
  state.lastStatusPayloadLen = copyLen;
  state.lastStatusPayloadValid = true;
}

bool CcTalkBillValidator::applyMd100RecyclerInventory(BillValidatorState& state,
                                                      const uint8_t* data,
                                                      uint8_t len) const {
  if (!usesMd100RecyclerInventory()) return false;

  uint16_t c10 = 0;
  uint16_t c20 = 0;
  uint16_t c50 = 0;
  if (!parseRecyclerInventory(data, len, c10, c20, c50)) return false;

  state.recyclerCount10 = c10;
  state.recyclerCount20 = c20;
  state.recyclerCount50 = c50;
  refreshRecyclerTotals(state);
  state.recyclerInventoryValid = true;
  return true;
}

bool CcTalkBillValidator::applySmartPayoutRecyclerInventory(BillValidatorState& state,
                                                            const CcTalkFrame& req,
                                                            const CcTalkFrame& resp) const {
  if (!usesSmartPayoutRecyclerInventory()) return false;

  uint32_t valueCents = 0;
  uint16_t count = 0;
  if (!requestValueCents(req, valueCents) ||
      !responseCountValue(resp.data, resp.dataLen, count) ||
      !updateRecyclerCountFromValue(state, valueCents, count)) {
    return false;
  }

  state.recyclerInventoryValid = true;
  return true;
}

bool CcTalkBillValidator::parseRecyclerInventory(const uint8_t* data, uint8_t len,
                                                 uint16_t& c10, uint16_t& c20, uint16_t& c50) const {
  // Layout OEM MD100: i tre contatori d'inventario si trovano in coda payload.
  if (!usesMd100RecyclerInventory()) return false;
  if (!data || len < 41) return false;
  c10 = readU16LE(&data[35]);
  c20 = readU16LE(&data[37]);
  c50 = readU16LE(&data[39]);
  return true;
}

bool CcTalkBillValidator::updateRecyclerCountFromValue(BillValidatorState& state,
                                                       uint32_t valueCents,
                                                       uint16_t count) const {
  uint16_t euro = 0;
  if (!billValidatorDatasetLookupEuro(_dataset.recyclerMap, _dataset.recyclerMapCount, valueCents, euro)) {
    return false;
  }

  switch (euro) {
    case 10:
      state.recyclerCount10 = count;
      break;
    case 20:
      state.recyclerCount20 = count;
      break;
    case 50:
      state.recyclerCount50 = count;
      break;
    default:
      return false;
  }

  refreshRecyclerTotals(state);
  return true;
}

void CcTalkBillValidator::refreshRecyclerTotals(BillValidatorState& state) const {
  state.recyclerInventoryTotalEuro = (uint32_t)state.recyclerCount10 * 10UL +
                                     (uint32_t)state.recyclerCount20 * 20UL +
                                     (uint32_t)state.recyclerCount50 * 50UL;
}

int16_t CcTalkBillValidator::recyclerDenomFromKey(uint32_t key) const {
  uint16_t euro = 0;
  if (!billValidatorDatasetLookupEuro(_dataset.recyclerMap, _dataset.recyclerMapCount, key, euro)) {
    return -1;
  }
  return (int16_t)euro;
}

void CcTalkBillValidator::printBillEvent(Stream& out, uint8_t idx, uint8_t a, uint8_t b) {
  // Stampa una singola coppia evento in forma sia grezza sia interpretata.
  out.print(F("  ev"));
  out.print(idx);
  out.print(F(": A="));
  out.print(a);
  out.print(F(" B="));
  out.print(b);
  out.print(F(" -> "));
  out.print(billEventLabel(a, b));
  out.print(F(" ["));
  out.print(billEventClass(a, b));
  out.print(']');

  const bool recyclerStored = isRecyclerStoredEvent(a, b);
  if ((b == 0 || b == 1 || recyclerStored) && a >= 1) {
    out.print(F(" (billType="));
    out.print(a);
    uint8_t denom = 0;
    if (billTypeToEuro(a, denom) && (recyclerStored || a >= 2)) {
      out.print(F(" denom="));
      out.print(denom);
      out.print(F(" EUR"));
    }
    out.print(')');
  } else if (a == 0 && b > 21) {
    out.print(F(" [faultCode="));
    out.print(b);
    out.print(']');
  }
  out.println();
}

void CcTalkBillValidator::printBillEvents(Stream& out, const CcTalkFrame& resp) {
  // 0x9F torna sempre con 1 byte contatore + 5 coppie evento.
  out.print(F("eventCounter="));
  out.println(resp.data[0]);

  bool allZero = true;
  for (uint8_t i = 1; i < resp.dataLen; i++) {
    if (resp.data[i] != 0) { allZero = false; break; }
  }
  if (allZero) out.println(F("  note: buffer vuoto (o solo master inhibit)"));

  for (uint8_t i = 0; i < 5; i++) {
    const uint8_t a = resp.data[1 + (i * 2)];
    const uint8_t b = resp.data[2 + (i * 2)];
    printBillEvent(out, (uint8_t)(i + 1), a, b);
  }
}

void CcTalkBillValidator::printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp) {
  // La risposta viene interpretata in funzione dell'header che l'ha generata.
  // Questo rende leggibili anche gli ACK con payload strutturato.
  out.print(F("BV["));
  out.print(resp.src);
  out.print(F("] -> MASTER: "));

  if (resp.hdr == 0x05) { out.println(F("NAK")); return; }
  if (resp.hdr == 0x06) { out.println(F("BUSY")); return; }

  if (resp.hdr != 0x00) {
    out.print(F("HDR=0x"));
    if (resp.hdr < 16) out.print('0');
    out.println(resp.hdr, HEX);
    return;
  }

  switch (hostHdr) {
    case 0xFE: out.println(F("ACK (presente)")); return;
    case 0x01: out.println(F("ACK (reset eseguito)")); return;

    case 0xE7:
    case 0xE4:
    case 0x9E:
    case 0x99:
    case 0x97:
    case 0x92:
    case 0x90:
    case 0x8F:
    case 0x8E:
    case 0x8C:
    case 0x8B:
    case 0x8A:
    case 0x89:
    case 0x88:
    case 0x87:
    case 0x6B:
    case 0x14:
    case 0x17:
    case 0x18:
      out.println(F("ACK"));
      return;

    case 0x61:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE) break;
      out.println(F("ACK"));
      {
        const BillValidatorState* state = stateFor(resp.src);
        if (state) {
          if (state->lastDispensedValid) {
            out.print(F("  [MEM] lastDispensed="));
            out.print(state->lastDispensedEuro);
            out.println(F(" EUR"));
          }
          out.print(F("  [MEM] dispensedTotal="));
          out.print(state->dispensedTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] netBalance="));
          const int32_t net = (int32_t)state->acceptedTotalEuro - (int32_t)state->dispensedTotalEuro;
          out.print(net);
          out.println(F(" EUR"));
        }
      }
      return;

    case 0x15:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) break;
      if (resp.dataLen == 1) {
        out.print(F("Route="));
        printSmartPayoutRoute(out, resp.data[0]);
        out.println();
      } else {
        out.print(F("0x15 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x16:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) break;
      out.println(F("ACK"));
      {
        const BillValidatorState* state = stateFor(resp.src);
        if (state) {
          if (state->lastDispensedValid) {
            out.print(F("  [MEM] lastDispensed="));
            out.print(state->lastDispensedEuro);
            out.println(F(" EUR"));
          }
          out.print(F("  [MEM] dispensedTotal="));
          out.print(state->dispensedTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] netBalance="));
          const int32_t net = (int32_t)state->acceptedTotalEuro - (int32_t)state->dispensedTotalEuro;
          out.print(net);
          out.println(F(" EUR"));
        }
      }
      return;

    case 0x19:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) break;
      if (resp.dataLen >= 4) {
        out.print(F("MinimumPayout="));
        printMoneyCents(out, readU32LE(resp.data));
        out.println();
      } else {
        out.print(F("0x19 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0xF6: case 0xF5: case 0xF4: case 0xF1: case 0xC0: case 0xFF:
      out.print('"');
      dumpAscii(out, resp.data, resp.dataLen);
      out.println('"');
      return;

    case 0xF2:
      if (resp.dataLen == 3) {
        out.print(F("Serial="));
        out.println(readU24LE(resp.data));
      } else {
        out.print(F("F2 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x04:
      if (resp.dataLen == 3) {
        out.print(F("Level="));
        out.print(resp.data[0]);
        out.print(F(" Major="));
        out.print(resp.data[1]);
        out.print(F(" Minor="));
        out.println(resp.data[2]);
      } else {
        out.print(F("04 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x9F:
      if (resp.dataLen == 11) {
        printBillEvents(out, resp);
        const BillValidatorState* state = stateFor(resp.src);
        if (state) {
          out.print(F("  [MEM] acceptedTotal="));
          out.print(state->acceptedTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] Cassa="));
          out.print(state->cashboxTotalEuro);
          out.println(F(" EUR"));
          if (state->recyclerInventoryValid) {
            out.print(F("  [MEM] Recycler: 10EUR="));
            out.print(state->recyclerCount10);
            out.print(F(" 20EUR="));
            out.print(state->recyclerCount20);
            out.print(F(" 50EUR="));
            out.print(state->recyclerCount50);
            out.print(F(" total="));
            out.print(state->recyclerInventoryTotalEuro);
            out.println(F(" EUR"));
          }
          out.print(F("  [MEM] dispensedTotal="));
          out.print(state->dispensedTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] netBalance="));
          const int32_t net = (int32_t)state->acceptedTotalEuro - (int32_t)state->dispensedTotalEuro;
          out.print(net);
          out.println(F(" EUR"));
          if (state->lastAcceptedValid) {
            out.print(F("  [MEM] lastAccepted billType="));
            out.print(state->lastAcceptedBillType);
            out.print(F(" value="));
            out.print(state->lastAcceptedEuro);
            out.println(F(" EUR"));
          }
          if (state->lastCashboxValid) {
            out.print(F("  [MEM] lastCashbox value="));
            out.print(state->lastCashboxEuro);
            out.println(F(" EUR"));
          }
          if (state->lastDispensedValid) {
            out.print(F("  [MEM] lastDispensed value="));
            out.print(state->lastDispensedEuro);
            out.println(F(" EUR"));
          }
        }
      } else {
        out.print(F("159 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x5E:
      if (_dataset.recyclerInventoryMode != BILL_VALIDATOR_RECYCLER_INVENTORY_MD100_PAYLOAD) break;
      {
        uint16_t count10 = 0, count20 = 0, count50 = 0;
        if (parseRecyclerInventory(resp.data, resp.dataLen, count10, count20, count50)) {
          out.print(F("RecyclerInventory: 10EUR="));
          out.print(count10);
          out.print(F(" 20EUR="));
          out.print(count20);
          out.print(F(" 50EUR="));
          out.print(count50);
          out.print(F(" total="));
          out.print((uint32_t)count10 * 10UL + (uint32_t)count20 * 20UL + (uint32_t)count50 * 50UL);
          out.println(F(" EUR"));
          const BillValidatorState* state = stateFor(resp.src);
          if (state && state->recyclerInventoryValid) {
            out.print(F("  [MEM] RecyclerInventory saved: 10EUR="));
            out.print(state->recyclerCount10);
            out.print(F(" 20EUR="));
            out.print(state->recyclerCount20);
            out.print(F(" 50EUR="));
            out.print(state->recyclerCount50);
            out.print(F(" total="));
            out.print(state->recyclerInventoryTotalEuro);
            out.println(F(" EUR"));
          }
        } else {
          out.print(F("0x5E raw: "));
          dumpHex(out, resp.data, resp.dataLen);
          out.println();
        }
      }
      return;

    case 0x1A:
      if (_dataset.recyclerInventoryMode != BILL_VALIDATOR_RECYCLER_INVENTORY_NOTE_AMOUNT) break;
      {
        uint16_t count = 0;
        if (responseCountValue(resp.data, resp.dataLen, count)) {
          out.print(F("NoteAmount="));
          out.print(count);
          const BillValidatorState* state = stateFor(resp.src);
          if (state && state->recyclerInventoryValid) {
            out.print(F(" [MEM] 10EUR="));
            out.print(state->recyclerCount10);
            out.print(F(" 20EUR="));
            out.print(state->recyclerCount20);
            out.print(F(" 50EUR="));
            out.print(state->recyclerCount50);
            out.print(F(" total="));
            out.print(state->recyclerInventoryTotalEuro);
            out.print(F(" EUR"));
          }
          out.println();
        } else {
          out.print(F("0x1A raw: "));
          dumpHex(out, resp.data, resp.dataLen);
          out.println();
        }
      }
      return;

    case 0x20:
      if (_dataset.recyclerInventoryMode != BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT) break;
      out.println(F("ACK"));
      {
        const BillValidatorState* state = stateFor(resp.src);
        if (state && state->iproRecycleBoxMapValid) {
          out.print(F("  [MEM] iPRO recycler boxes: box1="));
          out.print(state->iproRecycleBoxEuro[0]);
          out.print(F(" EUR box2="));
          out.print(state->iproRecycleBoxEuro[1]);
          out.println(F(" EUR"));
        }
      }
      return;

    case 0x24:
      if (_dataset.recyclerInventoryMode != BILL_VALIDATOR_RECYCLER_INVENTORY_IPRO_BOX_COUNT) break;
      if (resp.dataLen >= 4) {
        out.print(F("RecycleCurrent: box1="));
        out.print(readU16LE(&resp.data[0]));
        out.print(F(" box2="));
        out.println(readU16LE(&resp.data[2]));
        const BillValidatorState* state = stateFor(resp.src);
        if (state && state->recyclerInventoryValid) {
          out.print(F("  [MEM] RecyclerInventory saved: 10EUR="));
          out.print(state->recyclerCount10);
          out.print(F(" 20EUR="));
          out.print(state->recyclerCount20);
          out.print(F(" 50EUR="));
          out.print(state->recyclerCount50);
          out.print(F(" total="));
          out.print(state->recyclerInventoryTotalEuro);
          out.println(F(" EUR"));
        }
      } else {
        out.print(F("0x24 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x1D:
      if (_dataset.payoutCommandMode != BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32) break;
      if (resp.dataLen == 0) {
        out.println(F("Status: payload vuoto"));
        return;
      }
      {
        uint8_t pos = 0;
        while (pos < resp.dataLen) {
          const uint8_t code = resp.data[pos++];
          const uint8_t extraLen = smartPayoutStatusDataLen(code);
          if (extraLen == 0xFF || (uint8_t)(resp.dataLen - pos) < extraLen) {
            out.print(F("Status raw: "));
            dumpHex(out, resp.data, resp.dataLen);
            out.println();
            return;
          }

          out.print(F("Status: "));
          out.print(smartPayoutStatusLabel(code));
          out.print(F(" (0x"));
          if (code < 16) out.print('0');
          out.print(code, HEX);
          out.print(')');

          if (extraLen >= 4) {
            out.print(F(" value="));
            printMoneyCents(out, readU32LE(&resp.data[pos]));
          }
          out.println();
          pos = (uint8_t)(pos + extraLen);
        }

        const BillValidatorState* state = stateFor(resp.src);
        if (state) {
          out.print(F("  [MEM] acceptedTotal="));
          out.print(state->acceptedTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] Cassa="));
          out.print(state->cashboxTotalEuro);
          out.println(F(" EUR"));
          out.print(F("  [MEM] dispensedTotal="));
          out.print(state->dispensedTotalEuro);
          out.println(F(" EUR"));
          if (state->lastAcceptedValid) {
            out.print(F("  [MEM] lastAccepted="));
            out.print(state->lastAcceptedEuro);
            out.println(F(" EUR"));
          }
          if (state->lastCashboxValid) {
            out.print(F("  [MEM] lastCashbox="));
            out.print(state->lastCashboxEuro);
            out.println(F(" EUR"));
          }
          if (state->pendingAcceptedRouteValid) {
            out.print(F("  [MEM] pendingRoute="));
            out.print(state->pendingAcceptedRouteEuro);
            out.println(F(" EUR"));
          }
        }
      }
      return;

    case 0x9D:
      out.print(F("BillID=\""));
      dumpAscii(out, resp.data, resp.dataLen);
      out.println('"');
      return;

    case 0x9C:
      if (resp.dataLen == 3) {
        const uint16_t scale = readU16LE(resp.data);
        out.print(F("ScalingFactor="));
        out.print(scale);
        out.print(F(" DecimalPlaces="));
        out.println(resp.data[2]);
      } else {
        out.print(F("156 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x9B:
      out.print(F("BillPositionMask: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;

    case 0x9A:
      if (resp.dataLen == 0) out.println(F("ACK (route accepted)"));
      else if (resp.dataLen == 1 && resp.data[0] == 254) out.println(F("Route error: escrow empty"));
      else if (resp.dataLen == 1 && resp.data[0] == 255) out.println(F("Route error: failed to route bill"));
      else {
        out.print(F("154 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x98:
      if (resp.dataLen == 1) {
        const uint8_t mode = resp.data[0];
        out.print(F("mode=0x"));
        if (mode < 16) out.print('0');
        out.print(mode, HEX);
        out.print(F(" stacker="));
        out.print((mode & 0x01) ? F("on") : F("off"));
        out.print(F(" escrow="));
        out.println((mode & 0x02) ? F("on") : F("off"));
      } else {
        out.print(F("152 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x96:
    case 0x95:
      if (resp.dataLen == 3) {
        out.print(F("Counter="));
        out.println(readU24LE(resp.data));
      } else {
        out.print(F("counter raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x94:
      out.print(F("OptoVoltages raw: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;

    case 0x93:
      if (resp.dataLen == 0) out.println(F("ACK (stacker cycle done)"));
      else if (resp.dataLen == 1 && resp.data[0] == 254) out.println(F("Stacker cycle error: stacker fault"));
      else if (resp.dataLen == 1 && resp.data[0] == 255) out.println(F("Stacker cycle error: stacker not fitted"));
      else {
        out.print(F("147 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x91:
    case 0x81:
      if (resp.dataLen == 0) out.println(F("ACK (no data)"));
      else {
        out.print('"');
        dumpAscii(out, resp.data, resp.dataLen);
        out.println('"');
      }
      return;

    case 0x8D:
      if (resp.dataLen == 1) {
        out.print(F("FirmwareOptions=0x"));
        if (resp.data[0] < 16) out.print('0');
        out.println(resp.data[0], HEX);
      } else {
        out.print(F("141 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0x6A:
      if (resp.dataLen == 1) {
        out.print(F("EscrowStatus="));
        if (resp.data[0] == 0) out.println(F("empty"));
        else if (resp.data[0] == 255) out.println(F("full"));
        else out.println(resp.data[0]);
      } else {
        out.print(F("106 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    case 0xE6:
      out.print(F("InhibitMask: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;

    case 0xE3:
      if (resp.dataLen == 1) {
        out.print(F("masterInhibit="));
        out.println((resp.data[0] & 0x01) ? F("OFF (enabled)") : F("ON (blocked)"));
      } else {
        out.print(F("227 raw: "));
        dumpHex(out, resp.data, resp.dataLen);
        out.println();
      }
      return;

    default:
      out.print(F("ACK data: "));
      dumpHex(out, resp.data, resp.dataLen);
      out.println();
      return;
  }

  out.print(F("ACK data: "));
  dumpHex(out, resp.data, resp.dataLen);
  out.println();
}

void CcTalkBillValidator::onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) {
  (void)printRaw;
  // Prima aggiorna il modello runtime, poi produce l'output umano coerente con
  // lo stato appena appreso.
  updateState(t);
  out.println();

  uint8_t devAddr = t.hasReq ? t.req.dest : (t.hasResp ? t.resp.src : 0);
  if (t.hasReq) printRequest(out, t.req);
  else {
    out.print(F("MASTER -> BV["));
    out.print(devAddr);
    out.println(F("]: (richiesta assente)"));
  }

  if (t.hasResp) printResponse(out, t.hasReq ? t.req.hdr : 0, t.resp);
  else {
    out.print(F("BV["));
    out.print(devAddr);
    out.println(F("] -> MASTER: (nessuna risposta)"));
  }
}
