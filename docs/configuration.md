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
  "auth_method": "trust",
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
| `auth_method` | `trust` | Authentication: `trust`, `md5`, or `scram-sha-256` |
| `shutdown_timeout_s` | `30` | Seconds to wait for active queries on graceful shutdown |

## Replication

SixSevenDB supports primary/standby replication. Start a standby with `--standby` or set `standby_mode` in config:

| Field | Default | Description |
|-------|---------|-------------|
| `standby_mode` | `false` | Run as read-only standby |
| `replication_primary_host` | `""` | Primary server hostname |
| `replication_primary_port` | `6767` | Primary server port |
| `replication_synchronous_mode` | `off` | `off`, `remote_write`, `remote_flush`, `remote_apply` |
