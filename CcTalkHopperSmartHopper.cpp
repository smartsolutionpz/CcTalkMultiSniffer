// Scopo del file:
// implementa la parte di `CcTalkHopper` specifica dell'ITL Smart Hopper
// (protocollo CC2, manuale GA863_1_2_50A): descrizione comandi, decodifica di
// richieste/risposte, eventi di Request Status e contabilita dell'erogato.
//
// Differenze rispetto a un hopper ccTalk classico:
// - i valori sono uint32 little-endian in centesimi (es. 5.00 EUR = 500);
// - il payout si chiede per valore (0x16/0x27) o per taglio (0x20/0x2C), non
//   con 0xA7/0xAA, e l'avanzamento si legge dagli eventi di Request Status
//   (0x1D senza valuta, 0x2F con codice valuta ASCII a 3 byte);
// - il serial number (0xF2) e big-endian;
// - un NAK puo portare un byte con il codice d'errore.
// Limite: se il device ha la cifratura di pacchetto BNV attiva, i frame non
// sono leggibili dallo sniffer. I livelli di sicurezza "payout" (byte extra o
// DES sul solo comando payout) lasciano invece in chiaro gli eventi di stato,
// da cui deriva la contabilita.
#include "CcTalkHopper.h"
#include "CcTalkUtils.h"
#include "AnomalyDebugLog.h" // [ANOMALY_DEBUG]
#include <string.h>

