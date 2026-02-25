"use client";

import { useEffect, useState, useCallback } from "react";
import { TreeNode } from "./TreeNode";
import { fetchDatabases, fetchDatabaseSchema } from "@/lib/schema-utils";
import { useConnection } from "@/lib/ConnectionContext";
import type {
  DatabaseInfo,
  DatabaseSchema,
  SelectedItem,
} from "@/lib/types";

interface SchemaBrowserProps {
  onSelect: (item: SelectedItem) => void;
}

export function SchemaBrowser({ onSelect }: SchemaBrowserProps) {
  const { connectionParams, status } = useConnection();
  const [databases, setDatabases] = useState<DatabaseInfo[]>([]);
  const [schemas, setSchemas] = useState<Record<string, DatabaseSchema>>({});
  const [loading, setLoading] = useState<Record<string, boolean>>({});
  const [schemaErrors, setSchemaErrors] = useState<Record<string, string>>({});
  const [filter, setFilter] = useState("");
  const [error, setError] = useState<string | null>(null);

  const loadDatabases = useCallback(async () => {
    try {
      setError(null);
      const dbs = await fetchDatabases(connectionParams);
      setDatabases(dbs);
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to load databases");
    }
  }, [connectionParams]);

  // Reload when connection status changes to connected
  useEffect(() => {
    if (status === "connected") {
      setSchemas({});
      loadDatabases();
    }
  }, [status, loadDatabases]);

  const loadSchema = async (dbName: string) => {
    if (schemas[dbName] || loading[dbName]) return;
    setLoading((prev) => ({ ...prev, [dbName]: true }));
    setSchemaErrors((prev) => {
      const next = { ...prev };
      delete next[dbName];
      return next;
    });
    try {
      const schema = await fetchDatabaseSchema(dbName, connectionParams);
      setSchemas((prev) => ({ ...prev, [dbName]: schema }));
    } catch (err) {
      const msg = err instanceof Error ? err.message : "Failed to load schema";
      setSchemaErrors((prev) => ({ ...prev, [dbName]: msg }));
    } finally {
      setLoading((prev) => ({ ...prev, [dbName]: false }));
    }
  };

  const handleRefresh = () => {
    setSchemas({});
    setLoading({});
    setSchemaErrors({});
    loadDatabases();
  };

  const matchesFilter = (name: string) =>
    !filter || name.toLowerCase().includes(filter.toLowerCase());

  return (
    <div className="flex-1 flex flex-col overflow-hidden">
      {/* Search + Refresh */}
      <div className="p-2 flex gap-1">
        <input
          type="text"
          placeholder="Filter..."
          className="flex-1 bg-gray-900 border border-gray-800 rounded px-2 py-1 text-xs text-gray-300 placeholder-gray-600 focus:outline-none focus:border-gray-600"
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
        />
        <button
          onClick={handleRefresh}
          className="px-2 py-1 text-xs text-gray-400 hover:text-gray-200 bg-gray-900 border border-gray-800 rounded hover:bg-gray-800"
          title="Refresh schema"
        >
          ↻
        </button>
      </div>

      {/* Error state */}
      {error && (
        <div className="px-3 py-2 text-xs text-red-400 bg-red-950/30">
          {error}
        </div>
      )}

      {/* Tree */}
      <div className="flex-1 overflow-y-auto px-1 py-1">
        {databases.length === 0 && !error && (
          <div className="px-3 py-4 text-xs text-gray-600">Loading...</div>
        )}
        {databases.map((db) => (
          <DatabaseNode
            key={db.name}
            db={db}
            schema={schemas[db.name]}
            isLoading={loading[db.name] ?? false}
            schemaError={schemaErrors[db.name]}
            filter={filter}
            matchesFilter={matchesFilter}
            onExpand={() => loadSchema(db.name)}
            onSelect={onSelect}
          />
        ))}
      </div>
    </div>
  );
}

