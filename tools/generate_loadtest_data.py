#!/usr/bin/env python3
"""
SixSevenDB Load Test Data Generator

Generates millions of rows and graph edges for query load testing.
Outputs pure schema + data SQL (INSERT + LINK) — no queries or admin commands.

Uses streaming generators so memory stays flat regardless of scale.

Usage:
    python tools/generate_loadtest_data.py --scale stress   > tools/loadtest_stress.sql
    python tools/generate_loadtest_data.py --scale massive  | psql -h localhost -p 6767
    python tools/generate_loadtest_data.py --scale extreme  | psql -h localhost -p 6767

    # Skip EMBEDDING columns for faster loading:
    python tools/generate_loadtest_data.py --scale stress --skip-embeddings > out.sql

Requires no external Python dependencies (stdlib only).
"""

import argparse
import io
import json
import random
import sys
import uuid

# ---------------------------------------------------------------------------
# Word lists for realistic data generation
# ---------------------------------------------------------------------------

FIRST_NAMES = [
    "Alice", "Bob", "Charlie", "Diana", "Eve", "Frank", "Grace", "Hank",
    "Ivy", "Jack", "Karen", "Leo", "Mia", "Nathan", "Olivia", "Paul",
    "Quinn", "Rachel", "Sam", "Tina", "Uma", "Victor", "Wendy", "Xander",
    "Yuki", "Zoe", "Aaron", "Bella", "Caleb", "Daisy", "Ethan", "Fiona",
    "Gavin", "Hannah", "Isaac", "Julia", "Kyle", "Luna", "Mason", "Nina",
    "Oscar", "Piper", "Reed", "Sofia", "Tyler", "Ursula", "Vera", "Wade",
    "Ximena", "Yosef", "Zara", "Adrian", "Brooke", "Carter", "Delilah",
    "Eli", "Freya", "Grant", "Hazel", "Ivan", "Jade", "Knox", "Lydia",
    "Miles", "Nora", "Owen", "Paige", "Rhys", "Stella", "Theo", "Uma",
    "Violet", "Wyatt", "Xena", "Yasmine", "Zeke", "Aria", "Blake", "Cora",
    "Dante", "Elise", "Felix", "Gemma", "Hugo", "Isla", "Jude", "Kira",
    "Liam", "Maya", "Nolan", "Opal", "Percy", "Rosa", "Silas", "Tessa",
    "Ulric", "Vivian", "Wren", "Xiomara", "Yara", "Zion",
]

LAST_NAMES = [
    "Smith", "Johnson", "Williams", "Brown", "Jones", "Garcia", "Miller",
    "Davis", "Rodriguez", "Martinez", "Hernandez", "Lopez", "Gonzalez",
    "Wilson", "Anderson", "Thomas", "Taylor", "Moore", "Jackson", "Martin",
    "Lee", "Perez", "Thompson", "White", "Harris", "Sanchez", "Clark",
    "Ramirez", "Lewis", "Robinson", "Walker", "Young", "Allen", "King",
    "Wright", "Scott", "Torres", "Nguyen", "Hill", "Flores", "Green",
    "Adams", "Nelson", "Baker", "Hall", "Rivera", "Campbell", "Mitchell",
    "Carter", "Roberts", "Gomez", "Phillips", "Evans", "Turner", "Diaz",
    "Parker", "Cruz", "Edwards", "Collins", "Reyes", "Stewart", "Morris",
    "Morales", "Murphy", "Cook", "Rogers", "Gutierrez", "Ortiz", "Morgan",
    "Cooper", "Peterson", "Bailey", "Reed", "Kelly", "Howard", "Ramos",
    "Kim", "Cox", "Ward", "Richardson", "Watson", "Brooks", "Chavez",
    "Wood", "James", "Bennett", "Gray", "Mendoza", "Ruiz", "Hughes",
    "Price", "Alvarez", "Castillo", "Sanders", "Patel", "Myers", "Long",
    "Ross", "Foster", "Jimenez",
]

