// Scopo del file:
// definisce i dataset statici dei modelli hopper supportati dal firmware.
#pragma once

#include <Arduino.h>

enum HopperCustomCommandMode : uint8_t {
  HOPPER_CUSTOM_COMMANDS_NONE = 0,
  HOPPER_CUSTOM_COMMANDS_AZKOYEN_DISCRIMINATOR = 1,
  // Protocollo ITL CC2 (Smart Hopper): payout per valore (0x16/0x27/0x20/0x2C)
  // ed eventi via Request Status (0x1D/0x2F), niente 0xA7/0xA6/0xAB.
  HOPPER_CUSTOM_COMMANDS_ITL_SMART_HOPPER = 2
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
  // Tabella statica posizione(1..16, indicizzata 0-based)->valore in cent,
  // nota a priori per il modello. Serve a correlare il percorso sorter
  // (0xD2, per posizione) al taglio moneta sui modelli il cui host non
  // interroga mai la coin table via 0x83 sul bus (es. Alberici Evolution).
  // nullptr = nessuna tabella nota: si usa solo cio' che viene osservato
  // via 0x83 sul bus.
  const uint16_t* defaultCoinPositionValueCents;

  HopperDataset()
    : modelName("GENERIC_HOPPER"),
      customCommandMode(HOPPER_CUSTOM_COMMANDS_NONE),
      statusMode(HOPPER_STATUS_MODE_STANDARD),
      monoCoin(false),
      defaultBaseCoinValueCents(0),
      defaultType1ValueUnits(0),
      defaultType2ValueUnits(0),
      defaultCoinPositionValueCents(nullptr) {}

  HopperDataset(const char* name,
                HopperCustomCommandMode mode,
                bool isMonoCoin,
                uint16_t baseCoinValueCents = 0,
                uint16_t type1ValueUnits = 0,
                uint16_t type2ValueUnits = 0,
                HopperStatusMode hopperStatusMode = HOPPER_STATUS_MODE_STANDARD,
                const uint16_t* coinPositionValueCents = nullptr)
    : modelName(name),
      customCommandMode(mode),
      statusMode(hopperStatusMode),
      monoCoin(isMonoCoin),
      defaultBaseCoinValueCents(baseCoinValueCents),
      defaultType1ValueUnits(type1ValueUnits),
      defaultType2ValueUnits(type2ValueUnits),
      defaultCoinPositionValueCents(coinPositionValueCents) {}
};

const HopperDataset& hopperDatasetAlbericiDiscriminator();
const HopperDataset& hopperDatasetAlbericiHopperCd();
const HopperDataset& hopperDatasetAlbericiEvolution();
const HopperDataset& hopperDatasetSuzoEvolution();
const HopperDataset& hopperDatasetAzkoyenDiscriminator();
const HopperDataset& hopperDatasetSmartHopper();
