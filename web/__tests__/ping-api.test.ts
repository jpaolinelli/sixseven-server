import { describe, it, expect, vi, beforeEach } from "vitest";

// Mock the db module before importing the route
vi.mock("@/lib/db", () => ({
  ping: vi.fn(),
}));

import { POST } from "@/app/api/ping/route";
import { ping } from "@/lib/db";
import { NextRequest } from "next/server";

const mockedPing = vi.mocked(ping);

function makeRequest(body: Record<string, unknown>): NextRequest {
  return new NextRequest("http://localhost:3000/api/ping", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

beforeEach(() => {
  vi.clearAllMocks();
});

describe("POST /api/ping", () => {
  it("returns ok: true when server is reachable", async () => {
    mockedPing.mockResolvedValueOnce(true);

    const res = await POST(makeRequest({}));
    const body = await res.json();
    expect(body.ok).toBe(true);
  });

  it("returns ok: false when server is unreachable", async () => {
    mockedPing.mockResolvedValueOnce(false);

    const res = await POST(makeRequest({}));
    const body = await res.json();
    expect(body.ok).toBe(false);
  });

  it("passes connection params to ping", async () => {
    mockedPing.mockResolvedValueOnce(true);

    await POST(
      makeRequest({
        connection: { host: "prod.example.com", port: 5432, user: "admin" },
      })
    );

    expect(mockedPing).toHaveBeenCalledWith({
      host: "prod.example.com",
      port: 5432,
      user: "admin",
    });
  });

  it("calls ping with undefined when no connection params", async () => {
    mockedPing.mockResolvedValueOnce(true);

    await POST(makeRequest({}));

    expect(mockedPing).toHaveBeenCalledWith(undefined);
  });

  it("returns ok: false on exception", async () => {
    mockedPing.mockRejectedValueOnce(new Error("Network error"));

    const res = await POST(makeRequest({}));
    const body = await res.json();
    expect(body.ok).toBe(false);
  });
});
