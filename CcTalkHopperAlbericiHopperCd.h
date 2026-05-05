// Scopo del file:
// dichiara `CcTalkHopperAlbericiHopperCd`, specializzazione nominale del
// decoder hopper per il modello Alberici HopperCD.
#pragma once

#include "CcTalkHopper.h"

class CcTalkHopperAlbericiHopperCd : public CcTalkHopper {
public:
  CcTalkHopperAlbericiHopperCd()
    : CcTalkHopper(hopperDatasetAlbericiHopperCd()) {}

  const char* name() const override { return "HOPPER_ALBERICI_HOPPERCD"; }
};