EMAIL_DOMAINS = [
    "example.com", "test.org", "demo.io", "sample.net", "mail.dev",
    "corp.co", "acme.com", "globex.org", "initech.io", "umbrella.net",
]

DEPARTMENTS = [
    ("Engineering", 2500000), ("Product", 1800000), ("Design", 1200000),
    ("Marketing", 1500000), ("Sales", 2000000), ("Support", 900000),
    ("Data Science", 1600000), ("DevOps", 1100000), ("Security", 1300000),
    ("Legal", 800000), ("Finance", 950000), ("HR", 750000),
    ("Research", 2100000), ("QA", 1000000), ("Operations", 1400000),
    ("Analytics", 1350000), ("Platform", 1700000), ("Infrastructure", 1550000),
    ("Mobile", 1250000), ("AI/ML", 2200000),
]

JOB_TITLES = [
    "Software Engineer", "Senior Engineer", "Staff Engineer",
    "Principal Engineer", "Engineering Manager", "Director of Engineering",
    "Product Manager", "Senior PM", "Designer", "UX Researcher",
    "Data Scientist", "ML Engineer", "DevOps Engineer", "SRE",
    "Security Engineer", "QA Engineer", "Technical Writer", "Analyst",
    "Account Executive", "Solutions Architect",
]

CITIES = [
    ("San Francisco", 37.7749, -122.4194), ("New York", 40.7128, -74.0060),
    ("London", 51.5074, -0.1278), ("Tokyo", 35.6762, 139.6503),
    ("Berlin", 52.5200, 13.4050), ("Paris", 48.8566, 2.3522),
    ("Sydney", -33.8688, 151.2093), ("Toronto", 43.6532, -79.3832),
    ("Singapore", 1.3521, 103.8198), ("Amsterdam", 52.3676, 4.9041),
    ("Seoul", 37.5665, 126.9780), ("Mumbai", 19.0760, 72.8777),
    ("Austin", 30.2672, -97.7431), ("Seattle", 47.6062, -122.3321),
    ("Denver", 39.7392, -104.9903), ("Chicago", 41.8781, -87.6298),
    ("Boston", 42.3601, -71.0589), ("Portland", 45.5152, -122.6784),
    ("Dublin", 53.3498, -6.2603), ("Zurich", 47.3769, 8.5417),
]

PRODUCT_CATEGORIES = [
    "Electronics", "Software", "Hardware", "Networking", "Storage",
    "Security", "Cloud", "Analytics", "AI/ML", "DevTools",
]

PRODUCT_ADJECTIVES = [
    "Ultra", "Pro", "Enterprise", "Quantum", "Hyper", "Nano", "Smart",
    "Turbo", "Prime", "Elite", "Advanced", "Rapid", "Precision", "Dynamic",
]

PRODUCT_NOUNS = [
    "Router", "Switch", "Firewall", "Server", "Monitor", "Keyboard",
    "Controller", "Adapter", "Scanner", "Sensor", "Gateway", "Module",
    "Processor", "Amplifier", "Converter", "Optimizer", "Accelerator",
]

PRODUCT_SUFFIXES = ["X", "Z", "S", "Pro", "Max", "Plus"]

POST_TOPICS = [
    "distributed systems", "machine learning", "database internals",
    "graph algorithms", "vector search", "cloud architecture",
    "microservices", "event sourcing", "stream processing",
    "data engineering", "compiler design", "type systems",
    "concurrency patterns", "memory management", "network protocols",
    "cryptography", "observability", "performance tuning",
    "API design", "testing strategies", "DevOps practices",
]

POST_VERBS = [
    "Understanding", "Building", "Scaling", "Optimizing", "Debugging",
    "Designing", "Implementing", "Exploring", "Benchmarking", "Migrating",
    "Deploying", "Monitoring", "Securing", "Refactoring", "Automating",
]

