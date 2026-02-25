#pragma once

#include "giodb/common/result.h"
#include "giodb/executor/tuple.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace giodb {

/// Abstract base class for all Volcano-model executor operators.
///
/// The pull-based interface:
///   1. `open()`  — initialise the operator (acquire resources, open children).
///   2. `next()`  — return the next row, or nullopt when exhausted.
///   3. `close()` — release resources.
///
/// Every operator also exposes its output schema so that parent operators
/// (and the expression evaluator) can resolve column references.
///
/// The non-virtual public methods delegate to protected `do_*` methods
/// (Template Method pattern) so that instrumentation for EXPLAIN ANALYZE
/// is applied automatically with zero overhead when disabled.
class Iterator {
public:
    virtual ~Iterator() = default;

    // -- Public non-virtual interface (with instrumentation) ------------------

    /// Initialise the operator. Must be called before next().
    [[nodiscard]] Result<void> open() {
        if (instrumented_) {
            ++loops_;
            auto start = std::chrono::steady_clock::now();
            auto result = do_open();
            total_time_ += std::chrono::steady_clock::now() - start;
            return result;
        }
        return do_open();
    }

    /// Pull the next tuple. Returns nullopt when the operator is exhausted.
    [[nodiscard]] Result<std::optional<Tuple>> next() {
        if (instrumented_) {
            auto start = std::chrono::steady_clock::now();
            auto result = do_next();
            total_time_ += std::chrono::steady_clock::now() - start;
            if (result && result->has_value()) {
                ++rows_produced_;
            }
            return result;
        }
        return do_next();
    }

    /// Release all resources. Safe to call multiple times.
    void close() {
        if (instrumented_) {
            auto start = std::chrono::steady_clock::now();
            do_close();
            total_time_ += std::chrono::steady_clock::now() - start;
            return;
        }
        do_close();
    }

    /// The logical schema of the tuples this operator produces.
    [[nodiscard]] virtual const OutputSchema& output_schema() const = 0;

    // -- Plan inspection (for EXPLAIN) ----------------------------------------

    /// Human-readable operator name (e.g. "Seq Scan", "Hash Join").
    [[nodiscard]] virtual std::string plan_node_name() const { return "Unknown"; }

    /// Operator-specific detail string (e.g. table name, join condition).
    [[nodiscard]] virtual std::string plan_node_detail() const { return ""; }

    /// Child iterators for tree-walking during EXPLAIN formatting.
    [[nodiscard]] virtual std::vector<const Iterator*> plan_children() const { return {}; }

    // -- Instrumentation (for EXPLAIN ANALYZE) --------------------------------

    /// Enable timing and row counting. Must be called before open().
    /// Recursively enables instrumentation on all children.
    void enable_instrumentation() {
        instrumented_ = true;
        for (auto* child : plan_children_mutable()) {
            child->enable_instrumentation();
        }
    }

    [[nodiscard]] bool is_instrumented() const { return instrumented_; }
    [[nodiscard]] int64_t rows_produced() const { return rows_produced_; }
    [[nodiscard]] int64_t loops() const { return loops_; }
    [[nodiscard]] std::chrono::nanoseconds execution_time() const { return total_time_; }

protected:
    // -- Subclass implementation hooks ----------------------------------------

    /// Initialise the operator (Template Method hook).
    [[nodiscard]] virtual Result<void> do_open() = 0;

    /// Pull the next tuple (Template Method hook).
    [[nodiscard]] virtual Result<std::optional<Tuple>> do_next() = 0;

    /// Release all resources (Template Method hook).
    virtual void do_close() = 0;

    /// Mutable version of plan_children for enable_instrumentation().
    [[nodiscard]] virtual std::vector<Iterator*> plan_children_mutable() { return {}; }

    bool instrumented_ = false;
    int64_t rows_produced_ = 0;
    int64_t loops_ = 0;
    std::chrono::nanoseconds total_time_{0};
};

} // namespace giodb
