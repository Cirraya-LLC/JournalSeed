<script lang="ts">
  import {
    ArrowDownLeft,
    ArrowLeftRight,
    ArrowUpRight,
    FileText,
    LoaderCircle,
    Save,
    Trash2
  } from 'lucide-svelte';
  import Drawer from './Drawer.svelte';
  import { errorMessage, normalizeMoneyInput, today } from '$lib/format';
  import type { Account, Category, Column, JournalRow, RowInput, RowKind } from '$lib/types';

  export let row: JournalRow | null = null;
  export let accounts: Account[];
  export let categories: Category[];
  export let columns: Column[];
  export let onClose: () => void;
  export let onSave: (input: RowInput) => Promise<void>;
  export let onRecycle: (() => Promise<void>) | null = null;

  let kind: RowKind = row?.kind ?? 'entry';
  let date = row?.date ?? today();
  let description = row?.description ?? '';
  let amount = row?.amount ?? '';
  let accountId = row?.accountId ?? '';
  let categoryId = row?.categoryId ?? '';
  let transferAccountId = row?.transferAccountId ?? '';
  let cells = { ...(row?.cells ?? {}) };
  let saving = false;
  let recycling = false;
  let error = '';

  $: direction = amount.trim().startsWith('-') ? 'expense' : 'income';
  $: visibleCategories = categories.filter((category) => category.direction === direction);
  $: customColumns = columns.filter((column) => !column.system && column.type !== 'formula');
  $: if (kind === 'note') amount = '0.00';

  function selectKind(next: RowKind): void {
    kind = next;
    error = '';
    if (next === 'note') amount = '0.00';
    if (next === 'transfer' && amount.startsWith('-')) amount = amount.slice(1);
  }

  async function submit(): Promise<void> {
    error = '';
    try {
      const normalizedAmount = kind === 'note' ? '0.00' : normalizeMoneyInput(amount);
      if (kind === 'transfer' && normalizedAmount.startsWith('-')) {
        throw new Error('转账金额需要填写正数');
      }
      if (kind === 'transfer' && (!accountId || !transferAccountId)) {
        throw new Error('请选择转出账户和转入账户');
      }
      if (kind === 'transfer' && accountId === transferAccountId) {
        throw new Error('转出账户和转入账户需要不同');
      }
      saving = true;
      await onSave({
        date,
        description: description.trim(),
        kind,
        amount: normalizedAmount,
        accountId: accountId || null,
        categoryId: kind === 'entry' ? categoryId || null : null,
        transferAccountId: kind === 'transfer' ? transferAccountId || null : null,
        cells
      });
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      saving = false;
    }
  }

  async function recycle(): Promise<void> {
    if (!onRecycle) return;
    recycling = true;
    error = '';
    try {
      await onRecycle();
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      recycling = false;
    }
  }
</script>

<Drawer
  title={row ? '编辑流水' : '新增流水'}
  eyebrow={row ? `修订 ${row.revision}` : '快速录入'}
  {onClose}
