"use client";

/**
 * QueryPlanViewer — Renders EXPLAIN JSON output as an interactive tree diagram.
 *
 * Nodes are color-coded by relative cost (green=fast, yellow=moderate, red=slow).
 * Clicking a node shows details (predicates, sort keys, etc.).
 */

import { useState, useMemo } from "react";

export interface PlanNode {
  "Node Type": string;
  "Total Cost"?: number;
  "Startup Cost"?: number;
  "Plan Rows"?: number;
  "Plan Width"?: number;
  "Actual Rows"?: number;
  "Actual Total Time"?: number;
  "Actual Startup Time"?: number;
  "Actual Loops"?: number;
  "Relation Name"?: string;
  "Alias"?: string;
  "Join Type"?: string;
  "Index Name"?: string;
  "Index Cond"?: string;
  "Filter"?: string;
  "Sort Key"?: string[];
  "Sort Method"?: string;
  "Hash Cond"?: string;
  "Merge Cond"?: string;
  "Group Key"?: string[];
  Plans?: PlanNode[];
  [key: string]: unknown;
}

interface QueryPlanViewerProps {
  planData: PlanNode;
}

/** Known metadata keys that are shown in the summary line, not in details. */
const SUMMARY_KEYS = new Set([
  "Node Type",
  "Plans",
  "Total Cost",
  "Startup Cost",
  "Plan Rows",
  "Plan Width",
  "Actual Rows",
  "Actual Total Time",
  "Actual Startup Time",
  "Actual Loops",
]);

function getMaxCost(node: PlanNode): number {
  let max = node["Total Cost"] ?? 0;
  if (node.Plans) {
    for (const child of node.Plans) {
      max = Math.max(max, getMaxCost(child));
    }
  }
  return max;
}

function costColor(cost: number, maxCost: number): string {
  if (maxCost === 0) return "text-green-400";
  const ratio = cost / maxCost;
  if (ratio < 0.33) return "text-green-400";
  if (ratio < 0.66) return "text-yellow-400";
  return "text-red-400";
}

function costBgColor(cost: number, maxCost: number): string {
  if (maxCost === 0) return "bg-green-950/30 border-green-900/40";
  const ratio = cost / maxCost;
  if (ratio < 0.33) return "bg-green-950/30 border-green-900/40";
  if (ratio < 0.66) return "bg-yellow-950/30 border-yellow-900/40";
  return "bg-red-950/30 border-red-900/40";
}

