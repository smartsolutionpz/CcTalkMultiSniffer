// Scopo del file:
// dichiara `CcTalkCoinAcceptor`, il decoder della gettoniera ccTalk
// collegata all'indirizzo 2.
#pragma once
#include <Arduino.h>
#include "CcTalkDevice.h"
#include "CcTalkCoinAcceptorDataset.h"

// Decoder minimale per la gettoniera sull'indirizzo 2.
// Non mantiene uno stato ricco come hopper/BV, ma fornisce una decodifica
// leggibile dei principali comandi e risposte osservati sul bus.
class CcTalkCoinAcceptor : public CcTalkDevice {
public:
  struct CoinIdState {
    bool valid = false;
    char id[8] = {0};
    uint16_t valueCents = 0;
  };

  struct BufferedCreditTrace {
    bool valid = false;
    bool isCredit = false;
    uint8_t coinType = 0;
    uint8_t sorterPathOrError = 0;
    uint8_t accountingCode = 0;
    uint16_t valueCents = 0;
  };

  struct CoinAcceptorState {
    bool present = false;
    uint8_t addr = 2;
    uint8_t activeValueProfile = 0;
    bool eventCounterValid = false;
    uint8_t eventCounter = 0;
    bool eventCounterSeen = false;
    uint8_t lastProcessedEventCounter = 0;
    uint32_t acceptedTotalCents = 0;
    bool lastAcceptedValid = false;
    uint8_t lastAcceptedCoinType = 0;
    uint16_t lastAcceptedValueCents = 0;
    uint8_t pendingUnknownCreditCount[16] = {0};
    bool lastBufferedPollDiagnosticsValid = false;
    uint8_t lastBufferedPollDelta = 0;
    BufferedCreditTrace lastBufferedCredits[5];
    CoinIdState coinIds[16];
  };

  explicit CcTalkCoinAcceptor(const CoinAcceptorDataset& dataset);
  bool matches(uint8_t addr) const override { return addr == 2; }
  const char* name() const override { return "GETTONIERA"; }

  void onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) override;
  void resetState();
  const CoinAcceptorState* stateFor(uint8_t addr) const;
  void dumpState(Stream& out) const;
  void setValueProfile(uint8_t profileId);

private:
  enum BufferedAccountingCode : uint8_t {
    BUFFERED_ACCOUNTING_NONE = 0,
    BUFFERED_ACCOUNTING_COUNTED = 1,
    BUFFERED_ACCOUNTING_SKIPPED_FIRST_POLL = 2,
    BUFFERED_ACCOUNTING_SKIPPED_TYPE_OUT_OF_RANGE = 3,
    BUFFERED_ACCOUNTING_SKIPPED_UNKNOWN_COIN_ID = 4
  };

  const CoinAcceptorDataset& _dataset;
  CoinAcceptorState _state;

  // Mappa un header ccTalk in una descrizione testuale specializzata.
  const __FlashStringHelper* cmdDesc(uint8_t hdr) const;
  void printRequest(Stream& out, const CcTalkFrame& req);
  void printRequestPayload(Stream& out, const CcTalkFrame& req);
  void printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp);
  void printBufferedEvent(Stream& out, uint8_t idx, uint8_t a, uint8_t b) const;
  void printBufferedAccounting(Stream& out, uint8_t idx) const;
  const char* statusLabel(uint8_t code) const;
  const char* errorLabel(uint8_t code) const;
  uint8_t eventCounterDelta(uint8_t previous, uint8_t current) const;
  bool parseCoinIdValueCents(const uint8_t* data, uint8_t len, uint16_t& valueCents) const;
  void applyPendingCreditsForCoinType(uint8_t coinType);
  bool resolveCoinValueCents(uint8_t coinType, uint16_t& valueCents) const;
  void updateState(const CcTalkTransaction& t);
};
