import { describe, it, expect } from "vitest";
import {
  makeNodeId,
  tableColor,
  parseNodeId,
} from "@/lib/graph-utils";

describe("makeNodeId", () => {
  it("combines table and id with colon separator", () => {
    expect(makeNodeId("users", "42")).toBe("users:42");
  });

  it("handles empty table name", () => {
    expect(makeNodeId("", "1")).toBe(":1");
  });

  it("handles empty id", () => {
    expect(makeNodeId("users", "")).toBe("users:");
  });

  it("preserves special characters in table/id", () => {
    expect(makeNodeId("my-table", "abc-123")).toBe("my-table:abc-123");
  });
});

describe("parseNodeId", () => {
  it("splits on first colon", () => {
    expect(parseNodeId("users:42")).toEqual({ table: "users", pk: "42" });
  });

  it("handles id containing colons", () => {
    expect(parseNodeId("items:a:b:c")).toEqual({ table: "items", pk: "a:b:c" });
  });

  it("handles no colon (fallback)", () => {
    expect(parseNodeId("foobar")).toEqual({ table: "foobar", pk: "" });
  });

  it("handles empty string", () => {
    expect(parseNodeId("")).toEqual({ table: "", pk: "" });
  });

  it("roundtrips with makeNodeId", () => {
    const id = makeNodeId("products", "99");
    const parsed = parseNodeId(id);
    expect(parsed.table).toBe("products");
    expect(parsed.pk).toBe("99");
  });
});

describe("tableColor", () => {
  it("returns a hex color string", () => {
    const color = tableColor("users");
    expect(color).toMatch(/^#[0-9a-f]{6}$/);
  });

  it("returns deterministic color for same table", () => {
    expect(tableColor("orders")).toBe(tableColor("orders"));
  });

  it("returns different colors for different tables", () => {
    // Not guaranteed but very likely for these names
    const colors = new Set([
      tableColor("users"),
      tableColor("products"),
      tableColor("orders"),
      tableColor("reviews"),
      tableColor("categories"),
    ]);
    // Should get at least 2 distinct colors from 5 different names
    expect(colors.size).toBeGreaterThanOrEqual(2);
  });

  it("handles empty string", () => {
    const color = tableColor("");
    expect(color).toMatch(/^#[0-9a-f]{6}$/);
  });
});
