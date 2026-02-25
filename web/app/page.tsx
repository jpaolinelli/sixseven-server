"use client";

import { SchemaBrowser } from "@/components/SchemaBrowser";
import { SchemaDetails } from "@/components/SchemaDetails";
import { QueryEditor } from "@/components/QueryEditor";
import { useState, useEffect, useCallback } from "react";
import type { SelectedItem, DatabaseInfo, DatabaseSchema } from "@/lib/types";
import { fetchDatabases, fetchDatabaseSchema } from "@/lib/schema-utils";
import type { SchemaCompletionData } from "@/lib/giodb-sql-lang";

type ActivePanel = "schema" | "query";

export default function Home() {
  const [selected, setSelected] = useState<SelectedItem | null>(null);
  const [activePanel, setActivePanel] = useState<ActivePanel>("query");
  const [databases, setDatabases] = useState<DatabaseInfo[]>([]);
  const [schemaData, setSchemaData] = useState<SchemaCompletionData>({
    tables: [],
    edgeTypes: [],
  });

  // Load databases for the query editor dropdown and autocomplete
  const loadSchemaForCompletion = useCallback(async () => {
    try {
      const dbs = await fetchDatabases();
      setDatabases(dbs);

      // Load schema for all non-system databases (for autocomplete)
      const allTables: SchemaCompletionData["tables"] = [];
      const allEdgeTypes: string[] = [];

      for (const db of dbs.filter((d) => !d.isSystem)) {
        try {
          const schema: DatabaseSchema = await fetchDatabaseSchema(db.name);
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
            }
          }
        } catch {
          // Skip databases that fail to load
        }
      }

      setSchemaData({ tables: allTables, edgeTypes: allEdgeTypes });
    } catch {
      // Databases will be empty — editor still works
    }
  }, []);

  useEffect(() => {
    loadSchemaForCompletion();
  }, [loadSchemaForCompletion]);

  const databaseNames = databases
    .filter((d) => !d.isSystem)
    .map((d) => d.name);

  return (
    <div className="flex h-screen">
      {/* Left panel: Schema Browser */}
      <div className="w-80 border-r border-gray-800 flex flex-col overflow-hidden shrink-0">
        <div className="p-3 border-b border-gray-800 flex items-center gap-2">
          <span className="text-sm font-semibold text-gray-100">GioDB</span>
          <span className="text-xs text-gray-500">Admin</span>
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
        </div>

        <SchemaBrowser onSelect={setSelected} />
      </div>

      {/* Right panel: Query Editor or Schema Details */}
      <div className="flex-1 overflow-hidden">
        {activePanel === "query" ? (
          <QueryEditor
            databases={databaseNames}
            schemaData={schemaData}
            defaultDatabase={databaseNames[0] || ""}
          />
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
