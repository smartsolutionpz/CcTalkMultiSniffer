// Scopo del file:
// dichiara `CcTalkCoinAcceptorNriFalcon`, alias di modello concreto per il
// decoder della gettoniera NRI Falcon.
#pragma once

#include "CcTalkCoinAcceptor.h"

class CcTalkCoinAcceptorNriFalcon : public CcTalkCoinAcceptor {
public:
  CcTalkCoinAcceptorNriFalcon() : CcTalkCoinAcceptor(coinAcceptorDatasetNriFalcon()) {}
  const char* name() const override { return "GETTONIERA_NRI_FALCON"; }
};
