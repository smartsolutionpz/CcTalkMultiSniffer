<?php
declare(strict_types=1);

// --- EMQX Cloud (opzionale, richiesto solo se usi MQTT) ---
// Ottieni questi valori dal pannello EMQX Cloud → il tuo deployment → Overview.
// Se non definiti, il sistema funziona in modalita HTTP polling (retrocompatibile).
define('EMQX_API_BASE_URL', 'https://xxxx.ala.eu-central-1.emqxsl.com:8083'); // URL REST API EMQX
define('EMQX_APP_ID',       'inserisci-app-id-emqx');      // Dashboard → API Keys → App ID
define('EMQX_APP_SECRET',   'inserisci-app-secret-emqx');  // Dashboard → API Keys → App Secret
// ----------------------------------------------------------

return [
    'db_host' => '31.11.39.97',
    'db_port' => 3306,
    'db_name' => 'Sql1669074_4',
    'db_user' => 'inserisci_utente_mysql',
    'db_pass' => 'inserisci_password_mysql',
    'table_name' => 'RemoteRegistroEventiUbicazione',
    'api_key' => 'imposta-una-api-key-lunga-e-casuale',
];
