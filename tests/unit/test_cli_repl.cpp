#include "sixseven/cli/repl.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

using namespace sixseven;
using namespace sixseven::cli;

// ---------------------------------------------------------------------------
// Helper: collects SQL statements sent to exec_fn
// ---------------------------------------------------------------------------

struct FakeExec {
    std::vector<std::string> captured;
    bool should_fail{false};

    Result<void> operator()(const std::string& sql) {
        captured.push_back(sql);
        if (should_fail) {
            return make_error(StatusCode::NETWORK_ERROR, "simulated transport failure");
        }
        return ok();
    }
};

// Helper: wrap FakeExec in a lambda that captures by reference so the
// std::function copy in run_repl does not copy the FakeExec state.
static ExecFn make_fn(FakeExec& exec) {
    return [&exec](const std::string& sql) -> Result<void> { return exec(sql); };
}

// ---------------------------------------------------------------------------
// One-shot mode (-c flag)
// ---------------------------------------------------------------------------

TEST(CliRepl, OneShotExecsSingleStatement) {
    FakeExec exec;
    ReplOptions opts;
    opts.one_shot = "SELECT 1";
    opts.interactive = false;

    std::istringstream in("");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(exec.captured.size(), 1u);
    EXPECT_EQ(exec.captured[0], "SELECT 1");
}

TEST(CliRepl, OneShotTrimsWhitespace) {
    FakeExec exec;
    ReplOptions opts;
    opts.one_shot = "  SELECT 42  ";
    opts.interactive = false;

    std::istringstream in("");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(exec.captured.size(), 1u);
    EXPECT_EQ(exec.captured[0], "SELECT 42");
}

// ---------------------------------------------------------------------------
// Multi-line accumulation until semicolon
// ---------------------------------------------------------------------------

TEST(CliRepl, MultiLineUntilSemicolon) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("SELECT\n1;");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(exec.captured.size(), 1u);
    EXPECT_EQ(exec.captured[0], "SELECT\n1;");
}

TEST(CliRepl, TwoStatements) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("SELECT 1;\nSELECT 2;");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(exec.captured.size(), 2u);
    EXPECT_EQ(exec.captured[0], "SELECT 1;");
    EXPECT_EQ(exec.captured[1], "SELECT 2;");
}

// ---------------------------------------------------------------------------
// \q exits cleanly
// ---------------------------------------------------------------------------

TEST(CliRepl, BackslashQExits) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("\\q");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(exec.captured.empty());
}

TEST(CliRepl, BackslashQuitExits) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("\\quit");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(exec.captured.empty());
}

// ---------------------------------------------------------------------------
// \help prints help without crashing
// ---------------------------------------------------------------------------

TEST(CliRepl, BackslashHelpPrintsHelp) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("\\help\n\\q");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(out.str().find("Meta-commands"), std::string::npos);
    EXPECT_TRUE(exec.captured.empty());
}

// ---------------------------------------------------------------------------
// Transport failure bubbles up
// ---------------------------------------------------------------------------

TEST(CliRepl, TransportFailureReturnsError) {
    FakeExec exec;
    exec.should_fail = true;

    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("SELECT 1;");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, StatusCode::NETWORK_ERROR);
}

// ---------------------------------------------------------------------------
// EOF (empty stdin) exits cleanly
// ---------------------------------------------------------------------------

TEST(CliRepl, EmptyInputExitsCleanly) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(exec.captured.empty());
}

// ---------------------------------------------------------------------------
// Statement not sent without semicolon (no-op, just EOF)
// ---------------------------------------------------------------------------

TEST(CliRepl, IncompleteStatementNotSentOnEof) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("SELECT 1");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(exec.captured.empty());
}

// ---------------------------------------------------------------------------
// Unknown meta-command is reported, not executed
// ---------------------------------------------------------------------------

TEST(CliRepl, UnknownMetaCommandReported) {
    FakeExec exec;
    ReplOptions opts;
    opts.interactive = false;

    std::istringstream in("\\bogus\n\\q");
    std::ostringstream out;

    auto fn = make_fn(exec);
    auto result = run_repl(in, out, fn, opts);
    ASSERT_TRUE(result.has_value());
    EXPECT_NE(out.str().find("Unknown"), std::string::npos);
    EXPECT_TRUE(exec.captured.empty());
}
