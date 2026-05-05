// Scopo del file:
// definisce i dataset statici dei modelli hopper supportati dal firmware.
#pragma once

#include <Arduino.h>

enum HopperCustomCommandMode : uint8_t {
  HOPPER_CUSTOM_COMMANDS_NONE = 0,
  HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR = 1
};

enum HopperStatusMode : uint8_t {
  HOPPER_STATUS_MODE_STANDARD = 0,
  HOPPER_STATUS_MODE_AZKOYEN_TYPE1_PAYOUT_COUNTER = 1
};

struct HopperDataset {
  const char* modelName;
  HopperCustomCommandMode customCommandMode;
  HopperStatusMode statusMode;
  bool monoCoin;
  uint16_t defaultBaseCoinValueCents;
  uint16_t defaultType1ValueUnits;
  uint16_t defaultType2ValueUnits;

  HopperDataset()
    : modelName("GENERIC_HOPPER"),
      customCommandMode(HOPPER_CUSTOM_COMMANDS_NONE),
      statusMode(HOPPER_STATUS_MODE_STANDARD),
      monoCoin(false),
      defaultBaseCoinValueCents(0),
      defaultType1ValueUnits(0),
      defaultType2ValueUnits(0) {}

  HopperDataset(const char* name,
                HopperCustomCommandMode mode,
                bool isMonoCoin,
                uint16_t baseCoinValueCents = 0,
                uint16_t type1ValueUnits = 0,
                uint16_t type2ValueUnits = 0,
                HopperStatusMode hopperStatusMode = HOPPER_STATUS_MODE_STANDARD)
    : modelName(name),
      customCommandMode(mode),
      statusMode(hopperStatusMode),
      monoCoin(isMonoCoin),
      defaultBaseCoinValueCents(baseCoinValueCents),
      defaultType1ValueUnits(type1ValueUnits),
      defaultType2ValueUnits(type2ValueUnits) {}
};

const HopperDataset& hopperDatasetAlbericiDiscriminator();
const HopperDataset& hopperDatasetAlbericiHopperCd();
const HopperDataset& hopperDatasetSuzoEvolution();
const HopperDataset& hopperDatasetAzkoyenDiscriminator();
