// Scopo del file:
// [ANOMALY_DEBUG] modulo di debug TEMPORANEO per rilevare sul campo
// un'anomalia di conteggio IN/OUT (banconoteIn-Out+moneteIn-Out deve tornare
// a zero a macchina "a riposo") e conservare il contesto ccTalk che l'ha
// causata, per periferica: comando, counter, delta.
//
// Rimozione a fine indagine: cancellare questo file + AnomalyDebugLog.cpp, e
// tutte le righe marcate "[ANOMALY_DEBUG]" in CcTalkBillValidator.cpp,
// CcTalkHopper.cpp, CcTalkCoinAcceptor.cpp, CcTalkMultiSniffer.ino e
// web/WebServerService.h/.cpp. Nessun altro file dipende da questo modulo.
#ifndef CCTALK_MULTI_SNIFFER_ANOMALY_DEBUG_LOG_H
#define CCTALK_MULTI_SNIFFER_ANOMALY_DEBUG_LOG_H

#include <Arduino.h>
#include <Wire.h>

namespace anomalydebug {

enum DeviceKind : uint8_t {
  DEV_NONE = 0,
  DEV_BILL_VALIDATOR = 1,
  DEV_HOPPER = 2,
  DEV_COIN_ACCEPTOR = 3
};

// Fino a 4 periferiche economiche distinte (tipica installazione: 1 BV +
// 2 hopper + 1 gettoniera), assegnate dinamicamente al primo evento
// osservato per ogni coppia (tipo, indirizzo). 1000 eventi/canale in RAM:
// 4*1000*8 byte = 32000 byte, trascurabile. Lo stesso numero di entry viene
// anche scritto per intero in FRAM al momento della cattura (vedi .cpp per
// il conto esatto dei byte occupati sul chip da 32KB).
static const uint8_t kChannelCount = 4;
static const uint16_t kEntriesPerChannel = 1000;

// Finestra di quiete: il controllo saldo scatta solo se sono passati almeno
// questi millisecondi dall'ultimo evento che ha mosso un totalizzatore.
// Valore scelto per coprire il tempo massimo di erogazione a monete singole
// della macchina (vedi discussione con l'utente). Tunabile qui.
static const uint32_t kSettleMs = 45000UL;

// Inizializza l'accesso alla FRAM per la cattura (stesso chip/bus della
// persistenza economica esistente, indirizzo I2C diverso in byte, non in
// device: va richiamato con lo stesso Wire/indirizzo di FramPersistence).
bool begin(TwoWire& wire, uint8_t i2cAddress);

// Registra un evento che ha fatto avanzare il totalizzatore di una
// periferica. deltaCents e' la variazione (sempre >=0, i totalizzatori dei
// decoder sono monotoni crescenti) di questo singolo evento elementare
// (una banconota, una moneta, uno step di erogazione — non un batch di poll).
// Aggiorna anche il timer di quiete usato da tick().
void logEvent(DeviceKind kind, uint8_t addr, uint8_t cmdHeader, uint8_t eventCounter, uint16_t deltaCents);

// Da chiamare periodicamente (task aux, NON dal task sniffer) passando il
// saldo corrente (BanconoteIn-Out)+(MoneteIn-Out) in centesimi. Se e'
// trascorsa la finestra di quiete dall'ultimo evento e il saldo e' diverso
// da zero, valuta l'anomalia una sola volta per periodo di quiete e
// incrementa il contatore di occorrenze. Vengono mantenute in FRAM DUE
// catture complete con dettaglio comando/counter/delta (non solo il
// contatore): la PRIMA occorrenza mai vista da avvio/da clearCapturedAnomaly()
// (protetta, mai sovrascritta) e la PIU' RECENTE (risalvata per intero ad
// ogni nuova occorrenza) — cosi' anche se la prima cattura non viene
// analizzata per giorni, il dettaglio dell'occorrenza piu' recente resta
// comunque disponibile.
void tick(int32_t currentSaldoCents, uint32_t nowMs);

// True se e' presente almeno la prima cattura salvata in FRAM (mai letta/
// cancellata dall'ultimo trigger), anche se il firmware nel frattempo e'
// stato riavviato (la cattura sopravvive al reset perche' e' su FRAM).
bool hasCapturedAnomaly();

// Riarma la cattura (entrambi gli slot, "first" e "latest"): da chiamare
// SOLO dopo aver scaricato/copiato il log delle catture correnti, altrimenti
// si perdono alla prossima occorrenza.
void clearCapturedAnomaly();

// Selettore di slot per visitFramChannel(): la prima occorrenza mai vista
// (protetta) oppure la piu' recente (sempre aggiornata).
enum FramSlot : uint8_t {
  FRAM_SLOT_FIRST = 0,
  FRAM_SLOT_LATEST = 1
};

// Metadati delle catture salvate in FRAM (se presenti). occurrenceCount e'
// condiviso: quante volte l'anomalia e' stata rilevata in totale, anche
// dopo il primo salvataggio.
struct FramCaptureInfo {
  bool present = false; // true se almeno la prima cattura esiste
  uint32_t occurrenceCount = 0;

  uint32_t firstTriggerTsMs = 0;          // millis() del boot in cui e' scattata la prima occorrenza (informativo)
  int64_t firstTriggerWallClockEpoch = 0; // 0 se non disponibile (NTP non sincronizzato al trigger)
  int32_t firstImbalanceCents = 0;

  uint32_t latestTriggerTsMs = 0;          // come sopra, per l'occorrenza piu' recente
  int64_t latestTriggerWallClockEpoch = 0;
  int32_t latestImbalanceCents = 0;
};
FramCaptureInfo framCaptureInfo();

// Callback di iterazione per esportare il log (usato dal web server per
// generare il CSV in streaming, senza materializzare tutto in un buffer).
// wallClockEpoch e' 0 se non ricostruibile (nessuna sincronizzazione NTP
// disponibile all'epoca dell'evento); tsMs resta sempre valido come
// riferimento relativo (millis() del boot in cui l'evento e' avvenuto).
typedef void (*EntryVisitor)(void* ctx,
                              DeviceKind kind,
                              uint8_t addr,
                              uint8_t cmdHeader,
                              uint8_t eventCounter,
                              uint16_t deltaCents,
                              uint32_t tsMs,
                              int64_t wallClockEpoch);

// Quanti canali RAM sono attualmente assegnati (0..kChannelCount).
uint8_t ramChannelCount();
// Itera in ordine cronologico (dal piu' vecchio) le entry del canale RAM
// live indicato (0..kChannelCount-1). Ritorna false se il canale non e'
// mai stato assegnato.
bool visitRamChannel(uint8_t channelIndex, EntryVisitor visitor, void* ctx);

// Come sopra ma rilegge dalla cattura salvata in FRAM per lo slot indicato
// (indipendente dallo stato RAM corrente: funziona anche dopo un riavvio
// successivo al trigger). Ritorna false se quello slot non ha una cattura o
// la lettura/checksum falliscono. La profondita' per canale salvata in FRAM
// e' inferiore a quella del ring RAM live (vedi commento nel .cpp), per
// poter tenere due catture complete nei 32KB del chip.
bool visitFramChannel(FramSlot slot, uint8_t channelIndex, EntryVisitor visitor, void* ctx);

} // namespace anomalydebug

#endif // CCTALK_MULTI_SNIFFER_ANOMALY_DEBUG_LOG_H
