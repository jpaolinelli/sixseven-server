#include "giodb/storage/overflow.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>

using namespace giodb;

// -- Helper: create a vector of bytes with a repeating pattern ----------------

static std::vector<uint8_t> make_data(size_t size) {
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    return data;
}

// -- OverflowPointer tests ----------------------------------------------------

TEST(OverflowPointer, DefaultValues) {
    OverflowPointer ptr;
    EXPECT_EQ(ptr.first_page_id, 0u);
    EXPECT_EQ(ptr.total_length, 0u);
}

TEST(OverflowPointer, Equality) {
    OverflowPointer a{1, 100};
    OverflowPointer b{1, 100};
    OverflowPointer c{2, 100};

    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

// -- Constants tests ----------------------------------------------------------

TEST(Overflow, Constants) {
    // Overflow data starts after header (24) + next_page_id (4) = 28 bytes.
    EXPECT_EQ(OVERFLOW_DATA_OFFSET, PAGE_HEADER_SIZE + sizeof(uint32_t));
    EXPECT_EQ(OVERFLOW_CHUNK_CAPACITY, PAGE_SIZE - OVERFLOW_DATA_OFFSET);
    // Chunk capacity should be PAGE_SIZE - 28 = 8164 bytes.
    EXPECT_EQ(OVERFLOW_CHUNK_CAPACITY, 8164u);
}

TEST(Overflow, NeedsOverflow) {
    EXPECT_FALSE(OverflowManager::needs_overflow(0));
    EXPECT_FALSE(OverflowManager::needs_overflow(1024));
    EXPECT_FALSE(OverflowManager::needs_overflow(OVERFLOW_THRESHOLD));
    EXPECT_TRUE(OverflowManager::needs_overflow(OVERFLOW_THRESHOLD + 1));
    EXPECT_TRUE(OverflowManager::needs_overflow(100000));
}

// -- InMemoryPageAllocator tests ----------------------------------------------

TEST(InMemoryPageAllocator, AllocateAndGet) {
    InMemoryPageAllocator alloc;

    auto id_result = alloc.allocate_page(PageType::OVERFLOW_PAGE);
    ASSERT_TRUE(id_result.has_value());
    uint32_t id = *id_result;
    EXPECT_EQ(id, 1u);

    auto page_result = alloc.get_page(id);
    ASSERT_TRUE(page_result.has_value());
    EXPECT_EQ((*page_result)->page_id(), id);
    EXPECT_EQ((*page_result)->page_type(), PageType::OVERFLOW_PAGE);
}

TEST(InMemoryPageAllocator, FreeAndRetrieve) {
    InMemoryPageAllocator alloc;

    auto id_result = alloc.allocate_page(PageType::DATA);
    ASSERT_TRUE(id_result.has_value());
    uint32_t id = *id_result;

    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto free_result = alloc.free_page(id);
    ASSERT_TRUE(free_result.has_value());
    EXPECT_EQ(alloc.allocated_count(), 0u);

    // Getting a freed page should fail.
    auto page_result = alloc.get_page(id);
    ASSERT_FALSE(page_result.has_value());
    EXPECT_EQ(page_result.error().code, StatusCode::NOT_FOUND);
}

TEST(InMemoryPageAllocator, InvalidPageId) {
    InMemoryPageAllocator alloc;

    auto result = alloc.get_page(0);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);

    auto result2 = alloc.get_page(99);
    ASSERT_FALSE(result2.has_value());
    EXPECT_EQ(result2.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(InMemoryPageAllocator, DoubleFree) {
    InMemoryPageAllocator alloc;

    auto id_result = alloc.allocate_page(PageType::DATA);
    ASSERT_TRUE(id_result.has_value());
    uint32_t id = *id_result;

    auto free1 = alloc.free_page(id);
    ASSERT_TRUE(free1.has_value());

    auto free2 = alloc.free_page(id);
    ASSERT_FALSE(free2.has_value());
    EXPECT_EQ(free2.error().code, StatusCode::NOT_FOUND);
}

// -- OverflowManager: store and read ------------------------------------------

TEST(OverflowManager, SmallValueSinglePage) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(1024); // 1KB — fits in a single overflow page.

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_NE(ptr.first_page_id, 0u);
    EXPECT_EQ(ptr.total_length, 1024u);
    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

TEST(OverflowManager, ExactlyOneChunk) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(OVERFLOW_CHUNK_CAPACITY); // Exactly fills one page.

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(ptr.total_length, static_cast<uint32_t>(OVERFLOW_CHUNK_CAPACITY));
    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

TEST(OverflowManager, TwoPages) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    // One byte more than fits in a single overflow page.
    auto data = make_data(OVERFLOW_CHUNK_CAPACITY + 1);

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(ptr.total_length, static_cast<uint32_t>(OVERFLOW_CHUNK_CAPACITY + 1));
    EXPECT_EQ(alloc.allocated_count(), 2u);

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

TEST(OverflowManager, MediumValue4KB) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(4096); // 4KB.

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(ptr.total_length, 4096u);
    EXPECT_EQ(alloc.allocated_count(), 1u); // 4KB < 8164, fits in one page.

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

TEST(OverflowManager, LargeValue32KB) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(32768); // 32KB.

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(ptr.total_length, 32768u);

    // 32768 / 8164 = 4.01..., so need 5 pages.
    size_t expected_pages = (32768 + OVERFLOW_CHUNK_CAPACITY - 1) / OVERFLOW_CHUNK_CAPACITY;
    EXPECT_EQ(expected_pages, 5u);
    EXPECT_EQ(alloc.allocated_count(), expected_pages);

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

TEST(OverflowManager, VeryLargeValue1MB) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    size_t size = 1024 * 1024; // 1MB.
    auto data = make_data(size);

    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(ptr.total_length, static_cast<uint32_t>(size));

    size_t expected_pages = (size + OVERFLOW_CHUNK_CAPACITY - 1) / OVERFLOW_CHUNK_CAPACITY;
    EXPECT_EQ(alloc.allocated_count(), expected_pages);

    auto read_result = mgr.read_overflow(ptr);
    ASSERT_TRUE(read_result.has_value());
    EXPECT_EQ(*read_result, data);
}

// -- OverflowManager: free ----------------------------------------------------

TEST(OverflowManager, FreeSinglePage) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(1024);
    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    EXPECT_EQ(alloc.allocated_count(), 1u);

    auto free_result = mgr.free_overflow(ptr);
    ASSERT_TRUE(free_result.has_value());
    EXPECT_EQ(alloc.allocated_count(), 0u);
}

