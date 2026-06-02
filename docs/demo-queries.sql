-- ============================================================================
-- SixSevenDB Demo Queries
-- Run against the auto-generated "SixSeven Bookstore" demo database.
--
-- Connect:  psql -h localhost -p 6767 -U sixseven
-- ============================================================================


-- ============================================================================
-- 1. RELATIONAL BASICS — Standard SQL, familiar to everyone
-- ============================================================================

-- Browse the bookstore
SELECT * FROM books LIMIT 10;

-- Filter + sort — find top-rated Thrillers
SELECT title, rating, published_year, pages
FROM books
WHERE genre like 'Thriller'
ORDER BY rating DESC
LIMIT 10;

-- Aggregations — average rating by genre
SELECT genre, COUNT(*) AS book_count, AVG(rating) AS avg_rating
FROM books
GROUP BY genre
ORDER BY avg_rating DESC;

-- Readers by city
SELECT city, COUNT(*) AS readers
FROM readers
GROUP BY city
ORDER BY readers DESC
LIMIT 10;

-- Star distribution across all reviews
SELECT stars, COUNT(*) AS review_count
FROM reviews
GROUP BY stars
ORDER BY stars;


-- ============================================================================
-- 2. JOINS — Relational power, combining tables
-- ============================================================================

-- What are readers in Tokyo reviewing?
SELECT b.title, b.genre, r.stars, rd.username
FROM reviews r
JOIN books b ON b.id = r.book_id
JOIN readers rd ON rd.id = r.reader_id
WHERE rd.city = 'Tokyo'
ORDER BY r.stars DESC
LIMIT 10;

-- Most prolific reviewers
SELECT rd.username, rd.city, COUNT(*) AS reviews_written, AVG(r.stars) AS avg_stars
FROM reviews r
JOIN readers rd ON rd.id = r.reader_id
GROUP BY rd.id, rd.username, rd.city
ORDER BY reviews_written DESC
LIMIT 10;

-- Highest-rated books with review counts
SELECT b.title, b.genre, b.rating,
       COUNT(r.id) AS num_reviews, AVG(r.stars) AS avg_review_stars
FROM books b
JOIN reviews r ON r.book_id = b.id
GROUP BY b.id, b.title, b.genre, b.rating
HAVING COUNT(r.id) >= 3
ORDER BY avg_review_stars DESC
LIMIT 10;


-- ============================================================================
-- 3. GRAPH TRAVERSAL — Follow relationships through the network
-- ============================================================================

-- Who does reader 1 follow? (direct connections)
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 1;

-- Social network: 3 hops from reader 1 (friend-of-friend-of-friend)
SELECT username, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3;

-- Who follows reader 42? (inbound edges)
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(42) DIRECTION IN MAX_DEPTH 1;

-- Bidirectional: reader 10's full social circle within 2 hops
SELECT username, __depth
FROM TRAVERSE follows FROM readers(10) DIRECTION BOTH MAX_DEPTH 2;

-- Filter during traversal: only find readers in "London" within 3 hops
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 3
WHERE city = 'London';


-- ============================================================================
-- 4. GRAPH ALGORITHMS — Analytics on the social network
-- ============================================================================

-- PageRank: who are the most influential readers?
SELECT node_id, score
FROM PageRank('follows')
ORDER BY score DESC
LIMIT 10;

-- PageRank with custom damping factor
SELECT node_id, score
FROM PageRank('follows', damping := 0.9, iterations := 30)
ORDER BY score DESC
LIMIT 10;

-- Connected components: are there isolated reader communities?
SELECT component_id, COUNT(*) AS community_size
FROM connected_components('follows')
GROUP BY component_id
ORDER BY community_size DESC
LIMIT 10;

-- Betweenness centrality: who bridges reader communities?
SELECT node_id, centrality
FROM betweenness('follows')
ORDER BY centrality DESC
LIMIT 10;

-- Closeness centrality: who can reach everyone fastest?
SELECT node_id, closeness, reachable_count
FROM closeness('follows')
ORDER BY closeness DESC
LIMIT 10;


-- ============================================================================
-- 5. VECTOR SEARCH — Semantic similarity powered by ONNX embeddings
-- ============================================================================

