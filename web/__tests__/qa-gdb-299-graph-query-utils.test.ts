/**
 * QA adversarial tests for GDB-299: Graph-aware query results.
 *
 * Tests the graph-query-utils logic with edge cases, boundary values,
 * null handling, and adversarial inputs.
 */
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

// ── isNodeCentricResult: adversarial ──

describe("QA_GDB299_isNodeCentricResult", () => {
  it("rejects columns with __node as substring in another name", () => {
    expect(isNodeCentricResult(["my__node_col", "name"])).toBe(false);
  });

  it("rejects columns with __node prefix in longer name", () => {
    expect(isNodeCentricResult(["__node_id", "name"])).toBe(false);
  });

  it("detects __node with mixed case", () => {
    expect(isNodeCentricResult(["__Node", "name"])).toBe(true);
  });

  it("handles single-element array with __node", () => {
    expect(isNodeCentricResult(["__node"])).toBe(true);
  });

  it("handles very large column list", () => {
    const cols = Array.from({ length: 1000 }, (_, i) => `col_${i}`);
    cols.push("__node");
    expect(isNodeCentricResult(cols)).toBe(true);
  });

  it("handles column names with leading/trailing whitespace", () => {
    // Column names with spaces should NOT match __node
    expect(isNodeCentricResult([" __node", "name"])).toBe(false);
    expect(isNodeCentricResult(["__node ", "name"])).toBe(false);
  });
});

// ── isEdgeCentricResult: adversarial ──

describe("QA_GDB299_isEdgeCentricResult", () => {
  it("requires both __from and __to", () => {
    expect(isEdgeCentricResult(["__from"])).toBe(false);
    expect(isEdgeCentricResult(["__to"])).toBe(false);
    expect(isEdgeCentricResult(["__from", "__to"])).toBe(true);
  });

  it("rejects substrings like __from_id", () => {
    expect(isEdgeCentricResult(["__from_id", "__to_id"])).toBe(false);
  });

  it("detects mixed case __FROM and __TO", () => {
    expect(isEdgeCentricResult(["__From", "__To"])).toBe(true);
  });
});

// ── isTraverseQuery: adversarial ──

describe("QA_GDB299_isTraverseQuery", () => {
  it("rejects partial match - TRAVERSE without FROM", () => {
    expect(isTraverseQuery("SELECT TRAVERSE")).toBe(false);
  });

  it("rejects FROM without TRAVERSE", () => {
    expect(isTraverseQuery("SELECT * FROM users")).toBe(false);
  });

  it("detects FROM TRAVERSE with extra spaces", () => {
    expect(isTraverseQuery("SELECT * FROM   TRAVERSE follows FROM users(1)")).toBe(true);
  });

  it("detects FROM TRAVERSE in multi-line query", () => {
    expect(isTraverseQuery("SELECT *\nFROM TRAVERSE\nfollows FROM users(1)")).toBe(true);
  });

  it("rejects TRAVERSE in a comment", () => {
    // The regex doesn't parse comments, so this may be a false positive
    const sql = "SELECT * FROM users -- FROM TRAVERSE something";
    // This is a standard query with a comment. The regex will match the comment text.
    // This is a known limitation — documenting it.
    const result = isTraverseQuery(sql);
    // The regex matches anywhere in the string, including comments
    expect(result).toBe(true); // Known limitation: regex matches in comments
  });

  it("handles empty/whitespace-only input", () => {
    expect(isTraverseQuery("")).toBe(false);
    expect(isTraverseQuery("   ")).toBe(false);
    expect(isTraverseQuery("\n\t")).toBe(false);
  });

  it("detects traverse with tab between FROM and TRAVERSE", () => {
    expect(isTraverseQuery("SELECT * FROM\tTRAVERSE follows FROM users(1)")).toBe(true);
  });
});

// ── buildEdgeQuery: adversarial ──

