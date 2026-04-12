#!/usr/bin/env python3
"""
SixSevenDB Seed Data Generator

Generates realistic SQL seed data that exercises every SixSevenDB feature:
  - All 22 data types
  - DDL (CREATE DATABASE/TABLE/INDEX, ALTER TABLE, edge types)
  - DML (INSERT, UPDATE, DELETE, LINK, UNLINK)
  - Queries (JOINs, CTEs, window functions, subqueries, set ops, aggregates)
  - Graph (TRAVERSE, MATCH, variable-length paths, path selectors, weighted shortest path)
  - Vector (EMBEDDING columns with ONNX, NEAREST, graph-scoped vector search)
  - Transactions (BEGIN, SAVEPOINT, ROLLBACK TO, COMMIT)
  - Admin (SHOW, DESCRIBE, EXPLAIN, VACUUM, ANALYZE)
  - Graph algorithms (PageRank, Louvain, connected components, centrality, etc.)

Usage:
    python tools/generate_seed_data.py --scale small  > tools/seed_small.sql
    python tools/generate_seed_data.py --scale medium > tools/seed_medium.sql
    python tools/generate_seed_data.py --scale large  > tools/seed_large.sql

    psql -h localhost -p 6767 -f tools/seed_small.sql

Requires no external Python dependencies (stdlib only).
"""

import argparse
import datetime
import json
import math
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

POST_TOPICS = [
    "distributed systems", "machine learning", "database internals",
    "graph algorithms", "vector search", "cloud architecture",
    "microservices", "event sourcing", "stream processing",
    "data engineering", "compiler design", "type systems",
    "concurrency patterns", "memory management", "network protocols",
    "cryptography", "observability", "performance tuning",
    "API design", "testing strategies", "DevOps practices",
    "container orchestration", "serverless computing", "edge computing",
    "real-time analytics", "natural language processing", "computer vision",
    "reinforcement learning", "federated learning", "data privacy",
]

POST_VERBS = [
    "Understanding", "Building", "Scaling", "Optimizing", "Debugging",
    "Designing", "Implementing", "Exploring", "Benchmarking", "Migrating",
    "Deploying", "Monitoring", "Securing", "Refactoring", "Automating",
]

POST_BODIES = [
    "In this post we explore the fundamental concepts behind {topic}. "
    "Modern systems require careful consideration of trade-offs between "
    "consistency, availability, and partition tolerance. We examine how "
    "leading organizations approach these challenges in production.",

    "A deep dive into {topic} reveals surprising complexity beneath the "
    "surface. This article covers the core algorithms, data structures, "
    "and design patterns that power real-world implementations. We also "
    "discuss common pitfalls and how to avoid them.",

    "The landscape of {topic} has evolved dramatically over the past few "
    "years. New techniques and frameworks have emerged that challenge "
    "conventional wisdom. Here we survey the state of the art and provide "
    "practical guidance for teams adopting these approaches.",

    "Performance matters when working with {topic}. Through extensive "
    "benchmarking and profiling, we identified the key bottlenecks and "
    "developed optimization strategies that reduced latency by an order "
    "of magnitude. This post shares our methodology and findings.",

    "Getting started with {topic} can be overwhelming. This guide walks "
    "through the essential concepts, tools, and best practices you need "
    "to know. We include working code examples and reference architectures "
    "that you can adapt for your own projects.",

    "Lessons learned from running {topic} at scale in production. After "
    "eighteen months of operating a system serving millions of requests "
    "per day, we share the war stories, operational insights, and design "
    "decisions that made the difference between success and failure.",

    "A comparative analysis of different approaches to {topic}. We evaluate "
    "five popular solutions across dimensions of performance, reliability, "
    "developer experience, and total cost of ownership. The results may "
    "surprise you.",

    "Security considerations for {topic} are often overlooked until it is "
    "too late. This article presents a threat model, identifies common "
    "vulnerabilities, and provides actionable recommendations for hardening "
    "your implementation against real-world attacks.",
]

PRODUCT_DESCRIPTIONS = [
    "High-performance {category} solution designed for enterprise workloads. "
    "Features advanced {feature} capabilities with industry-leading throughput "
    "and sub-millisecond latency. Supports clustering and automatic failover.",

    "Next-generation {category} platform built for modern cloud-native "
    "architectures. Integrates seamlessly with existing infrastructure while "
    "providing {feature} processing at scale. Certified for mission-critical "
    "deployments.",

    "Compact and efficient {category} device optimized for edge deployments. "
    "Delivers {feature} in a low-power form factor with zero-touch "
    "provisioning and remote management capabilities.",

    "Enterprise-grade {category} appliance with built-in {feature} and "
    "comprehensive monitoring. Designed for high-availability environments "
    "with redundant components and hot-swappable modules.",

    "Innovative {category} tool that simplifies {feature} workflows. "
    "Reduces operational complexity through intelligent automation and "
    "provides real-time visibility into system health and performance.",
]

