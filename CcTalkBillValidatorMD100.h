// Scopo del file:
// dichiara `CcTalkBillValidatorMD100`, alias di modello concreto per il decoder
// del bill validator MD100.
#pragma once

#include "CcTalkBillValidator.h"

// Specializzazione nominale del decoder generico per il modello MD100.
// Oggi non aggiunge comportamento: serve a rendere esplicita la selezione
// del modello nello sketch e nelle impostazioni.
class CcTalkBillValidatorMD100 : public CcTalkBillValidator {
public:
  CcTalkBillValidatorMD100() : CcTalkBillValidator(billValidatorDatasetMd100()) {}
  const char* name() const override { return "BILL_VALIDATOR_MD100"; }
};
