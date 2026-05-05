// Scopo del file:
// dichiara `CcTalkHopperAlbericiDiscriminator`, specializzazione nominale
// del decoder hopper per il modello Alberici.
#pragma once

#include "CcTalkHopper.h"

// Specializzazione nominale del decoder hopper per il modello Alberici.
// Come per il BV MD100, oggi differenzia il modello a livello architetturale
// piu che comportamentale.
class CcTalkHopperAlbericiDiscriminator : public CcTalkHopper {
public:
  CcTalkHopperAlbericiDiscriminator()
    : CcTalkHopper(hopperDatasetAlbericiDiscriminator()) {}

  const char* name() const override { return "HOPPER_ALBERICI_DISCRIMINATOR"; }
};
