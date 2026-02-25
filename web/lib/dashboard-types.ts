export interface ServerStatus {
  version: string;
  uptime_seconds: number;
  active_connections: number;
  memory_usage_mb: number;
}

export interface BufferPoolStats {
  total_pages: number;
  dirty_pages: number;
  hit_count: number;
  miss_count: number;
  eviction_count: number;
  hit_rate: number;
}

export interface EmbeddingPipelineStats {
  queue_depth: number;
  processing_rate: number;
  total_processed: number;
  total_failures: number;
  avg_latency_ms: number;
}

export interface SlowQuery {
  query: string;
  execution_time_ms: number;
  timestamp: string;
  database: string;
}

export interface DashboardData {
  server: ServerStatus;
  buffer_pool: BufferPoolStats;
  embedding_pipeline: EmbeddingPipelineStats;
  slow_queries: SlowQuery[];
}

export interface HitRateDataPoint {
  time: string;
  hit_rate: number;
}
