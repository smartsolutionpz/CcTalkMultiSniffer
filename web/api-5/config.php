<?php
/**
 * Centralised configuration helpers for database access.
 *
 * The script loads environment variables from a local `.env` file when present
 * and exposes utility functions to retrieve credentials and open PDO/mysqli
 * connections. Sensitive credentials should never be committed to version
 * control; keep `.env` outside the repository and with restrictive permissions.
 */

if (!function_exists('gr_load_env')) {
    /**
     * Load environment variables from a .env file.
     */
    function gr_load_env(string $path): void
    {
        static $loadedPaths = [];
        if (isset($loadedPaths[$path]) || !is_readable($path)) {
            return;
        }

        $lines = file($path, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        if ($lines === false) {
            return;
        }

        foreach ($lines as $line) {
            $line = trim($line);
            if ($line === '' || $line[0] === '#') {
                continue;
            }

            $parts = explode('=', $line, 2);
            if (count($parts) !== 2) {
                continue;
            }

            $name = trim($parts[0]);
            $value = trim($parts[1]);

            $length = strlen($value);
            if ($length >= 2) {
                $quote = $value[0];
                if (($quote === '"' || $quote === "'") && $value[$length - 1] === $quote) {
                    $value = substr($value, 1, $length - 2);
                }
            }

            putenv($name . '=' . $value);
            $_ENV[$name] = $value;
            $_SERVER[$name] = $value;
        }

        $loadedPaths[$path] = true;
    }
}

if (!function_exists('gr_env')) {
    /**
     * Helper to fetch an environment variable with optional default.
     */
    function gr_env(string $key, ?string $default = null): ?string
    {
        $value = getenv($key);
        if ($value === false) {
            return $default;
        }
        return $value;
    }
}

if (!function_exists('gr_get_database_credentials')) {
    /**
     * Retrieve the database credentials from the environment.
     *
     * @return array{host: string, username: string, password: string, database: string}
     * @throws RuntimeException when a mandatory credential is missing.
     */
    function gr_get_database_credentials(): array
    {
        $rootDir = dirname(__DIR__);
        gr_load_env($rootDir . '/.env');
        // fallback: alcune installazioni mettono lo .env nella stessa cartella api
        gr_load_env(__DIR__ . '/.env');

        $host = gr_env('DB_HOST');
        $username = gr_env('DB_USERNAME');
        $password = gr_env('DB_PASSWORD');
        $database = gr_env('DB_DATABASE');

        if ($host === null || $host === '') {
            throw new RuntimeException('Variabile di ambiente DB_HOST non configurata.');
        }
        if ($username === null || $username === '') {
            throw new RuntimeException('Variabile di ambiente DB_USERNAME non configurata.');
        }
        if ($password === null) {
            throw new RuntimeException('Variabile di ambiente DB_PASSWORD non configurata.');
        }
        if ($database === null || $database === '') {
            throw new RuntimeException('Variabile di ambiente DB_DATABASE non configurata.');
        }

        return [
            'host' => $host,
            'username' => $username,
            'password' => $password,
            'database' => $database,
        ];
    }
}

if (!function_exists('getDatabaseConnection')) {
    /**
     * Create a PDO connection using the configured credentials.
     */
    function getDatabaseConnection(): PDO
    {
        $credentials = gr_get_database_credentials();
        $dsn = sprintf('mysql:host=%s;dbname=%s;charset=utf8mb4', $credentials['host'], $credentials['database']);

        $pdo = new PDO(
            $dsn,
            $credentials['username'],
            $credentials['password'],
            [
                PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
            ]
        );

        return $pdo;
    }
}

if (!function_exists('getMysqliConnection')) {
    /**
     * Create a mysqli connection using the configured credentials.
     */
    function getMysqliConnection(): mysqli
    {
        $credentials = gr_get_database_credentials();
        $connection = @new mysqli(
            $credentials['host'],
            $credentials['username'],
            $credentials['password'],
            $credentials['database']
        );

        if ($connection->connect_errno) {
            throw new RuntimeException('Connessione al database fallita: ' . $connection->connect_error);
        }

        $connection->set_charset('utf8mb4');

        return $connection;
    }
}
