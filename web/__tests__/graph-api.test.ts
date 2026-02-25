import { describe, it, expect, vi, beforeEach } from "vitest";

// Mock the db module before importing the route
vi.mock("@/lib/db", () => ({
  query: vi.fn(),
}));

import { POST } from "@/app/api/graph/route";
import { query } from "@/lib/db";
import { NextRequest } from "next/server";

const mockedQuery = vi.mocked(query);

function makeRequest(body: Record<string, unknown>): NextRequest {
  return new NextRequest("http://localhost:3000/api/graph", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

async function parseResponse(res: Response) {
  return res.json();
}

beforeEach(() => {
  vi.clearAllMocks();
});

describe("POST /api/graph", () => {
  describe("validation", () => {
    it("returns 400 when action is missing", async () => {
      const res = await POST(makeRequest({ database: "test" }));
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("action");
    });

    it("returns 400 when database is missing", async () => {
      const res = await POST(makeRequest({ action: "traverse" }));
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("database");
    });

    it("returns 400 for unknown action", async () => {
      const res = await POST(
        makeRequest({ action: "unknown", database: "test" })
      );
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("Unknown action");
    });
  });

  describe("traverse action", () => {
    it("returns 400 when table is missing", async () => {
      const res = await POST(
        makeRequest({ action: "traverse", database: "test", id: "1" })
      );
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("table");
    });

    it("executes TRAVERSE BOTH query by default", async () => {
      mockedQuery.mockResolvedValueOnce({
        columns: ["source_table", "source_id", "edge_type", "target_table", "target_id"],
        rows: [["users", "1", "follows", "users", "2"]],
      });

      const res = await POST(
        makeRequest({
          action: "traverse",
          database: "social",
          table: "users",
          id: "1",
        })
      );
      expect(res.status).toBe(200);
      expect(mockedQuery).toHaveBeenCalledWith(
        expect.stringContaining("TRAVERSE BOTH"),
        "social",
        undefined
      );
    });

    it("respects direction parameter", async () => {
      mockedQuery.mockResolvedValueOnce({ columns: [], rows: [] });

      await POST(
        makeRequest({
          action: "traverse",
          database: "social",
          table: "users",
          id: "1",
          direction: "out",
        })
      );
      expect(mockedQuery).toHaveBeenCalledWith(
        expect.stringContaining("TRAVERSE OUT"),
        "social",
        undefined
      );
    });

    it("includes edge type when specified", async () => {
      mockedQuery.mockResolvedValueOnce({ columns: [], rows: [] });

      await POST(
        makeRequest({
          action: "traverse",
          database: "social",
          table: "users",
          id: "1",
          edgeType: "follows",
        })
      );
      expect(mockedQuery).toHaveBeenCalledWith(
        expect.stringContaining('EDGE "follows"'),
        "social",
        undefined
      );
    });

    it("quotes table name to prevent SQL injection", async () => {
      mockedQuery.mockResolvedValueOnce({ columns: [], rows: [] });

      await POST(
        makeRequest({
          action: "traverse",
          database: "test",
          table: 'users"; DROP TABLE--',
          id: "1",
        })
      );
      const sql = mockedQuery.mock.calls[0][0];
      expect(sql).toContain('"users""; DROP TABLE--"');
    });

    it("handles numeric IDs without quotes", async () => {
      mockedQuery.mockResolvedValueOnce({ columns: [], rows: [] });

      await POST(
        makeRequest({
          action: "traverse",
          database: "test",
          table: "users",
          id: "42",
        })
      );
      const sql = mockedQuery.mock.calls[0][0];
      expect(sql).toContain("id = 42");
    });

    it("quotes string IDs", async () => {
      mockedQuery.mockResolvedValueOnce({ columns: [], rows: [] });

      await POST(
        makeRequest({
          action: "traverse",
          database: "test",
          table: "users",
          id: "alice",
        })
      );
      const sql = mockedQuery.mock.calls[0][0];
      expect(sql).toContain("id = 'alice'");
    });
  });

  describe("shortest_path action", () => {
    it("returns 400 when source/target info is missing", async () => {
      const res = await POST(
        makeRequest({
          action: "shortest_path",
          database: "test",
          sourceTable: "users",
        })
      );
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("source/target");
    });

    it("executes SHORTEST PATH query", async () => {
      mockedQuery.mockResolvedValueOnce({
        columns: ["source_table", "source_id", "edge_type", "target_table", "target_id"],
        rows: [
          ["users", "1", "follows", "users", "3"],
          ["users", "3", "follows", "users", "5"],
        ],
      });

      const res = await POST(
        makeRequest({
          action: "shortest_path",
          database: "social",
          sourceTable: "users",
          sourceId: "1",
          targetTable: "users",
          targetId: "5",
        })
      );
      expect(res.status).toBe(200);
      expect(mockedQuery).toHaveBeenCalledWith(
        expect.stringContaining("SHORTEST PATH"),
        "social",
        undefined
      );
    });
  });

  describe("node_details action", () => {
    it("returns 400 when table or id is missing", async () => {
      const res = await POST(
        makeRequest({
          action: "node_details",
          database: "test",
          table: "users",
        })
      );
      expect(res.status).toBe(400);
      const body = await parseResponse(res);
      expect(body.error).toContain("table");
    });

    it("executes SELECT query for node details", async () => {
      mockedQuery.mockResolvedValueOnce({
        columns: ["id", "name", "email"],
        rows: [["1", "Alice", "alice@example.com"]],
      });

      const res = await POST(
        makeRequest({
          action: "node_details",
          database: "social",
          table: "users",
          id: "1",
        })
      );
      expect(res.status).toBe(200);
      const body = await parseResponse(res);
      expect(body.columns).toEqual(["id", "name", "email"]);
      expect(body.rows).toHaveLength(1);
    });
  });

  describe("error handling", () => {
    it("returns 500 when database query fails", async () => {
      mockedQuery.mockRejectedValueOnce(new Error("Connection refused"));

      const res = await POST(
        makeRequest({
          action: "traverse",
          database: "social",
          table: "users",
          id: "1",
        })
      );
      expect(res.status).toBe(500);
      const body = await parseResponse(res);
      expect(body.error).toContain("Connection refused");
    });
  });
});
