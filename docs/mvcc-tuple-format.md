# MVCC Tuple Format (file format v2, GDB-714)

## Overview

As of file format version 2, every tuple stored in a SQL table file carries a
24-byte MVCC header so that transaction metadata (`xmin` / `xmax`) persists
with the row. This is the storage foundation for MVCC visibility and VACUUM
(audit item C10); visibility filtering in scans and transactional stamping
arrive with the transactions epic.

## On-page tuple layout

Table files created by `StorageManager` construct their `TableHeap` with
`TableHeapOptions{.mvcc_headers = true}`. Each slotted-page tuple is stored
as:

```
[ MvccTupleHeader (24 bytes) | user data (TupleSerializer bytes) ]

MvccTupleHeader:
  xmin        uint64   creating transaction id
  xmax        uint64   deleting transaction id (0 = live)
  t_ctid.page uint32   version chain: next version page id (reserved)
  t_ctid.slot uint16   version chain: next version slot id (reserved)
  padding     uint16   reserved
```

The header is transparent to callers of `TableHeap`: insert/update accept
user bytes and get/scan return user bytes. `TableHeap::get_tuple_header(rid)`
exposes the header. The maximum user payload per tuple shrinks by 24 bytes
(8192 - 24 page header - 4 slot entry - 24 MVCC header = 8140 bytes).

Heaps constructed without options (`TableHeap(bpm, dm, file_id)`) keep the
legacy headerless layout; this is used by the graph engine's edge heaps and
low-level tests.

### Header semantics today

- **insert** stamps `xmin` and leaves `xmax = 0`. While DML executes without
  transaction context (autocommit), `xmin` is the reserved `frozen_txn_id`
  (`UINT64_MAX`), meaning "committed, visible to every snapshot" — the
  PostgreSQL `FrozenTransactionId` analogue. `TransactionManager::get_status`,
  the MVCC visibility helpers, and WAL recovery special-case it as COMMITTED.
- **update** preserves the existing header and replaces the user data
  (in-place updates do not create version-chain entries yet).
- **delete** stamps `xmax` into the tuple image carried by the WAL DELETE
  record, then physically deletes the slot. Physical deletion is intentionally
  preserved so the executor's observable semantics (deleted rows vanish
  immediately; space is reclaimable by page compaction) are unchanged until
  visibility filtering and a shared VACUUM horizon land (GDB-1230).

## WAL records

`TableHeap::attach_wal(writer, table_id)` enables physiological logging of
tuple mutations. INSERT/UPDATE/DELETE records address `(table_id, page_id,
slot_id)` and carry a payload of full on-page tuple images **including the
MVCC header**, so redo reconstructs pages byte-identically:

```
payload := [before_len u32][before image][after_len u32][after image]

INSERT:  before = empty,                 after = inserted image
UPDATE:  before = pre-update image,      after = post-update image
DELETE:  before = image with xmax stamped, after = empty
```

`TableHeapRecoveryHandler` (see `include/sixseven/table/table_wal.h`) applies
these records during `WalRecovery`:

- redo: INSERT/UPDATE restore the after-image at the exact RID (allocating
  pages and extending slot directories as needed); DELETE removes the slot.
- undo: INSERT removes the slot; UPDATE restores the before-image; DELETE
  restores the before-image with `xmax` cleared (the deleter didn't commit).

All operations are idempotent. Records stamped with `frozen_txn_id` are
treated as committed by recovery (autocommit semantics) and always redone.

WAL logging is wired per-heap and is not yet enabled on the SQL DML path;
turning it on globally (together with BEGIN/COMMIT records and page-LSN
gating) is part of the transactions epic.

## Upgrade / rebuild story (v1 → v2)

There is **no in-place migration** from format v1 (headerless tuples). The
file format version stored in every database file header (page 0) was bumped
from 1 to 2:

- A v2 binary refuses to open v1 files with:
  `file format version 1 is older than this binary's version 2 (MVCC tuple
  headers); rebuild required: dump and re-create the database`.
- A v1 binary refuses v2 files with its own `unsupported file version: 2`.

To upgrade an existing data directory:

1. With the **old** binary, export the data (e.g. `SELECT` the rows of each
   table and save the output, or keep the original `CREATE TABLE` / `INSERT`
   scripts).
2. Stop the server and move the old data directory aside.
3. Start the **new** binary with a fresh data directory and replay the DDL
   and data (`CREATE TABLE` + `INSERT`).

Index, graph, and HNSW sidecar files are rebuilt from table data, so only
table contents need to be exported.
