"use client";

import {
  createContext,
  useContext,
  useState,
  useEffect,
  useCallback,
  useRef,
} from "react";
import type { ReactNode } from "react";
import type {
  ServerProfile,
  ConnectionStatus,
  ConnectionParams,
} from "./connection-types";
import {
  loadProfiles,
  loadActiveProfileId,
  saveActiveProfileId,
  addProfile,
  updateProfile,
  deleteProfile,
} from "./connection-profiles";

const PING_INTERVAL_MS = 10_000;
const RECONNECT_DELAY_MS = 5_000;

interface ConnectionContextValue {
  profiles: ServerProfile[];
  activeProfile: ServerProfile;
  status: ConnectionStatus;
  /** Connection params derived from the active profile (pass to API calls). */
  connectionParams: ConnectionParams;
  setActiveProfile: (id: string) => void;
  addNewProfile: (
    name: string,
    host: string,
    port: number,
    user: string
  ) => ServerProfile;
  editProfile: (profile: ServerProfile) => void;
  removeProfile: (id: string) => void;
  /** Manually trigger a connectivity check. */
  checkConnection: () => Promise<void>;
}

const ConnectionContext = createContext<ConnectionContextValue | null>(null);

export function useConnection(): ConnectionContextValue {
  const ctx = useContext(ConnectionContext);
  if (!ctx) {
    throw new Error("useConnection must be used within ConnectionProvider");
  }
  return ctx;
}

export function ConnectionProvider({ children }: { children: ReactNode }) {
  const [profiles, setProfiles] = useState<ServerProfile[]>(() =>
    loadProfiles()
  );
  const [activeId, setActiveId] = useState<string>(() =>
    loadActiveProfileId()
  );
  const [status, setStatus] = useState<ConnectionStatus>("connecting");

  const pingTimer = useRef<ReturnType<typeof setInterval> | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const activeProfile =
    profiles.find((p) => p.id === activeId) || profiles[0];

  const connectionParams: ConnectionParams = {
    host: activeProfile.host,
    port: activeProfile.port,
    user: activeProfile.user,
  };

  const checkConnection = useCallback(async () => {
    try {
      const res = await fetch("/api/ping", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ connection: connectionParams }),
      });
      const data = await res.json();
      setStatus(data.ok ? "connected" : "disconnected");
    } catch {
      setStatus("disconnected");
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeProfile.host, activeProfile.port, activeProfile.user]);

  // Initial connection check + periodic ping
  useEffect(() => {
    setStatus("connecting");
    checkConnection();

    pingTimer.current = setInterval(checkConnection, PING_INTERVAL_MS);
    return () => {
      if (pingTimer.current) clearInterval(pingTimer.current);
    };
  }, [checkConnection]);

  // Auto-reconnect when disconnected
  useEffect(() => {
    if (status === "disconnected") {
      reconnectTimer.current = setTimeout(() => {
        setStatus("connecting");
        checkConnection();
      }, RECONNECT_DELAY_MS);
    }
    return () => {
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current);
    };
  }, [status, checkConnection]);

  const handleSetActiveProfile = useCallback(
    (id: string) => {
      setActiveId(id);
      saveActiveProfileId(id);
      setStatus("connecting");
    },
    []
  );

  const handleAddProfile = useCallback(
    (name: string, host: string, port: number, user: string) => {
      const profile = addProfile(name, host, port, user);
      setProfiles(loadProfiles());
      return profile;
    },
    []
  );

  const handleEditProfile = useCallback((profile: ServerProfile) => {
    updateProfile(profile);
    setProfiles(loadProfiles());
  }, []);

  const handleRemoveProfile = useCallback(
    (id: string) => {
      deleteProfile(id);
      const updated = loadProfiles();
      setProfiles(updated);
      if (activeId === id) {
        const newActive = updated[0]?.id || "default";
        setActiveId(newActive);
        saveActiveProfileId(newActive);
      }
    },
    [activeId]
  );

  return (
    <ConnectionContext.Provider
      value={{
        profiles,
        activeProfile,
        status,
        connectionParams,
        setActiveProfile: handleSetActiveProfile,
        addNewProfile: handleAddProfile,
        editProfile: handleEditProfile,
        removeProfile: handleRemoveProfile,
        checkConnection,
      }}
    >
      {children}
    </ConnectionContext.Provider>
  );
}
