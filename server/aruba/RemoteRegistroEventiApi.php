<?php
declare(strict_types=1);

final class RemoteRegistroEventiApi
{
    private PDO $pdo;
    private ?string $apiKey;
    private string $tableName;
    private int $retentionDays;

    public function __construct(
        PDO $pdo,
        ?string $apiKey = null,
        string $tableName = 'RemoteRegistroEventiUbicazione',
        int $retentionDays = 15
    ) {
        $this->pdo = $pdo;
        $this->apiKey = ($apiKey !== null && $apiKey !== '') ? $apiKey : null;
        $this->tableName = $tableName;
        $this->retentionDays = max(1, $retentionDays);
    }

    public function insertEvent(array $payload): array
    {
        $this->assertAuthorized($payload['apiKey'] ?? null);
        $this->purgeExpiredEvents();

        $codiceUbicazione = $this->sanitizeString($payload['codiceUbicazione'] ?? null, 14, true);
        $operazione = $this->sanitizeString($payload['operazione'] ?? null, 65535, false);
        $descrizione = $this->sanitizeString($payload['descrizione'] ?? null, 65535, false);
        $note1 = $this->sanitizeString($payload['note1'] ?? null, 250, false);
        $note2 = $this->sanitizeString($payload['note2'] ?? null, 250, false);
        $note3 = $this->sanitizeString($payload['note3'] ?? null, 250, false);
        $note4 = $this->sanitizeString($payload['note4'] ?? null, 250, false);
        $dispositivo = $this->sanitizeString($payload['dispositivo'] ?? null, 65535, false);

        $sql = sprintf(
            'INSERT INTO `%s`
            (`Data`, `CodiceUbicazione`, `Operazione`, `Descrizione`, `Valore1`, `Valore2`, `Valore3`, `Valore4`, `Valore5`, `Valore6`, `Note1`, `Note2`, `Note3`, `Note4`, `Dispositivo`)
            VALUES (NOW(), :CodiceUbicazione, :Operazione, :Descrizione, :Valore1, :Valore2, :Valore3, :Valore4, :Valore5, :Valore6, :Note1, :Note2, :Note3, :Note4, :Dispositivo)',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
        $this->bindNullableString($stmt, ':Operazione', $operazione);
        $this->bindNullableString($stmt, ':Descrizione', $descrizione);
        $this->bindNullableInt($stmt, ':Valore1', $payload['valore1'] ?? null);
        $this->bindNullableInt($stmt, ':Valore2', $payload['valore2'] ?? null);
        $this->bindNullableInt($stmt, ':Valore3', $payload['valore3'] ?? null);
        $this->bindNullableInt($stmt, ':Valore4', $payload['valore4'] ?? null);
        $this->bindNullableInt($stmt, ':Valore5', $payload['valore5'] ?? null);
        $this->bindNullableInt($stmt, ':Valore6', $payload['valore6'] ?? null);
        $this->bindNullableString($stmt, ':Note1', $note1);
        $this->bindNullableString($stmt, ':Note2', $note2);
        $this->bindNullableString($stmt, ':Note3', $note3);
        $this->bindNullableString($stmt, ':Note4', $note4);
        $this->bindNullableString($stmt, ':Dispositivo', $dispositivo);
        $stmt->execute();

        return [
            'ok' => true,
            'id' => (int) $this->pdo->lastInsertId(),
            'message' => 'Evento registrato',
        ];
    }

    public function upsertRequest(array $payload): array
    {
        $this->assertAuthorized($payload['apiKey'] ?? null);
        $this->purgeExpiredEvents();

        $codiceUbicazione = $this->sanitizeString($payload['codiceUbicazione'] ?? null, 14, true);
        $command = $this->sanitizeString($payload['command'] ?? $payload['descrizione'] ?? 'SNAPSHOT', 65535, false);
        if ($command === null) {
            $command = 'SNAPSHOT';
        }
        $requestPayload = $this->sanitizeString($payload['requestPayload'] ?? $payload['note1'] ?? null, 250, false);
        $slotId = $this->findLatestRequestSlotId($codiceUbicazione);

        if ($slotId === null) {
            $sql = sprintf(
                'INSERT INTO `%s`
                (`Data`, `CodiceUbicazione`, `Operazione`, `Descrizione`, `Valore1`, `Valore2`, `Valore3`, `Valore4`, `Valore5`, `Valore6`, `Note1`, `Note2`, `Note3`, `Note4`, `Dispositivo`)
                VALUES (NOW(), :CodiceUbicazione, :Operazione, :Descrizione, NULL, NULL, NULL, NULL, NULL, NULL, :Note1, :Note2, NULL, NULL, NULL)',
                $this->tableName
            );
            $stmt = $this->pdo->prepare($sql);
            $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
            $stmt->bindValue(':Operazione', 'MASTER_REQUEST', PDO::PARAM_STR);
            $stmt->bindValue(':Descrizione', $command, PDO::PARAM_STR);
            $this->bindNullableString($stmt, ':Note1', $requestPayload);
            $stmt->bindValue(':Note2', 'pending', PDO::PARAM_STR);
            $stmt->execute();

            return [
                'ok' => true,
                'id' => (int) $this->pdo->lastInsertId(),
                'message' => 'Richiesta master creata',
                'created' => true,
            ];
        }

        $sql = sprintf(
            'UPDATE `%s`
             SET `Data` = NOW(),
                 `Operazione` = :Operazione,
                 `Descrizione` = :Descrizione,
                 `Valore1` = NULL,
                 `Valore2` = NULL,
                 `Valore3` = NULL,
                 `Valore4` = NULL,
                 `Valore5` = NULL,
                 `Valore6` = NULL,
                 `Note1` = :Note1,
                 `Note2` = :Note2,
                 `Note3` = NULL,
                 `Note4` = NULL,
                 `Dispositivo` = NULL
             WHERE `_id` = :Id',
            $this->tableName
        );
        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':Id', $slotId, PDO::PARAM_INT);
        $stmt->bindValue(':Operazione', 'MASTER_REQUEST', PDO::PARAM_STR);
        $stmt->bindValue(':Descrizione', $command, PDO::PARAM_STR);
        $this->bindNullableString($stmt, ':Note1', $requestPayload);
        $stmt->bindValue(':Note2', 'pending', PDO::PARAM_STR);
        $stmt->execute();

        return [
            'ok' => true,
            'id' => $slotId,
            'message' => 'Richiesta master aggiornata',
            'created' => false,
        ];
    }

