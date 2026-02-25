"use client";

/**
 * QueryResults — Enhanced results panel with:
 *  - Sort columns by clicking header (client-side)
 *  - Filter rows by column value (text input per column)
 *  - Pagination for large result sets (50 rows per page)
 *  - Column resize by dragging
 *  - Export toolbar (CSV, JSON, clipboard)
 *  - Query plan visualization (auto-detected from EXPLAIN JSON)
 *  - View mode tabs: Table | Plan
 */

import { useState, useMemo, useCallback, useRef, useEffect } from "react";
import { exportCSV, exportJSON, copyToClipboard } from "@/lib/export";
import {
  QueryPlanViewer,
  tryParseExplainPlan,
  type PlanNode,
} from "./QueryPlanViewer";

interface QueryResultsProps {
  columns: string[];
  rows: (string | number | boolean | null)[][];
  error?: string | null;
  durationMs?: number;
  isLoading?: boolean;
}

type SortDirection = "asc" | "desc" | null;
type ViewMode = "table" | "plan";

const PAGE_SIZE = 50;

function compareValues(
  a: string | number | boolean | null,
  b: string | number | boolean | null,
  dir: "asc" | "desc"
): number {
  if (a === null && b === null) return 0;
  if (a === null) return dir === "asc" ? -1 : 1;
  if (b === null) return dir === "asc" ? 1 : -1;

  if (typeof a === "number" && typeof b === "number") {
    return dir === "asc" ? a - b : b - a;
  }

  const sa = String(a);
  const sb = String(b);

  // Try numeric comparison for string values that look like numbers
  const na = Number(sa);
  const nb = Number(sb);
  if (!isNaN(na) && !isNaN(nb) && sa !== "" && sb !== "") {
    return dir === "asc" ? na - nb : nb - na;
  }

  return dir === "asc" ? sa.localeCompare(sb) : sb.localeCompare(sa);
}

