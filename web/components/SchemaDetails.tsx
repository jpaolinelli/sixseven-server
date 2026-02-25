"use client";

import { useEffect, useState } from "react";
import { fetchSampleData, quoteIdent } from "@/lib/schema-utils";
import { useConnection } from "@/lib/ConnectionContext";
import type { SelectedItem } from "@/lib/types";

interface SchemaDetailsProps {
  item: SelectedItem;
}

export function SchemaDetails({ item }: SchemaDetailsProps) {
  switch (item.kind) {
    case "database":
      return <DatabaseDetails database={item.database} />;
    case "table":
      return (
        <TableDetails database={item.database} table={item.table} />
      );
    case "column":
      return (
        <ColumnDetails
          table={item.table}
          column={item.column}
        />
      );
    case "index":
      return <IndexDetails index={item.index} />;
    case "edgeType":
      return <EdgeTypeDetails edgeType={item.edgeType} />;
    case "embedding":
      return (
        <EmbeddingDetails
          database={item.database}
          embedding={item.embedding}
        />
      );
  }
}

function SectionHeader({ title }: { title: string }) {
  return (
    <h2 className="text-sm font-semibold text-gray-100 mb-3">{title}</h2>
  );
}

function DetailRow({ label, value }: { label: string; value: string }) {
  return (
    <div className="flex gap-3 py-1 text-sm">
      <span className="text-gray-500 w-32 shrink-0">{label}</span>
      <span className="text-gray-300">{value}</span>
    </div>
  );
}

function DatabaseDetails({ database }: { database: string }) {
  return (
    <div className="p-6">
      <SectionHeader title={`Database: ${database}`} />
      <DetailRow label="Name" value={database} />
    </div>
  );
}

function TableDetails({
  database,
  table,
}: {
  database: string;
  table: string;
}) {
  const { connectionParams } = useConnection();
  const [sampleData, setSampleData] = useState<{
    columns: string[];
    rows: unknown[][];
  } | null>(null);
  const [loading, setLoading] = useState(false);

  useEffect(() => {
    let cancelled = false;
    setLoading(true);
    setSampleData(null);
    fetchSampleData(database, table, connectionParams)
      .then((data) => {
        if (!cancelled) setSampleData(data);
      })
      .catch(() => {
        // Ignore — will show empty state.
      })
      .finally(() => {
        if (!cancelled) setLoading(false);
      });
    return () => {
      cancelled = true;
    };
  }, [database, table, connectionParams]);

  return (
    <div className="p-6">
      <SectionHeader title={`Table: ${table}`} />
      <DetailRow label="Database" value={database} />

      {/* Sample Data */}
      <div className="mt-6">
        <h3 className="text-xs font-semibold text-gray-400 mb-2 uppercase tracking-wide">
          Sample Data (first 10 rows)
        </h3>
        {loading && (
          <p className="text-xs text-gray-600">Loading...</p>
        )}
        {sampleData && sampleData.rows.length > 0 && (
          <div className="overflow-x-auto">
            <table className="w-full text-xs">
              <thead>
                <tr className="border-b border-gray-800">
                  {sampleData.columns.map((col) => (
                    <th
                      key={col}
                      className="text-left py-1.5 px-2 text-gray-400 font-medium"
                    >
                      {col}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {sampleData.rows.map((row, i) => (
                  <tr
                    key={i}
                    className="border-b border-gray-800/50 hover:bg-gray-900/50"
                  >
                    {row.map((cell, j) => (
                      <td key={j} className="py-1 px-2 text-gray-300">
                        {cell === null ? (
                          <span className="text-gray-600 italic">NULL</span>
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
        )}
        {sampleData && sampleData.rows.length === 0 && (
          <p className="text-xs text-gray-600">No rows</p>
        )}
      </div>
    </div>
  );
}

function ColumnDetails({
  table,
  column,
}: {
  table: string;
  column: { name: string; type: string; nullable: boolean };
}) {
  return (
    <div className="p-6">
      <SectionHeader title={`Column: ${column.name}`} />
      <DetailRow label="Table" value={table} />
      <DetailRow label="Type" value={column.type} />
      <DetailRow label="Nullable" value={column.nullable ? "Yes" : "No"} />
    </div>
  );
}

function IndexDetails({
  index,
}: {
  index: {
    name: string;
    tableName: string;
    columns: string;
    type: string;
    unique: boolean;
  };
}) {
  return (
    <div className="p-6">
      <SectionHeader title={`Index: ${index.name}`} />
      <DetailRow label="Table" value={index.tableName} />
      <DetailRow label="Columns" value={index.columns} />
      <DetailRow label="Type" value={index.type} />
      <DetailRow label="Unique" value={index.unique ? "Yes" : "No"} />
    </div>
  );
}

function EdgeTypeDetails({
  edgeType,
}: {
  edgeType: { name: string; sourceTable: string; targetTable: string };
}) {
  return (
    <div className="p-6">
      <SectionHeader title={`Edge Type: ${edgeType.name}`} />
      <DetailRow label="Source Table" value={edgeType.sourceTable} />
      <DetailRow label="Target Table" value={edgeType.targetTable} />
      <DetailRow
        label="Direction"
        value={`${edgeType.sourceTable} → ${edgeType.targetTable}`}
      />
    </div>
  );
}

function EmbeddingDetails({
  database,
  embedding,
}: {
  database: string;
  embedding: {
    tableName: string;
    columnName: string;
    dimension: number;
    sourceExpr: string;
    provider: string;
  };
}) {
  const { connectionParams } = useConnection();
  const [populated, setPopulated] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    // Fetch % populated: count non-null embedding values vs total rows.
    const sql = `SELECT COUNT(${quoteIdent(embedding.columnName)}) AS populated, COUNT(*) AS total FROM ${quoteIdent(embedding.tableName)}`;
    fetch("/api/query", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ sql, database, connection: connectionParams }),
    })
      .then((res) => res.json())
      .then((data) => {
        if (!cancelled && data.rows?.[0]) {
          const pop = Number(data.rows[0][0]);
          const total = Number(data.rows[0][1]);
          setPopulated(
            total > 0 ? `${Math.round((pop / total) * 100)}%` : "N/A (empty)"
          );
        }
      })
      .catch(() => {
        if (!cancelled) setPopulated("N/A");
      });
    return () => {
      cancelled = true;
    };
  }, [database, embedding.tableName, embedding.columnName, connectionParams]);

  return (
    <div className="p-6">
      <SectionHeader
        title={`Embedding: ${embedding.tableName}.${embedding.columnName}`}
      />
      <DetailRow label="Table" value={embedding.tableName} />
      <DetailRow label="Column" value={embedding.columnName} />
      <DetailRow label="Dimension" value={String(embedding.dimension)} />
      <DetailRow label="Source Expression" value={embedding.sourceExpr} />
      <DetailRow label="Provider" value={embedding.provider} />
      <DetailRow
        label="% Populated"
        value={populated ?? "Loading..."}
      />
    </div>
  );
}
