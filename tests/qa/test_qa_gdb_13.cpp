/// @file test_qa_gdb_13.cpp
/// @brief Adversarial QA tests for GDB-13: Page Layout
///
/// Covers: GDB-82 (slotted page), GDB-83 (tuple layout), GDB-84 (overflow pages).
/// Focuses on edge cases, boundary values, error paths, and adversarial sequences
/// not covered by the implementation's own test suite.

#include "sixseven/storage/overflow.h"
#include "sixseven/storage/page.h"
#include "sixseven/table/tuple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

using namespace sixseven;

// ============================================================================
// Helpers
// ============================================================================

static std::vector<uint8_t> make_bytes(size_t len, uint8_t fill = 0xAA) {
    return std::vector<uint8_t>(len, fill);
}

static std::vector<uint8_t> make_pattern(size_t len) {
    std::vector<uint8_t> v(len);
    for (size_t i = 0; i < len; ++i) {
        v[i] = static_cast<uint8_t>(i % 256);
    }
    return v;
}

// ============================================================================
// GDB-82: Slotted page — adversarial tests
// ============================================================================

// --- Boundary: insert exactly free_space() bytes ----------------------------

TEST(QA_Page, InsertExactlyFreeSpace) {
    Page page(1, PageType::DATA);
    size_t avail = page.free_space();
    ASSERT_GT(avail, 0u);

    auto data = make_bytes(avail, 0x42);
    auto result = page.insert_tuple(data);
    ASSERT_TRUE(result.has_value()) << "Insert of exactly free_space() bytes should succeed";

    // No more room even for a 1-byte tuple.
    auto tiny = make_bytes(1, 0x01);
    auto result2 = page.insert_tuple(tiny);
    EXPECT_FALSE(result2.has_value());
}

// --- Boundary: insert free_space()+1 bytes must fail -----------------------

TEST(QA_Page, InsertOneBeyondFreeSpaceFails) {
    Page page(1, PageType::DATA);
    size_t avail = page.free_space();

    auto data = make_bytes(avail + 1, 0x99);
    auto result = page.insert_tuple(data);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Boundary: 1-byte tuple -------------------------------------------------

TEST(QA_Page, InsertMinimumSizeTuple) {
    Page page(1, PageType::DATA);
    std::vector<uint8_t> one_byte = {0xFF};
    auto result = page.insert_tuple(one_byte);
    ASSERT_TRUE(result.has_value());

    auto read = page.get_tuple(*result);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), 1u);
    EXPECT_EQ((*read)[0], 0xFF);
}

// --- BUG REPRODUCTION: update_tuple data corruption on failure ---------------
//
// When update_tuple fails because the new data is too large, it should leave
// the original tuple data intact. Instead it zeros the original tuple data
// before doing the space check, then restores the slot entry pointing to
// the now-zeroed memory.

TEST(QA_Page, UpdateGrowFailureDoesNotCorruptOriginalData) {
    Page page(1, PageType::DATA);

    // Insert a 1000-byte tuple so the page has ~7100 bytes of free space.
    auto original = make_bytes(1000, 0xAB);
    auto slot_result = page.insert_tuple(original);
    ASSERT_TRUE(slot_result.has_value());
    SlotId slot = *slot_result;

    // Exhaust most of the remaining free space with another tuple so that
    // the update cannot grow the first tuple.
    size_t remaining = page.free_space();
    auto filler = make_bytes(remaining - 1, 0x00); // leave 1 byte reported free
    auto filler_result = page.insert_tuple(filler);
    ASSERT_TRUE(filler_result.has_value());

    // Now try to grow slot 0 by 200 bytes — there is no contiguous space.
    auto bigger = make_bytes(1200, 0xCD);
    auto update_result = page.update_tuple(slot, bigger);
    EXPECT_FALSE(update_result.has_value()) << "Update should fail due to lack of space";

    // *** Verify the original data is still intact ***
    auto read = page.get_tuple(slot);
    ASSERT_TRUE(read.has_value()) << "Slot should still be valid after failed update";
    ASSERT_EQ(read->size(), 1000u) << "Original tuple length must be preserved";

    bool all_original = true;
    for (uint8_t b : *read) {
        if (b != 0xAB) {
            all_original = false;
            break;
        }
    }
    EXPECT_TRUE(all_original) << "Original tuple data must not be corrupted by a failed update";
}

