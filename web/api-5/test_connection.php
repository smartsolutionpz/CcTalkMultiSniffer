<?php
// Endpoint di test connessione sicuro: richiede API key e usa SOLO credenziali lato server
header('Content-Type: application/json; charset=utf-8');

require_once __DIR__ . '/auth.php';      // impone API key configurata lato server
require_once __DIR__ . '/config.php';    // carica credenziali DB da .env

try {
    $conn = getMysqliConnection();
    $conn->close();
    echo json_encode(["success" => true, "message" => "Connessione riuscita!!!"]);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode(["success" => false, "message" => "Errore di connessione: " . $e->getMessage()]);
}
?>
