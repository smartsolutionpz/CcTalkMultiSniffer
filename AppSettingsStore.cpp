// Scopo del file:
// implementa `AppSettingsStore`, traducendo la struttura `AppSettings`
// in chiavi persistenti memorizzate nello storage ESP32.
#include "AppSettingsStore.h"

#include <Arduino.h>
#include <string.h>
#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>
#endif

namespace ccms {

namespace {
static const char* const kNs = "ccmscfg";

// Copia una String Arduino dentro un buffer C a dimensione fissa, garantendo
// sempre il terminatore finale ed evitando overflow.
static void copyString(const String& in, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  size_t n = in.length();
  if (n > outLen - 1) n = outLen - 1;
  memcpy(out, in.c_str(), n);
  out[n] = '\0';
}
} // namespace

bool AppSettingsStore::begin() {
#if defined(ARDUINO_ARCH_ESP32)
  // `Preferences::begin()` crea/apre il namespace. Lo richiudiamo subito:
  // qui ci interessa solo sapere se il backend NVS e disponibile.
  Preferences prefs;
  const bool ok = prefs.begin(kNs, false);
  prefs.end();
  return ok;
#else
  return false;
#endif
}

bool AppSettingsStore::load(AppSettings& out) {
  // Si parte da uno stato noto anche in caso di ritorno anticipato.
  out.clear();

#if defined(ARDUINO_ARCH_ESP32)
  Preferences prefs;
  if (!prefs.begin(kNs, true)) return false;

  // `valid` separa il caso "namespace presente ma mai inizializzato" dal caso
  // "configurazione utente realmente salvata".
  const bool valid = prefs.getBool("valid", false);
  out.saveWifiCredentials = prefs.getBool("wifi_save", false);
  copyString(prefs.getString("wifi_ssid", ""), out.wifiSsid, sizeof(out.wifiSsid));
  copyString(prefs.getString("wifi_pass", ""), out.wifiPass, sizeof(out.wifiPass));
  copyString(prefs.getString("srv_url", ""), out.serverUrl, sizeof(out.serverUrl));
  copyString(prefs.getString("revt_url", ""), out.remoteEventUrl, sizeof(out.remoteEventUrl));
  if (out.remoteEventUrl[0] == '\0') {
    buildDerivedRemoteEventUrl(out.serverUrl, out.remoteEventUrl, sizeof(out.remoteEventUrl));
  }
  copyString(prefs.getString("db_host", ""), out.dbHost, sizeof(out.dbHost));
  out.dbPort = (uint16_t)prefs.getUShort("db_port", 3306);
  copyString(prefs.getString("db_name", ""), out.dbName, sizeof(out.dbName));
  copyString(prefs.getString("db_user", ""), out.dbUser, sizeof(out.dbUser));
  copyString(prefs.getString("db_pass", ""), out.dbPass, sizeof(out.dbPass));
  copyString(prefs.getString("loc_code", ""), out.locationCode, sizeof(out.locationCode));
  copyString(prefs.getString("api_key", ""), out.apiKey, sizeof(out.apiKey));
  copyString(prefs.getString("mqtt_host", ""), out.mqttBrokerHost, sizeof(out.mqttBrokerHost));
  out.mqttBrokerPort = (uint16_t)prefs.getUShort("mqtt_port", 1883);
  copyString(prefs.getString("mqtt_user", ""), out.mqttUsername, sizeof(out.mqttUsername));
  copyString(prefs.getString("mqtt_pass", ""), out.mqttPassword, sizeof(out.mqttPassword));
  out.mqttEnabled = prefs.getBool("mqtt_en", false);
  out.hopperModel = sanitizeHopperModel(prefs.getUChar("hop_model", HOPPER_MODEL_ALBERICI_DISCRIMINATOR));
  out.billValidatorModel =
      sanitizeBillValidatorModel(prefs.getUChar("bv_model", BILL_VALIDATOR_MODEL_MD100));
  const bool hasHopperModelMasks =
      prefs.isKey("hop_m1") || prefs.isKey("hop_m2") || prefs.isKey("hop_m3") || prefs.isKey("hop_m4");
  const bool hasBillValidatorModelMasks =
      prefs.isKey("bv_m1") || prefs.isKey("bv_m2") || prefs.isKey("bv_m3");
  out.hopperAlbericiDiscriminatorMask =
      sanitizeHopperModelAssignmentMask(prefs.getUChar("hop_m1", 0));
  out.hopperAlbericiHopperCdMask =
      sanitizeHopperModelAssignmentMask(prefs.getUChar("hop_m2", 0));
  out.hopperSuzoEvolutionMask =
      sanitizeHopperModelAssignmentMask(prefs.getUChar("hop_m3", 0));
  out.hopperAzkoyenDiscriminatorMask =
      sanitizeHopperModelAssignmentMask(prefs.getUChar("hop_m4", 0));
  out.billValidatorMd100Mask =
      sanitizeBillValidatorModelAssignmentMask(prefs.getUShort("bv_m1", 0));
  out.billValidatorSmartPayoutMask =
      sanitizeBillValidatorModelAssignmentMask(prefs.getUShort("bv_m2", 0));
  out.billValidatorIproMask =
      sanitizeBillValidatorModelAssignmentMask(prefs.getUShort("bv_m3", 0));
  out.coinAcceptorInEnabled = prefs.getBool("coin_acc_in", true);
  out.coinAcceptorFalconProfile =
      sanitizeCoinAcceptorFalconProfile(
          prefs.getUChar("coin_fal_pf", COIN_ACCEPTOR_FALCON_PROFILE_BLOCK0));
  out.coinInHopperMask =
      sanitizeHopperContributionMask(prefs.getUChar("coin_in_hm", kDefaultCoinInHopperMask));
  out.coinOutHopperMask =
      sanitizeHopperContributionMask(prefs.getUChar("coin_out_hm", kDefaultCoinOutHopperMask));
  for (uint8_t i = 0; i < kHopperAddressCount; i++) {
    char key[12] = {0};
    snprintf(key, sizeof(key), "hop_cv%u", (unsigned)(kHopperAddressMin + i));
    out.hopperCoinValueCents[i] =
        prefs.getUShort(key, kDefaultHopperCoinValueCents);
  }
  out.billInValidatorMask =
      sanitizeBillValidatorContributionMask(prefs.getUShort("bill_in_vm", kAllBillValidatorMask));
  out.billOutValidatorMask =
      sanitizeBillValidatorContributionMask(prefs.getUShort("bill_out_vm", kAllBillValidatorMask));
  if (!hasHopperModelMasks) {
    out.hopperAlbericiDiscriminatorMask =
        (out.hopperModel == HOPPER_MODEL_ALBERICI_DISCRIMINATOR) ? kAllHopperMask : 0;
    out.hopperAlbericiHopperCdMask =
        (out.hopperModel == HOPPER_MODEL_ALBERICI_HOPPERCD) ? kAllHopperMask : 0;
    out.hopperSuzoEvolutionMask =
        (out.hopperModel == HOPPER_MODEL_SUZO_EVOLUTION) ? kAllHopperMask : 0;
    out.hopperAzkoyenDiscriminatorMask =
        (out.hopperModel == HOPPER_MODEL_AZKOYEN_DISCRIMINATOR) ? kAllHopperMask : 0;
  }
  if (!hasBillValidatorModelMasks) {
    out.billValidatorMd100Mask =
        (out.billValidatorModel == BILL_VALIDATOR_MODEL_MD100) ? kAllBillValidatorMask : 0;
    out.billValidatorSmartPayoutMask =
        (out.billValidatorModel == BILL_VALIDATOR_MODEL_SMART_PAYOUT) ? kAllBillValidatorMask : 0;
    out.billValidatorIproMask =
        (out.billValidatorModel == BILL_VALIDATOR_MODEL_IPRO) ? kAllBillValidatorMask : 0;
  }
  out.valid = valid;

  prefs.end();
  return valid;
#else
  (void)out;
  return false;
#endif
}

bool AppSettingsStore::save(const AppSettings& in) {
#if defined(ARDUINO_ARCH_ESP32)
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return false;

  // Le stringhe vengono scritte sempre, mentre i campi numerici usano una
  // sanitizzazione preventiva per evitare di persistere valori non supportati.
  const bool keepWifi = in.saveWifiCredentials && in.wifiSsid[0] != '\0';
  prefs.putString("wifi_ssid", keepWifi ? in.wifiSsid : "");
  prefs.putString("wifi_pass", keepWifi ? in.wifiPass : "");
  prefs.putString("srv_url", in.serverUrl);
  prefs.putString("revt_url", in.remoteEventUrl);
  prefs.putString("db_host", in.dbHost);
  prefs.putString("db_name", in.dbName);
  prefs.putString("db_user", in.dbUser);
  prefs.putString("db_pass", in.dbPass);
  prefs.putString("loc_code", in.locationCode);
  prefs.putString("api_key", in.apiKey);
  prefs.putString("mqtt_host", in.mqttBrokerHost);
  prefs.putString("mqtt_user", in.mqttUsername);
  prefs.putString("mqtt_pass", in.mqttPassword);

  bool ok = true;
  ok = ok && prefs.putBool("wifi_save", keepWifi) > 0;
  ok = ok && prefs.putUChar("hop_model", sanitizeHopperModel(in.hopperModel)) > 0;
  ok = ok && prefs.putUChar("bv_model", sanitizeBillValidatorModel(in.billValidatorModel)) > 0;
  ok = ok &&
       prefs.putUChar("hop_m1", sanitizeHopperModelAssignmentMask(in.hopperAlbericiDiscriminatorMask)) > 0;
  ok = ok &&
       prefs.putUChar("hop_m2", sanitizeHopperModelAssignmentMask(in.hopperAlbericiHopperCdMask)) > 0;
  ok = ok &&
       prefs.putUChar("hop_m3", sanitizeHopperModelAssignmentMask(in.hopperSuzoEvolutionMask)) > 0;
  ok = ok &&
       prefs.putUChar("hop_m4", sanitizeHopperModelAssignmentMask(in.hopperAzkoyenDiscriminatorMask)) > 0;
  ok = ok &&
       prefs.putUShort("bv_m1", sanitizeBillValidatorModelAssignmentMask(in.billValidatorMd100Mask)) > 0;
  ok = ok &&
       prefs.putUShort("bv_m2", sanitizeBillValidatorModelAssignmentMask(in.billValidatorSmartPayoutMask)) > 0;
  ok = ok &&
       prefs.putUShort("bv_m3", sanitizeBillValidatorModelAssignmentMask(in.billValidatorIproMask)) > 0;
  ok = ok && prefs.putBool("coin_acc_in", in.coinAcceptorInEnabled) > 0;
  ok = ok &&
       prefs.putUChar("coin_fal_pf",
                      sanitizeCoinAcceptorFalconProfile(in.coinAcceptorFalconProfile)) > 0;
  ok = ok && prefs.putUChar("coin_in_hm", sanitizeHopperContributionMask(in.coinInHopperMask)) > 0;
  ok = ok && prefs.putUChar("coin_out_hm", sanitizeHopperContributionMask(in.coinOutHopperMask)) > 0;
  for (uint8_t i = 0; i < kHopperAddressCount; i++) {
    char key[12] = {0};
    snprintf(key, sizeof(key), "hop_cv%u", (unsigned)(kHopperAddressMin + i));
    ok = ok && prefs.putUShort(key, in.hopperCoinValueCents[i]) > 0;
  }
  ok = ok &&
       prefs.putUShort("bill_in_vm", sanitizeBillValidatorContributionMask(in.billInValidatorMask)) > 0;
  ok = ok &&
       prefs.putUShort("bill_out_vm", sanitizeBillValidatorContributionMask(in.billOutValidatorMask)) > 0;
  ok = ok && prefs.putUShort("db_port", in.dbPort) > 0;
  ok = ok && prefs.putUShort("mqtt_port", in.mqttBrokerPort) > 0;
  ok = ok && prefs.putBool("mqtt_en", in.mqttEnabled) > 0;
  ok = ok && prefs.putBool("valid", true) > 0;
  prefs.end();
  return ok;
#else
  (void)in;
  return false;
#endif
}

bool AppSettingsStore::clearAll() {
#if defined(ARDUINO_ARCH_ESP32)
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return false;
  const bool ok = prefs.clear();
  prefs.end();
  return ok;
#else
  return false;
#endif
}

bool AppSettingsStore::clearRemoteSettings() {
#if defined(ARDUINO_ARCH_ESP32)
  Preferences prefs;
  if (!prefs.begin(kNs, false)) return false;

  prefs.remove("revt_url");
  prefs.remove("loc_code");
  prefs.remove("api_key");
  prefs.end();
  return true;
#else
  return false;
#endif
}

} // namespace ccms