export function QueryResults({
  columns,
  rows,
  error,
  durationMs,
  isLoading,
}: QueryResultsProps) {
  const [sortColumn, setSortColumn] = useState<number | null>(null);
  const [sortDir, setSortDir] = useState<SortDirection>(null);
  const [filters, setFilters] = useState<Record<number, string>>({});
  const [showFilters, setShowFilters] = useState(false);
  const [page, setPage] = useState(0);
  const [columnWidths, setColumnWidths] = useState<Record<number, number>>({});
  const [viewMode, setViewMode] = useState<ViewMode>("table");
  const [copyFeedback, setCopyFeedback] = useState(false);
  const resizeRef = useRef<{ col: number; startX: number; startW: number } | null>(null);

  // Detect EXPLAIN plan data
  const planData: PlanNode | null = useMemo(
    () => tryParseExplainPlan(columns, rows),
    [columns, rows]
  );

  // Auto-switch to plan view when plan data is detected
  useEffect(() => {
    if (planData) setViewMode("plan");
    else setViewMode("table");
  }, [planData]);

  // Reset pagination when sort/filter changes
  useEffect(() => {
    setPage(0);
  }, [sortColumn, sortDir, filters]);

  // Reset state when results change
  useEffect(() => {
    setSortColumn(null);
    setSortDir(null);
    setFilters({});
    setPage(0);
    setColumnWidths({});
  }, [columns, rows]);

  // Filtered rows
  const filteredRows = useMemo(() => {
    const activeFilters = Object.entries(filters).filter(
      ([, v]) => v.trim() !== ""
    );
    if (activeFilters.length === 0) return rows;

    return rows.filter((row) =>
      activeFilters.every(([colStr, query]) => {
        const col = Number(colStr);
        const cell = row[col];
        if (cell === null) return query.toLowerCase() === "null";
        return String(cell).toLowerCase().includes(query.toLowerCase());
      })
    );
  }, [rows, filters]);

  // Sorted rows
  const sortedRows = useMemo(() => {
    if (sortColumn === null || sortDir === null) return filteredRows;
    return [...filteredRows].sort((a, b) =>
      compareValues(a[sortColumn], b[sortColumn], sortDir)
    );
  }, [filteredRows, sortColumn, sortDir]);

  // Paginated rows
  const totalPages = Math.max(1, Math.ceil(sortedRows.length / PAGE_SIZE));
  const pagedRows = useMemo(
    () => sortedRows.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE),
    [sortedRows, page]
  );

  // Sort handler
  const handleSort = useCallback(
    (colIndex: number) => {
      if (sortColumn === colIndex) {
        if (sortDir === "asc") setSortDir("desc");
        else if (sortDir === "desc") {
          setSortColumn(null);
          setSortDir(null);
        }
      } else {
        setSortColumn(colIndex);
        setSortDir("asc");
      }
    },
    [sortColumn, sortDir]
  );

  // Filter handler
  const handleFilter = useCallback((colIndex: number, value: string) => {
    setFilters((prev) => ({ ...prev, [colIndex]: value }));
  }, []);

  // Column resize handlers
  const handleResizeStart = useCallback(
    (e: React.MouseEvent, colIndex: number) => {
      e.preventDefault();
      e.stopPropagation();
      const th = (e.target as HTMLElement).closest("th");
      const startW = th?.getBoundingClientRect().width ?? 100;
      resizeRef.current = { col: colIndex, startX: e.clientX, startW };

      const handleMove = (me: MouseEvent) => {
        if (!resizeRef.current) return;
        const diff = me.clientX - resizeRef.current.startX;
        const newWidth = Math.max(50, resizeRef.current.startW + diff);
        setColumnWidths((prev) => ({
          ...prev,
          [resizeRef.current!.col]: newWidth,
        }));
      };
      const handleUp = () => {
        resizeRef.current = null;
        document.removeEventListener("mousemove", handleMove);
        document.removeEventListener("mouseup", handleUp);
      };
      document.addEventListener("mousemove", handleMove);
      document.addEventListener("mouseup", handleUp);
    },
    []
  );

  // Export handlers
  const handleCopy = useCallback(async () => {
    const ok = await copyToClipboard(columns, sortedRows);
    if (ok) {
      setCopyFeedback(true);
      setTimeout(() => setCopyFeedback(false), 1500);
    }
  }, [columns, sortedRows]);

  // Loading state
  if (isLoading) {
    return (
      <div className="flex items-center gap-2 p-4 text-sm text-gray-400">
        <span className="animate-pulse">Executing query...</span>
      </div>
    );
  }

  // Error state
  if (error) {
    return (
      <div className="p-4">
        <div className="bg-red-950/40 border border-red-900/50 rounded p-3 text-sm text-red-300">
          <span className="font-medium text-red-400">Error: </span>
          {error}
        </div>
      </div>
    );
  }

  // Empty state
  if (columns.length === 0) {
    return (
      <div className="flex items-center justify-center h-full text-gray-600 text-sm">
        Execute a query to see results
      </div>
    );
  }

  const sortIndicator = (colIndex: number) => {
    if (sortColumn !== colIndex) return null;
    return (
      <span className="ml-1 text-blue-400">
        {sortDir === "asc" ? "\u25B2" : "\u25BC"}
      </span>
    );
  };

  return (
    <div className="flex flex-col h-full">
      {/* Toolbar: view mode tabs + export + status */}
      <div className="flex items-center gap-2 px-3 py-1.5 border-b border-gray-800 shrink-0">
        {/* View mode tabs */}
        <div className="flex items-center gap-0.5">
          <button
            className={`px-2 py-0.5 text-xs rounded ${
              viewMode === "table"
                ? "bg-gray-800 text-gray-200"
                : "text-gray-500 hover:text-gray-300"
            }`}
            onClick={() => setViewMode("table")}
          >
            Table
          </button>
          {planData && (
            <button
              className={`px-2 py-0.5 text-xs rounded ${
                viewMode === "plan"
                  ? "bg-gray-800 text-gray-200"
                  : "text-gray-500 hover:text-gray-300"
              }`}
              onClick={() => setViewMode("plan")}
            >
              Plan
            </button>
          )}
        </div>

        <div className="w-px h-4 bg-gray-800" />

        {/* Export buttons */}
        <button
          className="px-1.5 py-0.5 text-xs text-gray-500 hover:text-gray-300 hover:bg-gray-800 rounded"
          onClick={() => exportCSV(columns, sortedRows)}
          title="Export as CSV"
        >
          CSV
        </button>
        <button
          className="px-1.5 py-0.5 text-xs text-gray-500 hover:text-gray-300 hover:bg-gray-800 rounded"
          onClick={() => exportJSON(columns, sortedRows)}
          title="Export as JSON"
        >
          JSON
        </button>
        <button
          className={`px-1.5 py-0.5 text-xs rounded ${
            copyFeedback
              ? "text-green-400 bg-green-950/30"
              : "text-gray-500 hover:text-gray-300 hover:bg-gray-800"
          }`}
          onClick={handleCopy}
          title="Copy to clipboard"
        >
          {copyFeedback ? "Copied!" : "Copy"}
        </button>

        {/* Filter toggle */}
        {viewMode === "table" && (
          <>
            <div className="w-px h-4 bg-gray-800" />
            <button
              className={`px-1.5 py-0.5 text-xs rounded ${
                showFilters
                  ? "bg-gray-800 text-gray-200"
                  : "text-gray-500 hover:text-gray-300"
              }`}
              onClick={() => setShowFilters(!showFilters)}
              title="Toggle column filters"
            >
              Filter
            </button>
          </>
        )}

        {/* Spacer */}
        <div className="flex-1" />

        {/* Status info */}
        <span className="text-xs text-gray-500">
          {filteredRows.length !== rows.length
            ? `${filteredRows.length} of ${rows.length} rows`
            : `${rows.length} row${rows.length !== 1 ? "s" : ""}`}
        </span>
        {durationMs !== undefined && (
          <span className="text-xs text-gray-500">
            {durationMs < 1000
              ? `${durationMs}ms`
              : `${(durationMs / 1000).toFixed(2)}s`}
          </span>
        )}
      </div>

      {/* Plan view */}
      {viewMode === "plan" && planData && (
        <div className="flex-1 min-h-0 overflow-auto">
          <QueryPlanViewer planData={planData} />
        </div>
      )}

      {/* Table view */}
      {viewMode === "table" && (
        <>
          {/* Results table */}
          <div className="flex-1 overflow-auto min-h-0">
            <table className="w-full text-xs border-collapse">
              <thead className="sticky top-0 z-10">
                {/* Column headers */}
                <tr className="bg-gray-900 border-b border-gray-800">
                  <th className="text-left py-1.5 px-2 text-gray-500 font-medium w-10 border-r border-gray-800">
                    #
                  </th>
                  {columns.map((col, i) => (
                    <th
                      key={col}
                      className="text-left py-1.5 px-2 text-gray-400 font-medium border-r border-gray-800 last:border-r-0 select-none relative group"
                      style={
                        columnWidths[i]
                          ? { width: columnWidths[i], minWidth: columnWidths[i] }
                          : undefined
                      }
                    >
                      <span
                        className="cursor-pointer hover:text-gray-200"
                        onClick={() => handleSort(i)}
                      >
                        {col}
                        {sortIndicator(i)}
                      </span>
                      {/* Resize handle */}
                      <div
                        className="absolute right-0 top-0 bottom-0 w-1.5 cursor-col-resize opacity-0 group-hover:opacity-100 hover:bg-blue-500/30"
                        onMouseDown={(e) => handleResizeStart(e, i)}
                      />
                    </th>
                  ))}
                </tr>

                {/* Filter row */}
                {showFilters && (
                  <tr className="bg-gray-900/80 border-b border-gray-800">
                    <th className="border-r border-gray-800" />
                    {columns.map((col, i) => (
                      <th
                        key={`filter-${col}`}
                        className="px-1 py-1 border-r border-gray-800 last:border-r-0"
                      >
                        <input
                          type="text"
                          className="w-full bg-gray-800 border border-gray-700 rounded px-1.5 py-0.5 text-xs text-gray-300 placeholder-gray-600 focus:outline-none focus:border-gray-500 font-normal"
                          placeholder={`Filter ${col}...`}
                          value={filters[i] ?? ""}
                          onChange={(e) => handleFilter(i, e.target.value)}
                        />
                      </th>
                    ))}
                  </tr>
                )}
              </thead>
              <tbody>
                {pagedRows.map((row, i) => (
                  <tr
                    key={page * PAGE_SIZE + i}
                    className="border-b border-gray-800/50 hover:bg-gray-900/50"
                  >
                    <td className="py-1 px-2 text-gray-600 border-r border-gray-800 font-mono">
                      {page * PAGE_SIZE + i + 1}
                    </td>
                    {row.map((cell, j) => (
                      <td
                        key={j}
                        className="py-1 px-2 text-gray-300 border-r border-gray-800 last:border-r-0 max-w-xs truncate"
                        style={
                          columnWidths[j]
                            ? {
                                width: columnWidths[j],
                                minWidth: columnWidths[j],
                                maxWidth: columnWidths[j],
                              }
                            : undefined
                        }
                      >
                        {cell === null ? (
                          <span className="text-gray-600 italic">NULL</span>
                        ) : typeof cell === "boolean" ? (
                          <span
                            className={
                              cell ? "text-green-400" : "text-red-400"
                            }
                          >
                            {String(cell)}
                          </span>
                        ) : (
                          String(cell)
                        )}
                      </td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {/* Pagination */}
          {totalPages > 1 && (
            <div className="flex items-center justify-between px-3 py-1.5 border-t border-gray-800 shrink-0 text-xs text-gray-500">
              <span>
                Page {page + 1} of {totalPages}
              </span>
              <div className="flex items-center gap-1">
                <button
                  className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                  disabled={page === 0}
                  onClick={() => setPage(0)}
                  title="First page"
                >
                  &laquo;
                </button>
                <button
                  className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                  disabled={page === 0}
                  onClick={() => setPage((p) => p - 1)}
                  title="Previous page"
                >
                  &lsaquo;
                </button>
                <button
                  className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                  disabled={page >= totalPages - 1}
                  onClick={() => setPage((p) => p + 1)}
                  title="Next page"
                >
                  &rsaquo;
                </button>
                <button
                  className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                  disabled={page >= totalPages - 1}
                  onClick={() => setPage(totalPages - 1)}
                  title="Last page"
                >
                  &raquo;
                </button>
              </div>
            </div>
          )}
        </>
      )}
    </div>
  );
}
