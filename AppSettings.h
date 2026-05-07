// Scopo del file:
// definisce la struttura `AppSettings`, che contiene la configurazione
// persistente dell'applicazione, e gli enum dei modelli periferica supportati.
#ifndef CCTALK_MULTI_SNIFFER_APP_SETTINGS_H
#define CCTALK_MULTI_SNIFFER_APP_SETTINGS_H

#include <stdint.h>
#include <string.h>

namespace ccms {

// Enumerazione dei modelli hopper supportati dal firmware.
// Anche se oggi esiste un solo modello concreto, mantenere un enum separato
// evita di spargere valori "magici" nel resto del codice.
enum HopperModelType : uint8_t {
  HOPPER_MODEL_ALBERICI_DISCRIMINATOR = 1,
  HOPPER_MODEL_ALBERICI_HOPPERCD = 2,
  HOPPER_MODEL_SUZO_EVOLUTION = 3,
  HOPPER_MODEL_AZKOYEN_DISCRIMINATOR = 4
};

// Enumerazione dei modelli bill validator supportati.
enum BillValidatorModelType : uint8_t {
  BILL_VALIDATOR_MODEL_MD100 = 1,
  BILL_VALIDATOR_MODEL_SMART_PAYOUT = 2,
  BILL_VALIDATOR_MODEL_IPRO = 3
};

// Profilo statico dei canali monetari della NRI Falcon.
// I blocchi corrispondono alle due configurazioni riportate sull'etichetta
// macchina e permettono di valorizzare i crediti anche quando il master non
// interroga mai i Coin ID via 0xB8/0xB9.
enum CoinAcceptorFalconProfileType : uint8_t {
  COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0 = 1,
  COIN_ACCEPTOR_FALCON_PROFILE_BLOCK1 = 2
};

// Validator e funzioni di sanitizzazione:
// servono a rendere robusta la lettura di dati provenienti da NVS/web UI.
inline bool isValidHopperModel(uint8_t model) {
  return model == HOPPER_MODEL_ALBERICI_DISCRIMINATOR ||
         model == HOPPER_MODEL_ALBERICI_HOPPERCD ||
         model == HOPPER_MODEL_SUZO_EVOLUTION ||
         model == HOPPER_MODEL_AZKOYEN_DISCRIMINATOR;
}

inline bool isValidBillValidatorModel(uint8_t model) {
  return model == BILL_VALIDATOR_MODEL_MD100 ||
         model == BILL_VALIDATOR_MODEL_SMART_PAYOUT ||
         model == BILL_VALIDATOR_MODEL_IPRO;
}

inline bool isValidCoinAcceptorFalconProfile(uint8_t profile) {
  return profile == COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0 ||
         profile == COIN_ACCEPTOR_FALCON_PROFILE_BLOCK1;
}

inline uint8_t sanitizeHopperModel(uint8_t model) {
  return isValidHopperModel(model) ? model : (uint8_t)HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
}

inline uint8_t sanitizeBillValidatorModel(uint8_t model) {
  return isValidBillValidatorModel(model) ? model : (uint8_t)BILL_VALIDATOR_MODEL_MD100;
}

inline uint8_t sanitizeCoinAcceptorFalconProfile(uint8_t profile) {
  return isValidCoinAcceptorFalconProfile(profile)
             ? profile
             : (uint8_t)COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0;
}

inline bool hasHttpUrlScheme(const char* value) {
  if (!value) return false;
  return strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0;
}

inline bool buildSiblingPhpUrl(const char* baseUrl,
                               const char* phpFileName,
                               char* out,
                               size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  if (!baseUrl || !phpFileName || phpFileName[0] == '\0') return false;

  size_t start = 0;
  size_t end = strlen(baseUrl);
  while (start < end &&
         (baseUrl[start] == ' ' || baseUrl[start] == '\t' ||
          baseUrl[start] == '\r' || baseUrl[start] == '\n')) {
    start++;
  }
  while (end > start &&
         (baseUrl[end - 1] == ' ' || baseUrl[end - 1] == '\t' ||
          baseUrl[end - 1] == '\r' || baseUrl[end - 1] == '\n')) {
    end--;
  }
  if (start >= end || !hasHttpUrlScheme(baseUrl + start)) return false;

  size_t queryPos = end;
  for (size_t i = start; i < end; i++) {
    if (baseUrl[i] == '?') {
      queryPos = i;
      break;
    }
  }

  size_t dirEnd = queryPos;
  if ((dirEnd - start) >= 4 && memcmp(baseUrl + dirEnd - 4, ".php", 4) == 0) {
    size_t slashPos = dirEnd;
    while (slashPos > start && baseUrl[slashPos - 1] != '/') slashPos--;
    if (slashPos <= start) return false;
    dirEnd = slashPos;
  }

  const bool needsSlash = (dirEnd <= start || baseUrl[dirEnd - 1] != '/');
  const size_t fileLen = strlen(phpFileName);
  const size_t dirLen = dirEnd - start;
  const size_t totalLen = dirLen + (needsSlash ? 1U : 0U) + fileLen;
  if (totalLen >= outLen) return false;

  if (dirLen > 0) memcpy(out, baseUrl + start, dirLen);
  size_t pos = dirLen;
  if (needsSlash) out[pos++] = '/';
  memcpy(out + pos, phpFileName, fileLen);
  out[pos + fileLen] = '\0';
  return true;
}

inline bool buildDerivedRemoteEventUrl(const char* serverUrl, char* out, size_t outLen) {
  return buildSiblingPhpUrl(serverUrl, "remote_registro_eventi.php", out, outLen);
}

// Range indirizzi usati per decidere quali periferiche contribuiscono ai
// contatori economici. Il bit 0 di ogni mask corrisponde all'indirizzo minimo.
static const uint8_t kHopperAddressMin = 3;
static const uint8_t kHopperAddressMax = 10;
static const uint8_t kBillValidatorAddressMin = 40;
static const uint8_t kBillValidatorAddressMax = 50;
static const uint8_t kHopperAddressCount = (uint8_t)(kHopperAddressMax - kHopperAddressMin + 1);
static const uint8_t kBillValidatorAddressCount =
    (uint8_t)(kBillValidatorAddressMax - kBillValidatorAddressMin + 1);
static const uint16_t kDefaultHopperCoinValueCents = 0;
static const uint8_t kDefaultCoinInHopperMask = 0x01u;   // Hopper 3
static const uint8_t kDefaultCoinOutHopperMask = 0x06u;  // Hopper 4 + 5
static const uint8_t kAllHopperMask = 0xFFu;             // Hopper 3..10
static const uint16_t kAllBillValidatorMask = 0x07FFu;   // BV 40..50

inline bool isValidHopperAddress(uint8_t addr) {
  return addr >= kHopperAddressMin && addr <= kHopperAddressMax;
}

inline bool isValidBillValidatorAddress(uint8_t addr) {
  return addr >= kBillValidatorAddressMin && addr <= kBillValidatorAddressMax;
}

inline uint8_t hopperAddressBit(uint8_t addr) {
  return isValidHopperAddress(addr) ? (uint8_t)(1u << (addr - kHopperAddressMin)) : 0u;
}

inline uint8_t hopperAddressIndex(uint8_t addr) {
  return isValidHopperAddress(addr) ? (uint8_t)(addr - kHopperAddressMin) : 0xFFu;
}

inline uint16_t billValidatorAddressBit(uint8_t addr) {
  return isValidBillValidatorAddress(addr) ? (uint16_t)(1u << (addr - kBillValidatorAddressMin)) : 0u;
}

inline uint8_t sanitizeHopperContributionMask(uint8_t mask) {
  const uint16_t validMask = (uint16_t)((1u << kHopperAddressCount) - 1u);
  return (uint8_t)(mask & validMask);
}

inline uint16_t sanitizeBillValidatorContributionMask(uint16_t mask) {
  const uint16_t validMask = (uint16_t)((1u << kBillValidatorAddressCount) - 1u);
  return (uint16_t)(mask & validMask);
}

inline uint8_t sanitizeHopperModelAssignmentMask(uint8_t mask) {
  return sanitizeHopperContributionMask(mask);
}

inline uint16_t sanitizeBillValidatorModelAssignmentMask(uint16_t mask) {
  return sanitizeBillValidatorContributionMask(mask);
}

// Configurazione persistente dell'applicazione.
// I campi stringa hanno dimensione fissa per semplificare:
// - memorizzazione in NVS
// - serializzazione
// - assenza di allocazioni dinamiche lato firmware
struct AppSettings {
  // Dimensioni comprensive del terminatore '\0'.
  static const size_t kWifiSsidSize = 33;
  static const size_t kWifiPassSize = 65;
  static const size_t kServerUrlSize = 160;
  static const size_t kRemoteEventUrlSize = 160;
  static const size_t kDbHostSize = 96;
  static const size_t kDbNameSize = 64;
  static const size_t kDbUserSize = 64;
  static const size_t kDbPassSize = 96;
  static const size_t kLocationCodeSize = 15;
  static const size_t kApiKeySize = 65;

