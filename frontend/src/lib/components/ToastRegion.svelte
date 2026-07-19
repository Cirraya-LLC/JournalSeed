<script context="module" lang="ts">
  export interface Toast {
    id: number;
    tone: 'success' | 'error';
    message: string;
  }
</script>

<script lang="ts">
  import { CheckCircle2, CircleAlert, X } from 'lucide-svelte';

  export let toasts: Toast[];
  export let dismiss: (id: number) => void;
</script>

<div class="toast-region" aria-live="polite" aria-atomic="false">
  {#each toasts as toast (toast.id)}
    <div class:error={toast.tone === 'error'} class="toast" role="status">
      {#if toast.tone === 'success'}
        <CheckCircle2 size={18} />
      {:else}
        <CircleAlert size={18} />
      {/if}
      <span>{toast.message}</span>
      <button aria-label="关闭通知" title="关闭通知" on:click={() => dismiss(toast.id)}>
        <X size={16} />
      </button>
    </div>
  {/each}
</div>

<style>
  .toast-region {
    position: fixed;
    z-index: 800;
    right: max(16px, env(safe-area-inset-right));
    bottom: max(16px, env(safe-area-inset-bottom));
    display: grid;
    width: min(380px, calc(100vw - 32px));
    gap: 8px;
    pointer-events: none;
  }

  .toast {
    display: grid;
    grid-template-columns: 20px minmax(0, 1fr) 32px;
    align-items: center;
    gap: 10px;
    min-height: 48px;
    padding: 8px 8px 8px 14px;
    border: 1px solid oklch(63% 0.085 148);
    border-radius: var(--radius-sm);
    color: oklch(95% 0.012 148);
    background: oklch(31% 0.055 150);
    box-shadow: 0 12px 32px oklch(20% 0.02 150 / 0.2);
    pointer-events: auto;
    animation: toast-in 300ms var(--ease-out) both;
  }

  .toast.error {
    border-color: oklch(65% 0.11 27);
    color: oklch(96% 0.01 27);
    background: oklch(35% 0.09 27);
  }

  .toast span {
    overflow-wrap: anywhere;
    font-size: 0.875rem;
  }

  .toast button {
    display: grid;
    place-items: center;
    width: 32px;
    height: 32px;
    padding: 0;
    border: 0;
    border-radius: var(--radius-sm);
    color: inherit;
    background: transparent;
  }

  @keyframes toast-in {
    from {
      opacity: 0;
      transform: translateY(12px);
    }
  }
</style>
