"use client";

import {
  useState,
  useRef,
  useEffect,
  useCallback,
  useMemo,
} from "react";
import type {
  GraphNode,
  GraphEdge,
  LayoutType,
  ContextMenuState,
  GraphFilters,
} from "@/lib/graph-types";
import {
  traverseNode,
  findShortestPath,
  fetchNodeDetails,
  makeNodeId,
  tableColor,
  parseNodeId,
} from "@/lib/graph-utils";
import { useConnection } from "@/lib/ConnectionContext";
import type { EdgeTypeInfo } from "@/lib/types";

interface GraphExplorerProps {
  databases: string[];
  edgeTypes: EdgeTypeInfo[];
  defaultDatabase: string;
}

const MAX_VISIBLE_NODES = 200;

export function GraphExplorer({
  databases,
  edgeTypes,
  defaultDatabase,
}: GraphExplorerProps) {
  const { connectionParams } = useConnection();
  const [database, setDatabase] = useState(defaultDatabase);
  const [nodes, setNodes] = useState<Map<string, GraphNode>>(new Map());
  const [edges, setEdges] = useState<Map<string, GraphEdge>>(new Map());
  const [selectedNodes, setSelectedNodes] = useState<string[]>([]);
  const [highlightedPath, setHighlightedPath] = useState<Set<string>>(
    new Set()
  );
  const [highlightedEdges, setHighlightedEdges] = useState<Set<string>>(
    new Set()
  );
  const [layout, setLayout] = useState<LayoutType>("force");
  const [contextMenu, setContextMenu] = useState<ContextMenuState | null>(
    null
  );
  const [detailNode, setDetailNode] = useState<{
    node: GraphNode;
    data: Record<string, unknown>;
  } | null>(null);
  const [filters, setFilters] = useState<GraphFilters>({
    visibleEdgeTypes: new Set<string>(),
    maxDepth: 10,
  });
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Start node input
  const [startTable, setStartTable] = useState("");
  const [startId, setStartId] = useState("");

  const containerRef = useRef<HTMLDivElement>(null);
  const networkRef = useRef<any>(null);

  // Collect all edge types visible in the graph
  const graphEdgeTypes = useMemo(() => {
    const types = new Set<string>();
    edges.forEach((e) => types.add(e.edgeType));
    return Array.from(types).sort();
  }, [edges]);

  // All table names in graph for legend
  const graphTables = useMemo(() => {
    const tables = new Set<string>();
    nodes.forEach((n) => tables.add(n.table));
    return Array.from(tables).sort();
  }, [nodes]);

  // Filter edges based on current filters
  const visibleEdges = useMemo(() => {
    const result: GraphEdge[] = [];
    edges.forEach((e) => {
      if (
        filters.visibleEdgeTypes.size > 0 &&
        !filters.visibleEdgeTypes.has(e.edgeType)
      ) {
        return;
      }
      result.push(e);
    });
    return result;
  }, [edges, filters.visibleEdgeTypes]);

  // Filter nodes based on depth
  const visibleNodes = useMemo(() => {
    const result: GraphNode[] = [];
    nodes.forEach((n) => {
      if (n.depth <= filters.maxDepth) {
        result.push(n);
      }
    });
    return result;
  }, [nodes, filters.maxDepth]);

  // vis-network options per layout
  const networkOptions = useMemo(() => {
    const base = {
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
        smooth: { enabled: true, type: "continuous", roundness: 0.5 },
        width: 1.5,
      },
      interaction: {
        hover: true,
        multiselect: true,
        navigationButtons: true,
        keyboard: true,
      },
      physics: {
        enabled: layout === "force",
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
        layout === "hierarchical"
          ? {
              hierarchical: {
                enabled: true,
                direction: "UD" as const,
                sortMethod: "directed" as const,
                nodeSpacing: 120,
                levelSeparation: 100,
              },
            }
          : layout === "circular"
            ? { improvedLayout: true }
            : {},
    };
    return base;
  }, [layout]);

  // Initialize or update vis-network
  useEffect(() => {
    if (!containerRef.current) return;

    let isMounted = true;

    const initNetwork = async () => {
      const vis = await import("vis-network/standalone");

      if (!isMounted || !containerRef.current) return;

      const visNodes = visibleNodes.map((n) => ({
        id: n.id,
        label: n.label,
        color: {
          background: highlightedPath.has(n.id)
            ? "#f59e0b"
            : tableColor(n.table),
          border: selectedNodes.includes(n.id) ? "#ffffff" : "#374151",
          highlight: { background: "#60a5fa", border: "#ffffff" },
          hover: { background: "#93c5fd", border: "#ffffff" },
        },
        borderWidth: selectedNodes.includes(n.id) ? 3 : 2,
        size: highlightedPath.has(n.id) ? 24 : n.expanded ? 22 : 18,
      }));

      const visEdgeList = visibleEdges.map((e) => ({
        id: e.id,
        from: e.from,
        to: e.to,
        label: e.label,
        color: highlightedEdges.has(e.id)
          ? { color: "#f59e0b", highlight: "#f59e0b" }
          : undefined,
        width: highlightedEdges.has(e.id) ? 3 : 1.5,
      }));

      // Arrange circular layout manually if selected
      if (layout === "circular" && visNodes.length > 0) {
        const radius = Math.max(150, visNodes.length * 25);
        visNodes.forEach((n, i) => {
          const angle = (2 * Math.PI * i) / visNodes.length;
          (n as any).x = Math.cos(angle) * radius;
          (n as any).y = Math.sin(angle) * radius;
          (n as any).fixed = { x: true, y: true };
        });
      }

      const nodeDataSet = new vis.DataSet(visNodes);
      const edgeDataSet = new vis.DataSet(visEdgeList);

      if (networkRef.current) {
        networkRef.current.destroy();
      }

      const network = new vis.Network(
        containerRef.current,
        { nodes: nodeDataSet, edges: edgeDataSet },
        networkOptions
      );
      networkRef.current = network;

      // Click: select or expand
      network.on("click", (params: any) => {
        setContextMenu(null);
        if (params.nodes.length === 1) {
          const nodeId = params.nodes[0] as string;
          handleNodeClick(nodeId, params.event.srcEvent?.shiftKey ?? false);
        } else if (params.nodes.length === 0) {
          setSelectedNodes([]);
          setDetailNode(null);
        }
      });

      // Double-click: expand outgoing
      network.on("doubleClick", (params: any) => {
        if (params.nodes.length === 1) {
          const nodeId = params.nodes[0] as string;
          expandNode(nodeId, "out");
        }
      });

      // Right-click: context menu
      network.on("oncontext", (params: any) => {
        params.event.preventDefault();
        const nodeId = network.getNodeAt(params.pointer.DOM) as
          | string
          | undefined;
        if (nodeId) {
          setContextMenu({
            x: params.event.pageX ?? params.pointer.DOM.x,
            y: params.event.pageY ?? params.pointer.DOM.y,
            nodeId,
          });
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
  }, [visibleNodes, visibleEdges, highlightedPath, highlightedEdges, selectedNodes, networkOptions, layout]);

  const handleNodeClick = useCallback(
    (nodeId: string, shift: boolean) => {
      if (shift) {
        // Shift-click: toggle selection for path finding
        setSelectedNodes((prev) => {
          if (prev.includes(nodeId)) {
            return prev.filter((id) => id !== nodeId);
          }
          const next = [...prev, nodeId];
          // Keep at most 2 for path finding
          return next.length > 2 ? next.slice(-2) : next;
        });
      } else {
        setSelectedNodes([nodeId]);
      }
    },
    []
  );

  const expandNode = useCallback(
    async (nodeId: string, direction: "out" | "in" | "both") => {
      const node = nodes.get(nodeId);
      if (!node) return;

      if (nodes.size >= MAX_VISIBLE_NODES) {
        setError(
          `Node limit reached (${MAX_VISIBLE_NODES}). Remove some nodes first.`
        );
        return;
      }

      setLoading(true);
      setError(null);

      try {
        const result = await traverseNode(
          database,
          node.table,
          node.pk,
          direction,
          node.depth,
          undefined,
          connectionParams
        );

        setNodes((prev) => {
          const next = new Map(prev);
          // Mark the expanded node
          const existing = next.get(nodeId);
          if (existing) {
            next.set(nodeId, { ...existing, expanded: true });
          }
          // Add new nodes (don't overwrite existing)
          for (const n of result.nodes) {
            if (!next.has(n.id) && next.size < MAX_VISIBLE_NODES) {
              next.set(n.id, n);
            }
          }
          return next;
        });

        setEdges((prev) => {
          const next = new Map(prev);
          for (const e of result.edges) {
            if (!next.has(e.id)) {
              next.set(e.id, e);
            }
          }
          return next;
        });
      } catch (err) {
        setError(err instanceof Error ? err.message : "Traverse failed");
      } finally {
        setLoading(false);
      }
    },
    [database, nodes, connectionParams]
  );

  const removeNode = useCallback((nodeId: string) => {
    setNodes((prev) => {
      const next = new Map(prev);
      next.delete(nodeId);
      return next;
    });
    setEdges((prev) => {
      const next = new Map(prev);
      prev.forEach((e, id) => {
        if (e.from === nodeId || e.to === nodeId) {
          next.delete(id);
        }
      });
      return next;
    });
    setSelectedNodes((prev) => prev.filter((id) => id !== nodeId));
    setContextMenu(null);
  }, []);

  const showNodeDetails = useCallback(
    async (nodeId: string) => {
      const node = nodes.get(nodeId);
      if (!node) return;

      setLoading(true);
      try {
        const data = await fetchNodeDetails(database, node.table, node.pk, connectionParams);
        setDetailNode({ node, data });
      } catch (err) {
        setError(err instanceof Error ? err.message : "Failed to load details");
      } finally {
        setLoading(false);
      }
      setContextMenu(null);
    },
    [database, nodes, connectionParams]
  );

  const handleFindPath = useCallback(async () => {
    if (selectedNodes.length !== 2) return;

    const src = nodes.get(selectedNodes[0]);
    const tgt = nodes.get(selectedNodes[1]);
    if (!src || !tgt) return;

    setLoading(true);
    setError(null);

    try {
      const result = await findShortestPath(
        database,
        src.table,
        src.pk,
        tgt.table,
        tgt.pk,
        connectionParams
      );

      // Parse path result — expect rows with table/id pairs
      const pathNodes = new Set<string>();
      const pathEdges = new Set<string>();

      const colIdx = new Map<string, number>();
      result.columns.forEach((col, i) =>
        colIdx.set(col.toLowerCase(), i)
      );

      const srcTableIdx =
        colIdx.get("source_table") ?? colIdx.get("src_table") ?? -1;
      const srcIdIdx =
        colIdx.get("source_id") ?? colIdx.get("src_id") ?? -1;
      const edgeTypeIdx =
        colIdx.get("edge_type") ?? colIdx.get("edgetype") ?? -1;
      const tgtTableIdx =
        colIdx.get("target_table") ?? colIdx.get("tgt_table") ?? -1;
      const tgtIdIdx =
        colIdx.get("target_id") ?? colIdx.get("tgt_id") ?? -1;

      if (srcTableIdx >= 0 && tgtTableIdx >= 0) {
        for (const row of result.rows) {
          const sTable = String(row[srcTableIdx]);
          const sId = String(row[srcIdIdx]);
          const tTable = String(row[tgtTableIdx]);
          const tId = String(row[tgtIdIdx]);
          const edgeType = edgeTypeIdx >= 0 ? String(row[edgeTypeIdx]) : "edge";

          const sNodeId = makeNodeId(sTable, sId);
          const tNodeId = makeNodeId(tTable, tId);

          pathNodes.add(sNodeId);
          pathNodes.add(tNodeId);
          pathEdges.add(`${sNodeId}->${edgeType}->${tNodeId}`);

          // Add missing nodes to the graph
          if (!nodes.has(sNodeId)) {
            setNodes((prev) => {
              const next = new Map(prev);
              next.set(sNodeId, {
                id: sNodeId,
                table: sTable,
                pk: sId,
                label: `${sTable}:${sId}`,
                expanded: false,
                depth: 0,
              });
              return next;
            });
          }
          if (!nodes.has(tNodeId)) {
            setNodes((prev) => {
              const next = new Map(prev);
              next.set(tNodeId, {
                id: tNodeId,
                table: tTable,
                pk: tId,
                label: `${tTable}:${tId}`,
                expanded: false,
                depth: 0,
              });
              return next;
            });
          }

          // Add missing edges
          const edgeId = `${sNodeId}->${edgeType}->${tNodeId}`;
          setEdges((prev) => {
            const next = new Map(prev);
            if (!next.has(edgeId)) {
              next.set(edgeId, {
                id: edgeId,
                from: sNodeId,
                to: tNodeId,
                edgeType,
                label: edgeType,
              });
            }
            return next;
          });
        }
      }

      setHighlightedPath(pathNodes);
      setHighlightedEdges(pathEdges);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Path finding failed");
    } finally {
      setLoading(false);
    }
  }, [database, nodes, selectedNodes]);

  const handleAddStartNode = useCallback(async () => {
    if (!startTable || !startId) return;

    const nodeId = makeNodeId(startTable, startId);
    if (nodes.has(nodeId)) {
      setError("Node already in graph");
      return;
    }

    setNodes((prev) => {
      const next = new Map(prev);
      next.set(nodeId, {
        id: nodeId,
        table: startTable,
        pk: startId,
        label: `${startTable}:${startId}`,
        expanded: false,
        depth: 0,
      });
      return next;
    });

    setStartTable("");
    setStartId("");
    setError(null);
  }, [startTable, startId, nodes]);

  const handleClear = useCallback(() => {
    setNodes(new Map());
    setEdges(new Map());
    setSelectedNodes([]);
    setHighlightedPath(new Set());
    setHighlightedEdges(new Set());
    setDetailNode(null);
    setError(null);
  }, []);

  const handleClearPath = useCallback(() => {
    setHighlightedPath(new Set());
    setHighlightedEdges(new Set());
  }, []);

  const toggleEdgeTypeFilter = useCallback((edgeType: string) => {
    setFilters((prev) => {
      const next = new Set(prev.visibleEdgeTypes);
      if (next.has(edgeType)) {
        next.delete(edgeType);
      } else {
        next.add(edgeType);
      }
      return { ...prev, visibleEdgeTypes: next };
    });
  }, []);

  const handleContextAction = useCallback(
    (action: string) => {
      if (!contextMenu) return;
      const { nodeId } = contextMenu;

      switch (action) {
        case "expand_out":
          expandNode(nodeId, "out");
          break;
        case "expand_in":
          expandNode(nodeId, "in");
          break;
        case "expand_both":
          expandNode(nodeId, "both");
          break;
        case "show_details":
          showNodeDetails(nodeId);
          break;
        case "remove":
          removeNode(nodeId);
          break;
      }
      setContextMenu(null);
    },
    [contextMenu, expandNode, showNodeDetails, removeNode]
  );

  // Close context menu on outside click
  useEffect(() => {
    const handler = () => setContextMenu(null);
    window.addEventListener("click", handler);
    return () => window.removeEventListener("click", handler);
  }, []);

  return (
    <div className="flex flex-col h-full">
      {/* Toolbar */}
      <div className="flex items-center gap-2 p-2 border-b border-gray-800 bg-gray-900/50 flex-wrap">
        {/* Database selector */}
        <select
          value={database}
          onChange={(e) => setDatabase(e.target.value)}
          className="bg-gray-800 text-gray-200 text-xs px-2 py-1 rounded border border-gray-700"
        >
          {databases.map((db) => (
            <option key={db} value={db}>
              {db}
            </option>
          ))}
        </select>

        {/* Add start node */}
        <div className="flex items-center gap-1">
          <input
            type="text"
            placeholder="Table"
            value={startTable}
            onChange={(e) => setStartTable(e.target.value)}
            className="bg-gray-800 text-gray-200 text-xs px-2 py-1 rounded border border-gray-700 w-24"
          />
          <input
            type="text"
            placeholder="ID"
            value={startId}
            onChange={(e) => setStartId(e.target.value)}
            onKeyDown={(e) => e.key === "Enter" && handleAddStartNode()}
            className="bg-gray-800 text-gray-200 text-xs px-2 py-1 rounded border border-gray-700 w-16"
          />
          <button
            onClick={handleAddStartNode}
            disabled={!startTable || !startId}
            className="px-2 py-1 text-xs bg-blue-600 hover:bg-blue-500 disabled:bg-gray-700 disabled:text-gray-500 text-white rounded"
          >
            Add Node
          </button>
        </div>

        <div className="w-px h-5 bg-gray-700" />

        {/* Layout selector */}
        <div className="flex items-center gap-1">
          <span className="text-xs text-gray-500">Layout:</span>
          {(["force", "hierarchical", "circular"] as LayoutType[]).map((l) => (
            <button
              key={l}
              onClick={() => setLayout(l)}
              className={`px-2 py-1 text-xs rounded ${
                layout === l
                  ? "bg-blue-600 text-white"
                  : "bg-gray-800 text-gray-400 hover:text-gray-200"
              }`}
            >
              {l === "force"
                ? "Force"
                : l === "hierarchical"
                  ? "Hierarchy"
                  : "Circular"}
            </button>
          ))}
        </div>

        <div className="w-px h-5 bg-gray-700" />

        {/* Path finding */}
        <button
          onClick={handleFindPath}
          disabled={selectedNodes.length !== 2 || loading}
          className="px-2 py-1 text-xs bg-amber-600 hover:bg-amber-500 disabled:bg-gray-700 disabled:text-gray-500 text-white rounded"
          title="Select two nodes (shift-click) then find shortest path"
        >
          Find Path
        </button>
        {highlightedPath.size > 0 && (
          <button
            onClick={handleClearPath}
            className="px-2 py-1 text-xs bg-gray-700 hover:bg-gray-600 text-gray-300 rounded"
          >
            Clear Path
          </button>
        )}

        <div className="flex-1" />

        {/* Status */}
        <span className="text-xs text-gray-500">
          {nodes.size} nodes, {edges.size} edges
        </span>
        {loading && (
          <span className="text-xs text-blue-400 animate-pulse">Loading...</span>
        )}
        <button
          onClick={handleClear}
          className="px-2 py-1 text-xs bg-red-900/50 hover:bg-red-800/50 text-red-400 rounded"
        >
          Clear All
        </button>
      </div>

      {/* Error banner */}
      {error && (
        <div className="px-3 py-1.5 bg-red-900/30 border-b border-red-800/50 text-red-400 text-xs flex items-center justify-between">
          <span>{error}</span>
          <button
            onClick={() => setError(null)}
            className="text-red-500 hover:text-red-300 ml-2"
          >
            x
          </button>
        </div>
      )}

      <div className="flex flex-1 overflow-hidden">
        {/* Main graph area */}
        <div className="flex-1 relative">
          {nodes.size === 0 ? (
            <div className="flex items-center justify-center h-full text-gray-600">
              <div className="text-center">
                <p className="text-lg mb-2">Graph Explorer</p>
                <p className="text-sm">
                  Enter a table name and ID above to start exploring
                </p>
                <p className="text-xs mt-1 text-gray-700">
                  Double-click nodes to expand | Shift-click two nodes to find
                  paths | Right-click for options
                </p>
              </div>
            </div>
          ) : (
            <div ref={containerRef} className="w-full h-full" />
          )}

          {/* Context menu */}
          {contextMenu && (
            <div
              className="fixed bg-gray-800 border border-gray-700 rounded shadow-xl py-1 z-50 min-w-[160px]"
              style={{ left: contextMenu.x, top: contextMenu.y }}
              onClick={(e) => e.stopPropagation()}
            >
              <button
                className="w-full px-3 py-1.5 text-xs text-left text-gray-200 hover:bg-gray-700"
                onClick={() => handleContextAction("expand_out")}
              >
                Expand Out
              </button>
              <button
                className="w-full px-3 py-1.5 text-xs text-left text-gray-200 hover:bg-gray-700"
                onClick={() => handleContextAction("expand_in")}
              >
                Expand In
              </button>
              <button
                className="w-full px-3 py-1.5 text-xs text-left text-gray-200 hover:bg-gray-700"
                onClick={() => handleContextAction("expand_both")}
              >
                Expand Both
              </button>
              <div className="border-t border-gray-700 my-1" />
              <button
                className="w-full px-3 py-1.5 text-xs text-left text-gray-200 hover:bg-gray-700"
                onClick={() => handleContextAction("show_details")}
              >
                Show Details
              </button>
              <div className="border-t border-gray-700 my-1" />
              <button
                className="w-full px-3 py-1.5 text-xs text-left text-red-400 hover:bg-gray-700"
                onClick={() => handleContextAction("remove")}
              >
                Remove
              </button>
            </div>
          )}
        </div>

        {/* Right sidebar: filters + details */}
        <div className="w-56 border-l border-gray-800 flex flex-col overflow-y-auto bg-gray-900/30">
          {/* Legend */}
          {graphTables.length > 0 && (
            <div className="p-2 border-b border-gray-800">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Tables
              </div>
              {graphTables.map((t) => (
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

          {/* Edge type filter */}
          {graphEdgeTypes.length > 0 && (
            <div className="p-2 border-b border-gray-800">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Edge Types
              </div>
              {graphEdgeTypes.map((et) => (
                <label
                  key={et}
                  className="flex items-center gap-1.5 py-0.5 cursor-pointer"
                >
                  <input
                    type="checkbox"
                    checked={
                      filters.visibleEdgeTypes.size === 0 ||
                      filters.visibleEdgeTypes.has(et)
                    }
                    onChange={() => toggleEdgeTypeFilter(et)}
                    className="w-3 h-3 rounded accent-blue-500"
                  />
                  <span className="text-xs text-gray-300">{et}</span>
                </label>
              ))}
            </div>
          )}

          {/* Depth filter */}
          <div className="p-2 border-b border-gray-800">
            <div className="text-xs text-gray-500 mb-1 font-medium">
              Max Depth: {filters.maxDepth}
            </div>
            <input
              type="range"
              min={1}
              max={10}
              value={filters.maxDepth}
              onChange={(e) =>
                setFilters((prev) => ({
                  ...prev,
                  maxDepth: parseInt(e.target.value, 10),
                }))
              }
              className="w-full accent-blue-500"
            />
          </div>

          {/* Node details */}
          {detailNode && (
            <div className="p-2 flex-1">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Node Details
              </div>
              <div className="text-xs text-blue-400 mb-2">
                {detailNode.node.table}:{detailNode.node.pk}
              </div>
              <div className="space-y-1">
                {Object.entries(detailNode.data).map(([key, value]) => (
                  <div key={key}>
                    <span className="text-xs text-gray-500">{key}: </span>
                    <span className="text-xs text-gray-300">
                      {value === null ? "null" : String(value)}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          )}

          {/* Selection info */}
          {selectedNodes.length > 0 && !detailNode && (
            <div className="p-2">
              <div className="text-xs text-gray-500 mb-1 font-medium">
                Selected ({selectedNodes.length})
              </div>
              {selectedNodes.map((id) => {
                const { table, pk } = parseNodeId(id);
                return (
                  <div key={id} className="text-xs text-gray-300 py-0.5">
                    {table}:{pk}
                  </div>
                );
              })}
              {selectedNodes.length === 2 && (
                <p className="text-xs text-amber-400 mt-1">
                  Click &quot;Find Path&quot; to highlight shortest path
                </p>
              )}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
