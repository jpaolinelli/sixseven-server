/**
 * Utilities for detecting and processing graph-aware query results.
 *
 * Meta-column detection:
 *  - __node, __depth, __source → node-centric TRAVERSE result
 *  - __from, __to             → edge-centric TRAVERSE result (MODE EDGES)
 */

/** Known meta-columns emitted by enriched TRAVERSE. */
const NODE_META_COLUMNS = ["__node", "__depth", "__source"];
const EDGE_META_COLUMNS = ["__from", "__to"];

type CellValue = string | number | boolean | null;

/** Check if result columns indicate a graph (node-centric) result.
 *  Matches any node meta-column: __node, __depth, or __source. */
export function isNodeCentricResult(columns: string[]): boolean {
  const lower = columns.map((c) => c.toLowerCase());
  return NODE_META_COLUMNS.some((meta) => lower.includes(meta));
}

/** Check if result columns indicate an edge-centric result. */
export function isEdgeCentricResult(columns: string[]): boolean {
  const lower = columns.map((c) => c.toLowerCase());
  return lower.includes("__from") && lower.includes("__to");
}

/** Check if result columns indicate any kind of graph result. */
export function isGraphResult(columns: string[]): boolean {
  return isNodeCentricResult(columns) || isEdgeCentricResult(columns);
}

/**
 * Detect if a SQL string contains a TRAVERSE clause.
 * Matches: SELECT ... FROM TRAVERSE ...
 */
export function isTraverseQuery(sql: string): boolean {
  return /\bFROM\s+TRAVERSE\b/i.test(sql);
}

/**
 * Build an edge-mode variant of a TRAVERSE query by appending MODE EDGES.
 * If the query already has MODE EDGES, returns it unchanged.
 */
export function buildEdgeQuery(sql: string): string {
  if (/\bMODE\s+EDGES\b/i.test(sql)) return sql;
  // Remove trailing semicolons/whitespace, then append MODE EDGES
  return sql.replace(/\s*;?\s*$/, "") + " MODE EDGES";
}

/** Column index lookup for a result set. */
function colIndex(columns: string[], name: string): number {
  return columns.findIndex((c) => c.toLowerCase() === name.toLowerCase());
}

/** Parsed graph node for visualization. */
export interface GraphViewNode {
  id: string;
  label: string;
  table: string;
  pk: string;
  attributes: Record<string, CellValue>;
}

/** Parsed graph edge for visualization. */
export interface GraphViewEdge {
  id: string;
  from: string;
  to: string;
  edgeType: string;
  label: string;
  properties: Record<string, CellValue>;
}

/** Combined graph data for the visualization. */
export interface GraphViewData {
  nodes: GraphViewNode[];
  edges: GraphViewEdge[];
}

/**
 * Separate a column list into user columns and meta-columns.
 * User columns are displayed in tables; meta-columns provide graph structure.
 */
export function classifyColumns(columns: string[]): {
  userColumns: string[];
  metaColumns: string[];
} {
  const meta = new Set([
    ...NODE_META_COLUMNS,
    ...EDGE_META_COLUMNS,
  ]);
  const userColumns: string[] = [];
  const metaColumns: string[] = [];
  for (const col of columns) {
    if (meta.has(col.toLowerCase())) {
      metaColumns.push(col);
    } else {
      userColumns.push(col);
    }
  }
  return { userColumns, metaColumns };
}

/**
 * Parse node-centric TRAVERSE results into graph nodes.
 *
 * Expected meta-columns: __node (PK), __depth, __source (optional).
 * All other columns are user attributes.
 */
export function parseNodesFromResult(
  columns: string[],
  rows: CellValue[][]
): GraphViewNode[] {
  const nodeIdx = colIndex(columns, "__node");
  const sourceIdx = colIndex(columns, "__source");
  if (nodeIdx === -1) return [];

  const seen = new Set<string>();
  const nodes: GraphViewNode[] = [];

  for (const row of rows) {
    const pk = String(row[nodeIdx] ?? "");
    // Determine table from __source or use a generic label
    const table = sourceIdx >= 0 ? String(row[sourceIdx] ?? "node") : "node";
    const id = `${table}:${pk}`;

    if (seen.has(id)) continue;
    seen.add(id);

    // Collect all user attributes
    const attributes: Record<string, CellValue> = {};
    for (let i = 0; i < columns.length; i++) {
      const colLower = columns[i].toLowerCase();
      if (colLower !== "__node" && colLower !== "__depth" && colLower !== "__source") {
        attributes[columns[i]] = row[i];
      }
    }

    // Label: use first non-null user attribute or PK
    const firstAttr = Object.values(attributes).find(
      (v) => v !== null && v !== undefined
    );
    const label = firstAttr !== undefined ? String(firstAttr) : pk;

    nodes.push({ id, label, table, pk, attributes });
  }

  return nodes;
}