namespace {
uint32_t readU32LE(const uint8_t* d) {
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

uint32_t readU24BE(const uint8_t* d) {
  return ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | (uint32_t)d[2];
}

void printCountry(Stream& out, const uint8_t* cc) {
  dumpAscii(out, cc, 3);
}

void printRaw(Stream& out, const __FlashStringHelper* label, const uint8_t* data, uint8_t len) {
  out.print(label);
  dumpHex(out, data, len);
  out.println();
}

// Evento estratto dalla risposta a Request Status. `value` e la somma dei
// valori su tutte le valute dell'evento (in pratica una sola, EUR).
struct SmartEvent {
  uint8_t code = 0;
  bool hasValue = false;
  uint32_t value = 0;
  uint32_t requested = 0;  // solo Incomplete Payout/Float
  uint8_t aux = 0;         // Calibration Fault / periferica
  uint8_t aux2 = 0;
  const uint8_t* country = nullptr;
};

// Lunghezza dati degli eventi nella risposta a 0x1D (senza valuta).
// -1 = evento non documentato per lunghezza: non si puo proseguire il parse.
int8_t eventDataLenNoCurrency(uint8_t code) {
  switch (code) {
    case 0x00: case 0x03: case 0x04: case 0x0E: case 0x0F: case 0x11: case 0x13:
    case 0x21: case 0x22: case 0x25: case 0x26:
      return 0;
    case 0x24: case 0x38:
      return 1;
    case 0x37:
      return 2;
    case 0x01: case 0x02: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0C:
    case 0x0D: case 0x10: case 0x27: case 0x28: case 0x36:
      return 4;
    case 0x0A: case 0x0B:
      return 8; // valore erogato + valore richiesto
    default:
      return -1;
  }
}

// Eventi che in 0x2F portano [numero valute] + N * (valore 4 + valuta 3).
bool isMultiCurrencyValueEvent(uint8_t code) {
  switch (code) {
    case 0x01: case 0x02: case 0x06: case 0x07: case 0x08: case 0x09:
    case 0x0C: case 0x10: case 0x27: case 0x28: case 0x36:
      return true;
    default:
      return false;
  }
}

// Estrae il prossimo evento a partire da `pos`.
// Ritorna 1 = evento valido, 0 = fine dati, -1 = dati non interpretabili.
int8_t nextSmartEvent(bool withCurrency, const uint8_t* data, uint8_t len,
                      uint8_t& pos, SmartEvent& ev) {
  if (!data || pos >= len) return 0;
  ev = SmartEvent();
  ev.code = data[pos];
  const uint8_t start = (uint8_t)(pos + 1);
  const uint8_t remain = (uint8_t)(len - start);
  const uint8_t* p = &data[start];

  if (withCurrency && isMultiCurrencyValueEvent(ev.code)) {
    if (remain < 1) return -1;
    const uint8_t n = p[0];
    const uint16_t need = (uint16_t)(1u + (uint16_t)n * 7u);
    if (need > remain) return -1;
    for (uint8_t i = 0; i < n; i++) {
      ev.value += readU32LE(&p[1 + i * 7]);
    }
    if (n > 0) ev.country = &p[5];
    ev.hasValue = true;
    pos = (uint8_t)(start + need);
    return 1;
  }
  if (withCurrency && (ev.code == 0x0A || ev.code == 0x0B)) {
    if (remain < 1) return -1;
    const uint8_t n = p[0];
    const uint16_t need = (uint16_t)(1u + (uint16_t)n * 11u);
    if (need > remain) return -1;
    for (uint8_t i = 0; i < n; i++) {
      ev.value += readU32LE(&p[1 + i * 11]);
      ev.requested += readU32LE(&p[5 + i * 11]);
    }
    if (n > 0) ev.country = &p[9];
    ev.hasValue = true;
    pos = (uint8_t)(start + need);
    return 1;
  }
  if (withCurrency && ev.code == 0x0D) {
    if (remain < 7) return -1;
    ev.value = readU32LE(p);
    ev.country = &p[4];
    ev.hasValue = true;
    pos = (uint8_t)(start + 7);
    return 1;
  }

  const int8_t dl = eventDataLenNoCurrency(ev.code);
  if (dl < 0 || (uint8_t)dl > remain) return -1;
  if (dl == 4) {
    ev.value = readU32LE(p);
    ev.hasValue = true;
  } else if (dl == 8) {
    ev.value = readU32LE(p);
    ev.requested = readU32LE(&p[4]);
    ev.hasValue = true;
  } else if (dl >= 1) {
    ev.aux = p[0];
    if (dl == 2) ev.aux2 = p[1];
  }
  pos = (uint8_t)(start + (uint8_t)dl);
  return 1;
}

const __FlashStringHelper* smartEventName(uint8_t code) {
  switch (code) {
    case 0x00: return F("Idle");
    case 0x01: return F("Dispensing");
    case 0x02: return F("Dispensed");
    case 0x03: return F("Coins Low");
    case 0x04: return F("Empty");
    case 0x06: return F("Halted");
    case 0x07: return F("Floating");
    case 0x08: return F("Floated");
    case 0x09: return F("Timeout");
    case 0x0A: return F("Incomplete Payout");
    case 0x0B: return F("Incomplete Float");
    case 0x0C: return F("Cashbox Paid");
    case 0x0D: return F("Coin Credit");
    case 0x0E: return F("Emptying");
    case 0x0F: return F("Emptied");
    case 0x10: return F("Fraud Attempt");
    case 0x11: return F("Disabled");
    case 0x13: return F("Slave Reset");
    case 0x21: return F("Lid Open");
    case 0x22: return F("Lid Closed");
    case 0x24: return F("Calibration Fault");
    case 0x25: return F("Attached Mech Jam");
    case 0x26: return F("Attached Mech Open");
    case 0x27: return F("Smart Emptying");
    case 0x28: return F("Smart Emptied");
    case 0x36: return F("Multiple Value Added");
    case 0x37: return F("Peripheral Error");
    case 0x38: return F("Peripheral Device Disabled");
    case 0x3A: return F("Value Pay-in");
    case 0x3B: return F("Device Full");
    default:   return F("evento non mappato");
  }
}

// Codici d'errore del NAK sui comandi payout/float/empty.
const __FlashStringHelper* payoutNakReason(uint8_t code) {
  switch (code) {
    case 1: return F("valore insufficiente nel device");
    case 2: return F("importo esatto non erogabile");
    case 3: return F("device occupato");
    case 4: return F("device disabilitato");
    case 5: return F("coperchio/percorso aperto");
    case 6: return F("inceppamento");
    case 7: return F("errore calibrazione");
    case 8: return F("frode rilevata");
    case 9: return F("device scollegato");
    default: return F("codice non documentato");
  }
}

// Codici d'errore del NAK sui comandi verso la periferica collegata.
const __FlashStringHelper* peripheralNakReason(uint8_t code) {
  switch (code) {
    case 0: return F("periferica non collegata");
    case 1: return F("periferica fuori servizio");
    case 2: return F("valuta non corrispondente");
    default: return F("codice non documentato");
  }
}

const __FlashStringHelper* peripheralName(uint8_t code) {
  return (code == 0) ? F("coin mech") : (code == 1) ? F("coin feeder") : F("periferica ?");
}

bool isSmartPayoutLikeCommand(uint8_t hdr) {
  switch (hdr) {
    case 0x16: case 0x27: // payout amount
    case 0x17: case 0x28: // float amount
    case 0x18:            // empty
    case 0x20: case 0x2C: // payout by denomination
    case 0x21: case 0x2D: // float by denomination
    case 0x33:            // smart empty
      return true;
    default:
      return false;
  }
}

bool isSmartPeripheralCommand(uint8_t hdr) {
  return hdr == 0x30 || hdr == 0x31 || hdr == 0x32 || hdr == 0x35;
}

// Trova dove inizia [N][N * blocco] nei comandi "per taglio" (0x20/0x21
// blocco da 6 byte, 0x2C/0x2D blocco da 9 byte con valuta). La tabella del
// manuale mette N al byte 0, ma l'esempio di 0x2C ha un 0x00 in testa: si
// preferisce l'offset che copre esattamente il payload, altrimenti offset 0
// con eventuali byte di sicurezza in coda. -1 se il payload non torna.
int8_t denominationRequestOffset(const uint8_t* data, uint8_t len, uint8_t blockSize) {
  if (!data || len < 1) return -1;
  for (uint8_t off = 0; off < 2 && off < len; off++) {
    if ((uint16_t)(off + 1u + (uint16_t)data[off] * blockSize) == len) return (int8_t)off;
  }
  return ((uint16_t)(1u + (uint16_t)data[0] * blockSize) <= len) ? 0 : -1;
}

// Somma count*valore dei blocchi "per taglio". false se il payload non torna.
bool sumDenominationRequest(const uint8_t* data, uint8_t len, uint8_t blockSize, uint32_t& total) {
  total = 0;
  const int8_t off = denominationRequestOffset(data, len, blockSize);
  if (off < 0) return false;
  const uint8_t n = data[off];
  for (uint8_t i = 0; i < n; i++) {
    const uint8_t* b = &data[off + 1 + i * blockSize];
    total += (uint32_t)readU16LE(b) * readU32LE(&b[2]);
  }
  return true;
}
} // namespace

bool CcTalkHopper::smartHopperEnabled() const {
  return _dataset.customCommandMode == HOPPER_CUSTOM_COMMANDS_ITL_SMART_HOPPER;
}

const __FlashStringHelper* CcTalkHopper::smartHopperCmdDesc(uint8_t hdr) const {
  switch (hdr) {
    case 0x14: return F("set routing (14)");
    case 0x15: return F("get routing (15)");
    case 0x16: return F("payout amount (16)");
    case 0x17: return F("float amount (17)");
    case 0x18: return F("empty (18)");
    case 0x19: return F("get minimum payout (19)");
    case 0x1A: return F("get denomination amount (1A)");
    case 0x1B: return F("set denomination amount (1B)");
    case 0x1C: return F("get device setup (1C)");
    case 0x1D: return F("request status (1D)");
    case 0x1E: return F("set payout options (1E)");
    case 0x1F: return F("get payout options (1F)");
    case 0x20: return F("payout by denomination (20)");
    case 0x21: return F("float by denomination (21)");
    case 0x22: return F("run unit calibration (22)");
    case 0x25: return F("set routing cur (25)");
    case 0x26: return F("get routing cur (26)");
    case 0x27: return F("payout amount cur (27)");
    case 0x28: return F("float amount cur (28)");
    case 0x29: return F("get minimum payout cur (29)");
    case 0x2A: return F("get denomination amount cur (2A)");
    case 0x2B: return F("set denomination amount cur (2B)");
    case 0x2C: return F("payout by denomination cur (2C)");
    case 0x2D: return F("float by denomination cur (2D)");
    case 0x2E: return F("get device setup cur (2E)");
    case 0x2F: return F("request status cur (2F)");
    case 0x30: return F("set peripheral master inhibit (30)");
    case 0x31: return F("get peripheral master inhibit (31)");
    case 0x32: return F("set peripheral inhibit value (32)");
    case 0x33: return F("smart empty (33)");
    case 0x34: return F("get cashbox operation data (34)");
    case 0x35: return F("get peripheral inhibit value (35)");
    case 0x40: return F("halt (40)");
    case 0x6D: return F("request encrypted status (6D)");
    case 0x6E: return F("switch DES key (6E)");
    case 0x6F: return F("request encryption support (6F)");
    case 0x88: return F("store encryption code (88)");
    case 0x89: return F("switch encryption code (89)");
    case 0xA0: return F("request cipher key (A0)");
    case 0xA1: return F("pump RNG (A1)");
    case 0xC3: return F("request last mod date (C3)");
    case 0xE3: return F("get master inhibit status (E3)");
    case 0xE4: return F("set master inhibit status (E4)");
    default:   return nullptr;
  }
}

bool CcTalkHopper::updateSmartHopperState(HopperState& state, const CcTalkFrame& req,
                                          const CcTalkFrame& resp) {
  switch (req.hdr) {
    case 0x01:
      // Il device resettato perde l'operazione in corso: chiude l'episodio e
      // lascia al ramo generico l'azzeramento del resto dello stato.
      state.smartEpisodeActive = false;
      return false;

    case 0xF2:
      if (resp.dataLen == 3) {
        state.serial = readU24BE(resp.data);
        state.serialValid = true;
      }
      return true;

    case 0x16:
    case 0x27:
    case 0x20:
    case 0x2C: {
      // ACK = payout accettato: si apre un nuovo episodio. Il valore richiesto
      // e solo informativo (con DES sul comando payout sarebbe illeggibile):
      // l'erogato vero arriva dagli eventi di stato.
      uint32_t requested = 0;
      bool requestedValid = false;
      if (req.hdr == 0x16 || req.hdr == 0x27) {
        if (req.dataLen >= 4) {
          requested = readU32LE(req.data);
          requestedValid = true;
        }
      } else {
        requestedValid =
            sumDenominationRequest(req.data, req.dataLen, (req.hdr == 0x20) ? 6 : 9, requested);
      }
      state.smartPayoutRequestValid = requestedValid;
      state.smartPayoutRequestValue = requested;
      state.smartEpisodeActive = true;
      state.smartEpisodeCredited = 0;
      state.lastDispenseStepValid = false;
      if (resp.dataLen == 1) {
        state.smartEventCount = resp.data[0];
        state.smartEventCountValid = true;
      }
      return true;
    }

    case 0x17: case 0x28: case 0x18: case 0x21: case 0x2D: case 0x33:
      // Float/empty: le monete vanno in cassa, non al cliente. Non aprono un
      // episodio di payout, si registra solo l'event count (livelli sicuri).
      if (resp.dataLen == 1) {
        state.smartEventCount = resp.data[0];
        state.smartEventCountValid = true;
      }
      return true;

    case 0x1D:
    case 0x2F:
      processSmartHopperStatus(state, req.hdr, resp.data, resp.dataLen);
      return true;

    case 0x1C:
      // [valuta 3][N 1][N * valore 4]
      if (resp.dataLen >= 4) {
        const uint8_t n = resp.data[3];
        if ((uint16_t)(4u + (uint16_t)n * 4u) <= resp.dataLen) {
          memset(state.coinValues, 0, sizeof(state.coinValues));
          for (uint8_t i = 0; i < n && i < 16; i++) {
            CoinValueState& coin = state.coinValues[i];
            const uint32_t value = readU32LE(&resp.data[4 + i * 4]);
            coin.value = (value > 0xFFFFu) ? 0xFFFFu : (uint16_t)value;
            memcpy(coin.coin, resp.data, 3);
            coin.valid = true;
          }
        }
      }
      return true;

    case 0x2E:
      // [N 1][N * (valore 4 + valuta 3)]
      if (resp.dataLen >= 1) {
        const uint8_t n = resp.data[0];
        if ((uint16_t)(1u + (uint16_t)n * 7u) <= resp.dataLen) {
          memset(state.coinValues, 0, sizeof(state.coinValues));
          for (uint8_t i = 0; i < n && i < 16; i++) {
            CoinValueState& coin = state.coinValues[i];
            const uint8_t* b = &resp.data[1 + i * 7];
            const uint32_t value = readU32LE(b);
            coin.value = (value > 0xFFFFu) ? 0xFFFFu : (uint16_t)value;
            memcpy(coin.coin, &b[4], 3);
            coin.valid = true;
          }
        }
      }
      return true;

    case 0x19:
    case 0x29:
      if (resp.dataLen == 4) {
        state.smartMinPayout = readU32LE(resp.data);
        state.smartMinPayoutValid = true;
      }
      return true;

    case 0x1F:
      if (resp.dataLen == 2) {
        state.smartPayoutOptions[0] = resp.data[0];
        state.smartPayoutOptions[1] = resp.data[1];
        state.smartPayoutOptionsValid = true;
      }
      return true;

    default:
      // Tutti gli altri header CC2 non toccano lo stato; quelli ccTalk
      // standard (F6/F4/FF/...) passano al ramo generico.
      return smartHopperCmdDesc(req.hdr) != nullptr;
  }
}

void CcTalkHopper::processSmartHopperStatus(HopperState& state, uint8_t hdr,
                                            const uint8_t* data, uint8_t len) {
  const bool withCurrency = (hdr == 0x2F);
  uint8_t pos = 0;
  SmartEvent ev;
  bool anyStep = false;
  int8_t rc;
  while ((rc = nextSmartEvent(withCurrency, data, len, pos, ev)) > 0) {
    state.smartLastEventCode = ev.code;
    state.smartLastEventValid = true;
    switch (ev.code) {
      case 0x01: // Dispensing: valore progressivo dell'episodio
        applySmartHopperPayoutProgress(state, hdr, ev.code, ev.value, false);
        anyStep = anyStep || state.lastDispenseStepValid;
        break;
      case 0x02: // Dispensed
      case 0x06: // Halted
      case 0x09: // Timeout
      case 0x10: // Fraud Attempt
      case 0x0A: // Incomplete Payout (valore erogato prima dello spegnimento)
        applySmartHopperPayoutProgress(state, hdr, ev.code, ev.value, true);
        anyStep = anyStep || state.lastDispenseStepValid;
        break;
      case 0x0C:
        state.smartCashboxPaidTotal += ev.value;
        break;
      case 0x0D:
        state.smartCoinCreditTotal += ev.value;
        break;
      case 0x13:
        state.smartEpisodeActive = false;
        break;
      default:
        break;
    }
  }
  // Lo step resta visibile nella stampa della risposta solo se questa stessa
  // risposta ha accreditato qualcosa.
  if (!anyStep) state.lastDispenseStepValid = false;
}

void CcTalkHopper::applySmartHopperPayoutProgress(HopperState& state, uint8_t hdr, uint8_t eventCode,
                                                  uint32_t value, bool finalEvent) {
  state.lastDispenseStepValid = false;

  if (!state.smartEpisodeActive) {
    // Nessun comando payout osservato per questo episodio: tipicamente lo
    // sniffer si e riavviato a meta erogazione e la parte gia pagata e stata
    // accreditata (e persistita) prima del riavvio. Stesso criterio
    // conservativo del dedup Azkoyen: si prende il valore come baseline senza
    // credito e si conta solo l'eventuale progresso successivo. Un evento
    // finale senza episodio (es. Incomplete Payout all'avvio) non si conta.
    if (!finalEvent) {
      state.smartEpisodeActive = true;
      state.smartEpisodeCredited = value;
    }
    return;
  }

  uint32_t delta = 0;
  if (value > state.smartEpisodeCredited) {
    delta = value - state.smartEpisodeCredited;
    state.smartEpisodeCredited = value;
  }
  if (finalEvent) state.smartEpisodeActive = false;
  if (delta == 0) return;

  if (claimOrCheckDispenseSource(state, DISPENSE_SOURCE_SMART_HOPPER_STATUS)) {
    state.dispensedTotalValue += delta;
    state.lastDispenseStepValue = (delta > 0xFFFFu) ? 0xFFFFu : (uint16_t)delta;
    state.lastDispenseStepValid = true;
    // [ANOMALY_DEBUG]
    anomalydebug::logEvent(anomalydebug::DEV_HOPPER, state.addr, hdr,
                            eventCode, state.lastDispenseStepValue);
  }
}

void CcTalkHopper::printSmartHopperStatus(Stream& out, uint8_t hdr,
                                          const uint8_t* data, uint8_t len) const {
  if (len == 0) {
    out.println(F("Status: (nessun dato)"));
    return;
  }
  const bool withCurrency = (hdr == 0x2F);
  uint8_t pos = 0;
  SmartEvent ev;
  bool first = true;
  int8_t rc;
  out.print(F("Status: "));
  while ((rc = nextSmartEvent(withCurrency, data, len, pos, ev)) > 0) {
    if (!first) out.print(F(" | "));
    first = false;
    out.print(smartEventName(ev.code));
    out.print(F(" (0x"));
    if (ev.code < 16) out.print('0');
    out.print(ev.code, HEX);
    out.print(')');
    if (ev.hasValue) {
      out.print(F(" value="));
      printValueAsEuro(out, ev.value);
      if (ev.code == 0x0A || ev.code == 0x0B) {
        out.print(F(" requested="));
        printValueAsEuro(out, ev.requested);
      }
      if (ev.country) {
        out.print(F(" cur="));
        printCountry(out, ev.country);
      }
    } else if (ev.code == 0x24) {
      out.print(F(" code="));
      out.print(ev.aux);
    } else if (ev.code == 0x37) {
      out.print(F(" periph="));
      out.print(peripheralName(ev.aux));
      out.print(F(" err="));
      out.print(ev.aux2);
    } else if (ev.code == 0x38) {
      out.print(F(" periph="));
      out.print(peripheralName(ev.aux));
    }
  }
  if (rc < 0 && pos < len) {
    if (!first) out.print(F(" | "));
    out.print(F("non decodificato: "));
    dumpHex(out, &data[pos], (uint8_t)(len - pos));
  }
  out.println();
}

bool CcTalkHopper::printSmartHopperRequestPayload(Stream& out, const CcTalkFrame& req) const {
  const uint8_t* d = req.data;
  const uint8_t n = req.dataLen;
  switch (req.hdr) {
    case 0x16:
    case 0x27:
      if (n < 4) return false;
      out.print(F("  payload: value="));
      printValueAsEuro(out, readU32LE(d));
      if (req.hdr == 0x27 && n >= 7) {
        out.print(F(" cur="));
        printCountry(out, &d[4]);
      }
      {
        const uint8_t used = (req.hdr == 0x27) ? 7 : 4;
        if (n > used) {
          out.print(F(" security="));
          dumpHex(out, &d[used], (uint8_t)(n - used));
        }
      }
      out.println();
      return true;

    case 0x17:
    case 0x28:
      if (n < 8) return false;
      out.print(F("  payload: minPayout="));
      printValueAsEuro(out, readU32LE(d));
      out.print(F(" floatValue="));
      printValueAsEuro(out, readU32LE(&d[4]));
      if (req.hdr == 0x28 && n >= 11) {
        out.print(F(" cur="));
        printCountry(out, &d[8]);
      }
      out.println();
      return true;

    case 0x20: case 0x21: case 0x2C: case 0x2D: {
      const bool cur = (req.hdr == 0x2C || req.hdr == 0x2D);
      const uint8_t block = cur ? 9 : 6;
      uint32_t total = 0;
      if (!sumDenominationRequest(d, n, block, total)) return false;
      const uint8_t off = (uint8_t)denominationRequestOffset(d, n, block);
      out.print(F("  payload:"));
      for (uint8_t i = 0; i < d[off]; i++) {
        const uint8_t* b = &d[off + 1 + i * block];
        out.print(' ');
        out.print(readU16LE(b));
        out.print('x');
        printValueAsEuro(out, readU32LE(&b[2]));
        if (cur) {
          out.print(' ');
          printCountry(out, &b[6]);
        }
      }
      out.print(F(" totale="));
      printValueAsEuro(out, total);
      out.println();
      return true;
    }

    case 0x14:
    case 0x25:
      if (n < 5) return false;
      out.print(F("  payload: route="));
      out.print(d[0] == 0 ? F("payout") : d[0] == 1 ? F("cashbox") : F("?"));
      out.print(F(" value="));
      printValueAsEuro(out, readU32LE(&d[1]));
      if (req.hdr == 0x25 && n >= 8) {
        out.print(F(" cur="));
        printCountry(out, &d[5]);
      }
      out.println();
      return true;

    case 0x15: case 0x26: case 0x1A: case 0x2A:
      if (n < 4) return false;
      out.print(F("  payload: value="));
      printValueAsEuro(out, readU32LE(d));
      if ((req.hdr == 0x26 || req.hdr == 0x2A) && n >= 7) {
        out.print(F(" cur="));
        printCountry(out, &d[4]);
      }
      out.println();
      return true;

    case 0x1B:
    case 0x2B:
      if (n < 6) return false;
      out.print(F("  payload: value="));
      printValueAsEuro(out, readU32LE(d));
      out.print(F(" addCount="));
      out.print(readU16LE(&d[4]));
      if (readU16LE(&d[4]) == 0) out.print(F(" (azzera contatore)"));
      if (req.hdr == 0x2B && n >= 9) {
        out.print(F(" cur="));
        printCountry(out, &d[6]);
      }
      out.println();
      return true;

    case 0x29:
      if (n < 3) return false;
      out.print(F("  payload: cur="));
      printCountry(out, d);
      out.println();
      return true;

    case 0x1E:
      if (n != 2) return false;
      out.print(F("  payload: reg0=0x"));
      if (d[0] < 16) out.print('0');
      out.print(d[0], HEX);
      out.print(F(" reg1=0x"));
      if (d[1] < 16) out.print('0');
      out.println(d[1], HEX);
      return true;

    case 0x30:
      if (n != 2) return false;
      out.print(F("  payload: periph="));
      out.print(peripheralName(d[0]));
      out.println((d[1] & 0x01) ? F(" ENABLE") : F(" INHIBIT"));
      return true;

    case 0x31:
      if (n != 1) return false;
      out.print(F("  payload: periph="));
      out.println(peripheralName(d[0]));
      return true;

    case 0x32:
      if (n < 9) return false;
      out.print(F("  payload: periph="));
      out.print(peripheralName(d[0]));
      out.print(d[1] ? F(" ENABLE ") : F(" INHIBIT "));
      printValueAsEuro(out, readU32LE(&d[2]));
      out.print(' ');
      printCountry(out, &d[6]);
      out.println();
      return true;

    case 0x35:
      if (n < 8) return false;
      out.print(F("  payload: periph="));
      out.print(peripheralName(d[0]));
      out.print(' ');
      printValueAsEuro(out, readU32LE(&d[1]));
      out.print(' ');
      printCountry(out, &d[5]);
      out.println();
      return true;

    case 0xE4:
      if (n != 1) return false;
      out.println((d[0] & 0x01) ? F("  payload: ENABLE") : F("  payload: DISABLE"));
      return true;

    default:
      return false;
  }
}

bool CcTalkHopper::printSmartHopperResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp) const {
  if (resp.hdr == 0x05) {
    out.print(F("NAK"));
    if (resp.dataLen >= 1) {
      out.print(F(" err="));
      out.print(resp.data[0]);
      if (isSmartPayoutLikeCommand(hostHdr)) {
        out.print(F(" ("));
        out.print(payoutNakReason(resp.data[0]));
        out.print(')');
      } else if (isSmartPeripheralCommand(hostHdr)) {
        out.print(F(" ("));
        out.print(peripheralNakReason(resp.data[0]));
        out.print(')');
      }
    }
    out.println();
    return true;
  }
  if (resp.hdr != 0x00) return false;

  const HopperState* state = stateFor(resp.src);
  const uint8_t* d = resp.data;
  const uint8_t n = resp.dataLen;

  switch (hostHdr) {
    case 0x16: case 0x27: case 0x20: case 0x2C:
      out.print(F("ACK (payout accettato)"));
      if (n == 1) {
        out.print(F(" eventCount="));
        out.print(d[0]);
      }
      out.println();
      if (state && state->smartPayoutRequestValid) {
        out.print(F("  [MEM] payoutRequest="));
        printValueAsEuro(out, state->smartPayoutRequestValue);
        out.println();
      }
      return true;

    case 0x17: case 0x28: case 0x21: case 0x2D: case 0x18: case 0x33:
      if (hostHdr == 0x18 || hostHdr == 0x33) out.print(F("ACK (svuotamento in cassa avviato)"));
      else out.print(F("ACK (float avviato)"));
      if (n == 1) {
        out.print(F(" eventCount="));
        out.print(d[0]);
      }
      out.println();
      return true;

    case 0x1D:
    case 0x2F:
      printSmartHopperStatus(out, hostHdr, d, n);
      if (state && state->lastDispenseStepValid) {
        out.print(F("  [MEM] erogatoStep="));
        printValueAsEuro(out, state->lastDispenseStepValue);
        out.print(F(" erogatoTotale="));
        printValueAsEuro(out, state->dispensedTotalValue);
        out.println();
      }
      return true;

    case 0x1C:
      if (n >= 4 && (uint16_t)(4u + (uint16_t)d[3] * 4u) <= n) {
        out.print(F("Setup cur="));
        printCountry(out, d);
        out.print(F(" tagli="));
        out.print(d[3]);
        out.print(':');
        for (uint8_t i = 0; i < d[3]; i++) {
          out.print(' ');
          printValueAsEuro(out, readU32LE(&d[4 + i * 4]));
        }
        out.println();
      } else {
        printRaw(out, F("1C raw: "), d, n);
      }
      return true;

    case 0x2E:
      if (n >= 1 && (uint16_t)(1u + (uint16_t)d[0] * 7u) <= n) {
        out.print(F("Setup tagli="));
        out.print(d[0]);
        out.print(':');
        for (uint8_t i = 0; i < d[0]; i++) {
          const uint8_t* b = &d[1 + i * 7];
          out.print(' ');
          printValueAsEuro(out, readU32LE(b));
          out.print('/');
          printCountry(out, &b[4]);
        }
        out.println();
      } else {
        printRaw(out, F("2E raw: "), d, n);
      }
      return true;

    case 0x19:
    case 0x29:
      if (n == 4) {
        out.print(F("MinPayout="));
        printValueAsEuro(out, readU32LE(d));
        out.println();
      } else {
        printRaw(out, F("min payout raw: "), d, n);
      }
      return true;

    case 0x1A:
    case 0x2A:
      if (n == 2) {
        out.print(F("Count="));
        out.println(readU16LE(d));
      } else {
        printRaw(out, F("count raw: "), d, n);
      }
      return true;

    case 0x15:
    case 0x26:
      if (n == 1) {
        out.print(F("Route="));
        out.println(d[0] == 0 ? F("payout") : d[0] == 1 ? F("cashbox") : F("?"));
      } else {
        printRaw(out, F("route raw: "), d, n);
      }
      return true;

    case 0x1F:
      if (n == 2) {
        out.print(F("PayoutOptions reg0=0x"));
        if (d[0] < 16) out.print('0');
        out.print(d[0], HEX);
        out.print(F(" payMode="));
        out.print((d[0] & 0x01) ? F("highValueSplit") : F("freePay"));
        out.print(F(" levelCheck="));
        out.print((d[0] & 0x02) ? F("on") : F("off"));
        out.print(F(" motor="));
        out.print((d[0] & 0x04) ? F("high") : F("low"));
        out.print(F(" unknownCoin="));
        out.println((d[0] & 0x20) ? F("payout") : F("cashbox"));
      } else {
        printRaw(out, F("1F raw: "), d, n);
      }
      return true;

    case 0x34:
      // [N 1][N * (livello 2 + valore 4 + valuta 3)][monete sconosciute 4]
      // (l'esempio del manuale non ha i 4 byte finali: opzionali)
      if (n >= 1 && (uint16_t)(1u + (uint16_t)d[0] * 9u) <= n) {
        out.print(F("CashboxOperation:"));
        for (uint8_t i = 0; i < d[0]; i++) {
          const uint8_t* b = &d[1 + i * 9];
          out.print(' ');
          out.print(readU16LE(b));
          out.print('x');
          printValueAsEuro(out, readU32LE(&b[2]));
        }
        const uint16_t tail = (uint16_t)(1u + (uint16_t)d[0] * 9u);
        if ((uint16_t)(tail + 4u) <= n) {
          out.print(F(" sconosciute="));
          out.print(readU32LE(&d[tail]));
        }
        out.println();
      } else {
        printRaw(out, F("34 raw: "), d, n);
      }
      return true;

    case 0x31:
    case 0x35:
    case 0xE3:
      if (n == 1) {
        out.println((d[0] & 0x01) ? F("Stato=ENABLED") : F("Stato=DISABLED"));
      } else {
        printRaw(out, F("stato raw: "), d, n);
      }
      return true;

    case 0xF2:
      if (n == 3) {
        out.print(F("Serial="));
        out.println(readU24BE(d));
      } else {
        printRaw(out, F("Serial raw: "), d, n);
      }
      return true;

    case 0xC3:
    case 0x6D:
    case 0x6F:
    case 0xA0:
    case 0xA1:
      printRaw(out, F("ACK data: "), d, n);
      return true;

    case 0x14: case 0x25: case 0x1B: case 0x2B: case 0x1E: case 0x22:
    case 0x30: case 0x32: case 0x40: case 0xE4: case 0x6E: case 0x88: case 0x89:
      if (n == 0) out.println(F("ACK"));
      else printRaw(out, F("ACK data: "), d, n);
      return true;

    default:
      return false;
  }
}

