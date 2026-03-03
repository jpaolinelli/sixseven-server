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
 *  - Graph-aware views: Nodes | Edges | Graph (for TRAVERSE results)
 */

import { useState, useMemo, useCallback, useRef, useEffect } from "react";
import { exportCSV, exportJSON, copyToClipboard } from "@/lib/export";
import {
  QueryPlanViewer,
  tryParseExplainPlan,
  type PlanNode,
} from "./QueryPlanViewer";
import {
  isNodeCentricResult,
  buildGraphData,
  formatEdgeTooltip,
  type GraphViewNode,
  type GraphViewEdge,
  type GraphViewData,
} from "@/lib/graph-query-utils";
import { tableColor } from "@/lib/graph-utils";

type CellValue = string | number | boolean | null;

interface QueryResultsProps {
  columns: string[];
  rows: CellValue[][];
  /** Edge-centric result columns (from dual-query MODE EDGES). */
  edgeColumns?: string[];
  /** Edge-centric result rows (from dual-query MODE EDGES). */
  edgeRows?: CellValue[][];
  /** Source (starting) node columns for graph visualization. */
  sourceNodeColumns?: string[];
  /** Source (starting) node rows for graph visualization. */
  sourceNodeRows?: CellValue[][];
  error?: string | null;
  durationMs?: number;
  isLoading?: boolean;
  /** True when the executed SQL was a TRAVERSE query (fallback for graph detection). */
  isTraverseResult?: boolean;
}

type SortDirection = "asc" | "desc" | null;
type ViewMode = "table" | "plan" | "nodes" | "edges" | "graph";

const PAGE_SIZE = 50;

