// Scopo del file:
// raccoglie i parametri statici di compilazione del firmware:
// pin, timing, default Wi-Fi e soglie runtime.
#ifndef CCTALK_MULTI_SNIFFER_CONFIG_H
#define CCTALK_MULTI_SNIFFER_CONFIG_H

#include <stdint.h>

namespace appconfig {

// Parametri statici del firmware.
// Rappresentano i default di compilazione e vengono sovrascritti, quando serve,
// dalle impostazioni utente persistite.

// Imposta qui credenziali WiFi (oppure sovrascrivi da sketch via WifiService::setCredentials()).
static const char* const WIFI_SSID = "";
static const char* const WIFI_PASS = "";
static const char* const WIFI_HOSTNAME = "cctalk-sniffer";
static const char* const WIFI_FALLBACK_AP_SSID = "CcTalkSniffer-AP";
static const char* const WIFI_FALLBACK_AP_PASS = ""; // vuota = AP aperto
static const uint8_t WIFI_AP_CHANNEL = 1;
static const int8_t WIFI_TX_POWER = 78; // unita 0.25 dBm: 78 = 19.5 dBm
static const bool WIFI_FORCE_LEGACY_24G = true;
static const uint32_t WIFI_PERFORMANCE_REAPPLY_INTERVAL_MS = 5000;

// Timeout di una singola connessione e intervallo tra retry (ms).
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t WIFI_RETRY_INTERVAL_MS = 30000;
static const char* const TIME_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
static const char* const NTP_SERVER_1 = "pool.ntp.org";
static const char* const NTP_SERVER_2 = "time.cloudflare.com";
static const char* const NTP_SERVER_3 = "time.google.com";
static const uint32_t NTP_RESYNC_INTERVAL_MS = 21600000UL;

// Pin I/O.
// Verifica sempre il pinout della tua scheda e adatta questi valori.
#if CONFIG_IDF_TARGET_ESP32C6
// Seeed XIAO ESP32C6:
// D7/GPIO17 = RX sniff ccTalk, D4/GPIO22 = SDA, D5/GPIO23 = SCL.
// RF switch FM8625H: GPIO3=LOW abilita switch, GPIO14=HIGH seleziona antenna esterna.
static const bool WIFI_USE_EXTERNAL_ANTENNA = true; // true = antenna esterna IPEX
static const int CCTALK_UART_RX_PIN = 17;  // RX sniff ccTalk
static const int CCTALK_UART_TX_PIN = -1;  // RX-only
static const int FRAM_I2C_SDA_PIN = 22;
static const int FRAM_I2C_SCL_PIN = 23;
static const int LEVEL_SHIFTER_OE_PIN = -1;
static const int PROG_MODE_BUTTON_PIN = -1; // spostato su PCF8574 P3
static const int CCTALK_STATUS_LED_PIN = -1;
static const int WIFI_STATUS_LED_PIN = -1;
// PCF8574MT GPIO expander (stesso bus I2C di FRAM, A0=GND, A1=VCC, A2=GND -> 0x21)
static const bool    GPIO_EXPANDER_ENABLED         = true;
static const uint8_t GPIO_EXPANDER_I2C_ADDR        = 0x21;
static const uint8_t GPIO_EXPANDER_PIN_BUTTON_PROG = 3;  // P3: pulsante PROG
static const uint8_t GPIO_EXPANDER_PIN_LED_WIFI    = 6;  // P6: LED connessione WiFi
static const uint8_t GPIO_EXPANDER_PIN_LED_CCTALK  = 7;  // P7: LED stato CCTalk
#elif CONFIG_IDF_TARGET_ESP32C3
// NOTE: i pin del modulo ESP32-C3 SuperMini variano per clone/versione.
static const int CCTALK_UART_RX_PIN = 21;  // RX sniff ccTalk
static const int CCTALK_UART_TX_PIN = -1;  // RX-only
static const int FRAM_I2C_SDA_PIN = 8;
static const int FRAM_I2C_SCL_PIN = 9;
static const int LEVEL_SHIFTER_OE_PIN = 0;
static const int PROG_MODE_BUTTON_PIN = 4;
// -1 disabilita i LED: su alcune ESP32-C3 clone i pin esposti variano e
// usare GPIO non sicuri puo impedire l'avvio del WiFi.
static const int CCTALK_STATUS_LED_PIN = 7;  //7
static const int WIFI_STATUS_LED_PIN = 6;    //6
static const bool    GPIO_EXPANDER_ENABLED         = false;
static const uint8_t GPIO_EXPANDER_I2C_ADDR        = 0x20;
static const uint8_t GPIO_EXPANDER_PIN_BUTTON_PROG = 0;
static const uint8_t GPIO_EXPANDER_PIN_LED_WIFI    = 0;
static const uint8_t GPIO_EXPANDER_PIN_LED_CCTALK  = 0;
#else
static const int CCTALK_UART_RX_PIN = 16;
static const int CCTALK_UART_TX_PIN = -1;  // RX-only
static const int FRAM_I2C_SDA_PIN = 21;
static const int FRAM_I2C_SCL_PIN = 22;
static const int LEVEL_SHIFTER_OE_PIN = -1;
static const int PROG_MODE_BUTTON_PIN = 4;
static const int CCTALK_STATUS_LED_PIN = -1;
static const int WIFI_STATUS_LED_PIN = -1;
static const bool    GPIO_EXPANDER_ENABLED         = false;
static const uint8_t GPIO_EXPANDER_I2C_ADDR        = 0x20;
static const uint8_t GPIO_EXPANDER_PIN_BUTTON_PROG = 0;
static const uint8_t GPIO_EXPANDER_PIN_LED_WIFI    = 0;
static const uint8_t GPIO_EXPANDER_PIN_LED_CCTALK  = 0;
#endif
static const uint32_t PROG_MODE_DEBOUNCE_MS = 30;
static const bool LEVEL_SHIFTER_OE_ACTIVE_HIGH = true;
static const bool STATUS_LED_ACTIVE_LOW = false;
static const uint32_t CCTALK_STATUS_LED_HOLD_MS = 3000;

// FRAM I2C (Adafruit_FRAM_I2C)
static const uint8_t FRAM_I2C_ADDR = 0x50;
static const uint32_t FRAM_WRITE_MIN_INTERVAL_MS = 250;
static const uint32_t FRAM_CONGRUENCE_CHECK_INTERVAL_MS = 60000;

// Runtime scheduler (Fase 1: sniffer priorita alta, servizi non bloccanti).
// Questi valori bilanciano latenza di sniffing e tempo disponibile ai servizi.
static const uint32_t SNIFFER_LOOP_BUDGET_US = 2500;
static const uint32_t SNIFFER_COOP_YIELD_INTERVAL_US = 500;
static const uint32_t NET_SERVICE_LOOP_INTERVAL_MS = 2;
static const uint32_t PERSIST_LOOP_INTERVAL_MS = 20;
static const uint32_t TASK_STACK_BYTES_SNIFFER = 8192;
static const uint32_t TASK_STACK_BYTES_NET = 16384;
static const uint8_t TASK_PRIORITY_SNIFFER = 1;
static const uint8_t TASK_PRIORITY_NET = 1;

// Fase 3: cloud publish + mesh locale.
static const bool CLOUD_PUBLISH_ENABLED = false;
static const uint32_t CLOUD_PUBLISH_INTERVAL_MS = 5000;
static const uint16_t CLOUD_HTTP_TIMEOUT_MS = 200;
static const bool REMOTE_DB_AUTO_POLL_ENABLED = true;
static const uint16_t REMOTE_DB_HTTP_TIMEOUT_MS = 500;
static const uint32_t REMOTE_DB_POLL_INTERVAL_MS = 30000;
static const uint32_t REMOTE_DB_RETRY_INTERVAL_MS = 60000;
static const uint32_t REMOTE_DB_WIFI_SETTLE_MS = 3000;
static const uint32_t REMOTE_INTERNET_CHECK_INTERVAL_MS = 30000;
static const uint32_t REMOTE_INTERNET_RETRY_INTERVAL_MS = 15000;
static const uint32_t REMOTE_DB_FAILURE_BACKOFF_BASE_MS = 60000;
static const uint32_t REMOTE_DB_FAILURE_BACKOFF_MAX_MS = 300000;
static const uint32_t MQTT_RECONNECT_INTERVAL_MS = 15000;
static const uint32_t MESH_START_DELAY_MS = 3000;
static const uint32_t MESH_RETRY_INTERVAL_MS = 5000;
static const uint32_t MESH_HEARTBEAT_INTERVAL_MS = 1000;
static const bool MESH_ENABLED = false;

} // namespace appconfig

#endif // CCTALK_MULTI_SNIFFER_CONFIG_H
