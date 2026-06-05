# SixSevenDB — Demo Video / Loom Script

A ready-to-record walkthrough that shows off **one engine doing relational + graph +
vector**, across both the **server** (PostgreSQL wire protocol) and the **web admin
console**. Every query below is copy-paste-able and comes from
[`docs/demo-queries.sql`](./demo-queries.sql), run against the auto-seeded **SixSeven
Bookstore** demo database.

- **Target length:** ~9 minutes (a 60-second teaser cut is at the bottom).
- **Goal:** get viewers to `docker pull` and try it. Lead with the payoff (three
  databases in one), prove it live, end with a single command.

---

## Before you hit record (2-minute checklist)

- [ ] **Start from a clean data dir** so the demo DB seeds fresh and embeddings are
      ready. The demo "SixSeven Bookstore" is auto-created on first run.
- [ ] **Pre-warm vector search.** On first boot the ONNX model generates embeddings in
      the background. Run one `NEAREST ...` query *before* recording so the model is
      loaded and results are instant on camera.
- [ ] **BM25 indexes ship pre-built.** The demo database auto-creates full-text indexes on
      `books.description` and `reviews.review_text`, so the `MATCH ... TO` queries
      return immediately — no setup needed. (If you want the "watch me add full-text
      search" beat, you can still run `CREATE INDEX ... USING bm25` on another column live.)
- [ ] **Web console running** (`cd web && npm run dev`) and connected — the status dot
      top-right should be **green**.
- [ ] **Login:** the connection profile defaults to `sixseven` / `sixseven`. Make sure
      the password is filled in (auth is SCRAM-SHA-256 by default).
- [ ] **Zoom in.** Bump the SQL editor / terminal font size (≥16pt) and use a ~1280×800
      capture so text is legible after compression.
- [ ] **Have a terminal ready** with `psql` (or the bundled `sixseven-cli`) for the
      "it's just Postgres" moment.
- [ ] **Hide secrets** — no real API keys / tokens on screen.
- [ ] Keep `docs/demo-queries.sql` open in a side tab to copy from.

> **Port note:** the server listens on **6767** by default and the Docker image exposes
> **6767**, so `docker run -p 6767:6767 ...` and `psql -p 6767` match out of the box. If
> you override the port via a mounted `config.json`, publish and connect to that port
> instead.

---

## 0:00 — Cold open (the hook) · ~15s

**ON SCREEN:** The web console, Graph panel already showing a colorful node-and-edge
network. Hold for a beat.

**SAY:**
> "This is one database. It's running a SQL query, a graph traversal, and a semantic
> vector search — at the same time, over the same data. No Postgres plus Neo4j plus a
> vector store glued together. One engine. It speaks the PostgreSQL wire protocol, and
> it ships as a single Docker image. Let me show you."

---

## 0:15 — What it is · ~30s

**ON SCREEN:** Slowly pan the four panels in the left nav: **Schema, Query, Graph,
Dashboard.**

**SAY:**
> "SixSevenDB is a hybrid relational, graph, and vector database written in C++20. If
> you know SQL, you already know how to use it — it's PostgreSQL wire-compatible, so
> `psql`, your ORM, and any Postgres driver just connect. The twist: alongside normal
> tables and joins, you get native graph traversals, graph algorithms like PageRank,
> semantic vector search, *and* BM25 full-text search — all in the same query language,
> the same transaction, the same engine."

---

## 0:45 — Setup: one command, zero config · ~45s

**ON SCREEN:** Terminal.

```bash
docker pull <your-registry>/sixsevendb:latest
docker run -p 6767:6767 -v sixseven-data:/data <your-registry>/sixsevendb:latest
```

**SAY:**
> "Here's the whole setup. Pull the image, run it. On first boot it bootstraps a demo
> dataset — a fictional bookstore with authors, books, readers, reviews, a social graph
> of who-follows-who, and pre-computed text embeddings. The embedding model is bundled
> *inside the image*, so semantic search works offline, out of the box — no API keys,
> no external service."

**ON SCREEN:** Switch to the web console; point at the green status dot top-right.

**SAY:**
> "I'll drive most of this from the web admin console, which talks to that same server.
> Green dot — we're connected as the `sixseven` user."

---

## 1:30 — Dashboard tour · ~30s

**ON SCREEN:** Click **Dashboard**.

**SAY:**
> "The dashboard gives you the live pulse of the server — connections, throughput, and
> the shape of your data at a glance. This is the admin console you get for free; point
> any Postgres BI tool at the same port if you'd rather."

---

## 2:00 — Relational basics: it's real SQL · ~60s

**ON SCREEN:** Click **Query**. Paste and run, one at a time.

```sql
SELECT * FROM books LIMIT 10;
```

```sql
SELECT title, rating, published_year, pages
FROM books
WHERE genre LIKE 'Thriller'
ORDER BY rating DESC
LIMIT 10;
```

```sql
SELECT genre, COUNT(*) AS book_count, AVG(rating) AS avg_rating
FROM books
GROUP BY genre
ORDER BY avg_rating DESC;
```

**SAY:**
> "Let's start where everyone's comfortable — plain SQL. Select, filter, sort. Group-by
> with aggregates. Notice the autocomplete knows the schema. This is a familiar
> relational database; nothing exotic yet. The point is: your existing SQL knowledge
> carries over completely."

---

## 3:00 — Joins: relational power · ~45s

**ON SCREEN:** Run:

```sql
SELECT b.title, b.genre, r.stars, rd.username
FROM reviews r
JOIN books b ON b.id = r.book_id
JOIN readers rd ON rd.id = r.reader_id
WHERE rd.city = 'Tokyo'
ORDER BY r.stars DESC
LIMIT 10;
```

**SAY:**
> "Multi-table joins, exactly as you'd expect — what are readers in Tokyo reviewing?
> Three tables, joined and filtered. So far, a competent SQL database. Now let's do the
> thing other SQL databases make painful."

---

## 3:45 — Graph traversal (the visual wow) · ~90s

**ON SCREEN:** Run:

```sql
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3;
```

**SAY:**
> "This is a graph traversal — walk the `follows` relationships out from reader 1, up to
> three hops. Friend, friend-of-friend, friend-of-friend-of-friend. In a normal SQL
> database that's a gnarly recursive CTE. Here it's a first-class `TRAVERSE` clause, and
> `__depth` tells you how many hops away each reader is."

**ON SCREEN:** Switch the results view from **Table** to **Graph**. Let the network
render and settle.

**SAY:**
> "And because it knows this is a graph, the console renders it as one — nodes are
> readers, edges are follows. You can see the network structure immediately."

**ON SCREEN:** Run the filtered traversal:

```sql
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3
WHERE city = 'London';
```

**SAY:**
> "I can filter mid-traversal — say, only readers in London within three hops — and
> still see how they connect back through the network. Relational filtering and graph
> structure, together, in one query."

> **Recorder tip:** drag a node or two so the layout looks alive; hover an edge to show
> the relationship. This filtered query's nodes connect through intermediate readers, so
> the graph stays connected.

---

## 5:15 — Graph algorithms as table functions · ~45s

**ON SCREEN:** Run:

```sql
SELECT node_id, score
FROM PageRank('follows')
ORDER BY score DESC
LIMIT 10;
```

```sql
SELECT node_id, centrality
FROM betweenness('follows')
ORDER BY centrality DESC
LIMIT 10;
```

**SAY:**
> "It's not just traversal — real graph analytics ship in the box. PageRank to find the
> most influential readers. Betweenness centrality to find the people who bridge
> communities. There's also connected components and closeness. They're exposed as table
> functions, so you `SELECT` from them and compose them with the rest of your SQL."

---

## 6:00 — Vector search: semantic, built in · ~75s

**ON SCREEN:** Run:

```sql
SELECT * FROM books WHERE NEAREST(description_vec, 5) TO 'time travel adventure';
```

```sql
SELECT * FROM books WHERE NEAREST(description_vec, 5) TO 'artificial intelligence and technology';
```

**SAY:**
> "Now the third database. These books have an embedding column — a vector generated from
> the description by the bundled model. `NEAREST` does semantic search: I type a
> natural-language idea — 'time travel adventure' — and it returns the books that *mean*
> that, even when the words don't match. Change the phrase, change the meaning, get
> different books. This is the same kind of search powering AI apps and RAG — and it's a
> native column type with an HNSW index, not a bolted-on extension."

**ON SCREEN:** Run:

```sql
SELECT * FROM books WHERE NEAREST(description_vec, 10) TO 'survival story on a remote island' USING COSINE;
```

**SAY:**
> "You pick the distance metric, and it's indexed for speed. No separate vector store to
> keep in sync — the embeddings live next to your rows and update with them."

---

## 7:15 — Full-text search: BM25, the other kind of search · ~60s

**ON SCREEN:** Run, one at a time:

```sql
SELECT title, genre, _score
FROM books
WHERE MATCH(description) TO 'political intrigue power'
ORDER BY _score DESC
LIMIT 10;
```

**SAY:**
> "Vector search finds *meaning*. But sometimes you want the opposite — exact keywords,
> ranked by relevance. That's BM25, the algorithm behind Lucene and Elasticsearch. The
> demo ships with a full-text index on the book descriptions — built with one
> `CREATE INDEX ... USING bm25` — so `MATCH ... TO` ranks every book by how well its
> description matches my keywords. `_score` is the relevance, highest first — and it
> composes with everything else SQL gives you."

**ON SCREEN:** Run the hybrid filter, then the lexical-vs-semantic contrast:

```sql
SELECT title, rating, _score
FROM books
WHERE MATCH(description) TO 'detective' AND genre = 'Mystery'
ORDER BY _score DESC LIMIT 10;
```

```sql
-- Keywords must appear (lexical):
SELECT title, _score FROM books
WHERE MATCH(description) TO 'survival island'
ORDER BY _score DESC LIMIT 5;
-- Meaning, even without those words (semantic):
SELECT * FROM books WHERE NEAREST(description_vec, 5) TO 'survival story on a remote island';
```

**SAY:**
> "I can mix full-text relevance with a plain `genre` filter in the same `WHERE`. And
> here's the punchline — lexical and semantic search, side by side: BM25 wants the words
> 'survival' and 'island' to actually appear; the vector search returns books that *mean*
> survival on a remote island even when the wording differs. Same data, same engine, two
> complementary kinds of search — and the index stays in sync automatically as rows
> change."

---

## 8:15 — The payoff: one engine, four models · ~30s

**SAY:**
> "Here's why that matters. In a normal stack you'd run Postgres for the tables, a graph
> database for the relationships, a vector store for the embeddings, and a search engine
> like Elasticsearch for full-text — four systems, four sync pipelines, four things to
> operate. SixSevenDB is *one* engine, *one* copy of the data, *one* transaction.
> Relational, graph, vector, and full-text, side by side."

---

## 8:45 — "It's just Postgres" + EXPLAIN · ~30s

**ON SCREEN:** Terminal — connect with plain `psql`:

```bash
psql -h localhost -p 6767 -U sixseven
```

```sql
EXPLAIN ANALYZE SELECT * FROM books WHERE rating > 4.0;
```

**SAY:**
> "And it really is wire-compatible — here's stock `psql` connecting, no special client.
> `EXPLAIN ANALYZE` shows you the real execution plan and timing, just like you'd
> expect. Your tools, your drivers, your habits — they all work."

---

## 9:15 — Close + call to action · ~20s

**ON SCREEN:** Back to the Graph panel (the pretty shot). Overlay the pull command.

**SAY:**
> "That's SixSevenDB: relational, graph, vector, and full-text in a single
> PostgreSQL-compatible engine, in one Docker image you can run right now. Pull it, point
> `psql` at it, open the console, and try your own data. Link's in the description — I'd
> love to hear what you build."

```bash
docker run -p 6767:6767 <your-registry>/sixsevendb:latest
```

---

## Appendix A — 60-second teaser cut (for X / LinkedIn)

Fast, no narration pauses, captions on. One screen: the web console.

1. **0:00–0:05** — Graph panel network on screen. Caption: *"One database. SQL + Graph +
   Vector + Full-text."*
2. **0:05–0:16** — Type and run a `GROUP BY` aggregate. Caption: *"Real SQL. It's
   Postgres-wire compatible."*
3. **0:16–0:30** — Run the `TRAVERSE ... MAX_DEPTH 3` query, flip to Graph view, drag a
   node. Caption: *"Native graph traversals — no recursive CTEs."*
4. **0:30–0:42** — Run `SELECT * FROM books WHERE NEAREST(description_vec, 5) TO 'time travel adventure'`.
   Caption: *"Semantic vector search, built in. Model included."*
5. **0:42–0:52** — Run `... WHERE MATCH(description) TO 'political intrigue' ORDER BY
   _score DESC`. Caption: *"BM25 full-text search, ranked by relevance."*
6. **0:52–1:00** — Cut to terminal: `docker run -p 6767:6767 .../sixsevendb`. Caption:
   *"One image. Try it now 👇"*

---

## Appendix B — Query cheat-sheet (in record order)

| # | Segment | Query (short form) |
|---|---------|--------------------|
| 1 | Relational | `SELECT * FROM books LIMIT 10;` |
| 2 | Filter/sort | `... FROM books WHERE genre LIKE 'Thriller' ORDER BY rating DESC` |
| 3 | Aggregate | `SELECT genre, COUNT(*), AVG(rating) FROM books GROUP BY genre` |
| 4 | Join | `reviews ⋈ books ⋈ readers WHERE rd.city = 'Tokyo'` |
| 5 | Traversal | `TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3` |
| 6 | Filtered traversal | `... MAX_DEPTH 3 WHERE city = 'London'` |
| 7 | PageRank | `SELECT node_id, score FROM PageRank('follows') ...` |
| 8 | Betweenness | `SELECT node_id, centrality FROM betweenness('follows') ...` |
| 9 | Vector | `SELECT * FROM books WHERE NEAREST(description_vec, 5) TO 'time travel adventure'` |
| 10 | Vector + metric | `... WHERE NEAREST(description_vec, 10) TO 'survival story on a remote island' USING COSINE` |
| 11 | BM25 search | `... WHERE MATCH(description) TO 'political intrigue power' ORDER BY _score DESC` |
| 12 | BM25 + filter | `... WHERE MATCH(description) TO 'detective' AND genre = 'Mystery'` |
| 13 | Plan | `EXPLAIN ANALYZE SELECT * FROM books WHERE rating > 4.0` |

(The BM25 indexes ship pre-built with the demo DB; `CREATE INDEX ... USING bm25` is how you'd add your own.)

Full set: [`docs/demo-queries.sql`](./demo-queries.sql).

---

## Appendix C — Talking points / FAQ to keep in your back pocket

- **"Why not just use Postgres + pgvector + a graph extension?"** One engine means one
  copy of the data, one transaction boundary, and one thing to operate — no ETL/sync
  between three systems, and your graph/vector queries join directly against your tables.
- **"Is it really Postgres-compatible?"** It speaks the v3 wire protocol — `psql` and
  standard Postgres drivers connect. (Set expectations honestly on SQL surface coverage.)
- **"Where do the embeddings come from?"** A local ONNX model (`all-MiniLM-L6-v2`) is
  bundled in the image — semantic search works with no external API. You can also wire up
  OpenAI/Ollama providers for other models.
- **"BM25 vs vector search — when do I use which?"** BM25 (full-text) is *lexical*: it
  ranks by exact keyword matches, with stemming and stop-words — great for precise terms,
  names, and codes. Vector search is *semantic*: it matches meaning even when the words
  differ — great for natural-language and RAG. They're complementary, and because both
  live in one engine you can combine them (and add plain SQL filters) in a single query.
- **"Does the full-text index stay in sync?"** Yes — `INSERT`/`UPDATE`/`DELETE` maintain
  the BM25 index incrementally, and it's persisted so it survives a restart.
- **"What's under the hood?"** C++20, Volcano-style executor, buffer pool, write-ahead
  log, MVCC, B+-tree indexes, an HNSW index for vectors, and a BM25 inverted index for
  full-text.
- **Default login** is `sixseven` / `sixseven` over SCRAM-SHA-256 — tell viewers to
  change it for anything real.
</content>
</invoke>
