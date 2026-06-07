# SixSevenDB Demo Video Script

Target length: 4:30
Queries: docs/demo-queries.sql
Demo database: SixSeven Bookstore (auto-seeded on first run)


========================================
PRE-RECORD CHECKLIST
========================================

- Start from a clean data dir so the demo DB seeds fresh
- Pre-warm vector search: run one NEAREST query before recording
- Pre-run ALL hero queries once to verify results with current seed data
- BM25 indexes ship pre-built, no setup needed
- Web console running (cd web && npm run dev), green status dot
- Login: sixseven / sixseven (SCRAM-SHA-256)
- Font size 16pt or larger, capture at 1280x800
- Terminal ready with psql or sixseven-cli
- No API keys or tokens visible on screen
- Keep docs/demo-queries.sql open in a side tab
- Confirm reader 1 has enough follows for a visually impressive graph view
- Test graph view renders on TRAVERSE results (click Graph tab)


========================================
0:00  MONOLOGUE + REVEAL
~30 seconds
========================================

[BLACK SCREEN OR PRESENTER ON SIMPLE BACKDROP]
[NO PRODUCT ON SCREEN YET]

I needed a relational database for structured data.
A graph database for relationships and traversals.
And a vector database for semantic search and AI.

Three databases.
Three sync pipelines.
Three things to operate.

And every time I wanted to answer one question
that touched all three, I was writing glue code
to stitch separate systems together.

So I built one engine that does all three natively.

[CUT TO SIXSEVENDB LOGO]

SixSevenDB.

[CUT TO TERMINAL]

    psql -h localhost -p 6767 -U sixseven

It speaks PostgreSQL wire protocol.
psql connects. Your ORM connects.
Let me show you what this engine can do.


========================================
0:30  SCHEMA DDL: BUILDING A GRAPH RAG SCHEMA
~45 seconds
========================================

[WEB CONSOLE QUERY PANEL]

First, how you build a Graph RAG schema.

    CREATE TABLE documents (
        id INT PRIMARY KEY,
        title TEXT,
        body TEXT,
        body_vec EMBEDDING(384, body, 'local')
    );

EMBEDDING is a column type.
You declare the source text column and the provider.
The engine generates and indexes vectors automatically.
No external pipeline, no batch job.

    CREATE EDGE TYPE cites FROM documents TO documents;
    CREATE EDGE TYPE authored FROM authors TO documents;

Edge types are first-class schema objects.
They define relationships between tables.

    LINK authors(1) TO documents(42) VIA authored;
    LINK documents(42) TO documents(7) VIA cites;

LINK connects rows with typed edges.
Now you have a knowledge graph
with embedded vectors, in one schema.

The demo ships with a bookstore dataset
that already has this wired up:
books, authors, readers, reviews,
a social graph, and text embeddings.

Let me show you what you can do with it.


========================================
1:15  GRAPH + VECTOR BASICS
~45 seconds
========================================

[RUN QUERY]

    SELECT username, city, __depth
    FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3;

Graph traversal. One clause.
Walk the social network three hops deep.
Friend, friend-of-friend, friend-of-friend-of-friend.

Not a recursive CTE. A first-class TRAVERSE.

[SWITCH TO GRAPH VIEW -- LET THE NETWORK RENDER AND SETTLE]
[DRAG A NODE OR TWO SO THE LAYOUT LOOKS ALIVE]

And the console renders it as a network.

[BACK TO QUERY PANEL]
[RUN QUERY]

    SELECT title, genre, rating
    FROM books
    WHERE NEAREST(description_vec, 5)
      TO 'a desperate struggle for survival against nature';

Semantic search. One clause.
Natural language in, relevant books out,
even when the exact words don't appear.

Graph traversal, one clause.
Semantic search, one clause.

Now watch what happens when you combine them.


========================================
2:00  HERO 1: GRAPH-SCOPED VECTOR SEARCH
~45 seconds
========================================