POST_BODY_TEMPLATES = [
    "In this post we explore the fundamental concepts behind {topic}. "
    "Modern systems require careful consideration of trade-offs.",
    "A deep dive into {topic} reveals surprising complexity beneath "
    "the surface. We cover core algorithms and design patterns.",
    "The landscape of {topic} has evolved dramatically. New techniques "
    "challenge conventional wisdom.",
    "Performance matters when working with {topic}. We identified key "
    "bottlenecks and developed optimization strategies.",
    "Getting started with {topic} can be overwhelming. This guide walks "
    "through the essential concepts and best practices.",
    "Lessons learned from running {topic} at scale in production over "
    "eighteen months of operating a high-throughput system.",
]

EVENT_NAMES = [
    "Tech Summit", "Developer Conference", "Product Launch",
    "Architecture Review", "Sprint Planning", "Retrospective",
    "Design Workshop", "Security Audit", "Performance Review",
    "Team Standup", "Board Meeting", "Hackathon",
    "Training Session", "Webinar", "Customer Workshop",
]

REVIEW_TEMPLATES = [
    "Excellent product, exactly what we needed for our {use}.",
    "Good quality. Works well for {use}.",
    "Outstanding performance. Transformed our {use} workflow.",
    "Decent value for the price. Adequate for basic {use}.",
    "Best in class for {use}. Highly recommended.",
    "Solid product. Great for {use}.",
    "Game changer for our team. Essential for modern {use}.",
    "Works as advertised. Great for {use} at scale.",
]

USE_CASES = [
    "data processing", "network monitoring", "security operations",
    "cloud migration", "development", "testing", "deployment",
    "analytics", "machine learning", "edge computing",
]

TAG_PREFIXES = ["tech", "data", "cloud", "ai", "web", "mobile", "infra", "sec", "ops", "dev"]
TAG_SUFFIXES = ["core", "pro", "beta", "alpha", "next", "edge", "hub", "lab", "kit", "flow"]


# ---------------------------------------------------------------------------
# Scale configuration
# ---------------------------------------------------------------------------

SCALES = {
    "stress": {
        "departments": 200,
        "users":       1_000_000,
        "posts":       1_000_000,
        "products":    500_000,
        "orders":      2_000_000,
        "events":      500_000,
        "documents":   200_000,
        "tags":        100_000,
        "follows":     1_000_000,
        "authored":    1_000_000,
        "rated":       1_000_000,
        "reports_to":  500_000,
    },
    "massive": {
        "departments": 500,
        "users":       5_000_000,
        "posts":       5_000_000,
        "products":    2_500_000,
        "orders":      10_000_000,
        "events":      2_500_000,
        "documents":   1_000_000,
        "tags":        500_000,
        "follows":     5_000_000,
        "authored":    5_000_000,
        "rated":       5_000_000,
        "reports_to":  2_500_000,
    },
    "extreme": {
        "departments": 1_000,
        "users":       10_000_000,
        "posts":       10_000_000,
        "products":    5_000_000,
        "orders":      20_000_000,
        "events":      5_000_000,
        "documents":   2_000_000,
        "tags":        1_000_000,
        "follows":     10_000_000,
        "authored":    10_000_000,
        "rated":       10_000_000,
        "reports_to":  5_000_000,
    },
}

ONNX_PROVIDER = "onnx/models/all-MiniLM-L6-v2"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def sql_escape(s: str) -> str:
    return s.replace("'", "''").replace("\\", "\\\\")


def user_uuid(index: int) -> str:
    """Deterministic UUID from row index via LCG — no list needed."""
    val = (index * 6364136223846793005 + 1442695040888963407) & ((1 << 128) - 1)
    return str(uuid.UUID(int=val))


_out = None

def emit(line: str):
    _out.write(line)
    _out.write("\n")


def emit_comment(text: str):
    emit(f"\n-- {'=' * 70}")
    emit(f"-- {text}")
    emit(f"-- {'=' * 70}\n")


def progress(msg: str):
    sys.stderr.write(f"[loadtest] {msg}\n")
    sys.stderr.flush()


def emit_batch_insert_streaming(table: str, columns: list[str], row_gen, batch_size: int):
    """Emit INSERT statements by pulling from a generator in chunks."""
    col_list = ", ".join(columns)
    batch = []
    for row in row_gen:
        batch.append(row)
        if len(batch) >= batch_size:
            _flush_batch(table, col_list, batch)
            batch = []
    if batch:
        _flush_batch(table, col_list, batch)


