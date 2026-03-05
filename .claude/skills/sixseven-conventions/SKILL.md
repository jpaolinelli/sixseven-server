---
name: sixseven-conventions
description: Use when writing or reviewing C++ code in the SixSevenDB project. Provides coding conventions, error handling patterns, and naming rules.
user-invocable: false
---

# SixSevenDB Coding Conventions

Follow these conventions exactly when writing or reviewing code in this project.

## Naming

- **Functions / variables**: `snake_case`
- **Types / classes**: `PascalCase`
- **Constants / macros**: `UPPER_CASE`
- **Macro prefix**: `SIXSEVEN_`
- **File naming**: `snake_case.h` / `snake_case.cpp`
- **Namespaces**: All code lives in `sixseven::`

## Headers

- Always use `#pragma once` — no include guards.
- Order includes: project headers first, then third-party, then standard library. Separate each group with a blank line.

```cpp
#include "sixseven/common/result.h"
#include "sixseven/executor/iterator.h"

#include <tl/expected.hpp>

#include <memory>
#include <string>
#include <vector>
```

## Error Handling

Never throw exceptions. Always use `Result<T>` (alias for `tl::expected<T, Error>`).

```cpp
// Return success
return ok(value);       // Result<T>
return ok();            // Result<void>

// Return failure
return make_error(StatusCode::NOT_FOUND, "table not found: " + name);
```

Available StatusCodes: `OK`, `NOT_FOUND`, `ALREADY_EXISTS`, `INVALID_ARGUMENT`, `INTERNAL_ERROR`, `NOT_IMPLEMENTED`, `IO_ERROR`, `PARSE_ERROR`, `TYPE_ERROR`, `CONSTRAINT_VIOLATION`, `TXN_CONFLICT`, `TXN_ABORTED`.

Always check `Result` before accessing the value:

```cpp
auto result = some_function();
if (!result) {
    return make_error(result.error().code, result.error().message);
}
auto value = *result;
```

Mark all functions returning `Result<T>` with `[[nodiscard]]`.

## Logging

Use the project macros — never use `std::cout` or raw spdlog:

```cpp
SIXSEVEN_LOG_TRACE("detailed: {}", value);
SIXSEVEN_LOG_DEBUG("debug info");
SIXSEVEN_LOG_INFO("startup");
SIXSEVEN_LOG_WARN("warning: {}", reason);
SIXSEVEN_LOG_ERROR("error: {}", err.message);
SIXSEVEN_LOG_FATAL("unrecoverable");
```

## Memory Management

- Use `std::unique_ptr` for owning pointers. Never use raw `new`/`delete`.
- Use `std::make_unique` to construct.
- Pass non-owning references as `const T&` or `T&`, not raw pointers (unless nullable).

## General Rules

- Compiler flags: `-Wall -Wextra -Wpedantic -Werror` — all warnings are errors.
- Use `const` wherever possible.
- Prefer range-based for loops.
- Use `auto` when the type is obvious from context.
- Use `size_t` for container sizes and indices.