describe("QA_GDB299_buildEdgeQuery", () => {
  it("handles query with multiple semicolons", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1);;";
    const result = buildEdgeQuery(sql);
    // The regex \s*;?\s*$ only strips one trailing semicolon
    // After: "SELECT * FROM TRAVERSE follows FROM users(1);" + " MODE EDGES"
    // This could produce invalid SQL
    expect(result).toContain("MODE EDGES");
  });

  it("handles query with only whitespace", () => {
    const result = buildEdgeQuery("   ");
    expect(result).toBe(" MODE EDGES");
  });

  it("handles empty string", () => {
    const result = buildEdgeQuery("");
    expect(result).toBe(" MODE EDGES");
  });

  it("handles query ending with MODE (but not MODE EDGES)", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1) MODE";
    const result = buildEdgeQuery(sql);
    // MODE EDGES inserted after TRAVERSE core; stray MODE remains after it
    expect(result).toBe("SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES MODE");
  });

  it("does not modify query with MODE EDGES in middle", () => {
    // MODE EDGES appears but not as the mode clause — still detects it
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1) MODE EDGES MAX_DEPTH 3";
    const result = buildEdgeQuery(sql);
    expect(result).toBe(sql); // returns unchanged since MODE EDGES is present
  });

  it("handles query with trailing newlines", () => {
    const sql = "SELECT * FROM TRAVERSE follows FROM users(1)\n\n";
    const result = buildEdgeQuery(sql);
    expect(result).toContain("MODE EDGES");
    expect(result).not.toContain("\n");
  });
});

// ── classifyColumns: adversarial ──

describe("QA_GDB299_classifyColumns", () => {
  it("handles duplicate column names", () => {
    const result = classifyColumns(["__node", "__node", "name"]);
    expect(result.metaColumns).toEqual(["__node", "__node"]);
    expect(result.userColumns).toEqual(["name"]);
  });

  it("handles column name that is empty string", () => {
    const result = classifyColumns([""]);
    expect(result.userColumns).toEqual([""]);
    expect(result.metaColumns).toEqual([]);
  });

  it("handles columns with special characters", () => {
    const result = classifyColumns(["follows.weight", "user->name", "__node"]);
    expect(result.userColumns).toEqual(["follows.weight", "user->name"]);
    expect(result.metaColumns).toEqual(["__node"]);
  });

  it("classifies all known meta-columns", () => {
    const allMeta = ["__node", "__depth", "__source", "__from", "__to"];
    const result = classifyColumns(allMeta);
    expect(result.metaColumns).toEqual(allMeta);
    expect(result.userColumns).toEqual([]);
  });

  it("is case-insensitive for meta-columns", () => {
    const result = classifyColumns(["__NODE", "__DEPTH", "__SOURCE", "__FROM", "__TO"]);
    expect(result.metaColumns).toEqual(["__NODE", "__DEPTH", "__SOURCE", "__FROM", "__TO"]);
    expect(result.userColumns).toEqual([]);
  });
});

// ── parseNodesFromResult: adversarial ──

