<script lang="ts">
  import { onMount } from 'svelte';
  import Home from './pages/Home.svelte';
  import Config from './pages/Config.svelte';
  import System from './pages/System.svelte';
  import Keyer from './pages/Keyer.svelte';
  import Remote from './pages/Remote.svelte';
  import Timeline from './pages/Timeline.svelte';
  import Decoder from './pages/Decoder.svelte';
  import FirmwareUpdate from './pages/FirmwareUpdate.svelte';

  let currentPage: string = '';

  onMount(() => {
    // Simple client-side routing based on URL path
    currentPage = window.location.pathname;
  });
</script>

{#if currentPage === '/config'}
  <Config />
{:else if currentPage === '/system'}
  <System />
{:else if currentPage === '/keyer'}
  <Keyer />
{:else if currentPage === '/remote'}
  <Remote />
{:else if currentPage === '/timeline'}
  <Timeline />
{:else if currentPage === '/decoder'}
  <Decoder />
{:else if currentPage === '/firmware'}
  <FirmwareUpdate />
{:else}
  <Home />
{/if}

<style>
  :global(*) {
    box-sizing: border-box;
    margin: 0;
    padding: 0;
  }

  :global(body) {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Arial, sans-serif;
    background: #f5f7fa;
    color: #2c3e50;
    line-height: 1.6;
  }

  :global(input[type="text"]),
  :global(input[type="password"]),
  :global(input[type="number"]),
  :global(select) {
    width: 100%;
    padding: 0.75rem;
    border: 2px solid #e0e6ed;
    border-radius: 6px;
    font-size: 0.95rem;
    transition: border-color 0.2s;
    background: white;
  }

  :global(input:focus),
  :global(select:focus) {
    outline: none;
    border-color: #667eea;
  }

  :global(input[type="checkbox"]) {
    width: 20px;
    height: 20px;
    cursor: pointer;
  }
</style>