/**
 * Parse edge-centric TRAVERSE results into graph edges.
 *
 * Expected meta-columns: __from, __to, __depth (optional).
 * All other columns are edge properties.
 */
export function parseEdgesFromResult(
  columns: string[],
  rows: CellValue[][]
): GraphViewEdge[] {
  const fromIdx = colIndex(columns, "__from");
  const toIdx = colIndex(columns, "__to");
  if (fromIdx === -1 || toIdx === -1) return [];

  const edges: GraphViewEdge[] = [];

  for (const row of rows) {
    const from = String(row[fromIdx] ?? "");
    const to = String(row[toIdx] ?? "");

    // Collect edge properties (non-meta columns)
    const properties: Record<string, CellValue> = {};
    for (let i = 0; i < columns.length; i++) {
      const colLower = columns[i].toLowerCase();
      if (colLower !== "__from" && colLower !== "__to" && colLower !== "__depth") {
        properties[columns[i]] = row[i];
      }
    }

    // Extract edge type from property column names (e.g., "follows.weight" → "follows")
    const edgeType = detectEdgeType(columns) || "edge";
    const id = `${from}->${edgeType}->${to}`;

    edges.push({
      id,
      from,
      to,
      edgeType,
      label: edgeType,
      properties,
    });
  }

  return edges;
}

/**
 * Detect edge type from column names.
 * Looks for "tablename.property" patterns (e.g., "follows.weight").
 */
function detectEdgeType(columns: string[]): string | null {
  for (const col of columns) {
    const dotIdx = col.indexOf(".");
    if (dotIdx > 0) {
      return col.substring(0, dotIdx);
    }
  }
  return null;
}

/**
 * Build complete graph data from both node-centric and edge-centric results.
 * The node result provides node positions/attributes, the edge result provides connections.
 */
export function buildGraphData(
  nodeColumns: string[],
  nodeRows: CellValue[][],
  edgeColumns: string[],
  edgeRows: CellValue[][]
): GraphViewData {
  const nodes = parseNodesFromResult(nodeColumns, nodeRows);
  const edges = parseEdgesFromResult(edgeColumns, edgeRows);

  // Build a set of node IDs for edge resolution
  const nodeById = new Map<string, GraphViewNode>();
  for (const n of nodes) {
    nodeById.set(n.pk, n);
  }

  // Resolve edge endpoints: __from/__to are PKs, map them to node IDs
  const resolvedEdges = edges.map((e) => {
    const fromNode = nodeById.get(e.from);
    const toNode = nodeById.get(e.to);
    return {
      ...e,
      from: fromNode ? fromNode.id : `node:${e.from}`,
      to: toNode ? toNode.id : `node:${e.to}`,
    };
  });

  // Add placeholder nodes for edge endpoints that aren't in the node set
  const nodeIds = new Set(nodes.map((n) => n.id));
  for (const edge of resolvedEdges) {
    if (!nodeIds.has(edge.from)) {
      const pk = edge.from.includes(":") ? edge.from.split(":")[1] : edge.from;
      nodes.push({
        id: edge.from,
        label: pk,
        table: "node",
        pk,
        attributes: {},
      });
      nodeIds.add(edge.from);
    }
    if (!nodeIds.has(edge.to)) {
      const pk = edge.to.includes(":") ? edge.to.split(":")[1] : edge.to;
      nodes.push({
        id: edge.to,
        label: pk,
        table: "node",
        pk,
        attributes: {},
      });
      nodeIds.add(edge.to);
    }
  }

  return { nodes, edges: resolvedEdges };
}

/**
 * Format edge properties for tooltip display.
 * Shows "key: value" pairs on separate lines.
 */
export function formatEdgeTooltip(edgeType: string, properties: Record<string, CellValue>): string {
  const lines = [`Edge: ${edgeType}`];
  for (const [key, value] of Object.entries(properties)) {
    if (value !== null && value !== undefined) {
      lines.push(`${key}: ${String(value)}`);
    }
  }
  return lines.join("\n");
}
