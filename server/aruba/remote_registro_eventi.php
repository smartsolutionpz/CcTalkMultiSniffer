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
        } elseif ($mode === 'response') {
            $result = $api->updateRequestResponse($payload);
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
