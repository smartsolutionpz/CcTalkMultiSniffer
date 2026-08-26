// Scopo del file:
// dichiara `CcTalkHopperAlbericiEvolution`, specializzazione nominale
// del decoder hopper per il modello Alberici HopperCD Evolution II.
#pragma once

#include "CcTalkHopper.h"

// Specializzazione nominale del decoder hopper per il modello Alberici
// Evolution. Dataset separato da AlbericiDiscriminator perche, pur
// condividendo la maggior parte dei comandi ccTalk, l'Evolution aggiunge
// i comandi sorter (D1/D2) e l'address clash (FC) non presenti sul
// Discriminator.
class CcTalkHopperAlbericiEvolution : public CcTalkHopper {
public:
  CcTalkHopperAlbericiEvolution()
    : CcTalkHopper(hopperDatasetAlbericiEvolution()) {}

  const char* name() const override { return "HOPPER_ALBERICI_EVOLUTION"; }
};