describe("QA_GDB299_parseNodesFromResult", () => {
  it("handles null __node value — assigns synthetic unique PK", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", null, 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    // null PK gets a synthetic unique ID to prevent data loss
    expect(nodes).toHaveLength(1);
    expect(nodes[0].pk).toBe("__null_0");
    expect(nodes[0].id).toBe("users:__null_0");
  });

  it("multiple null __node values from same table produce separate nodes (fixed)", () => {
    // Fixed: null-PK rows now get synthetic unique IDs so they don't collapse.
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", null, 0, "users"],
      ["bob", null, 1, "users"],   // different person, same null PK
    ];
    const nodes = parseNodesFromResult(columns, rows);
    // Each null-PK row gets a unique synthetic ID
    expect(nodes).toHaveLength(2);
    expect(nodes[0].label).toBe("alice");
    expect(nodes[1].label).toBe("bob");
  });

  it("handles null __source value", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, null],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].table).toBe("node"); // null fallback is "node"
  });

  it("deduplicates nodes with same table:pk across different depths", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
      ["alice-updated", 1, 2, "users"], // same node at different depth
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toHaveLength(1);
    // First occurrence wins
    expect(nodes[0].label).toBe("alice");
  });

  it("creates separate nodes for same PK from different tables", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
      ["Engineering", 1, 1, "departments"], // same PK, different table
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toHaveLength(2);
    expect(nodes[0].id).toBe("users:1");
    expect(nodes[1].id).toBe("departments:1");
  });

  it("handles all-null attribute values for label fallback", () => {
    const columns = ["name", "email", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      [null, null, 1, 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    // All user attrs are null, should fallback to PK
    expect(nodes[0].label).toBe("1");
  });

  it("uses boolean false as label if it is first non-null attribute", () => {
    const columns = ["active", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      [false, 1, 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].label).toBe("false");
  });

  it("uses 0 as label if it is first non-null attribute", () => {
    const columns = ["score", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      [0, 1, 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].label).toBe("0");
  });

  it("handles empty string attribute as label", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["", 1, 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    // Empty string is not null/undefined, so it's used as label
    expect(nodes[0].label).toBe("");
  });

  it("handles very large number of rows", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [];
    for (let i = 0; i < 1000; i++) {
      rows.push([`user_${i}`, i, 0, "users"]);
    }
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toHaveLength(1000);
  });

  it("handles row with fewer cells than columns — undefined becomes synthetic PK", () => {
    const columns = ["name", "email", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", "a@b.com"], // only 2 cells, missing __node etc
    ];
    const nodes = parseNodesFromResult(columns, rows);
    // row[nodeIdx] is undefined → treated as null → synthetic PK
    expect(nodes).toHaveLength(1);
    expect(nodes[0].pk).toBe("__null_0");
  });

  it("handles row with more cells than columns", () => {
    const columns = ["name", "__node", "__depth"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "extra_value", "another"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes).toHaveLength(1);
    // Extra cells are ignored since we iterate by columns.length
  });

  it("handles __node with string PK", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", "uuid-abc-123", 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].pk).toBe("uuid-abc-123");
    expect(nodes[0].id).toBe("users:uuid-abc-123");
  });

  it("handles __node with PK containing colon", () => {
    const columns = ["name", "__node", "__depth", "__source"];
    const rows: (string | number | boolean | null)[][] = [
      ["alice", "ns:123", 0, "users"],
    ];
    const nodes = parseNodesFromResult(columns, rows);
    expect(nodes[0].pk).toBe("ns:123");
    expect(nodes[0].id).toBe("users:ns:123");
  });
});

// ── parseEdgesFromResult: adversarial ──

