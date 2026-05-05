<?php
// Autenticazione centralizzata per gli endpoint API.
// Verifica l'header X-API-Key prima di eseguire qualsiasi logica applicativa.

header('Content-Type: application/json; charset=utf-8');

// Logging minimale per diagnosi configurazione (solo in caso di problemi)
if (!function_exists('gr_log_auth_issue')) {
    function gr_log_auth_issue(string $message): void
    {
        $logFile = __DIR__ . '/debug_log.txt';
        @file_put_contents($logFile, '[' . date('c') . "] AUTH: " . $message . "\n", FILE_APPEND);
    }
}

$expectedApiKey = '';

// 1) Variabile d'ambiente
$envVal = getenv('API_KEY');
if ($envVal !== false && $envVal !== '') {
    $expectedApiKey = (string)$envVal;
    // gr_log_auth_issue('API_KEY trovata in env');
}

// 2) File segreto nella stessa cartella dell'API
if ($expectedApiKey === '') {
    $secretPath = __DIR__ . '/secret_api_key.php';
    if (is_readable($secretPath)) {
        require_once $secretPath; // deve definire API_KEY o $API_KEY
        if (defined('API_KEY')) {
            $expectedApiKey = (string)API_KEY;
        } elseif (isset($API_KEY)) {
            $expectedApiKey = (string)$API_KEY;
        }
    }
}

// 3) File segreto un livello sopra (deploy alternativi)
if ($expectedApiKey === '') {
    $secretPathUp = dirname(__DIR__) . '/secret_api_key.php';
    if (is_readable($secretPathUp)) {
        require_once $secretPathUp;
        if (defined('API_KEY')) {
            $expectedApiKey = (string)API_KEY;
        } elseif (isset($API_KEY)) {
            $expectedApiKey = (string)$API_KEY;
        }
    }
}

// 4) .env (sia in root gamerent_test sia in api/) usando i loader di config.php
if ($expectedApiKey === '') {
    $configPath = __DIR__ . '/config.php';
    if (is_readable($configPath)) {
        require_once $configPath;
        if (function_exists('gr_load_env') && function_exists('gr_env')) {
            $rootDir = dirname(__DIR__);
            @gr_load_env($rootDir . '/.env');     // es: /gamerent_test/.env
            @gr_load_env(__DIR__ . '/.env');      // es: /gamerent_test/api/.env
            $envApiKey = @gr_env('API_KEY');
            if ($envApiKey) {
                $expectedApiKey = (string)$envApiKey;
            }
        }
    }
}

if ($expectedApiKey === '') {
    gr_log_auth_issue('API_KEY non trovata. Cercato in: env, api/secret_api_key.php, ../secret_api_key.php, .env');
    http_response_code(500);
    echo json_encode([
        'success' => false,
        'message' => 'API Key non configurata sul server. Impostare env API_KEY o secret_api_key.php.'
    ]);
    exit();
}

// Valida X-API-Key del client
$providedKey = '';
if (isset($_SERVER['HTTP_X_API_KEY'])) {
    $providedKey = trim((string)$_SERVER['HTTP_X_API_KEY']);
} elseif (isset($_GET['api_key'])) {
    // Fallback per test manuali
    $providedKey = trim((string)$_GET['api_key']);
}

if ($providedKey === '' || !hash_equals($expectedApiKey, $providedKey)) {
    http_response_code(401);
    echo json_encode([
        'success' => false,
        'message' => 'Unauthorized'
    ]);
    exit();
}

return; // autenticazione ok
?>

