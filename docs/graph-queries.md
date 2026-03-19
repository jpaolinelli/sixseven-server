# Graph Queries

## Edge Types

Define relationships between tables:

```sql
CREATE EDGE TYPE follows FROM users TO users;
CREATE EDGE TYPE authored FROM users TO posts;
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;

DROP EDGE TYPE follows;
```

## Link and Unlink

Create and remove edges between rows:

```sql
LINK users(1) TO users(2) VIA follows;
LINK users(1) TO products(10) VIA rated (score=4.5, review='Excellent!');

UNLINK users(1) FROM users(2) VIA follows;
UNLINK users(1) FROM products(1) VIA rated WHERE score < 2.0;
```

## Traverse (BFS)

Walk edges from a starting node using breadth-first search:

```sql
-- All outgoing follows from user 1
TRAVERSE follows FROM users(1) DIRECTION OUT;

-- Incoming edges (who follows user 1?)
TRAVERSE follows FROM users(1) DIRECTION IN;

-- Both directions, limited depth, with row data
TRAVERSE follows FROM users(1)
    DIRECTION BOTH
    MAX_DEPTH 3
    FETCH;

-- Filter by traversal metadata
TRAVERSE follows FROM users(1)
    DIRECTION OUT
    WHERE __depth > 1
    FETCH;
```

**Direction options:** `OUT` (default), `IN`, `BOTH`. `BOTH` is only supported on edges between the same table (homogeneous edges).

### TRAVERSE in SELECT

TRAVERSE can be used as a FROM source in SELECT queries with full projection, filtering, aliasing, and join support:

```sql
-- Projection and ordering
SELECT name, __depth
FROM TRAVERSE follows FROM users(1) DIRECTION OUT FETCH AS t
ORDER BY name
LIMIT 10;

-- Join traverse results with other tables
SELECT u.name, p.title
FROM users u
JOIN TRAVERSE authored FROM users(1) DIRECTION OUT AS posts_t
  ON u.id = posts_t.__source;
```

### Output Modes

**MODE NODES** (default) — one row per discovered node:

| Column | Description |
|--------|-------------|
| *target table columns* | Full row data from the discovered node (requires `FETCH`) |
| `__node` | Primary key of the discovered node |
| `__depth` | BFS depth from the starting node |
| `__source` | PK of the previous node in the BFS tree (NULL for depth-0) |
| *edge properties* | Properties from the edge used to reach this node |

**MODE EDGES** — one row per edge, including cross-edges and bidirectional edges (useful for graph visualization):

| Column | Description |
|--------|-------------|
| `__from` | Source node primary key |
| `__to` | Target node primary key |
| `__depth` | Depth of the target node in the BFS tree |
| *edge properties* | Properties of the edge |

```sql
-- Node-centric output (default)
TRAVERSE follows FROM users(1) DIRECTION OUT MODE NODES FETCH;

-- Edge-centric output for graph rendering
TRAVERSE follows FROM users(1) DIRECTION OUT MODE EDGES;
```

### Edge Properties

Edge types can carry typed properties. These appear as additional columns in traverse output:

```sql
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;
LINK users(1) TO products(10) VIA rated (score=4.5, review='Excellent!');

-- Access edge properties (qualified or unqualified)
SELECT name, rated.score, rated.review
FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH AS t;

-- Star expansion includes edge properties
SELECT * FROM TRAVERSE rated FROM users(1) DIRECTION OUT FETCH;
-- Returns: [product columns...], __node, __depth, __source, score, review
```

### Heterogeneous Edge Traversal

Edges connecting different tables (e.g., `users → posts`) expose the opposite endpoint's columns:

```sql
CREATE EDGE TYPE authored FROM users TO posts;

-- OUT from source → target table columns (posts)
SELECT title FROM TRAVERSE authored FROM users(1) DIRECTION OUT FETCH;

-- IN from target → source table columns (users)
SELECT name FROM TRAVERSE authored FROM posts(10) DIRECTION IN FETCH;
```

## Match (Pattern Matching)

Cypher-style pattern matching over graph edges. Use `SELECT ... FROM MATCH` to compose with standard SQL clauses (WHERE, ORDER BY, LIMIT, OFFSET, DISTINCT):

