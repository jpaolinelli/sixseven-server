#include "sixseven/executor/demo_bootstrap.h"

#include "sixseven/common/logging.h"
#include "sixseven/common/result.h"
#include "sixseven/common/status.h"
#include "sixseven/executor/query_engine.h"

#include <string>
#include <vector>

namespace sixseven {

namespace {

// ---------------------------------------------------------------------------
// Data arrays — all deterministic, no <random> needed.
// ---------------------------------------------------------------------------

static const char* const k_first_names[] = {
    "James",  "Maria",  "Wei",   "Aisha",  "Carlos",
    "Elena",  "Raj",    "Yuki",  "Olga",   "Ahmed",
    "Sarah",  "David",  "Lin",   "Fatima", "Miguel",
    "Anna",   "Kwame",  "Priya", "Thomas", "Ingrid",
};
static constexpr int k_first_count = 20;

static const char* const k_last_names[] = {
    "Morrison", "Chen",     "Okafor",   "Nakamura", "Garcia",
    "Volkov",   "Patel",    "Johansson", "Kim",      "Schmidt",
    "Santos",   "Williams", "Nguyen",   "Andersson", "Hassan",
    "Taylor",   "Suzuki",   "Petrov",   "Singh",    "Torres",
};
static constexpr int k_last_count = 20;

// 200 unique names: first[i % 20] + " " + last[i / 20 % 20]
static constexpr int k_author_count = 200;

static const char* const k_nationalities[] = {
    "American", "British", "Chinese",  "Nigerian", "Japanese",
    "Brazilian","Indian",  "Swedish",  "Mexican",  "German",
};
static constexpr int k_nat_count = 10;

static const char* const k_genres[] = {
    "Science Fiction", "Fantasy",          "Mystery",       "Romance",
    "Historical Fiction","Literary Fiction","Thriller",      "Horror",
    "Biography",        "Science",          "Philosophy",    "Self-Help",
};
static constexpr int k_genre_count = 12;

static const char* const k_title_adj[] = {
    "The Last",      "A Thousand",   "The Silent",  "Midnight",      "The Forgotten",
    "Crimson",       "The Infinite", "Golden",      "The Secret",    "Shattered",
    "The Luminous",  "Dark",         "The Wandering","Emerald",      "The Seventh",
};
static constexpr int k_adj_count = 15;

static const char* const k_title_noun[] = {
    "Garden", "Stars",   "Kingdom", "Song",    "Shadow",
    "River",  "Dream",   "Forest",  "Storm",   "Light",
    "Ocean",  "Mountain","City",    "Fire",    "Winter",
};
static constexpr int k_noun_count = 15;

static const char* const k_desc_theme[] = {
    "betrayal and redemption",  "love across worlds",     "the search for identity",
    "political intrigue",       "scientific discovery",   "spiritual awakening",
    "survival against all odds","family secrets",         "revolution and justice",
    "memory and loss",
};
static constexpr int k_theme_count = 10;

static const char* const k_desc_setting[] = {
    "a dystopian future",       "a mythical kingdom",      "a bustling metropolis",
    "a remote island",          "a space colony",          "medieval Europe",
    "a parallel universe",      "rural Japan",             "colonial Africa",
    "a deep-sea research base",
};
static constexpr int k_setting_count = 10;

static const char* const k_desc_protagonist[] = {
    "a reluctant hero",         "an unlikely detective",   "a brilliant scientist",
    "a young apprentice",       "a hardened soldier",      "a street-smart orphan",
    "a disgraced noble",        "a time-traveling historian","a rogue AI",
    "a grieving parent",
};
static constexpr int k_protag_count = 10;

static const char* const k_desc_concept[] = {
    "the nature of power",      "human connection",        "the cost of ambition",
    "truth and deception",      "sacrifice and duty",      "hope in darkness",
    "the passage of time",      "cultural identity",       "the limits of knowledge",
    "morality without certainty",
};
static constexpr int k_concept_count = 10;

static const char* const k_cities[] = {
    "New York",    "London",      "Tokyo",        "Lagos",        "São Paulo",
    "Mumbai",      "Berlin",      "Sydney",       "Cairo",        "Toronto",
    "Seoul",       "Paris",       "Nairobi",      "Mexico City",  "Stockholm",
    "Dubai",       "Singapore",   "Buenos Aires", "Cape Town",    "Amsterdam",
};
static constexpr int k_city_count = 20;

static constexpr int k_reader_count = 500;
static constexpr int k_book_count   = 200; // Scaled down for reasonable startup time
static constexpr int k_review_count = 1000;

static const char* const k_tags[] = {
    "page-turner",     "classic",        "award-winner",   "debut",
    "bestseller",      "must-read",      "dark",           "uplifting",
    "epic",            "thought-provoking","slow-burn",    "action-packed",
    "lyrical",         "gritty",         "whimsical",      "haunting",
    "funny",           "romantic",       "educational",    "controversial",
    "international",   "debut-novel",    "series",         "standalone",
    "short-stories",   "novella",        "translated",     "illustrated",
    "audio-friendly",  "book-club-pick",
};
static constexpr int k_tag_count = 30;

static const char* const k_review_adj[] = {
    "stunning", "gripping", "mediocre", "brilliant", "disappointing",
    "unforgettable", "overrated", "captivating", "dense", "refreshing",
};
static constexpr int k_review_adj_count = 10;

static const char* const k_review_aspect[] = {
    "world-building", "character development", "plot structure", "prose style",
    "dialogue", "pacing", "ending", "atmosphere", "themes", "originality",
};
static constexpr int k_review_aspect_count = 10;

static const char* const k_review_assessment[] = {
    "exceptional", "above average", "solid", "underwhelming", "remarkable",
    "inconsistent", "masterful", "predictable", "nuanced", "bold",
};
static constexpr int k_review_assessment_count = 10;

// ---------------------------------------------------------------------------
// Helper: escape a single-quoted SQL string (replace ' with '')
// ---------------------------------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helper: execute and propagate error
// ---------------------------------------------------------------------------
#define DEMO_EXEC(engine, sql)                                                  \
    do {                                                                        \
        auto _r = (engine).execute((sql));                                      \
        if (!_r) {                                                              \
            return make_error(_r.error().code,                                  \
                              "demo bootstrap: " + _r.error().message);         \
        }                                                                       \
    } while (false)

// ---------------------------------------------------------------------------
// Schema creation
// ---------------------------------------------------------------------------
static Result<void> create_schema(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: creating bookstore schema");

    DEMO_EXEC(engine,
        "CREATE TABLE authors ("
        "  id         INT PRIMARY KEY,"
        "  name       TEXT NOT NULL,"
        "  nationality TEXT,"
        "  birth_year INT,"
        "  bio        TEXT"
        ")");

    DEMO_EXEC(engine,
        "CREATE TABLE books ("
        "  id              INT PRIMARY KEY,"
        "  title           TEXT NOT NULL,"
        "  genre           TEXT NOT NULL,"
        "  published_year  INT,"
        "  pages           INT,"
        "  rating          DOUBLE,"
        "  description     TEXT,"
        "  description_vec EMBEDDING(384, description, 'onnx/models/all-MiniLM-L6-v2')"
        ")");

    DEMO_EXEC(engine,
        "CREATE TABLE readers ("
        "  id          INT PRIMARY KEY,"
        "  username    TEXT NOT NULL,"
        "  email       TEXT,"
        "  city        TEXT,"
        "  joined_year INT"
        ")");

    DEMO_EXEC(engine,
        "CREATE TABLE reviews ("
        "  id          INT PRIMARY KEY,"
        "  book_id     INT NOT NULL,"
        "  reader_id   INT NOT NULL,"
        "  stars       INT NOT NULL,"
        "  review_text TEXT,"
        "  review_vec  EMBEDDING(384, review_text, 'onnx/models/all-MiniLM-L6-v2')"
        ")");

    DEMO_EXEC(engine,
        "CREATE TABLE tags ("
        "  id   INT PRIMARY KEY,"
        "  name TEXT NOT NULL"
        ")");

    // Edge types
    DEMO_EXEC(engine, "CREATE EDGE TYPE wrote FROM authors TO books");
    DEMO_EXEC(engine, "CREATE EDGE TYPE reviewed_by FROM books TO readers");
    DEMO_EXEC(engine, "CREATE EDGE TYPE follows FROM readers TO readers");
    DEMO_EXEC(engine, "CREATE EDGE TYPE tagged_as FROM books TO tags");

    return ok();
}


// ---------------------------------------------------------------------------
// Seed authors (200 rows, batches of 50)
// ---------------------------------------------------------------------------
static Result<void> seed_authors(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding {} authors", k_author_count);

    static const char* const bios[] = {
        "Award-winning novelist known for boundary-pushing narratives.",
        "Bestselling author whose work has been translated into forty languages.",
        "Debut novelist who stormed onto the literary scene with a cult classic.",
        "Former journalist turned fiction writer with a gift for authentic dialogue.",
        "Academic-turned-storyteller whose research enriches every paragraph.",
        "Genre-bending writer celebrated for blending myth with contemporary life.",
        "Pulitzer Prize finalist recognised for unflinching social commentary.",
        "Author of a beloved trilogy that redefined the genre for a generation.",
        "Short-story master whose collections have won multiple national awards.",
        "Retired diplomat who channels global experience into sweeping epics.",
    };
    static constexpr int k_bio_count = 10;

    constexpr int batch = 50;
    int row = 0;
    while (row < k_author_count) {
        std::string sql = "INSERT INTO authors (id, name, nationality, birth_year, bio) VALUES ";
        bool first_val = true;
        int end = std::min(row + batch, k_author_count);
        for (int i = row; i < end; ++i) {
            std::string name = std::string(k_first_names[i % k_first_count]) + " " +
                               std::string(k_last_names[(i / k_first_count) % k_last_count]);
            const char* nat  = k_nationalities[i % k_nat_count];
            int birth_year   = 1930 + (i * 17) % 70; // 1930–1999
            const char* bio  = bios[i % k_bio_count];

            if (!first_val) sql += ", ";
            first_val = false;
            sql += "(";
            sql += std::to_string(i + 1);
            sql += ", '";
            sql += esc(name);
            sql += "', '";
            sql += nat;
            sql += "', ";
            sql += std::to_string(birth_year);
            sql += ", '";
            sql += esc(bio);
            sql += "')";
        }
        DEMO_EXEC(engine, sql);
        row = end;
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Seed books (200 rows, batches of 50)
// ---------------------------------------------------------------------------
static Result<void> seed_books(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding {} books", k_book_count);

    constexpr int batch = 50;
    int row = 0;
    while (row < k_book_count) {
        std::string sql =
            "INSERT INTO books (id, title, genre, published_year, pages, rating, description)"
            " VALUES ";
        bool first_val = true;
        int end = std::min(row + batch, k_book_count);
        for (int i = row; i < end; ++i) {
            // Build title
            std::string title = std::string(k_title_adj[i % k_adj_count]) + " " +
                                std::string(k_title_noun[(i / k_adj_count) % k_noun_count]);

            // Cycle through genres
            const char* genre = k_genres[i % k_genre_count];

            int pub_year = 1950 + (i * 13) % 74; // 1950–2023
            int pages    = 150 + (i * 47) % 750;  // 150–899

            // rating 2.0 – 5.0 in 0.1 steps (deterministic)
            double rating = 2.0 + static_cast<double>((i * 7) % 30) / 10.0;

            // Build description from templates
            std::string desc =
                "A " + std::string(k_desc_theme[i % k_theme_count]) + " story set in " +
                std::string(k_desc_setting[(i + 3) % k_setting_count]) +
                ", following " + std::string(k_desc_protagonist[(i + 1) % k_protag_count]) +
                " as they confront " + std::string(k_desc_concept[(i + 2) % k_concept_count]) + ".";

            if (!first_val) sql += ", ";
            first_val = false;

            // Format rating with one decimal place
            char rating_buf[16];
            std::snprintf(rating_buf, sizeof(rating_buf), "%.1f", rating);

            sql += "(";
            sql += std::to_string(i + 1);
            sql += ", '";
            sql += esc(title);
            sql += "', '";
            sql += esc(genre);
            sql += "', ";
            sql += std::to_string(pub_year);
            sql += ", ";
            sql += std::to_string(pages);
            sql += ", ";
            sql += rating_buf;
            sql += ", '";
            sql += esc(desc);
            sql += "')";
        }
        DEMO_EXEC(engine, sql);
        row = end;
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Seed readers (500 rows, batches of 50)
// ---------------------------------------------------------------------------
static Result<void> seed_readers(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding {} readers", k_reader_count);

    static const char* const k_handle_adj[] = {
        "avid",   "eager",  "swift",  "quiet", "bright",
        "bold",   "gentle", "keen",   "sharp", "wise",
    };
    static constexpr int k_handle_adj_count = 10;

    static const char* const k_handle_noun[] = {
        "reader",   "bookworm", "librarian", "scholar",  "narrator",
        "reviewer", "browser",  "listener",  "collector","archivist",
    };
    static constexpr int k_handle_noun_count = 10;

    constexpr int batch = 50;
    int row = 0;
    while (row < k_reader_count) {
        std::string sql =
            "INSERT INTO readers (id, username, email, city, joined_year) VALUES ";
        bool first_val = true;
        int end = std::min(row + batch, k_reader_count);
        for (int i = row; i < end; ++i) {
            std::string username = std::string(k_handle_adj[i % k_handle_adj_count]) + "_" +
                                   std::string(k_handle_noun[(i / k_handle_adj_count) %
                                                             k_handle_noun_count]) +
                                   std::to_string(i + 1);
            std::string email    = username + "@bookmail.example";
            const char* city     = k_cities[i % k_city_count];
            int joined_year      = 2010 + (i * 3) % 14; // 2010–2023

            if (!first_val) sql += ", ";
            first_val = false;
            sql += "(";
            sql += std::to_string(i + 1);
            sql += ", '";
            sql += esc(username);
            sql += "', '";
            sql += esc(email);
            sql += "', '";
            sql += esc(city);
            sql += "', ";
            sql += std::to_string(joined_year);
            sql += ")";
        }
        DEMO_EXEC(engine, sql);
        row = end;
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Seed tags (30 rows, single INSERT)
// ---------------------------------------------------------------------------
static Result<void> seed_tags(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding {} tags", k_tag_count);

    std::string sql = "INSERT INTO tags (id, name) VALUES ";
    for (int i = 0; i < k_tag_count; ++i) {
        if (i > 0) sql += ", ";
        sql += "(";
        sql += std::to_string(i + 1);
        sql += ", '";
        sql += k_tags[i];
        sql += "')";
    }
    DEMO_EXEC(engine, sql);
    return ok();
}

// ---------------------------------------------------------------------------
// Seed reviews (1000 rows, batches of 50)
// ---------------------------------------------------------------------------
static Result<void> seed_reviews(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding {} reviews", k_review_count);

    constexpr int batch = 50;
    int row = 0;
    while (row < k_review_count) {
        std::string sql =
            "INSERT INTO reviews (id, book_id, reader_id, stars, review_text) VALUES ";
        bool first_val = true;
        int end = std::min(row + batch, k_review_count);
        for (int i = row; i < end; ++i) {
            int book_id   = (i % k_book_count) + 1;
            int reader_id = (i % k_reader_count) + 1;
            int stars     = 1 + (i * 7 + 3) % 5; // 1–5

            // Build review sentence deterministically
            std::string review_text =
                "An absolutely " +
                std::string(k_review_adj[i % k_review_adj_count]) +
                " read. The " +
                std::string(k_review_aspect[(i + 2) % k_review_aspect_count]) +
                " was " +
                std::string(k_review_assessment[(i + 1) % k_review_assessment_count]) +
                " and I could not put it down.";

            if (!first_val) sql += ", ";
            first_val = false;
            sql += "(";
            sql += std::to_string(i + 1);
            sql += ", ";
            sql += std::to_string(book_id);
            sql += ", ";
            sql += std::to_string(reader_id);
            sql += ", ";
            sql += std::to_string(stars);
            sql += ", '";
            sql += esc(review_text);
            sql += "')";
        }
        DEMO_EXEC(engine, sql);
        row = end;
    }
    return ok();
}

// ---------------------------------------------------------------------------
// Seed edges
// ---------------------------------------------------------------------------
static Result<void> seed_edges(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: seeding graph edges");

    // --- wrote: 1–2 authors per book ---
    {
        std::string sql = "LINK authors TO books VIA wrote VALUES ";
        bool first_val = true;
        for (int book = 1; book <= k_book_count; ++book) {
            int author1 = ((book - 1) % k_author_count) + 1;
            if (!first_val) sql += ", ";
            first_val = false;
            sql += "(";
            sql += std::to_string(author1);
            sql += ", ";
            sql += std::to_string(book);
            sql += ")";

            // Every 3rd book gets a second author
            if (book % 3 == 0) {
                int author2 = ((book + 10) % k_author_count) + 1;
                if (author2 != author1) {
                    sql += ", (";
                    sql += std::to_string(author2);
                    sql += ", ";
                    sql += std::to_string(book);
                    sql += ")";
                }
            }
        }
        DEMO_EXEC(engine, sql);
    }

    // --- reviewed_by: one edge per review ---
    // Batch in groups of 100 to keep SQL strings manageable
    {
        constexpr int batch = 100;
        int row = 0;
        while (row < k_review_count) {
            std::string sql = "LINK books TO readers VIA reviewed_by VALUES ";
            bool first_val = true;
            int end = std::min(row + batch, k_review_count);
            for (int i = row; i < end; ++i) {
                int book_id   = (i % k_book_count) + 1;
                int reader_id = (i % k_reader_count) + 1;
                if (!first_val) sql += ", ";
                first_val = false;
                sql += "(";
                sql += std::to_string(book_id);
                sql += ", ";
                sql += std::to_string(reader_id);
                sql += ")";
            }
            DEMO_EXEC(engine, sql);
            row = end;
        }
    }

    // --- follows: each reader follows 5–8 others ---
    // Use varied step sizes so the social graph fans out quickly and
    // every city is reachable within 2–3 hops from any starting reader.
    {
        constexpr int batch = 100;
        struct FollowPair { int src; int tgt; };
        std::vector<FollowPair> follows;
        follows.reserve(static_cast<size_t>(k_reader_count) * 8);
        // Step sizes chosen to cover all 20 city residues within 2 hops.
        static constexpr int steps[] = {1, 3, 7, 13, 31, 67, 137, 251};
        static constexpr int step_count = 8;
        for (int r = 1; r <= k_reader_count; ++r) {
            // Each reader follows 5–8 others depending on their ID.
            int n_follows = 5 + (r % 4); // 5, 6, 7, or 8
            for (int j = 0; j < n_follows; ++j) {
                int tgt = ((r - 1 + steps[j % step_count]) % k_reader_count) + 1;
                if (tgt != r) {
                    follows.push_back({r, tgt});
                }
            }
        }

        int total = static_cast<int>(follows.size());
        int row = 0;
        while (row < total) {
            std::string sql = "LINK readers TO readers VIA follows VALUES ";
            bool first_val = true;
            int end = std::min(row + batch, total);
            for (int i = row; i < end; ++i) {
                if (!first_val) sql += ", ";
                first_val = false;
                sql += "(";
                sql += std::to_string(follows[i].src);
                sql += ", ";
                sql += std::to_string(follows[i].tgt);
                sql += ")";
            }
            DEMO_EXEC(engine, sql);
            row = end;
        }
    }

    // --- tagged_as: 1–3 tags per book ---
    {
        constexpr int batch = 100;
        struct TagPair { int book; int tag; };
        std::vector<TagPair> tagged;
        tagged.reserve(static_cast<size_t>(k_book_count) * 3);
        for (int b = 1; b <= k_book_count; ++b) {
            int n_tags = 1 + (b % 3); // 1, 2, or 3 tags
            for (int t = 0; t < n_tags; ++t) {
                int tag_id = ((b - 1 + t * 7) % k_tag_count) + 1;
                tagged.push_back({b, tag_id});
            }
        }

        int total = static_cast<int>(tagged.size());
        int row = 0;
        while (row < total) {
            std::string sql = "LINK books TO tags VIA tagged_as VALUES ";
            bool first_val = true;
            int end = std::min(row + batch, total);
            for (int i = row; i < end; ++i) {
                if (!first_val) sql += ", ";
                first_val = false;
                sql += "(";
                sql += std::to_string(tagged[i].book);
                sql += ", ";
                sql += std::to_string(tagged[i].tag);
                sql += ")";
            }
            DEMO_EXEC(engine, sql);
            row = end;
        }
    }

    return ok();
}

#undef DEMO_EXEC

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
Result<void> create_demo_database(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: bootstrapping 'SixSeven Bookstore' demo dataset");

    auto r_schema = create_schema(engine);
    if (!r_schema) return r_schema;

    auto r_tags = seed_tags(engine);
    if (!r_tags) return r_tags;

    auto r_authors = seed_authors(engine);
    if (!r_authors) return r_authors;

    auto r_books = seed_books(engine);
    if (!r_books) return r_books;

    auto r_readers = seed_readers(engine);
    if (!r_readers) return r_readers;

    auto r_reviews = seed_reviews(engine);
    if (!r_reviews) return r_reviews;

    auto r_edges = seed_edges(engine);
    if (!r_edges) return r_edges;

    // NOTE: secondary indexes are intentionally NOT created here. They must be
    // built only after EMBEDDING vectors are generated (see create_demo_indexes),
    // because embedding write-backs move rows to new heap slots and would leave
    // any pre-existing index pointing at deleted slots.

    SIXSEVEN_LOG_INFO(
        "demo: dataset ready — {} authors, {} books, {} readers, {} reviews, {} tags",
        k_author_count, k_book_count, k_reader_count, k_review_count, k_tag_count);
    SIXSEVEN_LOG_INFO("demo: EMBEDDING vectors are generating in the background");

    return ok();
}

Result<void> create_demo_indexes(QueryEngine& engine) {
    SIXSEVEN_LOG_INFO("demo: building secondary indexes over seeded data");

    // Cover every column used in WHERE, JOIN, and ORDER BY across the demo
    // queries so nothing falls back to a sequential scan. CREATE INDEX scans
    // the now-stable heap and populates the index from existing rows.
    static const char* const k_index_ddl[] = {
        "CREATE INDEX idx_books_genre    ON books(genre)",
        "CREATE INDEX idx_books_rating   ON books(rating)",
        "CREATE INDEX idx_books_pubyear  ON books(published_year)",
        "CREATE INDEX idx_reviews_book   ON reviews(book_id)",
        "CREATE INDEX idx_reviews_reader ON reviews(reader_id)",
        "CREATE INDEX idx_reviews_stars  ON reviews(stars)",
        "CREATE INDEX idx_readers_city   ON readers(city)",
        "CREATE INDEX idx_readers_joined ON readers(joined_year)",
        "CREATE INDEX idx_authors_nat    ON authors(nationality)",
    };
    for (const char* ddl : k_index_ddl) {
        auto r = engine.execute(ddl);
        if (!r) {
            return make_error(r.error().code, "demo indexes: " + r.error().message);
        }
    }
    return ok();
}

} // namespace sixseven
