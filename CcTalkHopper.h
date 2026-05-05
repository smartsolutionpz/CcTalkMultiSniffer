// Scopo del file:
// dichiara `CcTalkHopper`, il decoder/stato degli hopper ccTalk, con supporto
// a poll di payout, coin table e totalizzatori monete.
#pragma once
#include <Arduino.h>
#include "CcTalkDevice.h"
#include "CcTalkHopperDataset.h"

// Decoder/stato per hopper agli indirizzi 3..10.
// Tiene traccia sia delle informazioni identificative del device sia
// dei dati di payout/polling da cui si ricavano i totalizzatori monete.
class CcTalkHopper : public CcTalkDevice {
public:
  // Associazione tra indice coin table e valore monetario dichiarato dal device.
  struct CoinValueState {
    bool valid = false;
    char coin[7] = {0}; // 6-char ccTalk coin id + terminatore
    uint16_t value = 0;
  };

  // Snapshot runtime associato a un singolo hopper.
  struct HopperState {
    bool present = false;
    uint8_t addr = 0;

    bool manufacturerValid = false;
    char manufacturer[24] = {0};

    bool productCodeValid = false;
    char productCode[24] = {0};

    bool serialValid = false;
    uint32_t serial = 0;

    bool extendedIdValid = false;
    char extendedId[40] = {0};

    bool payoutHiLowValid = false;
    uint8_t payoutHiLow = 0;

    bool testResultValid = false;
    uint8_t testErr1 = 0;
    uint8_t testErr2 = 0;

    bool eventCounterValid = false;
    uint8_t eventCounter = 0;

    bool azkoyenHopperStatusValid = false;
    uint8_t azkoyenType1Remaining = 0;
    uint8_t azkoyenLastType1PaidByA6 = 0;
    uint8_t azkoyenLastType1UnpaidByA6 = 0;
    bool azkoyenHopperStatusCounterSeen = false;
    uint8_t azkoyenLastHopperStatusCounter = 0;

    bool azkoyenAboutValid = false;
    uint8_t azkoyenDeviceType = 0;
    uint8_t azkoyenSoftwareMajor = 0;
    uint8_t azkoyenSoftwareMinor = 0;
    uint8_t azkoyenCommsMajor = 0;
    uint8_t azkoyenCommsMinor = 0;

    bool azkoyenCurrentStatusValid = false;
    uint8_t azkoyenCurrentStatusCode = 0;
    uint16_t azkoyenCurrentType1Paid = 0;
    uint16_t azkoyenCurrentType1Unpaid = 0;
    uint16_t azkoyenCurrentType2Paid = 0;
    uint16_t azkoyenCurrentType2Unpaid = 0;

    bool azkoyenLastCommandValid = false;
    uint8_t azkoyenLastCommandCode = 0;
    uint16_t azkoyenLastType1Paid = 0;
    uint16_t azkoyenLastType1Unpaid = 0;
    uint16_t azkoyenLastType2Paid = 0;
    uint16_t azkoyenLastType2Unpaid = 0;
    bool azkoyenLastCommandAccountedValid = false;
    uint8_t azkoyenLastCommandAccountedCode = 0;
    uint16_t azkoyenLastCommandAccountedPaidCoins = 0;

    bool azkoyenDiameterValid = false;
    uint16_t azkoyenType1Diameter = 0;
    uint16_t azkoyenType2Diameter = 0;

    bool azkoyenValueUnitsValid = false;
    uint16_t azkoyenType1ValueUnits = 0;
    uint16_t azkoyenType2ValueUnits = 0;

    bool azkoyenProgressAccountedValid = false;
    uint8_t azkoyenProgressAccountedCode = 0;
    uint32_t azkoyenProgressPaidBaseUnits = 0;
    uint16_t azkoyenRequestBaseUnits = 0;

    bool payoutRequestValid = false;
    uint32_t payoutRequestSerial = 0;
    uint16_t payoutRequestValue = 0;

    bool pollSnapshotValid = false;
    uint16_t lastRemainingValue = 0;
    uint16_t lastPaidValue = 0;
    uint16_t lastUnpaidValue = 0;

