<script lang="ts">
  import { Columns3, FileText, LoaderCircle, RotateCcw, Trash2 } from 'lucide-svelte';
  import { errorMessage, formatMoney } from '$lib/format';
  import type { Column, JournalRow } from '$lib/types';

  export let rows: JournalRow[];
  export let columns: Column[];
  export let onRestoreRow: (id: string) => Promise<void>;
  export let onRestoreColumn: (id: string) => Promise<void>;

  let tab: 'rows' | 'columns' = 'rows';
  let busyId = '';
  let error = '';

  async function restoreRow(id: string): Promise<void> {
    busyId = id;
    error = '';
    try {
      await onRestoreRow(id);
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      busyId = '';
    }
  }

  async function restoreColumn(id: string): Promise<void> {
    busyId = id;
    error = '';
    try {
      await onRestoreColumn(id);
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      busyId = '';
    }
  }
</script>

<div class="recycle-view">
  <header class="view-header">
    <div>
      <span>可恢复删除</span>
      <h2>回收站</h2>
    </div>
    <Trash2 size={20} />
  </header>

  <div class="tabs" role="tablist" aria-label="回收类型">
    <button
      role="tab"
      aria-selected={tab === 'rows'}
      class:active={tab === 'rows'}
      type="button"
      on:click={() => (tab = 'rows')}><FileText size={17} />流水 <span>{rows.length}</span></button
    >
    <button
      role="tab"
      aria-selected={tab === 'columns'}
      class:active={tab === 'columns'}
      type="button"
      on:click={() => (tab = 'columns')}
      ><Columns3 size={17} />列 <span>{columns.length}</span></button
    >
  </div>

  {#if error}<p class="field-error page-error" role="alert">{error}</p>{/if}

  <section class="recycle-list">
    {#if tab === 'rows'}
      <div class="list-heading row-grid">
        <span>日期</span><span>说明</span><span>金额</span><span>操作</span>
      </div>
      {#each rows as row}
        <div class="list-row row-grid">
          <span>{row.date}</span>
          <strong>{row.description || '未填写说明'}</strong>
          <span
            class:expense={row.amount.startsWith('-')}
            class:income={!row.amount.startsWith('-')}
          >
            {formatMoney(row.amount)}
          </span>
          <button
            class="button"
            type="button"
            disabled={busyId === row.id}
            on:click={() => restoreRow(row.id)}
          >
            {#if busyId === row.id}<LoaderCircle class="spinner-icon" size={16} />{:else}<RotateCcw
                size={16}
              />{/if}
            恢复
          </button>
        </div>
      {:else}
        <div class="empty-state"><RotateCcw size={24} /><strong>没有已删除流水</strong></div>
      {/each}
    {:else}
      <div class="list-heading column-grid">
        <span>列名</span><span>类型</span><span>操作</span>
      </div>
      {#each columns as column}
        <div class="list-row column-grid">
          <strong>{column.name}</strong>
          <span>{column.type}</span>
          <button
            class="button"
            type="button"
            disabled={busyId === column.id}
            on:click={() => restoreColumn(column.id)}
          >
            {#if busyId === column.id}<LoaderCircle
                class="spinner-icon"
                size={16}
              />{:else}<RotateCcw size={16} />{/if}
            恢复
          </button>
        </div>
      {:else}
        <div class="empty-state"><RotateCcw size={24} /><strong>没有已删除列</strong></div>
      {/each}
    {/if}
  </section>
</div>

<style>
  .recycle-view {
    min-height: 100%;
    background: var(--surface-raised);
  }

  .view-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 64px;
    padding: 12px 20px;
    border-bottom: 1px solid var(--line);
    color: var(--ink-muted);
  }

  .view-header > div {
    display: grid;
    gap: 2px;
  }

  .view-header span {
    color: var(--accent-strong);
    font-size: 0.75rem;
    font-weight: 750;
  }

  h2 {
    margin: 0;
    color: var(--ink-strong);
    font-size: 1.25rem;
  }

  .tabs {
    display: flex;
    min-height: 44px;
    gap: 4px;
    padding: 5px 20px 0;
    border-bottom: 1px solid var(--line);
  }

  .tabs button {
    position: relative;
    display: inline-flex;
    align-items: center;
    gap: 7px;
    padding: 0 14px;
    border: 0;
    color: var(--ink-muted);
    background: transparent;
    font-size: 0.875rem;
    font-weight: 650;
  }

  .tabs button.active {
    color: var(--ink-strong);
  }

  .tabs button.active::after {
    position: absolute;
    right: 8px;
    bottom: -1px;
    left: 8px;
    height: 2px;
    background: var(--accent-strong);
    content: '';
  }

  .tabs button span {
    display: inline-grid;
    place-items: center;
    min-width: 22px;
    height: 20px;
    padding: 0 5px;
    border-radius: 10px;
    background: var(--surface-subtle);
    font-size: 0.6875rem;
  }

  .page-error {
    margin: 14px 20px 0;
  }

  .recycle-list {
    padding: 16px 20px;
    font-size: 0.875rem;
  }

  .list-heading,
  .list-row {
    display: grid;
    align-items: center;
    gap: 10px;
    min-height: 42px;
    border-bottom: 1px solid var(--line);
  }

  .list-heading {
    min-height: 34px;
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .row-grid {
    grid-template-columns: 120px minmax(160px, 1fr) 140px 88px;
  }

  .column-grid {
    grid-template-columns: minmax(160px, 1fr) 160px 88px;
  }

  .list-row strong {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .list-row > .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
    min-height: 34px;
    padding: 5px 9px;
    font-size: 0.8125rem;
  }

  .income {
    color: var(--income);
  }

  .expense {
    color: var(--expense);
  }

  .empty-state {
    display: grid;
    place-items: center;
    gap: 8px;
    min-height: 220px;
    color: var(--ink-muted);
  }

  .empty-state strong {
    color: var(--ink);
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }

  @media (max-width: 640px) {
    .view-header,
    .recycle-list {
      padding-inline: 16px;
    }

    .tabs {
      padding-inline: 8px;
    }

    .recycle-list {
      overflow-x: auto;
    }

    .row-grid {
      width: 640px;
    }

    .column-grid {
      width: 480px;
    }
  }
</style>