function DatabaseNode({
  db,
  schema,
  isLoading,
  schemaError,
  filter,
  matchesFilter,
  onExpand,
  onSelect,
}: {
  db: DatabaseInfo;
  schema?: DatabaseSchema;
  isLoading: boolean;
  schemaError?: string;
  filter: string;
  matchesFilter: (name: string) => boolean;
  onExpand: () => void;
  onSelect: (item: SelectedItem) => void;
}) {
  const handleExpand = () => {
    onExpand();
    onSelect({ kind: "database", database: db.name });
  };

  const tables = schema?.tables.filter((t) => matchesFilter(t.name)) ?? [];
  const edgeTypes =
    schema?.edgeTypes.filter((e) => matchesFilter(e.name)) ?? [];

  return (
    <TreeNode
      label={db.name}
      icon="🗄"
      dimmed={db.isSystem}
      expandable
      onClick={handleExpand}
      badge={db.isSystem ? "system" : undefined}
    >
      {isLoading && (
        <div className="px-4 py-1 text-xs text-gray-600">Loading...</div>
      )}
      {schemaError && (
        <div className="px-4 py-1 text-xs text-red-400">{schemaError}</div>
      )}
      {schema && (
        <>
          {/* Tables */}
          <TreeNode
            label="Tables"
            icon="📋"
            expandable
            defaultExpanded={!filter}
            badge={String(tables.length)}
          >
            {tables.map((table) => (
              <TreeNode
                key={table.name}
                label={table.name}
                icon="⊞"
                expandable
                onClick={() =>
                  onSelect({
                    kind: "table",
                    database: db.name,
                    table: table.name,
                  })
                }
              >
                {/* Columns */}
                <TreeNode
                  label="Columns"
                  icon="≡"
                  expandable
                  badge={String(table.columns.length)}
                >
                  {table.columns
                    .filter((c) => matchesFilter(c.name))
                    .map((col) => {
                      const isEmbedding =
                        col.type.toUpperCase() === "EMBEDDING";
                      const emb = isEmbedding
                        ? table.embeddings.find(
                            (e) => e.columnName === col.name
                          )
                        : undefined;
                      return (
                        <TreeNode
                          key={col.name}
                          label={col.name}
                          icon={isEmbedding ? "⊕" : "·"}
                          badge={col.type}
                          onClick={() =>
                            isEmbedding && emb
                              ? onSelect({
                                  kind: "embedding",
                                  database: db.name,
                                  embedding: emb,
                                })
                              : onSelect({
                                  kind: "column",
                                  database: db.name,
                                  table: table.name,
                                  column: col,
                                })
                          }
                        />
                      );
                    })}
                </TreeNode>

                {/* Indexes */}
                {table.indexes.length > 0 && (
                  <TreeNode
                    label="Indexes"
                    icon="⚡"
                    expandable
                    badge={String(table.indexes.length)}
                  >
                    {table.indexes.map((idx) => (
                      <TreeNode
                        key={idx.name}
                        label={idx.name}
                        icon="↗"
                        badge={idx.type}
                        onClick={() =>
                          onSelect({
                            kind: "index",
                            database: db.name,
                            index: idx,
                          })
                        }
                      />
                    ))}
                  </TreeNode>
                )}
              </TreeNode>
            ))}
          </TreeNode>

          {/* Edge Types */}
          {edgeTypes.length > 0 && (
            <TreeNode
              label="Edge Types"
              icon="⇄"
              expandable
              badge={String(edgeTypes.length)}
            >
              {edgeTypes.map((et) => (
                <TreeNode
                  key={et.name}
                  label={et.name}
                  icon="→"
                  badge={`${et.sourceTable}→${et.targetTable}`}
                  onClick={() =>
                    onSelect({
                      kind: "edgeType",
                      database: db.name,
                      edgeType: et,
                    })
                  }
                />
              ))}
            </TreeNode>
          )}
        </>
      )}
    </TreeNode>
  );
}
