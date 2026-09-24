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

// Mappatura posizione->taglio nota di fabbrica per l'Alberici Evolution: il
// master imposta il percorso sorter per posizione (0xD2) ma non interroga
// mai la coin table via 0x83, quindi la posizione va nota a priori per poter
// correlare il percorso al taglio in modalita "Discriminatore".
// Posizione 1 = 2 EUR, posizione 2 = 1 EUR, posizione 3 = 0,50 EUR (0 = non
// utilizzata/sconosciuta, resta libera per essere osservata via 0x83).
const uint16_t kAlbericiEvolutionCoinPositionValueCents[16] = {
  200, 100, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const HopperDataset kAlbericiEvolutionDataset(
    "ALBERICI_EVOLUTION",
    HOPPER_CUSTOM_COMMANDS_NONE,
    false,
    0, 0, 0,
    HOPPER_STATUS_MODE_STANDARD,
    kAlbericiEvolutionCoinPositionValueCents);

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

// ITL Smart Hopper (protocollo CC2, manuale GA863): multimoneta, i tagli sono
// dichiarati dal device via Get Device Setup (0x1C/0x2E) e i valori erogati
// arrivano gia in centesimi negli eventi di stato, quindi nessun valore base.
const HopperDataset kSmartHopperDataset(
    "ITL_SMART_HOPPER",
    HOPPER_CUSTOM_COMMANDS_ITL_SMART_HOPPER,
    false);
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

const HopperDataset& hopperDatasetSmartHopper() {
  return kSmartHopperDataset;
}