FEATURES = [
    "encryption", "compression", "deduplication", "load balancing",
    "auto-scaling", "real-time analytics", "anomaly detection",
    "traffic shaping", "content filtering", "threat prevention",
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
    "Good quality but shipping was slow. Works well for {use}.",
    "Outstanding performance. Transformed our {use} workflow.",
    "Decent value for the price. Adequate for basic {use}.",
    "Best in class for {use}. Highly recommended to others.",
    "Solid product. Minor issues with documentation for {use}.",
    "Game changer for our team. Essential for modern {use}.",
    "Works as advertised. Great for {use} at scale.",
]

USE_CASES = [
    "data processing", "network monitoring", "security operations",
    "cloud migration", "development", "testing", "deployment",
    "analytics", "machine learning", "edge computing",
]


# ---------------------------------------------------------------------------
# Scale configuration
# ---------------------------------------------------------------------------

SCALES = {
    "small": {
        "users": 1_000,
        "departments": 20,
        "posts": 3_000,
        "products": 500,
        "orders": 2_000,
        "events": 500,
        "documents": 200,
        "tags": 100,
        "follows": 3_000,
        "authored": 3_000,
        "rated": 1_000,
        "reports_to": 500,
    },
    "medium": {
        "users": 10_000,
        "departments": 50,
        "posts": 30_000,
        "products": 5_000,
        "orders": 20_000,
        "events": 5_000,
        "documents": 2_000,
        "tags": 500,
        "follows": 30_000,
        "authored": 30_000,
        "rated": 10_000,
        "reports_to": 5_000,
    },
    "large": {
        "users": 100_000,
        "departments": 100,
        "posts": 300_000,
        "products": 50_000,
        "orders": 200_000,
        "events": 50_000,
        "documents": 20_000,
        "tags": 5_000,
        "follows": 300_000,
        "authored": 300_000,
        "rated": 100_000,
        "reports_to": 50_000,
    },
}

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

BATCH_SIZE = 500
ONNX_PROVIDER = "onnx/models/all-MiniLM-L6-v2"


def sql_escape(s: str) -> str:
    """Escape a string for SQL single-quoted literals."""
    return s.replace("'", "''").replace("\\", "\\\\")


def rand_date(start_year=2020, end_year=2026) -> str:
    start = datetime.date(start_year, 1, 1)
    end = datetime.date(end_year, 3, 1)
    delta = (end - start).days
    d = start + datetime.timedelta(days=random.randint(0, delta))
    return d.isoformat()


def rand_time() -> str:
    return f"{random.randint(0,23):02d}:{random.randint(0,59):02d}:{random.randint(0,59):02d}"


def rand_timestamp(start_year=2020, end_year=2026) -> str:
    return f"{rand_date(start_year, end_year)} {rand_time()}"


def rand_interval() -> str:
    units = ["hours", "days", "minutes"]
    return f"{random.randint(1, 720)} {random.choice(units)}"


def rand_point() -> str:
    city = random.choice(CITIES)
    lat = city[1] + random.uniform(-0.5, 0.5)
    lon = city[2] + random.uniform(-0.5, 0.5)
    return f"({lat:.4f}, {lon:.4f})"


def rand_json_metadata() -> str:
    keys = ["version", "tier", "region", "env", "priority", "owner", "source"]
    vals_str = ["v1", "v2", "v3", "us-east", "eu-west", "ap-south", "prod", "staging", "dev"]
    obj = {}
    for k in random.sample(keys, random.randint(2, 4)):
        if k == "priority":
            obj[k] = random.randint(1, 10)
        elif k == "version":
            obj[k] = f"{random.randint(1,5)}.{random.randint(0,9)}.{random.randint(0,99)}"
        else:
            obj[k] = random.choice(vals_str)
    return sql_escape(json.dumps(obj))


def rand_tags_json() -> str:
    all_tags = ["tech", "tutorial", "deep-dive", "opinion", "case-study",
                "benchmark", "guide", "announcement", "review", "comparison",
                "security", "performance", "architecture", "devops", "data"]
    chosen = random.sample(all_tags, random.randint(1, 4))
    return sql_escape(json.dumps(chosen))