  char wifiSsid[kWifiSsidSize] = {0};
  char wifiPass[kWifiPassSize] = {0};
  bool saveWifiCredentials = false;
  char serverUrl[kServerUrlSize] = {0};
  char remoteEventUrl[kRemoteEventUrlSize] = {0};
  char dbHost[kDbHostSize] = {0};
  uint16_t dbPort = 3306;
  char dbName[kDbNameSize] = {0};
  char dbUser[kDbUserSize] = {0};
  char dbPass[kDbPassSize] = {0};
  char locationCode[kLocationCodeSize] = {0};
  char apiKey[kApiKeySize] = {0};
  // Campi legacy mantenuti per compatibilita con configurazioni salvate
  // precedenti all'introduzione dell'assegnazione modello-per-indirizzo.
  uint8_t hopperModel = HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
  uint8_t billValidatorModel = BILL_VALIDATOR_MODEL_MD100;
  // Assegnazione del modello parser per singolo indirizzo periferica.
  uint8_t hopperAlbericiDiscriminatorMask = kAllHopperMask;
  uint8_t hopperAlbericiHopperCdMask = 0;
  uint8_t hopperSuzoEvolutionMask = 0;
  uint8_t hopperAzkoyenDiscriminatorMask = 0;
  uint16_t billValidatorMd100Mask = kAllBillValidatorMask;
  uint16_t billValidatorSmartPayoutMask = 0;
  uint16_t billValidatorIproMask = 0;
  bool coinAcceptorInEnabled = true;
  uint8_t coinAcceptorFalconProfile = COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0;
  uint8_t coinInHopperMask = kDefaultCoinInHopperMask;
  uint8_t coinOutHopperMask = kDefaultCoinOutHopperMask;
  uint16_t hopperCoinValueCents[kHopperAddressCount] = {0};
  uint16_t billInValidatorMask = kAllBillValidatorMask;
  uint16_t billOutValidatorMask = kAllBillValidatorMask;
  bool valid = false;

