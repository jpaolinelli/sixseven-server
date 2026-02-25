import { describe, it, expect } from "vitest";
import { compareValues } from "@/components/QueryResults";

describe("compareValues", () => {
  describe("null handling", () => {
    it("returns 0 for two nulls", () => {
      expect(compareValues(null, null, "asc")).toBe(0);
      expect(compareValues(null, null, "desc")).toBe(0);
    });

    it("sorts null before non-null in ascending", () => {
      expect(compareValues(null, "a", "asc")).toBeLessThan(0);
      expect(compareValues("a", null, "asc")).toBeGreaterThan(0);
    });

    it("sorts null after non-null in descending", () => {
      expect(compareValues(null, "a", "desc")).toBeGreaterThan(0);
      expect(compareValues("a", null, "desc")).toBeLessThan(0);
    });
  });

  describe("number comparison", () => {
    it("sorts numbers ascending", () => {
      expect(compareValues(1, 2, "asc")).toBeLessThan(0);
      expect(compareValues(2, 1, "asc")).toBeGreaterThan(0);
    });

    it("sorts numbers descending", () => {
      expect(compareValues(1, 2, "desc")).toBeGreaterThan(0);
      expect(compareValues(2, 1, "desc")).toBeLessThan(0);
    });

    it("returns 0 for equal numbers", () => {
      expect(compareValues(5, 5, "asc")).toBe(0);
    });

    it("handles negative numbers", () => {
      expect(compareValues(-1, 1, "asc")).toBeLessThan(0);
    });

    it("handles decimals", () => {
      expect(compareValues(1.1, 1.2, "asc")).toBeLessThan(0);
    });
  });

  describe("numeric string comparison", () => {
    it("compares numeric strings as numbers", () => {
      expect(compareValues("10", "2", "asc")).toBeGreaterThan(0);
      expect(compareValues("2", "10", "asc")).toBeLessThan(0);
    });

    it("handles decimal numeric strings", () => {
      expect(compareValues("1.5", "1.25", "asc")).toBeGreaterThan(0);
    });

    it("does not treat empty string as numeric", () => {
      // empty strings should be compared as strings, not as 0
      expect(compareValues("", "a", "asc")).toBeLessThan(0);
    });
  });

  describe("string comparison", () => {
    it("sorts strings alphabetically ascending", () => {
      expect(compareValues("apple", "banana", "asc")).toBeLessThan(0);
      expect(compareValues("banana", "apple", "asc")).toBeGreaterThan(0);
    });

    it("sorts strings alphabetically descending", () => {
      expect(compareValues("apple", "banana", "desc")).toBeGreaterThan(0);
    });

    it("returns 0 for equal strings", () => {
      expect(compareValues("abc", "abc", "asc")).toBe(0);
    });
  });

  describe("boolean comparison", () => {
    it("compares booleans as strings", () => {
      // false < true alphabetically
      expect(compareValues(false, true, "asc")).toBeLessThan(0);
      expect(compareValues(true, false, "asc")).toBeGreaterThan(0);
    });
  });

  describe("mixed type comparison", () => {
    it("compares number and numeric string", () => {
      // Number(42) and Number("42") both numeric
      expect(compareValues(42, "42", "asc")).toBe(0);
    });

    it("compares number and non-numeric string as strings", () => {
      // String(42) = "42" compared to "abc" as strings
      const result = compareValues(42, "abc", "asc");
      expect(typeof result).toBe("number");
    });
  });
});