    uint32_t dispensedTotalValue = 0;
    bool lastDispenseStepValid = false;
    uint16_t lastDispenseStepValue = 0;

    uint16_t resetCount = 0;
    CoinValueState coinValues[16];
  };

  explicit CcTalkHopper(const HopperDataset& dataset = hopperDatasetAlbericiDiscriminator());
  bool matches(uint8_t addr) const override;
  const char* name() const override { return "HOPPER"; }

  void onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) override;
  void resetState();
  const HopperState* stateFor(uint8_t addr) const;
  void dumpState(Stream& out) const;
  void setAddressMask(uint8_t mask);
  void setConfiguredCoinValueCents(uint8_t addr, uint16_t valueCents);
  uint8_t addressMask() const { return _addressMask; }

private:
  static const uint8_t kAddrMin = 3;
  static const uint8_t kAddrMax = 10;
  static const uint8_t kStateCount = (kAddrMax - kAddrMin + 1);

  const HopperDataset& _dataset;
  HopperState _states[kStateCount];
  uint16_t _configuredCoinValueCents[kStateCount] = {0};
  uint8_t _addressMask = 0xFFu;

  // Blocchi di supporto alla decodifica e all'aggiornamento di stato.
  bool addressEnabled(uint8_t addr) const;
  const __FlashStringHelper* cmdDesc(uint8_t hdr) const;
  void printRequest(Stream& out, const CcTalkFrame& req);
  void printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp);
  void printRequestPayload(Stream& out, const CcTalkFrame& req);
  void printPayoutHiLowStatus(Stream& out, uint8_t status);
  void printTestHopperErrors(Stream& out, uint8_t err1, uint8_t err2);
  const __FlashStringHelper* azkoyenStatusLabel(uint8_t code) const;
  uint16_t azkoyenBaseCoinValueCents(const HopperState& state) const;
  bool azkoyenHasBaseCoinConfig(const HopperState& state) const;
  uint16_t azkoyenType1CoinValueCents(const HopperState& state) const;
  bool azkoyenHasType1CoinValue(const HopperState& state) const;
  uint16_t azkoyenType1ValueUnits(const HopperState& state) const;
  uint16_t azkoyenType2ValueUnits(const HopperState& state) const;
  bool azkoyenHasValueConfig(const HopperState& state) const;
  uint8_t azkoyenHopperStatusCounterDelta(uint8_t previous, uint8_t current) const;
  void updateAzkoyenDispensedFromHopperStatus(HopperState& state,
                                              uint8_t payoutCounter,
                                              uint8_t type1Remaining,
                                              uint8_t type1Paid,
                                              uint8_t type1Unpaid);
  uint32_t azkoyenPaidBaseUnits(const HopperState& state,
                                uint8_t code,
                                uint16_t type1Paid,
                                uint16_t type2Paid) const;
  uint32_t azkoyenUnpaidBaseUnits(const HopperState& state,
                                  uint8_t code,
                                  uint16_t type1Unpaid,
                                  uint16_t type2Unpaid) const;
  void updateAzkoyenDispensedValue(HopperState& state,
                                   uint8_t code,
                                   uint16_t type1Paid,
                                   uint16_t type2Paid);
  bool parseAzkoyenStatusPayload(const uint8_t* data, uint8_t len,
                                 uint8_t& code,
                                 uint16_t& type1Paid,
                                 uint16_t& type1Unpaid,
                                 uint16_t& type2Paid,
                                 uint16_t& type2Unpaid) const;
  void printAzkoyenStatusPayload(Stream& out, const uint8_t* data, uint8_t len) const;
  void printValueAsEuro(Stream& out, uint32_t units) const;
  uint16_t configuredCoinValueCents(uint8_t addr) const;
  uint16_t knownCoinValue(const HopperState& state) const;
  void updateDispensedFromPoll(HopperState& state, uint16_t remaining, uint16_t paid, uint16_t unpaid);
  void updateState(const CcTalkTransaction& t);
  HopperState* mutableStateFor(uint8_t addr);
};
