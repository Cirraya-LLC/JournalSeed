<script lang="ts">
  import {
    ArrowDown,
    ArrowDownLeft,
    ArrowLeftRight,
    ArrowUp,
    ArrowUpRight,
    Check,
    FileText,
    PencilLine,
    Plus
  } from 'lucide-svelte';
  import { formatMoney } from '$lib/format';
  import type { Column, JournalRow } from '$lib/types';

  export let rows: JournalRow[];
  export let columns: Column[];
  export let loading = false;
  export let sort = 'date:desc';
  export let onSort: (sort: string) => void;
  export let onSelect: (row: JournalRow) => void;
  export let onCreate: () => void;

  $: orderedColumns = [...columns].sort((left, right) => left.position - right.position);

  function sortColumn(column: Column): void {
    const key = column.system ?? column.id;
    const [activeKey, direction] = sort.split(':');
    onSort(`${key}:${activeKey === key && direction === 'asc' ? 'desc' : 'asc'}`);
  }

  function valueFor(row: JournalRow, column: Column): string | boolean | null {
    switch (column.system) {
      case 'date':
        return row.date;
      case 'description':
        return row.description;
      case 'account':
        return row.accountName ?? '未分配';
      case 'category':
        return row.categoryName ?? (row.kind === 'transfer' ? '转账' : '未分配');
      case 'amount':
        return row.amount;
      default:
        return row.cells[column.id] ?? null;
    }
  }

  function isNegative(amount: string): boolean {
    return amount.startsWith('-');
  }

  function isPositive(amount: string): boolean {
    return !isNegative(amount) && !/^0+(?:\.0+)?$/.test(amount);
  }
</script>

