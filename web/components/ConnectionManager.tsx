"use client";

import { useState, useEffect } from "react";
import { useConnection } from "@/lib/ConnectionContext";
import type { ServerProfile } from "@/lib/connection-types";

export function ConnectionManager() {
  const {
    profiles,
    activeProfile,
    status,
    setActiveProfile,
    addNewProfile,
    editProfile,
    removeProfile,
    checkConnection,
  } = useConnection();

  const [showModal, setShowModal] = useState(false);
  const [showDropdown, setShowDropdown] = useState(false);

  // Close dropdown on outside click
  useEffect(() => {
    if (!showDropdown) return;
    const handler = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (!target.closest("[data-conn-dropdown]")) {
        setShowDropdown(false);
      }
    };
    document.addEventListener("mousedown", handler);
    return () => document.removeEventListener("mousedown", handler);
  }, [showDropdown]);

  const statusColor =
    status === "connected"
      ? "bg-green-500"
      : status === "connecting"
        ? "bg-yellow-500 animate-pulse"
        : "bg-red-500";

  const statusLabel =
    status === "connected"
      ? "Connected"
      : status === "connecting"
        ? "Connecting..."
        : "Disconnected";

  return (
    <>
      <div className="flex items-center gap-2" data-conn-dropdown>
        {/* Status dot + profile selector */}
        <button
          className="flex items-center gap-2 px-2 py-1 rounded hover:bg-gray-800 text-xs"
          onClick={() => setShowDropdown(!showDropdown)}
          title={`${activeProfile.name} — ${statusLabel}`}
        >
          <span className={`w-2 h-2 rounded-full shrink-0 ${statusColor}`} />
          <span className="text-gray-300 truncate max-w-[120px]">
            {activeProfile.name}
          </span>
          <span className="text-gray-600 text-[10px]">
            {activeProfile.host}:{activeProfile.port}
          </span>
          <svg
            className="w-3 h-3 text-gray-500"
            fill="none"
            viewBox="0 0 24 24"
            stroke="currentColor"
          >
            <path
              strokeLinecap="round"
              strokeLinejoin="round"
              strokeWidth={2}
              d="M19 9l-7 7-7-7"
            />
          </svg>
        </button>

        {/* Dropdown */}
        {showDropdown && (
          <div className="absolute top-8 right-0 z-50 w-64 bg-gray-900 border border-gray-700 rounded-lg shadow-xl">
            <div className="p-2 border-b border-gray-800">
              <div className="text-[10px] text-gray-500 uppercase tracking-wider px-2 py-1">
                Server Profiles
              </div>
              {profiles.map((p) => (
                <button
                  key={p.id}
                  className={`w-full flex items-center gap-2 px-2 py-1.5 rounded text-xs ${
                    p.id === activeProfile.id
                      ? "bg-blue-900/40 text-blue-300"
                      : "text-gray-300 hover:bg-gray-800"
                  }`}
                  onClick={() => {
                    setActiveProfile(p.id);
                    setShowDropdown(false);
                  }}
                >
                  <span
                    className={`w-1.5 h-1.5 rounded-full shrink-0 ${
                      p.id === activeProfile.id ? statusColor : "bg-gray-600"
                    }`}
                  />
                  <span className="truncate">{p.name}</span>
                  <span className="text-gray-600 text-[10px] ml-auto">
                    {p.host}:{p.port}
                  </span>
                </button>
              ))}
            </div>
            <div className="p-2 flex gap-1">
              <button
                className="flex-1 text-[10px] text-gray-400 hover:text-gray-200 px-2 py-1 rounded hover:bg-gray-800"
                onClick={() => {
                  setShowModal(true);
                  setShowDropdown(false);
                }}
              >
                Manage Profiles...
              </button>
              <button
                className="text-[10px] text-gray-400 hover:text-gray-200 px-2 py-1 rounded hover:bg-gray-800"
                onClick={() => {
                  checkConnection();
                  setShowDropdown(false);
                }}
                title="Test connection"
              >
                Reconnect
              </button>
            </div>
          </div>
        )}
      </div>

      {/* Profile management modal */}
      {showModal && (
        <ProfileModal
          profiles={profiles}
          activeProfile={activeProfile}
          onAdd={addNewProfile}
          onEdit={editProfile}
          onDelete={removeProfile}
          onClose={() => setShowModal(false)}
        />
      )}
    </>
  );
}

