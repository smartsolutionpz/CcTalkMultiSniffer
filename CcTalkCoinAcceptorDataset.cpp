// Scopo del file:
// espone i dataset statici dei modelli di gettoniera supportati.
#include "CcTalkCoinAcceptorDataset.h"

namespace {
const CoinAcceptorLabelEntry kNriFalconStatusLabels[] = {
  CoinAcceptorLabelEntry(0, "OK"),
  CoinAcceptorLabelEntry(1, "flight deck open"),
  CoinAcceptorLabelEntry(2, "coin on string detected"),
};

const CoinAcceptorLabelEntry kNriFalconErrorLabels[] = {
  CoinAcceptorLabelEntry(0, "no error"),
  CoinAcceptorLabelEntry(1, "rejected coin"),
  CoinAcceptorLabelEntry(2, "inhibited coin"),
  CoinAcceptorLabelEntry(5, "validation timeout"),
  CoinAcceptorLabelEntry(6, "credit sensor timeout"),
  CoinAcceptorLabelEntry(8, "2nd close coin error"),
  CoinAcceptorLabelEntry(10, "credit sensor not ready / light attack"),
  CoinAcceptorLabelEntry(11, "sorter not ready / escrow open"),
  CoinAcceptorLabelEntry(14, "credit sensor blocked"),
  CoinAcceptorLabelEntry(15, "sorter sensor error"),
  CoinAcceptorLabelEntry(29, "acceptance gate open/not closed"),
  CoinAcceptorLabelEntry(30, "acceptance gate closed/not open"),
  CoinAcceptorLabelEntry(120, "external light"),
  CoinAcceptorLabelEntry(121, "validation sensor blocked"),
  CoinAcceptorLabelEntry(122, "diverter failed"),
  CoinAcceptorLabelEntry(254, "coin return mechanism activated"),
  CoinAcceptorLabelEntry(255, "unspecified alarm"),
};

// Mappatura statica ricavata dall'etichetta della gettoniera allegata:
// due blocchi selezionabili che cambiano il valore dei coinType.
const CoinAcceptorValueEntry kNriFalconBlock0Values[] = {
  CoinAcceptorValueEntry(1, 5),
  CoinAcceptorValueEntry(2, 10),
  CoinAcceptorValueEntry(3, 20),
  CoinAcceptorValueEntry(4, 50),
  CoinAcceptorValueEntry(5, 100),
  CoinAcceptorValueEntry(6, 200),
  CoinAcceptorValueEntry(7, 50),
};

const CoinAcceptorValueEntry kNriFalconBlock1Values[] = {
  CoinAcceptorValueEntry(1, 50),
  CoinAcceptorValueEntry(2, 100),
  CoinAcceptorValueEntry(3, 200),
  CoinAcceptorValueEntry(4, 50),
};

const CoinAcceptorValueProfile kNriFalconValueProfiles[] = {
  CoinAcceptorValueProfile(1,
                           "Block 0: 0.05 / 0.10 / 0.20 / 0.50 / 1.00 / 2.00 / 0.50 EUR",
                           kNriFalconBlock0Values,
                           (uint8_t)(sizeof(kNriFalconBlock0Values) / sizeof(kNriFalconBlock0Values[0]))),
  CoinAcceptorValueProfile(2,
                           "Block 1: 0.50 / 1.00 / 2.00 / 0.50 EUR",
                           kNriFalconBlock1Values,
                           (uint8_t)(sizeof(kNriFalconBlock1Values) / sizeof(kNriFalconBlock1Values[0]))),
};

const CoinAcceptorDataset kNriFalconDataset(
  "NRI_FALCON",
  kNriFalconStatusLabels,
  (uint8_t)(sizeof(kNriFalconStatusLabels) / sizeof(kNriFalconStatusLabels[0])),
  kNriFalconErrorLabels,
  (uint8_t)(sizeof(kNriFalconErrorLabels) / sizeof(kNriFalconErrorLabels[0])),
  kNriFalconValueProfiles,
  (uint8_t)(sizeof(kNriFalconValueProfiles) / sizeof(kNriFalconValueProfiles[0])));
} // namespace

const CoinAcceptorDataset& coinAcceptorDatasetNriFalcon() {
  return kNriFalconDataset;
}

const char* coinAcceptorLookupLabel(const CoinAcceptorLabelEntry* entries,
                                    uint8_t count,
                                    uint8_t code) {
  if (!entries) return nullptr;

  for (uint8_t i = 0; i < count; i++) {
    if (entries[i].code == code) return entries[i].label;
  }

  return nullptr;
}

const CoinAcceptorValueProfile* coinAcceptorLookupValueProfile(const CoinAcceptorDataset& dataset,
                                                               uint8_t profileId) {
  if (!dataset.valueProfiles) return nullptr;

  for (uint8_t i = 0; i < dataset.valueProfileCount; i++) {
    if (dataset.valueProfiles[i].id == profileId) return &dataset.valueProfiles[i];
  }

  return nullptr;
}

bool coinAcceptorLookupStaticValueCents(const CoinAcceptorDataset& dataset,
                                        uint8_t profileId,
                                        uint8_t coinType,
                                        uint16_t& valueCents) {
  valueCents = 0;
  const CoinAcceptorValueProfile* profile =
      coinAcceptorLookupValueProfile(dataset, profileId);
  if (!profile || !profile->values) return false;

  for (uint8_t i = 0; i < profile->valueCount; i++) {
    if (profile->values[i].coinType != coinType) continue;
    valueCents = profile->values[i].valueCents;
    return valueCents > 0;
  }

  return false;
}