export function compareValues(
  a: CellValue,
  b: CellValue,
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
  edgeColumns,
  edgeRows,
  sourceNodeColumns,
  sourceNodeRows,
  error,
  durationMs,
  isLoading,
  isTraverseResult,
}: QueryResultsProps) {
  const [sortColumn, setSortColumn] = useState<number | null>(null);
  const [sortDir, setSortDir] = useState<SortDirection>(null);
  const [filters, setFilters] = useState<Record<number, string>>({});
  const [showFilters, setShowFilters] = useState(false);
  const [page, setPage] = useState(0);
  const [columnWidths, setColumnWidths] = useState<Record<number, number>>({});
  const [viewMode, setViewMode] = useState<ViewMode>("table");
  const [copyFeedback, setCopyFeedback] = useState<"idle" | "success" | "error">("idle");
  const resizeRef = useRef<{ col: number; startX: number; startW: number } | null>(null);

  // Edge table state (separate sort/filter/page for edges view)
  const [edgeSortColumn, setEdgeSortColumn] = useState<number | null>(null);
  const [edgeSortDir, setEdgeSortDir] = useState<SortDirection>(null);
  const [edgeFilters, setEdgeFilters] = useState<Record<number, string>>({});
  const [showEdgeFilters, setShowEdgeFilters] = useState(false);
  const [edgePage, setEdgePage] = useState(0);
  const [edgeColumnWidths, setEdgeColumnWidths] = useState<Record<number, number>>({});

  // Graph view state
  const [graphLabelAttr, setGraphLabelAttr] = useState<string>("__auto__");
  const [selectedGraphItem, setSelectedGraphItem] = useState<
    | { type: "node"; node: GraphViewNode }
    | { type: "edge"; edge: GraphViewEdge }
    | null
  >(null);
  const [graphLayout, setGraphLayout] = useState<"force" | "hierarchical" | "circular">("force");
  const graphContainerRef = useRef<HTMLDivElement>(null);
  const networkRef = useRef<any>(null);

  // Detect graph results (meta-column detection OR TRAVERSE query fallback)
  const hasGraphResult = useMemo(
    () => isNodeCentricResult(columns) || !!isTraverseResult,
    [columns, isTraverseResult]
  );
  const hasEdgeData = useMemo(
    () => !!(edgeColumns && edgeColumns.length > 0 && edgeRows && edgeRows.length > 0),
    [edgeColumns, edgeRows]
  );

  // Detect EXPLAIN plan data
  const planData: PlanNode | null = useMemo(
    () => tryParseExplainPlan(columns, rows),
    [columns, rows]
  );

  // Build graph data when we have both node and edge results
  const rawGraphData: GraphViewData | null = useMemo(() => {
    if (!hasGraphResult) return null;
    return buildGraphData(
      columns,
      rows,
      edgeColumns ?? [],
      edgeRows ?? [],
      sourceNodeColumns,
      sourceNodeRows
    );
  }, [hasGraphResult, columns, rows, edgeColumns, edgeRows, sourceNodeColumns, sourceNodeRows]);

  // Collect available label attributes from all graph nodes
  const graphLabelOptions: string[] = useMemo(() => {
    if (!rawGraphData) return [];
    const attrSet = new Set<string>();
    for (const node of rawGraphData.nodes) {
      for (const key of Object.keys(node.attributes)) {
        attrSet.add(key);
      }
    }
    return Array.from(attrSet).sort();
  }, [rawGraphData]);

  // Apply the selected label attribute to graph nodes
  const graphData: GraphViewData | null = useMemo(() => {
    if (!rawGraphData) return null;
    if (graphLabelAttr === "__auto__") return rawGraphData;

    return {
      ...rawGraphData,
      nodes: rawGraphData.nodes.map((n) => {
        if (graphLabelAttr === "__pk__") {
          return { ...n, label: n.pk };
        }
        const val = n.attributes[graphLabelAttr];
        return {
          ...n,
          label: val !== null && val !== undefined ? String(val) : n.pk,
        };
      }),
    };
  }, [rawGraphData, graphLabelAttr]);

  // Auto-switch view mode based on result type
  useEffect(() => {
    if (planData) {
      setViewMode("plan");
    } else if (hasGraphResult) {
      setViewMode("nodes");
    } else {
      setViewMode("table");
    }
  }, [planData, hasGraphResult]);

  // Reset pagination when sort/filter changes
  useEffect(() => {
    setPage(0);
  }, [sortColumn, sortDir, filters]);

  useEffect(() => {
    setEdgePage(0);
  }, [edgeSortColumn, edgeSortDir, edgeFilters]);

  // Reset state when results change
  useEffect(() => {
    setSortColumn(null);
    setSortDir(null);
    setFilters({});
    setPage(0);
    setColumnWidths({});
    setEdgeSortColumn(null);
    setEdgeSortDir(null);
    setEdgeFilters({});
    setEdgePage(0);
    setEdgeColumnWidths({});
    setSelectedGraphItem(null);
  }, [columns, rows]);

  // ── Table data processing (for nodes view and standard table) ──

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

  const sortedRows = useMemo(() => {
    if (sortColumn === null || sortDir === null) return filteredRows;
    return [...filteredRows].sort((a, b) =>
      compareValues(a[sortColumn], b[sortColumn], sortDir)
    );
  }, [filteredRows, sortColumn, sortDir]);

  const totalPages = Math.max(1, Math.ceil(sortedRows.length / PAGE_SIZE));
  const pagedRows = useMemo(
    () => sortedRows.slice(page * PAGE_SIZE, (page + 1) * PAGE_SIZE),
    [sortedRows, page]
  );

  // ── Edge table data processing ──

  const filteredEdgeRows = useMemo(() => {
    if (!edgeRows) return [];
    const activeFilters = Object.entries(edgeFilters).filter(
      ([, v]) => v.trim() !== ""
    );
    if (activeFilters.length === 0) return edgeRows;

    return edgeRows.filter((row) =>
      activeFilters.every(([colStr, query]) => {
        const col = Number(colStr);
        const cell = row[col];
        if (cell === null) return query.toLowerCase() === "null";
        return String(cell).toLowerCase().includes(query.toLowerCase());
      })
    );
  }, [edgeRows, edgeFilters]);

  const sortedEdgeRows = useMemo(() => {
    if (edgeSortColumn === null || edgeSortDir === null) return filteredEdgeRows;
    return [...filteredEdgeRows].sort((a, b) =>
      compareValues(a[edgeSortColumn], b[edgeSortColumn], edgeSortDir)
    );
  }, [filteredEdgeRows, edgeSortColumn, edgeSortDir]);

  const edgeTotalPages = Math.max(1, Math.ceil(sortedEdgeRows.length / PAGE_SIZE));
  const pagedEdgeRows = useMemo(
    () => sortedEdgeRows.slice(edgePage * PAGE_SIZE, (edgePage + 1) * PAGE_SIZE),
    [sortedEdgeRows, edgePage]
  );

  // ── Handlers ──

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

  const handleEdgeSort = useCallback(
    (colIndex: number) => {
      if (edgeSortColumn === colIndex) {
        if (edgeSortDir === "asc") setEdgeSortDir("desc");
        else if (edgeSortDir === "desc") {
          setEdgeSortColumn(null);
          setEdgeSortDir(null);
        }
      } else {
        setEdgeSortColumn(colIndex);
        setEdgeSortDir("asc");
      }
    },
    [edgeSortColumn, edgeSortDir]
  );

  const handleFilter = useCallback((colIndex: number, value: string) => {
    setFilters((prev) => ({ ...prev, [colIndex]: value }));
  }, []);

  const handleEdgeFilter = useCallback((colIndex: number, value: string) => {
    setEdgeFilters((prev) => ({ ...prev, [colIndex]: value }));
  }, []);

  // Column resize handlers
  const resizeCleanupRef = useRef<(() => void) | null>(null);

  useEffect(() => {
    return () => {
      resizeCleanupRef.current?.();
    };
  }, []);

  const handleResizeStart = useCallback(
    (
      e: React.MouseEvent,
      colIndex: number,
      setWidths: React.Dispatch<React.SetStateAction<Record<number, number>>>
    ) => {
      e.preventDefault();
      e.stopPropagation();
      const th = (e.target as HTMLElement).closest("th");
      const startW = th?.getBoundingClientRect().width ?? 100;
      resizeRef.current = { col: colIndex, startX: e.clientX, startW };

      const handleMove = (me: MouseEvent) => {
        if (!resizeRef.current) return;
        const diff = me.clientX - resizeRef.current.startX;
        const newWidth = Math.max(50, resizeRef.current.startW + diff);
        setWidths((prev) => ({
          ...prev,
          [resizeRef.current!.col]: newWidth,
        }));
      };
      const cleanup = () => {
        resizeRef.current = null;
        document.removeEventListener("mousemove", handleMove);
        document.removeEventListener("mouseup", cleanup);
        resizeCleanupRef.current = null;
      };
      resizeCleanupRef.current = cleanup;
      document.addEventListener("mousemove", handleMove);
      document.addEventListener("mouseup", cleanup);
    },
    []
  );

  // Export handlers
  const handleCopy = useCallback(async () => {
    const exportCols = viewMode === "edges" ? (edgeColumns ?? []) : columns;
    const exportRows = viewMode === "edges" ? sortedEdgeRows : sortedRows;
    const ok = await copyToClipboard(exportCols, exportRows);
    setCopyFeedback(ok ? "success" : "error");
    setTimeout(() => setCopyFeedback("idle"), 1500);
  }, [columns, edgeColumns, sortedRows, sortedEdgeRows, viewMode]);

  // ── Graph visualization ──

  const graphNetworkOptions = useMemo(() => {
    return {
      nodes: {
        shape: "dot",
        size: 18,
        font: { color: "#e5e7eb", size: 12, face: "system-ui" },
        borderWidth: 2,
        shadow: { enabled: true, size: 4, color: "rgba(0,0,0,0.3)" },
      },
      edges: {
        arrows: { to: { enabled: true, scaleFactor: 0.6 } },
        color: { color: "#6b7280", highlight: "#f59e0b", hover: "#9ca3af" },
        font: { color: "#9ca3af", size: 10, face: "system-ui", strokeWidth: 0 },
        smooth: { enabled: true, type: "continuous" as const, roundness: 0.5 },
        width: 1.5,
      },
      interaction: {
        hover: true,
        tooltipDelay: 200,
        navigationButtons: true,
        keyboard: true,
        dragNodes: true,
        zoomView: true,
        dragView: true,
      },
      physics: {
        enabled: graphLayout === "force",
        solver: "forceAtlas2Based" as const,
        forceAtlas2Based: {
          gravitationalConstant: -40,
          centralGravity: 0.005,
          springLength: 120,
          springConstant: 0.08,
          damping: 0.4,
        },
        stabilization: { iterations: 100 },
      },
      layout:
        graphLayout === "hierarchical"
          ? {
              hierarchical: {
                enabled: true,
                direction: "UD" as const,
                sortMethod: "directed" as const,
                nodeSpacing: 120,
                levelSeparation: 100,
              },
            }
          : {},
    };
  }, [graphLayout]);

  // Initialize or update vis-network for graph view
  useEffect(() => {
    if (viewMode !== "graph" || !graphData || !graphContainerRef.current) return;

    let isMounted = true;

    const initNetwork = async () => {
      const vis = await import("vis-network/standalone");

      if (!isMounted || !graphContainerRef.current) return;

      // Collect distinct tables for color coding
      const tableSet = new Set(graphData.nodes.map((n) => n.table));

      const visNodes = graphData.nodes.map((n) => {
        const nodeVis: any = {
          id: n.id,
          label: n.label,
          color: {
            background: tableColor(n.table),
            border: "#374151",
            highlight: { background: "#60a5fa", border: "#ffffff" },
            hover: { background: "#93c5fd", border: "#ffffff" },
          },
          title: `${n.table}:${n.pk}`,
        };

        // Arrange circular layout manually
        if (graphLayout === "circular" && graphData.nodes.length > 0) {
          const idx = graphData.nodes.indexOf(n);
          const radius = Math.max(150, graphData.nodes.length * 25);
          const angle = (2 * Math.PI * idx) / graphData.nodes.length;
          nodeVis.x = Math.cos(angle) * radius;
          nodeVis.y = Math.sin(angle) * radius;
          nodeVis.fixed = { x: true, y: true };
        }

        return nodeVis;
      });

      const visEdges = graphData.edges.map((e) => ({
        id: e.id,
        from: e.from,
        to: e.to,
        label: e.label,
        title: formatEdgeTooltip(e.edgeType, e.properties),
      }));

      if (networkRef.current) {
        networkRef.current.destroy();
      }

      const network = new vis.Network(
        graphContainerRef.current,
        {
          nodes: new vis.DataSet(visNodes),
          edges: new vis.DataSet(visEdges),
        },
        graphNetworkOptions
      );
      networkRef.current = network;

      // Click handler: select node or edge
      network.on("click", (params: any) => {
        if (params.nodes.length === 1) {
          const nodeId = params.nodes[0] as string;
          const node = graphData.nodes.find((n) => n.id === nodeId);
          if (node) {
            setSelectedGraphItem({ type: "node", node });
          }
        } else if (params.edges.length === 1) {
          const edgeId = params.edges[0] as string;
          const edge = graphData.edges.find((e) => e.id === edgeId);
          if (edge) {
            setSelectedGraphItem({ type: "edge", edge });
          }
        } else {
          setSelectedGraphItem(null);
        }
      });
    };

    initNetwork();

    return () => {
      isMounted = false;
      if (networkRef.current) {
        networkRef.current.destroy();
        networkRef.current = null;
      }
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [viewMode, graphData, graphNetworkOptions, graphLayout]);

  // ── Render helpers ──

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

  // Empty traversal state
  if (hasGraphResult && rows.length === 0) {
    return (
      <div className="flex flex-col h-full">
        {renderToolbar()}
        <div className="flex items-center justify-center flex-1 text-gray-500">
          <div className="text-center">
            <p className="text-sm mb-1">No traversal results</p>
            <p className="text-xs text-gray-600">
              The TRAVERSE query returned no nodes. Check that the starting node
              exists and has connections.
            </p>
          </div>
        </div>
      </div>
    );
  }

  const sortIndicator = (colIdx: number, currentSort: number | null, currentDir: SortDirection) => {
    if (currentSort !== colIdx) return null;
    return (
      <span className="ml-1 text-blue-400">
        {currentDir === "asc" ? "\u25B2" : "\u25BC"}
      </span>
    );
  };

  // ── Toolbar ──

  function renderToolbar() {
    const isTableLike = viewMode === "table" || viewMode === "nodes" || viewMode === "edges";
    const activeFiltersShown = viewMode === "nodes" || viewMode === "table";
    const edgeFiltersShown = viewMode === "edges";

    const displayRows = viewMode === "edges" ? sortedEdgeRows : sortedRows;
    const totalRows = viewMode === "edges" ? (edgeRows?.length ?? 0) : rows.length;
    const filteredCount = displayRows.length;

    return (
      <div className="flex items-center gap-2 px-3 py-1.5 border-b border-gray-800 shrink-0">
        {/* View mode tabs */}
        <div className="flex items-center gap-0.5">
          {hasGraphResult ? (
            <>
              <button
                className={`px-2 py-0.5 text-xs rounded ${
                  viewMode === "nodes"
                    ? "bg-gray-800 text-gray-200"
                    : "text-gray-500 hover:text-gray-300"
                }`}
                onClick={() => setViewMode("nodes")}
              >
                Nodes
              </button>
              {hasEdgeData && (
                <button
                  className={`px-2 py-0.5 text-xs rounded ${
                    viewMode === "edges"
                      ? "bg-gray-800 text-gray-200"
                      : "text-gray-500 hover:text-gray-300"
                  }`}
                  onClick={() => setViewMode("edges")}
                >
                  Edges
                </button>
              )}
              <button
                className={`px-2 py-0.5 text-xs rounded ${
                  viewMode === "graph"
                    ? "bg-gray-800 text-gray-200"
                    : "text-gray-500 hover:text-gray-300"
                }`}
                onClick={() => setViewMode("graph")}
              >
                Graph
              </button>
            </>
          ) : (
            <>
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
            </>
          )}
        </div>

        {/* Graph layout selector + label picker */}
        {viewMode === "graph" && (
          <>
            <div className="w-px h-4 bg-gray-800" />
            <div className="flex items-center gap-0.5">
              <span className="text-xs text-gray-500">Layout:</span>
              {(["force", "hierarchical", "circular"] as const).map((l) => (
                <button
                  key={l}
                  onClick={() => setGraphLayout(l)}
                  className={`px-1.5 py-0.5 text-xs rounded ${
                    graphLayout === l
                      ? "bg-blue-600 text-white"
                      : "text-gray-500 hover:text-gray-300"
                  }`}
                >
                  {l === "force" ? "Force" : l === "hierarchical" ? "Hierarchy" : "Circular"}
                </button>
              ))}
            </div>
            {graphLabelOptions.length > 0 && (
              <>
                <div className="w-px h-4 bg-gray-800" />
                <div className="flex items-center gap-1">
                  <span className="text-xs text-gray-500">Label:</span>
                  <select
                    className="bg-gray-900 border border-gray-700 rounded px-1.5 py-0.5 text-xs text-gray-300 focus:outline-none focus:border-gray-500"
                    value={graphLabelAttr}
                    onChange={(e) => setGraphLabelAttr(e.target.value)}
                  >
                    <option value="__auto__">Auto</option>
                    {graphLabelOptions.map((attr) => (
                      <option key={attr} value={attr}>
                        {attr}
                      </option>
                    ))}
                    <option value="__pk__">PK</option>
                  </select>
                </div>
              </>
            )}
          </>
        )}

        {isTableLike && (
          <>
            <div className="w-px h-4 bg-gray-800" />

            {/* Export buttons */}
            <button
              className="px-1.5 py-0.5 text-xs text-gray-500 hover:text-gray-300 hover:bg-gray-800 rounded"
              onClick={() => {
                const cols = viewMode === "edges" ? (edgeColumns ?? []) : columns;
                const rws = viewMode === "edges" ? sortedEdgeRows : sortedRows;
                exportCSV(cols, rws);
              }}
              title="Export as CSV"
            >
              CSV
            </button>
            <button
              className="px-1.5 py-0.5 text-xs text-gray-500 hover:text-gray-300 hover:bg-gray-800 rounded"
              onClick={() => {
                const cols = viewMode === "edges" ? (edgeColumns ?? []) : columns;
                const rws = viewMode === "edges" ? sortedEdgeRows : sortedRows;
                exportJSON(cols, rws);
              }}
              title="Export as JSON"
            >
              JSON
            </button>
            <button
              className={`px-1.5 py-0.5 text-xs rounded ${
                copyFeedback === "success"
                  ? "text-green-400 bg-green-950/30"
                  : copyFeedback === "error"
                    ? "text-red-400 bg-red-950/30"
                    : "text-gray-500 hover:text-gray-300 hover:bg-gray-800"
              }`}
              onClick={handleCopy}
              title="Copy to clipboard"
            >
              {copyFeedback === "success"
                ? "Copied!"
                : copyFeedback === "error"
                  ? "Copy failed"
                  : "Copy"}
            </button>

            {/* Filter toggle */}
            {(activeFiltersShown || edgeFiltersShown) && (
              <>
                <div className="w-px h-4 bg-gray-800" />
                <button
                  className={`px-1.5 py-0.5 text-xs rounded ${
                    (activeFiltersShown && showFilters) || (edgeFiltersShown && showEdgeFilters)
                      ? "bg-gray-800 text-gray-200"
                      : "text-gray-500 hover:text-gray-300"
                  }`}
                  onClick={() => {
                    if (edgeFiltersShown) setShowEdgeFilters(!showEdgeFilters);
                    else setShowFilters(!showFilters);
                  }}
                  title="Toggle column filters"
                >
                  Filter
                </button>
              </>
            )}
          </>
        )}

        {/* Spacer */}
        <div className="flex-1" />

        {/* Status info */}
        {isTableLike && (
          <span className="text-xs text-gray-500">
            {filteredCount !== totalRows
              ? `${filteredCount} of ${totalRows} rows`
              : `${totalRows} row${totalRows !== 1 ? "s" : ""}`}
          </span>
        )}
        {viewMode === "graph" && graphData && (
          <span className="text-xs text-gray-500">
            {graphData.nodes.length} nodes, {graphData.edges.length} edges
          </span>
        )}
        {durationMs !== undefined && (
          <span className="text-xs text-gray-500">
            {durationMs < 1000
              ? `${durationMs}ms`
              : `${(durationMs / 1000).toFixed(2)}s`}
          </span>
        )}
      </div>
    );
  }

  // ── Table renderer (shared between table, nodes, and edges views) ──

  function renderTable(
    tableCols: string[],
    tableRows: CellValue[][],
    pagedTableRows: CellValue[][],
    currentPage: number,
    tableTotalPages: number,
    setPageFn: (p: number | ((prev: number) => number)) => void,
    currentSort: number | null,
    currentDir: SortDirection,
    sortFn: (colIdx: number) => void,
    currentFilters: Record<number, string>,
    filterFn: (colIdx: number, value: string) => void,
    isFiltersShown: boolean,
    widths: Record<number, number>,
    setWidths: React.Dispatch<React.SetStateAction<Record<number, number>>>,
    options?: {
      directionColumn?: string;
      onRowClick?: (rowIndex: number) => void;
    }
  ) {
    return (
      <>
        <div className="flex-1 overflow-auto min-h-0">
          <table className="w-full text-xs border-collapse">
            <thead className="sticky top-0 z-10">
              <tr className="bg-gray-900 border-b border-gray-800">
                <th className="text-left py-1.5 px-2 text-gray-500 font-medium w-10 border-r border-gray-800">
                  #
                </th>
                {tableCols.map((col, i) => {
                  // Show direction arrow for __from → __to columns
                  const isFromCol = options?.directionColumn && col.toLowerCase() === "__from";
                  const displayName = isFromCol ? `${col} \u2192` : col;
                  return (
                    <th
                      key={i}
                      className="text-left py-1.5 px-2 text-gray-400 font-medium border-r border-gray-800 last:border-r-0 select-none relative group"
                      style={
                        widths[i]
                          ? { width: widths[i], minWidth: widths[i] }
                          : undefined
                      }
                    >
                      <span
                        className="cursor-pointer hover:text-gray-200"
                        onClick={() => sortFn(i)}
                      >
                        {displayName}
                        {sortIndicator(i, currentSort, currentDir)}
                      </span>
                      <div
                        className="absolute right-0 top-0 bottom-0 w-1.5 cursor-col-resize opacity-0 group-hover:opacity-100 hover:bg-blue-500/30"
                        onMouseDown={(e) => handleResizeStart(e, i, setWidths)}
                      />
                    </th>
                  );
                })}
              </tr>

              {isFiltersShown && (
                <tr className="bg-gray-900/80 border-b border-gray-800">
                  <th className="border-r border-gray-800" />
                  {tableCols.map((col, i) => (
                    <th
                      key={`filter-${i}`}
                      className="px-1 py-1 border-r border-gray-800 last:border-r-0"
                    >
                      <input
                        type="text"
                        className="w-full bg-gray-800 border border-gray-700 rounded px-1.5 py-0.5 text-xs text-gray-300 placeholder-gray-600 focus:outline-none focus:border-gray-500 font-normal"
                        placeholder={`Filter ${col}...`}
                        value={currentFilters[i] ?? ""}
                        onChange={(e) => filterFn(i, e.target.value)}
                      />
                    </th>
                  ))}
                </tr>
              )}
            </thead>
            <tbody>
              {pagedTableRows.map((row, i) => (
                <tr
                  key={currentPage * PAGE_SIZE + i}
                  className={`border-b border-gray-800/50 hover:bg-gray-900/50 ${
                    options?.onRowClick ? "cursor-pointer" : ""
                  }`}
                  onClick={() => options?.onRowClick?.(currentPage * PAGE_SIZE + i)}
                >
                  <td className="py-1 px-2 text-gray-600 border-r border-gray-800 font-mono">
                    {currentPage * PAGE_SIZE + i + 1}
                  </td>
                  {row.map((cell, j) => (
                    <td
                      key={j}
                      className="py-1 px-2 text-gray-300 border-r border-gray-800 last:border-r-0 max-w-xs truncate"
                      style={
                        widths[j]
                          ? {
                              width: widths[j],
                              minWidth: widths[j],
                              maxWidth: widths[j],
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

        {tableTotalPages > 1 && (
          <div className="flex items-center justify-between px-3 py-1.5 border-t border-gray-800 shrink-0 text-xs text-gray-500">
            <span>
              Page {currentPage + 1} of {tableTotalPages}
            </span>
            <div className="flex items-center gap-1">
              <button
                className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                disabled={currentPage === 0}
                onClick={() => setPageFn(0)}
                title="First page"
              >
                &laquo;
              </button>
              <button
                className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                disabled={currentPage === 0}
                onClick={() => setPageFn((p) => p - 1)}
                title="Previous page"
              >
                &lsaquo;
              </button>
              <button
                className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                disabled={currentPage >= tableTotalPages - 1}
                onClick={() => setPageFn((p) => p + 1)}
                title="Next page"
              >
                &rsaquo;
              </button>
              <button
                className="px-2 py-0.5 rounded hover:bg-gray-800 disabled:opacity-30 disabled:cursor-not-allowed"
                disabled={currentPage >= tableTotalPages - 1}
                onClick={() => setPageFn(tableTotalPages - 1)}
                title="Last page"
              >
                &raquo;
              </button>
            </div>
          </div>
        )}
      </>
    );
  }

  // ── Graph view renderer ──

  function renderGraphView() {
    if (!graphData) return null;

    // Collect distinct tables for legend
    const tableSet = new Set(graphData.nodes.map((n) => n.table));
    const tables = Array.from(tableSet).sort();

    return (
      <div className="flex flex-1 min-h-0 overflow-hidden">
        {/* Graph canvas */}
        <div className="flex-1 relative">
          {graphData.nodes.length === 0 ? (
            <div className="flex items-center justify-center h-full text-gray-500">
              <div className="text-center">
                <p className="text-sm mb-1">No graph data</p>
                <p className="text-xs text-gray-600">
                  Run a TRAVERSE query to visualize the graph.
                </p>
              </div>
            </div>
          ) : (
            <div ref={graphContainerRef} className="w-full h-full" />
          )}
        </div>

        {/* Right sidebar: legend + details */}
        <div className="w-56 border-l border-gray-800 flex flex-col overflow-y-auto bg-gray-900/30">
          {/* Legend */}
          {tables.length > 0 && (
            <div className="p-2 border-b border-gray-800">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Tables
              </div>
              {tables.map((t) => (
                <div key={t} className="flex items-center gap-1.5 py-0.5">
                  <span
                    className="w-2.5 h-2.5 rounded-full shrink-0"
                    style={{ backgroundColor: tableColor(t) }}
                  />
                  <span className="text-xs text-gray-300 truncate">{t}</span>
                </div>
              ))}
            </div>
          )}

          {/* Selected node details */}
          {selectedGraphItem?.type === "node" && (
            <div className="p-2 flex-1">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Node Details
              </div>
              <div className="text-xs text-blue-400 mb-2">
                {selectedGraphItem.node.table}:{selectedGraphItem.node.pk}
              </div>
              <div className="space-y-1">
                {Object.entries(selectedGraphItem.node.attributes).map(
                  ([key, value]) => (
                    <div key={key}>
                      <span className="text-xs text-gray-500">{key}: </span>
                      <span className="text-xs text-gray-300">
                        {value === null ? (
                          <span className="italic text-gray-600">NULL</span>
                        ) : (
                          String(value)
                        )}
                      </span>
                    </div>
                  )
                )}
              </div>
            </div>
          )}

          {/* Selected edge details */}
          {selectedGraphItem?.type === "edge" && (
            <div className="p-2 flex-1">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Edge Details
              </div>
              <div className="text-xs text-amber-400 mb-1">
                {selectedGraphItem.edge.edgeType}
              </div>
              <div className="text-xs text-gray-400 mb-2">
                {selectedGraphItem.edge.from} → {selectedGraphItem.edge.to}
              </div>
              {Object.keys(selectedGraphItem.edge.properties).length > 0 && (
                <div className="space-y-1">
                  <div className="text-xs text-gray-500 font-medium">
                    Properties
                  </div>
                  {Object.entries(selectedGraphItem.edge.properties).map(
                    ([key, value]) => (
                      <div key={key}>
                        <span className="text-xs text-gray-500">{key}: </span>
                        <span className="text-xs text-gray-300">
                          {value === null ? (
                            <span className="italic text-gray-600">NULL</span>
                          ) : (
                            String(value)
                          )}
                        </span>
                      </div>
                    )
                  )}
                </div>
              )}
            </div>
          )}

          {/* No selection hint */}
          {!selectedGraphItem && (
            <div className="p-2 text-xs text-gray-600">
              Click a node or edge to see details
            </div>
          )}
        </div>
      </div>
    );
  }

  // ── Main render ──

  return (
    <div className="flex flex-col h-full">
      {renderToolbar()}

      {/* Plan view */}
      {viewMode === "plan" && planData && (
        <div className="flex-1 min-h-0 overflow-auto">
          <QueryPlanViewer planData={planData} />
        </div>
      )}

      {/* Standard table view (non-graph results) */}
      {viewMode === "table" &&
        renderTable(
          columns,
          sortedRows,
          pagedRows,
          page,
          totalPages,
          setPage,
          sortColumn,
          sortDir,
          handleSort,
          filters,
          handleFilter,
          showFilters,
          columnWidths,
          setColumnWidths
        )}

      {/* Nodes view (graph results — node-centric table) */}
      {viewMode === "nodes" &&
        renderTable(
          columns,
          sortedRows,
          pagedRows,
          page,
          totalPages,
          setPage,
          sortColumn,
          sortDir,
          handleSort,
          filters,
          handleFilter,
          showFilters,
          columnWidths,
          setColumnWidths
        )}

      {/* Edges view (graph results — edge-centric table) */}
      {viewMode === "edges" && hasEdgeData &&
        renderTable(
          edgeColumns!,
          sortedEdgeRows,
          pagedEdgeRows,
          edgePage,
          edgeTotalPages,
          setEdgePage,
          edgeSortColumn,
          edgeSortDir,
          handleEdgeSort,
          edgeFilters,
          handleEdgeFilter,
          showEdgeFilters,
          edgeColumnWidths,
          setEdgeColumnWidths,
          { directionColumn: "__from" }
        )}

      {/* Graph view */}
      {viewMode === "graph" && renderGraphView()}
    </div>
  );
}