[PAUSE FOR EFFECT]
[TYPE THE QUERY SLOWLY, LET THE AUDIENCE READ IT]
[RUN QUERY]

    SELECT title, genre, rating
    FROM books
    WHERE NEAREST(description_vec, 5)
      TO 'mind-bending science fiction about consciousness'
    WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2;

Graph RAG in one statement.

The engine traverses reader 1's social network
two hops deep, collects the candidate set,
then runs semantic vector search
only within that subgraph.

No pipeline. No orchestration code.
No syncing between systems.
One query. One transaction.

The graph shapes what the vector search sees.

This is what Graph RAG looks like
when it's native to the engine.


========================================
2:45  HERO 2: MATCH PATTERN + VECTOR SEARCH
~50 seconds
========================================

[TYPE THE QUERY]
[RUN QUERY]

    SELECT b.title, b.genre, b.rating
    FROM books b
    WHERE NEAREST(b.description_vec, 10)
      TO 'epic fantasy with political intrigue'
    AND b.id IN (
      MATCH (rd:readers)-[f:follows]->{1,3}(friend:readers)
            <-[rv:reviewed_by]-(book:books)
      WHERE rd.id = 1
      RETURN book.id
    );

One query. Let me break it down.

The MATCH clause pattern-matches
reader 1's social network
with variable-length paths:
one to three hops of follows.

For every friend-of-a-friend it finds,
it discovers books they reviewed
via the graph edge.

Then NEAREST runs semantic vector search
over only those books
for "epic fantasy with political intrigue."

Graph pattern matching.
Variable-length traversal.
Semantic vector similarity.
All in one SQL statement.

This is what no combination of separate
databases can give you.


========================================
3:35  HERO 3: PAGERANK + JOIN + BM25
~40 seconds
========================================

[RUN QUERY]

    SELECT rd.username, r.stars, r.review_text, _score
    FROM reviews r
    JOIN readers rd ON rd.id = r.reader_id
    JOIN (
      SELECT node_id FROM PageRank('follows')
      ORDER BY score DESC LIMIT 20
    ) AS influencers ON influencers.node_id = r.reader_id
    WHERE MATCH(r.review_text) TO 'masterpiece'
      AND r.stars >= 4
    ORDER BY _score DESC
    LIMIT 10;

PageRank runs over the social graph.
Finds the 20 most influential readers.

Standard SQL JOIN pulls their reviews.

BM25 full-text search ranks by
the keyword "masterpiece."
Filter to four stars and above.

Graph algorithm, relational join, full-text search.
One query. One result set.

Algorithms are table functions.
They compose with everything SQL gives you.


========================================
4:15  CLOSE + CALL TO ACTION
~15 seconds
========================================

[GRAPH PANEL -- THE PRETTY SHOT]
[OVERLAY THE DOCKER RUN COMMAND]

That is SixSevenDB.

Your relational database,
your graph database,
your vector database,
and your search engine.

One C++ engine.
One transaction.
PostgreSQL-compatible.

    docker run -p 6767:6767 sixsevendb/sixsevendb:latest

Pull it and try your own data.


========================================
QUERY REFERENCE SHEET
========================================

Copy-paste these from docs/demo-queries.sql

SETUP
    psql -h localhost -p 6767 -U sixseven

DDL-1  CREATE TABLE documents (
           id INT PRIMARY KEY,
           title TEXT,
           body TEXT,
           body_vec EMBEDDING(384, body, 'local')
       );

DDL-2  CREATE EDGE TYPE cites FROM documents TO documents;
       CREATE EDGE TYPE authored FROM authors TO documents;

DDL-3  LINK authors(1) TO documents(42) VIA authored;
       LINK documents(42) TO documents(7) VIA cites;

Q1  SELECT username, city, __depth
    FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3;

Q2  SELECT title, genre, rating FROM books
    WHERE NEAREST(description_vec, 5)
      TO 'a desperate struggle for survival against nature';

HERO-1  SELECT title, genre, rating FROM books
        WHERE NEAREST(description_vec, 5)
          TO 'mind-bending science fiction about consciousness'
        WITHIN TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 2;

