#include "sixseven/common/types.h"
#include "sixseven/common/value.h"
#include "sixseven/executor/iterator.h"
#include "sixseven/executor/prepend_tuple.h"
#include "sixseven/executor/tuple.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

using namespace sixseven;

namespace {

// In-memory source iterator producing a fixed list of tuples.
class VectorSource : public Iterator {
public:
    VectorSource(std::vector<Tuple> tuples, OutputSchema schema)
        : tuples_(std::move(tuples)), schema_(std::move(schema)) {}

    const OutputSchema& output_schema() const override { return schema_; }

protected:
    Result<void> do_open() override {
        cursor_ = 0;
        return ok();
    }
    Result<std::optional<Tuple>> do_next() override {
        if (cursor_ >= tuples_.size()) {
            return ok(std::optional<Tuple>(std::nullopt));
        }
        return ok(std::optional<Tuple>(tuples_[cursor_++]));
    }
    void do_close() override { cursor_ = 0; }

private:
    std::vector<Tuple> tuples_;
    OutputSchema schema_;
    size_t cursor_ = 0;
};

OutputColumn col(const std::string& table, const std::string& name, TypeId type) {
    return OutputColumn{table, name, type, false, 0};
}

} // namespace

TEST(PrependTupleOperator, PrependsOuterColumnsToEveryRow) {
    // Child produces two rows of (id, name).
    OutputSchema child_schema({col("d", "id", TypeId::INT32), col("d", "name", TypeId::STRING)});
    std::vector<Tuple> rows;
    rows.push_back(Tuple{{Value(int32_t{1}), Value(std::string("a"))}, std::nullopt});
    rows.push_back(Tuple{{Value(int32_t{2}), Value(std::string("b"))}, std::nullopt});
    auto child = std::make_unique<VectorSource>(std::move(rows), child_schema);

    // Prefix is a single outer column (u.oid = 99).
    Tuple prefix{{Value(int32_t{99})}, std::nullopt};
    OutputSchema combined(
        {col("u", "oid", TypeId::INT32), col("d", "id", TypeId::INT32),
         col("d", "name", TypeId::STRING)});

    PrependTupleOperator op(std::move(child), prefix, combined);
    ASSERT_TRUE(op.open().has_value());

    EXPECT_EQ(op.output_schema().column_count(), 3u);

    auto r1 = op.next();
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r1->has_value());
    ASSERT_EQ(r1->value().values.size(), 3u);
    EXPECT_EQ(r1->value().values[0].as_int32(), 99); // outer prefix
    EXPECT_EQ(r1->value().values[1].as_int32(), 1);  // child id
    EXPECT_EQ(r1->value().values[2].as_string(), "a");

    auto r2 = op.next();
    ASSERT_TRUE(r2.has_value());
    ASSERT_TRUE(r2->has_value());
    EXPECT_EQ(r2->value().values[0].as_int32(), 99);
    EXPECT_EQ(r2->value().values[1].as_int32(), 2);

    auto r3 = op.next();
    ASSERT_TRUE(r3.has_value());
    EXPECT_FALSE(r3->has_value()); // exhausted
    op.close();
}

TEST(PrependTupleOperator, EmptyChildProducesNoRows) {
    OutputSchema child_schema({col("d", "id", TypeId::INT32)});
    auto child = std::make_unique<VectorSource>(std::vector<Tuple>{}, child_schema);

    Tuple prefix{{Value(int32_t{7})}, std::nullopt};
    OutputSchema combined({col("u", "oid", TypeId::INT32), col("d", "id", TypeId::INT32)});

    PrependTupleOperator op(std::move(child), prefix, combined);
    ASSERT_TRUE(op.open().has_value());
    auto r = op.next();
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r->has_value());
    op.close();
}
