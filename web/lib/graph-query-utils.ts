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
 * Build an edge-mode variant of a TRAVERSE query.
 *
 * 1. Replaces the SELECT list with `*` (edge mode has a different output schema).
 * 2. Inserts MODE EDGES in the correct grammar position — after the TRAVERSE core
 *    (edge_type, FROM table(key), DIRECTION, MAX_DEPTH) and before WHERE/FETCH.
 *
 * If the query already has MODE EDGES, returns it unchanged.
 */
export function buildEdgeQuery(sql: string): string {
  if (/\bMODE\s+EDGES\b/i.test(sql)) return sql;

  // Remove trailing semicolons/whitespace
  let edgeSql = sql.replace(/\s*;?\s*$/, "");

  // Replace SELECT list with SELECT * (edge mode columns differ from node mode)
  // Use [\s\S] instead of . with 's' flag for ES2017 compatibility
  edgeSql = edgeSql.replace(
    /^(\s*SELECT)\s+[\s\S]+?(\s+FROM\s+TRAVERSE\b)/i,
    "$1 *$2"
  );

  // Strip ORDER BY and LIMIT — they may reference node-mode columns
  // that don't exist in edge mode (e.g. ORDER BY name).
  edgeSql = edgeSql.replace(/\s+ORDER\s+BY\s+[\s\S]+$/i, "");
  edgeSql = edgeSql.replace(/\s+LIMIT\s+\d+\s*$/i, "");

  // Insert MODE EDGES after the TRAVERSE core:
  //   TRAVERSE <edge> FROM <table>(<key>) [DIRECTION <dir>] [MAX_DEPTH <n>]
  const corePattern =
    /\bFROM\s+TRAVERSE\s+\S+\s+FROM\s+\S+\s*\([^)]*\)(?:\s+DIRECTION\s+(?:IN|OUT|BOTH))?(?:\s+MAX_DEPTH\s+\d+)?/i;
  const match = corePattern.exec(edgeSql);
  if (match) {
    const pos = match.index + match[0].length;
    return edgeSql.substring(0, pos) + " MODE EDGES" + edgeSql.substring(pos);
  }

  // Fallback: append at end (non-TRAVERSE or unusual SQL)
  return edgeSql + " MODE EDGES";
}

/**
 * Parse the starting node from a TRAVERSE query.
 * Extracts the source table and primary key from "FROM TRAVERSE <edge> FROM <table>(<pk>)".
 * Returns null if the pattern is not found.
 */
export function parseTraverseSource(
  sql: string
): { table: string; pk: string } | null {
  const match = sql.match(
    /\bFROM\s+TRAVERSE\s+\S+\s+FROM\s+(\w+)\s*\(\s*([^)]+?)\s*\)/i
  );
  if (!match) return null;
  return { table: match[1], pk: match[2] };
}

/**
 * Build a query to fetch the starting (source) node of a TRAVERSE.
 * The TRAVERSE result set does not include the starting node (depth 0),
 * so we fetch it separately for use in the graph visualization.
 *
 * Returns null if the source cannot be parsed from the SQL.
 */
