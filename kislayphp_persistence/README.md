# KislayPersistence

> Request-safe database and caching extension for long-lived KislayPHP servers — auto-rolls back uncommitted transactions and evicts stale cache on every request boundary.

[![PHP Version](https://img.shields.io/badge/PHP-8.2+-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)

## Installation

**Via PIE (recommended):**
```bash
pie install kislayphp/persistence:0.0.1
```

Add to `php.ini`:
```ini
extension=kislayphp_persistence.so
```

**Build from source:**
```bash
git clone https://github.com/KislayPHP/persistence.git
cd persistence && phpize && ./configure --enable-kislayphp_persistence && make && sudo make install
```

## Requirements

- PHP 8.2+
- PDO extension with appropriate driver (pdo_mysql, pdo_pgsql, pdo_sqlite, …)
- kislayphp/core ≥ v0.0.2 (for `onRequestStart` / `onRequestEnd` hooks)

## Quick Start

```php
<?php
$app = new Kislay\Core\App();

Kislay\Persistence\DB::boot([
    'default' => 'mysql',
    'connections' => [
        'mysql' => [
            'driver'   => 'mysql',
            'host'     => '127.0.0.1',
            'port'     => 3306,
            'database' => 'mydb',
            'username' => 'root',
            'password' => 'secret',
        ],
    ],
]);

// Auto-cleanup: rolls back any open transaction after each request
Kislay\Persistence\DB::attach($app);

$app->get('/users', function ($req, $res) {
    $db    = Kislay\Persistence\DB::connection();
    $users = $db->query('SELECT id, name FROM users')->fetchAll(PDO::FETCH_ASSOC);
    $res->json($users);
});

$app->post('/users', function ($req, $res) {
    $user = Kislay\Persistence\DB::transaction(function (PDO $db) use ($req) {
        $payload = $req->body() ?? [];
        $stmt    = $db->prepare('INSERT INTO users (name, email) VALUES (:name, :email)');
        $stmt->execute([':name' => $payload['name'], ':email' => $payload['email']]);
        return ['id' => (int) $db->lastInsertId(), 'name' => $payload['name']];
    });
    $res->status(201)->json($user);
});

$app->listen('0.0.0.0', 8080);
```

## API Reference

### `DB`

#### `DB::boot(array $config): bool`
Initialises the connection pool. Must be called once before any other `DB` method.
- `$config` follows the Laravel/Illuminate database config shape:
  ```php
  [
    'default'     => 'mysql',
    'connections' => [
      'mysql' => ['driver' => 'mysql', 'host' => '...', 'database' => '...', 'username' => '...', 'password' => '...'],
    ],
  ]
  ```

#### `DB::connection(?string $name = null): PDO`
Returns the PDO instance for the named (or default) connection.

#### `DB::connect(?string $name = null): PDO`
Alias for `connection()`. Creates the connection lazily on first call.

#### `DB::ping(?string $name = null): bool`
Sends a lightweight keepalive. Returns `false` if the connection is lost.

#### `DB::transaction(callable $callback, ?string $connection = null): mixed`
Executes `$callback` inside a database transaction.
- Signature: `function(PDO $db): mixed`
- Commits on success, rolls back on exception and re-throws
- Returns the callback return value

```php
$result = Kislay\Persistence\DB::transaction(function (PDO $db) {
    $db->exec('UPDATE accounts SET balance = balance - 100 WHERE id = 1');
    $db->exec('UPDATE accounts SET balance = balance + 100 WHERE id = 2');
    return true;
});
```

#### `DB::attach(Kislay\Core\App $app): bool`
Registers `onRequestStart` and `onRequestEnd` hooks on `$app`.
- On request start: tracks open connections
- On request end: rolls back any uncommitted transaction and resets connection state

#### `DB::cleanup(): int`
Manually triggers cleanup (rollback + reset). Returns the number of connections cleaned.

---

### `Eloquent`

Drop-in replacement for `DB` when using Eloquent-style models.

| Method | Signature | Description |
|--------|-----------|-------------|
| `boot` | `boot(array $config): bool` | Initialise Eloquent with the same config shape as `DB` |
| `connection` | `connection(?string $name = null): PDO` | Return PDO for named connection |
| `transaction` | `transaction(callable $callback, ?string $connection = null): mixed` | Run callback in transaction |
| `attach` | `attach(Kislay\Core\App $app): bool` | Register lifecycle hooks |

---

### `Runtime`

Low-level lifecycle and in-process cache API.

#### `Runtime::attach(Kislay\Core\App $app): bool`
Registers request lifecycle hooks. Equivalent to `DB::attach()` but for manual PDO tracking.

#### `Runtime::track(object $pdo): bool`
Registers a PDO instance for automatic cleanup on request end.

#### `Runtime::transaction(object $pdo, callable $callback): mixed`
Runs `$callback` in a transaction on `$pdo`.

#### `Runtime::beginRequest(): void`
Manually signals request start. Called automatically by `attach()`.

#### `Runtime::cleanup(): int`
Manually triggers end-of-request cleanup. Returns number of connections cleaned.

#### `Runtime::cachePut(string $pool, string $key, mixed $value, ?int $ttlSeconds = null): bool`
Stores `$value` in the named in-process cache pool.
- `$pool` — logical cache namespace (e.g. `'users'`)
- `$ttlSeconds` — TTL; `null` uses the default set by `setCacheLimits()`

#### `Runtime::cacheGet(string $pool, string $key, mixed $default = null): mixed`
Returns a cached value, or `$default` if absent or expired.

#### `Runtime::cacheForget(string $pool, string $key): bool`
Removes a single cache entry.

#### `Runtime::cacheClear(?string $pool = null): int`
Clears all entries in a pool, or all pools if `$pool` is `null`. Returns entries removed.

#### `Runtime::setCacheLimits(int $maxEntriesPerPool, int $defaultTtlSeconds): bool`
Configures the in-process cache limits. Call before request handling starts.

## Configuration

### Config Shape for `DB::boot()`

```php
[
  'default' => 'mysql',                  // connection name to use by default
  'connections' => [
    'mysql' => [
      'driver'    => 'mysql',            // mysql | pgsql | sqlite
      'host'      => '127.0.0.1',
      'port'      => 3306,
      'database'  => 'mydb',
      'username'  => 'root',
      'password'  => 'secret',
      'charset'   => 'utf8mb4',          // optional
      'collation' => 'utf8mb4_unicode_ci', // optional
    ],
    'read_replica' => [
      'driver'   => 'mysql',
      'host'     => '10.0.0.2',
      'database' => 'mydb',
      'username' => 'reader',
      'password' => 'secret',
    ],
  ],
]
```

## Examples

### Multiple Connections

```php
Kislay\Persistence\DB::boot($config);

$primary = Kislay\Persistence\DB::connection('mysql');
$replica = Kislay\Persistence\DB::connection('read_replica');

$app->get('/users/:id', function ($req, $res) {
    $db   = Kislay\Persistence\DB::connection('read_replica');
    $stmt = $db->prepare('SELECT * FROM users WHERE id = ?');
    $stmt->execute([$req->param('id')]);
    $res->json($stmt->fetch(PDO::FETCH_ASSOC));
});
```

### In-Process Cache

```php
$app->get('/products/:id', function ($req, $res) {
    $id    = $req->param('id');
    $cache = Kislay\Persistence\Runtime::cacheGet('products', $id);

    if ($cache !== null) {
        $res->json($cache);
        return;
    }

    $db   = Kislay\Persistence\DB::connection();
    $stmt = $db->prepare('SELECT * FROM products WHERE id = ?');
    $stmt->execute([$id]);
    $product = $stmt->fetch(PDO::FETCH_ASSOC);

    Kislay\Persistence\Runtime::cachePut('products', $id, $product, ttlSeconds: 60);
    $res->json($product);
});
```

## Related Extensions

| Extension | Use Case |
|-----------|----------|
| [kislayphp/core](https://github.com/KislayPHP/core) | Provides `onRequestStart`/`onRequestEnd` hooks for auto-cleanup |
| [kislayphp/config](https://github.com/KislayPHP/config) | Supply DB credentials from a config store |
| [kislayphp/metrics](https://github.com/KislayPHP/metrics) | Track query counts and cache hit rates |

## License

Licensed under the [Apache License 2.0](LICENSE).