def _flush_batch(table: str, col_list: str, batch: list):
    emit(f"INSERT INTO {table} ({col_list}) VALUES")
    last = len(batch) - 1
    for j, row in enumerate(batch):
        vals = ", ".join(row)
        sep = "," if j < last else ";"
        emit(f"  ({vals}){sep}")
    emit("")


def emit_links_streaming(link_gen, label: str, count: int, batch_size: int):
    """Emit LINK statements from a generator, with progress every batch_size."""
    emit(f"-- {label} ({count:,})")
    emitted = 0
    for stmt in link_gen:
        emit(stmt)
        emitted += 1
        if emitted % batch_size == 0:
            progress(f"  {label}: {emitted:,}/{count:,}")
    emit("")


# ---------------------------------------------------------------------------
# Streaming data generators (yield row tuples as list[str])
# ---------------------------------------------------------------------------

def gen_departments(n: int):
    base = DEPARTMENTS
    for i in range(1, n + 1):
        idx = (i - 1) % len(base)
        name, budget = base[idx]
        suffix = f" {i}" if i > len(base) else ""
        meta = json.dumps({"tier": f"t{i % 5}", "region": CITIES[i % len(CITIES)][0]})
        yield [
            str(i),
            f"'{sql_escape(name + suffix)}'",
            str(budget + (i * 137 % 400000) - 200000),
            f"'{sql_escape(meta)}'",
        ]


def gen_users(n: int, num_depts: int):
    nf = len(FIRST_NAMES)
    nl = len(LAST_NAMES)
    nd = len(EMAIL_DOMAINS)
    nc = len(CITIES)
    nj = len(JOB_TITLES)
    for i in range(1, n + 1):
        first = FIRST_NAMES[i % nf]
        last = LAST_NAMES[i % nl]
        domain = EMAIL_DOMAINS[i % nd]
        email = f"user{i}@{domain}"
        age = 18 + (i * 31) % 58
        city = CITIES[i % nc][0]
        title = JOB_TITLES[i % nj]
        active = "TRUE" if i % 4 != 0 else "FALSE"
        birth_year = 1950 + (i * 17) % 55
        birth_month = (i % 12) + 1
        birth_day = (i % 28) + 1
        dept_id = (i % num_depts) + 1
        yield [
            f"'{user_uuid(i)}'",
            f"'{sql_escape(first + ' ' + last)}'",
            f"'{email}'",
            str(age),
            f"'{sql_escape(first + ' is a ' + title + ' based in ' + city + '.')}'",
            active,
            f"'{birth_year}-{birth_month:02d}-{birth_day:02d}'",
            str(dept_id),
        ]


def gen_posts(n: int, num_users: int):
    nt = len(POST_TOPICS)
    nv = len(POST_VERBS)
    nb = len(POST_BODY_TEMPLATES)
    tags_pool = ["tech", "tutorial", "deep-dive", "opinion", "case-study",
                 "benchmark", "guide", "announcement", "review", "comparison",
                 "security", "performance", "architecture", "devops", "data"]
    nt_pool = len(tags_pool)
    for i in range(1, n + 1):
        topic = POST_TOPICS[i % nt]
        verb = POST_VERBS[i % nv]
        title = f"{verb} {topic.title()}"
        body = POST_BODY_TEMPLATES[i % nb].format(topic=topic)
        author_idx = (i * 7 + 13) % num_users + 1
        published = "TRUE" if i % 4 != 0 else "FALSE"
        tag1 = tags_pool[i % nt_pool]
        tag2 = tags_pool[(i * 3 + 7) % nt_pool]
        tags = json.dumps([tag1, tag2])
        yield [
            f"'{user_uuid(author_idx)}'",
            f"'{sql_escape(title)}'",
            f"'{sql_escape(body)}'",
            published,
            f"'{sql_escape(tags)}'",
        ]