TEST(OverflowManager, FreeMultiPageChain) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(32768); // 32KB -> 5 pages.
    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    size_t page_count = alloc.allocated_count();
    EXPECT_GT(page_count, 1u);

    auto free_result = mgr.free_overflow(ptr);
    ASSERT_TRUE(free_result.has_value());
    EXPECT_EQ(alloc.allocated_count(), 0u);
}

TEST(OverflowManager, ReadAfterFreeFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(2048);
    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    auto free_result = mgr.free_overflow(ptr);
    ASSERT_TRUE(free_result.has_value());

    // Reading freed overflow should fail.
    auto read_result = mgr.read_overflow(ptr);
    ASSERT_FALSE(read_result.has_value());
}

// -- OverflowManager: error cases ---------------------------------------------

TEST(OverflowManager, StoreEmptyDataFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    std::vector<uint8_t> empty;
    auto result = mgr.store_overflow(empty);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(OverflowManager, ReadInvalidPointerFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    OverflowPointer bad_ptr{0, 0};
    auto result = mgr.read_overflow(bad_ptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

TEST(OverflowManager, FreeInvalidPointerFails) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    OverflowPointer bad_ptr{0, 0};
    auto result = mgr.free_overflow(bad_ptr);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::INVALID_ARGUMENT);
}

// -- Multiple independent overflow chains -------------------------------------

TEST(OverflowManager, MultipleChains) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data1 = make_data(4096);
    auto data2 = make_data(16384);
    auto data3 = make_data(512);

    auto ptr1_result = mgr.store_overflow(data1);
    ASSERT_TRUE(ptr1_result.has_value());
    auto ptr2_result = mgr.store_overflow(data2);
    ASSERT_TRUE(ptr2_result.has_value());
    auto ptr3_result = mgr.store_overflow(data3);
    ASSERT_TRUE(ptr3_result.has_value());

    // All three should be independently readable.
    auto read1 = mgr.read_overflow(*ptr1_result);
    ASSERT_TRUE(read1.has_value());
    EXPECT_EQ(*read1, data1);

    auto read2 = mgr.read_overflow(*ptr2_result);
    ASSERT_TRUE(read2.has_value());
    EXPECT_EQ(*read2, data2);

    auto read3 = mgr.read_overflow(*ptr3_result);
    ASSERT_TRUE(read3.has_value());
    EXPECT_EQ(*read3, data3);

    // Free the middle one; others should still work.
    auto free2 = mgr.free_overflow(*ptr2_result);
    ASSERT_TRUE(free2.has_value());

    auto reread1 = mgr.read_overflow(*ptr1_result);
    ASSERT_TRUE(reread1.has_value());
    EXPECT_EQ(*reread1, data1);

    auto reread3 = mgr.read_overflow(*ptr3_result);
    ASSERT_TRUE(reread3.has_value());
    EXPECT_EQ(*reread3, data3);
}

// -- Overflow page type verification ------------------------------------------

TEST(OverflowManager, PagesHaveOverflowType) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    auto data = make_data(OVERFLOW_CHUNK_CAPACITY + 100); // 2 pages.
    auto ptr_result = mgr.store_overflow(data);
    ASSERT_TRUE(ptr_result.has_value());
    OverflowPointer ptr = *ptr_result;

    // Verify each page in the chain has OVERFLOW_PAGE type.
    auto page1 = alloc.get_page(ptr.first_page_id);
    ASSERT_TRUE(page1.has_value());
    EXPECT_EQ((*page1)->page_type(), PageType::OVERFLOW_PAGE);
}

// -- Data integrity with specific patterns ------------------------------------

TEST(OverflowManager, DataIntegrityAllZeros) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    std::vector<uint8_t> data(10000, 0x00);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

TEST(OverflowManager, DataIntegrityAllOnes) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    std::vector<uint8_t> data(10000, 0xFF);
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

// -- Single byte overflow -----------------------------------------------------

TEST(OverflowManager, SingleByte) {
    InMemoryPageAllocator alloc;
    OverflowManager mgr(alloc);

    std::vector<uint8_t> data = {0x42};
    auto ptr = mgr.store_overflow(data);
    ASSERT_TRUE(ptr.has_value());

    auto read = mgr.read_overflow(*ptr);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}
