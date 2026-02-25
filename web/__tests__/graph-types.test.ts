import { describe, it, expect } from "vitest";
import type {
  GraphNode,
  GraphEdge,
  LayoutType,
  ContextAction,
  GraphFilters,
  ContextMenuState,
  TraverseResult,
} from "@/lib/graph-types";

describe("GraphNode type", () => {
  it("can create a basic graph node", () => {
    const node: GraphNode = {
      id: "users:1",
      table: "users",
      pk: "1",
      label: "users:1",
      expanded: false,
      depth: 0,
    };
    expect(node.id).toBe("users:1");
    expect(node.expanded).toBe(false);
    expect(node.depth).toBe(0);
  });

  it("can include optional data", () => {
    const node: GraphNode = {
      id: "users:1",
      table: "users",
      pk: "1",
      label: "users:1",
      expanded: true,
      depth: 2,
      data: { name: "Alice", age: 30 },
    };
    expect(node.data).toEqual({ name: "Alice", age: 30 });
  });
});

describe("GraphEdge type", () => {
  it("can create a basic graph edge", () => {
    const edge: GraphEdge = {
      id: "users:1->follows->users:2",
      from: "users:1",
      to: "users:2",
      edgeType: "follows",
      label: "follows",
    };
    expect(edge.from).toBe("users:1");
    expect(edge.to).toBe("users:2");
    expect(edge.edgeType).toBe("follows");
  });
});

describe("LayoutType type", () => {
  it("accepts all valid layout types", () => {
    const layouts: LayoutType[] = ["force", "hierarchical", "circular"];
    expect(layouts).toHaveLength(3);
    expect(layouts).toContain("force");
    expect(layouts).toContain("hierarchical");
    expect(layouts).toContain("circular");
  });
});

describe("ContextAction type", () => {
  it("accepts all valid context actions", () => {
    const actions: ContextAction[] = [
      "expand_out",
      "expand_in",
      "expand_both",
      "show_details",
      "remove",
    ];
    expect(actions).toHaveLength(5);
  });
});

describe("GraphFilters", () => {
  it("can create default filters", () => {
    const filters: GraphFilters = {
      visibleEdgeTypes: new Set(),
      maxDepth: 10,
    };
    expect(filters.visibleEdgeTypes.size).toBe(0);
    expect(filters.maxDepth).toBe(10);
  });

  it("can filter by specific edge types", () => {
    const filters: GraphFilters = {
      visibleEdgeTypes: new Set(["follows", "likes"]),
      maxDepth: 5,
    };
    expect(filters.visibleEdgeTypes.has("follows")).toBe(true);
    expect(filters.visibleEdgeTypes.has("likes")).toBe(true);
    expect(filters.visibleEdgeTypes.has("blocks")).toBe(false);
  });
});

describe("ContextMenuState", () => {
  it("stores position and node id", () => {
    const menu: ContextMenuState = { x: 100, y: 200, nodeId: "users:1" };
    expect(menu.x).toBe(100);
    expect(menu.y).toBe(200);
    expect(menu.nodeId).toBe("users:1");
  });
});

describe("TraverseResult", () => {
  it("can represent an empty traversal", () => {
    const result: TraverseResult = { nodes: [], edges: [] };
    expect(result.nodes).toHaveLength(0);
    expect(result.edges).toHaveLength(0);
  });

  it("can represent a traversal with nodes and edges", () => {
    const result: TraverseResult = {
      nodes: [
        {
          id: "users:2",
          table: "users",
          pk: "2",
          label: "users:2",
          expanded: false,
          depth: 1,
        },
      ],
      edges: [
        {
          id: "users:1->follows->users:2",
          from: "users:1",
          to: "users:2",
          edgeType: "follows",
          label: "follows",
        },
      ],
    };
    expect(result.nodes).toHaveLength(1);
    expect(result.edges).toHaveLength(1);
  });
});
