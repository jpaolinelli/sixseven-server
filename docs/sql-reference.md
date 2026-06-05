# SQL Reference

## Data Types

| Type | Description |
|------|-------------|
| `TINYINT`, `SMALLINT`, `INT`/`INTEGER`, `BIGINT` | Signed integers (8, 16, 32, 64-bit) |
| `FLOAT`, `DOUBLE` | IEEE floating point (32, 64-bit) |
| `DECIMAL(p, s)`, `NUMERIC(p, s)` | Fixed-point decimal |
| `BOOLEAN` | Boolean |
| `CHAR(n)`, `VARCHAR(n)`, `TEXT` | Text |
| `BLOB` | Binary data |
| `DATE`, `TIME`, `TIMESTAMP`, `INTERVAL` | Temporal types |
| `POINT` | 2D spatial point |
| `JSON` | JSON documents |
| `UUID` | Universally unique identifier |
| `EMBEDDING(dim, source, provider)` | Vector embedding (auto-generated) |

## Databases

```sql
CREATE DATABASE analytics;
CREATE DATABASE analytics IF NOT EXISTS;
DROP DATABASE analytics;
DROP DATABASE analytics IF EXISTS CASCADE;
```

## Tables

```sql
CREATE TABLE users (
    id UUID PRIMARY KEY DEFAULT gen_uuid(),
    name TEXT NOT NULL,
    age INT CHECK (age > 0),
    email VARCHAR(255) UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    deleted_at TIMESTAMP
);

ALTER TABLE users ADD COLUMN bio TEXT;
ALTER TABLE users DROP COLUMN bio;
ALTER TABLE users RENAME COLUMN email TO email_address;

DROP TABLE users;
DROP TABLE users IF EXISTS CASCADE;
```

## Indexes

```sql
CREATE INDEX idx_email ON users(email);
CREATE UNIQUE INDEX idx_username ON users(username);
CREATE INDEX idx_compound ON users(department, salary);
CREATE INDEX idx_hash ON users(id) USING HASH;

DROP INDEX idx_email;
```

## Insert, Update, Delete

```sql
INSERT INTO users (name, age) VALUES ('Alice', 30), ('Bob', 25);

INSERT INTO users (name, age)
SELECT name, age FROM temp_users WHERE status = 'active';

UPDATE users SET age = 31 WHERE name = 'Alice';

DELETE FROM users WHERE age < 18;
```

## Queries

```sql
-- Basic select with filtering and sorting
SELECT id, name, age FROM users
WHERE age > 25
ORDER BY name ASC
LIMIT 10 OFFSET 20;

-- Aggregates and grouping
SELECT department, COUNT(*) AS cnt, AVG(salary) AS avg_sal
FROM employees
GROUP BY department
HAVING COUNT(*) > 5
ORDER BY avg_sal DESC;

-- Joins (INNER, LEFT, RIGHT, FULL, CROSS)
SELECT u.name, p.title
FROM users u
INNER JOIN posts p ON u.id = p.author_id;

SELECT u.name, COUNT(o.id) AS orders
FROM users u
LEFT JOIN orders o ON u.id = o.user_id
GROUP BY u.name;

-- Subqueries
SELECT name FROM users
WHERE id IN (SELECT user_id FROM posts WHERE published = true);

SELECT name FROM users
WHERE EXISTS (SELECT 1 FROM posts WHERE posts.user_id = users.id);

-- CTEs (Common Table Expressions)
WITH active_users AS (
    SELECT id, name FROM users WHERE status = 'active'
),
user_posts AS (
    SELECT user_id, COUNT(*) AS post_count FROM posts GROUP BY user_id
)
SELECT u.name, COALESCE(p.post_count, 0) AS posts
FROM active_users u
LEFT JOIN user_posts p ON u.id = p.user_id;

-- Window functions
SELECT name, salary,
       ROW_NUMBER() OVER (PARTITION BY department ORDER BY salary DESC) AS rank,
       LAG(salary) OVER (ORDER BY salary) AS prev_salary,
       SUM(salary) OVER (PARTITION BY department) AS dept_total
FROM employees;

-- Set operations
SELECT name FROM users WHERE age > 30
UNION
SELECT name FROM contractors WHERE active = true;

SELECT id FROM table1 INTERSECT SELECT id FROM table2;
SELECT id FROM table1 EXCEPT SELECT id FROM table2;

-- Type casting
SELECT price::INT FROM products;
SELECT CAST(created_at AS DATE) FROM events;

-- CASE expressions
SELECT name,
    CASE
        WHEN age < 18 THEN 'minor'
        WHEN age < 65 THEN 'adult'
        ELSE 'senior'
    END AS category
FROM users;
```