// --- Variant: single-tuple page, grow update fails -------------------------

TEST(QA_Page, UpdateGrowSingleTuplePageFailure) {
    // A page with a single large tuple leaving very little free space.
    Page page(1, PageType::DATA);

    // Insert a tuple that fills almost all the page.
    // free_space() accounts for one slot entry; use 90% to leave some slots
    // but not enough for a bigger tuple.
    size_t initial_free = page.free_space();
    size_t tuple_size = initial_free - 100; // leave 100 bytes free
    auto original = make_bytes(tuple_size, 0xDE);
    auto slot_result = page.insert_tuple(original);
    ASSERT_TRUE(slot_result.has_value());
    SlotId slot = *slot_result;

    // Attempt to grow by 200 bytes — should fail (only ~100 bytes free).
    auto bigger = make_bytes(tuple_size + 200, 0xBE);
    auto update_result = page.update_tuple(slot, bigger);
    EXPECT_FALSE(update_result.has_value());

    // Original data must be intact.
    auto read = page.get_tuple(slot);
    ASSERT_TRUE(read.has_value());
    ASSERT_EQ(read->size(), tuple_size);
    for (uint8_t b : *read) {
        EXPECT_EQ(b, 0xDE);
    }
}

// --- Compact reclaims the space freed by update-shrink ---------------------

TEST(QA_Page, CompactAfterShrinkingUpdate) {
    Page page(1, PageType::DATA);

    // Insert 3 tuples of 500 bytes each.
    for (int i = 0; i < 3; ++i) {
        auto r = page.insert_tuple(make_bytes(500, static_cast<uint8_t>(i + 1)));
        ASSERT_TRUE(r.has_value()) << "Insert " << i << " failed";
    }

    // Shrink tuple 1 in-place from 500 to 100.
    auto r = page.update_tuple(1, make_bytes(100, 0xFF));
    ASSERT_TRUE(r.has_value());

    size_t before_compact = page.free_space();
    page.compact();
    size_t after_compact = page.free_space();

    // Compact should recover the 400 bytes that the shrunk-in-place tuple left
    // as internal fragmentation. update_tuple shrink-in-place keeps data_offset
    // unchanged (same slot offset, shorter length), so the 400-byte slack is
    // invisible to free_space() until compact() repacks by slot length.
    // The delta is deterministic: old_len(500) - new_len(100) = 400 bytes.
    // EXPECT_GE was vacuous — it passed even for a no-op compact (after==before).
    EXPECT_EQ(after_compact, before_compact + 400);

    // All live tuples still readable.
    auto t0 = page.get_tuple(0);
    ASSERT_TRUE(t0.has_value());
    EXPECT_EQ((*t0)[0], 0x01);

    auto t1 = page.get_tuple(1);
    ASSERT_TRUE(t1.has_value());
    EXPECT_EQ(t1->size(), 100u);
    EXPECT_EQ((*t1)[0], 0xFF);

    auto t2 = page.get_tuple(2);
    ASSERT_TRUE(t2.has_value());
    EXPECT_EQ((*t2)[0], 0x03);
}

// --- Compact: delete all, then insert fresh --------------------------------

TEST(QA_Page, CompactAllDeletedThenInsert) {
    Page page(1, PageType::DATA);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(page.insert_tuple(make_bytes(200, static_cast<uint8_t>(i))).has_value());
    }
    for (SlotId s = 0; s < 5; ++s) {
        ASSERT_TRUE(page.delete_tuple(s).has_value());
    }

    page.compact();

    // Should be able to insert again after compacting all-deleted.
    auto r = page.insert_tuple(make_bytes(500, 0xEE));
    ASSERT_TRUE(r.has_value()) << "Should be able to insert after compacting all-deleted page";
    auto read = page.get_tuple(*r);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 500u);
}

