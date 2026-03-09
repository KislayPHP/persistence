# KislayPHP Persistence Extension - Technical Reference

## Overview

The KislayPHP Persistence extension is a thin C++ wrapper around PHP's PDO (PHP Data Objects) that provides:
- Named connection pooling for multi-database setups
- Fluent query builders for common operations
- Built-in migration system with tracking
- Eloquent ORM integration
- Thread-safe connection management
- Health check utilities

**Namespace:** `Kislay\Persistence\DB` (alias: `KislayPHP\Persistence\DB`)

---

## 1. Architecture

### PDO Wrapper Design

The extension implements a minimalist PDO wrapper pattern in C++:

```cpp
// Internal structure (simplified)
std::unordered_map<std::string, zval> connections;  // Named connections
pthread_mutex_t lock;                                // Thread-safe access
```

Each named connection is stored as a lazy-loaded PDO object. The wrapper intercepts query execution to:
- Apply parameter binding
- Track migrations
- Support Eloquent integration

### Connection Lifecycle

1. **Registration**: `addConnection()` stores connection credentials
2. **Lazy Loading**: First query on a connection creates the PDO instance
3. **Persistence**: Connections live for the lifetime of the Persistence object
4. **Thread Safety**: All operations are protected by `pthread_mutex_t`

### Important Limitations

- **Single connection per name**: No true connection pooling—each named connection is one PDO instance
- **No connection reuse across requests**: Each PHP request gets its own Persistence object
- **Synchronous only**: All operations block; no async support

### Named Connection Pool

Multiple logical databases can be configured:

```php
$db = new Kislay\Persistence\DB();
$db->addConnection('default', 'mysql:host=localhost;dbname=main', 'user', 'pass');
$db->addConnection('read',    'mysql:host=replica1.local;dbname=main', 'user', 'pass');
$db->addConnection('write',   'mysql:host=master.local;dbname=main', 'user', 'pass');
```

Each connection is independent; you can use different servers, credentials, and databases.

---

## 2. Configuration Reference

### Environment Variables

| Variable | Type | Default | Purpose |
|----------|------|---------|---------|
| `KISLAY_DB_DEFAULT_CONNECTION` | string | `"default"` | Default connection name if not specified in methods |
| `KISLAY_RPC_ENABLED` | bool | `false` | Enable gRPC metrics/tracing integration |
| `KISLAY_RPC_TIMEOUT_MS` | long | `200` | Timeout for RPC calls (milliseconds) |

**Example:**
```bash
export KISLAY_DB_DEFAULT_CONNECTION="main"
export KISLAY_RPC_ENABLED=true
export KISLAY_RPC_TIMEOUT_MS=500
```

### Connection Parameters

#### `addConnection(string $name, string $dsn, string $user = '', string $password = '', array $options = []): bool`

**Parameters:**

- `$name` (string): Unique identifier for this connection (e.g., "default", "read", "analytics")
- `$dsn` (string): PDO Data Source Name
  - MySQL: `mysql:host=localhost;dbname=myapp;charset=utf8mb4`
  - PostgreSQL: `pgsql:host=localhost;dbname=myapp;user=postgres;password=pass`
  - SQLite: `sqlite:/path/to/database.db`
- `$user` (string): Database username
- `$password` (string): Database password
- `$options` (array): PDO constructor options

**PDO Options (most common):**

```php
$options = [
    PDO::ATTR_ERRMODE            => PDO::ERRMODE_EXCEPTION,  // Throw on error
    PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,       // Return assoc arrays
    PDO::ATTR_EMULATE_PREPARES   => false,                   // Use native prepared
    PDO::ATTR_TIMEOUT            => 30,                      // 30-second timeout
    PDO::MYSQL_ATTR_SSL_MODE     => PDO::MYSQL_SSL_MODE_REQUIRED,
];
```

**Returns:** `true` on success, `false` on error

**Example:**
```php
$db->addConnection(
    'main',
    'mysql:host=db.example.com;dbname=production;charset=utf8mb4',
    'app_user',
    getenv('DB_PASSWORD'),
    [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
    ]
);
```

---

## 3. API Reference

### Connection Management

#### `getConnection(string $name = 'default'): PDO`

Returns the raw PDO object for direct access.

```php
$pdo = $db->getConnection('default');
$stmt = $pdo->prepare('SELECT * FROM users WHERE id = ?');
$stmt->execute([42]);
```

**Throws:** `\Exception` if connection not found

#### `hasConnection(string $name): bool`

