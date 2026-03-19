# ODBC Interface Support — Research & Recommendation

**Ticket**: [GDB-562](https://undiscoveredtech.atlassian.net/browse/GDB-562)
**Date**: 2026-03-17
**Author**: Joseph Paolinelli

## Executive Summary

SixSevenDB should implement **pg_catalog virtual tables** to enable psqlODBC and BI tool connectivity. This is the lowest-effort, highest-impact approach — matching what CockroachDB and YugabyteDB do. Building a native ODBC driver is unnecessary given our PostgreSQL wire protocol compatibility.

## 1. Market Analysis

### How Competitors Handle ODBC

| Database | Wire Protocol | ODBC Approach | Native Driver? |
|----------|--------------|---------------|----------------|
| CockroachDB | PostgreSQL | psqlODBC | No |
| YugabyteDB | PostgreSQL | psqlODBC | No |
| QuestDB | PostgreSQL | Third-party (Devart) | No |
| ClickHouse | Custom (HTTP/Native) | Official C++ driver | Yes |
| DuckDB | Custom | Official driver | Yes |
| Neo4j | Bolt | Third-party (Simba/CData) | No |
| TigerGraph | Custom | Third-party (CData) | No |
| Pinecone | REST/gRPC | None | No |
| Weaviate | REST/GraphQL | None | No |
| Milvus | gRPC/REST | None | No |

### Key Observations

- **Every PG-wire-compatible database relies on psqlODBC.** None have built a native ODBC driver. This is the established pattern.
- Only ClickHouse and DuckDB (non-PG-wire databases) invested in native ODBC drivers — they had no alternative.
- **Vector databases have zero ODBC support.** Even basic psqlODBC compatibility would differentiate SixSevenDB from Pinecone, Weaviate, and Milvus.
- Graph databases (Neo4j, TigerGraph) rely on third-party vendors (Simba/insightsoftware, CData) for ODBC, which involves SQL-to-native-query translation layers.

## 2. psqlODBC Compatibility Assessment

### Current SixSevenDB Wire Protocol Support

SixSevenDB already implements:
- Full PG v3.0 wire protocol (simple + extended query)
- MD5 and SCRAM-SHA-256 authentication
- Type OID mapping for all 22 SixSevenDB types (including EMBEDDING = OID 100000)
- SQLSTATE error codes
- Transaction state tracking (BEGIN/COMMIT/ROLLBACK)
- Session variables (SET/SHOW/RESET)
- Prepared statements via wire protocol

### Critical Gap: No pg_catalog Support

psqlODBC queries these pg_catalog tables on every connection and metadata operation:

| Table | Purpose | psqlODBC Usage |
|-------|---------|---------------|
| `pg_catalog.pg_type` | Type metadata | SQLGetTypeInfo, type resolution |
| `pg_catalog.pg_class` | Table/relation metadata | SQLTables, table discovery |
| `pg_catalog.pg_attribute` | Column metadata | SQLColumns, column introspection |
| `pg_catalog.pg_namespace` | Schema information | Schema-qualified names |
| `pg_catalog.pg_index` | Index metadata | SQLStatistics, SQLPrimaryKeys |
| `pg_catalog.pg_database` | Database listing | Connection setup |

**SixSevenDB currently implements none of these.** This means psqlODBC can connect (authentication works) but all metadata operations fail — BI tools cannot browse schemas or tables.

### Additional Missing Features

| Feature | Impact | Priority |
|---------|--------|----------|
| SSL/TLS | psqlODBC may require it; server currently rejects SSL requests | Medium |
| SAVEPOINT | psqlODBC uses this for error recovery during catalog queries | High |
| Cancel requests | Long query cancellation; currently ignored | Low |

## 3. BI Tool Requirements

### Tableau
- Calls `SQLGetInfo` extensively to detect driver capabilities
- Uses `SQLTables`, `SQLColumns`, `SQLGetTypeInfo`, `SQLStatistics` for schema browsing
- All backed by pg_catalog queries through psqlODBC
- Customizable via TDC files to override detected capabilities

### Power BI
- Uses ODBC for generic database access via DSN or connection string
- Retrieves metadata to present tables for selection (Catalog > Schema > Table hierarchy)
- DirectQuery mode requires reliable metadata and `SQLGetInfo` responses
- ODBC 3.8 compliance preferred

### Looker
- Uses JDBC, not ODBC — not relevant for this initiative
- Would need a LookML dialect (separate effort)

### Common Requirements
All BI tools need:
- Working `SQLTables` / `SQLColumns` (→ pg_class, pg_attribute)
- Accurate `SQLGetTypeInfo` (→ pg_type)
- Schema navigation (→ pg_namespace)
- Standard aggregate function support (already implemented in SixSevenDB)

## 4. Effort Estimation

### Option A: pg_catalog Virtual Tables (RECOMMENDED)

**Scope**: Implement 6-7 virtual tables backed by SixSevenDB's internal catalog.

| Component | Effort |
|-----------|--------|
| Virtual table infrastructure (schema registration, query routing) | 2-3 days |
| pg_type (22 type rows) | 1 day |
| pg_class (table + index entries) | 1-2 days |
| pg_attribute (column metadata) | 1-2 days |
| pg_namespace (public + pg_catalog schemas) | 0.5 day |
| pg_index (B+ tree index entries) | 1 day |
| pg_database (single row) | 0.5 day |
| SAVEPOINT support | 1 day |
| psqlODBC integration testing | 2-3 days |
| Power BI integration testing | 1-2 days |
| **Total** | **~2-3 weeks** |

**Advantages:**
- Zero ODBC driver code to write or maintain
- Immediate compatibility with psqlODBC, `psql` metadata commands (`\dt`, `\d`), and BI tools
- Follows the established pattern (CockroachDB, YugabyteDB)
- SixSevenDB's internal catalog already has all the data; just needs SQL-queryable exposure

**Disadvantages:**
- Cannot expose non-SQL features (graph traversal, vector search) through ODBC metadata
- Must maintain pg_catalog compatibility as psqlODBC evolves
- EMBEDDING type will appear as a custom/unknown type to BI tools

### Option B: Native ODBC Driver

**Scope**: Implement ODBC Core conformance (30+ API functions), cross-platform build.

| Component | Effort |
|-----------|--------|
| Core ODBC API (30+ functions) | 4-6 weeks |
| Type mapping and conversion | 1-2 weeks |
| Catalog functions | 1-2 weeks |
| Cross-platform build (Windows, macOS, Linux) | 1-2 weeks |
| Driver manager integration (unixODBC, Windows DM) | 1 week |
| Testing with BI tools | 2-3 weeks |
| **Total** | **~3-4 months** |

Alternative: SimbaEngine SDK reduces this to ~2-4 weeks but adds commercial licensing costs and third-party dependency.

### Option C: Do Nothing

Not viable. Users can connect but cannot browse schemas — BI tools are unusable.

## 5. Recommendation

**Implement pg_catalog virtual tables (Option A).**

### Rationale

1. **Industry standard**: Every PG-wire-compatible database takes this approach.
2. **Minimal effort**: ~2-3 weeks vs ~3-4 months for a native driver.
3. **Maximum compatibility**: Works with psqlODBC, `psql`, BI tools, and any PG-compatible client.
4. **Competitive advantage**: Vector DBs have zero ODBC support; even basic BI connectivity differentiates SixSevenDB.
5. **Low risk**: The data already exists in SixSevenDB's catalog — this is a presentation layer, not new logic.

### When to Reconsider

Build a native ODBC driver only if:
- A customer requires ODBC-native features not available through psqlODBC (rare)
- Graph/vector features need first-class ODBC metadata exposure (future consideration)
- psqlODBC compatibility proves too fragile to maintain (unlikely given CockroachDB/YugabyteDB precedent)

## 6. Technical Approach (Option A)

### Architecture

```
SQL Query: SELECT * FROM pg_catalog.pg_type
                |
         Parser: recognizes "pg_catalog" schema prefix
                |
         Binder: resolves to virtual catalog table
                |
         Planner: creates VirtualCatalogScan iterator
                |
         VirtualCatalogScan::next()
                |
         Reads from Catalog (existing internal metadata)
                |
         Returns rows matching PostgreSQL pg_catalog schema
```

### Implementation Strategy

1. **Register `pg_catalog` as a virtual schema** in the catalog — queries against it are routed to virtual table iterators rather than storage.

2. **Implement a `VirtualCatalogScan` iterator** that generates rows on-the-fly from the catalog's in-memory metadata.

3. **Each pg_catalog table is a function** that produces rows matching PostgreSQL's column layout. Only the columns that psqlODBC actually queries need to be populated — the rest can return NULL.

4. **SAVEPOINT support** — implement as a no-op that succeeds. psqlODBC uses savepoints for error recovery but SixSevenDB's simple transaction model doesn't need real savepoint semantics for this use case.

### Known Limitations to Document

- EMBEDDING columns will appear as type OID 100000 (unknown to BI tools) — they can still query the data as text
- Graph-specific metadata (edge types, node labels) is not exposed through pg_catalog
- Only the subset of pg_catalog columns that psqlODBC queries will be populated

## Appendix: psqlODBC Catalog Queries

Specific queries psqlODBC issues (from source code analysis):

```sql
-- Type discovery
SELECT oid, typname, typlen, typtype, typelem, typrelid
FROM pg_catalog.pg_type
WHERE typname IN ('int4', 'text', 'bool', ...)

-- Table listing (SQLTables)
SELECT c.relname, n.nspname, c.relkind
FROM pg_catalog.pg_class c
JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid
WHERE c.relkind IN ('r', 'v')

-- Column listing (SQLColumns)
SELECT a.attname, a.atttypid, a.attlen, a.attnum, a.attnotnull
FROM pg_catalog.pg_attribute a
JOIN pg_catalog.pg_class c ON a.attrelid = c.oid
WHERE c.relname = '<table>' AND a.attnum > 0

-- Index listing (SQLStatistics)
SELECT i.indexrelid, i.indkey, c.relname
FROM pg_catalog.pg_index i
JOIN pg_catalog.pg_class c ON i.indexrelid = c.oid
WHERE i.indrelid = <table_oid>
```
