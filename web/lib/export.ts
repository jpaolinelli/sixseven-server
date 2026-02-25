/**
 * Export utilities — CSV download, JSON download, and clipboard copy
 * for query result data.
 */

type CellValue = string | number | boolean | null;

export function escapeCSV(value: CellValue): string {
  if (value === null) return "";
  const str = String(value);
  if (str.includes(",") || str.includes('"') || str.includes("\n")) {
    return `"${str.replace(/"/g, '""')}"`;
  }
  return str;
}

export function buildCSV(columns: string[], rows: CellValue[][]): string {
  const header = columns.map(escapeCSV).join(",");
  const body = rows.map((row) => row.map(escapeCSV).join(",")).join("\n");
  return header + "\n" + body;
}

export function buildJSON(columns: string[], rows: CellValue[][]): string {
  const objects = rows.map((row) => {
    const obj: Record<string, CellValue> = {};
    columns.forEach((col, i) => {
      obj[col] = row[i] ?? null;
    });
    return obj;
  });
  return JSON.stringify(objects, null, 2);
}

function downloadBlob(content: string, filename: string, mimeType: string) {
  const blob = new Blob([content], { type: mimeType });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

export function exportCSV(columns: string[], rows: CellValue[][]) {
  const csv = buildCSV(columns, rows);
  downloadBlob(csv, "query-results.csv", "text/csv;charset=utf-8");
}

export function exportJSON(columns: string[], rows: CellValue[][]) {
  const json = buildJSON(columns, rows);
  downloadBlob(json, "query-results.json", "application/json;charset=utf-8");
}

export async function copyToClipboard(
  columns: string[],
  rows: CellValue[][]
): Promise<boolean> {
  const tsv =
    columns.join("\t") +
    "\n" +
    rows.map((row) => row.map((v) => (v === null ? "" : String(v))).join("\t")).join("\n");
  try {
    await navigator.clipboard.writeText(tsv);
    return true;
  } catch {
    return false;
  }
}
