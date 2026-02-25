/** A node in the graph visualization. */
export interface GraphNode {
  /** Unique identifier: "table:id" */
  id: string;
  /** Source table name */
  table: string;
  /** Primary key value */
  pk: string;
  /** Display label (table + pk) */
  label: string;
  /** Whether this node has been expanded */
  expanded: boolean;
  /** Depth from the root node (hops) */
  depth: number;
  /** Full row data for details panel */
  data?: Record<string, unknown>;
}

/** An edge in the graph visualization. */
export interface GraphEdge {
  /** Unique identifier: "from->edgeType->to" */
  id: string;
  /** Source node id */
  from: string;
  /** Target node id */
  to: string;
  /** Edge type name */
  edgeType: string;
  /** Display label */
  label: string;
}

/** Layout algorithm options. */
export type LayoutType = "force" | "hierarchical" | "circular";

/** Context menu action types. */
export type ContextAction =
  | "expand_out"
  | "expand_in"
  | "expand_both"
  | "show_details"
  | "remove";

/** Graph filter state. */
export interface GraphFilters {
  /** Edge types to show (empty = show all) */
  visibleEdgeTypes: Set<string>;
  /** Max depth to display */
  maxDepth: number;
}

/** Result of a traverse API call, parsed into nodes and edges. */
export interface TraverseResult {
  nodes: GraphNode[];
  edges: GraphEdge[];
}

/** Context menu state. */
export interface ContextMenuState {
  x: number;
  y: number;
  nodeId: string;
}
