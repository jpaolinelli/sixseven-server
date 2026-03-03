import { describe, it, expect } from "vitest";
import {
  isNodeCentricResult,
  isEdgeCentricResult,
  isGraphResult,
  isTraverseQuery,
  buildEdgeQuery,
  classifyColumns,
  parseNodesFromResult,
  parseEdgesFromResult,
  buildGraphData,
  formatEdgeTooltip,
} from "@/lib/graph-query-utils";

// ── Detection functions ──

describe("isNodeCentricResult", () => {
  it("returns true when __node column is present", () => {
    expect(isNodeCentricResult(["name", "email", "__node", "__depth"])).toBe(true);
  });

  it("returns true with case-insensitive match", () => {
    expect(isNodeCentricResult(["__NODE", "name"])).toBe(true);
  });

  it("returns true when __depth column is present without __node", () => {
    expect(isNodeCentricResult(["name", "__depth"])).toBe(true);
  });

  it("returns true when __source column is present without __node", () => {
    expect(isNodeCentricResult(["name", "__source"])).toBe(true);
  });

  it("returns true with case-insensitive __DEPTH", () => {
    expect(isNodeCentricResult(["name", "__DEPTH"])).toBe(true);
  });

  it("returns false when no node meta-columns are present", () => {
    expect(isNodeCentricResult(["name", "email", "id"])).toBe(false);
  });

  it("returns false for empty columns", () => {
    expect(isNodeCentricResult([])).toBe(false);
  });

  it("returns false for edge-only columns", () => {
    expect(isNodeCentricResult(["__from", "__to", "weight"])).toBe(false);
  });
});

describe("isEdgeCentricResult", () => {
  it("returns true when __from and __to are present", () => {
    expect(isEdgeCentricResult(["__from", "__to", "__depth"])).toBe(true);
  });

  it("returns true with case-insensitive match", () => {
    expect(isEdgeCentricResult(["__FROM", "__TO", "weight"])).toBe(true);
  });

  it("returns false when only __from is present", () => {
    expect(isEdgeCentricResult(["__from", "name"])).toBe(false);
  });

  it("returns false when only __to is present", () => {
    expect(isEdgeCentricResult(["__to", "name"])).toBe(false);
  });

  it("returns false for empty columns", () => {
    expect(isEdgeCentricResult([])).toBe(false);
  });
});

describe("isGraphResult", () => {
  it("returns true for node-centric results", () => {
    expect(isGraphResult(["name", "__node", "__depth"])).toBe(true);
  });

  it("returns true for edge-centric results", () => {
    expect(isGraphResult(["__from", "__to", "weight"])).toBe(true);
  });

  it("returns true when only __depth is present (GDB-309)", () => {
    expect(isGraphResult(["name", "__depth"])).toBe(true);
  });

  it("returns false for standard table results", () => {
    expect(isGraphResult(["id", "name", "email"])).toBe(false);
  });

  it("returns false for empty columns", () => {
    expect(isGraphResult([])).toBe(false);
  });
});

// ── TRAVERSE query detection ──

describe("isTraverseQuery", () => {
  it("detects SELECT ... FROM TRAVERSE", () => {
    expect(
      isTraverseQuery(
        "SELECT name, email FROM TRAVERSE follows FROM users(1) DIRECTION OUT"
      )
    ).toBe(true);
  });

  it("detects case-insensitive FROM TRAVERSE", () => {
    expect(
      isTraverseQuery("select * from traverse follows from users(1)")
    ).toBe(true);
  });

  it("detects with leading whitespace", () => {
    expect(
      isTraverseQuery("  SELECT * FROM  TRAVERSE  follows FROM users(1)")
    ).toBe(true);
  });

  it("returns false for standard SELECT", () => {
    expect(isTraverseQuery("SELECT * FROM users")).toBe(false);
  });

  it("returns false for empty string", () => {
    expect(isTraverseQuery("")).toBe(false);
  });

  it("returns false for INSERT", () => {
    expect(isTraverseQuery("INSERT INTO users (name) VALUES ('alice')")).toBe(false);
  });
});