function ProfileModal({
  profiles,
  activeProfile,
  onAdd,
  onEdit,
  onDelete,
  onClose,
}: {
  profiles: ServerProfile[];
  activeProfile: ServerProfile;
  onAdd: (name: string, host: string, port: number, user: string) => void;
  onEdit: (profile: ServerProfile) => void;
  onDelete: (id: string) => void;
  onClose: () => void;
}) {
  const [editing, setEditing] = useState<ServerProfile | null>(null);
  const [isNew, setIsNew] = useState(false);

  const startNew = () => {
    setEditing({
      id: "",
      name: "",
      host: "localhost",
      port: 6767,
      user: "sixseven",
    });
    setIsNew(true);
  };

  const startEdit = (p: ServerProfile) => {
    setEditing({ ...p });
    setIsNew(false);
  };

  const handleSave = () => {
    if (!editing) return;
    if (!editing.name.trim() || !editing.host.trim()) return;

    if (isNew) {
      onAdd(editing.name, editing.host, editing.port, editing.user);
    } else {
      onEdit(editing);
    }
    setEditing(null);
    setIsNew(false);
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60">
      <div className="bg-gray-900 border border-gray-700 rounded-lg shadow-2xl w-[480px] max-h-[80vh] flex flex-col">
        {/* Header */}
        <div className="flex items-center justify-between p-4 border-b border-gray-800">
          <h2 className="text-sm font-semibold text-gray-100">
            Server Profiles
          </h2>
          <button
            className="text-gray-500 hover:text-gray-300"
            onClick={onClose}
          >
            <svg
              className="w-4 h-4"
              fill="none"
              viewBox="0 0 24 24"
              stroke="currentColor"
            >
              <path
                strokeLinecap="round"
                strokeLinejoin="round"
                strokeWidth={2}
                d="M6 18L18 6M6 6l12 12"
              />
            </svg>
          </button>
        </div>

        {/* Profile list */}
        <div className="flex-1 overflow-y-auto p-4 space-y-2">
          {profiles.map((p) => (
            <div
              key={p.id}
              className={`flex items-center gap-3 p-3 rounded border ${
                p.id === activeProfile.id
                  ? "border-blue-800 bg-blue-950/30"
                  : "border-gray-800 bg-gray-950"
              }`}
            >
              <div className="flex-1 min-w-0">
                <div className="text-xs font-medium text-gray-200 truncate">
                  {p.name}
                </div>
                <div className="text-[10px] text-gray-500">
                  {p.host}:{p.port} / {p.user}
                </div>
              </div>
              <div className="flex gap-1 shrink-0">
                <button
                  className="text-[10px] text-gray-500 hover:text-blue-400 px-2 py-1 rounded hover:bg-gray-800"
                  onClick={() => startEdit(p)}
                >
                  Edit
                </button>
                {!p.isDefault && (
                  <button
                    className="text-[10px] text-gray-500 hover:text-red-400 px-2 py-1 rounded hover:bg-gray-800"
                    onClick={() => onDelete(p.id)}
                  >
                    Delete
                  </button>
                )}
              </div>
            </div>
          ))}

          {/* Add new button */}
          {!editing && (
            <button
              className="w-full text-xs text-gray-500 hover:text-gray-300 border border-dashed border-gray-800 rounded p-2 hover:bg-gray-900"
              onClick={startNew}
            >
              + Add Profile
            </button>
          )}

          {/* Edit/New form */}
          {editing && (
            <div className="border border-gray-700 rounded p-3 space-y-3 bg-gray-950">
              <div className="text-xs font-medium text-gray-300 mb-2">
                {isNew ? "New Profile" : `Edit: ${editing.name}`}
              </div>
              <div className="grid grid-cols-2 gap-2">
                <label className="block">
                  <span className="text-[10px] text-gray-500">Name</span>
                  <input
                    className="w-full mt-0.5 px-2 py-1 bg-gray-900 border border-gray-700 rounded text-xs text-gray-200 focus:border-blue-600 focus:outline-none"
                    value={editing.name}
                    onChange={(e) =>
                      setEditing({ ...editing, name: e.target.value })
                    }
                    placeholder="Production"
                  />
                </label>
                <label className="block">
                  <span className="text-[10px] text-gray-500">Host</span>
                  <input
                    className="w-full mt-0.5 px-2 py-1 bg-gray-900 border border-gray-700 rounded text-xs text-gray-200 focus:border-blue-600 focus:outline-none"
                    value={editing.host}
                    onChange={(e) =>
                      setEditing({ ...editing, host: e.target.value })
                    }
                    placeholder="localhost"
                  />
                </label>
                <label className="block">
                  <span className="text-[10px] text-gray-500">Port</span>
                  <input
                    type="number"
                    className="w-full mt-0.5 px-2 py-1 bg-gray-900 border border-gray-700 rounded text-xs text-gray-200 focus:border-blue-600 focus:outline-none"
                    value={editing.port}
                    onChange={(e) =>
                      setEditing({
                        ...editing,
                        port: parseInt(e.target.value, 10) || 6767,
                      })
                    }
                  />
                </label>
                <label className="block">
                  <span className="text-[10px] text-gray-500">User</span>
                  <input
                    className="w-full mt-0.5 px-2 py-1 bg-gray-900 border border-gray-700 rounded text-xs text-gray-200 focus:border-blue-600 focus:outline-none"
                    value={editing.user}
                    onChange={(e) =>
                      setEditing({ ...editing, user: e.target.value })
                    }
                    placeholder="sixseven"
                  />
                </label>
              </div>
              <div className="flex gap-2 justify-end">
                <button
                  className="text-[10px] px-3 py-1 rounded text-gray-400 hover:text-gray-200 hover:bg-gray-800"
                  onClick={() => {
                    setEditing(null);
                    setIsNew(false);
                  }}
                >
                  Cancel
                </button>
                <button
                  className="text-[10px] px-3 py-1 rounded bg-blue-700 text-white hover:bg-blue-600 disabled:opacity-50"
                  disabled={!editing.name.trim() || !editing.host.trim()}
                  onClick={handleSave}
                >
                  {isNew ? "Add" : "Save"}
                </button>
              </div>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