Check if a named connection exists.

```php
if ($db->hasConnection('replica')) {
    $data = $db->select('SELECT * FROM users', [], 'replica');
}
```

#### `removeConnection(string $name): bool`

Disconnect and remove a named connection.

```php
$db->removeConnection('analytics');  // Close connection
```

#### `ping(string $connection = 'default'): bool`

Health check—executes `SELECT 1` and returns true if successful.

```php
if (!$db->ping('write')) {
    throw new Exception('Write database offline');
}
```

### Query Execution

#### `select(string $sql, array $bindings = [], string $connection = 'default'): array`

Execute SELECT query. Returns **array of rows** as associative arrays.

```php
$users = $db->select(
    'SELECT id, name, email FROM users WHERE age > ? AND status = ?',
    [18, 'active'],
    'read'
);

foreach ($users as $user) {
    echo $user['name'];  // Associative array access
}
```

**Returns:** Empty array `[]` if no rows match

#### `selectOne(string $sql, array $bindings = [], string $connection = 'default'): array|null`

Execute SELECT query. Returns **first row only** or `null`.

```php
$admin = $db->selectOne(
    'SELECT * FROM users WHERE email = ? LIMIT 1',
    ['admin@example.com']
);

if ($admin) {
    echo $admin['name'];
} else {
    echo 'Admin not found';
}
```

**Returns:** `null` if no rows match

#### `insert(string $table, array $data, string $connection = 'default'): int`

Build and execute INSERT. Returns **last insert ID**.

```php
$id = $db->insert('users', [
    'name' => 'Alice',
    'email' => 'alice@example.com',
    'created_at' => date('Y-m-d H:i:s'),
]);

echo "Created user ID: $id";
```

**Returns:** Last inserted ID (or `0` for non-auto-increment tables)

#### `update(string $table, array $data, array $where, string $connection = 'default'): int`

Build and execute UPDATE. Returns **number of affected rows**.

```php
$updated = $db->update(
    'users',
    ['status' => 'inactive', 'updated_at' => date('Y-m-d H:i:s')],
    ['id' => 42]
);

echo "$updated rows updated";
```

**WHERE clause matching:** All `$where` conditions are AND'd together (exact equality).

#### `delete(string $table, array $where, string $connection = 'default'): int`

Build and execute DELETE. Returns **number of affected rows**.

```php
$deleted = $db->delete('sessions', ['expired' => 1]);
echo "Deleted $deleted expired sessions";
```

**Warning:** Be careful with WHERE conditions!

#### `statement(string $sql, array $bindings = [], string $connection = 'default'): bool`

Execute raw SQL (INSERT/UPDATE/DELETE). Returns **true on success**.

```php
$success = $db->statement(
    'UPDATE users SET login_count = login_count + 1 WHERE id = ?',
    [42]
);
```

Use for complex queries the helper methods can't express.

#### `raw(string $sql, string $connection = 'default'): PDOStatement`

Prepare and execute SQL, return the PDOStatement for advanced operations.

```php
$stmt = $db->raw('SELECT * FROM orders WHERE total > 1000 ORDER BY created_at DESC');
while ($row = $stmt->fetch(PDO::FETCH_ASSOC)) {
    process($row);  // Stream processing
}
```

### Transactions

#### `beginTransaction(string $connection = 'default'): bool`

Start a transaction. Returns `true` on success.

```php
$db->beginTransaction();
try {
    $db->insert('orders', $orderData);
    $db->update('inventory', ['qty' => $qty - 1], ['id' => $item_id]);
    $db->commit();
} catch (Exception $e) {
    $db->rollback();
    throw $e;
}
```

#### `commit(string $connection = 'default'): bool`

Commit the current transaction.

#### `rollback(string $connection = 'default'): bool`

Roll back the current transaction.

#### `inTransaction(string $connection = 'default'): bool`

Check if currently in a transaction.

```php
if ($db->inTransaction()) {
    $db->rollback();
}
```

### Migrations

#### `runMigrations(array $migrations, string $connection = 'default'): int`

Run pending migrations. Returns **count of migrations executed**.

**Migration Format:**

```php
$migrations = [
    [
        'name' => 'create_users_table_2024',
        'up'   => 'CREATE TABLE users (
                    id INT AUTO_INCREMENT PRIMARY KEY,
                    name VARCHAR(255) NOT NULL,
                    email VARCHAR(255) UNIQUE,
                    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
                  )',
        'down' => 'DROP TABLE users',
    ],
    [
        'name' => 'add_phone_to_users',
        'up'   => 'ALTER TABLE users ADD COLUMN phone VARCHAR(20)',
        'down' => 'ALTER TABLE users DROP COLUMN phone',
    ],
];

$executed = $db->runMigrations($migrations);
echo "Ran $executed migrations";
```

