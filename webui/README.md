# QRS2HST Keyer WebUI

Modern web interface for configuring the QRS2HST keyer, built with Svelte + TypeScript.

## Development

### Prerequisites
- Node.js 18+ and npm

### Setup
```bash
cd webui
npm install
```

### Development Server
```bash
npm run dev
```

Navigate to http://localhost:5173. The dev server proxies API requests to `http://192.168.4.1` (ESP32 device).

To proxy to a different device IP:
```bash
# Edit vite.config.ts, change proxy target
```

### Testing
```bash
npm test              # Run tests
npm run test:ui       # Visual test UI
```

### Type Checking
```bash
npm run check
```

## Production Build

### Build for Firmware
```bash
npm run build
```

Output in `dist/` directory (~26KB uncompressed, ~10KB gzipped).

### Embed in Firmware
```bash
cd ..
python3 scripts/webui/embed_assets.py --source webui/dist --output build/esp-idf/ui/generated/web_assets_data.inc --gzip
idf.py build
```

## Architecture

- **Framework**: Svelte 4 + TypeScript 5
- **Build**: Vite 5 with esbuild
- **State**: Svelte stores
- **Testing**: Vitest + @testing-library/svelte

## File Structure

```
src/
├── components/       # Reusable Svelte components
│   ├── ParameterInput.svelte
│   ├── ParameterSection.svelte
│   └── StatusBanner.svelte
├── pages/            # Page components
│   └── Config.svelte
├── stores/           # State management
│   └── config.ts
├── lib/              # Utilities
│   ├── api.ts
│   ├── types.ts
│   └── validators.ts
├── App.svelte        # Root component
└── main.ts           # Entry point
```

## API Integration

WebUI uses REST API:
- `GET /api/config/schema` - Parameter metadata
- `GET /api/config` - Current configuration
- `POST /api/parameter` - Update single parameter
- `POST /api/config/save` - Persist to NVS
- `POST /api/config/save?reboot=true` - Save and reboot

## Phase 1 Status

✅ Completed:
- Svelte + TypeScript setup
- API client with type safety
- Parameter categorization (normal/advanced)
- Configuration page with tabs
- Validation and visual feedback
- Status banner for reset-required parameters
- Build pipeline

❌ Not in Phase 1:
- WebSocket (Phase 2)
- Preset editor V0-V9 (Phase 3)
- Captive portal (Phase 4)
