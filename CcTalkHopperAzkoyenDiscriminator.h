// Scopo del file:
// dichiara `CcTalkHopperAzkoyenDiscriminator`, specializzazione del decoder
// hopper per il protocollo Azkoyen U Discriminator.
#pragma once

#include "CcTalkHopper.h"

class CcTalkHopperAzkoyenDiscriminator : public CcTalkHopper {
public:
  CcTalkHopperAzkoyenDiscriminator()
    : CcTalkHopper(hopperDatasetAzkoyenDiscriminator()) {}

  const char* name() const override { return "HOPPER_AZKOYEN_DISCRIMINATOR"; }
};