**Tracking:** Stores migration names in `_kislay_migrations` table with batch number and timestamp.

#### `rollbackMigrations(int $steps = 1, string $connection = 'default'): int`

Rollback the last N migration batches. Returns **count of migrations rolled back**.

```php
$rolled_back = $db->rollbackMigrations(1);  // Undo last batch
echo "Rolled back $rolled_back migrations";

$db->rollbackMigrations(5);  // Undo last 5 batches
```

#### `getMigrations(string $connection = 'default'): array`

Retrieve all executed migrations.

```php
$migrations = $db->getMigrations();
foreach ($migrations as $m) {
    echo "{$m['migration']} (batch {$m['batch']})\n";
}
```

**Returns:** Array of objects with `migration`, `batch`, `ran_at` fields.

#### Migrations Table Schema

Automatically created if not exists:

```
_kislay_migrations:
  - id (INT AUTO_INCREMENT PRIMARY KEY)
  - migration (VARCHAR(255))
  - batch (INT)
  - ran_at (TIMESTAMP DEFAULT CURRENT_TIMESTAMP)
```

### Eloquent Integration

#### `bootEloquent(string $connection = 'default'): void`

Initialize Eloquent ORM with a named connection.

**Requirements:** `illuminate/database` package must be installed.

```php
// Requires: composer require illuminate/database
$db->bootEloquent('default');

// Now use standard Eloquent models
$users = User::where('status', 'active')->get();
$user = User::find(42);
```

**Note:** Boot Eloquent before defining/using Model classes. Call once per connection.

---

## 4. Patterns and Recipes

### Multi-Database Setup

```php
$db = new Kislay\Persistence\DB();

// Production database
$db->addConnection('default', 'mysql:host=db1.example.com;dbname=prod', 'user', 'pass');

// Read replica for reporting
$db->addConnection('read', 'mysql:host=db-replica.example.com;dbname=prod', 'user', 'pass');

// Analytics warehouse
$db->addConnection('analytics', 'mysql:host=warehouse.example.com;dbname=analytics', 'user', 'pass');

// Use accordingly
$dashboard = $db->select('SELECT COUNT(*) as cnt FROM users', [], 'read');
$stats = $db->select('SELECT * FROM events', [], 'analytics');
$db->insert('logs', ['msg' => 'event'], 'default');
```

### Read/Write Splitting

```php
// Reads go to replica
$user = $db->selectOne('SELECT * FROM users WHERE id = ?', [$id], 'read');

// Writes go to primary
$db->beginTransaction('default');
try {
    $db->update('users', ['updated_at' => now()], ['id' => $id], 'default');
    $db->insert('audit_log', [...], 'default');
    $db->commit('default');
} catch (Exception $e) {
    $db->rollback('default');
}
```

### Transaction Wrapper Pattern

```php
function withTransaction(callable $callback, string $conn = 'default') {
    global $db;
    $db->beginTransaction($conn);
    try {
        $result = $callback();
        $db->commit($conn);
        return $result;
    } catch (Exception $e) {
        $db->rollback($conn);
        throw $e;
    }
}

// Usage
$userId = withTransaction(function() use ($db) {
    $id = $db->insert('users', ['name' => 'Bob']);
    $db->insert('notifications', ['user_id' => $id, 'msg' => 'Welcome!']);
    return $id;
});
```

### Migration Workflow

```php
// Define migrations
$migrations = [
    [
        'name' => 'initial_schema',
        'up' => file_get_contents('migrations/001_initial.sql'),
        'down' => file_get_contents('migrations/001_initial_down.sql'),
    ],
];

// Run
$executed = $db->runMigrations($migrations);
echo "Deployed $executed migrations\n";

// Check status
$ran = $db->getMigrations();
var_dump($ran);

// Rollback if needed
$db->rollbackMigrations(1);
```

---

## 5. Performance Notes

### Connection Pooling Limitations

- **Single PDO per name:** Each named connection is one PDO instance, not a true pool
- **No multiplexing:** Cannot distribute load across multiple PDO objects
- **Appropriate for:** Low-traffic sites, internal tools
- **Not recommended for:** High-concurrency applications (use connection pooling proxy like pgBouncer)

### Prepared Statements

