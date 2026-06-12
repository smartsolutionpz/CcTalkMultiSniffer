// Scopo del file:
// dichiara `CcTalkBillValidator`, il decoder/stato dei validatori di banconote
// ccTalk, inclusi eventi bufferizzati, contatori e recycler.
#pragma once
#include <Arduino.h>
#include "CcTalkDevice.h"
#include "CcTalkBillValidatorDataset.h"

// Decoder/stato per bill validator agli indirizzi 40..50.
// La classe ha due ruoli:
// - produrre una decodifica leggibile dei frame ccTalk
// - mantenere una memoria runtime dei valori economici e diagnostici
class CcTalkBillValidator : public CcTalkDevice {
public:
  // Coppia A/B riportata dal comando 0x9F dei buffered events.
  struct BufferedEventState {
    uint8_t a = 0;
    uint8_t b = 0;
  };

  // Bill ID letto dinamicamente dal device per indice.
  struct BillIdState {
    bool valid = false;
    char id[24] = {0};
  };

  // Snapshot runtime associato a un singolo indirizzo BV.
  // Viene aggiornato in modo incrementale ogni volta che passano transazioni
  // sufficienti a confermare un certo dato.
  struct BillValidatorState {
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

    bool eventCounterValid = false;
    uint8_t eventCounter = 0;
    bool eventCounterSeen = false;
    uint8_t lastProcessedEventCounter = 0;
    BufferedEventState events[5];

    uint32_t acceptedTotalEuro = 0;
    bool lastAcceptedValid = false;
    uint8_t lastAcceptedBillType = 0;
    uint8_t lastAcceptedEuro = 0;
    bool pendingAcceptedRouteValid = false;
    uint8_t pendingAcceptedRouteEuro = 0;
    uint32_t cashboxTotalEuro = 0;
    bool lastCashboxValid = false;
    uint8_t lastCashboxEuro = 0;
    uint32_t dispensedTotalEuro = 0;
    bool lastDispensedValid = false;
    uint8_t lastDispensedEuro = 0;

    bool operatingModeValid = false;
    uint8_t operatingMode = 0;

    bool masterInhibitValid = false;
    bool masterInhibitOff = false;

    bool inhibitMaskValid = false;
    uint8_t inhibitMask[8] = {0};
    uint8_t inhibitMaskLen = 0;

    bool recyclerInventoryValid = false;
    uint16_t recyclerCount10 = 0;
    uint16_t recyclerCount20 = 0;
    uint16_t recyclerCount50 = 0;
    uint32_t recyclerInventoryTotalEuro = 0;
    bool iproRecycleBoxMapValid = false;
    uint8_t iproRecycleBoxEuro[2] = {0};

    bool lastFaultValid = false;
    uint8_t lastFaultCode = 0;

    bool lastStatusPayloadValid = false;
    uint8_t lastStatusPayload[32] = {0};
    uint8_t lastStatusPayloadLen = 0;

    uint16_t resetCount = 0;
    BillIdState billIds[32];
  };

  explicit CcTalkBillValidator(const BillValidatorDataset& dataset);
  bool matches(uint8_t addr) const override;
  const char* name() const override { return "BILL_VALIDATOR"; }

  void onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) override;
  void resetState();
  const BillValidatorState* stateFor(uint8_t addr) const;
  void dumpState(Stream& out) const;
  void setAddressMask(uint16_t mask);
  bool preloadRecyclerInventory(uint8_t addr, uint16_t c10, uint16_t c20, uint16_t c50);
  uint16_t addressMask() const { return _addressMask; }
  // Inietta direttamente un credito banconota (EUR) nel totalizzatore del BV.
  // Usato per banconote JCM push-mode: il checksum non standard bypassa il
  // routing normale, quindi la contabilita va aggiornata esternamente.
  void injectAcceptedEuro(uint8_t addr, uint32_t euros);