// --- Slot reuse does not advance slot_count --------------------------------

TEST(QA_Page, SlotReuseDoesNotGrowDirectory) {
    Page page(1, PageType::DATA);

    ASSERT_TRUE(page.insert_tuple(make_bytes(10, 0x01)).has_value()); // slot 0
    ASSERT_TRUE(page.insert_tuple(make_bytes(10, 0x02)).has_value()); // slot 1
    ASSERT_TRUE(page.insert_tuple(make_bytes(10, 0x03)).has_value()); // slot 2

    ASSERT_TRUE(page.delete_tuple(0).has_value());
    ASSERT_TRUE(page.delete_tuple(1).has_value());

    uint16_t count_before = page.slot_count();

    // Reuse slot 0.
    auto r = page.insert_tuple(make_bytes(10, 0x44));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0u); // Should reuse slot 0 first.

    // slot_count should NOT increase.
    EXPECT_EQ(page.slot_count(), count_before);
}

// --- Multiple inserts / deletes / compact cycle ----------------------------

TEST(QA_Page, InsertDeleteCompactCycle) {
    Page page(1, PageType::DATA);
    constexpr int ROUNDS = 5;

    for (int round = 0; round < ROUNDS; ++round) {
        // Insert until full.
        std::vector<SlotId> slots;
        while (true) {
            auto r = page.insert_tuple(make_bytes(64, static_cast<uint8_t>(round)));
            if (!r.has_value()) {
                break;
            }
            slots.push_back(*r);
        }
        EXPECT_GT(slots.size(), 0u) << "Round " << round << ": no tuples inserted";

        // Delete every other tuple.
        for (size_t i = 0; i < slots.size(); i += 2) {
            // Only delete if it's still a live slot.
            auto rd = page.get_tuple(slots[i]);
            if (rd.has_value()) {
                ASSERT_TRUE(page.delete_tuple(slots[i]).has_value());
            }
        }

        page.compact();
    }
}

// --- Data integrity across raw() round-trip --------------------------------

TEST(QA_Page, RawBytesRoundTrip) {
    Page page(99, PageType::BTREE_LEAF);
    page.set_lsn(12345);
    page.set_checksum(0xDEADCAFE);

    auto p1 = make_pattern(100);
    auto p2 = make_pattern(200);
    ASSERT_TRUE(page.insert_tuple(p1).has_value());
    ASSERT_TRUE(page.insert_tuple(p2).has_value());

    // Re-construct page from raw bytes.
    Page copy(page.raw());
    EXPECT_EQ(copy.page_id(), 99u);
    EXPECT_EQ(copy.page_type(), PageType::BTREE_LEAF);
    EXPECT_EQ(copy.lsn(), 12345u);
    EXPECT_EQ(copy.checksum(), 0xDEADCAFEu);
    EXPECT_EQ(copy.slot_count(), 2u);

    auto r0 = copy.get_tuple(0);
    ASSERT_TRUE(r0.has_value());
    ASSERT_EQ(r0->size(), 100u);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ((*r0)[i], static_cast<uint8_t>(i % 256));
    }

    auto r1 = copy.get_tuple(1);
    ASSERT_TRUE(r1.has_value());
    ASSERT_EQ(r1->size(), 200u);
    for (size_t i = 0; i < 200; ++i) {
        EXPECT_EQ((*r1)[i], static_cast<uint8_t>(i % 256));
    }
}

// --- All PageType values are stored and retrieved correctly ----------------

TEST(QA_Page, AllPageTypes) {
    const PageType types[] = {
        PageType::DATA,
        PageType::BTREE_INTERNAL,
        PageType::BTREE_LEAF,
        PageType::OVERFLOW_PAGE,
        PageType::FREE_LIST,
        PageType::HNSW_NODE,
        PageType::HNSW_META,
        PageType::HNSW_VECTOR_DATA,
    };
    for (auto t : types) {
        Page page(0, t);
        EXPECT_EQ(page.page_type(), t);
        page.set_page_type(t);
        EXPECT_EQ(page.page_type(), t);
    }
}

