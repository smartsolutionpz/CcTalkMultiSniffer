// Scopo del file:
// dichiara `AppSettingsStore`, la classe incaricata di salvare e caricare
// le impostazioni utente persistenti da NVS/Preferences.
#ifndef CCTALK_MULTI_SNIFFER_APP_SETTINGS_STORE_H
#define CCTALK_MULTI_SNIFFER_APP_SETTINGS_STORE_H

#include "AppSettings.h"

namespace ccms {

// Facade minima sopra Preferences/NVS.
// Isola dal resto del progetto i dettagli di persistenza della configurazione
// utente, mantenendo l'interfaccia semplice e stabile.
class AppSettingsStore {
public:
  // Verifica che lo store sia raggiungibile e che il namespace sia apribile.
  bool begin();
  // Carica le impostazioni persistenti in `out`.
  // Restituisce true solo se esiste una configurazione marcata come valida.
  bool load(AppSettings& out);
  // Salva una nuova configurazione persistente.
  bool save(const AppSettings& in);
  // Cancella l'intero namespace configurazione.
  bool clearAll();
  // Cancella solo la configurazione del registro remoto.
  bool clearRemoteSettings();
};

} // namespace ccms

#endif // CCTALK_MULTI_SNIFFER_APP_SETTINGS_STORE_H
