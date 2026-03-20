# Admin Commands

## Implemented

```sql
SHOW DATABASES;
SHOW TABLES;
SHOW COLUMNS FROM users;
SHOW EDGE TYPES;
SHOW INDEXES;
SHOW EMBEDDINGS;

EXPLAIN SELECT * FROM users WHERE age > 25;
EXPLAIN ANALYZE SELECT * FROM users WHERE age > 25;
```

## Not Yet Implemented

The following commands are planned but not yet functional:

```sql
SHOW PROVIDERS;         -- provider cache not initialized
DESCRIBE users;         -- not implemented
VACUUM;                 -- not implemented
ANALYZE;                -- not implemented
SET log_level = 'debug';          -- unrecognized parameter
SET max_connections = 200;        -- unrecognized parameter
SHOW PARAMETER max_connections;   -- unrecognized parameter
```