// --- Stress: fill page with 1-byte tuples, verify count --------------------

TEST(QA_Page, FillWithMinimalTuples) {
    Page page(1, PageType::DATA);
    int count = 0;
    while (true) {
        std::vector<uint8_t> one = {static_cast<uint8_t>(count % 256)};
        auto r = page.insert_tuple(one);
        if (!r.has_value()) {
            break;
        }
        ++count;
    }
    EXPECT_GT(count, 0);

    // Verify every inserted tuple.
    for (int i = 0; i < count; ++i) {
        auto r = page.get_tuple(static_cast<SlotId>(i));
        ASSERT_TRUE(r.has_value()) << "Slot " << i << " should be readable";
        ASSERT_EQ(r->size(), 1u);
        EXPECT_EQ((*r)[0], static_cast<uint8_t>(i % 256));
    }
}

// --- Update to same length (should be in-place, no data_offset change) -----

TEST(QA_Page, UpdateSameLengthPreservesDataOffset) {
    Page page(1, PageType::DATA);
    ASSERT_TRUE(page.insert_tuple(make_bytes(100, 0x11)).has_value());
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x22)).has_value());

    size_t free_before = page.free_space();

    // Update slot 0 with same length.
    auto r = page.update_tuple(0, make_bytes(100, 0x33));
    ASSERT_TRUE(r.has_value());

    // free_space() should be identical (in-place update consumes no extra space).
    EXPECT_EQ(page.free_space(), free_before);

    auto read = page.get_tuple(0);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 100u);
    EXPECT_EQ((*read)[0], 0x33);
}

// --- Compact preserves data integrity for mixed sizes ----------------------

TEST(QA_Page, CompactDataIntegrity) {
    Page page(1, PageType::DATA);

    // Insert varying-size tuples with known patterns.
    ASSERT_TRUE(page.insert_tuple(make_bytes(50, 0x01)).has_value());  // slot 0
    ASSERT_TRUE(page.insert_tuple(make_bytes(150, 0x02)).has_value()); // slot 1
    ASSERT_TRUE(page.insert_tuple(make_bytes(75, 0x03)).has_value());  // slot 2
    ASSERT_TRUE(page.insert_tuple(make_bytes(200, 0x04)).has_value()); // slot 3
    ASSERT_TRUE(page.insert_tuple(make_bytes(25, 0x05)).has_value());  // slot 4

    // Delete non-contiguous slots.
    ASSERT_TRUE(page.delete_tuple(1).has_value());
    ASSERT_TRUE(page.delete_tuple(3).has_value());

    page.compact();

    // Verify surviving tuples.
    auto r0 = page.get_tuple(0);
    ASSERT_TRUE(r0.has_value());
    EXPECT_EQ(r0->size(), 50u);
    EXPECT_EQ((*r0)[0], 0x01);

    // Slot 1 is deleted.
    EXPECT_FALSE(page.get_tuple(1).has_value());

    auto r2 = page.get_tuple(2);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->size(), 75u);
    EXPECT_EQ((*r2)[0], 0x03);

    // Slot 3 is deleted.
    EXPECT_FALSE(page.get_tuple(3).has_value());

    auto r4 = page.get_tuple(4);
    ASSERT_TRUE(r4.has_value());
    EXPECT_EQ(r4->size(), 25u);
    EXPECT_EQ((*r4)[0], 0x05);
}

// ============================================================================
// GDB-83: Tuple layout — adversarial tests
// ============================================================================

// --- All-null schema with only variable-length columns ----------------------

TEST(QA_TupleSerializer, AllNullVariableColumns) {
    Schema schema({
        {"a", TypeId::STRING},
        {"b", TypeId::BLOB},
        {"c", TypeId::JSON},
    });

    std::vector<Value> values = {Value::make_null(), Value::make_null(), Value::make_null()};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE((*result)[i].is_null()) << "column " << i << " should be null";
    }
}

// --- Multiple consecutive empty strings ------------------------------------

