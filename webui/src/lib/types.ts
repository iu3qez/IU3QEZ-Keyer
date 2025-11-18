// Parameter types matching backend
export type ParameterType =
  | 'INT32' | 'UINT32' | 'UINT16' | 'UINT8' | 'INT8'
  | 'BOOL' | 'STRING' | 'FLOAT' | 'ENUM';

export type ParameterCategory = 'normal' | 'advanced';

export interface EnumValue {
  name: string;
  value: string;
  description: string;
}

export interface ParameterMeta {
  name: string;
  type: ParameterType;
  min?: number;
  max?: number;
  description: string;
  unit: string;
  reset_required: boolean;
  category: ParameterCategory;
  enum_values?: EnumValue[];
  widget_hint?: string;
}

export interface SubsystemSchema {
  [paramName: string]: ParameterMeta;
}

export interface ConfigSchema {
  subsystems: {
    [subsystemName: string]: SubsystemSchema;
  };
}

export interface DeviceConfig {
  [subsystem: string]: {
    [param: string]: string | number | boolean;
  };
}

export interface ParameterUpdateRequest {
  param: string;  // e.g., "keying.wpm"
  value: string | number | boolean;
}

export interface ParameterUpdateResponse {
  success: boolean;
  requires_reset: boolean;
  message: string;
}

export interface SaveConfigResponse {
  success: boolean;
  message: string;
}

export interface ValidationError {
  param: string;
  error: string;
}

// Status API types
export interface DeviceStatus {
  mode: string;      // e.g., "STA", "AP", "STA+AP"
  ip: string;        // IP address
  ready: boolean;    // Connection status
}

// System Stats API types
export interface SystemUptime {
  hours: number;
  minutes: number;
  seconds: number;
  total_seconds: number;
}

export interface HeapInfo {
  free_bytes: number;
  minimum_free_bytes: number;
  total_bytes: number;
  largest_free_block: number;
  fragmentation_percent: number;
}

export interface TaskInfo {
  name: string;
  state: string;        // "Running", "Blocked", "Suspended", etc.
  priority: number;
  stack_hwm: number;    // High water mark (minimum free stack)
  task_number: number;
  runtime_us?: number;  // Runtime in microseconds
  cpu_percent?: number; // CPU usage percentage
}

export interface SystemStats {
  uptime: SystemUptime;
  heap: HeapInfo;
  tasks: TaskInfo[];
}

// Keyer API types
export interface KeyerStatus {
  state: string;      // "idle", "sending", etc.
  wpm: number;
  progress: number;
  total: number;
}

// Remote CW API types
export interface RemoteClientStatus {
  state: number; // 0=Idle, 1=Resolving, 2=Connecting, 3=Handshake, 4=Connected, 5=Error
  server_host?: string;
  server_port?: number;
  latency_ms: number;
  ptt_tail_base_ms: number;
}

export interface RemoteServerStatus {
  state: number; // 0=Idle, 1=Listening, 2=Handshake, 3=Connected, 4=Error
  listen_port: number;
  client_ip?: string;
  ptt_tail_ms: number;
}

export interface RemoteConfig {
  client_enabled: boolean;
  client_server_host: string;
  client_server_port: number;
  client_auto_reconnect: boolean;
  server_enabled: boolean;
  server_listen_port: number;
}

export interface RemoteStatus {
  client: RemoteClientStatus;
  server: RemoteServerStatus;
  config: RemoteConfig;
}

// Timeline API types
export interface TimelineEvent {
  timestamp_us: number;
  type: string; // "paddle_edge", "keying", "memory_window", "latch", "squeeze", "gap_marker", "decoded_char"
  arg0: number;
  arg1: number;
}

export interface TimelineEventsResponse {
  events: TimelineEvent[];
}

export interface TimelineConfig {
  wpm: number;
  wpm_source: string; // "keying_config", "decoder", etc.
}

// Decoder API types
export interface DecoderStatus {
  enabled: boolean;
  wpm: number;
  pattern: string;
  text: string;
}
