// Scopo del file:
// espone i dataset statici dei modelli hopper supportati dal firmware.
#include "CcTalkHopperDataset.h"

namespace {
const HopperDataset kAlbericiDiscriminatorDataset(
    "ALBERICI_DISCRIMINATOR",
    HOPPER_CUSTOM_COMMANDS_NONE,
    false);

const HopperDataset kAlbericiHopperCdDataset(
    "ALBERICI_HOPPERCD",
    HOPPER_CUSTOM_COMMANDS_NONE,
    true);

const HopperDataset kAlbericiEvolutionDataset(
    "ALBERICI_EVOLUTION",
    HOPPER_CUSTOM_COMMANDS_NONE,
    false);

const HopperDataset kSuzoEvolutionDataset(
    "SUZO_EVOLUTION",
    HOPPER_CUSTOM_COMMANDS_NONE,
    true);

const HopperDataset kAzkoyenDiscriminatorDataset(
    "AZKOYEN_DISCRIMINATOR",
    HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR,
    false,
    50,
    2,
    4,
    HOPPER_STATUS_MODE_AZKOYEN_TYPE1_PAYOUT_COUNTER);
} // namespace

const HopperDataset& hopperDatasetAlbericiDiscriminator() {
  return kAlbericiDiscriminatorDataset;
}

const HopperDataset& hopperDatasetAlbericiHopperCd() {
  return kAlbericiHopperCdDataset;
}

const HopperDataset& hopperDatasetAlbericiEvolution() {
  return kAlbericiEvolutionDataset;
}

const HopperDataset& hopperDatasetSuzoEvolution() {
  return kSuzoEvolutionDataset;
}

const HopperDataset& hopperDatasetAzkoyenDiscriminator() {
  return kAzkoyenDiscriminatorDataset;
}