-- Find books about "time travel adventure" (natural language query)
NEAREST 5 FROM books.description_vec TO 'time travel adventure';

-- Find books about "political drama in a kingdom"
NEAREST 5 FROM books.description_vec TO 'political drama in a kingdom';

-- Find books about "AI and technology in the future"
NEAREST 5 FROM books.description_vec TO 'artificial intelligence and technology';

-- Find reviews that talk about "beautiful writing style"
NEAREST 5 FROM reviews.review_vec TO 'beautiful prose and writing style';

-- Find reviews mentioning "disappointing ending"
NEAREST 5 FROM reviews.review_vec TO 'the ending was disappointing';

-- Cosine similarity search (explicit distance metric)
NEAREST 10 FROM books.description_vec TO 'survival story on a remote island' USING COSINE;


-- ============================================================================
-- 6. EDGE OPERATIONS — Create and manage relationships
-- ============================================================================

-- Add a new follow relationship
LINK readers(1) TO readers(100) VIA follows;

-- Verify it worked — reader 100 should now appear
SELECT username, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 1
WHERE username LIKE '%100';

-- Remove the relationship
UNLINK readers(1) FROM readers(100) VIA follows;


-- ============================================================================
-- 7. EXPLAIN — See the query execution plan
-- ============================================================================

-- Understand how a filter query is executed
EXPLAIN SELECT * FROM books WHERE genre = 'Mystery';

-- See actual execution stats (rows processed, timing)
EXPLAIN ANALYZE SELECT * FROM books WHERE rating > 4.0;

-- Explain a join query
EXPLAIN ANALYZE
SELECT b.title, r.stars
FROM books b
JOIN reviews r ON r.book_id = b.id
WHERE b.genre = 'Fantasy'
ORDER BY r.stars DESC
LIMIT 5;

-- Explain a vector search
EXPLAIN ANALYZE NEAREST 5 FROM books.description_vec TO 'epic space adventure';


-- ============================================================================
-- 8. DDL — Schema management
-- ============================================================================

-- See all tables
SHOW TABLES;

-- See table structure
SHOW COLUMNS FROM books;
SHOW COLUMNS FROM reviews;

-- See edge types
SHOW EDGE TYPES;

-- See indexes
SHOW INDEXES;

-- See embedding configuration
SHOW EMBEDDING;


-- ============================================================================
-- 9. COMBINED QUERIES — The real power: relational + graph + vector together
-- ============================================================================

-- Find semantically similar books, then check their ratings
-- "Which highly-rated books are similar to 'survival in a harsh environment'?"
NEAREST 10 FROM books.description_vec TO 'survival in a harsh environment';
-- Then manually cross-reference with ratings:
SELECT title, genre, rating, description
FROM books
WHERE rating > 4.0
ORDER BY rating DESC;

-- Who does reader 1 follow, and what did they review?
-- Step 1: see reader 1's friends
SELECT username, city, __depth
FROM TRAVERSE follows FROM readers(1) DIRECTION OUT MAX_DEPTH 1;

-- Step 2: pick a friend and see their reviews
-- (use a reader ID from the traversal above, e.g. reader 2)
SELECT b.title, b.genre, r.stars, r.review_text
FROM reviews r
JOIN books b ON b.id = r.book_id
WHERE r.reader_id = 2
ORDER BY r.stars DESC
LIMIT 5;


-- ============================================================================
-- 10. DATA EXPLORATION — Quick stats about the demo dataset
-- ============================================================================

-- Dataset overview
SELECT 'authors' AS table_name, COUNT(*) AS row_count FROM authors
UNION ALL
SELECT 'books', COUNT(*) FROM books
UNION ALL
SELECT 'readers', COUNT(*) FROM readers
UNION ALL
SELECT 'reviews', COUNT(*) FROM reviews
UNION ALL
SELECT 'tags', COUNT(*) FROM tags;

-- Genre breakdown
SELECT genre, COUNT(*) AS books, AVG(rating) AS avg_rating,
       MIN(published_year) AS earliest, MAX(published_year) AS latest
FROM books
GROUP BY genre
ORDER BY books DESC;

-- Reader geography
SELECT city, COUNT(*) AS readers,
       AVG(joined_year) AS avg_join_year
FROM readers
GROUP BY city
ORDER BY readers DESC;
