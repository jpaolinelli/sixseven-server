/**
 * Query history persistence via localStorage.
 *
 * Stores the last MAX_HISTORY queries with timestamps.
 * Provides search/filter and CRUD operations.
 */

const STORAGE_KEY = "sixseven-query-history";
const MAX_HISTORY = 100;

export interface HistoryEntry {
  id: string;
  sql: string;
  database: string;
  timestamp: number;
  durationMs?: number;
  error?: string;
}

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

/** Load the full history array from localStorage. */
export function loadHistory(): HistoryEntry[] {
  if (typeof window === "undefined") return [];
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed as HistoryEntry[];
  } catch {
    return [];
  }
}

/** Save the history array to localStorage (internal). */
function saveHistory(entries: HistoryEntry[]): void {
  if (typeof window === "undefined") return;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(entries));
  } catch {
    // localStorage quota exceeded — silently drop oldest entries
    const trimmed = entries.slice(0, Math.floor(entries.length / 2));
    try {
      localStorage.setItem(STORAGE_KEY, JSON.stringify(trimmed));
    } catch {
      // Give up
    }
  }
}

/** Add a query to history. Deduplicates consecutive identical queries. */
export function addToHistory(
  sql: string,
  database: string,
  durationMs?: number,
  error?: string
): HistoryEntry {
  const entries = loadHistory();
  const trimmedSql = sql.trim();

  // Don't add empty queries
  if (!trimmedSql) {
    const entry: HistoryEntry = {
      id: generateId(),
      sql: trimmedSql,
      database,
      timestamp: Date.now(),
      durationMs,
      error,
    };
    return entry;
  }

  // Deduplicate: skip if the most recent entry is the same SQL
  if (
    entries.length > 0 &&
    entries[0].sql.trim() === trimmedSql &&
    entries[0].database === database
  ) {
    // Update the existing entry with new timing
    entries[0].timestamp = Date.now();
    entries[0].durationMs = durationMs;
    entries[0].error = error;
    saveHistory(entries);
    return entries[0];
  }

  const entry: HistoryEntry = {
    id: generateId(),
    sql: trimmedSql,
    database,
    timestamp: Date.now(),
    durationMs,
    error,
  };

  // Prepend and cap at MAX_HISTORY
  const updated = [entry, ...entries].slice(0, MAX_HISTORY);
  saveHistory(updated);
  return entry;
}

/** Search history by SQL text (case-insensitive). */
export function searchHistory(query: string): HistoryEntry[] {
  const entries = loadHistory();
  if (!query.trim()) return entries;
  const lower = query.toLowerCase();
  return entries.filter((e) => e.sql.toLowerCase().includes(lower));
}

/** Clear all history. */
export function clearHistory(): void {
  saveHistory([]);
}

/** Delete a single history entry. */
export function deleteHistoryEntry(id: string): void {
  const entries = loadHistory();
  saveHistory(entries.filter((e) => e.id !== id));
}
