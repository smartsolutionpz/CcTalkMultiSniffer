// Scopo del file:
// dichiara `CcTalkHopperSmartHopper`, specializzazione del decoder hopper per
// l'ITL Smart Hopper multimoneta (protocollo CC2, manuale GA863_1_2_50A).
#pragma once

#include "CcTalkHopper.h"

class CcTalkHopperSmartHopper : public CcTalkHopper {
public:
  CcTalkHopperSmartHopper()
    : CcTalkHopper(hopperDatasetSmartHopper()) {}

  const char* name() const override { return "HOPPER_SMART_HOPPER"; }
};
