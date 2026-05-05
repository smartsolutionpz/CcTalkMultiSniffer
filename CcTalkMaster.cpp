// Scopo del file:
// implementa la mappa descrittiva degli header del master ccTalk.
#include "CcTalkMaster.h"

const __FlashStringHelper* CcTalkMaster::headerDesc(uint8_t hdr) {
  // Mappa ragionata degli header piu comuni. Quando un decoder non ha una
  // descrizione specializzata, questo catalogo evita output "grezzo" poco utile.
  switch (hdr) {
    case 0xFF: return F("identificazione estesa (vendor/equipment id)");
    case 0xFE: return F("poll semplice");
    case 0xFD: return F("address poll (MDCES)");
    case 0xFC: return F("address clash (MDCES)");
    case 0xFB: return F("address change (MDCES)");
    case 0xFA: return F("address random (MDCES)");
    case 0xF6: return F("ID produttore");
    case 0xF5: return F("ID categoria");
    case 0xF4: return F("product code");
    case 0xF2: return F("numero di serie");
    case 0xF1: return F("revisione software");
    case 0xC0: return F("build code");
    case 0xC5: return F("calcolo checksum ROM");
    case 0xD9: return F("stato sensori hi/low payout");
    case 0xD2: return F("modifica percorso sorter");
    case 0xD1: return F("richiesta percorso sorter");
    case 0xA4: return F("abilita/disabilita hopper");
    case 0xA3: return F("test hopper");
    case 0x86: return F("eroga valore hopper");
    case 0x85: return F("richiesta polling hopper");
    case 0x84: return F("stop emergenza payout");
    case 0x83: return F("richiesta valore moneta hopper");
    case 0x82: return F("richiesta conteggio erogato indicizzato");
    case 0xE7: return F("modifica inhibit status");
    case 0xE6: return F("richiesta inhibit status");
    case 0xE4: return F("modifica master inhibit");
    case 0xE3: return F("richiesta master inhibit");
    case 0x9F: return F("lettura eventi banconote bufferizzati");
    case 0x9E: return F("modifica bill ID");
    case 0x9D: return F("richiesta bill ID");
    case 0x9C: return F("richiesta scaling factor paese");
    case 0x9B: return F("richiesta posizione banconota");
    case 0x9A: return F("instradamento banconota escrow");
    case 0x99: return F("modifica modo operativo bill");
    case 0x98: return F("richiesta modo operativo bill");
    case 0x97: return F("test lampade");
    case 0x96: return F("richiesta contatore accettazioni");
    case 0x95: return F("richiesta contatore errori");
    case 0x94: return F("lettura tensioni opto");
    case 0x93: return F("ciclo stacker");
    case 0x92: return F("controllo motori bidirezionali");
    case 0x91: return F("richiesta revisione valuta");
    case 0x90: return F("upload bill tables");
    case 0x8F: return F("inizio upgrade tabelle banconote");
    case 0x8E: return F("fine upgrade tabelle banconote");
    case 0x8D: return F("richiesta capacita upgrade firmware");
    case 0x8C: return F("upload firmware");
    case 0x8B: return F("inizio upgrade firmware");
    case 0x8A: return F("fine upgrade firmware");
    case 0x89: return F("switch codice cifratura");
    case 0x88: return F("salvataggio codice cifratura");
    case 0x87: return F("imposta limite accettazione");
    case 0x81: return F("lettura barcode");
    case 0x6B: return F("operate escrow");
    case 0x6A: return F("richiesta stato escrow");
    case 0x04: return F("comms revision");
    case 0x01: return F("reset dispositivo");
    default:   return F("comando non mappato");
  }
}