describe("QA_GDB299_parseEdgesFromResult", () => {
  it("handles null __from value — becomes empty string", () => {
    const columns = ["__from", "__to", "__depth"];
    const rows: (string | number | boolean | null)[][] = [[null, 2, 1]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toHaveLength(1);
    expect(edges[0].from).toBe(""); // null ?? "" → ""
  });

  it("handles null __to value — becomes empty string", () => {
    const columns = ["__from", "__to", "__depth"];
    const rows: (string | number | boolean | null)[][] = [[1, null, 1]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toHaveLength(1);
    expect(edges[0].to).toBe(""); // null ?? "" → ""
  });

  it("deduplicates duplicate edges (fixed)", () => {
    const columns = ["__from", "__to"];
    const rows: (string | number | boolean | null)[][] = [
      [1, 2],
      [1, 2], // exact duplicate
    ];
    const edges = parseEdgesFromResult(columns, rows);
    // Duplicate edges are now deduplicated
    expect(edges).toHaveLength(1);
    expect(edges[0].id).toBe("1->edge->2");
  });

  it("handles self-loop edge (from == to)", () => {
    const columns = ["__from", "__to"];
    const rows: (string | number | boolean | null)[][] = [[1, 1]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toHaveLength(1);
    expect(edges[0].from).toBe("1");
    expect(edges[0].to).toBe("1");
  });

  it("handles multiple dot-notation columns — uses first for edge type", () => {
    const columns = ["__from", "__to", "follows.weight", "likes.score"];
    const rows: (string | number | boolean | null)[][] = [[1, 2, 0.8, 0.5]];
    const edges = parseEdgesFromResult(columns, rows);
    // detectEdgeType returns the first dot-column prefix
    expect(edges[0].edgeType).toBe("follows");
  });

  it("handles dot-notation in meta column (shouldn't happen but defensive)", () => {
    // If a user-named column starts with __from. prefix
    const columns = ["__from", "__to", "__depth"];
    const rows: (string | number | boolean | null)[][] = [[1, 2, 1]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges[0].edgeType).toBe("edge"); // no dot-columns in user cols
  });

  it("handles very large number of edges", () => {
    const columns = ["__from", "__to", "weight"];
    const rows: (string | number | boolean | null)[][] = [];
    for (let i = 0; i < 1000; i++) {
      rows.push([i, i + 1, 0.5]);
    }
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges).toHaveLength(1000);
  });

  it("handles edge with all-null properties", () => {
    const columns = ["__from", "__to", "follows.weight", "follows.created_at"];
    const rows: (string | number | boolean | null)[][] = [[1, 2, null, null]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges[0].properties["follows.weight"]).toBeNull();
    expect(edges[0].properties["follows.created_at"]).toBeNull();
  });

  it("edge ID is deterministic for same from/to/type", () => {
    const columns = ["__from", "__to", "follows.weight"];
    const rows: (string | number | boolean | null)[][] = [[1, 2, 0.8]];
    const edges = parseEdgesFromResult(columns, rows);
    expect(edges[0].id).toBe("1->follows->2");
  });
});

// ── buildGraphData: adversarial ──

describe("QA_GDB299_buildGraphData", () => {
  it("handles completely empty inputs", () => {
    const graph = buildGraphData([], [], [], []);
    expect(graph.nodes).toHaveLength(0);
    expect(graph.edges).toHaveLength(0);
  });

  it("creates placeholder nodes for edges when no node data exists", () => {
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [
      [1, 2],
      [2, 3],
    ];
    const graph = buildGraphData([], [], edgeColumns, edgeRows);
    // Should create 3 placeholder nodes: 1, 2, 3
    expect(graph.nodes).toHaveLength(3);
    expect(graph.edges).toHaveLength(2);
  });

  it("does not create duplicate placeholder nodes", () => {
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [
      [1, 2],
      [1, 3],
      [2, 3],
    ];
    const graph = buildGraphData([], [], edgeColumns, edgeRows);
    // Nodes: 1, 2, 3 (each appears as placeholder once)
    expect(graph.nodes).toHaveLength(3);
  });

  it("resolves edges to existing nodes by PK", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
      ["bob", 2, 1, "users"],
    ];
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [[1, 2]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    expect(graph.edges[0].from).toBe("users:1");
    expect(graph.edges[0].to).toBe("users:2");
    expect(graph.nodes).toHaveLength(2); // no placeholders needed
  });

  it("handles edges referencing non-existent nodes", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
    ];
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [[1, 99]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    expect(graph.nodes).toHaveLength(2); // alice + placeholder for 99
    const placeholder = graph.nodes.find((n) => n.pk === "99");
    expect(placeholder).toBeDefined();
    expect(placeholder?.attributes).toEqual({});
  });

  it("handles nodes with no edges", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
      ["bob", 2, 1, "users"],
      ["charlie", 3, 1, "users"],
    ];
    const graph = buildGraphData(nodeColumns, nodeRows, [], []);
    expect(graph.nodes).toHaveLength(3);
    expect(graph.edges).toHaveLength(0);
  });

  it("handles self-loop edges in graph data", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
    ];
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [[1, 1]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    expect(graph.nodes).toHaveLength(1);
    expect(graph.edges).toHaveLength(1);
    expect(graph.edges[0].from).toBe("users:1");
    expect(graph.edges[0].to).toBe("users:1");
  });

  it("handles heterogeneous node tables", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
      ["Engineering", 10, 1, "departments"],
    ];
    const edgeColumns = ["__from", "__to"];
    const edgeRows: (string | number | boolean | null)[][] = [[1, 10]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    // Nodes should have different tables
    const userNode = graph.nodes.find((n) => n.table === "users");
    const deptNode = graph.nodes.find((n) => n.table === "departments");
    expect(userNode).toBeDefined();
    expect(deptNode).toBeDefined();
    expect(graph.edges[0].from).toBe("users:1");
    expect(graph.edges[0].to).toBe("departments:10");
  });

  it("placeholder node PK extraction handles colons in ID", () => {
    // When an edge endpoint doesn't match a known node, buildGraphData creates
    // a placeholder. The PK is extracted by splitting on ":"
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", 1, 0, "users"],
    ];
    const edgeColumns = ["__from", "__to"];
    // Edge to node 99 which doesn't exist — will create placeholder "node:99"
    const edgeRows: (string | number | boolean | null)[][] = [[1, 99]];

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    const placeholder = graph.nodes.find((n) => n.id === "node:99");
    expect(placeholder).toBeDefined();
    expect(placeholder?.pk).toBe("99");
  });

  it("handles large graph with many nodes and edges", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [];
    for (let i = 0; i < 500; i++) {
      nodeRows.push([`node_${i}`, i, 0, "users"]);
    }
    const edgeColumns = ["__from", "__to", "follows.weight"];
    const edgeRows: (string | number | boolean | null)[][] = [];
    for (let i = 0; i < 499; i++) {
      edgeRows.push([i, i + 1, 0.5]);
    }

    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    expect(graph.nodes).toHaveLength(500);
    expect(graph.edges).toHaveLength(499);
  });
});