def gen_products(n: int):
    na = len(PRODUCT_ADJECTIVES)
    nn = len(PRODUCT_NOUNS)
    ns = len(PRODUCT_SUFFIXES)
    nc = len(PRODUCT_CATEGORIES)
    ncit = len(CITIES)
    for i in range(1, n + 1):
        adj = PRODUCT_ADJECTIVES[i % na]
        noun = PRODUCT_NOUNS[i % nn]
        suffix = PRODUCT_SUFFIXES[i % ns]
        name = f"{adj} {noun} {suffix}{i}"
        category = PRODUCT_CATEGORIES[i % nc]
        desc = f"High-performance {category.lower()} solution. Model {suffix}{i}."
        price = round(9.99 + (i * 7.3) % 4990.0, 2)
        weight = round(0.1 + (i * 3.7) % 49.9, 2)
        city = CITIES[i % ncit]
        lat = city[1] + (i % 100) * 0.005
        lon = city[2] + (i % 100) * 0.005
        meta = json.dumps({"sku": f"SKU-{i:08d}", "tier": f"t{i % 3}"})
        ref_id = str(uuid.UUID(int=(i * 48271 + 12345) & ((1 << 128) - 1)))
        yield [
            str(i),
            f"'{sql_escape(name)}'",
            f"'{sql_escape(desc)}'",
            str(price),
            f"'{category}'",
            str(weight),
            f"'({lat:.4f}, {lon:.4f})'",
            f"'{sql_escape(meta)}'",
            f"'{ref_id}'",
        ]


def gen_orders(n: int, num_users: int, num_products: int):
    for i in range(1, n + 1):
        user_idx = (i * 13 + 7) % num_users + 1
        product_id = (i * 17 + 3) % num_products + 1
        quantity = 1 + (i * 11) % 20
        unit_price = round(9.99 + (product_id * 7.3) % 990.0, 2)
        total = round(unit_price * quantity, 2)
        dur = 1 + (i * 3) % 720
        units = ["hours", "days", "minutes"][i % 3]
        yield [
            str(i),
            f"'{user_uuid(user_idx)}'",
            str(product_id),
            str(quantity),
            str(total),
            f"'{dur} {units}'",
        ]


def gen_events(n: int):
    ne = len(EVENT_NAMES)
    quarters = ["Q1", "Q2", "Q3", "Q4"]
    for i in range(1, n + 1):
        name = f"{EVENT_NAMES[i % ne]} {quarters[i % 4]} {2022 + i % 5}"
        year = 2020 + (i % 6)
        month = (i % 12) + 1
        day = (i % 28) + 1
        event_date = f"{year}-{month:02d}-{day:02d}"
        hour = (i * 3) % 24
        start_time = f"{hour:02d}:{(i * 7) % 60:02d}:00"
        dur_hours = 1 + i % 8
        capacity = 10 + (i * 37) % 491
        yield [
            str(i),
            f"'{sql_escape(name)}'",
            f"'{event_date}'",
            f"'{start_time}'",
            f"'{dur_hours} hours'",
            str(capacity),
        ]


def gen_documents(n: int):
    nt = len(POST_TOPICS)
    for i in range(1, n + 1):
        topic = POST_TOPICS[i % nt]
        content = f"Document {i}: {topic} reference material revision {1 + i % 20}"
        blob_words = ["data", "binary", "payload", "block", "chunk", "stream", "buffer", "frame"]
        blob = " ".join(blob_words[j % len(blob_words)] for j in range(i % 5 + 2))
        yield [
            str(i),
            f"'{sql_escape(content)}'",
            f"'{sql_escape(blob)}'",
        ]


def gen_tags(n: int):
    np_ = len(TAG_PREFIXES)
    ns = len(TAG_SUFFIXES)
    for i in range(1, n + 1):
        prefix = TAG_PREFIXES[i % np_]
        suffix = TAG_SUFFIXES[(i * 3) % ns]
        label = f"{prefix}-{suffix}-{i}"
        tag_uuid = str(uuid.UUID(int=(i * 65537 + 999983) & ((1 << 128) - 1)))
        yield [
            str(i),
            f"'{sql_escape(label)}'",
            f"'{tag_uuid}'",
        ]


