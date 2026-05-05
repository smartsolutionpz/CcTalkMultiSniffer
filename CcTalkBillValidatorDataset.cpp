// Scopo del file:
// espone i dataset statici dei modelli BV supportati dal firmware.
#include "CcTalkBillValidatorDataset.h"

namespace {
const BillValidatorValueMapEntry kMd100BillTypeMap[] = {
  {2, 5},
  {3, 10},
  {4, 20},
  {5, 50},
};

const BillValidatorValueMapEntry kMd100RecyclerMap[] = {
  {0x12, 10},
  {0x13, 20},
  {0x14, 50},
};

const BillValidatorValueMapEntry kSmartPayoutBillTypeMap[] = {
  {1, 5},
  {2, 10},
  {3, 20},
  {4, 50},
};

const BillValidatorValueMapEntry kSmartPayoutRecyclerMap[] = {
  {1000, 10},
  {2000, 20},
  {5000, 50},
};

const BillValidatorValueMapEntry kIproBillTypeMap[] = {
  {1, 5},
  {2, 10},
  {3, 20},
  {4, 50},
  {5, 100},
  {6, 200},
  {7, 500},
};

const BillValidatorDataset kMd100Dataset = {
  "MD100",
  kMd100BillTypeMap,
  (uint8_t)(sizeof(kMd100BillTypeMap) / sizeof(kMd100BillTypeMap[0])),
  kMd100RecyclerMap,
  (uint8_t)(sizeof(kMd100RecyclerMap) / sizeof(kMd100RecyclerMap[0])),
  BILL_VALIDATOR_PAYOUT_COMMAND_MD100_CODE,
  BILL_VALIDATOR_RECYCLER_INVENTORY_MD100_PAYLOAD,
};

const BillValidatorDataset kSmartPayoutDataset = {
  "SMART_PAYOUT",
  kSmartPayoutBillTypeMap,
  (uint8_t)(sizeof(kSmartPayoutBillTypeMap) / sizeof(kSmartPayoutBillTypeMap[0])),
  kSmartPayoutRecyclerMap,
  (uint8_t)(sizeof(kSmartPayoutRecyclerMap) / sizeof(kSmartPayoutRecyclerMap[0])),
  BILL_VALIDATOR_PAYOUT_COMMAND_VALUE32,
  BILL_VALIDATOR_RECYCLER_INVENTORY_NOTE_AMOUNT,
};

const BillValidatorDataset kIproDataset = {
  "IPRO",
  kIproBillTypeMap,
  (uint8_t)(sizeof(kIproBillTypeMap) / sizeof(kIproBillTypeMap[0])),
  nullptr,
  0,
  BILL_VALIDATOR_PAYOUT_COMMAND_NONE,
  BILL_VALIDATOR_RECYCLER_INVENTORY_NONE,
};
} // namespace

const BillValidatorDataset& billValidatorDatasetMd100() {
  return kMd100Dataset;
}

const BillValidatorDataset& billValidatorDatasetSmartPayout() {
  return kSmartPayoutDataset;
}

const BillValidatorDataset& billValidatorDatasetIpro() {
  return kIproDataset;
}

bool billValidatorDatasetLookupEuro(const BillValidatorValueMapEntry* entries,
                                    uint8_t count,
                                    uint32_t key,
                                    uint16_t& euro) {
  if (!entries) return false;

  for (uint8_t i = 0; i < count; i++) {
    if (entries[i].key != key) continue;
    euro = entries[i].euro;
    return true;
  }

  return false;
}
