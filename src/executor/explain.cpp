#include "giodb/executor/explain.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

namespace giodb {

// ---------------------------------------------------------------------------
// Text format
// ---------------------------------------------------------------------------

void ExplainFormatter::format_text_node(const Iterator& node,
                                        bool analyze,
                                        int depth,
                                        bool /*is_last*/,
                                        const std::string& prefix,
                                        std::string& out) {
    // Build the line for this node.
    out += prefix;
    if (depth > 0) {
        out += "-> ";
    }

    out += node.plan_node_name();

    // Append detail if present.
    auto detail = node.plan_node_detail();
    if (!detail.empty()) {
        out += "  (" + detail + ")";
    }

    // Show estimated row count from output schema column count (informational).
    // In a full implementation with a cost model, we'd show startup_cost..total_cost rows=N.

    if (analyze) {
        out += "  (actual rows=" + std::to_string(node.rows_produced());
        // Convert nanoseconds to milliseconds.
        double ms = static_cast<double>(node.execution_time().count()) / 1'000'000.0;
        out += " time=" + std::to_string(ms) + "ms";
        if (node.loops() > 1) {
            out += " loops=" + std::to_string(node.loops());
        }
        out += ")";
    }

    out += "\n";

    // Recurse into children.
    auto children = node.plan_children();
    for (size_t i = 0; i < children.size(); ++i) {
        bool last_child = (i == children.size() - 1);
        std::string child_prefix;
        if (depth > 0) {
            child_prefix = prefix + (last_child ? "   " : "   ");
        } else {
            child_prefix = "  ";
        }
        format_text_node(*children[i], analyze, depth + 1, last_child, child_prefix, out);
    }
}

std::string ExplainFormatter::format_text(const Iterator& root, bool analyze) {
    std::string result;
    format_text_node(root, analyze, 0, true, "", result);
    // Remove trailing newline if present.
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

// ---------------------------------------------------------------------------
// JSON format
// ---------------------------------------------------------------------------

namespace {

nlohmann::json node_to_json(const Iterator& node, bool analyze) {
    nlohmann::json obj;
    obj["Node Type"] = node.plan_node_name();

    auto detail = node.plan_node_detail();
    if (!detail.empty()) {
        obj["Detail"] = detail;
    }

    if (analyze) {
        obj["Actual Rows"] = node.rows_produced();
        double ms = static_cast<double>(node.execution_time().count()) / 1'000'000.0;
        obj["Actual Total Time"] = ms;
        obj["Actual Loops"] = node.loops();
    }

    auto children = node.plan_children();
    if (!children.empty()) {
        nlohmann::json plans = nlohmann::json::array();
        for (const auto* child : children) {
            plans.push_back(node_to_json(*child, analyze));
        }
        obj["Plans"] = std::move(plans);
    }

    return obj;
}

} // anonymous namespace

std::string ExplainFormatter::format_json(const Iterator& root, bool analyze) {
    nlohmann::json plan_array = nlohmann::json::array();
    plan_array.push_back(node_to_json(root, analyze));
    return plan_array.dump(2);
}

// ---------------------------------------------------------------------------
// QueryResult builder
// ---------------------------------------------------------------------------

QueryResult
ExplainFormatter::to_query_result(const Iterator& root, ExplainFormat format, bool analyze) {
    QueryResult qr;
    qr.column_names = {"QUERY PLAN"};
    qr.column_types = {TypeId::STRING};

    if (format == ExplainFormat::JSON) {
        // JSON output as a single row.
        qr.rows.push_back({Value(format_json(root, analyze))});
    } else {
        // Text output: one row per line.
        auto text = format_text(root, analyze);
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            qr.rows.push_back({Value(std::move(line))});
        }
    }

    return qr;
}

} // namespace giodb