## Graph Subqueries

`TRAVERSE` (graph) and `MATCH` (graph pattern) are first-class query statements:
anywhere a `SELECT` subquery is allowed, they may be used too. This is what lets
relational and graph queries blend in a single SQL statement.

Supported positions: derived tables (`FROM (...)`), `IN (...)`, `EXISTS (...)`, and
scalar subqueries. The subquery produces a normal row stream — `TRAVERSE` yields
`node`, `depth`, and (with `FETCH`) `source`; `MATCH` yields its `RETURN` columns.

> **Vector search is not a subquery.** `NEAREST` is a `WHERE` predicate on a single
> table (`SELECT ... FROM t WHERE NEAREST(col, k) TO ...`), not a statement, so it
> cannot appear as a derived table, in `IN (...)`, or in `EXISTS (...)`. See the
> [Vector Search Guide](./vector-search.md) for the predicate form.

```sql
-- Derived tables: treat a graph result like a table.
SELECT * FROM (TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 2) AS t;

-- IN / EXISTS: filter relational rows by a graph result.
SELECT name FROM users
WHERE EXISTS (TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 2);

SELECT body FROM docs
WHERE id IN (MATCH (a:docs)-[r:refs]->(b:docs) RETURN b.id);

-- Vector search uses the NEAREST predicate instead of a subquery:
SELECT *, _distance FROM docs
WHERE NEAREST(body_vec, 5) TO [0.1, 0.2, 0.3, 0.4]
ORDER BY _distance;
```

### Correlated subqueries

A nested `TRAVERSE` may reference a column from the enclosing query (a *correlated*
subquery). The subquery is re-evaluated per outer row, with the outer value substituted
into the start key:

```sql
-- Keep only users who follow at least one other user.
SELECT u.name FROM users u
WHERE EXISTS (TRAVERSE follows FROM users(u.id) DIRECTION OUT MAX_DEPTH 1);

-- Correlated MATCH: anchor the pattern to the outer row via its WHERE clause.
SELECT u.name FROM users u
WHERE EXISTS (
    MATCH (a:users)-[r:follows]->(b:users) WHERE a.id = u.id RETURN b.id);
```

**Notes / limitations.** A scalar subquery must return exactly one column, so a
multi-column `TRAVERSE` is rejected there (wrap it in a single-column `SELECT`).
Correlation is supported for `TRAVERSE` (start key) and `MATCH` (its `WHERE` clause);
outer columns referenced in inline node / edge pattern filters of a `MATCH` are not yet
supported — put the correlation in the `WHERE` clause.

## Aggregate Functions

`COUNT(*)`, `COUNT(col)`, `COUNT(DISTINCT col)`, `SUM`, `AVG`, `MIN`, `MAX`, `STRING_AGG`

## Window Functions

**Ranking:** `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `NTILE(n)`

**Offset:** `LAG(col, offset, default)`, `LEAD(col, offset, default)`, `FIRST_VALUE(col)`, `LAST_VALUE(col)`

**Aggregate:** `SUM`, `AVG`, `COUNT`, `MIN`, `MAX` with `OVER (PARTITION BY ... ORDER BY ... ROWS/RANGE BETWEEN ...)`

## Transactions (Not Yet Implemented)

Transaction support (`BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `ROLLBACK TO`) is planned but not yet functional.

```sql
-- Planned syntax (not yet available):
BEGIN;
SAVEPOINT sp1;
-- ... queries ...
ROLLBACK TO sp1;
COMMIT;

BEGIN;
-- ... queries ...
ROLLBACK;
```