TEST(QA_TupleSerializer, MultipleEmptyStrings) {
    Schema schema({
        {"a", TypeId::STRING},
        {"b", TypeId::STRING},
        {"c", TypeId::STRING},
    });

    std::vector<Value> values = {
        Value(std::string{""}),
        Value(std::string{""}),
        Value(std::string{""}),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 3u);

    for (size_t i = 0; i < 3; ++i) {
        ASSERT_FALSE((*result)[i].is_null()) << "column " << i << " should not be null";
        EXPECT_EQ((*result)[i].as_string(), "") << "column " << i;
    }
}

// --- Interleaved null and non-null variable-length fields ------------------

TEST(QA_TupleSerializer, InterleavedNullAndNonNullVarFields) {
    Schema schema({
        {"a", TypeId::STRING},
        {"b", TypeId::STRING},
        {"c", TypeId::STRING},
        {"d", TypeId::STRING},
    });

    std::vector<Value> values = {
        Value(std::string{"hello"}),
        Value::make_null(),
        Value(std::string{"world"}),
        Value::make_null(),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4u);

    EXPECT_EQ((*result)[0].as_string(), "hello");
    EXPECT_TRUE((*result)[1].is_null());
    EXPECT_EQ((*result)[2].as_string(), "world");
    EXPECT_TRUE((*result)[3].is_null());
}

// --- Null bitmap boundary: exactly 8 columns (1 byte) ----------------------

TEST(QA_TupleSerializer, NullBitmapExactly8Cols) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 8; ++i) {
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    }
    Schema schema(cols);
    EXPECT_EQ(schema.null_bitmap_size(), 1u);

    std::vector<Value> values;
    for (int i = 0; i < 8; ++i) {
        values.push_back(Value(int32_t{i}));
    }

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ((*result)[i].as_int32(), i) << "col " << i;
    }
}

// --- Null bitmap boundary: exactly 9 columns (2 bytes) ----------------------

TEST(QA_TupleSerializer, NullBitmapExactly9Cols) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 9; ++i) {
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    }
    Schema schema(cols);
    EXPECT_EQ(schema.null_bitmap_size(), 2u);

    std::vector<Value> values;
    for (int i = 0; i < 9; ++i) {
        values.push_back(Value(int32_t{100 + i}));
    }

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ((*result)[i].as_int32(), 100 + i) << "col " << i;
    }
}

// --- Null in the 9th column (second bitmap byte) ---------------------------

TEST(QA_TupleSerializer, NullInSecondBitmapByte) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < 9; ++i) {
        cols.push_back({"c" + std::to_string(i), TypeId::INT32});
    }
    Schema schema(cols);

    std::vector<Value> values;
    for (int i = 0; i < 8; ++i) {
        values.push_back(Value(int32_t{i}));
    }
    values.push_back(Value::make_null()); // col 8 null — in second bitmap byte

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 9u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ((*result)[i].as_int32(), i) << "col " << i;
    }
    EXPECT_TRUE((*result)[8].is_null());
}

// --- compute_tuple_size matches actual serialized size ---------------------

TEST(QA_TupleSerializer, ComputeSizeMatchesActual) {
    Schema schema({
        {"id", TypeId::INT64},
        {"name", TypeId::STRING},
        {"score", TypeId::FLOAT64},
        {"bio", TypeId::BLOB},
        {"active", TypeId::BOOL},
    });

    std::vector<Value> values = {
        Value(int64_t{42}),
        Value(std::string{"Alice"}),
        Value(3.14),
        Value(Blob{0x01, 0x02, 0x03}),
        Value(true),
    };

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    EXPECT_EQ(computed, buf->size());
}

// --- compute_tuple_size with all-null values --------------------------------

TEST(QA_TupleSerializer, ComputeSizeAllNulls) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING},
    });

    std::vector<Value> values = {Value::make_null(), Value::make_null()};

    size_t computed = TupleSerializer::compute_tuple_size(values, schema);

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    EXPECT_EQ(computed, buf->size());
}

// --- get_field for all column positions in mixed schema -------------------