// ── buildEdgeQuery ──

describe("buildEdgeQuery", () => {
  it("appends MODE EDGES to a TRAVERSE query", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1)";
    expect(buildEdgeQuery(sql)).toBe(
      "SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES"
    );
  });

  it("strips trailing semicolons before appending", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1);";
    expect(buildEdgeQuery(sql)).toBe(
      "SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES"
    );
  });

  it("strips trailing whitespace and semicolons", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1) ;  ";
    expect(buildEdgeQuery(sql)).toBe(
      "SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES"
    );
  });

  it("does not duplicate MODE EDGES if already present", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES";
    expect(buildEdgeQuery(sql)).toBe(sql);
  });

  it("detects MODE EDGES case-insensitively", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1) mode edges";
    expect(buildEdgeQuery(sql)).toBe(sql);
  });
});

// ── classifyColumns ──

describe("classifyColumns", () => {
  it("separates meta-columns from user columns", () => {
    const result = classifyColumns(["name", "email", "__node", "__depth", "__source"]);
    expect(result.userColumns).toEqual(["name", "email"]);
    expect(result.metaColumns).toEqual(["__node", "__depth", "__source"]);
  });

  it("handles edge meta-columns", () => {
    const result = classifyColumns(["__from", "__to", "follows.weight"]);
    expect(result.userColumns).toEqual(["follows.weight"]);
    expect(result.metaColumns).toEqual(["__from", "__to"]);
  });

  it("handles no meta-columns", () => {
    const result = classifyColumns(["id", "name", "email"]);
    expect(result.userColumns).toEqual(["id", "name", "email"]);
    expect(result.metaColumns).toEqual([]);
  });

  it("handles empty array", () => {
    const result = classifyColumns([]);
    expect(result.userColumns).toEqual([]);
    expect(result.metaColumns).toEqual([]);
  });
});

// ── parseNodesFromResult ──

describe("parseNodesFromResult", () => {
  it("parses nodes from node-centric result", () => {
    const columns = ["name", "email", "__node", "__depth", "__source"];
    const rows = [
      ["alice", "alice@example.com", 1, 0, "users"],
      ["bob", "bob@example.com", 2, 1, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);

    expect(nodes).toHaveLength(2);
    expect(nodes[0].id).toBe("users:1");
    expect(nodes[0].pk).toBe("1");
    expect(nodes[0].table).toBe("users");
    expect(nodes[0].label).toBe("alice");
    expect(nodes[0].attributes).toEqual({ name: "alice", email: "alice@example.com" });

    expect(nodes[1].id).toBe("users:2");
    expect(nodes[1].label).toBe("bob");
  });

  it("deduplicates nodes by id", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows = [
      ["alice", 1, 0, "users"],
      ["alice", 1, 0, "users"], // duplicate
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toHaveLength(1);
  });

  it("returns empty array when __node column is missing", () => {
    const columns = ["name", "email"];
    const rows = [["alice", "alice@example.com"]];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toEqual([]);
  });

  it("uses 'node' as default table when __source is missing", () => {
    const columns = ["name", "__node", "__depth"];
    const rows = [["alice", 1, 0]];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].table).toBe("node");
    expect(nodes[0].id).toBe("node:1");
  });

  it("handles null attribute values", () => {
    const columns = ["name", "email", "__node", "__depth", "__source"];
    const rows = [["alice", null, 1, 0, "users"]];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].attributes.email).toBeNull();
  });

  it("handles empty rows", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const nodes = parseNodesFromResult(columns, []);
    expect(nodes).toEqual([]);
  });
});

// ── parseEdgesFromResult ──

