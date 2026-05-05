// Scopo del file:
// definisce i dataset statici usati dai decoder dei bill validator supportati.
#pragma once

#include <stdint.h>

enum BillValidatorPayoutCommandMode : uint8_t {
  BILL_VALIDATOR_PAYOUT_COMMAND_NONE = 0,
  BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE = 1,
  BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32 = 2
};

enum BillValidatorRecyclerInventoryMode : uint8_t {
  BILL_VALIDATOR_RECYCLER_INVENTORY_NONE = 0,
  BILL_VALIDATOR_RECYCLER_INVENTORY_MD100_PAYLOAD = 1,
  BILL_VALIDATOR_RECYCLER_INVENTORY_NOTE_AMOUNT = 2
};

struct BillValidatorValueMapEntry {
  uint32_t key;
  uint16_t euro;

  BillValidatorValueMapEntry() : key(0), euro(0) {}
  BillValidatorValueMapEntry(uint32_t inKey, uint16_t inEuro)
    : key(inKey), euro(inEuro) {}
};

struct BillValidatorDataset {
  const char* modelName;
  const BillValidatorValueMapEntry* billTypeMap;
  uint8_t billTypeMapCount;
  const BillValidatorValueMapEntry* recyclerMap;
  uint8_t recyclerMapCount;
  uint8_t payoutCommandMode;
  uint8_t recyclerInventoryMode;

  BillValidatorDataset()
    : modelName(""),
      billTypeMap(nullptr),
      billTypeMapCount(0),
      recyclerMap(nullptr),
      recyclerMapCount(0),
      payoutCommandMode(BILL_VALIDATOR_PAYOUT_COMMAND_NONE),
      recyclerInventoryMode(BILL_VALIDATOR_RECYCLER_INVENTORY_NONE) {}

  BillValidatorDataset(const char* inModelName,
                       const BillValidatorValueMapEntry* inBillTypeMap,
                       uint8_t inBillTypeMapCount,
                       const BillValidatorValueMapEntry* inRecyclerMap,
                       uint8_t inRecyclerMapCount,
                       uint8_t inPayoutCommandMode,
                       uint8_t inRecyclerInventoryMode)
    : modelName(inModelName),
      billTypeMap(inBillTypeMap),
      billTypeMapCount(inBillTypeMapCount),
      recyclerMap(inRecyclerMap),
      recyclerMapCount(inRecyclerMapCount),
      payoutCommandMode(inPayoutCommandMode),
      recyclerInventoryMode(inRecyclerInventoryMode) {}
};

const BillValidatorDataset& billValidatorDatasetMd100();
const BillValidatorDataset& billValidatorDatasetSmartPayout();
const BillValidatorDataset& billValidatorDatasetIpro();
bool billValidatorDatasetLookupEuro(const BillValidatorValueMapEntry* entries,
                                    uint8_t count,
                                    uint32_t key,
                                    uint16_t& euro);
