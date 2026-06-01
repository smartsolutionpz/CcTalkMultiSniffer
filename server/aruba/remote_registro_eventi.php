<?php
declare(strict_types=1);

function gr_require_local_or_fallback(string $localName, ?string $fallbackRelative = null): void
{
    $candidates = [__DIR__ . '/' . $localName];
    if ($fallbackRelative !== null && $fallbackRelative !== '') {
        $candidates[] = dirname(__DIR__, 2) . '/' . ltrim($fallbackRelative, '/');
    }

    foreach ($candidates as $candidate) {
        if (is_file($candidate)) {
            require_once $candidate;
            return;
        }
    }

    throw new RuntimeException('Dipendenza PHP mancante: ' . $localName);
}

gr_require_local_or_fallback('bootstrap.php', 'web/api-5/bootstrap.php');
gr_method_or_405(['GET', 'POST']);

gr_require_local_or_fallback('config.php', 'web/api-5/config.php');
gr_require_local_or_fallback('auth.php', 'web/api-5/auth.php');
require_once __DIR__ . '/RemoteRegistroEventiApi.php';
require_once __DIR__ . '/MqttPublisher.php';

function buildMqttPublisher(): ?MqttPublisher {
    if (!defined('EMQX_API_BASE_URL') || !defined('EMQX_APP_ID') || !defined('EMQX_APP_SECRET')) {
        return null;
    }
    return new MqttPublisher(EMQX_API_BASE_URL, EMQX_APP_ID, EMQX_APP_SECRET);
}

try {
    $pdo = getDatabaseConnection();
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $pdo->setAttribute(PDO::ATTR_DEFAULT_FETCH_MODE, PDO::FETCH_ASSOC);
    $pdo->setAttribute(PDO::ATTR_EMULATE_PREPARES, false);

    $tableName = 'RemoteRegistroEventiUbicazione';
    if (function_exists('gr_env')) {
        $envTable = gr_env('DB_TABLE_REMOTE_REGISTRO_EVENTI');
        if (is_string($envTable) && trim($envTable) !== '') {
            $tableName = trim($envTable);
        }
    }

    $api = new RemoteRegistroEventiApi($pdo, null, $tableName, 15);

    if (($_SERVER['REQUEST_METHOD'] ?? 'GET') === 'POST') {
        $rawBody = file_get_contents('php://input');
        $payload = json_decode($rawBody ?: '{}', true);
        if (!is_array($payload)) {
            gr_bad_request('JSON non valido');
        }

        $mode = strtolower(trim((string) ($payload['mode'] ?? 'legacy')));
        if ($mode === 'request') {
            $result = $api->upsertRequest($payload);
            // Pubblica il comando via MQTT (retained) se EMQX e configurato.
            // Il device riceve il push istantaneamente invece di attendere il polling.
            if (($result['id'] ?? null) !== null) {
                $mqtt = buildMqttPublisher();
                if ($mqtt !== null) {
                    $mqtt->publishCommand(
                        (string) ($payload['CodiceUbicazione'] ?? ''),
                        (string) ($payload['Descrizione'] ?? 'SNAPSHOT'),
                        (int)    $result['id'],
                        (string) ($payload['Note1'] ?? '')
                    );
                }
            }
        } elseif ($mode === 'response') {
            $result = $api->updateRequestResponse($payload);
            // Cancella il retained message ora che il device ha risposto.
            $mqtt = buildMqttPublisher();
            if ($mqtt !== null) {
                $mqtt->clearRetained((string) ($payload['CodiceUbicazione'] ?? ''));
            }
        } elseif ($mode === 'mqtt_response') {
            // Endpoint chiamato dall'EMQX Rule Engine quando il device pubblica
            // su devices/{locationCode}/responses.
            // Payload atteso: {"requestId":42,"status":"served","message":"...","locationCode":"X"}
            $responsePayload = [
                'Id'               => (int) ($payload['requestId'] ?? 0),
                'CodiceUbicazione' => (string) ($payload['locationCode'] ?? ''),
                'Note2'            => (string) ($payload['status'] ?? 'error'),
                'Note4'            => (string) ($payload['message'] ?? ''),
            ];
            $result = $api->updateRequestResponse($responsePayload);
            $mqtt = buildMqttPublisher();
            if ($mqtt !== null) {
                $mqtt->clearRetained((string) ($payload['locationCode'] ?? ''));
            }
        } else {
            $result = $api->insertEvent($payload);
        }

        echo json_encode([
            'success' => true,
            'message' => $result['message'] ?? 'Operazione completata',
            'id' => $result['id'] ?? null,
            'created' => $result['created'] ?? null,
        ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        exit();
    }

    $pendingOnly = isset($_GET['pending']) && $_GET['pending'] !== '0';
    if ($pendingOnly) {
        $result = $api->readPendingRequest(
            (string) ($_GET['codiceUbicazione'] ?? ''),
            null
        );
        $item = is_array($result['item'] ?? null) ? $result['item'] : null;

        echo json_encode([
            'success' => true,
            'found' => $result['found'] ?? false,
            'message' => $result['message'] ?? '',
            'id' => $item['_id'] ?? null,
            'command' => $item['Descrizione'] ?? '',
            'requestPayload' => $item['Note1'] ?? '',
            'state' => $item['Note2'] ?? '',
            'requestTime' => $item['Data'] ?? '',
            'item' => $item,
        ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        exit();
    }

    $latestOnly = isset($_GET['latest']) && $_GET['latest'] !== '0';
    if ($latestOnly) {
        $result = $api->readLatestEventStatus(
            (string) ($_GET['codiceUbicazione'] ?? ''),
            null
        );

        echo json_encode([
            'success' => true,
            'found' => $result['found'] ?? false,
            'shouldInsert' => $result['shouldInsert'] ?? false,
            'reason' => $result['reason'] ?? null,
            'message' => $result['message'] ?? '',
            'ageSeconds' => $result['ageSeconds'] ?? null,
            'valore6IsNull' => $result['valore6IsNull'] ?? null,
            'item' => $result['item'] ?? null,
        ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        exit();
    }

    $result = $api->readEvents(
        $_GET['codiceUbicazione'] ?? null,
        isset($_GET['limit']) ? (int) $_GET['limit'] : 50,
        isset($_GET['afterId']) ? (int) $_GET['afterId'] : null,
        null
    );

    echo json_encode([
        'success' => true,
        'count' => $result['count'] ?? 0,
        'items' => $result['items'] ?? [],
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
} catch (InvalidArgumentException $e) {
    http_response_code(400);
    echo json_encode([
        'success' => false,
        'message' => $e->getMessage(),
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
} catch (Throwable $e) {
    http_response_code(500);
    echo json_encode([
        'success' => false,
        'message' => $e->getMessage(),
    ], JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
}