function PlanNodeView({
  node,
  maxCost,
  depth,
}: {
  node: PlanNode;
  maxCost: number;
  depth: number;
}) {
  const [expanded, setExpanded] = useState(true);
  const [showDetails, setShowDetails] = useState(false);

  const cost = node["Total Cost"] ?? 0;
  const hasChildren = (node.Plans?.length ?? 0) > 0;

  const detailEntries = useMemo(() => {
    return Object.entries(node).filter(
      ([key, value]) => !SUMMARY_KEYS.has(key) && value !== undefined && value !== null
    );
  }, [node]);

  return (
    <div className={depth > 0 ? "ml-6 border-l border-gray-800 pl-3" : ""}>
      <div
        className={`border rounded px-3 py-2 mb-1 cursor-pointer ${costBgColor(cost, maxCost)}`}
        onClick={() => setShowDetails(!showDetails)}
      >
        {/* Node header */}
        <div className="flex items-center gap-2">
          {hasChildren && (
            <button
              className="text-gray-500 hover:text-gray-300 text-xs w-4"
              onClick={(e) => {
                e.stopPropagation();
                setExpanded(!expanded);
              }}
            >
              {expanded ? "\u25BC" : "\u25B6"}
            </button>
          )}
          {!hasChildren && <span className="w-4" />}

          <span className={`font-medium text-sm ${costColor(cost, maxCost)}`}>
            {node["Node Type"]}
          </span>

          {node["Relation Name"] && (
            <span className="text-xs text-gray-400">
              on {node["Alias"] || node["Relation Name"]}
            </span>
          )}

          {node["Join Type"] && (
            <span className="text-xs text-gray-500">({node["Join Type"]})</span>
          )}

          {node["Index Name"] && (
            <span className="text-xs text-gray-500">
              using {node["Index Name"]}
            </span>
          )}
        </div>

        {/* Cost / rows summary */}
        <div className="flex items-center gap-3 mt-1 text-xs text-gray-500">
          {cost > 0 && (
            <span>
              cost: {node["Startup Cost"]?.toFixed(2)}..{cost.toFixed(2)}
            </span>
          )}
          {node["Plan Rows"] !== undefined && (
            <span>rows: {node["Plan Rows"]}</span>
          )}
          {node["Actual Rows"] !== undefined && (
            <span className="text-blue-400">
              actual: {node["Actual Rows"]} rows
            </span>
          )}
          {node["Actual Total Time"] !== undefined && (
            <span className="text-blue-400">
              {node["Actual Total Time"].toFixed(3)}ms
            </span>
          )}
        </div>

        {/* Detail panel */}
        {showDetails && detailEntries.length > 0 && (
          <div className="mt-2 pt-2 border-t border-gray-800 text-xs space-y-1">
            {detailEntries.map(([key, value]) => (
              <div key={key} className="flex gap-2">
                <span className="text-gray-500 shrink-0">{key}:</span>
                <span className="text-gray-300 break-all">
                  {Array.isArray(value)
                    ? value.join(", ")
                    : String(value)}
                </span>
              </div>
            ))}
          </div>
        )}
      </div>

      {/* Children */}
      {expanded &&
        node.Plans?.map((child, i) => (
          <PlanNodeView
            key={i}
            node={child}
            maxCost={maxCost}
            depth={depth + 1}
          />
        ))}
    </div>
  );
}

export function QueryPlanViewer({ planData }: QueryPlanViewerProps) {
  const maxCost = useMemo(() => getMaxCost(planData), [planData]);

  return (
    <div className="p-3 overflow-auto h-full">
      <div className="flex items-center gap-3 mb-3 text-xs text-gray-500">
        <span>Query Plan</span>
        <span className="flex items-center gap-1">
          <span className="w-2 h-2 rounded-full bg-green-500" /> Fast
        </span>
        <span className="flex items-center gap-1">
          <span className="w-2 h-2 rounded-full bg-yellow-500" /> Moderate
        </span>
        <span className="flex items-center gap-1">
          <span className="w-2 h-2 rounded-full bg-red-500" /> Slow
        </span>
        <span className="text-gray-600 ml-2">Click nodes for details</span>
      </div>
      <PlanNodeView node={planData} maxCost={maxCost} depth={0} />
    </div>
  );
}

/**
 * Try to parse query results as EXPLAIN JSON output.
 * PostgreSQL EXPLAIN (FORMAT JSON) returns a single row with a single column
 * containing a JSON array of plan objects.
 */
export function tryParseExplainPlan(
  columns: string[],
  rows: (string | number | boolean | null)[][]
): PlanNode | null {
  if (columns.length !== 1 || rows.length !== 1) return null;

  const colName = columns[0].toUpperCase();
  if (colName !== "QUERY PLAN" && colName !== "EXPLAIN") return null;

  const raw = rows[0][0];
  if (typeof raw !== "string") return null;

  try {
    const parsed = JSON.parse(raw);
    if (Array.isArray(parsed) && parsed.length > 0 && parsed[0].Plan) {
      return parsed[0].Plan as PlanNode;
    }
    if (parsed.Plan) {
      return parsed.Plan as PlanNode;
    }
    if (parsed["Node Type"]) {
      return parsed as PlanNode;
    }
  } catch {
    // Not valid JSON — not an EXPLAIN result
  }
  return null;
}
