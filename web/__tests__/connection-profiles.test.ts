import { describe, it, expect, beforeEach, vi } from "vitest";
import {
  loadProfiles,
  loadActiveProfileId,
  saveActiveProfileId,
  addProfile,
  updateProfile,
  deleteProfile,
} from "@/lib/connection-profiles";

// Mock localStorage
const store = new Map<string, string>();

beforeEach(() => {
  store.clear();
  vi.stubGlobal("window", {});
  vi.stubGlobal("localStorage", {
    getItem: (key: string) => store.get(key) ?? null,
    setItem: (key: string, value: string) => store.set(key, value),
    removeItem: (key: string) => store.delete(key),
  });
});

describe("connection-profiles", () => {
  describe("loadProfiles", () => {
    it("returns default profile when localStorage is empty", () => {
      const profiles = loadProfiles();
      expect(profiles).toHaveLength(1);
      expect(profiles[0]).toMatchObject({
        id: "default",
        name: "Local",
        host: "localhost",
        port: 6767,
        user: "sixseven",
        isDefault: true,
      });
    });

    it("returns saved profiles from localStorage", () => {
      const saved = [
        { id: "1", name: "Prod", host: "prod.example.com", port: 5432, user: "admin" },
      ];
      store.set("sixseven-server-profiles", JSON.stringify(saved));

      const profiles = loadProfiles();
      expect(profiles).toHaveLength(1);
      expect(profiles[0].name).toBe("Prod");
    });

    it("returns default profile when localStorage has invalid JSON", () => {
      store.set("sixseven-server-profiles", "not-json");
      const profiles = loadProfiles();
      expect(profiles).toHaveLength(1);
      expect(profiles[0].id).toBe("default");
    });

    it("returns default profile when localStorage has empty array", () => {
      store.set("sixseven-server-profiles", "[]");
      const profiles = loadProfiles();
      expect(profiles).toHaveLength(1);
      expect(profiles[0].id).toBe("default");
    });
  });

  describe("loadActiveProfileId / saveActiveProfileId", () => {
    it("returns 'default' when no active profile is saved", () => {
      expect(loadActiveProfileId()).toBe("default");
    });

    it("returns saved active profile ID", () => {
      saveActiveProfileId("my-profile");
      expect(loadActiveProfileId()).toBe("my-profile");
    });
  });

  describe("addProfile", () => {
    it("adds a new profile and persists it", () => {
      const profile = addProfile("Staging", "staging.example.com", 5433, "dev");

      expect(profile.name).toBe("Staging");
      expect(profile.host).toBe("staging.example.com");
      expect(profile.port).toBe(5433);
      expect(profile.user).toBe("dev");
      expect(profile.id).toBeTruthy();

      // Verify it's persisted
      const profiles = loadProfiles();
      expect(profiles.length).toBeGreaterThanOrEqual(2);
      expect(profiles.find((p) => p.name === "Staging")).toBeTruthy();
    });
  });

  describe("updateProfile", () => {
    it("updates an existing profile", () => {
      const profile = addProfile("Test", "test.example.com", 6767, "sixseven");

      updateProfile({
        ...profile,
        host: "updated.example.com",
        port: 9999,
      });

      const profiles = loadProfiles();
      const updated = profiles.find((p) => p.id === profile.id);
      expect(updated?.host).toBe("updated.example.com");
      expect(updated?.port).toBe(9999);
    });

    it("does nothing for non-existent profile", () => {
      const before = loadProfiles();
      updateProfile({
        id: "nonexistent",
        name: "Ghost",
        host: "ghost",
        port: 1234,
        user: "ghost",
      });
      const after = loadProfiles();
      expect(after).toEqual(before);
    });
  });

  describe("deleteProfile", () => {
    it("removes a profile by ID", () => {
      const profile = addProfile("ToDelete", "delete.example.com", 6767, "sixseven");
      expect(loadProfiles().find((p) => p.id === profile.id)).toBeTruthy();

      deleteProfile(profile.id);
      expect(loadProfiles().find((p) => p.id === profile.id)).toBeFalsy();
    });

    it("cannot delete the default profile", () => {
      deleteProfile("default");
      const profiles = loadProfiles();
      expect(profiles.find((p) => p.id === "default")).toBeTruthy();
    });
  });
});