<div class="table-shell">
  <div class="desktop-table" role="region" aria-label="流水表格">
    <table>
      <colgroup>
        <col class="type-column" />
        {#each orderedColumns as column}
          <col style={`width: ${column.width}px`} />
        {/each}
        <col class="action-column" />
      </colgroup>
      <thead>
        <tr>
          <th class="type-heading" scope="col"><span class="sr-only">类型</span></th>
          {#each orderedColumns as column}
            <th scope="col" class:amount-heading={column.system === 'amount'}>
              <button type="button" on:click={() => sortColumn(column)}>
                <span>{column.name}</span>
                {#if sort.startsWith(`${column.system ?? column.id}:`)}
                  {#if sort.endsWith(':asc')}<ArrowUp size={14} />{:else}<ArrowDown
                      size={14}
                    />{/if}
                {/if}
              </button>
            </th>
          {/each}
          <th scope="col"><span class="sr-only">操作</span></th>
        </tr>
      </thead>
      <tbody>
        {#if loading}
          {#each Array(8) as _}
            <tr class="skeleton-row" aria-hidden="true">
              <td><i></i></td>
              {#each orderedColumns as column}<td
                  ><i class:amount={column.system === 'amount'}></i></td
                >{/each}
              <td></td>
            </tr>
          {/each}
        {:else}
          {#each rows as row (row.id)}
            <tr
              class:income={row.kind === 'entry' && isPositive(row.amount)}
              class:expense={row.kind === 'entry' && isNegative(row.amount)}
              class:note={row.kind === 'note'}
              class:transfer={row.kind === 'transfer'}
              on:click={() => onSelect(row)}
            >
              <td class="type-cell">
                <span
                  class="type-icon"
                  title={row.kind === 'transfer'
                    ? '转账'
                    : row.kind === 'note'
                      ? '备注'
                      : isPositive(row.amount)
                        ? '收入'
                        : '支出'}
                >
                  {#if row.kind === 'transfer'}
                    <ArrowLeftRight size={16} />
                  {:else if row.kind === 'note'}
                    <FileText size={16} />
                  {:else if isPositive(row.amount)}
                    <ArrowDownLeft size={16} />
                  {:else}
                    <ArrowUpRight size={16} />
                  {/if}
                </span>
              </td>
              {#each orderedColumns as column}
                {@const value = valueFor(row, column)}
                <td
                  class:amount-cell={column.system === 'amount'}
                  class:description-cell={column.system === 'description'}
                  title={typeof value === 'string' ? value : undefined}
                >
                  {#if column.system === 'account' && row.kind === 'transfer'}
                    <span class="transfer-path">
                      <span>{row.accountName ?? '未分配'}</span>
                      <ArrowLeftRight size={13} />
                      <span>{row.transferAccountName ?? '未分配'}</span>
                    </span>
                  {:else if column.system === 'amount'}
                    <span>{formatMoney(String(value ?? '0.00'))}</span>
                  {:else if column.type === 'boolean'}
                    {#if value === true}<Check size={16} aria-label="是" />{:else}<span
                        class="empty-value">—</span
                      >{/if}
                  {:else if value === null || value === ''}
                    <span class="empty-value">—</span>
                  {:else}
                    <span>{String(value)}</span>
                  {/if}
                </td>
              {/each}
              <td class="action-cell">
                <button
                  type="button"
                  aria-label={`编辑 ${row.description || row.date}`}
                  title="编辑流水"
                  on:click|stopPropagation={() => onSelect(row)}><PencilLine size={16} /></button
                >
              </td>
            </tr>
          {/each}
        {/if}
      </tbody>
    </table>
  </div>

  <div class="mobile-list" aria-label="流水列表">
    {#if loading}
      {#each Array(6) as _}<div class="mobile-skeleton" aria-hidden="true">
          <i></i><i></i>
        </div>{/each}
    {:else}
      {#each rows as row (row.id)}
        <button
          class:income={row.kind === 'entry' && isPositive(row.amount)}
          class:expense={row.kind === 'entry' && isNegative(row.amount)}
          class:note={row.kind === 'note'}
          class:transfer={row.kind === 'transfer'}
          class="mobile-row"
          type="button"
          on:click={() => onSelect(row)}
        >
          <span class="mobile-icon">
            {#if row.kind === 'transfer'}
              <ArrowLeftRight size={17} />
            {:else if row.kind === 'note'}
              <FileText size={17} />
            {:else if isPositive(row.amount)}
              <ArrowDownLeft size={17} />
            {:else}
              <ArrowUpRight size={17} />
            {/if}
          </span>
          <span class="mobile-main">
            <strong>{row.description || (row.kind === 'note' ? '备注' : '未填写说明')}</strong>
            <small>
              {row.date} · {row.accountName ?? '未分配'}
              {#if row.kind === 'transfer'}
                → {row.transferAccountName ?? '未分配'}{/if}
            </small>
          </span>
          <span class="mobile-amount">
            {#if row.kind === 'note'}备注{:else}{formatMoney(row.amount)}{/if}
          </span>
        </button>
      {/each}
    {/if}
  </div>

  {#if !loading && rows.length === 0}
    <div class="empty-state">
      <div><FileText size={24} /></div>
      <strong>还没有流水</strong>
      <span>第一笔记录会出现在这里。</span>
      <button class="button primary" type="button" on:click={onCreate}
        ><Plus size={17} />新增流水</button
      >
    </div>
  {/if}
</div>

<style>
  .table-shell {
    position: relative;
    min-height: 360px;
    background: var(--surface-raised);
  }

  .desktop-table {
    max-height: calc(100vh - 242px);
    max-height: calc(100dvh - 242px);
    overflow: auto;
    overscroll-behavior: contain;
  }

  table {
    width: max(100%, 920px);
    border-spacing: 0;
    table-layout: fixed;
    font-size: 0.875rem;
    font-variant-numeric: tabular-nums;
  }

  .type-column {
    width: 42px;
  }

  .action-column {
    width: 48px;
  }

  th {
    position: sticky;
    z-index: 2;
    top: 0;
    height: 38px;
    padding: 0;
    border-right: 1px solid var(--line);
    border-bottom: 1px solid var(--line-strong);
    color: var(--ink-muted);
    background: var(--surface-subtle);
    font-size: 0.75rem;
    font-weight: 700;
    text-align: left;
  }

  th button {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 6px;
    width: 100%;
    height: 100%;
    padding: 0 10px;
    border: 0;
    color: inherit;
    background: transparent;
    text-align: left;
  }

  th button span {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  th.amount-heading button {
    justify-content: flex-end;
  }

  td {
    height: 44px;
    overflow: hidden;
    padding: 0 10px;
    border-right: 1px solid var(--line);
    border-bottom: 1px solid var(--line);
    color: var(--ink);
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  tbody tr {
    position: relative;
    cursor: pointer;
    transition: background var(--duration-fast) var(--ease-out);
  }

  tbody tr::after {
    position: absolute;
    top: 0;
    bottom: 0;
    left: 0;
    width: 2px;
    background: transparent;
    content: '';
  }

  tbody tr.income::after {
    background: var(--income);
  }

  tbody tr.expense::after {
    background: var(--expense);
  }

  tbody tr.transfer::after {
    background: var(--transfer);
  }

  tbody tr.note::after {
    background: var(--line-strong);
  }

  tbody tr:focus-visible {
    outline: 2px solid var(--focus);
    outline-offset: -2px;
  }

  .type-cell {
    padding: 0;
    text-align: center;
  }

  .type-icon {
    display: inline-grid;
    place-items: center;
    width: 28px;
    height: 28px;
    color: var(--transfer);
  }

  tr.income .type-icon,
  tr.income .amount-cell {
    color: var(--income);
  }

  tr.expense .type-icon,
  tr.expense .amount-cell {
    color: var(--expense);
  }

  tr.note .type-icon,
  tr.note .amount-cell {
    color: var(--ink-muted);
  }

  .amount-cell {
    color: var(--transfer);
    font-weight: 700;
    text-align: right;
  }

  .description-cell {
    color: var(--ink-strong);
    font-weight: 550;
  }

  .transfer-path {
    display: inline-flex;
    align-items: center;
    max-width: 100%;
    gap: 5px;
    color: var(--transfer);
  }

  .transfer-path span {
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .empty-value {
    color: var(--line-strong);
  }

  .action-cell {
    padding: 0;
    text-align: center;
  }

  .action-cell button {
    display: inline-grid;
    place-items: center;
    width: 32px;
    height: 32px;
    padding: 0;
    border: 0;
    border-radius: var(--radius-sm);
    color: var(--ink-muted);
    background: transparent;
    opacity: 0;
  }

  .skeleton-row i {
    display: block;
    width: 70%;
    height: 8px;
    border-radius: 2px;
    background: var(--surface-subtle);
    animation: pulse 1.2s ease-in-out infinite alternate;
  }

  .skeleton-row i.amount {
    width: 48%;
    margin-left: auto;
  }

  .mobile-list {
    display: none;
  }

  .empty-state {
    position: absolute;
    inset: 38px 0 0;
    display: grid;
    place-content: center;
    justify-items: center;
    gap: 8px;
    min-height: 320px;
    padding: 24px;
    color: var(--ink-muted);
    text-align: center;
  }

  .empty-state > div {
    display: grid;
    place-items: center;
    width: 48px;
    height: 48px;
    border: 1px solid var(--line);
    border-radius: 50%;
    color: var(--accent-strong);
  }

  .empty-state strong {
    color: var(--ink-strong);
  }

  .empty-state span {
    font-size: 0.875rem;
  }

  .empty-state .button {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    margin-top: 8px;
  }

  @keyframes pulse {
    to {
      opacity: 0.42;
    }
  }

  @media (hover: hover) {
    tbody tr:hover {
      background: var(--surface-subtle);
    }

    tbody tr:hover .action-cell button,
    .action-cell button:focus-visible {
      opacity: 1;
    }

    .action-cell button:hover {
      color: var(--ink-strong);
      background: var(--line);
    }
  }

  @media (max-width: 720px) {
    .table-shell {
      min-height: 300px;
    }

    .desktop-table {
      display: none;
    }

    .mobile-list {
      display: block;
    }

    .mobile-row {
      display: grid;
      width: 100%;
      min-height: 68px;
      grid-template-columns: 34px minmax(0, 1fr) auto;
      align-items: center;
      gap: 8px;
      padding: 10px 14px;
      border: 0;
      border-bottom: 1px solid var(--line);
      color: var(--ink);
      background: var(--surface-raised);
      text-align: left;
    }

    .mobile-icon {
      display: grid;
      place-items: center;
      width: 30px;
      height: 30px;
      border-radius: 50%;
      color: var(--transfer);
      background: var(--surface-subtle);
    }

    .mobile-row.income .mobile-icon,
    .mobile-row.income .mobile-amount {
      color: var(--income);
      background: var(--income-soft);
    }

    .mobile-row.expense .mobile-icon,
    .mobile-row.expense .mobile-amount {
      color: var(--expense);
      background: var(--expense-soft);
    }

    .mobile-row.income .mobile-amount,
    .mobile-row.expense .mobile-amount {
      background: transparent;
    }

    .mobile-row.note .mobile-icon,
    .mobile-row.note .mobile-amount {
      color: var(--ink-muted);
    }

    .mobile-main {
      display: grid;
      min-width: 0;
      gap: 3px;
    }

    .mobile-main strong,
    .mobile-main small {
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .mobile-main strong {
      color: var(--ink-strong);
      font-size: 0.875rem;
      font-weight: 650;
    }

    .mobile-main small {
      color: var(--ink-muted);
      font-size: 0.75rem;
    }

    .mobile-amount {
      padding-left: 6px;
      color: var(--transfer);
      font-size: 0.875rem;
      font-variant-numeric: tabular-nums;
      font-weight: 750;
    }

    .mobile-skeleton {
      display: grid;
      min-height: 68px;
      grid-template-columns: 1fr 72px;
      align-items: center;
      gap: 24px;
      padding: 14px;
      border-bottom: 1px solid var(--line);
    }

    .mobile-skeleton i {
      height: 10px;
      border-radius: 2px;
      background: var(--surface-subtle);
      animation: pulse 1.2s ease-in-out infinite alternate;
    }

    .empty-state {
      inset: 0;
    }
  }
</style>
