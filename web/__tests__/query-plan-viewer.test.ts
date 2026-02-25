import { describe, it, expect } from "vitest";
import { tryParseExplainPlan } from "@/components/QueryPlanViewer";

describe("tryParseExplainPlan", () => {
  it("returns null for multi-column results", () => {
    expect(tryParseExplainPlan(["a", "b"], [["x", "y"]])).toBeNull();
  });

  it("returns null for multi-row results", () => {
    expect(
      tryParseExplainPlan(["QUERY PLAN"], [["row1"], ["row2"]])
    ).toBeNull();
  });

  it("returns null when column name is not QUERY PLAN or EXPLAIN", () => {
    expect(
      tryParseExplainPlan(["result"], [['{"Node Type": "Seq Scan"}']])
    ).toBeNull();
  });

  it("returns null for non-string cell value", () => {
    expect(tryParseExplainPlan(["QUERY PLAN"], [[42]])).toBeNull();
  });

  it("returns null for invalid JSON", () => {
    expect(
      tryParseExplainPlan(["QUERY PLAN"], [["not json"]])
    ).toBeNull();
  });

  it("returns null for valid JSON that is not an EXPLAIN plan", () => {
    expect(
      tryParseExplainPlan(["QUERY PLAN"], [['{"foo": "bar"}']])
    ).toBeNull();
  });

  it("parses PostgreSQL EXPLAIN (FORMAT JSON) array shape", () => {
    const plan = {
      "Node Type": "Seq Scan",
      "Relation Name": "users",
      "Total Cost": 10.5,
      "Plan Rows": 100,
    };
    const json = JSON.stringify([{ Plan: plan }]);
    const result = tryParseExplainPlan(["QUERY PLAN"], [[json]]);
    expect(result).not.toBeNull();
    expect(result!["Node Type"]).toBe("Seq Scan");
    expect(result!["Relation Name"]).toBe("users");
    expect(result!["Total Cost"]).toBe(10.5);
    expect(result!["Plan Rows"]).toBe(100);
  });

  it("parses object with Plan key", () => {
    const plan = { "Node Type": "Index Scan", "Total Cost": 5.0 };
    const json = JSON.stringify({ Plan: plan });
    const result = tryParseExplainPlan(["QUERY PLAN"], [[json]]);
    expect(result).not.toBeNull();
    expect(result!["Node Type"]).toBe("Index Scan");
  });

  it("parses direct Node Type object", () => {
    const plan = { "Node Type": "Hash Join", "Total Cost": 20.0 };
    const json = JSON.stringify(plan);
    const result = tryParseExplainPlan(["QUERY PLAN"], [[json]]);
    expect(result).not.toBeNull();
    expect(result!["Node Type"]).toBe("Hash Join");
  });

  it("is case-insensitive for column name", () => {
    const plan = { "Node Type": "Seq Scan" };
    const json = JSON.stringify([{ Plan: plan }]);
    expect(
      tryParseExplainPlan(["query plan"], [[json]])
    ).not.toBeNull();
    expect(
      tryParseExplainPlan(["Query Plan"], [[json]])
    ).not.toBeNull();
    expect(
      tryParseExplainPlan(["explain"], [[json]])
    ).not.toBeNull();
    expect(
      tryParseExplainPlan(["EXPLAIN"], [[json]])
    ).not.toBeNull();
  });

  it("parses nested plan with children", () => {
    const plan = {
      "Node Type": "Hash Join",
      "Total Cost": 50.0,
      Plans: [
        { "Node Type": "Seq Scan", "Relation Name": "a", "Total Cost": 10.0 },
        { "Node Type": "Hash", "Total Cost": 15.0, Plans: [
          { "Node Type": "Seq Scan", "Relation Name": "b", "Total Cost": 5.0 },
        ]},
      ],
    };
    const json = JSON.stringify([{ Plan: plan }]);
    const result = tryParseExplainPlan(["QUERY PLAN"], [[json]]);
    expect(result).not.toBeNull();
    expect(result!.Plans).toHaveLength(2);
    expect(result!.Plans![1].Plans).toHaveLength(1);
  });

  it("returns null for empty array JSON", () => {
    expect(
      tryParseExplainPlan(["QUERY PLAN"], [["[]"]])
    ).toBeNull();
  });
});
