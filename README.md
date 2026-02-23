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

### Required Tools

This project requires **LLVM/Clang 20** for formatting and static analysis (must match CI).

**macOS:**

```bash
brew install llvm@20
```

Homebrew installs LLVM 20 as keg-only. The binaries are at `/opt/homebrew/opt/llvm@20/bin/`. To make them available system-wide, add to your shell profile:

```bash
export PATH="/opt/homebrew/opt/llvm@20/bin:$PATH"
```

**Ubuntu:**

```bash
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | sudo tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc
sudo add-apt-repository -y "deb http://apt.llvm.org/$(lsb_release -cs)/ llvm-toolchain-$(lsb_release -cs)-20 main"
sudo apt-get update
sudo apt-get install -y clang-format-20 clang-tidy-20
```

### Pre-commit hooks

Install the project's pre-commit hooks to automatically check formatting before each commit:

```bash
git config core.hooksPath .githooks
```

The hook checks `clang-format-20` on staged `.cpp` and `.h` files. It warns if a different major version is detected and skips gracefully if clang-format is not installed.

### Formatting and Static Analysis

CMake targets are provided to reproduce CI checks locally:

```bash
# Check formatting (dry-run, mirrors CI)
cmake --build build/debug --target format-check

# Auto-fix formatting in-place
cmake --build build/debug --target format-fix

# Run clang-tidy with CI flags
cmake --build build/debug --target tidy
```

### CI Pipeline

The CI pipeline runs on every push to `main` and every pull request:

1. **Format Check** - Verifies all source files match `clang-format-20` output
2. **Clang-Tidy** - Static analysis with `clang-tidy-20` (select warnings as errors)
3. **Build Matrix** - Compiles and tests on Linux GCC 13, Linux Clang 20, and macOS Apple Clang

## Contributing

Contributions are welcome. Please open an issue or pull request.