  // Riporta la struttura allo stato "default di fabbrica".
  // E utile sia prima di un load, sia quando il parsing di input fallisce.
  void clear() {
    memset(wifiSsid, 0, sizeof(wifiSsid));
    memset(wifiPass, 0, sizeof(wifiPass));
    saveWifiCredentials = false;
    memset(serverUrl, 0, sizeof(serverUrl));
    memset(remoteEventUrl, 0, sizeof(remoteEventUrl));
    memset(dbHost, 0, sizeof(dbHost));
    dbPort = 3306;
    memset(dbName, 0, sizeof(dbName));
    memset(dbUser, 0, sizeof(dbUser));
    memset(dbPass, 0, sizeof(dbPass));
    memset(locationCode, 0, sizeof(locationCode));
    memset(apiKey, 0, sizeof(apiKey));
    hopperModel = HOPPER_MODEL_ALBERICI_DISCRIMINATOR;
    billValidatorModel = BILL_VALIDATOR_MODEL_MD100;
    hopperAlbericiDiscriminatorMask = kAllHopperMask;
    hopperAlbericiHopperCdMask = 0;
    hopperSuzoEvolutionMask = 0;
    hopperAzkoyenDiscriminatorMask = 0;
    billValidatorMd100Mask = kAllBillValidatorMask;
    billValidatorSmartPayoutMask = 0;
    billValidatorIproMask = 0;
    coinAcceptorInEnabled = true;
    coinAcceptorFalconProfile = COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0;
    coinInHopperMask = kDefaultCoinInHopperMask;
    coinOutHopperMask = kDefaultCoinOutHopperMask;
    for (uint8_t i = 0; i < kHopperAddressCount; i++) {
      hopperCoinValueCents[i] = kDefaultHopperCoinValueCents;
    }
    billInValidatorMask = kAllBillValidatorMask;
    billOutValidatorMask = kAllBillValidatorMask;
    valid = false;
  }
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_APP_SETTINGS_H
