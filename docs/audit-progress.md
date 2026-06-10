# Audit progress checkpoint

Report: `docs/audit-2026-06-09.md`. Approach: main-loop only (no subagent swarms), batched greps/reads, one phase per session.

## Done
- [x] Finder sweep: all 344 test files + 11/15 source partitions (338 findings, from interrupted workflow run `wf_9121101a-ba3`)
- [x] 7 criticals hand-verified
- [x] Broken QA-fixture cluster reproduced (GDB-596 et al.: `database with id 1 not found`, binary aborts)
- [x] Phase 1 (2026-06-10): all 43 highs triaged — 43 confirmed, 0 refuted. Empirical: GDB-251 autoincrement restart broken at HEAD (product bug); GDB-569 stale bug-lock-in test fails at HEAD.

## Remaining (in priority order)
- [x] Phase 2 (2026-06-10): txn + executor core. HEADLINE: no transactions exist — BEGIN/COMMIT/ROLLBACK error at the planner (NOT_IMPLEMENTED); entire src/txn/ module is dead code (no MVCC, no locks, no vacuum); updates overwrite in place. Also: int/int division returns float; no integer overflow checks; DECIMAL arithmetic via double confirmed. Cleared: mvcc.cpp logic correct (unreachable); window functions wired; NULL 3VL present; dual-planner is semantic-vs-physical, not duplication. Report section 2c.
- [x] Phase 3 (2026-06-10): relational ops + infra. HEADLINE: B+tree/hash indexes never maintained by DML (C12, critical — wrong results after insert-after-index; IndexManager has no per-row API; all tests insert before indexing). Mediums: hash_join duplicates flawed ValueHash (1-bucket joins on temporal/decimal keys); COUNT(DISTINCT) O(n^2); BM25 maintenance errors swallowed. Cleared: window functions real; SMJ duplicates OK; join NULL semantics OK; caches/persistence OK. Report section 2d.
- [x] Phase 4 (2026-06-10): mechanical sweeps. SortMergeJoin/BitmapScan/ExternalSort operators never constructed in production; trio >50% copy-paste (~200 shared lines); 51 std::sto* sites in 12 files. Cleared: no raw throws, no CMake orphans, no dead headers. Report section 2e. AUDIT PLANNED SCOPE COMPLETE.
- [ ] Optional: spot-check top mediums (~20 worth triaging); lows stay as leads

## Rules learned
- No verification agent swarms (burned ~3M tokens). Verify in main loop with targeted greps.
- Findings data: `/tmp/audit_salvage.json`, `/tmp/audit_grouped.json` (regenerate from workflow journal `.../workflows/wf_9121101a-ba3.json` if /tmp cleared).
