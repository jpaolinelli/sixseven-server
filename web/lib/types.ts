export interface DatabaseInfo {
  name: string;
  isSystem: boolean;
}

export interface ColumnInfo {
  name: string;
  type: string;
  nullable: boolean;
}

export interface IndexInfo {
  name: string;
  tableName: string;
  columns: string;
  type: string;
  unique: boolean;
}

export interface EdgeTypeInfo {
  name: string;
  sourceTable: string;
  targetTable: string;
}

export interface EmbeddingInfo {
  tableName: string;
  columnName: string;
  dimension: number;
  sourceExpr: string;
  provider: string;
}

export interface TableInfo {
  name: string;
  columns: ColumnInfo[];
  indexes: IndexInfo[];
  embeddings: EmbeddingInfo[];
}

export interface DatabaseSchema {
  database: DatabaseInfo;
  tables: TableInfo[];
  edgeTypes: EdgeTypeInfo[];
}

export interface QueryResult {
  columns: string[];
  rows: (string | number | boolean | null)[][];
}

export type SelectedItem =
  | { kind: "database"; database: string }
  | { kind: "table"; database: string; table: string }
  | { kind: "column"; database: string; table: string; column: ColumnInfo }
  | { kind: "index"; database: string; index: IndexInfo }
  | { kind: "edgeType"; database: string; edgeType: EdgeTypeInfo }
  | { kind: "embedding"; database: string; embedding: EmbeddingInfo };
