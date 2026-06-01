<?php
/**
 * MqttPublisher
 *
 * Pubblica messaggi MQTT tramite l'HTTP API di EMQX Cloud (v5).
 * Usato dal server per inviare comandi ai dispositivi ESP32 in modalità push,
 * eliminando il polling HTTP dal lato device.
 *
 * Configurazione richiesta nel file config.php:
 *   define('EMQX_API_BASE_URL', 'https://xxxx.ala.eu-central-1.emqxsl.com:8083');
 *   define('EMQX_APP_ID',       'your-app-id');
 *   define('EMQX_APP_SECRET',   'your-app-secret');
 */
class MqttPublisher {
    private string $apiBaseUrl;
    private string $appId;
    private string $appSecret;

    public function __construct(string $apiBaseUrl, string $appId, string $appSecret) {
        $this->apiBaseUrl  = rtrim($apiBaseUrl, '/');
        $this->appId       = $appId;
        $this->appSecret   = $appSecret;
    }

    /**
     * Pubblica un comando verso il dispositivo identificato da $locationCode.
     * Usa retain=true: il dispositivo riceve il comando appena si (ri)connette.
     *
     * Payload JSON inviato al device:
     *   {"command":"SNAPSHOT","requestId":42,"payload":"..."}
     */
    public function publishCommand(
        string $locationCode,
        string $command,
        int    $requestId,
        string $payload = ''
    ): bool {
        $topic = "devices/{$locationCode}/commands";
        $body  = json_encode([
            'command'   => $command,
            'requestId' => $requestId,
            'payload'   => $payload,
        ]);
        return $this->publish($topic, $body, 1, true);
    }

    /**
     * Cancella il retained message del comando (da chiamare dopo che il device
     * ha risposto, per evitare che un nuovo avvio del device lo riespegua).
     */
    public function clearRetained(string $locationCode): bool {
        $topic = "devices/{$locationCode}/commands";
        return $this->publish($topic, '', 1, true);
    }

    /**
     * Primitiva di pubblicazione: POST su /api/v5/publish di EMQX.
     */
    private function publish(string $topic, string $payload, int $qos, bool $retain): bool {
        $url  = $this->apiBaseUrl . '/api/v5/publish';
        $data = json_encode([
            'topic'    => $topic,
            'payload'  => $payload,
            'qos'      => $qos,
            'retain'   => $retain,
            'encoding' => 'plain',
        ]);

        $credentials = base64_encode("{$this->appId}:{$this->appSecret}");

        $ctx = stream_context_create([
            'http' => [
                'method'  => 'POST',
                'header'  => implode("\r\n", [
                    'Content-Type: application/json',
                    "Authorization: Basic {$credentials}",
                    'Content-Length: ' . strlen($data),
                ]),
                'content'         => $data,
                'timeout'         => 5,
                'ignore_errors'   => true,
            ],
            'ssl' => [
                'verify_peer'       => true,
                'verify_peer_name'  => true,
            ],
        ]);

        $response = @file_get_contents($url, false, $ctx);
        if ($response === false) {
            return false;
        }

        $result = json_decode($response, true);
        // EMQX v5: risposta di successo ha "id" oppure array vuoto {}
        return isset($result['id']) || $result === [] || $result === null;
    }
}