# ---------------------------------------------------------------------------
# Streaming edge generators (yield LINK statement strings)
# ---------------------------------------------------------------------------

def gen_follows(n: int, num_users: int):
    """Follows edges using stride-based generation — no dedup set needed."""
    stride = max(1, num_users // 3)
    for i in range(n):
        a = (i * 7 + 1) % num_users + 1
        b = (a + stride + (i % 97)) % num_users + 1
        if a == b:
            b = (b % num_users) + 1
        yield f"LINK users('{user_uuid(a)}') TO users('{user_uuid(b)}') VIA follows;"


def gen_authored(n: int, num_users: int, num_posts: int):
    """Each post linked to a deterministic author."""
    for i in range(1, n + 1):
        post_id = ((i - 1) % num_posts) + 1
        author_idx = (post_id * 7 + 13) % num_users + 1
        yield f"LINK users('{user_uuid(author_idx)}') TO posts({post_id}) VIA authored;"


def gen_rated(n: int, num_users: int, num_products: int):
    """Rated edges with score and review properties."""
    nu = len(USE_CASES)
    nr = len(REVIEW_TEMPLATES)
    for i in range(n):
        uid_idx = (i * 13 + 7) % num_users + 1
        pid = (i * 17 + 3) % num_products + 1
        score = round(1.0 + (i % 41) / 10.0, 1)
        if score > 5.0:
            score = 5.0
        use = USE_CASES[i % nu]
        review = sql_escape(REVIEW_TEMPLATES[i % nr].format(use=use))
        yield f"LINK users('{user_uuid(uid_idx)}') TO products({pid}) VIA rated (score={score}, review='{review}');"


def gen_reports_to(n: int, num_users: int):
    """Org-chart edges with tree-ish structure."""
    for i in range(1, n + 1):
        # Manager is someone with a lower index (tree structure)
        divisor = 3 + (i % 6)
        manager_idx = max(1, i // divisor)
        if manager_idx == i:
            manager_idx = max(1, i - 1)
        # Ensure manager_idx is within bounds
        if manager_idx > num_users:
            manager_idx = 1
        year = 2020 + (i % 6)
        month = (i % 12) + 1
        day = (i % 28) + 1
        yield f"LINK users('{user_uuid(i)}') TO users('{user_uuid(manager_idx)}') VIA reports_to (since='{year}-{month:02d}-{day:02d}');"


# ---------------------------------------------------------------------------
# Main generation
# ---------------------------------------------------------------------------

def generate(scale_name: str, skip_embeddings: bool, batch_size: int, database: str):
    cfg = SCALES[scale_name]

    data_keys = ["departments", "users", "posts", "products", "orders", "events", "documents", "tags"]
    edge_keys = ["follows", "authored", "rated", "reports_to"]
    total_rows = sum(cfg[k] for k in data_keys)
    total_edges = sum(cfg[k] for k in edge_keys)

    emit(f"-- SixSevenDB Load Test Data ({scale_name} scale)")
    emit(f"-- Generated rows: ~{total_rows:,} data + ~{total_edges:,} edges")
    emit(f"-- Total: ~{total_rows + total_edges:,}")
    if skip_embeddings:
        emit("-- EMBEDDING columns: SKIPPED (--skip-embeddings)")
    else:
        emit("-- EMBEDDING columns: ENABLED (use --skip-embeddings for faster loading)")
    emit("")

    progress(f"Scale: {scale_name} — {total_rows:,} rows + {total_edges:,} edges")

    # -------------------------------------------------------------------
    emit_comment("DATABASE")
    # -------------------------------------------------------------------
    emit(f"DROP DATABASE IF EXISTS {database};")
    emit(f"CREATE DATABASE {database};")
    emit(f"\\c {database}")
    emit("")

    # -------------------------------------------------------------------
    emit_comment("DDL: TABLES")
    # -------------------------------------------------------------------

    embedding_post = ""
    embedding_product = ""
    if not skip_embeddings:
        embedding_post = f",\n    body_vec    EMBEDDING(384, source='body', provider='{ONNX_PROVIDER}')"
        embedding_product = f",\n    desc_vec    EMBEDDING(384, source='description', provider='{ONNX_PROVIDER}')"

    emit(f"""\
CREATE TABLE departments (
    id          INT PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    budget      DECIMAL(12, 2) NOT NULL CHECK (budget > 0),
    metadata    TEXT
);

CREATE TABLE users (
    id          UUID PRIMARY KEY DEFAULT gen_uuid(),
    name        TEXT NOT NULL,
    email       VARCHAR(255) NOT NULL UNIQUE,
    age         INT NOT NULL CHECK (age > 0 AND age < 150),
    bio         TEXT,
    active      BOOLEAN DEFAULT TRUE,
    birth_date  TEXT,
    dept_id     INT
);

CREATE TABLE posts (
    id          INT PRIMARY KEY AUTOINCREMENT,
    author_id   UUID NOT NULL,
    title       TEXT NOT NULL,
    body        TEXT,
    published   BOOLEAN DEFAULT FALSE,
    tags        TEXT{embedding_post}
);

CREATE TABLE products (
    id          INT PRIMARY KEY,
    name        TEXT NOT NULL,
    description TEXT,
    price       DOUBLE NOT NULL CHECK (price >= 0),
    category    VARCHAR(50) NOT NULL,
    weight_kg   FLOAT,
    location    TEXT,
    metadata    TEXT,
    ref_id      UUID{embedding_product}
);

CREATE TABLE orders (
    id              INT PRIMARY KEY,
    user_id         UUID NOT NULL,
    product_id      INT NOT NULL,
    quantity        SMALLINT NOT NULL CHECK (quantity > 0),
    total           DECIMAL(10, 2) NOT NULL,
    shipping_time   TEXT
);

CREATE TABLE events (
    id          INT PRIMARY KEY,
    name        TEXT NOT NULL,
    event_date  TEXT NOT NULL,
    start_time  TEXT,
    duration    TEXT,
    capacity    BIGINT DEFAULT 100
);

CREATE TABLE documents (
    id          INT PRIMARY KEY,
    content     TEXT,
    binary_data TEXT
);

CREATE TABLE tags (
    id          INT PRIMARY KEY,
    label       TEXT NOT NULL UNIQUE,
    ref_uuid    UUID DEFAULT gen_uuid()
);
""")

    # -------------------------------------------------------------------
    emit_comment("DDL: INDEXES")
    # -------------------------------------------------------------------

    emit("""\
CREATE INDEX idx_user_age ON users(age);
CREATE INDEX idx_user_dept ON users(dept_id);
CREATE INDEX idx_post_author ON posts(author_id);
CREATE INDEX idx_product_category ON products(category);
CREATE INDEX idx_product_price ON products(price);
CREATE INDEX idx_order_user ON orders(user_id);
CREATE INDEX idx_order_compound ON orders(user_id, product_id);
CREATE INDEX idx_event_date ON events(event_date);
CREATE INDEX idx_tag_label ON tags(label);
""")

    # -------------------------------------------------------------------
    emit_comment("DDL: EDGE TYPES")
    # -------------------------------------------------------------------

    emit("""\
CREATE EDGE TYPE follows FROM users TO users;
CREATE EDGE TYPE authored FROM users TO posts;
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;
CREATE EDGE TYPE reports_to (since TEXT) FROM users TO users;
""")

    # -------------------------------------------------------------------
    emit_comment("DML: DEPARTMENTS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['departments']:,} departments...")
    emit_batch_insert_streaming("departments",
        ["id", "name", "budget", "metadata"],
        gen_departments(cfg["departments"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: USERS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['users']:,} users...")
    emit_batch_insert_streaming("users",
        ["id", "name", "email", "age", "bio", "active", "birth_date", "dept_id"],
        gen_users(cfg["users"], cfg["departments"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: POSTS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['posts']:,} posts...")
    emit_batch_insert_streaming("posts",
        ["author_id", "title", "body", "published", "tags"],
        gen_posts(cfg["posts"], cfg["users"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: PRODUCTS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['products']:,} products...")
    emit_batch_insert_streaming("products",
        ["id", "name", "description", "price", "category", "weight_kg",
         "location", "metadata", "ref_id"],
        gen_products(cfg["products"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: ORDERS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['orders']:,} orders...")
    emit_batch_insert_streaming("orders",
        ["id", "user_id", "product_id", "quantity", "total", "shipping_time"],
        gen_orders(cfg["orders"], cfg["users"], cfg["products"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: EVENTS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['events']:,} events...")
    emit_batch_insert_streaming("events",
        ["id", "name", "event_date", "start_time", "duration", "capacity"],
        gen_events(cfg["events"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: DOCUMENTS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['documents']:,} documents...")
    emit_batch_insert_streaming("documents",
        ["id", "content", "binary_data"],
        gen_documents(cfg["documents"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("DML: TAGS")
    # -------------------------------------------------------------------
    progress(f"Generating {cfg['tags']:,} tags...")
    emit_batch_insert_streaming("tags",
        ["id", "label", "ref_uuid"],
        gen_tags(cfg["tags"]), batch_size)

    # -------------------------------------------------------------------
    emit_comment("GRAPH: LINK edges")
    # -------------------------------------------------------------------

    edge_progress_interval = 500_000

    progress(f"Generating {cfg['follows']:,} follows edges...")
    emit_links_streaming(
        gen_follows(cfg["follows"], cfg["users"]),
        "follows edges", cfg["follows"], edge_progress_interval)

    progress(f"Generating {cfg['authored']:,} authored edges...")
    emit_links_streaming(
        gen_authored(cfg["authored"], cfg["users"], cfg["posts"]),
        "authored edges", cfg["authored"], edge_progress_interval)

    progress(f"Generating {cfg['rated']:,} rated edges...")
    emit_links_streaming(
        gen_rated(cfg["rated"], cfg["users"], cfg["products"]),
        "rated edges", cfg["rated"], edge_progress_interval)

    progress(f"Generating {cfg['reports_to']:,} reports_to edges...")
    emit_links_streaming(
        gen_reports_to(cfg["reports_to"], cfg["users"]),
        "reports_to edges", cfg["reports_to"], edge_progress_interval)

    # -------------------------------------------------------------------
    emit_comment("COMPLETE")
    # -------------------------------------------------------------------
    emit(f"-- Load test data generation complete ({scale_name} scale)")
    emit(f"-- Data rows: ~{total_rows:,}")
    emit(f"-- Edge links: ~{total_edges:,}")
    emit(f"-- Tables: 8 | Edge types: 4 | Indexes: 9")

    progress(f"Done. {total_rows:,} rows + {total_edges:,} edges generated.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate SixSevenDB load test data (millions of rows)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--scale",
        choices=["stress", "massive", "extreme"],
        default="stress",
        help="Data scale: stress (~8M), massive (~39M), extreme (~78M) total rows+edges",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducible output (default: 42)",
    )
    parser.add_argument(
        "--skip-embeddings",
        action="store_true",
        default=False,
        help="Omit EMBEDDING columns from schema for faster loading",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=2000,
        help="Rows per INSERT VALUES statement (default: 2000)",
    )
    parser.add_argument(
        "--database",
        type=str,
        default="loadtest_db",
        help="Database name to create (default: loadtest_db)",
    )
    args = parser.parse_args()

    random.seed(args.seed)

    # Buffer stdout for significantly faster I/O at scale
    global _out
    _out = io.TextIOWrapper(
        io.BufferedWriter(io.FileIO(sys.stdout.fileno(), "w", closefd=False),
                          buffer_size=1024 * 1024),
        encoding="utf-8",
        newline="\n",
    )

    try:
        generate(args.scale, args.skip_embeddings, args.batch_size, args.database)
    except BrokenPipeError:
        pass  # Expected when piping to head, less, etc.
    finally:
        try:
            _out.flush()
        except BrokenPipeError:
            pass


if __name__ == "__main__":
    # Restore default SIGPIPE handling for clean pipe behavior
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