private:
  static const uint8_t kAddrMin = 40;
  static const uint8_t kAddrMax = 50;
  static const uint8_t kStateCount = (kAddrMax - kAddrMin + 1);

  const BillValidatorDataset& _dataset;
  BillValidatorState _states[kStateCount];
  uint16_t _addressMask = 0x07FFu;

  // Blocchi di supporto alla decodifica e all'aggiornamento di stato.
  bool addressEnabled(uint8_t addr) const;
  const __FlashStringHelper* cmdDesc(uint8_t hdr) const;
  void printRequest(Stream& out, const CcTalkFrame& req);
  void printRequestPayload(Stream& out, const CcTalkFrame& req);
  void printResponse(Stream& out, uint8_t hostHdr, const CcTalkFrame& resp);
  void printBillEvents(Stream& out, const CcTalkFrame& resp);
  void printBillEvent(Stream& out, uint8_t idx, uint8_t a, uint8_t b);
  const __FlashStringHelper* billEventLabel(uint8_t a, uint8_t b) const;
  const __FlashStringHelper* billEventClass(uint8_t a, uint8_t b) const;
  const __FlashStringHelper* billFaultLabel(uint8_t code) const;
  bool isAcceptedCreditEvent(uint8_t a, uint8_t b) const;
  bool isCashboxCreditEvent(uint8_t a, uint8_t b) const;
  bool isRecyclerStoredEvent(uint8_t a, uint8_t b) const;
  bool billTypeToEuro(uint8_t billType, uint8_t& euro) const;
  bool billTypeToEuro(const BillValidatorState& state, uint8_t billType, uint8_t& euro) const;
  bool billIdToEuro(const char* billId, uint16_t& euro) const;
  int16_t payoutDenomFromRequest(const CcTalkFrame& req, uint8_t& code) const;
  bool requestValueCents(const CcTalkFrame& req, uint32_t& valueCents) const;
  bool responseCountValue(const uint8_t* data, uint8_t len, uint16_t& count) const;
  uint8_t eventCounterDelta(uint8_t previous, uint8_t current) const;
  void accumulateAcceptedBills(BillValidatorState& state, const CcTalkFrame& resp) const;
  uint8_t smartPayoutStatusDataLen(uint8_t code) const;
  bool usesMd100PayoutCommands() const;
  bool usesSmartPayoutValueCommands() const;
  bool usesMd100RecyclerInventory() const;
  bool usesSmartPayoutRecyclerInventory() const;
  bool usesIproRecyclerInventory() const;
  void accumulateMd100Dispense(BillValidatorState& state, const CcTalkFrame& req) const;
  void accumulateSmartPayoutDispense(BillValidatorState& state, const CcTalkFrame& req) const;
  bool applyRecyclerDelta(BillValidatorState& state, uint8_t euro, int8_t delta) const;
  bool applyIproRecycleCurrencySetting(BillValidatorState& state, const uint8_t* data, uint8_t len) const;
  bool applyIproRecyclerCurrent(BillValidatorState& state, const uint8_t* data, uint8_t len) const;
  bool iproBillTypeToRecyclerEuro(const BillValidatorState& state, uint8_t billType, uint8_t& euro) const;
  void accumulateSmartPayoutStatus(BillValidatorState& state, const uint8_t* data, uint8_t len) const;
  bool applyMd100RecyclerInventory(BillValidatorState& state, const uint8_t* data, uint8_t len) const;
  bool applySmartPayoutRecyclerInventory(BillValidatorState& state,
                                         const CcTalkFrame& req,
                                         const CcTalkFrame& resp) const;
  bool parseRecyclerInventory(const uint8_t* data, uint8_t len,
                              uint16_t& c10, uint16_t& c20, uint16_t& c50) const;
  bool updateRecyclerCountFromValue(BillValidatorState& state, uint32_t valueCents, uint16_t count) const;
  void refreshRecyclerTotals(BillValidatorState& state) const;
  int16_t recyclerDenomFromKey(uint32_t key) const;
  void updateState(const CcTalkTransaction& t);
  BillValidatorState* mutableStateFor(uint8_t addr);
};
