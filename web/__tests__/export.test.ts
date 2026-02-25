import { describe, it, expect } from "vitest";
import { escapeCSV, buildCSV, buildJSON } from "@/lib/export";

describe("escapeCSV", () => {
  it("returns empty string for null", () => {
    expect(escapeCSV(null)).toBe("");
  });

  it("converts numbers to string", () => {
    expect(escapeCSV(42)).toBe("42");
    expect(escapeCSV(3.14)).toBe("3.14");
  });

  it("converts booleans to string", () => {
    expect(escapeCSV(true)).toBe("true");
    expect(escapeCSV(false)).toBe("false");
  });

  it("returns plain string unchanged when no special chars", () => {
    expect(escapeCSV("hello")).toBe("hello");
  });

  it("wraps string containing commas in quotes", () => {
    expect(escapeCSV("a,b")).toBe('"a,b"');
  });

  it("wraps string containing double quotes and escapes them", () => {
    expect(escapeCSV('say "hello"')).toBe('"say ""hello"""');
  });

  it("wraps string containing newlines in quotes", () => {
    expect(escapeCSV("line1\nline2")).toBe('"line1\nline2"');
  });

  it("handles string with all special characters", () => {
    expect(escapeCSV('a,b"c\nd')).toBe('"a,b""c\nd"');
  });

  it("handles empty string", () => {
    expect(escapeCSV("")).toBe("");
  });
});

describe("buildCSV", () => {
  it("builds header-only CSV for empty rows", () => {
    expect(buildCSV(["id", "name"], [])).toBe("id,name\n");
  });

  it("builds CSV with rows", () => {
    const result = buildCSV(["id", "name"], [
      [1, "Alice"],
      [2, "Bob"],
    ]);
    expect(result).toBe("id,name\n1,Alice\n2,Bob");
  });

  it("handles null cells as empty strings", () => {
    const result = buildCSV(["a", "b"], [[null, "x"]]);
    expect(result).toBe("a,b\n,x");
  });

  it("escapes values in rows", () => {
    const result = buildCSV(["msg"], [["hello, world"]]);
    expect(result).toBe('msg\n"hello, world"');
  });

  it("escapes column names with special chars", () => {
    const result = buildCSV(["col,1"], [[1]]);
    expect(result).toBe('"col,1"\n1');
  });

  it("handles boolean values", () => {
    const result = buildCSV(["flag"], [[true], [false]]);
    expect(result).toBe("flag\ntrue\nfalse");
  });
});

describe("buildJSON", () => {
  it("builds empty array for no rows", () => {
    expect(buildJSON(["id"], [])).toBe("[]");
  });

  it("builds array of objects keyed by column names", () => {
    const result = buildJSON(["id", "name"], [
      [1, "Alice"],
      [2, "Bob"],
    ]);
    const parsed = JSON.parse(result);
    expect(parsed).toEqual([
      { id: 1, name: "Alice" },
      { id: 2, name: "Bob" },
    ]);
  });

  it("maps null cells to null in JSON", () => {
    const result = buildJSON(["a", "b"], [[null, "x"]]);
    const parsed = JSON.parse(result);
    expect(parsed).toEqual([{ a: null, b: "x" }]);
  });

  it("maps missing cells to null", () => {
    const result = buildJSON(["a", "b", "c"], [[1]]);
    const parsed = JSON.parse(result);
    expect(parsed).toEqual([{ a: 1, b: null, c: null }]);
  });

  it("preserves boolean and number types", () => {
    const result = buildJSON(["num", "bool"], [[42, true]]);
    const parsed = JSON.parse(result);
    expect(parsed[0].num).toBe(42);
    expect(parsed[0].bool).toBe(true);
  });

  it("produces valid pretty-printed JSON", () => {
    const result = buildJSON(["x"], [[1]]);
    expect(result).toContain("\n");
    expect(() => JSON.parse(result)).not.toThrow();
  });
});