TEST(QA_TupleSerializer, GetFieldAllPositions) {
    Schema schema({
        {"a", TypeId::INT32},
        {"b", TypeId::STRING},
        {"c", TypeId::BOOL},
        {"d", TypeId::BLOB},
        {"e", TypeId::INT64},
    });

    std::vector<Value> values = {
        Value(int32_t{1}),
        Value(std::string{"hello"}),
        Value(true),
        Value(Blob{0xDE, 0xAD}),
        Value(int64_t{999}),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    // verify every field matches deserialize output
    auto full = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(full.has_value());

    for (size_t i = 0; i < 5; ++i) {
        auto f = TupleSerializer::get_field(*buf, schema, i);
        ASSERT_TRUE(f.has_value()) << "get_field col " << i;
        EXPECT_EQ(f->type_id(), (*full)[i].type_id()) << "type mismatch at col " << i;
    }

    EXPECT_EQ(TupleSerializer::get_field(*buf, schema, 0)->as_int32(), 1);
    EXPECT_EQ(TupleSerializer::get_field(*buf, schema, 1)->as_string(), "hello");
    EXPECT_EQ(TupleSerializer::get_field(*buf, schema, 2)->as_bool(), true);
    EXPECT_EQ(TupleSerializer::get_field(*buf, schema, 3)->as_blob(), (Blob{0xDE, 0xAD}));
    EXPECT_EQ(TupleSerializer::get_field(*buf, schema, 4)->as_int64(), 999);
}

// --- Tuple with only fixed columns (no var-length overhead) ---------------

TEST(QA_TupleSerializer, PureFixedSchema) {
    Schema schema({
        {"a", TypeId::INT8},
        {"b", TypeId::INT64},
        {"c", TypeId::BOOL},
        {"d", TypeId::FLOAT32},
    });

    // 0 variable-length columns => no var offset table
    EXPECT_EQ(schema.variable_column_count(), 0u);
    EXPECT_EQ(schema.fixed_column_count(), 4u);

    std::vector<Value> values = {
        Value(int8_t{-1}),
        Value(int64_t{0x7FFFFFFFFFFFFFFF}),
        Value(false),
        Value(0.001f),
    };

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ((*result)[0].as_int8(), -1);
    EXPECT_EQ((*result)[1].as_int64(), 0x7FFFFFFFFFFFFFFF);
    EXPECT_EQ((*result)[2].as_bool(), false);
    EXPECT_FLOAT_EQ((*result)[3].as_float32(), 0.001f);
}

// --- Extremely long string (near max_tuple_size) ---------------------------

TEST(QA_TupleSerializer, NearMaxTupleSize) {
    Schema schema({{"data", TypeId::STRING}});

    // Total = bitmap(1) + fixed(0) + var_table(4) + var_data(N)
    // max_tuple_size = 65535, so N = 65535 - 5 = 65530
    size_t str_len = max_tuple_size - 1 /* bitmap */ - 4 /* var_entry */;
    std::string large(str_len, 'z');
    std::vector<Value> values = {Value(large)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());
    EXPECT_EQ(buf->size(), max_tuple_size);

    auto result = TupleSerializer::deserialize(*buf, schema);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1u);
    EXPECT_EQ((*result)[0].as_string().size(), str_len);
    EXPECT_EQ((*result)[0].as_string(), large);
}

// --- Tuple exceeding max_tuple_size must fail -------------------------------

