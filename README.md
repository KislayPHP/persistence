# KislayPHP Persistence

[![PHP Version](https://img.shields.io/badge/PHP-8.2%2B-blue.svg)](https://php.net)
[![License](https://img.shields.io/badge/License-Apache%202.0-green.svg)](LICENSE)
[![Build Status](https://img.shields.io/github/actions/workflow/status/KislayPHP/persistence/ci.yml?branch=main&label=CI)](https://github.com/KislayPHP/persistence/actions)
[![PIE](https://img.shields.io/badge/install-pie-blueviolet)](https://github.com/php/pie)

> **Per-request data persistence lifecycle for PHP microservices.** Automatic transaction management when attached to a `kislayphp/core` App — begin on request start, commit or rollback on request end.

Part of the [KislayPHP ecosystem](https://skelves.com/kislayphp/docs).

---

## ✨ What It Does

`kislayphp/persistence` provides a persistence runtime that integrates with `kislayphp/core`'s request lifecycle hooks. Attach it to your App and every request gets automatic transaction management — begin on start, commit on success, rollback on exception.

```php
<?php
Kislay\Persistence\Runtime::attach($app);  // that's it
```

Every request now has a clean transaction scope. No manual begin/commit/rollback per handler.

---

## 📦 Installation

```bash
pie install kislayphp/persistence
```

Enable in `php.ini`:
```ini
extension=kislayphp_persistence.so
```

---

## 🚀 Quick Start

### Automatic Transaction Per Request

```php
<?php
$app = new Kislay\Core\App();

// Attach persistence — handles begin/commit/rollback per request automatically
Kislay\Persistence\Runtime::attach($app);

$app->post('/api/orders', function($req, $res) {
    $data = $req->getJson();

    // Transaction is already open — just write your business logic
    $orderId = DB::create('orders', $data);
    Inventory::decrement($data['product_id'], $data['qty']);
    Email::queue('order_confirmation', $data['email']);

    $res->json(['order_id' => $orderId], 201);
    // Transaction commits automatically on clean return
    // Transaction rolls back automatically on exception
});

$app->listen('0.0.0.0', 8080);
```

### Manual Usage

```php
<?php
$persistence = new Kislay\Persistence\Runtime();

$persistence->begin();

try {
    $id = DB::insert('users', $data);
    Log::write('user_created', $id);
    $persistence->commit();
} catch (\Throwable $e) {
    $persistence->rollback();
    throw $e;
}
```

---

## 📖 Public API

```php
namespace Kislay\Persistence;

class Runtime {
    public function __construct();
    public static function attach(Kislay\Core\App $app): void;  // per-request auto lifecycle
    public function begin(): bool;
    public function commit(): bool;
    public function rollback(): bool;
    public function isActive(): bool;
}
```

Legacy aliases: `KislayPHP\Persistence\Runtime`

---

## 🔗 Ecosystem

[core](https://github.com/KislayPHP/core) · [gateway](https://github.com/KislayPHP/gateway) · [discovery](https://github.com/KislayPHP/discovery) · [metrics](https://github.com/KislayPHP/metrics) · [queue](https://github.com/KislayPHP/queue) · [eventbus](https://github.com/KislayPHP/eventbus) · **persistence**

## 📄 License

[Apache License 2.0](LICENSE) · **[Full Docs](https://skelves.com/kislayphp/docs)**
