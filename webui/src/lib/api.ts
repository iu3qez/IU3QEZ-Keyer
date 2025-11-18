import type {
  ConfigSchema,
  DeviceConfig,
  ParameterUpdateRequest,
  ParameterUpdateResponse,
  SaveConfigResponse,
  ParameterMeta,
  ParameterType,
  ParameterCategory,
  DeviceStatus,
  SystemStats,
  KeyerStatus,
  RemoteStatus,
  TimelineEventsResponse,
  TimelineConfig,
  DecoderStatus,
} from './types';

// Backend API schema format (flat parameter list)
interface BackendParameter {
  name: string;
  type: string; // "int", "bool", "string", "float", "enum"
  widget: string;
  min?: number;
  max?: number;
  min_length?: number;
  max_length?: number;
  precision?: number;
  unit?: string;
  description: string;
  values?: Array<{ name: string; description: string }>;
  true?: string;
  false?: string;
}

interface BackendSchema {
  parameters: BackendParameter[];
}

export class ApiClient {
  private baseUrl: string;

  constructor(baseUrl: string = '') {
    this.baseUrl = baseUrl;
  }

  private transformSchema(backendSchema: BackendSchema): ConfigSchema {
    const subsystems: ConfigSchema['subsystems'] = {};

    for (const param of backendSchema.parameters) {
      // Parse subsystem.paramName
      const parts = param.name.split('.');
      if (parts.length !== 2) {
        console.warn(`Invalid parameter name format: ${param.name}`);
        continue;
      }

      const [subsystemName, paramName] = parts;

      // Create subsystem if it doesn't exist
      if (!subsystems[subsystemName]) {
        subsystems[subsystemName] = {};
      }

      // Map backend type to frontend type
      let paramType: ParameterType;
      switch (param.type.toLowerCase()) {
        case 'int':
          paramType = 'INT32';
          break;
        case 'bool':
          paramType = 'BOOL';
          break;
        case 'string':
          paramType = 'STRING';
          break;
        case 'float':
          paramType = 'FLOAT';
          break;
        case 'enum':
          paramType = 'ENUM';
          break;
        default:
          console.warn(`Unknown parameter type: ${param.type}`);
          paramType = 'STRING';
      }

      // Build ParameterMeta
      const meta: ParameterMeta = {
        name: paramName,
        type: paramType,
        description: param.description,
        unit: param.unit || '',
        reset_required: false, // Backend doesn't provide this, default to false
        category: (param.category as ParameterCategory) || 'normal', // Use backend category
        widget_hint: param.widget, // Pass the backend widget hint
      };

      // Add type-specific fields
      if (param.min !== undefined) meta.min = param.min;
      if (param.max !== undefined) meta.max = param.max;

      // Map enum values
      if (paramType === 'ENUM' && param.values) {
        meta.enum_values = param.values.map((v) => ({
          name: v.name,
          description: v.description,
        }));
      }

      subsystems[subsystemName][paramName] = meta;
    }

    return { subsystems };
  }

  async getSchema(): Promise<ConfigSchema> {
    const response = await fetch(`${this.baseUrl}/api/config/schema`);
    if (!response.ok) {
      throw new Error(`Failed to fetch schema: ${response.statusText}`);
    }
    const backendSchema: BackendSchema = await response.json();
    return this.transformSchema(backendSchema);
  }

