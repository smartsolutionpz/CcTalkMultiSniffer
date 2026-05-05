// Scopo del file:
// dichiara `CcTalkBillValidatorSmartPayout`, alias di modello concreto per il
// decoder del bill validator SMART Payout.
#pragma once

#include "CcTalkBillValidator.h"

class CcTalkBillValidatorSmartPayout : public CcTalkBillValidator {
public:
  CcTalkBillValidatorSmartPayout() : CcTalkBillValidator(billValidatorDatasetSmartPayout()) {}
  const char* name() const override { return "BILL_VALIDATOR_SMART_PAYOUT"; }
};
