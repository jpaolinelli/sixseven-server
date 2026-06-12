#include "sixseven/executor/explain.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

/// Format an optimizer cost estimate as "cost=0.00..12.34 rows=42".
std::string format_cost_estimate(const sixseven::PlanCostEstimate& cost) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);
    os << "cost=" << cost.startup_cost << ".." << cost.total_cost;
    os << " rows=" << static_cast<long long>(std::llround(cost.estimated_rows));
    return os.str();
}

} // anonymous namespace

namespace sixseven {

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

    // Show optimizer cost estimates when the planner attached them
    // (i.e. when table statistics were available via ANALYZE).
    if (node.estimated_cost().has_value()) {
        out += "  (" + format_cost_estimate(*node.estimated_cost()) + ")";
    }

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

    if (node.estimated_cost().has_value()) {
        const auto& cost = *node.estimated_cost();
        obj["Startup Cost"] = cost.startup_cost;
        obj["Total Cost"] = cost.total_cost;
        obj["Plan Rows"] = static_cast<long long>(std::llround(cost.estimated_rows));
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

} // namespace sixseven
