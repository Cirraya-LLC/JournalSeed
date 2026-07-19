<script lang="ts">
  import { ArrowDownLeft, ArrowUpRight, Landmark, LoaderCircle, Plus } from 'lucide-svelte';
  import { errorMessage, formatMoney, normalizeMoneyInput } from '$lib/format';
  import type { Account, Category, Direction } from '$lib/types';

  export let accounts: Account[];
  export let categories: Category[];
  export let onCreateAccount: (name: string, openingBalance: string) => Promise<void>;
  export let onCreateCategory: (name: string, direction: Direction) => Promise<void>;

  let accountName = '';
  let openingBalance = '0.00';
  let categoryName = '';
  let categoryDirection: Direction = 'expense';
  let accountSaving = false;
  let categorySaving = false;
  let accountError = '';
  let categoryError = '';

  async function createAccount(): Promise<void> {
    accountError = '';
    if (!accountName.trim()) {
      accountError = '请填写账户名称';
      return;
    }
    accountSaving = true;
    try {
      await onCreateAccount(accountName.trim(), normalizeMoneyInput(openingBalance));
      accountName = '';
      openingBalance = '0.00';
    } catch (reason) {
      accountError = errorMessage(reason);
    } finally {
      accountSaving = false;
    }
  }

  async function createCategory(): Promise<void> {
    categoryError = '';
    if (!categoryName.trim()) {
      categoryError = '请填写分类名称';
      return;
    }
    categorySaving = true;
    try {
      await onCreateCategory(categoryName.trim(), categoryDirection);
      categoryName = '';
    } catch (reason) {
      categoryError = errorMessage(reason);
    } finally {
      categorySaving = false;
    }
  }
</script>

