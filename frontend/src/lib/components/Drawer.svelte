<script lang="ts">
  import { X } from 'lucide-svelte';
  import { onMount } from 'svelte';

  export let title: string;
  export let eyebrow = '';
  export let onClose: () => void;

  let panel: HTMLElement;

  onMount(() => {
    const previous = document.activeElement as HTMLElement | null;
    panel?.focus();
    const onKeydown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onClose();
    };
    document.addEventListener('keydown', onKeydown);
    return () => {
      document.removeEventListener('keydown', onKeydown);
      previous?.focus();
    };
  });
</script>

<div class="drawer-layer" role="presentation">
  <button class="drawer-backdrop" aria-label="关闭抽屉" on:click={onClose}></button>
  <div
    class="drawer"
    role="dialog"
    aria-modal="true"
    aria-labelledby="drawer-title"
    tabindex="-1"
    bind:this={panel}
  >
    <header class="drawer-header">
      <div>
        {#if eyebrow}<span>{eyebrow}</span>{/if}
        <h2 id="drawer-title">{title}</h2>
      </div>
      <button class="icon-button" type="button" aria-label="关闭" title="关闭" on:click={onClose}>
        <X size={20} />
      </button>
    </header>
    <div class="drawer-body"><slot /></div>
  </div>
</div>

<style>
  .drawer-layer {
    position: fixed;
    z-index: 500;
    inset: 0;
  }

  .drawer-backdrop {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    padding: 0;
    border: 0;
    background: oklch(18% 0.02 155 / 0.32);
    animation: backdrop-in var(--duration-state) var(--ease-out) both;
  }

  .drawer {
    position: absolute;
    top: 0;
    right: 0;
    display: grid;
    width: min(100%, 480px);
    height: 100%;
    height: 100dvh;
    grid-template-rows: auto minmax(0, 1fr);
    color: var(--ink-strong);
    background: var(--surface-raised);
    box-shadow: var(--shadow-drawer);
    animation: drawer-in 360ms var(--ease-out) both;
  }

  .drawer-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 64px;
    padding: max(12px, env(safe-area-inset-top)) 18px 12px;
    border-bottom: 1px solid var(--line);
  }

  .drawer-header > div {
    display: grid;
    gap: 0;
  }

  .drawer-header span {
    color: var(--accent-strong);
    font-size: 0.6875rem;
    font-weight: 750;
  }

  .drawer-header h2 {
    margin: 0;
    font-size: 1.125rem;
    line-height: 1.3;
  }

  .drawer-body {
    min-height: 0;
    overflow: auto;
    overscroll-behavior: contain;
  }

  @keyframes drawer-in {
    from {
      opacity: 0;
      transform: translateX(32px);
    }
  }

  @keyframes backdrop-in {
    from {
      opacity: 0;
    }
  }
</style>
