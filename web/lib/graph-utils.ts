import type { GraphNode, GraphEdge, TraverseResult } from "./graph-types";
import type { ConnectionParams } from "./connection-types";

const API_BASE = "/api";

interface TraverseApiResponse {
  columns: string[];
  rows: (string | number | boolean | null)[][];
}

/** Execute a graph traversal and return parsed nodes/edges. */
export async function traverseNode(
  database: string,
  table: string,
  id: string,
  direction: "out" | "in" | "both",
  parentDepth: number,
  edgeType?: string,
  conn?: ConnectionParams
): Promise<TraverseResult> {
  const res = await fetch(`${API_BASE}/graph`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      action: "traverse",
      database,
      table,
      id,
      direction,
      edgeType,
      connection: conn,
    }),
  });
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`Traverse error ${res.status}: ${body}`);
  }
  const data: TraverseApiResponse = await res.json();
  return parseTraverseResult(data, table, id, parentDepth);
}

/** Find shortest path between two nodes. */
export async function findShortestPath(
  database: string,
  sourceTable: string,
  sourceId: string,
  targetTable: string,
  targetId: string,
  conn?: ConnectionParams
): Promise<{ columns: string[]; rows: (string | number | boolean | null)[][] }> {
  const res = await fetch(`${API_BASE}/graph`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      action: "shortest_path",
      database,
      sourceTable,
      sourceId,
      targetTable,
      targetId,
      connection: conn,
    }),
  });
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`Shortest path error ${res.status}: ${body}`);
  }
  return res.json();
}

/** Fetch full row data for a node. */
export async function fetchNodeDetails(
  database: string,
  table: string,
  id: string,
  conn?: ConnectionParams
): Promise<Record<string, unknown>> {
  const res = await fetch(`${API_BASE}/graph`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ action: "node_details", database, table, id, connection: conn }),
  });
  if (!res.ok) {
    const body = await res.text();
    throw new Error(`Node details error ${res.status}: ${body}`);
  }
  const data: TraverseApiResponse = await res.json();
  if (data.rows.length === 0) return {};
  const record: Record<string, unknown> = {};
  data.columns.forEach((col, i) => {
    record[col] = data.rows[0][i];
  });
  return record;
}

/**
 * Parse the result of a TRAVERSE query into graph nodes and edges.
 *
 * Expected columns from TRAVERSE:
 *   source_table, source_id, edge_type, target_table, target_id
 *
 * This may vary — we adapt to whatever columns are returned.
 */
function parseTraverseResult(
  data: TraverseApiResponse,
  originTable: string,
  originId: string,
  parentDepth: number
): TraverseResult {
  const nodes: GraphNode[] = [];
  const edges: GraphEdge[] = [];
  const seenNodes = new Set<string>();

  // Map column indices
  const colIdx = new Map<string, number>();
  data.columns.forEach((col, i) => colIdx.set(col.toLowerCase(), i));

  // Try to detect column positions
  const srcTableIdx = colIdx.get("source_table") ?? colIdx.get("src_table") ?? -1;
  const srcIdIdx = colIdx.get("source_id") ?? colIdx.get("src_id") ?? -1;
  const edgeTypeIdx = colIdx.get("edge_type") ?? colIdx.get("edgetype") ?? -1;
  const tgtTableIdx = colIdx.get("target_table") ?? colIdx.get("tgt_table") ?? -1;
  const tgtIdIdx = colIdx.get("target_id") ?? colIdx.get("tgt_id") ?? -1;

  // If we can't detect edge columns, return raw rows as connected nodes
  if (srcTableIdx === -1 || tgtTableIdx === -1) {
    // Fallback: treat each row as a neighbor node, using first two columns as table + id
    const tableIdx = colIdx.get("table") ?? colIdx.get("_table") ?? 0;
    const idIdx = colIdx.get("id") ?? colIdx.get("_id") ?? 1;

    for (const row of data.rows) {
      const nTable = String(row[tableIdx] ?? originTable);
      const nId = String(row[idIdx] ?? "");
      const nodeId = makeNodeId(nTable, nId);
      if (!seenNodes.has(nodeId)) {
        seenNodes.add(nodeId);
        nodes.push({
          id: nodeId,
          table: nTable,
          pk: nId,
          label: `${nTable}:${nId}`,
          expanded: false,
          depth: parentDepth + 1,
        });
      }
      edges.push({
        id: `${makeNodeId(originTable, originId)}->${nodeId}`,
        from: makeNodeId(originTable, originId),
        to: nodeId,
        edgeType: "related",
        label: "related",
      });
    }
    return { nodes, edges };
  }

  // Standard edge-column parsing
  for (const row of data.rows) {
    const srcTable = String(row[srcTableIdx] ?? "");
    const srcId = String(row[srcIdIdx] ?? "");
    const edgeType = String(row[edgeTypeIdx] ?? "edge");
    const tgtTable = String(row[tgtTableIdx] ?? "");
    const tgtId = String(row[tgtIdIdx] ?? "");

    const srcNodeId = makeNodeId(srcTable, srcId);
    const tgtNodeId = makeNodeId(tgtTable, tgtId);

    if (!seenNodes.has(srcNodeId)) {
      seenNodes.add(srcNodeId);
      nodes.push({
        id: srcNodeId,
        table: srcTable,
        pk: srcId,
        label: `${srcTable}:${srcId}`,
        expanded: false,
        depth: parentDepth + 1,
      });
    }

    if (!seenNodes.has(tgtNodeId)) {
      seenNodes.add(tgtNodeId);
      nodes.push({
        id: tgtNodeId,
        table: tgtTable,
        pk: tgtId,
        label: `${tgtTable}:${tgtId}`,
        expanded: false,
        depth: parentDepth + 1,
      });
    }

    const edgeId = `${srcNodeId}->${edgeType}->${tgtNodeId}`;
    edges.push({
      id: edgeId,
      from: srcNodeId,
      to: tgtNodeId,
      edgeType,
      label: edgeType,
    });
  }

  return { nodes, edges };
}

export function makeNodeId(table: string, id: string): string {
  return `${table}:${id}`;
}

/** Deterministic color for a table name. */
export function tableColor(table: string): string {
  const colors = [
    "#3b82f6", // blue
    "#10b981", // emerald
    "#f59e0b", // amber
    "#ef4444", // red
    "#8b5cf6", // violet
    "#06b6d4", // cyan
    "#f97316", // orange
    "#ec4899", // pink
    "#14b8a6", // teal
    "#a855f7", // purple
  ];
  let hash = 0;
  for (let i = 0; i < table.length; i++) {
    hash = (hash * 31 + table.charCodeAt(i)) | 0;
  }
  return colors[Math.abs(hash) % colors.length];
}

/** Parse a node ID back to table and pk. */
export function parseNodeId(nodeId: string): { table: string; pk: string } {
  const idx = nodeId.indexOf(":");
  if (idx === -1) return { table: nodeId, pk: "" };
  return { table: nodeId.slice(0, idx), pk: nodeId.slice(idx + 1) };
}
