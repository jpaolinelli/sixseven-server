#pragma once

#include "giodb/executor/iterator.h"
#include "giodb/executor/tuple.h"
#include "giodb/parser/ast.h"
#include "giodb/planner/binder.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace giodb {

/// Hash Join operator: build a hash table on one input, probe with the other.
///
/// The build side (right child) is materialised into a hash table keyed on
/// the join key expression. The probe side (left child) is then scanned, and
/// for each probe tuple the hash table is looked up for matches.
///
/// Supports INNER and LEFT join types.
///
/// Grace hash join: when the build side exceeds `work_mem` tuples, both sides
/// are partitioned into buckets by a secondary hash and each partition is
/// joined recursively.
class HashJoinOperator : public Iterator {
public:
    /// @param probe     Left (probe side) child iterator.
    /// @param build     Right (build side) child iterator.
    /// @param type      Join type (INNER or LEFT).
    /// @param probe_key Expression to extract the key from probe tuples.
    /// @param build_key Expression to extract the key from build tuples.
    /// @param bound     BoundStatement with expr_types map.
    /// @param schema    Combined output schema (probe columns + build columns).
    /// @param work_mem  Max build tuples before grace hash partitioning (0 = unlimited).
    HashJoinOperator(std::unique_ptr<Iterator> probe,
                     std::unique_ptr<Iterator> build,
                     JoinType type,
                     const Expr* probe_key,
                     const Expr* build_key,
                     const BoundStatement& bound,
                     OutputSchema schema,
                     size_t work_mem = 0);

    const OutputSchema& output_schema() const override;
    std::string plan_node_name() const override;
    std::string plan_node_detail() const override;
    std::vector<const Iterator*> plan_children() const override;

protected:
    Result<void> do_open() override;
    Result<std::optional<Tuple>> do_next() override;
    void do_close() override;
    std::vector<Iterator*> plan_children_mutable() override;

private:
    /// Hash a Value for the hash table.
    static size_t hash_value(const Value& v);

    /// Check if two Values are equal for join matching.
    static bool values_equal(const Value& a, const Value& b);

    /// Combine a probe and build tuple into a single output tuple.
    Tuple combine(const Tuple& probe, const Tuple& build) const;

    /// Create a tuple with NULLs for the build side.
    Tuple probe_with_null_build(const Tuple& probe) const;

    /// Run in-memory hash join on two sets of tuples.
    Result<void> run_in_memory(std::vector<Tuple>& build_tuples, std::vector<Tuple>& probe_tuples);

    /// Run grace hash join by partitioning both sides.
    Result<void> run_grace(std::vector<Tuple>& build_tuples, std::vector<Tuple>& probe_tuples);

    std::unique_ptr<Iterator> probe_child_;
    std::unique_ptr<Iterator> build_child_;
    JoinType type_;
    const Expr* probe_key_;
    const Expr* build_key_;
    const BoundStatement& bound_;
    OutputSchema schema_;
    size_t work_mem_;

    size_t probe_col_count_ = 0;
    size_t build_col_count_ = 0;

    // Pre-computed output stored after open().
    std::vector<Tuple> output_;
    size_t output_cursor_ = 0;
};

} // namespace giodb