```sql
-- Direct follow relationships
SELECT a.name, b.name
FROM MATCH (a:users)-[e:follows]->(b:users);

-- Incoming edges (who follows user a?)
SELECT a.name
FROM MATCH (a:users)<-[e:follows]-(b:users);

-- Multi-hop: users who follow someone who authored a post
SELECT b.name, p.title
FROM MATCH (a:users)-[f:follows]->(b:users)-[w:authored]->(p:posts)
WHERE a.name = 'Alice';

-- Undirected edges
SELECT a.name, b.name
FROM MATCH (a:users)-[e:follows]-(b:users);

-- Full SQL composability
SELECT DISTINCT a.name, b.name
FROM MATCH (a:users)-[e:follows]->(b:users)
WHERE a.id > 0
ORDER BY a.name DESC
LIMIT 5 OFFSET 2;
```

**Backward compatibility:** The original `MATCH ... RETURN` syntax still works:

```sql
MATCH (a:users)-[r:follows]->(b:users)
RETURN a.name, b.name;
```

### Variable-Length Path Patterns

Add quantifiers to edges to match paths of variable length:

```sql
-- Range: 1 to 6 hops
SELECT a.name, b.name
FROM MATCH (a:users)-[r:knows]->{1,6}(b:users);

-- Exact: exactly 3 hops
SELECT a.name
FROM MATCH (a:users)-[r:knows]->{3}(b:users);

-- One or more hops (+)
SELECT a.name
FROM MATCH (a:users)-[r:knows]->+(b:users);

-- Zero or more hops (*)
SELECT a.name
FROM MATCH (a:users)-[r:knows]->*(b:users);

-- Incoming direction with quantifier
SELECT a.name
FROM MATCH (a:users)<-[r:knows]-{1,3}(b:users);
```

| Quantifier | Meaning |
|------------|---------|
| `{min,max}` | Between min and max hops |
| `{n}` | Exactly n hops |
| `+` | One or more hops (shorthand for `{1,}`) |
| `*` | Zero or more hops (shorthand for `{0,}`) |

### Inline Predicate Filtering

Filter nodes and edges directly inside the pattern, without a separate WHERE clause:

```sql
-- Filter target nodes inline
SELECT a.name
FROM MATCH (a:users)-[r:knows]->(b:users WHERE b.active = TRUE);

-- Filter edges inline
SELECT a.name
FROM MATCH (a:users)-[r:knows WHERE r.since > '2020-01-01']->(b:users);

-- Combine edge and node filters with variable-length paths
SELECT a.name, b.name
FROM MATCH (a:users)-[r:knows WHERE r.since > '2020']->{1,6}(b:users WHERE b.active = TRUE);
```

Inline predicates are applied during traversal, pruning branches early for better performance on large graphs.

### Path Selectors (Shortest Path in MATCH)

Find shortest paths using path selectors in MATCH patterns. Requires a variable-length edge:

```sql
-- Any single shortest path
SELECT a.name, b.name
FROM MATCH p = ANY SHORTEST (a:users)-[r:knows]->{1,10}(b:users)
WHERE a.id = 1;

-- All shortest paths (same minimum length)
MATCH p = ALL SHORTEST (a:users)-[r:knows]->{1,10}(b:users)
RETURN a.name, b.name;

-- K shortest paths
MATCH p = SHORTEST 3 (a:users)-[r:knows]->{1,10}(b:users)
RETURN a.name, b.name;
```

| Selector | Returns |
|----------|---------|
| `ANY SHORTEST` | One arbitrary shortest path |
| `ALL SHORTEST` | All paths tied for minimum length |
| `SHORTEST K` | The K shortest paths |

### Weighted Shortest Path

Add a `WEIGHT` clause to use edge properties as costs (Dijkstra's algorithm):

```sql
-- Shortest path by distance (weighted)
SELECT a.name, b.name
FROM MATCH p = ANY SHORTEST (a:cities)-[r:road]->{1,20}(b:cities)
WEIGHT r.distance;

-- All shortest weighted paths
MATCH p = ALL SHORTEST (a:cities)-[r:road]->{1,20}(b:cities)
WEIGHT r.distance
RETURN a.name, b.name;
```

The `WEIGHT` clause is only valid with a path selector. Without `WEIGHT`, shortest path uses hop count (BFS).

## Shortest Path (Legacy Syntax)

The standalone `SHORTEST PATH` syntax is still supported for simple shortest-path queries:

```sql
SHORTEST PATH FROM users(1) TO users(100) VIA follows;

SHORTEST PATH FROM users(1) TO users(100)
    VIA follows
    DIRECTION OUT
    MAX_DEPTH 10;
```

For more advanced use cases (weighted paths, all shortest paths, K shortest), use path selectors in MATCH (see above).