    public function readPendingRequest(string $codiceUbicazione, ?string $apiKey = null): array
    {
        $this->assertAuthorized($apiKey);
        $this->purgeExpiredEvents();

        $codiceUbicazione = $this->sanitizeString($codiceUbicazione, 14, true);

        $sql = sprintf(
            'SELECT `_id`, `Data`, `CodiceUbicazione`, `Operazione`, `Descrizione`, `Valore1`, `Valore2`, `Valore3`, `Valore4`, `Valore5`, `Valore6`, `Note1`, `Note2`, `Note3`, `Note4`, `Dispositivo`
             FROM `%s`
             WHERE `CodiceUbicazione` = :CodiceUbicazione
               AND `Operazione` = :Operazione
               AND `Note2` = :PendingState
             ORDER BY `_id` DESC
             LIMIT 1',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
        $stmt->bindValue(':Operazione', 'MASTER_REQUEST', PDO::PARAM_STR);
        $stmt->bindValue(':PendingState', 'pending', PDO::PARAM_STR);
        $stmt->execute();
        $item = $stmt->fetch(PDO::FETCH_ASSOC);

        if (!is_array($item)) {
            return [
                'ok' => true,
                'found' => false,
                'message' => 'Nessuna richiesta pendente per il codice ubicazione richiesto.',
                'item' => null,
            ];
        }

        return [
            'ok' => true,
            'found' => true,
            'message' => 'Richiesta pendente trovata.',
            'item' => $item,
        ];
    }

    public function updateRequestResponse(array $payload): array
    {
        $this->assertAuthorized($payload['apiKey'] ?? null);
        $this->purgeExpiredEvents();

        $requestId = isset($payload['id']) ? (int) $payload['id'] : 0;
        if ($requestId <= 0) {
            throw new InvalidArgumentException('ID richiesta non valido');
        }

        $codiceUbicazione = $this->sanitizeString($payload['codiceUbicazione'] ?? null, 14, true);
        $status = strtolower(trim((string) ($payload['status'] ?? 'served')));
        if ($status !== 'served' && $status !== 'error') {
            $status = 'served';
        }

        $responseMessage = $this->sanitizeString($payload['responseMessage'] ?? $payload['note4'] ?? null, 250, false);
        $responseNote1 = $this->sanitizeString($payload['note1'] ?? null, 250, false);
        $dispositivo = $this->sanitizeString($payload['dispositivo'] ?? null, 65535, false);

        $sql = sprintf(
            'UPDATE `%s`
             SET `Note1` = :Note1,
                 `Valore1` = :Valore1,
                 `Valore2` = :Valore2,
                 `Valore3` = :Valore3,
                 `Valore4` = :Valore4,
                 `Valore5` = :Valore5,
                 `Valore6` = :Valore6,
                 `Note2` = :Note2,
                 `Note3` = DATE_FORMAT(NOW(), \'%%Y-%%m-%%d %%H:%%i:%%s\'),
                 `Note4` = :Note4,
                 `Dispositivo` = :Dispositivo
             WHERE `_id` = :Id
               AND `CodiceUbicazione` = :CodiceUbicazione
               AND `Operazione` = :Operazione',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':Id', $requestId, PDO::PARAM_INT);
        $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
        $stmt->bindValue(':Operazione', 'MASTER_REQUEST', PDO::PARAM_STR);
        $this->bindNullableString($stmt, ':Note1', $responseNote1);
        $this->bindNullableInt($stmt, ':Valore1', $payload['valore1'] ?? null);
        $this->bindNullableInt($stmt, ':Valore2', $payload['valore2'] ?? null);
        $this->bindNullableInt($stmt, ':Valore3', $payload['valore3'] ?? null);
        $this->bindNullableInt($stmt, ':Valore4', $payload['valore4'] ?? null);
        $this->bindNullableInt($stmt, ':Valore5', $payload['valore5'] ?? null);
        $this->bindNullableInt($stmt, ':Valore6', $payload['valore6'] ?? null);
        $stmt->bindValue(':Note2', $status, PDO::PARAM_STR);
        $this->bindNullableString($stmt, ':Note4', $responseMessage);
        $this->bindNullableString($stmt, ':Dispositivo', $dispositivo);
        $stmt->execute();

        if ($stmt->rowCount() <= 0) {
            throw new RuntimeException('Richiesta non trovata o non aggiornabile');
        }

        return [
            'ok' => true,
            'id' => $requestId,
            'message' => 'Risposta device registrata',
        ];
    }

    public function readEvents(?string $codiceUbicazione, int $limit = 50, ?int $afterId = null, ?string $apiKey = null): array
    {
        $this->assertAuthorized($apiKey);

        $codiceUbicazione = $this->sanitizeString($codiceUbicazione, 14, false);
        $limit = max(1, min($limit, 200));

        $sql = sprintf(
            'SELECT `_id`, `Data`, `CodiceUbicazione`, `Operazione`, `Descrizione`, `Valore1`, `Valore2`, `Valore3`, `Valore4`, `Valore5`, `Valore6`, `Note1`, `Note2`, `Note3`, `Note4`, `Dispositivo`
             FROM `%s`
             WHERE (:CodiceUbicazioneFilter IS NULL OR `CodiceUbicazione` = :CodiceUbicazioneValue)
               AND (:AfterIdFilter IS NULL OR `_id` > :AfterIdValue)
             ORDER BY `_id` DESC
             LIMIT :LimitRows',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        if ($codiceUbicazione === null) {
            $stmt->bindValue(':CodiceUbicazioneFilter', null, PDO::PARAM_NULL);
            $stmt->bindValue(':CodiceUbicazioneValue', null, PDO::PARAM_NULL);
        } else {
            $stmt->bindValue(':CodiceUbicazioneFilter', $codiceUbicazione, PDO::PARAM_STR);
            $stmt->bindValue(':CodiceUbicazioneValue', $codiceUbicazione, PDO::PARAM_STR);
        }
        if ($afterId === null || $afterId <= 0) {
            $stmt->bindValue(':AfterIdFilter', null, PDO::PARAM_NULL);
            $stmt->bindValue(':AfterIdValue', null, PDO::PARAM_NULL);
        } else {
            $stmt->bindValue(':AfterIdFilter', $afterId, PDO::PARAM_INT);
            $stmt->bindValue(':AfterIdValue', $afterId, PDO::PARAM_INT);
        }
        $stmt->bindValue(':LimitRows', $limit, PDO::PARAM_INT);
        $stmt->execute();
        $items = $stmt->fetchAll(PDO::FETCH_ASSOC);

        return [
            'ok' => true,
            'count' => count($items),
            'items' => $items,
        ];
    }

    public function readLatestEventStatus(string $codiceUbicazione, ?string $apiKey = null): array
    {
        $this->assertAuthorized($apiKey);

        $codiceUbicazione = $this->sanitizeString($codiceUbicazione, 14, true);

        $sql = sprintf(
            'SELECT `_id`, `Data`, `CodiceUbicazione`, `Operazione`, `Descrizione`, `Valore1`, `Valore2`, `Valore3`, `Valore4`, `Valore5`, `Valore6`, `Note1`, `Note2`, `Note3`, `Note4`, `Dispositivo`,
                    TIMESTAMPDIFF(SECOND, `Data`, NOW()) AS `AgeSeconds`
             FROM `%s`
             WHERE `CodiceUbicazione` = :CodiceUbicazione
             ORDER BY `_id` DESC
             LIMIT 1',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
        $stmt->execute();
        $item = $stmt->fetch(PDO::FETCH_ASSOC);

        if (!is_array($item)) {
            return [
                'ok' => true,
                'found' => false,
                'shouldInsert' => true,
                'reason' => 'missing_latest_event',
                'message' => 'Nessun record presente per il codice ubicazione richiesto.',
                'item' => null,
            ];
        }

        $ageSeconds = isset($item['AgeSeconds']) ? max(0, (int) $item['AgeSeconds']) : PHP_INT_MAX;
        $olderThan24Hours = ($ageSeconds >= 86400);
        $valore6IsNull = !array_key_exists('Valore6', $item) || $item['Valore6'] === null;
        $shouldInsert = $olderThan24Hours || $valore6IsNull;

        return [
            'ok' => true,
            'found' => true,
            'shouldInsert' => $shouldInsert,
            'reason' => $olderThan24Hours
                ? 'older_than_24_hours'
                : ($valore6IsNull ? 'valore6_is_null' : 'up_to_date'),
            'message' => $olderThan24Hours
                ? 'Ultimo record piu vecchio di 24 ore.'
                : ($valore6IsNull ? 'Ultimo record con Valore6 nullo.' : 'Ultimo record recente e completo.'),
            'ageSeconds' => $ageSeconds,
            'valore6IsNull' => $valore6IsNull,
            'item' => $item,
        ];
    }

    private function findLatestRequestSlotId(string $codiceUbicazione): ?int
    {
        $sql = sprintf(
            'SELECT `_id`
             FROM `%s`
             WHERE `CodiceUbicazione` = :CodiceUbicazione
               AND `Operazione` = :Operazione
             ORDER BY `_id` DESC
             LIMIT 1',
            $this->tableName
        );

        $stmt = $this->pdo->prepare($sql);
        $stmt->bindValue(':CodiceUbicazione', $codiceUbicazione, PDO::PARAM_STR);
        $stmt->bindValue(':Operazione', 'MASTER_REQUEST', PDO::PARAM_STR);
        $stmt->execute();
        $row = $stmt->fetch(PDO::FETCH_ASSOC);

        if (!is_array($row) || !isset($row['_id'])) {
            return null;
        }

        return (int) $row['_id'];
    }

    private function purgeExpiredEvents(): void
    {
        $sql = sprintf(
            'DELETE FROM `%s` WHERE `Data` < (NOW() - INTERVAL %d DAY)',
            $this->tableName,
            $this->retentionDays
        );
        $this->pdo->exec($sql);
    }

    private function assertAuthorized(?string $providedKey): void
    {
        if ($this->apiKey === null) {
            return;
        }

        $providedKey = is_string($providedKey) ? trim($providedKey) : '';
        if ($providedKey === '' || !hash_equals($this->apiKey, $providedKey)) {
            throw new RuntimeException('API key non valida');
        }
    }

    private function sanitizeString($value, int $maxLen, bool $required): ?string
    {
        if ($value === null) {
            if ($required) {
                throw new InvalidArgumentException('Campo obbligatorio mancante');
            }
            return null;
        }

        $value = trim((string) $value);
        if ($value === '') {
            if ($required) {
                throw new InvalidArgumentException('Campo obbligatorio vuoto');
            }
            return null;
        }

        return substr($value, 0, $maxLen);
    }

    private function bindNullableString(PDOStatement $stmt, string $param, ?string $value): void
    {
        if ($value === null || $value === '') {
            $stmt->bindValue($param, null, PDO::PARAM_NULL);
            return;
        }

        $stmt->bindValue($param, $value, PDO::PARAM_STR);
    }

    private function bindNullableInt(PDOStatement $stmt, string $param, $value): void
    {
        if ($value === null || $value === '') {
            $stmt->bindValue($param, null, PDO::PARAM_NULL);
            return;
        }

        $stmt->bindValue($param, (int) $value, PDO::PARAM_INT);
    }
}
