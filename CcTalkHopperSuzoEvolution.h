// Scopo del file:
// dichiara `CcTalkHopperSuzoEvolution`, specializzazione nominale del
// decoder hopper per il modello Suzo Happ Evolution mono-moneta.
#pragma once

#include "CcTalkHopper.h"

class CcTalkHopperSuzoEvolution : public CcTalkHopper {
public:
  CcTalkHopperSuzoEvolution()
    : CcTalkHopper(hopperDatasetSuzoEvolution()) {}

  const char* name() const override { return "HOPPER_SUZO_EVOLUTION"; }
};