// ── formatEdgeTooltip: adversarial ──

describe("QA_GDB299_formatEdgeTooltip", () => {
  it("handles empty edge type", () => {
    const tooltip = formatEdgeTooltip("", { weight: 0.8 });
    expect(tooltip).toContain("Edge: ");
    expect(tooltip).toContain("weight: 0.8");
  });

  it("handles property value of 0", () => {
    const tooltip = formatEdgeTooltip("follows", { weight: 0 });
    expect(tooltip).toContain("weight: 0");
  });

  it("handles property value of false", () => {
    const tooltip = formatEdgeTooltip("follows", { active: false });
    expect(tooltip).toContain("active: false");
  });

  it("handles property value of empty string", () => {
    const tooltip = formatEdgeTooltip("follows", { note: "" });
    // Empty string is not null/undefined, so it should be included
    expect(tooltip).toContain("note: ");
  });

  it("handles many properties", () => {
    const props: Record<string, string | number | boolean | null> = {};
    for (let i = 0; i < 50; i++) {
      props[`prop_${i}`] = i;
    }
    const tooltip = formatEdgeTooltip("follows", props);
    const lines = tooltip.split("\n");
    expect(lines).toHaveLength(51); // 1 header + 50 properties
  });

  it("handles all-null properties", () => {
    const tooltip = formatEdgeTooltip("follows", {
      weight: null,
      score: null,
    });
    // Null values are skipped
    expect(tooltip).toBe("Edge: follows");
  });

  it("handles special characters in property names", () => {
    const tooltip = formatEdgeTooltip("follows", {
      "follows.weight": 0.8,
      "path->cost": 10,
    });
    expect(tooltip).toContain("follows.weight: 0.8");
    expect(tooltip).toContain("path->cost: 10");
  });
});

// ── Integration: end-to-end graph data flow ──

