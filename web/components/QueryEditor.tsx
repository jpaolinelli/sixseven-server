"use client";

/**
 * QueryEditor — Full query editor with:
 *  - Multi-tab support
 *  - CodeMirror editor with SixSevenDB syntax highlighting & autocomplete
 *  - Query execution with results display
 *  - Persistent query history (localStorage)
 *  - Keyboard shortcuts
 */

import { useState, useCallback, useEffect, useRef } from "react";
import { SqlEditor } from "./SqlEditor";
import { QueryResults } from "./QueryResults";
import { useConnection } from "@/lib/ConnectionContext";
import type { SchemaCompletionData } from "@/lib/sixseven-sql-lang";
import {
  loadHistory,
  addToHistory,
  searchHistory,
  clearHistory,
  type HistoryEntry,
} from "@/lib/query-history";
import { isTraverseQuery, buildEdgeQuery, buildSourceNodeQuery } from "@/lib/graph-query-utils";

interface QueryTab {
  id: string;
  name: string;
  sql: string;
  database: string;
  results: {
    columns: string[];
    rows: (string | number | boolean | null)[][];
    edgeColumns?: string[];
    edgeRows?: (string | number | boolean | null)[][];
    /** Source (starting) node data for graph visualization. */
    sourceNodeColumns?: string[];
    sourceNodeRows?: (string | number | boolean | null)[][];
    error?: string | null;
    durationMs?: number;
    isTraverseResult?: boolean;
  } | null;
  isExecuting: boolean;
}

interface QueryEditorProps {
  /** Available databases for the dropdown. */
  databases: string[];
  /** Schema data for autocomplete. */
  schemaData?: SchemaCompletionData;
  /** Default database. */
  defaultDatabase?: string;
}

function createTab(database: string, index: number): QueryTab {
  return {
    id: `tab-${Date.now()}-${index}`,
    name: `Query ${index}`,
    sql: "",
    database,
    results: null,
    isExecuting: false,
  };
}

