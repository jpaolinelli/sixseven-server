# Server Configuration

Create a `config.json` file and pass it as the first argument:

```bash
./build/debug/src/sixseven-server config.json
```

All fields are optional and fall back to defaults:

```json
{
  "data_dir": "./data",
  "port": 6767,
  "log_level": "info",
  "buffer_pool_size_mb": 256,
  "wal_segment_size_mb": 16,
  "max_connections": 100,
  "auth_method": "scram-sha-256",
  "shutdown_timeout_s": 30,
  "archive_enabled": false,
  "archive_cleanup_policy": "keep_all",
  "replication_max_wal_senders": 10,
  "replication_keepalive_interval_ms": 10000,
  "replication_sender_timeout_ms": 60000
}
```

| Field | Default | Description |
|-------|---------|-------------|
| `data_dir` | `./data` | Storage directory for database files |
| `port` | `6767` | TCP listen port |
| `log_level` | `info` | Logging level: `trace`, `debug`, `info`, `warn`, `error`, `fatal` |
| `buffer_pool_size_mb` | `256` | Buffer pool size in MB |
| `wal_segment_size_mb` | `16` | Write-ahead log segment size in MB |
| `max_connections` | `100` | Maximum concurrent client connections |
| `auth_method` | `scram-sha-256` | Authentication: `trust`, `md5`, or `scram-sha-256`. See the [Security Guide](security.md) |
| `shutdown_timeout_s` | `30` | Seconds to wait for active queries on graceful shutdown |

## The `sixseven_system` database

Beyond `config.json`, runtime state and configuration live in the built-in
`sixseven_system` database. Its system catalog tables are created on first
start and persisted across restarts:

| Table | Holds | Configured via |
|-------|-------|----------------|
| `sys_settings` | Server settings (seeded from config, some runtime-mutable) | `SET <key> = <value>` |
| `sys_providers` | Embedding provider definitions (with encrypted API keys) | `INSERT INTO sixseven_system.sys_providers ...` |
| `sys_users` | Database login credentials (one-way hashed) | `CREATE/ALTER/DROP USER` — see [Security Guide](security.md) |
| `sys_embedding_columns` | EMBEDDING column → provider mappings | Implicitly by `CREATE TABLE ... EMBEDDING(...)` |
| `sys_embedding_jobs` | Durable queue of pending embedding generation jobs | Implicitly on INSERT/UPDATE |
| `sys_databases` | Registry of databases in the catalog | `CREATE DATABASE` |

You can inspect them directly, e.g. `SELECT username FROM sixseven_system.sys_users`.

### Runtime settings

Runtime-mutable settings can be changed without a restart:

```sql
SET logging.level = 'debug';
```

### Configuring OpenAI (and other) embedding models

Embedding generation for `EMBEDDING` columns is driven by a *provider*, named
`type/model`. To use OpenAI, set the API key for the session and reference the
provider in your column definition:

```sql
-- Provide the API key (stored encrypted; never echoed back in plaintext).
SET embedding_api_key = 'sk-...';

CREATE TABLE docs (
    id INT PRIMARY KEY,
    content TEXT,
    vec EMBEDDING(1536, source='content', provider='openai/text-embedding-3-small')
);
```

On INSERT/UPDATE, SixSevenDB asynchronously calls the provider, stores the
returned vector, and indexes it (HNSW) so `NEAREST` queries work. To register a
provider durably (with an encrypted key) instead of per session, insert into
`sixseven_system.sys_providers`. Supported provider types are `openai`,
`ollama`, `onnx`, and `builtin` — full details, models, and offline options are
in the [Embedding Providers Guide](embedding-providers.md).

## Replication

SixSevenDB supports primary/standby replication. Start a standby with `--standby` or set `standby_mode` in config:

| Field | Default | Description |
|-------|---------|-------------|
| `standby_mode` | `false` | Run as read-only standby |
| `replication_primary_host` | `""` | Primary server hostname |
| `replication_primary_port` | `6767` | Primary server port |
| `replication_synchronous_mode` | `off` | `off`, `remote_write`, `remote_flush`, `remote_apply` |