TEST(QA_TupleSerializer, TupleExceedingMaxSizeFails) {
    Schema schema({{"data", TypeId::STRING}});

    // One byte more than max_tuple_size.
    size_t str_len = max_tuple_size - 1 /* bitmap */ - 4 /* var_entry */ + 1;
    std::string too_large(str_len, 'x');
    std::vector<Value> values = {Value(too_large)};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_FALSE(buf.has_value());
    EXPECT_EQ(buf.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Serialize then mutate buffer then deserialize detects corruption ------

TEST(QA_TupleSerializer, CorruptedBufferDetected) {
    Schema schema({{"id", TypeId::INT32}, {"name", TypeId::STRING}});
    std::vector<Value> values = {Value(int32_t{42}), Value(std::string{"test"})};

    auto buf = TupleSerializer::serialize(values, schema);
    ASSERT_TRUE(buf.has_value());

    // Truncate: remove the last byte (corrupts the string data).
    buf->pop_back();
    auto result = TupleSerializer::deserialize(*buf, schema);
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// GDB-84: Overflow pages — adversarial tests
// ============================================================================

// --- Boundary: exactly overflow_threshold bytes (not overflow) -------------

TEST(QA_Overflow, ExactlyThresholdNeedsNoOverflow) {
    EXPECT_FALSE(OverflowManager::needs_overflow(overflow_threshold));
}

// --- Boundary: threshold+1 needs overflow ----------------------------------

TEST(QA_Overflow, ThresholdPlusOneNeedsOverflow) {
    EXPECT_TRUE(OverflowManager::needs_overflow(overflow_threshold + 1));
}

// --- Boundary: exactly 1 byte below chunk boundary ------------------------

TEST(QA_Overflow, OneByteBeforeChunkBoundary) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_pattern(overflow_chunk_capacity - 1);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());
    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

// --- Boundary: exactly overflow_chunk_capacity bytes -----------------------

TEST(QA_Overflow, ExactlyChunkCapacity) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_pattern(overflow_chunk_capacity);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());
    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

// --- Boundary: exactly overflow_chunk_capacity + 1 (two pages) ------------

TEST(QA_Overflow, ChunkCapacityPlusOne) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_pattern(overflow_chunk_capacity + 1);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());
    EXPECT_EQ(alloc.allocated_count(), 2u);

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

// --- Store, free, re-store: allocator grows (no page reuse) ---------------

TEST(QA_Overflow, FreeAndRestore) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data1 = make_pattern(4096);
    auto ptr1 = mgr.store_overflow(data1);
    ASSERT_TRUE(ptr1.has_value());

    auto free1 = mgr.free_overflow(*ptr1);
    ASSERT_TRUE(free1.has_value());

    // Re-store the same data.
    auto data2 = make_pattern(4096);
    auto ptr2 = mgr.store_overflow(data2);
    ASSERT_TRUE(ptr2.has_value());

    auto read2 = mgr.read_overflow(*ptr2);
    ASSERT_TRUE(read2.has_value());
    EXPECT_EQ(*read2, data2);
}

// --- All-zeros data round-trip at multiple sizes ---------------------------

TEST(QA_Overflow, AllZerosAtVariousSizes) {
    const size_t sizes[] = {1,
                            overflow_threshold + 1,
                            overflow_chunk_capacity,
                            overflow_chunk_capacity * 3,
                            1024 * 1024};

    for (size_t sz : sizes) {
        InMemoryPageAllocator alloc;
        OverflowManager mgr(alloc);

        std::vector<uint8_t> zeros(sz, 0x00);
        auto ptr = mgr.store_overflow(zeros);
        ASSERT_TRUE(ptr.has_value()) << "store failed for size " << sz;

        auto read = mgr.read_overflow(*ptr);
        ASSERT_TRUE(read.has_value()) << "read failed for size " << sz;
        EXPECT_EQ(*read, zeros) << "data mismatch for size " << sz;
    }
}

// --- All-0xFF data round-trip -----------------------------------------------

TEST(QA_Overflow, AllOnesRoundTrip) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    std::vector<uint8_t> ones(overflow_chunk_capacity * 2 + 100, 0xFF);
    auto ptr = mgr.store_overflow(ones);
    ASSERT_TRUE(ptr.has_value());

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, ones);
}

// --- Free pointer with zero page_id but nonzero length fails ---------------