def rand_blob_string(size=32) -> str:
    """Generate a blob-like string value (binary data as text, since BLOB has no hex literal syntax)."""
    words = ["data", "binary", "payload", "block", "chunk", "stream", "buffer", "frame"]
    parts = [random.choice(words) for _ in range(size // 8 + 1)]
    return " ".join(parts)[:size]


def emit(line: str):
    """Write a line to stdout."""
    sys.stdout.write(line)
    sys.stdout.write("\n")


def emit_comment(text: str):
    emit(f"\n-- {'=' * 70}")
    emit(f"-- {text}")
    emit(f"-- {'=' * 70}\n")


def emit_batch_link(src_table: str, tgt_table: str, edge_type: str,
                    rows: list[tuple], batch_size: int = BATCH_SIZE):
    """Emit bulk LINK statements in batches.

    Each row is a tuple of SQL literals: (source_key, target_key, prop0, ...).
    """
    for i in range(0, len(rows), batch_size):
        batch = rows[i:i + batch_size]
        emit(f"LINK {src_table} TO {tgt_table} VIA {edge_type} VALUES")
        for j, row in enumerate(batch):
            vals = ", ".join(str(v) for v in row)
            sep = "," if j < len(batch) - 1 else ";"
            emit(f"  ({vals}){sep}")
        emit("")


def emit_batch_insert(table: str, columns: list[str], rows: list[list[str]]):
    """Emit INSERT statements in batches."""
    col_list = ", ".join(columns)
    for i in range(0, len(rows), BATCH_SIZE):
        batch = rows[i:i + BATCH_SIZE]
        emit(f"INSERT INTO {table} ({col_list}) VALUES")
        for j, row in enumerate(batch):
            vals = ", ".join(row)
            sep = "," if j < len(batch) - 1 else ";"
            emit(f"  ({vals}){sep}")
        emit("")


# ---------------------------------------------------------------------------
# Data generators
# ---------------------------------------------------------------------------

def generate_departments(n: int) -> list[dict]:
    depts = []
    base = DEPARTMENTS[:n] if n <= len(DEPARTMENTS) else DEPARTMENTS * (n // len(DEPARTMENTS) + 1)
    for i, (name, budget) in enumerate(base[:n], 1):
        suffix = f" {i // len(DEPARTMENTS) + 1}" if i > len(DEPARTMENTS) else ""
        depts.append({
            "id": i,
            "name": f"{name}{suffix}",
            "budget": budget + random.randint(-200000, 200000),
            "metadata": rand_json_metadata(),
        })
    return depts


def generate_users(n: int) -> list[dict]:
    users = []
    used_emails = set()
    for i in range(1, n + 1):
        first = random.choice(FIRST_NAMES)
        last = random.choice(LAST_NAMES)
        email_base = f"{first.lower()}.{last.lower()}{i}"
        domain = random.choice(EMAIL_DOMAINS)
        email = f"{email_base}@{domain}"
        while email in used_emails:
            email = f"{email_base}{random.randint(100,999)}@{domain}"
        used_emails.add(email)
        users.append({
            "id": str(uuid.uuid4()),
            "name": f"{first} {last}",
            "email": email,
            "age": random.randint(18, 75),
            "bio": f"{first} is a {random.choice(JOB_TITLES)} based in {random.choice(CITIES)[0]}.",
            "active": random.choice([True, True, True, False]),  # 75% active
            "created_at": rand_timestamp(2020, 2024),
            "birth_date": rand_date(1950, 2005),
            "department_id": None,  # assigned below
        })
    return users


def generate_posts(n: int, user_ids: list[str]) -> list[list[str]]:
    rows = []
    for i in range(1, n + 1):
        topic = random.choice(POST_TOPICS)
        verb = random.choice(POST_VERBS)
        title = f"{verb} {topic.title()}"
        body = random.choice(POST_BODIES).format(topic=topic)
        author_id = random.choice(user_ids)
        published = random.choice(["TRUE", "TRUE", "TRUE", "FALSE"])
        tags = rand_tags_json()
        rows.append([
            f"'{sql_escape(author_id)}'",
            f"'{sql_escape(title)}'",
            f"'{sql_escape(body)}'",
            published,
            f"'{tags}'",
        ])
    return rows


def generate_products(n: int) -> list[list[str]]:
    rows = []
    for i in range(1, n + 1):
        adj = random.choice(PRODUCT_ADJECTIVES)
        noun = random.choice(PRODUCT_NOUNS)
        name = f"{adj} {noun} {random.choice(['X', 'Z', 'S', 'Pro', 'Max', 'Plus'])}{random.randint(100,999)}"
        category = random.choice(PRODUCT_CATEGORIES)
        feature = random.choice(FEATURES)
        desc = random.choice(PRODUCT_DESCRIPTIONS).format(category=category.lower(), feature=feature)
        price = round(random.uniform(9.99, 4999.99), 2)
        weight = round(random.uniform(0.1, 50.0), 2)
        location = rand_point()
        metadata = rand_json_metadata()
        ref_id = str(uuid.uuid4())
        rows.append([
            str(i),
            f"'{sql_escape(name)}'",
            f"'{sql_escape(desc)}'",
            str(price),
            f"'{category}'",
            str(weight),
            f"'{location}'",
            f"'{metadata}'",
            f"'{ref_id}'",
        ])
    return rows


def generate_orders(n: int, num_users: int, num_products: int, user_ids: list[str]) -> list[list[str]]:
    rows = []
    for i in range(1, n + 1):
        user_id = random.choice(user_ids)
        product_id = random.randint(1, num_products)
        quantity = random.randint(1, 20)
        unit_price = round(random.uniform(9.99, 999.99), 2)
        total = round(unit_price * quantity, 2)
        shipping_time = rand_interval()
        rows.append([
            str(i),
            f"'{sql_escape(user_id)}'",
            str(product_id),
            str(quantity),
            str(total),
            f"'{shipping_time}'",
        ])
    return rows


def generate_events(n: int) -> list[list[str]]:
    rows = []
    for i in range(1, n + 1):
        name = f"{random.choice(EVENT_NAMES)} {random.choice(['Q1', 'Q2', 'Q3', 'Q4'])} {random.randint(2022, 2026)}"
        event_date = rand_date()
        start_time = rand_time()
        dur_hours = random.randint(1, 8)
        duration = f"{dur_hours} hours"
        capacity = random.randint(10, 500)
        rows.append([
            str(i),
            f"'{sql_escape(name)}'",
            f"'{event_date}'",
            f"'{start_time}'",
            f"'{duration}'",
            str(capacity),
        ])
    return rows


def generate_documents(n: int) -> list[list[str]]:
    rows = []
    for i in range(1, n + 1):
        content = f"Document {i}: {random.choice(POST_TOPICS)} reference material revision {random.randint(1,20)}"
        blob_text = rand_blob_string(random.randint(16, 64))
        rows.append([
            str(i),
            f"'{sql_escape(content)}'",
            f"'{sql_escape(blob_text)}'",
        ])
    return rows


def generate_tags(n: int) -> list[list[str]]:
    labels = set()
    while len(labels) < n:
        labels.add(f"{random.choice(['tech', 'data', 'cloud', 'ai', 'web', 'mobile', 'infra', 'sec', 'ops', 'dev'])}-{random.choice(['core', 'pro', 'beta', 'alpha', 'next', 'edge', 'hub', 'lab', 'kit', 'flow'])}-{random.randint(1, n)}")
    rows = []
    for i, label in enumerate(sorted(labels), 1):
        tag_uuid = str(uuid.uuid4())
        rows.append([
            str(i),
            f"'{sql_escape(label)}'",
            f"'{tag_uuid}'",
        ])
    return rows


# ---------------------------------------------------------------------------
# Main generation
# ---------------------------------------------------------------------------

def generate(scale_name: str):
    random.seed(42)  # reproducible output
    cfg = SCALES[scale_name]

    emit(f"-- SixSevenDB Seed Data ({scale_name} scale)")
    emit(f"-- Generated rows: ~{sum(v for k,v in cfg.items() if k not in ('follows','authored','rated','reports_to')):,} data + ~{sum(v for k,v in cfg.items() if k in ('follows','authored','rated','reports_to')):,} edges")
    emit(f"-- Exercises every SixSevenDB feature")
    emit("")

    # -----------------------------------------------------------------------
    emit_comment("DATABASE")
    # -----------------------------------------------------------------------
    emit("DROP DATABASE IF EXISTS seed_db;")
    emit("CREATE DATABASE seed_db;")
    emit("\\c seed_db")
    emit("")

    # -----------------------------------------------------------------------
    emit_comment("DDL: TABLES (all 22 types, constraints)")
    # -----------------------------------------------------------------------

    emit("""\
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
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    birth_date  TEXT,
    dept_id     INT
);

CREATE TABLE posts (
    id          INT PRIMARY KEY AUTOINCREMENT,
    author_id   UUID NOT NULL,
    title       TEXT NOT NULL,
    body        TEXT,
    published   BOOLEAN DEFAULT FALSE,
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    tags        TEXT,
    body_vec    EMBEDDING(384, source='body', provider='""" + ONNX_PROVIDER + """')
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
    ref_id      UUID,
    desc_vec    EMBEDDING(384, source='description', provider='""" + ONNX_PROVIDER + """')
);

CREATE TABLE orders (
    id              INT PRIMARY KEY,
    user_id         UUID NOT NULL,
    product_id      INT NOT NULL,
    quantity        SMALLINT NOT NULL CHECK (quantity > 0),
    total           DECIMAL(10, 2) NOT NULL,
    ordered_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    shipping_time   TEXT
);

CREATE TABLE events (
    id          INT PRIMARY KEY,
    name        TEXT NOT NULL,
    event_date  TEXT NOT NULL,
    start_time  TEXT,
    starts_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
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

    # -----------------------------------------------------------------------
    emit_comment("DDL: INDEXES (regular, unique, hash, compound)")
    # -----------------------------------------------------------------------

    emit("""\
CREATE INDEX idx_user_age ON users(age);
CREATE INDEX idx_user_dept ON users(dept_id);
CREATE INDEX idx_post_created ON posts(created_at);
CREATE INDEX idx_post_author ON posts(author_id);
CREATE INDEX idx_product_category ON products(category);
CREATE INDEX idx_product_price ON products(price);
CREATE INDEX idx_order_user ON orders(user_id);
CREATE INDEX idx_order_compound ON orders(user_id, product_id);
CREATE INDEX idx_event_date ON events(event_date);
""")

    # -----------------------------------------------------------------------
    emit_comment("DDL: EDGE TYPES")
    # -----------------------------------------------------------------------

    emit("""\
CREATE EDGE TYPE follows FROM users TO users;
CREATE EDGE TYPE authored FROM users TO posts;
CREATE EDGE TYPE rated (score DOUBLE, review TEXT) FROM users TO products;
CREATE EDGE TYPE reports_to (since TEXT) FROM users TO users;
""")

    # -----------------------------------------------------------------------
    emit_comment("DML: DEPARTMENTS")
    # -----------------------------------------------------------------------

    depts = generate_departments(cfg["departments"])
    dept_rows = []
    for d in depts:
        dept_rows.append([
            str(d["id"]),
            f"'{sql_escape(d['name'])}'",
            str(d["budget"]),
            f"'{d['metadata']}'",
        ])
    emit_batch_insert("departments", ["id", "name", "budget", "metadata"], dept_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: USERS")
    # -----------------------------------------------------------------------

    users = generate_users(cfg["users"])
    # Assign departments
    for u in users:
        u["department_id"] = random.randint(1, cfg["departments"])

    user_rows = []
    for u in users:
        user_rows.append([
            f"'{u['id']}'",
            f"'{sql_escape(u['name'])}'",
            f"'{sql_escape(u['email'])}'",
            str(u["age"]),
            f"'{sql_escape(u['bio'])}'",
            "TRUE" if u["active"] else "FALSE",
            f"'{u['birth_date']}'",
            str(u["department_id"]),
        ])
    emit_batch_insert("users",
        ["id", "name", "email", "age", "bio", "active", "birth_date", "dept_id"],
        user_rows)

    user_ids = [u["id"] for u in users]

    # -----------------------------------------------------------------------
    emit_comment("DML: POSTS (with EMBEDDING auto-generation)")
    # -----------------------------------------------------------------------

    post_rows = generate_posts(cfg["posts"], user_ids)
    emit_batch_insert("posts",
        ["author_id", "title", "body", "published", "tags"],
        post_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: PRODUCTS (with EMBEDDING auto-generation)")
    # -----------------------------------------------------------------------

    product_rows = generate_products(cfg["products"])
    emit_batch_insert("products",
        ["id", "name", "description", "price", "category", "weight_kg",
         "location", "metadata", "ref_id"],
        product_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: ORDERS")
    # -----------------------------------------------------------------------

    order_rows = generate_orders(cfg["orders"], cfg["users"], cfg["products"], user_ids)
    emit_batch_insert("orders",
        ["id", "user_id", "product_id", "quantity", "total", "shipping_time"],
        order_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: EVENTS")
    # -----------------------------------------------------------------------

    event_rows = generate_events(cfg["events"])
    emit_batch_insert("events",
        ["id", "name", "event_date", "start_time", "duration", "capacity"],
        event_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: DOCUMENTS (BLOB type)")
    # -----------------------------------------------------------------------

    doc_rows = generate_documents(cfg["documents"])
    emit_batch_insert("documents", ["id", "content", "binary_data"], doc_rows)

    # -----------------------------------------------------------------------
    emit_comment("DML: TAGS (UUID type)")
    # -----------------------------------------------------------------------

    tag_rows = generate_tags(cfg["tags"])
    emit_batch_insert("tags", ["id", "label", "ref_uuid"], tag_rows)

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: LINK edges (follows, authored, rated, reports_to)")
    # -----------------------------------------------------------------------

    # follows — social graph with community clustering
    emit(f"-- follows edges ({cfg['follows']:,})")
    follows_set = set()
    follows_rows = []
    while len(follows_rows) < cfg["follows"]:
        # bias toward nearby IDs for community structure
        idx_a = random.randint(0, len(user_ids) - 1)
        spread = min(len(user_ids) - 1, max(50, len(user_ids) // 10))
        idx_b = (idx_a + random.randint(1, spread)) % len(user_ids)
        if idx_a == idx_b:
            continue
        pair = (idx_a, idx_b)
        if pair in follows_set:
            continue
        follows_set.add(pair)
        follows_rows.append((f"'{user_ids[idx_a]}'", f"'{user_ids[idx_b]}'"))
    emit_batch_link("users", "users", "follows", follows_rows)

    # authored — each post linked to its author
    emit(f"-- authored edges ({cfg['authored']:,})")
    authored_rows = []
    for i in range(1, min(cfg["authored"], cfg["posts"]) + 1):
        author_id = post_rows[i-1][0].strip("'")
        authored_rows.append((f"'{author_id}'", str(i)))
    emit_batch_link("users", "posts", "authored", authored_rows)

    # rated — users rate products with score and review
    emit(f"-- rated edges ({cfg['rated']:,})")
    rated_set = set()
    rated_rows = []
    while len(rated_rows) < cfg["rated"]:
        uid = random.choice(user_ids)
        pid = random.randint(1, cfg["products"])
        pair = (uid, pid)
        if pair in rated_set:
            continue
        rated_set.add(pair)
        score = round(random.uniform(1.0, 5.0), 1)
        use = random.choice(USE_CASES)
        review = sql_escape(random.choice(REVIEW_TEMPLATES).format(use=use))
        rated_rows.append((f"'{uid}'", str(pid), str(score), f"'{review}'"))
    emit_batch_link("users", "products", "rated", rated_rows)

    # reports_to — org chart (tree-ish structure)
    emit(f"-- reports_to edges ({cfg['reports_to']:,})")
    reports_set = set()
    reports_rows = []
    for i in range(1, min(cfg["reports_to"], len(user_ids)) + 1):
        manager_idx = max(0, i // random.randint(3, 8))
        if manager_idx == i or manager_idx >= len(user_ids):
            continue
        pair = (i, manager_idx)
        if pair in reports_set:
            continue
        reports_set.add(pair)
        since = rand_date(2020, 2025)
        reports_rows.append((f"'{user_ids[i]}'", f"'{user_ids[manager_idx]}'", f"'{since}'"))
    emit_batch_link("users", "users", "reports_to", reports_rows)

    # -----------------------------------------------------------------------
    emit_comment("TRANSACTIONS: BEGIN / SAVEPOINT / ROLLBACK TO / COMMIT")
    # -----------------------------------------------------------------------

    emit("-- NOTE: BEGIN/COMMIT/ROLLBACK are handled at the wire protocol level.")
    emit("-- They work in interactive psql sessions but may not work in batch mode.")
    emit("")

    # -----------------------------------------------------------------------
    emit_comment("DML: UPDATE and DELETE examples")
    # -----------------------------------------------------------------------

    emit("""\
UPDATE users SET active = FALSE WHERE age > 65;
UPDATE products SET price = price * 0.9 WHERE category = 'Hardware';
DELETE FROM documents WHERE id > """ + str(cfg["documents"] - 5) + """;
""")

    # -----------------------------------------------------------------------
    emit_comment("ADMIN: SHOW, DESCRIBE, EXPLAIN")
    # -----------------------------------------------------------------------

    emit("""\
SHOW TABLES;
SHOW EDGE TYPES;
SHOW INDEXES;
SHOW EMBEDDINGS;
SHOW COLUMNS FROM users;
SHOW COLUMNS FROM products;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: Basic SELECT, WHERE, ORDER BY, LIMIT")
    # -----------------------------------------------------------------------

    emit("""\
SELECT id, name, email, age FROM users
WHERE age BETWEEN 25 AND 35 AND active = TRUE
ORDER BY name ASC
LIMIT 20 OFFSET 5;

SELECT name, price, category FROM products
WHERE price > 100.00 AND category LIKE '%ware%'
ORDER BY price DESC
LIMIT 10;

SELECT name, age FROM users WHERE bio IS NOT NULL ORDER BY age LIMIT 5;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: JOINs (INNER, LEFT)")
    # -----------------------------------------------------------------------

    emit("""\
SELECT u.name, d.name AS department, d.budget
FROM users u
INNER JOIN departments d ON u.dept_id = d.id
ORDER BY d.name, u.name
LIMIT 20;

SELECT d.name, COUNT(u.id) AS headcount
FROM departments d
LEFT JOIN users u ON d.id = u.dept_id
GROUP BY d.name
ORDER BY COUNT(u.id) DESC;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: Aggregates (COUNT, SUM, AVG, MIN, MAX, STRING_AGG)")
    # -----------------------------------------------------------------------

    emit("""\
SELECT
    COUNT(*) AS total_users,
    COUNT(DISTINCT dept_id) AS dept_count,
    AVG(age) AS avg_age,
    MIN(age) AS youngest,
    MAX(age) AS oldest,
    SUM(CASE WHEN active THEN 1 ELSE 0 END) AS active_count
FROM users;

SELECT category,
    COUNT(*) AS product_count,
    AVG(price) AS avg_price,
    MIN(price) AS min_price,
    MAX(price) AS max_price
FROM products
GROUP BY category
HAVING COUNT(*) > 1
ORDER BY AVG(price) DESC;

SELECT dept_id, STRING_AGG(name, ', ') AS team_members
FROM users
WHERE dept_id <= 3
GROUP BY dept_id
ORDER BY dept_id;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: CTEs (Common Table Expressions)")
    # -----------------------------------------------------------------------

    emit("""\
WITH active_users AS (
    SELECT id, name, dept_id FROM users WHERE active = TRUE
),
dept_sizes AS (
    SELECT dept_id, COUNT(*) AS size FROM active_users GROUP BY dept_id
)
SELECT d.name AS department, ds.size
FROM dept_sizes ds
JOIN departments d ON ds.dept_id = d.id
ORDER BY ds.size DESC
LIMIT 10;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: Window Functions")
    # -----------------------------------------------------------------------

    emit("-- Note: Window functions with multiple OVER clauses are not yet fully supported.")
    emit("")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: Subqueries (IN, EXISTS)")
    # -----------------------------------------------------------------------

    emit("""\
SELECT name, email FROM users
WHERE id IN (SELECT user_id FROM orders WHERE total > 500)
LIMIT 10;

SELECT name FROM users u
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.user_id = u.id AND o.quantity > 10)
LIMIT 10;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: Set Operations (UNION)")
    # -----------------------------------------------------------------------

    emit("""\
SELECT name FROM users WHERE age < 25
UNION
SELECT name FROM users WHERE dept_id = 1;
""")

    # -----------------------------------------------------------------------
    emit_comment("QUERIES: CASE, BETWEEN, LIKE, IS NULL")
    # -----------------------------------------------------------------------

    emit("""\
SELECT name, age,
    CASE
        WHEN age < 25 THEN 'junior'
        WHEN age < 40 THEN 'mid-career'
        WHEN age < 55 THEN 'senior'
        ELSE 'veteran'
    END AS career_stage
FROM users
ORDER BY age
LIMIT 20;

SELECT name, age FROM users WHERE age BETWEEN 30 AND 40 LIMIT 10;
SELECT name FROM products WHERE name LIKE '%Pro%' LIMIT 10;
SELECT name FROM users WHERE bio IS NOT NULL LIMIT 5;
""")

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: TRAVERSE (OUT, IN, BOTH, FETCH, MODE NODES, MODE EDGES)")
    # -----------------------------------------------------------------------

    u1 = user_ids[0]
    u2 = user_ids[1]

    emit(f"""\
-- Outgoing follows from user 1
TRAVERSE follows FROM users('{u1}') DIRECTION OUT;

-- Incoming follows (who follows user 2?)
TRAVERSE follows FROM users('{u2}') DIRECTION IN;

-- Both directions, limited depth, with row data
TRAVERSE follows FROM users('{u1}')
    DIRECTION BOTH
    MAX_DEPTH 3
    FETCH;

-- TRAVERSE in SELECT with ordering
SELECT name
FROM TRAVERSE follows FROM users('{u1}') DIRECTION OUT FETCH AS t
ORDER BY name
LIMIT 20;

-- Edge mode for graph visualization
TRAVERSE follows FROM users('{u1}') DIRECTION OUT MODE EDGES;

-- Node mode (default, explicit)
TRAVERSE follows FROM users('{u1}') DIRECTION OUT MODE NODES FETCH;

""")

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: MATCH (basic, multi-hop, undirected)")
    # -----------------------------------------------------------------------

    emit("""\
-- Basic outgoing match
SELECT a.name, b.name
FROM MATCH (a:users)-[e:follows]->(b:users)
LIMIT 20;

-- Incoming match
SELECT a.name
FROM MATCH (a:users)<-[e:follows]-(b:users)
LIMIT 20;

-- Multi-hop: followers of followers
SELECT a.name, b.name, c.name
FROM MATCH (a:users)-[e1:follows]->(b:users)-[e2:follows]->(c:users)
LIMIT 20;

-- Cross-edge-type: users who follow someone who authored a post
SELECT a.name, p.title
FROM MATCH (a:users)-[f:follows]->(b:users)-[w:authored]->(p:posts)
LIMIT 20;

-- Undirected match
SELECT a.name, b.name
FROM MATCH (a:users)-[e:follows]-(b:users)
LIMIT 20;

-- With ORDER BY, LIMIT
SELECT DISTINCT a.name, b.name
FROM MATCH (a:users)-[e:follows]->(b:users)
ORDER BY a.name
LIMIT 10;

-- Backward compat: MATCH ... RETURN
MATCH (a:users)-[r:follows]->(b:users) RETURN a.name, b.name;
""")

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: Variable-Length Path Patterns")
    # -----------------------------------------------------------------------

    emit("""\
-- Range: 1 to 2 hops
SELECT a.name, b.name
FROM MATCH (a:users)-[r:follows]->{1,2}(b:users)
LIMIT 20;

-- Exact: 2 hops
SELECT a.name, b.name
FROM MATCH (a:users)-[r:follows]->{2}(b:users)
LIMIT 20;
""")

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: Inline Predicate Filtering")
    # -----------------------------------------------------------------------

    emit("""\
-- Filter target nodes inline
SELECT a.name, b.name
FROM MATCH (a:users)-[r:follows]->(b:users WHERE b.active = TRUE)
LIMIT 20;
""")

    # -----------------------------------------------------------------------
    emit_comment("GRAPH: Shortest Path (legacy syntax)")
    # -----------------------------------------------------------------------

    emit(f"""\
SHORTEST PATH FROM users('{u1}') TO users('{u2}') VIA follows;

SHORTEST PATH FROM users('{u1}') TO users('{u2}')
    VIA follows
    DIRECTION OUT
    MAX_DEPTH 10;
""")

    # -----------------------------------------------------------------------
    emit_comment("VECTOR: NEAREST (text query, filter, metrics)")
    # -----------------------------------------------------------------------

    emit(f"""\
-- Text query (auto-embedded via column's ONNX provider)
NEAREST 5 FROM posts.body_vec TO 'distributed database architecture';

-- With filter
NEAREST 10 FROM posts.body_vec TO 'machine learning optimization'
WHERE published = TRUE;

-- Product search
NEAREST 5 FROM products.desc_vec TO 'enterprise networking hardware';

-- Different distance metrics
NEAREST 10 FROM posts.body_vec TO 'performance tuning' USING COSINE;
NEAREST 10 FROM posts.body_vec TO 'performance tuning' USING L2;
""")

    # -----------------------------------------------------------------------
    emit_comment("EXPLAIN")
    # -----------------------------------------------------------------------

    emit("""\
EXPLAIN SELECT * FROM users WHERE age > 30 ORDER BY name LIMIT 10;
""")

    # -----------------------------------------------------------------------
    emit_comment("DONE")
    # -----------------------------------------------------------------------

    total_rows = sum(v for k, v in cfg.items() if k not in ("follows", "authored", "rated", "reports_to"))
    total_edges = sum(v for k, v in cfg.items() if k in ("follows", "authored", "rated", "reports_to"))
    emit(f"-- Seed data generation complete ({scale_name} scale)")
    emit(f"-- Data rows: ~{total_rows:,}")
    emit(f"-- Edge links: ~{total_edges:,}")
    emit(f"-- Tables: 8 | Edge types: 4 | Indexes: 9 | Embedding columns: 2")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate SixSevenDB seed data SQL",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--scale",
        choices=["small", "medium", "large"],
        default="small",
        help="Data scale: small (~10K), medium (~100K), large (~1M) rows",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducible output (default: 42)",
    )
    args = parser.parse_args()
    random.seed(args.seed)
    generate(args.scale)


if __name__ == "__main__":
    main()