  async getConfig(subsystem?: string): Promise<DeviceConfig> {
    const url = subsystem
      ? `${this.baseUrl}/api/config?subsystem=${subsystem}`
      : `${this.baseUrl}/api/config`;

    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`Failed to fetch config: ${response.statusText}`);
    }
    return response.json();
  }

  async setParameter(request: ParameterUpdateRequest): Promise<ParameterUpdateResponse> {
    const response = await fetch(`${this.baseUrl}/api/parameter`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(request),
    });

    if (!response.ok) {
      throw new Error(`Failed to set parameter: ${response.statusText}`);
    }
    return response.json();
  }

  async saveConfig(): Promise<SaveConfigResponse> {
    const response = await fetch(`${this.baseUrl}/api/config/save`, {
      method: 'POST',
    });

    if (!response.ok) {
      throw new Error(`Failed to save config: ${response.statusText}`);
    }
    return response.json();
  }

  async reboot(): Promise<void> {
    const response = await fetch(`${this.baseUrl}/api/config/save?reboot=true`, {
      method: 'POST',
    });

    if (!response.ok) {
      throw new Error(`Failed to reboot: ${response.statusText}`);
    }
  }

  async getStatus(): Promise<DeviceStatus> {
    const response = await fetch(`${this.baseUrl}/api/status`);
    if (!response.ok) {
      throw new Error(`Failed to fetch status: ${response.statusText}`);
    }
    return response.json();
  }

  async getSystemStats(): Promise<SystemStats> {
    const response = await fetch(`${this.baseUrl}/api/system/stats`);
    if (!response.ok) {
      throw new Error(`Failed to fetch system stats: ${response.statusText}`);
    }
    return response.json();
  }

  async getKeyerStatus(): Promise<KeyerStatus> {
    const response = await fetch(`${this.baseUrl}/api/keyer/status`);
    if (!response.ok) {
      throw new Error(`Failed to fetch keyer status: ${response.statusText}`);
    }
    return response.json();
  }

  async sendText(text: string, wpm: number): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/keyer/send`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text, wpm }),
    });
    if (!response.ok) {
      throw new Error(`Failed to send text: ${response.statusText}`);
    }
    return response.json();
  }

  async sendMessage(messageNum: number): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/keyer/message`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ message: messageNum }),
    });
    if (!response.ok) {
      throw new Error(`Failed to send message: ${response.statusText}`);
    }
    return response.json();
  }

  async abortTransmission(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/keyer/abort`, {
      method: 'POST',
    });
    if (!response.ok) {
      throw new Error(`Failed to abort: ${response.statusText}`);
    }
    return response.json();
  }

  async getRemoteStatus(): Promise<RemoteStatus> {
    const response = await fetch(`${this.baseUrl}/api/remote/status`);
    if (!response.ok) {
      throw new Error(`Failed to fetch remote status: ${response.statusText}`);
    }
    return response.json();
  }

  async startRemoteClient(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/remote/client/start`, {
      method: 'POST',
    });
    if (!response.ok) {
      throw new Error(`Failed to start client: ${response.statusText}`);
    }
    return response.json();
  }

  async stopRemoteClient(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/remote/client/stop`, {
      method: 'POST',
    });
    if (!response.ok) {
      throw new Error(`Failed to stop client: ${response.statusText}`);
    }
    return response.json();
  }

  async startRemoteServer(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/remote/server/start`, {
      method: 'POST',
    });
    if (!response.ok) {
      throw new Error(`Failed to start server: ${response.statusText}`);
    }
    return response.json();
  }

  async stopRemoteServer(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/remote/server/stop`, {
      method: 'POST',
    });
    if (!response.ok) {
      throw new Error(`Failed to stop server: ${response.statusText}`);
    }
    return response.json();
  }

  async getTimelineEvents(
    since: number,
    limit: number = 100
  ): Promise<TimelineEventsResponse> {
    const response = await fetch(
      `${this.baseUrl}/api/timeline/events?since=${since}&limit=${limit}`
    );
    if (!response.ok) {
      throw new Error(
        `Failed to fetch timeline events: ${response.statusText}`
      );
    }
    return response.json();
  }

  async getTimelineConfig(): Promise<TimelineConfig> {
    const response = await fetch(`${this.baseUrl}/api/timeline/config`);
    if (!response.ok) {
      throw new Error(
        `Failed to fetch timeline config: ${response.statusText}`
      );
    }
    return response.json();
  }

  async getDecoderStatus(): Promise<DecoderStatus> {
    const response = await fetch(`${this.baseUrl}/api/decoder/status`);
    if (!response.ok) {
      throw new Error(
        `Failed to fetch decoder status: ${response.statusText}`
      );
    }
    return response.json();
  }

  async setDecoderEnabled(enabled: boolean): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/decoder/enable`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled }),
    });
    if (!response.ok) {
      throw new Error(`Failed to set decoder state: ${response.statusText}`);
    }
    return response.json();
  }

  async enterBootloader(): Promise<{ message: string }> {
    const response = await fetch(`${this.baseUrl}/api/enter-bootloader`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({}),
    });
    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(errorText || response.statusText);
    }
    return response.json();
  }
}

// Default instance using relative URLs (same origin)
export const api = new ApiClient();
