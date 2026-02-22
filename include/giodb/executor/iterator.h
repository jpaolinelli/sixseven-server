#pragma once

#include "giodb/common/result.h"
#include "giodb/executor/tuple.h"

#include <optional>

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
class Iterator {
public:
    virtual ~Iterator() = default;

    /// Initialise the operator. Must be called before next().
    [[nodiscard]] virtual Result<void> open() = 0;

    /// Pull the next tuple. Returns nullopt when the operator is exhausted.
    [[nodiscard]] virtual Result<std::optional<Tuple>> next() = 0;

    /// Release all resources. Safe to call multiple times.
    virtual void close() = 0;

    /// The logical schema of the tuples this operator produces.
    [[nodiscard]] virtual const OutputSchema& output_schema() const = 0;
};

} // namespace giodb
