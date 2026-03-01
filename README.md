# Kislay Persistence

Request-safe persistence runtime for long-lived KislayPHP servers.

## Concurrency Mode

- Default API mode is synchronous.
- Database and transaction operations are request-scoped and deterministic by design.
- This module should remain sync-first; async behavior belongs in core/gateway/eventbus and async HTTP clients.

## Why this extension

Persistent servers keep PHP objects alive across requests. This extension hardens two common failure modes:

- Unclosed DB transactions leaking between requests
- Unbounded static in-memory caches

It is designed to be attached once to `Kislay\Core\App`; after that, cleanup is automatic on each request.

## Install

```bash
pie install kislayphp/persistence:0.0.1
```

Add to `php.ini`:

```ini
extension=kislayphp_persistence.so
```

## Usage (C++ facades)

```php
<?php

$app = new Kislay\Core\App();
$config = require __DIR__ . '/config/app.php';

Kislay\Persistence\DB::boot($config['database']);
Kislay\Persistence\DB::attach($app);

$app->post('/users', function ($req, $res) {
    $user = Kislay\Persistence\DB::transaction(function (PDO $db) use ($req) {
        $payload = $req->getJson() ?? [];
        $stmt = $db->prepare('INSERT INTO users(name,email) VALUES(:name,:email)');
        $stmt->execute([
            ':name' => $payload['name'] ?? 'unknown',
            ':email' => $payload['email'] ?? 'unknown@example.com',
        ]);

        return [
            'id' => (int) $db->lastInsertId(),
            'name' => $payload['name'] ?? 'unknown'
        ];
    });

    $res->json($user, 201);
});
```

## API

`Kislay\Persistence\DB`:
- `boot(array $databaseConfig): bool`
- `connection(?string $name = null): PDO`
- `connect(?string $name = null): PDO`
- `transaction(callable $callback, ?string $connection = null): mixed`
- `attach(Kislay\Core\App $app): bool`
- `cleanup(): int`

`Kislay\Persistence\Eloquent`:
- `boot(array $databaseConfig): bool`
- `connection(?string $name = null): PDO`
- `transaction(callable $callback, ?string $connection = null): mixed`
- `attach(Kislay\Core\App $app): bool`

`Kislay\Persistence\Runtime`:
- `attach(Kislay\Core\App $app): bool`
- `track(object $pdo): bool`
- `transaction(object $pdo, callable $callback): mixed`
- `beginRequest(): void`
- `cleanup(): int`
- `cachePut(string $pool, string $key, mixed $value, ?int $ttlSeconds = null): bool`
- `cacheGet(string $pool, string $key, mixed $default = null): mixed`
- `cacheForget(string $pool, string $key): bool`
- `cacheClear(?string $pool = null): int`
- `setCacheLimits(int $maxEntriesPerPool, int $defaultTtlSeconds): bool`

## Notes

- Cache is in-process memory; use Redis/Memcached for distributed cache.
- `attach()` requires Core versions that expose `onRequestStart` and `onRequestEnd`.
- `DB::boot()` expects Laravel-style config shape: `default` + `connections`.
