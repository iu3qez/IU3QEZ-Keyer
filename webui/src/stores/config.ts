import { writable, derived } from 'svelte/store';
import type { ConfigSchema, DeviceConfig, ParameterMeta } from '../lib/types';

interface DirtyParam {
  oldValue: string | number | boolean;
  newValue: string | number | boolean;
}

interface ConfigState {
  schema: ConfigSchema | null;
  config: DeviceConfig;
  dirtyParams: Map<string, DirtyParam>;
  loading: boolean;
  error: string | null;
  resetRequired: boolean;
}

function createConfigStore() {
  const initialState: ConfigState = {
    schema: null,
    config: {},
    dirtyParams: new Map(),
    loading: false,
    error: null,
    resetRequired: false,
  };

  const { subscribe, set, update } = writable<ConfigState>(initialState);

  return {
    subscribe,

    setSchema: (schema: ConfigSchema) => {
      update((state) => ({ ...state, schema }));
    },

    setConfig: (config: DeviceConfig) => {
      update((state) => ({ ...state, config }));
    },

    setLoading: (loading: boolean) => {
      update((state) => ({ ...state, loading }));
    },

    setError: (error: string | null) => {
      update((state) => ({ ...state, error }));
    },

    markDirty: (
      paramPath: string,
      oldValue: string | number | boolean,
      newValue: string | number | boolean
    ) => {
      update((state) => {
        const dirtyParams = new Map(state.dirtyParams);
        dirtyParams.set(paramPath, { oldValue, newValue });

        // Check if any dirty param requires reset
        let resetRequired = false;
        if (state.schema) {
          for (const [path] of dirtyParams) {
            const [subsystem, paramName] = path.split('.');
            const paramMeta = state.schema.subsystems[subsystem]?.[paramName];
            if (paramMeta?.reset_required) {
              resetRequired = true;
              break;
            }
          }
        }

        return { ...state, dirtyParams, resetRequired };
      });
    },

    clearDirty: (paramPath?: string) => {
      update((state) => {
        if (paramPath) {
          const dirtyParams = new Map(state.dirtyParams);
          dirtyParams.delete(paramPath);

          // Recalculate resetRequired
          let resetRequired = false;
          if (state.schema) {
            for (const [path] of dirtyParams) {
              const [subsystem, paramName] = path.split('.');
              const paramMeta = state.schema.subsystems[subsystem]?.[paramName];
              if (paramMeta?.reset_required) {
                resetRequired = true;
                break;
              }
            }
          }

          return { ...state, dirtyParams, resetRequired };
        } else {
          return { ...state, dirtyParams: new Map(), resetRequired: false };
        }
      });
    },

    reset: () => {
      set(initialState);
    },
  };
}

export const configStore = createConfigStore();

// Derived store: check if any dirty param requires reset
export const resetRequired = derived(configStore, ($config) => {
  if (!$config.schema) return false;

  for (const [paramPath] of $config.dirtyParams) {
    const [subsystem, paramName] = paramPath.split('.');
    const paramMeta = $config.schema.subsystems[subsystem]?.[paramName];
    if (paramMeta?.reset_required) {
      return true;
    }
  }

  return false;
});

// Derived store: count dirty params requiring reset
export const resetRequiredCount = derived(configStore, ($config) => {
  if (!$config.schema) return 0;

  let count = 0;
  for (const [paramPath] of $config.dirtyParams) {
    const [subsystem, paramName] = paramPath.split('.');
    const paramMeta = $config.schema.subsystems[subsystem]?.[paramName];
    if (paramMeta?.reset_required) {
      count++;
    }
  }

  return count;
});