TEST(QA_Overflow, FreeZeroPageIdFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    OverflowPointer bad{0, 100};
    auto result = mgr.free_overflow(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- Read pointer with valid page_id but zero length fails -----------------

TEST(QA_Overflow, ReadZeroLengthFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    OverflowPointer bad{1, 0};
    auto result = mgr.read_overflow(bad);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- InMemoryPageAllocator: free page_id=0 must fail ----------------------

TEST(QA_InMemoryAllocator, FreeInvalidIdZeroFails) {
    InMemoryPageAllocator alloc;
    auto result = alloc.free_page(0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// --- InMemoryPageAllocator: get after double-free returns NOT_FOUND --------

TEST(QA_InMemoryAllocator, GetAfterDoubleFree) {
    InMemoryPageAllocator alloc;

    auto id = alloc.allocate_page(PageType::DATA);
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(alloc.free_page(*id).has_value());
    // Second free returns error.
    EXPECT_FALSE(alloc.free_page(*id).has_value());

    // Get after free returns error.
    auto get = alloc.get_page(*id);
    ASSERT_FALSE(get.has_value());
    EXPECT_EQ(get.error().code, StatusCode::NOT_FOUND);
}

// --- OverflowManager: page type must be OVERFLOW_PAGE ----------------------

TEST(QA_Overflow, StoredPagesHaveOverflowType) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    // 3-page chain.
    auto data = make_pattern(overflow_chunk_capacity * 2 + 50);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());

    // Walk the chain and verify all pages are OVERFLOW_PAGE type.
    uint32_t current = ptr->first_page_id;
    int pages_checked = 0;
    while (current != overflow_no_next_page) {
        auto page = alloc.get_page(current);
        ASSERT_TRUE(page.has_value());
        EXPECT_EQ((*page)->page_type(), PageType::OVERFLOW_PAGE)
            << "page " << current << " should be OVERFLOW_PAGE";

        uint32_t next = 0;
        std::memcpy(&next, &(*page)->raw()[overflow_next_page_offset], sizeof(uint32_t));
        current = next;
        ++pages_checked;
    }
    EXPECT_EQ(pages_checked, 3);
}

// --- Overflow free releases ALL pages in chain -----------------------------

TEST(QA_Overflow, FreeReleasesAllPagesInChain) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    // 5-page chain: 4*chunk + small remainder.
    size_t data_size = overflow_chunk_capacity * 4 + 100;
    auto data = make_pattern(data_size);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());

    size_t count_before = alloc.allocated_count();
    EXPECT_EQ(count_before, 5u);

    auto free_result = mgr.free_overflow(*ptr);
    ASSERT_TRUE(free_result.has_value());
    EXPECT_EQ(alloc.allocated_count(), 0u);
}

// --- Multiple independent chains do not interfere --------------------------

TEST(QA_Overflow, IndependentChainIntegrity) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    // Store 4 different chains with different patterns.
    const size_t sizes[] = {1000, overflow_chunk_capacity, overflow_chunk_capacity * 2 + 7, 500};
    std::vector<OverflowPointer> ptrs;
    std::vector<std::vector<uint8_t>> datas;

    for (size_t sz : sizes) {
        auto d = make_pattern(sz);
        datas.push_back(d);
        auto p = mgr.store_overflow(d);
        ASSERT_TRUE(p.has_value()) << "store failed for size " << sz;
        ptrs.push_back(*p);
    }

    // All chains must be independently readable and correct.
    for (size_t i = 0; i < ptrs.size(); ++i) {
        auto read = mgr.read_overflow(ptrs[i]);
        ASSERT_TRUE(read.has_value()) << "read failed for chain " << i;
        EXPECT_EQ(*read, datas[i]) << "data mismatch for chain " << i;
    }

    // Free chains 0 and 2, verify 1 and 3 still correct.
    ASSERT_TRUE(mgr.free_overflow(ptrs[0]).has_value());
    ASSERT_TRUE(mgr.free_overflow(ptrs[2]).has_value());

    auto read1 = mgr.read_overflow(ptrs[1]);
    ASSERT_TRUE(read1.has_value());
    EXPECT_EQ(*read1, datas[1]);

    auto read3 = mgr.read_overflow(ptrs[3]);
    ASSERT_TRUE(read3.has_value());
    EXPECT_EQ(*read3, datas[3]);
}
