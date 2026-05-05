// Scopo del file:
// dichiara `CcTalkBillValidatorIPRO`, alias di modello concreto per il
// decoder del bill validator JCM iPRO in modalita ccTalk.
#pragma once

#include "CcTalkBillValidator.h"

class CcTalkBillValidatorIPRO : public CcTalkBillValidator {
public:
  CcTalkBillValidatorIPRO() : CcTalkBillValidator(billValidatorDatasetIpro()) {}
  const char* name() const override { return "BILL_VALIDATOR_IPRO"; }
};
