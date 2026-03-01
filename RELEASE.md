# Release Guide

## Versioning policy

Current release line is `v0.0.x`.

- Start from `v0.0.1`.
- Keep incrementing patch while APIs are stabilizing.
- Do not cut `1.0.0` until namespace/API and runtime behavior are finalized.

## Pre-publish checks

Run from repository root:

```bash
phpize
./configure --enable-kislayphp_persistence
make -j4
php -n -d extension=modules/kislayphp_persistence.so --ri kislayphp_persistence
```

## Publish checklist

- Confirm `README.md`, `composer.json`, and `package.xml` are up to date.
- Confirm `package.xml` release and API versions are set correctly.
- Confirm docs reflect namespace (`Kislay\\...`) and compatibility behavior.
- Tag release and push tag to origin.