<div class="settings-view">
  <header class="view-header">
    <div>
      <span>账本结构</span>
      <h2>账户与分类</h2>
    </div>
  </header>

  <div class="settings-columns">
    <section>
      <header class="section-header">
        <div>
          <h3>账户</h3>
          <span>{accounts.length} 个启用账户</span>
        </div>
        <Landmark size={20} />
      </header>

      <form class="inline-form account-form" on:submit|preventDefault={createAccount}>
        <div class="field">
          <label for="account-name">账户名称</label>
          <input
            id="account-name"
            class="input"
            bind:value={accountName}
            placeholder="例如：工资卡"
          />
        </div>
        <div class="field">
          <label for="opening-balance">期初余额</label>
          <input
            id="opening-balance"
            class="input"
            inputmode="decimal"
            bind:value={openingBalance}
          />
        </div>
        <button class="button primary add-button" type="submit" disabled={accountSaving}>
          {#if accountSaving}<LoaderCircle class="spinner-icon" size={17} />{:else}<Plus
              size={17}
            />{/if}
          添加账户
        </button>
      </form>
      {#if accountError}<p class="field-error" role="alert">{accountError}</p>{/if}

      <div class="data-list" aria-label="账户列表">
        <div class="list-heading"><span>名称</span><span>期初</span><span>当前余额</span></div>
        {#each accounts as account}
          <div class="list-row">
            <strong>{account.name}</strong>
            <span>{formatMoney(account.openingBalance)}</span>
            <span
              class:positive={!account.balance.startsWith('-')}
              class:negative={account.balance.startsWith('-')}
            >
              {formatMoney(account.balance)}
            </span>
          </div>
        {:else}
          <div class="list-empty">添加第一个账户后即可记录余额。</div>
        {/each}
      </div>
    </section>

    <section>
      <header class="section-header">
        <div>
          <h3>收支分类</h3>
          <span>{categories.length} 个启用分类</span>
        </div>
        {#if categoryDirection === 'income'}<ArrowDownLeft size={20} />{:else}<ArrowUpRight
            size={20}
          />{/if}
      </header>

      <form class="inline-form category-form" on:submit|preventDefault={createCategory}>
        <div class="field category-name">
          <label for="category-name">分类名称</label>
          <input
            id="category-name"
            class="input"
            bind:value={categoryName}
            placeholder="例如：餐饮"
          />
        </div>
        <div class="field">
          <span class="field-label">方向</span>
          <div class="direction-control" aria-label="分类方向">
            <button
              type="button"
              class:active={categoryDirection === 'expense'}
              aria-pressed={categoryDirection === 'expense'}
              on:click={() => (categoryDirection = 'expense')}>支出</button
            >
            <button
              type="button"
              class:active={categoryDirection === 'income'}
              aria-pressed={categoryDirection === 'income'}
              on:click={() => (categoryDirection = 'income')}>收入</button
            >
          </div>
        </div>
        <button class="button primary add-button" type="submit" disabled={categorySaving}>
          {#if categorySaving}<LoaderCircle class="spinner-icon" size={17} />{:else}<Plus
              size={17}
            />{/if}
          添加分类
        </button>
      </form>
      {#if categoryError}<p class="field-error" role="alert">{categoryError}</p>{/if}

      <div class="category-groups">
        <div>
          <h4><ArrowUpRight size={15} />支出</h4>
          <div class="category-list">
            {#each categories.filter((category) => category.direction === 'expense') as category}
              <span class="expense-category">{category.name}</span>
            {:else}<span class="category-empty">还没有支出分类</span>{/each}
          </div>
        </div>
        <div>
          <h4><ArrowDownLeft size={15} />收入</h4>
          <div class="category-list">
            {#each categories.filter((category) => category.direction === 'income') as category}
              <span class="income-category">{category.name}</span>
            {:else}<span class="category-empty">还没有收入分类</span>{/each}
          </div>
        </div>
      </div>
    </section>
  </div>
</div>

<style>
  .settings-view {
    min-height: 100%;
    background: var(--surface-raised);
  }

  .view-header {
    display: flex;
    align-items: center;
    min-height: 84px;
    padding: 16px 24px;
    border-bottom: 1px solid var(--line);
  }

  .view-header > div,
  .section-header > div {
    display: grid;
    gap: 2px;
  }

  .view-header span,
  .section-header span {
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .view-header > div > span {
    color: var(--accent-strong);
    font-weight: 750;
  }

  h2,
  h3,
  h4 {
    margin: 0;
    color: var(--ink-strong);
  }

  h2 {
    font-size: 1.25rem;
  }

  h3 {
    font-size: 1rem;
  }

  .settings-columns {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  section {
    min-width: 0;
    padding: 24px;
  }

  section + section {
    border-left: 1px solid var(--line);
  }

  .section-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 44px;
    margin-bottom: 20px;
    color: var(--accent-strong);
  }

  .inline-form {
    display: grid;
    align-items: end;
    gap: 10px;
    padding-bottom: 20px;
    border-bottom: 1px solid var(--line);
  }

  .account-form {
    grid-template-columns: minmax(120px, 1fr) minmax(100px, 0.7fr) auto;
  }

  .category-form {
    grid-template-columns: minmax(120px, 1fr) 116px auto;
  }

  .add-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    white-space: nowrap;
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }

  .data-list {
    margin-top: 20px;
    font-size: 0.875rem;
    font-variant-numeric: tabular-nums;
  }

  .list-heading,
  .list-row {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 100px 110px;
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

  .list-heading span:not(:first-child),
  .list-row span {
    text-align: right;
  }

  .list-row strong {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .positive {
    color: var(--income);
  }

  .negative {
    color: var(--expense);
  }

  .list-empty {
    padding: 28px 0;
    color: var(--ink-muted);
    text-align: center;
  }

  .direction-control {
    display: grid;
    height: 42px;
    grid-template-columns: 1fr 1fr;
    padding: 3px;
    border: 1px solid var(--line-strong);
    border-radius: var(--radius-sm);
    background: var(--surface-subtle);
  }

  .direction-control button {
    min-width: 0;
    padding: 0 6px;
    border: 0;
    border-radius: 3px;
    color: var(--ink-muted);
    background: transparent;
    font-size: 0.875rem;
  }

  .direction-control button.active {
    color: var(--ink-strong);
    background: var(--surface-raised);
  }

  .category-groups {
    display: grid;
    gap: 24px;
    margin-top: 20px;
  }

  .category-groups > div {
    display: grid;
    gap: 10px;
  }

  h4 {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    font-size: 0.875rem;
  }

  .category-list {
    display: flex;
    flex-wrap: wrap;
    gap: 7px;
  }

  .category-list > span:not(.category-empty) {
    padding: 5px 9px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    font-size: 0.8125rem;
  }

  .expense-category {
    color: var(--expense);
    background: var(--expense-soft);
  }

  .income-category {
    color: var(--income);
    background: var(--income-soft);
  }

  .category-empty {
    color: var(--ink-muted);
    font-size: 0.8125rem;
  }

  @media (max-width: 1040px) {
    .settings-columns {
      grid-template-columns: 1fr;
    }

    section + section {
      border-top: 1px solid var(--line);
      border-left: 0;
    }
  }

  @media (max-width: 640px) {
    .view-header,
    section {
      padding-inline: 16px;
    }

    .inline-form,
    .account-form,
    .category-form {
      grid-template-columns: 1fr;
    }
  }
</style>
