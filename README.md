# GioDB

A hybrid relational, graph, and vector database built in C++20.

## Build

```bash
cmake --preset default
cmake --build build/debug
```

## Usage

```bash
./build/debug/giodb-server
```

## Development

### Pre-commit hooks

Install the project's pre-commit hooks to automatically check formatting before each commit:

```bash
git config core.hooksPath .githooks
```

The hook checks `clang-format` on staged `.cpp` and `.h` files. If clang-format is not installed, the hook prints a warning and allows the commit.

### Formatting

Format all source files:

```bash
find src include tests -name '*.cpp' -o -name '*.h' | xargs clang-format -i
```

## Contributing

Contributions are welcome. Please open an issue or pull request.
