// Scopo del file:
// implementa il routing delle transazioni sniffate verso il decoder corretto.
#include "CcTalkRouter.h"
#include "CcTalkDevice.h"

void CcTalkRouter::add(CcTalkDevice* d) {
  // Il numero massimo e fisso per evitare allocazione dinamica.
  if (_count < (sizeof(_devs) / sizeof(_devs[0]))) _devs[_count++] = d;
}

CcTalkDevice* CcTalkRouter::pickByAddr(uint8_t addr) {
  for (uint8_t i = 0; i < _count; i++) {
    if (_devs[i]->matches(addr)) return _devs[i];
  }
  return nullptr;
}

void CcTalkRouter::route(const CcTalkTransaction& t, Stream& out, bool printFull) {
  // La priorita e la richiesta, se presente, perche identifica con certezza
  // il destinatario; in alternativa si usa il mittente della risposta.
  uint8_t devAddr = 0;
  if (t.hasReq) devAddr = t.req.dest;
  else if (t.hasResp) devAddr = t.resp.src;

  CcTalkDevice* d = pickByAddr(devAddr);
  if (d) {
    d->onTransaction(t, out, false);
  } else {
    // Fallback utile quando sul bus compare un indirizzo non ancora gestito.
    out.println();
    out.print(F("DEVICE["));
    out.print(devAddr);
    out.println(F("]: (non gestito)"));
  }

  // La stampa raw resta una responsabilita del router, cosi tutti i decoder
  // ottengono lo stesso formato finale senza duplicare codice.
  if (!printFull) return;

  if (t.hasReq) CcTalkDevice::printRawIf(out, true, t.req, "RAW REQ");
  else out.println(F("RAW REQ: (assente)"));

  if (t.hasResp) CcTalkDevice::printRawIf(out, true, t.resp, "RAW RESP");
  else out.println(F("RAW RESP: (assente)"));
}
