# WebUI Migration TODO

## Overview
The WebUI is being migrated from legacy HTML pages to a modern Svelte + TypeScript SPA.

**Current Status:**
- ✅ **MIGRATION COMPLETE** - All 8 pages successfully migrated to Svelte SPA
- ✅ Client-side routing implemented in App.svelte
- ✅ API client with TypeScript types for all endpoints
- ✅ Consistent purple gradient header design across all pages
- ✅ Responsive mobile-first design
- ✅ All legacy HTML pages can now be removed

## Pages to Migrate

### High Priority
- [x] **Status/Home Page** (`/html/index.html`) ✅ **COMPLETED**
  - Dashboard with device status
  - Quick links to all sections
  - Real-time connection status (auto-refresh every 3s)

- [x] **System Page** (`/html/system.html`) ✅ **COMPLETED**
  - System uptime and memory statistics
  - CPU usage by task
  - Task information table
  - Auto-refresh every 2 seconds

### Medium Priority
- [x] **Keyer Page** (`/html/keyer.html`) ✅ **COMPLETED**
  - Send CW messages from text input
  - Keyer controls (Send, Abort)
  - Speed adjustment (WPM)
  - Stored messages (F1-F10)
  - Real-time status monitoring (auto-refresh every 500ms)
  - Progress bar for transmission

- [x] **Remote Page** (`/html/remote.html`) ✅ **COMPLETED**
  - CWNet client/server controls (Start/Stop buttons)
  - Connection status monitoring (auto-refresh every 1s)
  - Remote keying status (latency, PTT tail)
  - Configuration display
  - State-based UI (Idle, Connecting, Connected, Error, Listening)

- [x] **Timeline Page** (`/html/timeline.html`) ✅ **COMPLETED**
  - Real-time event timeline visualization with canvas rendering
  - 4-track oscilloscope display (DOT, DASH, OUT, LOGIC)
  - WPM-aware grid alignment (1 dit intervals)
  - Memory window, late release, latch, and squeeze visualization
  - Gap markers with thickness based on gap type
  - Decoded character display aligned to timeline
  - Configurable visualization options
  - Auto-refresh every 100ms for events, 1s for config
  - localStorage configuration persistence

### Low Priority
- [x] **Decoder Page** (`/html/decoder.html`) ✅ **COMPLETED**
  - Adaptive CW decoder with timing analysis
  - Enable/disable toggle with custom styled checkbox
  - Real-time WPM display with color-coded gauge (10-60 WPM range)
  - Current pattern display (dit/dah sequence)
  - Decoded text display with auto-scroll
  - Reset buffer functionality
  - Auto-refresh every 500ms
  - Alert notifications for actions

- [x] **Firmware Update** (`/html/firmware_update.html`) ✅ **COMPLETED**
  - UF2 bootloader firmware update via USB drive method
  - Step-by-step instructions (4 steps)
  - Enter bootloader mode button with confirmation dialog
  - Warning box with important safety notes
  - Info box about configuration preservation
  - Build instructions for custom firmware
  - Troubleshooting section (bootloop, USB issues, update failures)
  - Status notifications (success, error, info)
  - Footer with GitHub link

## Migration Steps for Each Page

1. **Create Svelte component** in `webui/src/pages/`
2. **Identify API endpoints** used by legacy page
3. **Add TypeScript types** for API responses
4. **Implement reactive state management** with stores
5. **Add routing** in `App.svelte`
6. **Test functionality** thoroughly
7. **Update `http_server.cpp`** to route to new page
8. **Remove legacy HTML** file

## Shared Components Needed

- [ ] StatusIndicator (connection, WiFi strength)
- [ ] CommandButton (with confirmation)
- [ ] LogViewer (for decoder, timeline)
- [ ] ProgressBar (for firmware update)
- [ ] NetworkInfo widget
- [ ] DeviceInfo widget

## Architecture Decisions

### Routing
- Consider adding `svelte-spa-router` or similar
- Or continue hybrid approach with separate routes per page

### State Management
- Extend current stores pattern
- Add WebSocket support for real-time updates

### API Client
- Extend `src/lib/api.ts` with new endpoints
- Add type definitions in `src/lib/types.ts`

## Notes
- Keep legacy pages functional during migration
- Test each migrated page on actual device
- Maintain backward compatibility for bookmarks
- Consider adding URL redirects from old paths to new
