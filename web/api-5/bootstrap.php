<?php
// Bootstrap comune per endpoint JSON protetti

// CORS e tipo di contenuto
header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Headers: Content-Type, X-API-Key');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');

// Gestione preflight
if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit();
}

if (!function_exists('gr_method_or_405')) {
    /**
     * Enforce allowed HTTP methods, otherwise return 405.
     * @param array $allowed e.g. ['POST']
     */
    function gr_method_or_405(array $allowed): void
    {
        $method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
        if (!in_array($method, $allowed, true)) {
            http_response_code(405);
            echo json_encode([
                'success' => false,
                'message' => 'Metodo non supportato.'
            ]);
            exit();
        }
    }
}

if (!function_exists('gr_bad_request')) {
    /**
     * Send a 400 JSON response with message and exit.
     */
    function gr_bad_request(string $message): void
    {
        http_response_code(400);
        echo json_encode(['success' => false, 'message' => $message]);
        exit();
    }
}

?>