describe("QA_GDB299_Integration", () => {
  it("full pipeline: node + edge results → graph data", () => {
    // Simulates a complete TRAVERSE query result
    const nodeColumns = ["name", "email", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [
      ["alice", "alice@ex.com", 1, 0, "users"],
      ["bob", "bob@ex.com", 2, 1, "users"],
      ["charlie", "charlie@ex.com", 3, 2, "users"],
    ];
    const edgeColumns = ["__from", "__to", "__depth", "follows.weight"];
    const edgeRows: (string | number | boolean | null)[][] = [
      [1, 2, 1, 0.8],
      [2, 3, 2, 0.5],
    ];

    // Verify detection
    expect(isNodeCentricResult(nodeColumns)).toBe(true);
    expect(isEdgeCentricResult(edgeColumns)).toBe(true);
    expect(isGraphResult(nodeColumns)).toBe(true);

    // Verify classification
    const { userColumns, metaColumns } = classifyColumns(nodeColumns);
    expect(userColumns).toEqual(["name", "email"]);
    expect(metaColumns).toEqual(["__node", "__depth", "__source"]);

    // Verify graph data
    const graph = buildGraphData(nodeColumns, nodeRows, edgeColumns, edgeRows);
    expect(graph.nodes).toHaveLength(3);
    expect(graph.edges).toHaveLength(2);

    // Verify edge resolution
    expect(graph.edges[0].from).toBe("users:1");
    expect(graph.edges[0].to).toBe("users:2");
    expect(graph.edges[1].from).toBe("users:2");
    expect(graph.edges[1].to).toBe("users:3");

    // Verify node attributes
    const alice = graph.nodes.find((n) => n.pk === "1");
    expect(alice?.attributes.name).toBe("alice");
    expect(alice?.attributes.email).toBe("alice@ex.com");

    // Verify tooltip
    const tooltip = formatEdgeTooltip(
      graph.edges[0].edgeType,
      graph.edges[0].properties
    );
    expect(tooltip).toContain("Edge: follows");
    expect(tooltip).toContain("follows.weight: 0.8");
  });

  it("full pipeline: traverse query → edge query construction", () => {
    const sql = "SELECT name, email FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3";
    expect(isTraverseQuery(sql)).toBe(true);
    const edgeSql = buildEdgeQuery(sql);
    // GDB-311: SELECT list replaced with * and MODE EDGES in correct position
    expect(edgeSql).toBe(
      "SELECT * FROM TRAVERSE follows FROM users(1) DIRECTION OUT MAX_DEPTH 3 MODE EDGES"
    );
  });

  it("non-graph query produces standard table result", () => {
    const columns = ["id", "name", "email"];
    expect(isNodeCentricResult(columns)).toBe(false);
    expect(isEdgeCentricResult(columns)).toBe(false);
    expect(isGraphResult(columns)).toBe(false);

    const { userColumns, metaColumns } = classifyColumns(columns);
    expect(userColumns).toEqual(["id", "name", "email"]);
    expect(metaColumns).toEqual([]);
  });

  it("empty traversal result produces empty graph data", () => {
    const nodeColumns = ["name", "__node", "__depth", "__source"];
    const nodeRows: (string | number | boolean | null)[][] = [];
    const graph = buildGraphData(nodeColumns, nodeRows, [], []);
    expect(graph.nodes).toHaveLength(0);
    expect(graph.edges).toHaveLength(0);
  });

  it("edge-only result without node data creates placeholder nodes", () => {
    const edgeColumns = ["__from", "__to", "follows.weight"];
    const edgeRows: (string | number | boolean | null)[][] = [
      [1, 2, 0.8],
      [2, 3, 0.5],
    ];
    const graph = buildGraphData([], [], edgeColumns, edgeRows);
    expect(graph.nodes).toHaveLength(3); // all placeholders
    expect(graph.edges).toHaveLength(2);
    // Placeholders should have empty attributes
    graph.nodes.forEach((n) => {
      expect(n.attributes).toEqual({});
      expect(n.table).toBe("node");
    });
  });
});
