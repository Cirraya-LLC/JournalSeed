<script lang="ts">
  import { onMount } from 'svelte';
  import { LoaderCircle, Sprout } from 'lucide-svelte';
  import LedgerWorkspace from '$lib/components/LedgerWorkspace.svelte';
  import LoginView from '$lib/components/LoginView.svelte';
  import SetupView from '$lib/components/SetupView.svelte';
  import { api, ApiError } from '$lib/api/client';
  import type { Session } from '$lib/types';

  type Screen = 'boot' | 'setup' | 'login' | 'ledger' | 'error';
  let screen: Screen = 'boot';
  let session: Session | null = null;
  let bootError = '';

  async function boot(): Promise<void> {
    screen = 'boot';
    bootError = '';
    try {
      const status = await api.setupStatus();
      if (status.required) {
        screen = 'setup';
        return;
      }
      try {
        session = await api.session();
        screen = 'ledger';
      } catch (reason) {
        if (reason instanceof ApiError && reason.status === 401) {
          screen = 'login';
          return;
        }
        throw reason;
      }
    } catch (reason) {
      bootError = reason instanceof Error ? reason.message : '服务连接失败';
      screen = 'error';
    }
  }

  async function setup(input: {
    username: string;
    password: string;
    ledgerName: string;
  }): Promise<void> {
    session = await api.setup(input);
    screen = 'ledger';
  }

  async function login(input: { username: string; password: string }): Promise<void> {
    session = await api.login(input);
    screen = 'ledger';
  }

  onMount(boot);
</script>

<svelte:head>
  <title>JournalSeed · 网页账本</title>
  <meta name="description" content="JournalSeed 中文网页账本" />
</svelte:head>

{#if screen === 'boot'}
  <main class="boot-screen" aria-label="正在打开账本">
    <span><Sprout size={24} /></span>
    <strong>JournalSeed</strong>
    <LoaderCircle class="boot-spinner" size={18} />
  </main>
{:else if screen === 'setup'}
  <SetupView onSetup={setup} />
{:else if screen === 'login'}
  <LoginView onLogin={login} />
{:else if screen === 'ledger' && session}
  <LedgerWorkspace
    {session}
    onLoggedOut={() => {
      session = null;
      screen = 'login';
    }}
  />
{:else}
  <main class="error-screen">
    <span><Sprout size={24} /></span>
    <h1>账本服务没有响应</h1>
    <p>{bootError}</p>
    <button class="button primary" type="button" on:click={boot}>重新连接</button>
  </main>
{/if}

<style>
  .boot-screen,
  .error-screen {
    display: grid;
    place-content: center;
    justify-items: center;
    min-height: 100vh;
    min-height: 100dvh;
    gap: 10px;
    padding: 24px;
    color: var(--ink-muted);
    background: var(--surface);
    text-align: center;
  }

  .boot-screen > span,
  .error-screen > span {
    display: grid;
    place-items: center;
    width: 48px;
    height: 48px;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-sm);
    color: var(--accent-strong);
    background: var(--surface-raised);
  }

  .boot-screen strong {
    color: var(--ink-strong);
    font-size: 1rem;
  }

  :global(.boot-spinner) {
    margin-top: 8px;
    animation: spin 700ms linear infinite;
  }

  .error-screen h1 {
    margin: 12px 0 0;
    color: var(--ink-strong);
    font-size: 1.25rem;
  }

  .error-screen p {
    max-width: 52ch;
    margin: 0 0 8px;
    overflow-wrap: anywhere;
    font-size: 0.875rem;
  }
</style>
