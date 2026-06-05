# Security & Authentication

SixSevenDB authenticates clients over the PostgreSQL wire protocol. Login
credentials are stored in the `sys_users` table inside the built-in
`sixseven_system` database, so they survive server restarts.

## Authentication methods

The method is set with `auth_method` in `config.json`:

| Method | Description |
|--------|-------------|
| `scram-sha-256` | **Default.** Modern challenge-response auth (PBKDF2-HMAC-SHA-256). Recommended. |
| `md5` | Legacy PostgreSQL MD5 auth. Supported for older clients. |
| `trust` | No password check — any username connects. Use only on trusted, isolated networks. |

```json
{
  "auth_method": "scram-sha-256"
}
```

> **Breaking change:** the default was previously `trust` (no password). It is
> now `scram-sha-256`. Deployments that relied on password-less access must
> explicitly set `"auth_method": "trust"` to keep the old behavior.

## Default login

On first start (or whenever no users exist yet), SixSevenDB seeds a default
administrator:

| Username | Password |
|----------|----------|
| `sixseven` | `sixseven` |

The credential is hashed with the configured `auth_method` and persisted to
`sixseven_system.sys_users`.

> **Change the default password immediately** on any non-local deployment:
>
> ```sql
> ALTER USER sixseven WITH PASSWORD 'a-strong-secret';
> ```

## Connecting

Any PostgreSQL client works. For example, with `psql`:

```bash
psql "host=localhost port=6767 user=sixseven dbname=sixseven"
# password: sixseven   (until you change it)
```

## Managing users

User management is done with SQL. Changes are written through to `sys_users`
immediately and take effect for subsequent logins.

```sql
-- Create a user
CREATE USER alice WITH PASSWORD 'her-secret';

-- Change a password
ALTER USER alice WITH PASSWORD 'her-new-secret';

-- Remove a user
DROP USER alice;
DROP USER IF EXISTS alice;   -- no error if absent
```

You can list users with:

```sql
SELECT username FROM sixseven_system.sys_users;
```

## How credentials are stored

- Passwords are **never stored in plaintext**. For `scram-sha-256`, only the
  PBKDF2-derived stored/server keys and a random per-user salt are kept; for
  `md5`, only the salted MD5 digest. These are one-way — the original password
  cannot be recovered from `sys_users`.
- Each row records the method used to hash it, so existing logins keep working
  even if you later change the server's `auth_method`. To re-hash a user under a
  new method, run `ALTER USER ... WITH PASSWORD ...` again.

## Persistence & restart behavior

- Users persist in `sys_users` and are reloaded into memory on startup, so they
  survive restarts.
- The default admin is seeded only when the user store is empty, so it is **not**
  recreated after you delete it (unless the store becomes empty again).

### Legacy data directories

`sys_users` uses a reserved catalog table id (11). On a data directory created
*before* this feature existed, that id may already belong to a user table. In
that case the server logs an error and falls back to **in-memory** users
(credentials are not persisted and reset on restart). To get persisted
authentication, use a data directory created with this version (or later).
