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
  // Motivo per cui un delta rilevato dal polling 0x85/0xAB non e stato
  // incluso nel totale monete "accettato" (vedi coinValueAccepted).
  enum CoinFilterReason : uint8_t {
    COIN_FILTER_REASON_NONE = 0,             // accettata, nessuna esclusione
    COIN_FILTER_REASON_SORTER = 1,           // percorso sorter (0xD2) <> 1, osservato su questo indirizzo
    COIN_FILTER_REASON_MANUAL_FILTER = 2     // taglio non nel filtro manuale (1e/2e)
  };

  // Sui dataset Azkoyen il device puo esporre lo stesso conteggio erogato
  // tramite piu famiglie di comandi (status generico 0xA6/0xAB/0x85, oppure
  // status "custom" 0x13/0x15/0x23). Sono percorsi indipendenti che, se il
  // master reale li usa entrambi per lo stesso indirizzo, finirebbero per
  // accreditare due volte la stessa moneta erogata su dispensedTotalValue.
  // Per evitarlo, la prima fonte che produce un delta > 0 "vince" e resta
  // l'unica autorevole per quell'indirizzo finche' non arriva un reset (0x01).
  enum DispenseAccountingSource : uint8_t {
    DISPENSE_SOURCE_NONE = 0,
    DISPENSE_SOURCE_A6_AZKOYEN = 1,      // 0xA6 con statusMode AZKOYEN_TYPE1_PAYOUT_COUNTER
    DISPENSE_SOURCE_POLL_GENERIC = 2,    // 0xAB / 0x85
    DISPENSE_SOURCE_AZKOYEN_CUSTOM = 3   // 0x13 / 0x15 / 0x23
  };

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
    // Fonte comando che ha "vinto" il diritto ad accumulare su
    // dispensedTotalValue per questo indirizzo (vedi DispenseAccountingSource).
    DispenseAccountingSource dispenseAccountingSource = DISPENSE_SOURCE_NONE;

    // Valore riconosciuto dal polling ma escluso dal totale perche il taglio
    // risulta scartato dal sorter (percorso <> 1 osservato via 0xD2) oppure,
    // in assenza di quell'informazione, non rientra nel filtro monete
    // configurato manualmente dall'utente per l'indirizzo (vedi
    // CcTalkHopper::coinValueAccepted / setConfiguredCoinValueCents).
    uint32_t excludedTotalValue = 0;
    bool lastExcludedStepValid = false;
    uint16_t lastExcludedStepValue = 0;
    CoinFilterReason lastExclusionReason = COIN_FILTER_REASON_NONE;

    uint16_t resetCount = 0;
    CoinValueState coinValues[16];
    // Percorso sorter corrente per posizione moneta (1-based nel protocollo,
    // indicizzato qui 0-based), osservato dai comandi 0xD2 confermati con
    // ACK. 0 = mai osservato = default di fabbrica (path 1, "accettata").
    uint8_t coinSorterPath[16] = {0};
  };

  explicit CcTalkHopper(const HopperDataset& dataset = hopperDatasetAlbericiDiscriminator());
  bool matches(uint8_t addr) const override;
  const char* name() const override { return "HOPPER"; }

  void onTransaction(const CcTalkTransaction& t, Stream& out, bool printRaw) override;
  void resetState();
  const HopperState* stateFor(uint8_t addr) const;
  void dumpState(Stream& out) const;
  void setAddressMask(uint8_t mask);
  // Taglio/i moneta accettati per l'indirizzo, configurabile dall'utente
  // (pagina impostazioni):
  // - 0 = "Discriminatore" (default): nessun filtro statico, il totale
  //   monete segue il percorso sorter osservato sul bus per QUESTO
  //   indirizzo (0xD2, confermato ACK, correlato alla coin table). Un
  //   taglio viene ESCLUSO solo se e stato osservato esplicitamente un
  //   percorso diverso da 1 ("accettata") per quella posizione su questo
  //   stesso indirizzo. In assenza di segnale live (nessun 0xD2 mai
  //   ricevuto per questo indirizzo/posizione, es. hopper usato solo per
  //   erogazione senza sorter) la moneta viene comunque contata: per spec
  //   ccTalk il default di fabbrica/dopo reset di ogni posizione e gia
  //   path 1, quindi "nessun segnale" equivale ad "accettata".
  // - 100/200 = solo quel taglio, filtro statico diretto (usato anche per il
  //   conteggio A7/A6 e per il valore base Azkoyen sugli hopper mono-moneta
  //   senza sorter, dove il segnale live 0xD2 non esiste).
  // - kCoinFilterComboOneTwoEuro (300, non piu selezionabile da UI ma ancora
  //   supportato per retrocompatibilita) = solo 1e o 2e, filtro statico.
  void setConfiguredCoinValueCents(uint8_t addr, uint16_t valueCents);
  // Percorso sorter (1..5, il sorter Evolution supporta fino a 5 vie) da
  // considerare "di esclusione": una moneta instradata su questo percorso
  // NON viene contata nel totale, qualunque altro percorso osservato SI.
  // Valido solo in modalita "Discriminatore" (setConfiguredCoinValueCents==0);
  // valori fuori range 1..5 (incluso 0) ricadono sul default storico (2).
  void setConfiguredExcludedSorterPath(uint8_t addr, uint8_t path);
  uint8_t addressMask() const { return _addressMask; }

private:
  static const uint8_t kAddrMin = 3;
  static const uint8_t kAddrMax = 10;
  static const uint8_t kStateCount = (kAddrMax - kAddrMin + 1);
  // Sentinella per il filtro monete configurabile: nessun taglio reale puo
  // valere 300 centesimi in questo contesto, quindi e sicuro riservarla come
  // codice per "accetta solo 1e o 2e" (vedi setConfiguredCoinValueCents).
  static const uint16_t kCoinFilterComboOneTwoEuro = 300;
  // Percorso sorter di esclusione di default quando l'utente non ne ha
  // configurato uno esplicito per l'indirizzo (vedi setConfiguredExcludedSorterPath).
  static const uint8_t kDefaultExcludedSorterPath = 2;

  const HopperDataset& _dataset;
  HopperState _states[kStateCount];
  uint16_t _configuredCoinValueCents[kStateCount] = {0};
  uint8_t _configuredExcludedSorterPath[kStateCount] = {0};
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
  // Arbitra fra le famiglie di comandi che possono riportare l'erogato per lo
  // stesso indirizzo (vedi DispenseAccountingSource): la prima fonte che
  // richiede il diritto di contabilizzare lo ottiene stabilmente per
  // l'indirizzo, finche' non arriva un reset (0x01). Ritorna true se `source`
  // e' (o diventa ora) la fonte autorevole per questo stato, false se
  // un'altra fonte ha gia' il diritto e questo delta non va accreditato.
  bool claimOrCheckDispenseSource(HopperState& state, DispenseAccountingSource source) const;
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
                                   uint8_t cmdHeader,
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
  uint8_t configuredExcludedSorterPath(uint8_t addr) const;
  uint16_t knownCoinValue(const HopperState& state) const;
  int8_t findCoinPositionByValue(const HopperState& state, uint16_t valueCents) const;
  bool coinValueAccepted(const HopperState& state, uint16_t valueCents, CoinFilterReason& reason) const;
  void updateDispensedFromPoll(HopperState& state, uint8_t cmdHeader, uint8_t eventCounter,
                                uint16_t remaining, uint16_t paid, uint16_t unpaid);
  void updateState(const CcTalkTransaction& t);
  HopperState* mutableStateFor(uint8_t addr);
};
