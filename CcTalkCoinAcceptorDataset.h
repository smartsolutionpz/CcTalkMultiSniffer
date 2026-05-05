// Scopo del file:
// definisce i dataset statici usati dai decoder delle gettoniere supportate.
#pragma once

#include <Arduino.h>
#include <stdint.h>

struct CoinAcceptorLabelEntry {
  uint8_t code;
  const char* label;

  CoinAcceptorLabelEntry() : code(0), label("") {}
  CoinAcceptorLabelEntry(uint8_t inCode, const char* inLabel)
    : code(inCode), label(inLabel) {}
};

struct CoinAcceptorValueEntry {
  uint8_t coinType;
  uint16_t valueCents;

  CoinAcceptorValueEntry() : coinType(0), valueCents(0) {}
  CoinAcceptorValueEntry(uint8_t inCoinType, uint16_t inValueCents)
    : coinType(inCoinType), valueCents(inValueCents) {}
};

struct CoinAcceptorValueProfile {
  uint8_t id;
  const char* label;
  const CoinAcceptorValueEntry* values;
  uint8_t valueCount;

  CoinAcceptorValueProfile()
    : id(0), label(""), values(nullptr), valueCount(0) {}

  CoinAcceptorValueProfile(uint8_t inId,
                           const char* inLabel,
                           const CoinAcceptorValueEntry* inValues,
                           uint8_t inValueCount)
    : id(inId), label(inLabel), values(inValues), valueCount(inValueCount) {}
};

struct CoinAcceptorDataset {
  const char* modelName;
  const CoinAcceptorLabelEntry* statusLabels;
  uint8_t statusLabelCount;
  const CoinAcceptorLabelEntry* errorLabels;
  uint8_t errorLabelCount;
  const CoinAcceptorValueProfile* valueProfiles;
  uint8_t valueProfileCount;

  CoinAcceptorDataset()
    : modelName(""),
      statusLabels(nullptr),
      statusLabelCount(0),
      errorLabels(nullptr),
      errorLabelCount(0),
      valueProfiles(nullptr),
      valueProfileCount(0) {}

  CoinAcceptorDataset(const char* inModelName,
                      const CoinAcceptorLabelEntry* inStatusLabels,
                      uint8_t inStatusLabelCount,
                      const CoinAcceptorLabelEntry* inErrorLabels,
                      uint8_t inErrorLabelCount,
                      const CoinAcceptorValueProfile* inValueProfiles = nullptr,
                      uint8_t inValueProfileCount = 0)
    : modelName(inModelName),
      statusLabels(inStatusLabels),
      statusLabelCount(inStatusLabelCount),
      errorLabels(inErrorLabels),
      errorLabelCount(inErrorLabelCount),
      valueProfiles(inValueProfiles),
      valueProfileCount(inValueProfileCount) {}
};

const CoinAcceptorDataset& coinAcceptorDatasetNriFalcon();
const char* coinAcceptorLookupLabel(const CoinAcceptorLabelEntry* entries,
                                    uint8_t count,
                                    uint8_t code);
const CoinAcceptorValueProfile* coinAcceptorLookupValueProfile(const CoinAcceptorDataset& dataset,
                                                               uint8_t profileId);
bool coinAcceptorLookupStaticValueCents(const CoinAcceptorDataset& dataset,
                                        uint8_t profileId,
                                        uint8_t coinType,
                                        uint16_t& valueCents);