void CcTalkHopper::dumpSmartHopperState(Stream& out, const HopperState& s) const {
  out.print(F("    smart episode="));
  out.print(s.smartEpisodeActive ? F("attivo") : F("chiuso"));
  out.print(F(" credited="));
  printValueAsEuro(out, s.smartEpisodeCredited);
  if (s.smartPayoutRequestValid) {
    out.print(F(" lastRequest="));
    printValueAsEuro(out, s.smartPayoutRequestValue);
  }
  if (s.smartEventCountValid) {
    out.print(F(" eventCount="));
    out.print(s.smartEventCount);
  }
  if (s.smartLastEventValid) {
    out.print(F(" lastEvent="));
    out.print(smartEventName(s.smartLastEventCode));
  }
  out.println();
  out.print(F("    smart cashboxPaid="));
  printValueAsEuro(out, s.smartCashboxPaidTotal);
  out.print(F(" coinCredit="));
  printValueAsEuro(out, s.smartCoinCreditTotal);
  if (s.smartMinPayoutValid) {
    out.print(F(" minPayout="));
    printValueAsEuro(out, s.smartMinPayout);
  }
  if (s.smartPayoutOptionsValid) {
    out.print(F(" options=0x"));
    if (s.smartPayoutOptions[0] < 16) out.print('0');
    out.print(s.smartPayoutOptions[0], HEX);
    if (s.smartPayoutOptions[1] < 16) out.print('0');
    out.print(s.smartPayoutOptions[1], HEX);
  }
  out.println();
}