>
  <form class="entry-form" on:submit|preventDefault={submit}>
    <div class="segmented" aria-label="流水类型">
      <button
        type="button"
        class:active={kind === 'entry'}
        aria-pressed={kind === 'entry'}
        on:click={() => selectKind('entry')}><ArrowDownLeft size={17} />收支</button
      >
      <button
        type="button"
        class:active={kind === 'transfer'}
        aria-pressed={kind === 'transfer'}
        on:click={() => selectKind('transfer')}><ArrowLeftRight size={17} />转账</button
      >
      <button
        type="button"
        class:active={kind === 'note'}
        aria-pressed={kind === 'note'}
        on:click={() => selectKind('note')}><FileText size={17} />备注</button
      >
    </div>

    <div class="primary-fields">
      <div class="field">
        <label for="entry-date">日期</label>
        <input id="entry-date" class="input" type="date" bind:value={date} required />
      </div>
      <div
        class="field amount-field"
        class:income={direction === 'income'}
        class:expense={direction === 'expense'}
      >
        <label for="entry-amount">金额</label>
        <div>
          {#if kind === 'entry'}
            {#if direction === 'expense'}<ArrowUpRight size={18} />{:else}<ArrowDownLeft
                size={18}
              />{/if}
          {:else if kind === 'transfer'}
            <ArrowLeftRight size={18} />
          {:else}
            <FileText size={18} />
          {/if}
          <input
            id="entry-amount"
            class="input"
            inputmode="decimal"
            bind:value={amount}
            disabled={kind === 'note'}
            placeholder={kind === 'entry' ? '-48.50 / 1250' : '500.00'}
            required
          />
        </div>
      </div>
    </div>

    <div class="field">
      <label for="entry-description">说明</label>
      <input
        id="entry-description"
        class="input"
        bind:value={description}
        placeholder={kind === 'note' ? '记录一条备注' : '这笔钱用在了哪里'}
      />
    </div>

    {#if kind !== 'note'}
      <div class="account-grid">
        <div class="field">
          <label for="entry-account">{kind === 'transfer' ? '转出账户' : '账户'}</label>
          <select
            id="entry-account"
            class="select"
            bind:value={accountId}
            required={kind === 'transfer'}
          >
            {#if kind !== 'transfer'}<option value="">未分配</option>{/if}
            {#each accounts as account}<option value={account.id}>{account.name}</option>{/each}
          </select>
        </div>

        {#if kind === 'transfer'}
          <div class="field">
            <label for="entry-transfer-account">转入账户</label>
            <select
              id="entry-transfer-account"
              class="select"
              bind:value={transferAccountId}
              required
            >
              <option value="" disabled>选择账户</option>
              {#each accounts as account}<option value={account.id}>{account.name}</option>{/each}
            </select>
          </div>
        {:else}
          <div class="field">
            <label for="entry-category">分类</label>
            <select id="entry-category" class="select" bind:value={categoryId}>
              <option value="">未分配</option>
              {#each visibleCategories as category}
                <option value={category.id}>{category.name}</option>
              {/each}
            </select>
          </div>
        {/if}
      </div>
    {/if}

    {#if customColumns.length}
      <fieldset class="custom-fields">
        <legend>自定义字段</legend>
        {#each customColumns as column}
          <div class="field">
            <label for={`cell-${column.id}`}>{column.name}</label>
            {#if column.type === 'boolean'}
              <label class="checkbox-row" for={`cell-${column.id}`}>
                <input
                  id={`cell-${column.id}`}
                  type="checkbox"
                  checked={cells[column.id] === true}
                  on:change={(event) =>
                    (cells[column.id] = (event.currentTarget as HTMLInputElement).checked)}
                />
                <span>已选</span>
              </label>
            {:else if column.type === 'date'}
              <input
                id={`cell-${column.id}`}
                class="input"
                type="date"
                value={typeof cells[column.id] === 'string' ? cells[column.id] : ''}
                on:input={(event) =>
                  (cells[column.id] = (event.currentTarget as HTMLInputElement).value)}
              />
            {:else}
              <input
                id={`cell-${column.id}`}
                class="input"
                inputmode={column.type === 'number' ? 'decimal' : 'text'}
                value={typeof cells[column.id] === 'string' ? cells[column.id] : ''}
                on:input={(event) =>
                  (cells[column.id] = (event.currentTarget as HTMLInputElement).value)}
              />
            {/if}
          </div>
        {/each}
      </fieldset>
    {/if}

    {#if error}<p class="field-error" role="alert">{error}</p>{/if}

    <footer class="form-actions">
      {#if row && onRecycle}
        <button class="button danger recycle" type="button" disabled={recycling} on:click={recycle}>
          <Trash2 size={17} />
          {recycling ? '正在移入回收站' : '移入回收站'}
        </button>
      {/if}
      <button class="button primary save" type="submit" disabled={saving}>
        {#if saving}<LoaderCircle class="spinner-icon" size={18} />正在保存{:else}<Save
            size={18}
          />保存流水{/if}
      </button>
    </footer>
  </form>
</Drawer>

<style>
  .entry-form {
    display: flex;
    flex-direction: column;
    gap: 16px;
    min-height: 100%;
    padding: 18px 18px max(18px, env(safe-area-inset-bottom));
  }

  .segmented {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 2px;
    padding: 3px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    background: var(--surface-subtle);
  }

  .segmented button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    min-width: 0;
    min-height: 38px;
    padding: 6px 8px;
    border: 0;
    border-radius: 3px;
    color: var(--ink-muted);
    background: transparent;
    font-size: 0.875rem;
    font-weight: 650;
    transition:
      color var(--duration-fast) var(--ease-out),
      background var(--duration-fast) var(--ease-out),
      box-shadow var(--duration-fast) var(--ease-out);
  }

  .segmented button.active {
    color: var(--ink-strong);
    background: var(--surface-raised);
    box-shadow: 0 1px 3px oklch(25% 0.02 155 / 0.1);
  }

  .segmented button.active :global(svg) {
    color: var(--accent-strong);
  }

  .primary-fields,
  .account-grid {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
    align-items: start;
    gap: 10px;
  }

  .entry-form :global(.field) {
    min-width: 0;
    gap: 5px;
  }

  .entry-form :global(.input),
  .entry-form :global(.select) {
    min-width: 0;
  }

  .amount-field > div {
    position: relative;
  }

  .amount-field > div > :global(svg) {
    position: absolute;
    z-index: 1;
    top: 50%;
    left: 10px;
    transform: translateY(-50%);
    color: var(--transfer);
    pointer-events: none;
  }

  .amount-field .input {
    padding-left: 36px;
    font-variant-numeric: tabular-nums;
    font-weight: 700;
  }

  .amount-field.income > div > :global(svg),
  .amount-field.income .input {
    color: var(--income);
  }

  .amount-field.expense > div > :global(svg),
  .amount-field.expense .input {
    color: var(--expense);
  }

  .custom-fields {
    display: grid;
    gap: 14px;
    margin: 0;
    padding: 14px 0 0;
    border: 0;
    border-top: 1px solid var(--line);
  }

  .custom-fields legend {
    padding: 0 8px 0 0;
    color: var(--ink-muted);
    font-size: 0.75rem;
    font-weight: 750;
  }

  .checkbox-row {
    display: flex;
    align-items: center;
    gap: 10px;
    min-height: 42px;
    padding: 8px 10px;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-sm);
    background: var(--surface-raised);
    font-weight: 500;
  }

  .checkbox-row input {
    width: 18px;
    height: 18px;
    accent-color: var(--accent-strong);
  }

  .form-actions {
    display: flex;
    align-items: center;
    gap: 8px;
    margin-top: auto;
    padding-top: 12px;
    border-top: 1px solid var(--line);
  }

  .form-actions button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 7px;
  }

  .save {
    flex: 1;
  }

  .recycle {
    padding-inline: 10px;
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }

  @media (max-width: 350px) {
    .primary-fields,
    .account-grid {
      grid-template-columns: 1fr;
    }

    .recycle {
      width: 44px;
      padding: 0;
      font-size: 0;
    }
  }
</style>