describe("parseEdgesFromResult", () => {
  it("parses edges from edge-centric result", () => {
    const columns = ["__from", "__to", "__depth", "follows.weight", "follows.created_at"];
    const rows = [
      [1, 2, 1, 0.8, "2024-01-01"],
      [1, 3, 1, 0.5, "2024-02-15"],
    ];
    const edges = parseEdgesFromResult(columns, rows);

    expect(edges).toHaveLength(2);
    expect(edges[0].from).toBe("1");
    expect(edges[0].to).toBe("2");
    expect(edges[0].edgeType).toBe("follows");
    expect(edges[0].properties).toEqual({
      "follows.weight": 0.8,
      "follows.created_at": "2024-01-01",
    });
  });

  it("returns empty array when __from is missing", () => {
    const columns = ["__to", "weight"];
    const rows = [[2, 0.8]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toEqual([]);
  });

  it("returns empty array when __to is missing", () => {
    const columns = ["__from", "weight"];
    const rows = [[1, 0.8]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toEqual([]);
  });

  it("uses 'edge' as default edge type when no dot-columns found", () => {
    const columns = ["__from", "__to", "weight"];
    const rows = [[1, 2, 0.8]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges[0].edgeType).toBe("edge");
  });

  it("handles empty rows", () => {
    const columns = ["__from", "__to"];
    const edges = parseEdgesFromResult(columns, []);
    expect(edges).toEqual([]);
  });
});

// ── buildGraphData ──

describe("buildGraphData", () => {
  it("combines node and edge results into graph data", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows = [
      ["alice", 1, 0, "users"],
      ["bob", 2, 1, "users"],
      ["charlie", 3, 1, "users"],
    ];
    const edgeColumns = ["__from", "__to", "follows.weight"];
    const edgeRows = [
      [1, 2, 0.8],
      [1, 3, 0.5],
    ];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);

    expect(graph.nodes).toHaveLength(3);
    expect(graph.edges).toHaveLength(2);
  });

  it("resolves edge endpoints to node IDs", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows = [
      ["alice", 1, 0, "users"],
      ["bob", 2, 1, "users"],
    ];
    const edgeColumns = ["__from", "__to"];
    const edgeRows = [[1, 2]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);

    expect(graph.edges[0].from).toBe("users:1");
    expect(graph.edges[0].to).toBe("users:2");
  });

  it("adds placeholder nodes for unresolved edge endpoints", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows = [["alice", 1, 0, "users"]];
    const edgeColumns = ["__from", "__to"];
    const edgeRows = [[1, 99]]; // node 99 not in node results

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);

    // Should have 2 nodes: alice + placeholder for 99
    expect(graph.nodes).toHaveLength(2);
    const placeholder = graph.nodes.find((n) => n.pk === "99");
    expect(placeholder).toBeDefined();
    expect(placeholder?.table).toBe("node");
  });

  it("handles empty edge data", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows = [["alice", 1, 0, "users"]];

    const graph = buildGraphData(nodeColumns, nodeRows, [], []);

    expect(graph.nodes).toHaveLength(1);
    expect(graph.edges).toHaveLength(0);
  });

  it("handles empty node data", () => {
    const edgeColumns = ["__from", "__to"];
    const edgeRows = [[1, 2]];

    const graph = buildGraphData([], [], edgeColumns, edgeRows);

    // Should create placeholder nodes from edge endpoints
    expect(graph.nodes).toHaveLength(2);
    expect(graph.edges).toHaveLength(1);
  });
});

// ── formatEdgeTooltip ──

describe("formatEdgeTooltip", () => {
  it("formats edge type and properties", () => {
    const tooltip = formatEdgeTooltip("follows", {
      "follows.weight": 0.8,
      "follows.created_at": "2024-01-01",
    });
    expect(tooltip).toContain("Edge: follows");
    expect(tooltip).toContain("follows.weight: 0.8");
    expect(tooltip).toContain("follows.created_at: 2024-01-01");
  });

  it("skips null property values", () => {
    const tooltip = formatEdgeTooltip("follows", {
      "follows.weight": null,
      "follows.created_at": "2024-01-01",
    });
    expect(tooltip).not.toContain("follows.weight");
    expect(tooltip).toContain("follows.created_at: 2024-01-01");
  });

  it("shows only edge type when no properties", () => {
    const tooltip = formatEdgeTooltip("follows", {});
    expect(tooltip).toBe("Edge: follows");
  });
});