export function buildSourceNodeQuery(sql: string): string | null {
  const source = parseTraverseSource(sql);
  if (!source) return null;

  // Quote numeric PKs as-is, quote string PKs with single quotes
  const pkLiteral = /^\d+$/.test(source.pk)
    ? source.pk
    : `'${source.pk.replace(/'/g, "''")}'`;

  return `SELECT * FROM ${source.table} WHERE id = ${pkLiteral}`;
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
 *
 * When __node is absent (user selected only __depth or __source),
 * each row becomes a unique node using its row index as a fallback PK.
 */
export function parseNodesFromResult(
  columns: string[],
  rows: CellValue[][]
): GraphViewNode[] {
  const nodeIdx = colIndex(columns, "__node");
  const sourceIdx = colIndex(columns, "__source");
  const hasNodeCol = nodeIdx !== -1;

  // Need at least one node meta-column to be a graph result
  if (!isNodeCentricResult(columns)) return [];

  const seen = new Set<string>();
  const nodes: GraphViewNode[] = [];

  for (let rowIdx = 0; rowIdx < rows.length; rowIdx++) {
    const row = rows[rowIdx];
    // Use __node PK if available, otherwise row index as fallback.
    // When __node is null, assign a synthetic unique ID per row to prevent
    // multiple null-PK rows from collapsing into a single node.
    const rawPk = hasNodeCol ? row[nodeIdx] : rowIdx;
    const isNullPk = hasNodeCol && (rawPk === null || rawPk === undefined);
    const pk = isNullPk ? `__null_${rowIdx}` : String(rawPk ?? rowIdx);
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

  const seen = new Set<string>();
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

    // Deduplicate edges with the same id.
    if (seen.has(id)) continue;
    seen.add(id);

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
 * Build a GraphViewNode from a source-node query result (SELECT * FROM table WHERE id = pk).
 * Returns null if the data is missing or incomplete.
 */
function parseSourceNode(
  sourceColumns: string[] | undefined,
  sourceRows: CellValue[][] | undefined
): GraphViewNode | null {
  if (!sourceColumns || !sourceRows || sourceRows.length === 0) return null;

  const row = sourceRows[0];
  // Look for the "id" column to get the PK
  const idIdx = colIndex(sourceColumns, "id");
  if (idIdx === -1) return null;

  const pk = String(row[idIdx] ?? "");

  // Collect attributes (all columns except id)
  const attributes: Record<string, CellValue> = {};
  for (let i = 0; i < sourceColumns.length; i++) {
    if (i !== idIdx) {
      attributes[sourceColumns[i]] = row[i];
    }
  }

  // Label: use a display-friendly column (name, title, label) or fallback to PK
  const labelCol = sourceColumns.find((c) =>
    ["name", "title", "label", "username", "email"].includes(c.toLowerCase())
  );
  const label = labelCol
    ? String(row[sourceColumns.indexOf(labelCol)] ?? pk)
    : pk;

  return { id: "", label, table: "", pk, attributes }; // id/table set by caller
}

/**
 * Build complete graph data from node-centric and edge-centric results,
 * plus an optional source node (the starting node of the TRAVERSE).
 *
 * The TRAVERSE result set does not include the starting node (depth 0),
 * so it is fetched separately and merged here for a complete graph.
 */
export function buildGraphData(
  nodeColumns: string[],
  nodeRows: CellValue[][],
  edgeColumns: string[],
  edgeRows: CellValue[][],
  sourceNodeColumns?: string[],
  sourceNodeRows?: CellValue[][]
): GraphViewData {
  const nodes = parseNodesFromResult(nodeColumns, nodeRows);
  const edges = parseEdgesFromResult(edgeColumns, edgeRows);

  // Parse source node data (the starting node of the TRAVERSE)
  const sourceNode = parseSourceNode(sourceNodeColumns, sourceNodeRows);

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

  // Add missing nodes for edge endpoints that aren't in the node set.
  // If we have source node data, use it instead of a bare placeholder.
  const nodeIds = new Set(nodes.map((n) => n.id));
  for (const edge of resolvedEdges) {
    for (const endpointId of [edge.from, edge.to]) {
      if (nodeIds.has(endpointId)) continue;

      const pk = endpointId.includes(":") ? endpointId.split(":")[1] : endpointId;

      // Check if the source node matches this endpoint
      if (sourceNode && sourceNode.pk === pk) {
        nodes.push({
          ...sourceNode,
          id: endpointId,
          table: endpointId.includes(":") ? endpointId.split(":")[0] : "node",
        });
      } else {
        // Bare placeholder (no data available)
        nodes.push({
          id: endpointId,
          label: pk,
          table: "node",
          pk,
          attributes: {},
        });
      }
      nodeIds.add(endpointId);
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