All query methods use prepared statements internally. Binding prevents SQL injection:

```php
// SAFE: Uses prepared statements
$db->select('SELECT * FROM users WHERE name = ?', ['O\'Reilly']);

// SAFE: Named parameters also work via raw PDO
$stmt = $db->raw('SELECT * FROM users WHERE age > :age AND status = :status');
$stmt->execute([':age' => 21, ':status' => 'active']);
```

### Connection Health

Monitor connection liveness:

```php
// Health check endpoint
if (!$db->ping('default')) {
    http_response_code(503);
    exit('Database unavailable');
}

// Periodically in background jobs
set_error_handler(function() use ($db) {
    if (!$db->ping()) {
        // Notify ops
        mail('ops@example.com', 'DB Down', 'Database ping failed');
    }
});
```

### Query Optimization

- Use `selectOne()` instead of `select()` when fetching single rows
- Use `select()` with LIMIT for large result sets, process in chunks
- Index foreign keys and WHERE columns
- Use read replicas for reporting queries (see Multi-Database Setup above)

---

## 6. Troubleshooting

### Error: "Connection not found"

**Problem:** Trying to use a connection that was not registered.

```php
// Wrong: 'analytics' not added
$data = $db->select('SELECT * FROM events', [], 'analytics');  // ❌ throws

// Fix: Register first
$db->addConnection('analytics', 'mysql:host=...', 'user', 'pass');
$data = $db->select('SELECT * FROM events', [], 'analytics');  // ✓
```

### Error: "Migration table does not exist"

**Problem:** Migrations table not created automatically on some databases.

**Fix:** Create manually:

```php
$db->statement(
    'CREATE TABLE IF NOT EXISTS _kislay_migrations (
        id INT AUTO_INCREMENT PRIMARY KEY,
        migration VARCHAR(255) NOT NULL,
        batch INT NOT NULL,
        ran_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )'
);

// Then run migrations
$db->runMigrations($migrations);
```

### Error: "No transaction active"

**Problem:** Calling `commit()` or `rollback()` without `beginTransaction()`.

```php
// Wrong
$db->commit();  // ❌ Error: no transaction started

// Correct
$db->beginTransaction();
// ... queries ...
$db->commit();  // ✓
```

### Error: "Prepared statement failed"

**Problem:** Parameter binding mismatch.

```php
// Wrong: 2 placeholders, 1 binding
$db->select('SELECT * FROM users WHERE id = ? AND status = ?', [42]);  // ❌

// Correct: matching count
$db->select('SELECT * FROM users WHERE id = ? AND status = ?', [42, 'active']);  // ✓
```

### Eloquent "Model not found" after boot

**Problem:** Booting Eloquent on wrong connection or before connection created.

```php
// Wrong order
$db->bootEloquent('default');  // default not added yet
$db->addConnection('default', '...', '...', '...');  // Too late

// Correct order
$db->addConnection('default', 'mysql:host=...', 'user', 'pass');
$db->bootEloquent('default');  // Now boot

class User extends Model {}  // Define models after booting
```

### Connection timeout on large queries

**Problem:** Long-running queries exceed PDO timeout.

**Fix:** Increase timeout in options:

```php
$db->addConnection('default', $dsn, $user, $pass, [
    PDO::ATTR_TIMEOUT => 300,  // 5 minutes
]);
```

---

## Summary of Public Methods

| Method | Returns | Notes |
|--------|---------|-------|
| `addConnection()` | bool | Register a named connection |
| `getConnection()` | PDO | Raw PDO object |
| `hasConnection()` | bool | Check existence |
| `removeConnection()` | bool | Disconnect |
| `ping()` | bool | Health check |
| `select()` | array | All matching rows |
| `selectOne()` | array\|null | First row or null |
| `insert()` | int | Last insert ID |
| `update()` | int | Affected row count |
| `delete()` | int | Affected row count |
| `statement()` | bool | Raw SQL execution |
| `raw()` | PDOStatement | Prepare + execute |
| `beginTransaction()` | bool | Start transaction |
| `commit()` | bool | Commit transaction |
| `rollback()` | bool | Rollback transaction |
| `inTransaction()` | bool | Check if in transaction |
| `runMigrations()` | int | Count executed |
| `rollbackMigrations()` | int | Count rolled back |
| `getMigrations()` | array | Executed migrations |
| `bootEloquent()` | void | Enable Eloquent ORM |

---

**Version:** 1.0  
**Last Updated:** 2024  
**License:** Check phpExtension repository for details
