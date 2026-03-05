/**
 * Server profile persistence via localStorage.
 *
 * Stores named server profiles so users can switch between
 * different SixSevenDB server instances.
 */

import type { ServerProfile } from "./connection-types";

const PROFILES_KEY = "sixseven-server-profiles";
const ACTIVE_KEY = "sixseven-active-profile";

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

const DEFAULT_PROFILE: ServerProfile = {
  id: "default",
  name: "Local",
  host: "localhost",
  port: 6767,
  user: "sixseven",
  isDefault: true,
};

/** Load all saved profiles. Always includes the default profile. */
export function loadProfiles(): ServerProfile[] {
  if (typeof window === "undefined") return [DEFAULT_PROFILE];
  try {
    const raw = localStorage.getItem(PROFILES_KEY);
    if (!raw) return [DEFAULT_PROFILE];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed) || parsed.length === 0) return [DEFAULT_PROFILE];
    return parsed as ServerProfile[];
  } catch {
    return [DEFAULT_PROFILE];
  }
}

/** Save profiles to localStorage. */
function saveProfiles(profiles: ServerProfile[]): void {
  if (typeof window === "undefined") return;
  try {
    localStorage.setItem(PROFILES_KEY, JSON.stringify(profiles));
  } catch {
    // localStorage quota exceeded — ignore
  }
}

/** Get the ID of the active profile. */
export function loadActiveProfileId(): string {
  if (typeof window === "undefined") return "default";
  try {
    return localStorage.getItem(ACTIVE_KEY) || "default";
  } catch {
    return "default";
  }
}

/** Save the active profile ID. */
export function saveActiveProfileId(id: string): void {
  if (typeof window === "undefined") return;
  try {
    localStorage.setItem(ACTIVE_KEY, id);
  } catch {
    // ignore
  }
}

/** Add a new profile. Returns the created profile. */
export function addProfile(
  name: string,
  host: string,
  port: number,
  user: string
): ServerProfile {
  const profiles = loadProfiles();
  const profile: ServerProfile = {
    id: generateId(),
    name,
    host,
    port,
    user,
  };
  profiles.push(profile);
  saveProfiles(profiles);
  return profile;
}

/** Update an existing profile. */
export function updateProfile(updated: ServerProfile): void {
  const profiles = loadProfiles();
  const idx = profiles.findIndex((p) => p.id === updated.id);
  if (idx !== -1) {
    profiles[idx] = updated;
    saveProfiles(profiles);
  }
}

/** Delete a profile by ID. Cannot delete the default profile. */
export function deleteProfile(id: string): void {
  if (id === "default") return;
  const profiles = loadProfiles();
  saveProfiles(profiles.filter((p) => p.id !== id));
}