HERO-2  SELECT b.title, b.genre, b.rating
        FROM books b
        WHERE NEAREST(b.description_vec, 10)
          TO 'epic fantasy with political intrigue'
        AND b.id IN (
          MATCH (rd:readers)-[f:follows]->{1,3}(friend:readers)
                <-[rv:reviewed_by]-(book:books)
          WHERE rd.id = 1
          RETURN book.id
        );

HERO-3  SELECT rd.username, r.stars, r.review_text, _score
        FROM reviews r
        JOIN readers rd ON rd.id = r.reader_id
        JOIN (
          SELECT node_id FROM PageRank('follows')
          ORDER BY score DESC LIMIT 20
        ) AS influencers ON influencers.node_id = r.reader_id
        WHERE MATCH(r.review_text) TO 'masterpiece'
          AND r.stars >= 4
        ORDER BY _score DESC LIMIT 10;


========================================
APPENDIX A: 60-SECOND TEASER CUT (X / LINKEDIN)
========================================

Fast, no narration pauses, captions on. One screen: the web console.

0:00-0:05   Graph panel network on screen
            Caption: "One database. Graph + Vector + Full-text. Native Graph RAG."

0:05-0:15   Type and run TRAVERSE query, flip to Graph view
            Caption: "First-class graph traversals. Not a recursive CTE."

0:15-0:25   Run NEAREST description_vec TO 'survival against nature'
            Caption: "Semantic vector search. Embeddings are a column type."

0:25-0:42   Run HERO-1: NEAREST WITHIN TRAVERSE
            Caption: "Graph RAG in one query. Traverse the graph, vector search the subgraph."

0:42-0:52   Run HERO-2: MATCH + NEAREST
            Caption: "Pattern matching + variable-length paths + vector search. One statement."

0:52-1:00   Cut to terminal: docker run -p 6767:6767 sixsevendb/sixsevendb:latest
            Caption: "PostgreSQL-compatible. Try it now."


========================================
APPENDIX B: TALKING POINTS / FAQ
========================================

"Why not Postgres + pgvector + Apache AGE?"
    One engine = one copy of the data, one transaction boundary,
    one thing to operate. No ETL/sync between systems.
    Graph traversals and vector search compose natively in one query.
    You cannot do NEAREST WITHIN TRAVERSE in a Postgres extension stack.

"What is Graph RAG?"
    Retrieval-Augmented Generation where the retrieval step uses
    graph structure to scope or enrich the context window.
    Instead of naive top-K vector search, you traverse relationships
    first, then search semantically within that subgraph.
    SixSevenDB makes this a single query, not a multi-step pipeline.

"Is it really Postgres-compatible?"
    It speaks the v3 wire protocol. psql and standard Postgres
    drivers connect. Be honest about SQL surface coverage:
    the core language is there, not every pg_catalog view.

"Where do the embeddings come from?"
    A local ONNX model (all-MiniLM-L6-v2) is bundled in the image.
    Semantic search works with no external API. You can also wire
    up OpenAI/Ollama providers for other models via configuration.

"How does EMBEDDING differ from pgvector?"
    EMBEDDING is a column type that auto-generates vectors from a
    source text column. You don't manage embeddings yourself.
    INSERT a row with text, the engine embeds it, indexes it.
    UPDATE the text, the vector updates automatically.

"BM25 vs vector search: when do I use which?"
    BM25 is lexical: ranks by exact keyword matches with stemming.
    Vector search is semantic: matches meaning.
    They're complementary. Both live in one engine so you can
    combine them or use them independently in the same query.

"What graph algorithms ship built-in?"
    PageRank, betweenness centrality, closeness centrality,
    connected components. Exposed as table functions so they
    compose with standard SQL (JOIN, WHERE, ORDER BY, etc.).

"What's under the hood?"
    C++20, Volcano-style executor, buffer pool, write-ahead log,
    MVCC, B+-tree indexes, HNSW index for vectors, BM25 inverted
    index for full-text. Everything from scratch, no Postgres fork.

"Default login?"
    sixseven / sixseven over SCRAM-SHA-256.
    Tell viewers to change it for anything real.
