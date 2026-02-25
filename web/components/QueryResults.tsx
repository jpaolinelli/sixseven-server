"use client";

/**
 * QueryResults — Displays query execution results in a scrollable table,
 * along with status info (timing, row count, errors).
 */

interface QueryResultsProps {
  columns: string[];
  rows: (string | number | boolean | null)[][];
  error?: string | null;
  durationMs?: number;
  isLoading?: boolean;
}

export function QueryResults({
  columns,
  rows,
  error,
  durationMs,
  isLoading,
}: QueryResultsProps) {
  if (isLoading) {
    return (
      <div className="flex items-center gap-2 p-4 text-sm text-gray-400">
        <span className="animate-pulse">Executing query...</span>
      </div>
    );
  }

  if (error) {
    return (
      <div className="p-4">
        <div className="bg-red-950/40 border border-red-900/50 rounded p-3 text-sm text-red-300">
          <span className="font-medium text-red-400">Error: </span>
          {error}
        </div>
      </div>
    );
  }

  if (columns.length === 0) {
    return (
      <div className="flex items-center justify-center h-full text-gray-600 text-sm">
        Execute a query to see results
      </div>
    );
  }

  return (
    <div className="flex flex-col h-full">
      {/* Status bar */}
      <div className="flex items-center gap-3 px-3 py-1.5 border-b border-gray-800 text-xs text-gray-500 shrink-0">
        <span>
          {rows.length} row{rows.length !== 1 ? "s" : ""}
        </span>
        {durationMs !== undefined && (
          <span>{durationMs < 1000 ? `${durationMs}ms` : `${(durationMs / 1000).toFixed(2)}s`}</span>
        )}
      </div>

      {/* Results table */}
      <div className="flex-1 overflow-auto">
        <table className="w-full text-xs border-collapse">
          <thead className="sticky top-0 z-10">
            <tr className="bg-gray-900 border-b border-gray-800">
              <th className="text-left py-1.5 px-2 text-gray-500 font-medium w-10 border-r border-gray-800">
                #
              </th>
              {columns.map((col) => (
                <th
                  key={col}
                  className="text-left py-1.5 px-2 text-gray-400 font-medium border-r border-gray-800 last:border-r-0"
                >
                  {col}
                </th>
              ))}
            </tr>
          </thead>
          <tbody>
            {rows.map((row, i) => (
              <tr
                key={i}
                className="border-b border-gray-800/50 hover:bg-gray-900/50"
              >
                <td className="py-1 px-2 text-gray-600 border-r border-gray-800 font-mono">
                  {i + 1}
                </td>
                {row.map((cell, j) => (
                  <td
                    key={j}
                    className="py-1 px-2 text-gray-300 border-r border-gray-800 last:border-r-0 max-w-xs truncate"
                  >
                    {cell === null ? (
                      <span className="text-gray-600 italic">NULL</span>
                    ) : typeof cell === "boolean" ? (
                      <span className={cell ? "text-green-400" : "text-red-400"}>
                        {String(cell)}
                      </span>
                    ) : (
                      String(cell)
                    )}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  );
}
