/**
 * SixSevenDB SQL language support for CodeMirror 6.
 *
 * Extends the standard SQL dialect with SixSevenDB-specific keywords
 * (TRAVERSE, NEAREST, MATCH, LINK, UNLINK, EMBEDDING, REEMBED)
 * and provides autocomplete from catalog metadata.
 */

import {
  SQLDialect,
  sql,
  type SQLConfig,
} from "@codemirror/lang-sql";
import type {
  Completion,
  CompletionContext,
  CompletionResult,
} from "@codemirror/autocomplete";
import type { DatabaseSchema } from "./types";

// ----- SixSevenDB-specific keywords -----

const SIXSEVEN_KEYWORDS = [
  "TRAVERSE",
  "NEAREST",
  "MATCH",
  "LINK",
  "UNLINK",
  "EMBEDDING",
  "REEMBED",
  "EDGE",
  "VERTEX",
  "PATH",
  "SHORTEST",
  "NEIGHBORS",
  "DEPTH",
  "BREADTH",
  "HOPS",
];

// Standard SQL keywords (subset for autocomplete)
const SQL_KEYWORDS = [
  "SELECT",
  "FROM",
  "WHERE",
  "INSERT",
  "INTO",
  "VALUES",
  "UPDATE",
  "SET",
  "DELETE",
  "CREATE",
  "TABLE",
  "DROP",
  "ALTER",
  "INDEX",
  "ON",
  "AND",
  "OR",
  "NOT",
  "IN",
  "IS",
  "NULL",
  "AS",
  "JOIN",
  "LEFT",
  "RIGHT",
  "INNER",
  "OUTER",
  "CROSS",
  "GROUP",
  "BY",
  "ORDER",
  "ASC",
  "DESC",
  "HAVING",
  "LIMIT",
  "OFFSET",
  "DISTINCT",
  "COUNT",
  "SUM",
  "AVG",
  "MIN",
  "MAX",
  "BETWEEN",
  "LIKE",
  "EXISTS",
  "CASE",
  "WHEN",
  "THEN",
  "ELSE",
  "END",
  "UNION",
  "ALL",
  "PRIMARY",
  "KEY",
  "FOREIGN",
  "REFERENCES",
  "UNIQUE",
  "CHECK",
  "DEFAULT",
  "BEGIN",
  "COMMIT",
  "ROLLBACK",
  "TRANSACTION",
  "SHOW",
  "DATABASES",
  "TABLES",
  "COLUMNS",
  "INDEXES",
  "TRUE",
  "FALSE",
];

// Custom dialect that adds SixSevenDB keywords to SQL
const sixsevenDialect = SQLDialect.define({
  keywords:
    SQL_KEYWORDS.join(" ").toLowerCase() +
    " " +
    SIXSEVEN_KEYWORDS.join(" ").toLowerCase(),
  types:
    "int8 int16 int32 int64 uint8 uint16 uint32 uint64 float32 float64 decimal bool string blob date time timestamp interval point json uuid embedding text integer bigint smallint real double varchar char boolean",
  builtin:
    "current_timestamp current_date current_time coalesce nullif cast",
  operatorChars: "+-*/<>=~!@#%^&|?",
  specialVar: "",
  identifierQuotes: '"',
  hashComments: false,
  slashComments: true,
  backslashEscapes: true,
});

// ----- Schema-aware autocomplete -----

export interface SchemaCompletionData {
  tables: { name: string; columns: { name: string; type: string }[] }[];
  edgeTypes: string[];
}

/**
 * Extract the table name from the FROM clause preceding the cursor.
 * Handles simple cases like `SELECT ... FROM tablename WHERE ...`
 */
function extractTableFromContext(docText: string, pos: number): string | null {
  const textBefore = docText.slice(0, pos).toUpperCase();
  // Look backwards for FROM <table>
  const fromMatch = textBefore.match(
    /FROM\s+[""]?(\w+)[""]?(?:\s+(?:AS\s+)?\w+)?\s*$/i
  );
  if (fromMatch) return fromMatch[1].toLowerCase();

  // Also try: FROM <table> WHERE ... (cursor somewhere after)
  const fromMatchEarlier = textBefore.match(
    /FROM\s+[""]?(\w+)[""]?/i
  );
  if (fromMatchEarlier) return fromMatchEarlier[1].toLowerCase();

  return null;
}

/**
 * Build a CodeMirror autocomplete source from SixSevenDB schema data.
 */
export function sixsevenCompletionSource(schemaData: SchemaCompletionData) {
  return (context: CompletionContext): CompletionResult | null => {
    // Check if we're after a dot (table.column completion)
    const dotMatch = context.matchBefore(/\w+\.\w*/);
    if (dotMatch) {
      const dotPos = dotMatch.text.indexOf(".");
      const tableName = dotMatch.text.slice(0, dotPos).toLowerCase();
      const table = schemaData.tables.find(
        (t) => t.name.toLowerCase() === tableName
      );
      if (!table) return null;

      const options: Completion[] = table.columns.map((col) => ({
        label: col.name,
        type: "property",
        detail: col.type,
      }));

      return {
        from: dotMatch.from + dotPos + 1,
        options,
        validFor: /^\w*$/,
      };
    }

    // General word completion
    const wordMatch = context.matchBefore(/\w+/);
    if (!wordMatch && !context.explicit) return null;

    const from = wordMatch ? wordMatch.from : context.pos;
    const options: Completion[] = [];

    // SQL + SixSevenDB keywords
    const allKeywords = [...SQL_KEYWORDS, ...SIXSEVEN_KEYWORDS];
    for (const kw of allKeywords) {
      options.push({
        label: kw,
        type: "keyword",
        boost: SIXSEVEN_KEYWORDS.includes(kw) ? 1 : 0,
      });
    }

    // Table names
    for (const table of schemaData.tables) {
      options.push({
        label: table.name,
        type: "type",
        detail: "table",
      });
    }

    // Edge type names
    for (const et of schemaData.edgeTypes) {
      options.push({
        label: et,
        type: "type",
        detail: "edge type",
      });
    }

    // Context-aware column completion: if we can detect a FROM table, add its columns
    const docText = context.state.doc.toString();
    const contextTable = extractTableFromContext(docText, context.pos);
    if (contextTable) {
      const table = schemaData.tables.find(
        (t) => t.name.toLowerCase() === contextTable
      );
      if (table) {
        for (const col of table.columns) {
          options.push({
            label: col.name,
            type: "property",
            detail: `${table.name}.${col.type}`,
          });
        }
      }
    }

    return {
      from,
      options,
      validFor: /^\w*$/,
    };
  };
}

/**
 * Create the SixSevenDB SQL language extension for CodeMirror.
 */
export function sixsevenSQL(config?: Partial<SQLConfig>) {
  return sql({
    dialect: sixsevenDialect,
    upperCaseKeywords: true,
    ...config,
  });
}