export function QueryEditor({
  databases,
  schemaData,
  defaultDatabase = "",
}: QueryEditorProps) {
  const { connectionParams } = useConnection();
  const [tabs, setTabs] = useState<QueryTab[]>(() => [
    createTab(defaultDatabase || databases[0] || "", 1),
  ]);
  const [activeTabId, setActiveTabId] = useState<string>(tabs[0].id);
  const [showHistory, setShowHistory] = useState(false);
  const [historyEntries, setHistoryEntries] = useState<HistoryEntry[]>([]);
  const [historySearch, setHistorySearch] = useState("");
  const tabCounter = useRef(2);

  const activeTab = tabs.find((t) => t.id === activeTabId) ?? tabs[0];

  // Load history when panel opens
  useEffect(() => {
    if (showHistory) {
      setHistoryEntries(
        historySearch ? searchHistory(historySearch) : loadHistory()
      );
    }
  }, [showHistory, historySearch]);

  // Execute query
  const executeQuery = useCallback(
    async (sql: string) => {
      if (!sql.trim()) return;

      // Mark as executing
      setTabs((prev) =>
        prev.map((t) =>
          t.id === activeTabId
            ? { ...t, isExecuting: true, results: null }
            : t
        )
      );

      const startTime = performance.now();
      try {
        // Execute the primary query
        const fetchOpts = {
          method: "POST",
          headers: { "Content-Type": "application/json" },
        };

        const primaryFetch = fetch("/api/query", {
          ...fetchOpts,
          body: JSON.stringify({
            sql,
            database: activeTab.database,
            connection: connectionParams,
          }),
        });

        // If this is a TRAVERSE query, also fire the MODE EDGES variant
        // and a SELECT * node query for full graph metadata (__node, __source)
        const isTraverse = isTraverseQuery(sql);
        const edgeFetch = isTraverse
          ? fetch("/api/query", {
              ...fetchOpts,
              body: JSON.stringify({
                sql: buildEdgeQuery(sql),
                database: activeTab.database,
                connection: connectionParams,
              }),
            })
          : null;
        const sourceNodeSql = isTraverse ? buildSourceNodeQuery(sql) : null;
        const sourceNodeFetch = sourceNodeSql
          ? fetch("/api/query", {
              ...fetchOpts,
              body: JSON.stringify({
                sql: sourceNodeSql,
                database: activeTab.database,
                connection: connectionParams,
              }),
            })
          : null;

        const res = await primaryFetch;
        const durationMs = Math.round(performance.now() - startTime);

        if (!res.ok) {
          const body = await res.json().catch(() => ({ error: "Unknown error" }));
          const errorMsg = body.error || `HTTP ${res.status}`;
          addToHistory(sql, activeTab.database, durationMs, errorMsg);
          setTabs((prev) =>
            prev.map((t) =>
              t.id === activeTabId
                ? {
                    ...t,
                    isExecuting: false,
                    results: { columns: [], rows: [], error: errorMsg, durationMs },
                  }
                : t
            )
          );
          return;
        }

        const data = await res.json();

        // Await edge + graph-node results if available (failures are non-fatal)
        let edgeColumns: string[] | undefined;
        let edgeRows: (string | number | boolean | null)[][] | undefined;
        let sourceNodeColumns: string[] | undefined;
        let sourceNodeRows: (string | number | boolean | null)[][] | undefined;
        if (edgeFetch) {
          try {
            const edgeRes = await edgeFetch;
            if (edgeRes.ok) {
              const edgeData = await edgeRes.json();
              edgeColumns = edgeData.columns || [];
              edgeRows = edgeData.rows || [];
            }
          } catch {
            // Edge query failure is non-fatal — graph view just won't have edges
          }
        }
        if (sourceNodeFetch) {
          try {
            const snRes = await sourceNodeFetch;
            if (snRes.ok) {
              const snData = await snRes.json();
              sourceNodeColumns = snData.columns || [];
              sourceNodeRows = snData.rows || [];
            }
          } catch {
            // Source node fetch failure is non-fatal — placeholder will be used
          }
        }

        addToHistory(sql, activeTab.database, durationMs);
        setTabs((prev) =>
          prev.map((t) =>
            t.id === activeTabId
              ? {
                  ...t,
                  isExecuting: false,
                  results: {
                    columns: data.columns || [],
                    rows: data.rows || [],
                    edgeColumns,
                    edgeRows,
                    sourceNodeColumns,
                    sourceNodeRows,
                    durationMs,
                    isTraverseResult: isTraverse,
                  },
                }
              : t
          )
        );
      } catch (err) {
        const durationMs = Math.round(performance.now() - startTime);
        const errorMsg = err instanceof Error ? err.message : "Network error";
        addToHistory(sql, activeTab.database, durationMs, errorMsg);
        setTabs((prev) =>
          prev.map((t) =>
            t.id === activeTabId
              ? {
                  ...t,
                  isExecuting: false,
                  results: { columns: [], rows: [], error: errorMsg, durationMs },
                }
              : t
          )
        );
      }
    },
    [activeTabId, activeTab.database, connectionParams]
  );

  // Save to history (Ctrl+S)
  const saveToHistory = useCallback(
    (sql: string) => {
      if (sql.trim()) {
        addToHistory(sql, activeTab.database);
      }
    },
    [activeTab.database]
  );

  // Clear results (Ctrl+L)
  const clearResults = useCallback(() => {
    setTabs((prev) =>
      prev.map((t) =>
        t.id === activeTabId ? { ...t, results: null } : t
      )
    );
  }, [activeTabId]);

  // Update SQL content
  const handleSqlChange = useCallback(
    (sql: string) => {
      setTabs((prev) =>
        prev.map((t) => (t.id === activeTabId ? { ...t, sql } : t))
      );
    },
    [activeTabId]
  );

  // Update database for active tab
  const handleDatabaseChange = useCallback(
    (database: string) => {
      setTabs((prev) =>
        prev.map((t) => (t.id === activeTabId ? { ...t, database } : t))
      );
    },
    [activeTabId]
  );

  // Tab management
  const addTab = useCallback(() => {
    const newTab = createTab(
      activeTab.database,
      tabCounter.current++
    );
    setTabs((prev) => [...prev, newTab]);
    setActiveTabId(newTab.id);
  }, [activeTab.database]);

  const closeTab = useCallback(
    (tabId: string) => {
      setTabs((prev) => {
        const filtered = prev.filter((t) => t.id !== tabId);
        if (filtered.length === 0) {
          // Always keep at least one tab
          const newTab = createTab(defaultDatabase || databases[0] || "", tabCounter.current++);
          setActiveTabId(newTab.id);
          return [newTab];
        }
        if (tabId === activeTabId) {
          setActiveTabId(filtered[filtered.length - 1].id);
        }
        return filtered;
      });
    },
    [activeTabId, defaultDatabase, databases]
  );

  // Load history entry into editor
  const loadFromHistory = useCallback(
    (entry: HistoryEntry) => {
      setTabs((prev) =>
        prev.map((t) =>
          t.id === activeTabId
            ? { ...t, sql: entry.sql, database: entry.database }
            : t
        )
      );
      setShowHistory(false);
    },
    [activeTabId]
  );

  // Re-execute history entry
  const reExecuteFromHistory = useCallback(
    (entry: HistoryEntry) => {
      setTabs((prev) =>
        prev.map((t) =>
          t.id === activeTabId
            ? { ...t, sql: entry.sql, database: entry.database }
            : t
        )
      );
      setShowHistory(false);
      // Execute after state update
      setTimeout(() => executeQuery(entry.sql), 0);
    },
    [activeTabId, executeQuery]
  );

  return (
    <div className="flex flex-col h-full">
      {/* Toolbar: tabs + actions */}
      <div className="flex items-center border-b border-gray-800 bg-gray-950 shrink-0">
        {/* Tabs */}
        <div className="flex items-center overflow-x-auto flex-1 min-w-0">
          {tabs.map((tab) => (
            <div
              key={tab.id}
              className={`flex items-center gap-1 px-3 py-1.5 text-xs cursor-pointer border-r border-gray-800 shrink-0 group ${
                tab.id === activeTabId
                  ? "bg-gray-900 text-gray-200"
                  : "text-gray-500 hover:text-gray-300 hover:bg-gray-900/50"
              }`}
              onClick={() => setActiveTabId(tab.id)}
            >
              <span className="truncate max-w-[120px]">{tab.name}</span>
              {tab.isExecuting && (
                <span className="text-yellow-400 animate-pulse">*</span>
              )}
              <button
                className="ml-1 opacity-0 group-hover:opacity-100 text-gray-600 hover:text-gray-300 text-[10px]"
                onClick={(e) => {
                  e.stopPropagation();
                  closeTab(tab.id);
                }}
                title="Close tab"
              >
                x
              </button>
            </div>
          ))}
          {/* Add tab */}
          <button
            className="px-2 py-1.5 text-xs text-gray-600 hover:text-gray-300 hover:bg-gray-900/50 shrink-0"
            onClick={addTab}
            title="New query tab"
          >
            +
          </button>
        </div>

        {/* Right toolbar */}
        <div className="flex items-center gap-1 px-2 shrink-0">
          {/* Database selector */}
          <select
            className="bg-gray-900 border border-gray-800 rounded px-2 py-1 text-xs text-gray-300 focus:outline-none focus:border-gray-600"
            value={activeTab.database}
            onChange={(e) => handleDatabaseChange(e.target.value)}
          >
            {databases.length === 0 && (
              <option value="">No databases</option>
            )}
            {databases.map((db) => (
              <option key={db} value={db}>
                {db}
              </option>
            ))}
          </select>

          {/* Execute button */}
          <button
            className="px-2.5 py-1 text-xs font-medium bg-blue-600 hover:bg-blue-500 text-white rounded disabled:opacity-50 disabled:cursor-not-allowed"
            onClick={() => executeQuery(activeTab.sql)}
            disabled={!activeTab.sql.trim() || activeTab.isExecuting}
            title="Execute (Ctrl+Enter)"
          >
            {activeTab.isExecuting ? "..." : "Run"}
          </button>

          {/* History toggle */}
          <button
            className={`px-2 py-1 text-xs rounded ${
              showHistory
                ? "bg-gray-700 text-gray-200"
                : "text-gray-400 hover:text-gray-200 hover:bg-gray-800"
            }`}
            onClick={() => setShowHistory(!showHistory)}
            title="Query history"
          >
            History
          </button>
        </div>
      </div>

      {/* Main content area */}
      <div className="flex flex-1 min-h-0">
        {/* Editor + Results (vertical split) */}
        <div className="flex flex-col flex-1 min-w-0">
          {/* Editor pane */}
          <div className="h-[40%] min-h-[120px] border-b border-gray-800">
            <SqlEditor
              key={activeTabId}
              value={activeTab.sql}
              onChange={handleSqlChange}
              onExecute={executeQuery}
              onSave={saveToHistory}
              onClearResults={clearResults}
              schemaData={schemaData}
            />
          </div>

          {/* Results pane */}
          <div className="flex-1 min-h-0 overflow-hidden">
            {activeTab.results ? (
              <QueryResults
                columns={activeTab.results.columns}
                rows={activeTab.results.rows}
                edgeColumns={activeTab.results.edgeColumns}
                edgeRows={activeTab.results.edgeRows}
                sourceNodeColumns={activeTab.results.sourceNodeColumns}
                sourceNodeRows={activeTab.results.sourceNodeRows}
                error={activeTab.results.error}
                durationMs={activeTab.results.durationMs}
                isLoading={activeTab.isExecuting}
                isTraverseResult={activeTab.results.isTraverseResult}
              />
            ) : activeTab.isExecuting ? (
              <QueryResults
                columns={[]}
                rows={[]}
                isLoading
              />
            ) : (
              <QueryResults columns={[]} rows={[]} />
            )}
          </div>
        </div>

        {/* History panel (slide-in) */}
        {showHistory && (
          <div className="w-72 border-l border-gray-800 flex flex-col shrink-0 bg-gray-950">
            <div className="p-2 border-b border-gray-800 flex items-center gap-1">
              <input
                type="text"
                placeholder="Search history..."
                className="flex-1 bg-gray-900 border border-gray-800 rounded px-2 py-1 text-xs text-gray-300 placeholder-gray-600 focus:outline-none focus:border-gray-600"
                value={historySearch}
                onChange={(e) => setHistorySearch(e.target.value)}
              />
              <button
                className="px-1.5 py-1 text-[10px] text-gray-500 hover:text-red-400"
                onClick={() => {
                  clearHistory();
                  setHistoryEntries([]);
                }}
                title="Clear all history"
              >
                Clear
              </button>
            </div>
            <div className="flex-1 overflow-y-auto">
              {historyEntries.length === 0 && (
                <div className="px-3 py-4 text-xs text-gray-600">
                  No queries in history
                </div>
              )}
              {historyEntries.map((entry) => (
                <div
                  key={entry.id}
                  className="border-b border-gray-800/50 hover:bg-gray-900/50 group"
                >
                  <div className="px-2 py-1.5">
                    <div className="flex items-center gap-1 mb-1">
                      <span className="text-[10px] text-gray-600">
                        {new Date(entry.timestamp).toLocaleTimeString()}
                      </span>
                      <span className="text-[10px] text-gray-700">
                        {entry.database}
                      </span>
                      {entry.durationMs !== undefined && (
                        <span className="text-[10px] text-gray-700">
                          {entry.durationMs}ms
                        </span>
                      )}
                      {entry.error && (
                        <span className="text-[10px] text-red-500">error</span>
                      )}
                    </div>
                    <pre className="text-xs text-gray-400 whitespace-pre-wrap break-all line-clamp-3 font-mono">
                      {entry.sql}
                    </pre>
                    <div className="flex gap-1 mt-1 opacity-0 group-hover:opacity-100 transition-opacity">
                      <button
                        className="text-[10px] text-blue-400 hover:text-blue-300"
                        onClick={() => loadFromHistory(entry)}
                      >
                        Edit
                      </button>
                      <button
                        className="text-[10px] text-green-400 hover:text-green-300"
                        onClick={() => reExecuteFromHistory(entry)}
                      >
                        Run
                      </button>
                    </div>
                  </div>
                </div>
              ))}
            </div>
          </div>
        )}
      </div>

      {/* Shortcuts hint bar */}
      <div className="flex items-center gap-4 px-3 py-1 border-t border-gray-800 text-[10px] text-gray-600 shrink-0 bg-gray-950">
        <span>Ctrl+Enter: Run</span>
        <span>Ctrl+S: Save</span>
        <span>Ctrl+L: Clear</span>
        <span>Ctrl+Space: Autocomplete</span>
        <span>Tab: Indent</span>
      </div>
    </div>
  );
}
