"use client";

import { SchemaBrowser } from "@/components/SchemaBrowser";
import { SchemaDetails } from "@/components/SchemaDetails";
import { QueryEditor } from "@/components/QueryEditor";
import { GraphExplorer } from "@/components/GraphExplorer";
import { Dashboard } from "@/components/Dashboard";
import { ConnectionManager } from "@/components/ConnectionManager";
import { ConnectionProvider, useConnection } from "@/lib/ConnectionContext";
import { useState, useEffect, useCallback } from "react";
import type {
  SelectedItem,
  DatabaseInfo,
  DatabaseSchema,
  EdgeTypeInfo,
} from "@/lib/types";
import { fetchDatabases, fetchDatabaseSchema } from "@/lib/schema-utils";
import type { SchemaCompletionData } from "@/lib/sixseven-sql-lang";

type ActivePanel = "schema" | "query" | "graph" | "dashboard";

export default function Home() {
  return (
    <ConnectionProvider>
      <AppContent />
    </ConnectionProvider>
  );
}

function AppContent() {
  const { connectionParams, status } = useConnection();

  const [selected, setSelected] = useState<SelectedItem | null>(null);
  const [activePanel, setActivePanel] = useState<ActivePanel>("query");
  const [databases, setDatabases] = useState<DatabaseInfo[]>([]);
  const [schemaData, setSchemaData] = useState<SchemaCompletionData>({
    tables: [],
    edgeTypes: [],
  });
  const [allEdgeTypeInfos, setAllEdgeTypeInfos] = useState<EdgeTypeInfo[]>([]);

  // Load databases for the query editor dropdown and autocomplete
  const loadSchemaForCompletion = useCallback(async () => {
    try {
      const dbs = await fetchDatabases(connectionParams);
      setDatabases(dbs);

      // Load schema for all non-system databases (for autocomplete)
      const allTables: SchemaCompletionData["tables"] = [];
      const allEdgeTypes: string[] = [];
      const edgeTypeInfos: EdgeTypeInfo[] = [];

      for (const db of dbs.filter((d) => !d.isSystem)) {
        try {
          const schema: DatabaseSchema = await fetchDatabaseSchema(
            db.name,
            connectionParams
          );
          for (const table of schema.tables) {
            allTables.push({
              name: table.name,
              columns: table.columns.map((c) => ({
                name: c.name,
                type: c.type,
              })),
            });
          }
          for (const et of schema.edgeTypes) {
            if (!allEdgeTypes.includes(et.name)) {
              allEdgeTypes.push(et.name);
              edgeTypeInfos.push(et);
            }
          }
        } catch {
          // Skip databases that fail to load
        }
      }

      setSchemaData({ tables: allTables, edgeTypes: allEdgeTypes });
      setAllEdgeTypeInfos(edgeTypeInfos);
    } catch {
      // Databases will be empty — editor still works
    }
  }, [connectionParams]);

  // Reload schema when connection changes or becomes connected
  useEffect(() => {
    if (status === "connected") {
      loadSchemaForCompletion();
    }
  }, [status, loadSchemaForCompletion]);

  const databaseNames = databases
    .filter((d) => !d.isSystem)
    .map((d) => d.name);

  return (
    <div className="flex h-screen">
      {/* Left panel: Schema Browser */}
      <div className="w-80 border-r border-gray-800 flex flex-col overflow-hidden shrink-0">
        <div className="p-3 border-b border-gray-800 flex items-center gap-2">
          <span className="text-sm font-semibold text-gray-100">SixSevenDB</span>
          <span className="text-xs text-gray-500">Admin</span>
          <div className="ml-auto relative">
            <ConnectionManager />
          </div>
        </div>

        {/* Panel toggle */}
        <div className="flex border-b border-gray-800 shrink-0">
          <button
            className={`flex-1 px-3 py-1.5 text-xs font-medium ${
              activePanel === "query"
                ? "text-blue-400 border-b-2 border-blue-400 bg-gray-900/50"
                : "text-gray-500 hover:text-gray-300"
            }`}
            onClick={() => setActivePanel("query")}
          >
            Query
          </button>
          <button
            className={`flex-1 px-3 py-1.5 text-xs font-medium ${
              activePanel === "schema"
                ? "text-blue-400 border-b-2 border-blue-400 bg-gray-900/50"
                : "text-gray-500 hover:text-gray-300"
            }`}
            onClick={() => setActivePanel("schema")}
          >
            Schema
          </button>
          <button
            className={`flex-1 px-3 py-1.5 text-xs font-medium ${
              activePanel === "graph"
                ? "text-blue-400 border-b-2 border-blue-400 bg-gray-900/50"
                : "text-gray-500 hover:text-gray-300"
            }`}
            onClick={() => setActivePanel("graph")}
          >
            Graph
          </button>
          <button
            className={`flex-1 px-3 py-1.5 text-xs font-medium ${
              activePanel === "dashboard"
                ? "text-blue-400 border-b-2 border-blue-400 bg-gray-900/50"
                : "text-gray-500 hover:text-gray-300"
            }`}
            onClick={() => setActivePanel("dashboard")}
          >
            Dashboard
          </button>
        </div>

        <SchemaBrowser onSelect={setSelected} />
      </div>

      {/* Right panel: Query Editor, Schema Details, Graph Explorer, or Dashboard */}
      <div className="flex-1 overflow-hidden">
        {activePanel === "query" ? (
          <QueryEditor
            databases={databaseNames}
            schemaData={schemaData}
            defaultDatabase={databaseNames[0] || ""}
          />
        ) : activePanel === "graph" ? (
          <GraphExplorer
            databases={databaseNames}
            edgeTypes={allEdgeTypeInfos}
            defaultDatabase={databaseNames[0] || ""}
          />
        ) : activePanel === "dashboard" ? (
          <Dashboard />
        ) : selected ? (
          <SchemaDetails item={selected} />
        ) : (
          <div className="flex items-center justify-center h-full text-gray-600">
            <p>Select an item from the schema browser</p>
          </div>
        )}
      </div>
    </div>
  );
}
