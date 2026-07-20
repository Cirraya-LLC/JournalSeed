<script lang="ts">
  import { onDestroy, onMount } from 'svelte';
  import {
    BookOpenText,
    Braces,
    ChevronDown,
    CircleUserRound,
    Columns3,
    FileText,
    Landmark,
    ListFilter,
    LogOut,
    Menu,
    MoreHorizontal,
    Plus,
    RefreshCw,
    Search,
    Sprout,
    Trash2,
    X
  } from 'lucide-svelte';
  import AccountsPanel from './AccountsPanel.svelte';
  import ColumnsPanel from './ColumnsPanel.svelte';
  import EntryDrawer from './EntryDrawer.svelte';
  import FunctionsPanel from './FunctionsPanel.svelte';
  import RecyclePanel from './RecyclePanel.svelte';
  import ToastRegion from './ToastRegion.svelte';
  import TransactionTable from './TransactionTable.svelte';
  import { api } from '$lib/api/client';
  import { errorMessage, formatMoney } from '$lib/format';
  import type {
    Account,
    Category,
    Column,
    ColumnInput,
    JournalRow,
    Ledger,
    LedgerSummary,
    LuaFunction,
    RowInput,
    Session
  } from '$lib/types';
  import type { Toast } from './ToastRegion.svelte';

  type View = 'transactions' | 'accounts' | 'columns' | 'functions' | 'recycle';

  export let session: Session;
  export let onLoggedOut: () => void;

  const navigation: Array<{ id: View; label: string; icon: typeof FileText }> = [
    { id: 'transactions', label: '流水', icon: FileText },
    { id: 'accounts', label: '账户分类', icon: Landmark },
    { id: 'columns', label: '列设置', icon: Columns3 },
    { id: 'functions', label: '函数', icon: Braces },
    { id: 'recycle', label: '回收站', icon: Trash2 }
  ];

  let view: View = 'transactions';
  let ledgers: Ledger[] = [];
  let selectedLedgerId = '';
  let summary: LedgerSummary = { balance: '0.00', income: '0.00', expense: '0.00', rowCount: 0 };
  let accounts: Account[] = [];
  let categories: Category[] = [];
  let columns: Column[] = [];
  let rows: JournalRow[] = [];
  let recycledRows: JournalRow[] = [];
  let recycledColumns: Column[] = [];
  let functions: LuaFunction[] = [];
  let nextCursor: string | null = null;
  let hasMore = false;
  let loading = true;
  let loadingMore = false;
  let search = '';
  let sort = 'date:desc';
  let entryDrawerOpen = false;
  let selectedRow: JournalRow | null = null;
  let newLedgerOpen = false;
  let newLedgerName = '';
  let creatingLedger = false;
  let mobileMenuOpen = false;
  let toasts: Toast[] = [];
  let toastCounter = 0;
  let eventSource: EventSource | null = null;
  let pollTimer: ReturnType<typeof setInterval> | null = null;

  $: selectedLedger = ledgers.find((ledger) => ledger.id === selectedLedgerId) ?? null;
  $: normalizedSearch = search.trim().toLocaleLowerCase('zh-CN');
  $: visibleRows = normalizedSearch
    ? rows.filter((row) =>
        [
          row.description,
          row.accountName,
          row.categoryName,
          row.transferAccountName,
          row.amount,
          row.date
        ]
          .filter(Boolean)
          .some((value) => String(value).toLocaleLowerCase('zh-CN').includes(normalizedSearch))
      )
    : rows;

  function notify(tone: Toast['tone'], message: string): void {
    const id = ++toastCounter;
    toasts = [...toasts, { id, tone, message }];
    window.setTimeout(() => dismissToast(id), 4200);
  }

  function dismissToast(id: number): void {
    toasts = toasts.filter((toast) => toast.id !== id);
  }

  async function bootstrap(): Promise<void> {
    loading = true;
    try {
      const [ledgerResult, functionResult] = await Promise.all([api.ledgers(), api.functions()]);
      ledgers = ledgerResult;
      functions = functionResult;
      const remembered = localStorage.getItem('journalseed.ledger');
      const initial = ledgerResult.find((ledger) => ledger.id === remembered) ?? ledgerResult[0];
      if (initial) await chooseLedger(initial.id);
    } catch (reason) {
      notify('error', errorMessage(reason));
    } finally {
      loading = false;
    }
  }

  async function chooseLedger(id: string): Promise<void> {
    selectedLedgerId = id;
    localStorage.setItem('journalseed.ledger', id);
    mobileMenuOpen = false;
    await loadLedger();
  }

  async function loadLedger(): Promise<void> {
    if (!selectedLedgerId) return;
    loading = true;
    try {
      const [
        summaryResult,
        accountResult,
        categoryResult,
        columnResult,
        rowResult,
        recycledRowResult,
        recycledColumnResult
      ] = await Promise.all([
        api.summary(selectedLedgerId),
        api.accounts(selectedLedgerId),
        api.categories(selectedLedgerId),
        api.columns(selectedLedgerId),
        api.rows(selectedLedgerId, { sort }),
        api.rows(selectedLedgerId, { recycled: true, limit: 100 }),
        api.columns(selectedLedgerId, true)
      ]);
      summary = summaryResult;
      accounts = accountResult;
      categories = categoryResult;
      columns = columnResult;
      rows = rowResult.items;
      nextCursor = rowResult.nextCursor;
      hasMore = rowResult.hasMore;
      recycledRows = recycledRowResult.items;
      recycledColumns = recycledColumnResult;
    } catch (reason) {
      notify('error', errorMessage(reason));
    } finally {
      loading = false;
    }
  }

  async function changeSort(next: string): Promise<void> {
    sort = next;
    await loadLedger();
  }

  async function loadMore(): Promise<void> {
    if (!selectedLedgerId || !nextCursor || loadingMore) return;
    loadingMore = true;
    try {
      const result = await api.rows(selectedLedgerId, { cursor: nextCursor, sort });
      rows = [...rows, ...result.items];
      nextCursor = result.nextCursor;
      hasMore = result.hasMore;
    } catch (reason) {
      notify('error', errorMessage(reason));
    } finally {
      loadingMore = false;
    }
  }

  function openNewEntry(): void {
    selectedRow = null;
    entryDrawerOpen = true;
  }

  function openEntry(row: JournalRow): void {
    selectedRow = row;
    entryDrawerOpen = true;
  }

  async function saveEntry(input: RowInput): Promise<void> {
    if (!selectedLedgerId) return;
    if (selectedRow) {
      await api.updateRow(selectedRow.id, input);
      notify('success', '流水已更新');
    } else {
      await api.createRow(selectedLedgerId, input);
      notify(
        'success',
        input.kind === 'note'
          ? '备注已保存'
          : input.kind === 'transfer'
            ? '转账已保存'
            : '流水已保存'
      );
    }
    entryDrawerOpen = false;
    selectedRow = null;
    await loadLedger();
  }

  async function recycleSelectedRow(): Promise<void> {
    if (!selectedRow) return;
    await api.recycleRow(selectedRow.id);
    entryDrawerOpen = false;
    selectedRow = null;
    notify('success', '流水已移入回收站');
    await loadLedger();
  }

  async function createAccount(name: string, openingBalance: string): Promise<void> {
    await api.createAccount(selectedLedgerId, name, openingBalance);
    notify('success', `账户“${name}”已创建`);
    await loadLedger();
  }

  async function createCategory(name: string, direction: 'income' | 'expense'): Promise<void> {
    await api.createCategory(selectedLedgerId, name, direction);
    notify('success', `分类“${name}”已创建`);
    await loadLedger();
  }

  async function createColumn(input: ColumnInput): Promise<void> {
    await api.createColumn(selectedLedgerId, input);
    notify('success', `列“${input.name}”已创建`);
    await loadLedger();
  }

  async function updateColumn(
    id: string,
    input: Partial<Pick<Column, 'name' | 'position' | 'width'>>
  ): Promise<void> {
    await api.updateColumn(id, input);
    notify('success', '列设置已保存');
    await loadLedger();
  }

  async function recycleColumn(id: string): Promise<void> {
    await api.recycleColumn(id);
    notify('success', '列已移入回收站');
    await loadLedger();
  }

  async function restoreRow(id: string): Promise<void> {
    await api.restoreRow(id);
    notify('success', '流水已恢复');
    await loadLedger();
  }

  async function restoreColumn(id: string): Promise<void> {
    await api.restoreColumn(id);
    notify('success', '列已恢复');
    await loadLedger();
  }

  async function createFunction(source: string): Promise<LuaFunction> {
    const saved = await api.createFunction({ source });
    functions = await api.functions();
    notify('success', `函数“${saved.name}”已保存`);
    return saved;
  }

  async function updateFunction(name: string, source: string): Promise<LuaFunction> {
    const saved = await api.updateFunction(name, { source });
    functions = await api.functions();
    notify('success', `函数“${saved.name}”已更新`);
    return saved;
  }

  async function createLedger(): Promise<void> {
    if (!newLedgerName.trim()) return;
    creatingLedger = true;
    try {
      const ledger = await api.createLedger(newLedgerName.trim());
      ledgers = [...ledgers, ledger];
      newLedgerName = '';
      newLedgerOpen = false;
      notify('success', `账本“${ledger.name}”已创建`);
      await chooseLedger(ledger.id);
    } catch (reason) {
      notify('error', errorMessage(reason));
    } finally {
      creatingLedger = false;
    }
  }

  function selectView(next: View): void {
    view = next;
    mobileMenuOpen = false;
  }

  async function logout(): Promise<void> {
    try {
      await api.logout();
    } finally {
      onLoggedOut();
    }
  }

  function connectEvents(): void {
    eventSource = new EventSource('/api/v1/events', { withCredentials: true });
    eventSource.addEventListener('lua.reload', async () => {
      try {
        functions = await api.functions();
        notify('success', '函数脚本已重新载入');
      } catch {
        // The regular connection error path enables polling below.
      }
    });
    eventSource.addEventListener('job.completed', () => loadLedger());
    eventSource.onerror = () => {
      if (!pollTimer) {
        pollTimer = setInterval(async () => {
          try {
            functions = await api.functions();
          } catch {
            // The next interval retries without interrupting ledger editing.
          }
        }, 30_000);
      }
    };
    eventSource.onopen = () => {
      if (pollTimer) clearInterval(pollTimer);
      pollTimer = null;
    };
  }

  onMount(() => {
    bootstrap();
    connectEvents();
  });

  onDestroy(() => {
    eventSource?.close();
    if (pollTimer) clearInterval(pollTimer);
  });
</script>

<a class="skip-link" href="#main-content">跳到主要内容</a>

<div class="app-shell">
  <aside class:mobile-open={mobileMenuOpen} class="sidebar">
    <div class="sidebar-brand">
      <span class="brand-icon"><Sprout size={20} /></span>
      <span><strong>JournalSeed</strong><small>网页账本</small></span>
      <button
        class="sidebar-close"
        type="button"
        aria-label="关闭菜单"
        on:click={() => (mobileMenuOpen = false)}
      >
        <X size={19} />
      </button>
    </div>

    <div class="ledger-switcher">
      <label for="ledger-select">当前账本</label>
      <div>
        <BookOpenText size={16} />
        <select
          id="ledger-select"
          value={selectedLedgerId}
          on:change={(event) => chooseLedger(event.currentTarget.value)}
        >
          {#each ledgers as ledger}<option value={ledger.id}>{ledger.name}</option>{/each}
        </select>
        <ChevronDown size={15} />
      </div>
      <button type="button" on:click={() => (newLedgerOpen = !newLedgerOpen)}>
        <Plus size={15} />新建账本
      </button>
      <div class:open={newLedgerOpen} class="new-ledger-form">
        <form on:submit|preventDefault={createLedger}>
          <input aria-label="新账本名称" bind:value={newLedgerName} placeholder="账本名称" />
          <button
            type="submit"
            aria-label="创建账本"
            title="创建账本"
            disabled={creatingLedger || !newLedgerName.trim()}
          >
            <Plus size={16} />
          </button>
        </form>
      </div>
    </div>

    <nav class="primary-nav" aria-label="主要导航">
      {#each navigation as item}
        <button class:active={view === item.id} type="button" on:click={() => selectView(item.id)}>
          <item.icon size={18} />
          <span>{item.label}</span>
          {#if item.id === 'recycle' && recycledRows.length + recycledColumns.length > 0}
            <small>{recycledRows.length + recycledColumns.length}</small>
          {/if}
        </button>
      {/each}
    </nav>

    <div class="sidebar-user">
      <span><CircleUserRound size={18} /></span>
      <div><strong>{session.user.username}</strong><small>管理员</small></div>
      <button type="button" aria-label="退出登录" title="退出登录" on:click={logout}
        ><LogOut size={17} /></button
      >
    </div>
  </aside>

  {#if mobileMenuOpen}
    <button
      class="mobile-backdrop"
      type="button"
      aria-label="关闭菜单"
      on:click={() => (mobileMenuOpen = false)}
    ></button>
  {/if}

  <div class="workspace">
    <header class="topbar">
      <div class="topbar-leading">
        <button
          class="icon-button menu-button"
          type="button"
          aria-label="打开菜单"
          on:click={() => (mobileMenuOpen = true)}
        >
          <Menu size={20} />
        </button>
        <div>
          <strong>{selectedLedger?.name ?? '账本'}</strong>
          <span>{navigation.find((item) => item.id === view)?.label}</span>
        </div>
      </div>
      <div class="topbar-status"><i></i><span>已连接</span></div>
      <button class="icon-button more-button" type="button" aria-label="更多" title="更多">
        <MoreHorizontal size={20} />
      </button>
    </header>

    <main id="main-content" tabindex="-1">
      {#if view === 'transactions'}
        <section class="transactions-view">
          <div class="summary-strip">
            <div class="summary-balance">
              <span>账户总余额</span>
              <strong class:negative={summary.balance.startsWith('-')}
                >{formatMoney(summary.balance)}</strong
              >
            </div>
            <div>
              <span>期间收入</span>
              <strong class="income-value">+{formatMoney(summary.income)}</strong>
            </div>
            <div>
              <span>期间支出</span>
              <strong class="expense-value">{formatMoney(summary.expense)}</strong>
            </div>
            <div>
              <span>流水数量</span>
              <strong>{summary.rowCount.toLocaleString('zh-CN')}</strong>
            </div>
          </div>

          <div class="table-toolbar">
            <div class="search-box">
              <Search size={17} />
              <label class="sr-only" for="row-search">搜索流水</label>
              <input id="row-search" bind:value={search} placeholder="搜索说明、账户或分类" />
              {#if search}<button type="button" aria-label="清除搜索" on:click={() => (search = '')}
                  ><X size={15} /></button
                >{/if}
            </div>
            <div class="toolbar-actions">
              <label class="sort-control">
                <ListFilter size={16} />
                <span class="sr-only">排序</span>
                <select bind:value={sort} on:change={() => changeSort(sort)}>
                  <option value="date:desc">日期：新到旧</option>
                  <option value="date:asc">日期：旧到新</option>
                  <option value="amount:desc">金额：高到低</option>
                  <option value="amount:asc">金额：低到高</option>
                </select>
              </label>
              <button class="button primary add-entry" type="button" on:click={openNewEntry}>
                <Plus size={18} /><span>新增流水</span>
              </button>
            </div>
          </div>

          <TransactionTable
            {columns}
            rows={visibleRows}
            {loading}
            {sort}
            onSort={changeSort}
            onSelect={openEntry}
            onCreate={openNewEntry}
          />

          {#if hasMore && !search}
            <div class="load-more-row">
              <button class="button" type="button" disabled={loadingMore} on:click={loadMore}>
                <RefreshCw class={loadingMore ? 'spinning' : ''} size={16} />{loadingMore
                  ? '正在载入'
                  : '载入更多'}
              </button>
            </div>
          {/if}
        </section>
      {:else if view === 'accounts'}
        <AccountsPanel
          {accounts}
          {categories}
          onCreateAccount={createAccount}
          onCreateCategory={createCategory}
        />
      {:else if view === 'columns'}
        <ColumnsPanel
          {columns}
          onCreate={createColumn}
          onUpdate={updateColumn}
          onRecycle={recycleColumn}
        />
      {:else if view === 'functions'}
        <FunctionsPanel
          {functions}
          onInvoke={(name, input) => api.invokeFunction(name, input)}
          onCreate={createFunction}
          onUpdate={updateFunction}
        />
      {:else}
        <RecyclePanel
          rows={recycledRows}
          columns={recycledColumns}
          onRestoreRow={restoreRow}
          onRestoreColumn={restoreColumn}
        />
      {/if}
    </main>
  </div>
</div>

<nav class="mobile-nav" aria-label="手机导航">
  {#each navigation as item}
    <button class:active={view === item.id} type="button" on:click={() => selectView(item.id)}>
      <item.icon size={19} /><span>{item.label}</span>
    </button>
  {/each}
</nav>

{#if entryDrawerOpen}
  <EntryDrawer
    row={selectedRow}
    {accounts}
    {categories}
    {columns}
    onClose={() => {
      entryDrawerOpen = false;
      selectedRow = null;
    }}
    onSave={saveEntry}
    onRecycle={selectedRow ? recycleSelectedRow : null}
  />
{/if}

<ToastRegion {toasts} dismiss={dismissToast} />

<style>
  .app-shell {
    display: grid;
    min-height: 100vh;
    min-height: 100dvh;
    grid-template-columns: 204px minmax(0, 1fr);
    background: var(--surface-subtle);
  }

  .sidebar {
    position: sticky;
    z-index: 120;
    top: 0;
    display: grid;
    height: 100vh;
    height: 100dvh;
    grid-template-rows: 60px auto minmax(0, 1fr) 58px;
    color: oklch(87% 0.018 150);
    background: var(--surface-dark);
  }

  .sidebar-brand {
    display: grid;
    grid-template-columns: 32px minmax(0, 1fr) auto;
    align-items: center;
    gap: 8px;
    padding: 10px 12px;
    border-bottom: 1px solid oklch(34% 0.026 155);
  }

  .brand-icon {
    display: grid;
    place-items: center;
    width: 30px;
    height: 30px;
    border: 1px solid oklch(48% 0.04 150);
    border-radius: var(--radius-sm);
    color: var(--accent);
  }

  .sidebar-brand > span:nth-child(2),
  .sidebar-user > div {
    display: grid;
    min-width: 0;
    line-height: 1.2;
  }

  .sidebar-brand strong {
    color: oklch(96% 0.008 150);
    font-size: 0.875rem;
  }

  .sidebar-brand small,
  .sidebar-user small {
    margin-top: 3px;
    color: oklch(67% 0.022 150);
    font-size: 0.6875rem;
  }

  .sidebar-close {
    display: none;
  }

  .ledger-switcher {
    display: grid;
    gap: 5px;
    padding: 12px;
    border-bottom: 1px solid oklch(34% 0.026 155);
  }

  .ledger-switcher > label {
    color: oklch(64% 0.025 150);
    font-size: 0.6875rem;
    font-weight: 650;
  }

  .ledger-switcher > div:first-of-type {
    position: relative;
    display: grid;
    grid-template-columns: 20px minmax(0, 1fr) 18px;
    align-items: center;
    gap: 4px;
    min-height: 36px;
    padding: 0 8px;
    border: 1px solid oklch(42% 0.032 155);
    border-radius: var(--radius-sm);
    background: var(--surface-dark-raised);
  }

  .ledger-switcher select {
    width: 100%;
    min-width: 0;
    height: 34px;
    padding: 0;
    border: 0;
    appearance: none;
    color: oklch(94% 0.012 150);
    background: transparent;
    font-size: 0.8125rem;
    font-weight: 650;
  }

  .ledger-switcher > button {
    display: inline-flex;
    align-items: center;
    justify-content: flex-start;
    gap: 5px;
    min-height: 30px;
    padding: 4px 6px;
    border: 0;
    color: oklch(70% 0.03 150);
    background: transparent;
    font-size: 0.75rem;
  }

  .new-ledger-form {
    display: grid;
    grid-template-rows: 0fr;
    transition: grid-template-rows var(--duration-state) var(--ease-out);
  }

  .new-ledger-form.open {
    grid-template-rows: 1fr;
  }

  .new-ledger-form form {
    display: grid;
    min-height: 0;
    grid-template-columns: minmax(0, 1fr) 32px;
    overflow: hidden;
  }

  .new-ledger-form input {
    min-width: 0;
    height: 32px;
    padding: 5px 7px;
    border: 1px solid oklch(45% 0.035 155);
    border-radius: var(--radius-sm) 0 0 var(--radius-sm);
    color: oklch(93% 0.012 150);
    background: var(--surface-dark-raised);
  }

  .new-ledger-form form button {
    display: grid;
    place-items: center;
    height: 32px;
    padding: 0;
    border: 1px solid oklch(45% 0.035 155);
    border-left: 0;
    border-radius: 0 var(--radius-sm) var(--radius-sm) 0;
    color: var(--accent);
    background: var(--surface-dark-raised);
  }

  .primary-nav {
    display: grid;
    align-content: start;
    gap: 2px;
    overflow-y: auto;
    padding: 10px 8px;
  }

  .primary-nav button {
    display: grid;
    min-height: 36px;
    grid-template-columns: 24px minmax(0, 1fr) auto;
    align-items: center;
    gap: 6px;
    padding: 6px 9px;
    border: 0;
    border-radius: var(--radius-sm);
    color: oklch(70% 0.024 150);
    background: transparent;
    font-size: 0.8125rem;
    text-align: left;
  }

  .primary-nav button.active {
    color: oklch(96% 0.01 150);
    background: var(--surface-dark-raised);
  }

  .primary-nav button.active :global(svg) {
    color: var(--accent);
  }

  .primary-nav small {
    display: inline-grid;
    place-items: center;
    min-width: 20px;
    height: 20px;
    padding-inline: 5px;
    border-radius: 10px;
    color: oklch(88% 0.02 150);
    background: oklch(38% 0.034 155);
    font-size: 0.6875rem;
  }

  .sidebar-user {
    display: grid;
    grid-template-columns: 32px minmax(0, 1fr) 32px;
    align-items: center;
    gap: 6px;
    padding: 8px 12px max(8px, env(safe-area-inset-bottom));
    border-top: 1px solid oklch(34% 0.026 155);
  }

  .sidebar-user > span {
    display: grid;
    place-items: center;
    color: var(--accent);
  }

  .sidebar-user strong {
    overflow: hidden;
    color: oklch(92% 0.012 150);
    font-size: 0.75rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .sidebar-user > button {
    display: grid;
    place-items: center;
    width: 32px;
    height: 32px;
    padding: 0;
    border: 0;
    border-radius: var(--radius-sm);
    color: oklch(65% 0.025 150);
    background: transparent;
  }

  .workspace {
    min-width: 0;
  }

  .topbar {
    position: sticky;
    z-index: 80;
    top: 0;
    display: flex;
    align-items: center;
    justify-content: space-between;
    height: 52px;
    padding: 0 16px;
    border-bottom: 1px solid var(--line);
    background: var(--surface-raised);
  }

  .topbar-leading {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .topbar-leading > div {
    display: flex;
    align-items: baseline;
    min-width: 0;
    gap: 9px;
  }

  .topbar-leading strong {
    overflow: hidden;
    color: var(--ink-strong);
    font-size: 0.9375rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .topbar-leading span {
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .menu-button,
  .more-button {
    display: none;
  }

  .topbar-status {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .topbar-status i {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--income);
  }

  main {
    min-height: calc(100vh - 52px);
    min-height: calc(100dvh - 52px);
    padding: 12px;
  }

  main > :global(*) {
    overflow: hidden;
    border: 1px solid var(--line);
    border-radius: var(--radius-md);
  }

  .transactions-view {
    overflow: hidden;
    border: 1px solid var(--line);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
  }

  .summary-strip {
    display: grid;
    grid-template-columns: minmax(180px, 1.35fr) repeat(3, minmax(110px, 1fr));
    min-height: 64px;
    border-bottom: 1px solid var(--line);
  }

  .summary-strip > div {
    display: grid;
    align-content: center;
    gap: 3px;
    min-width: 0;
    padding: 10px 16px;
  }

  .summary-strip > div + div {
    border-left: 1px solid var(--line);
  }

  .summary-strip span {
    color: var(--ink-muted);
    font-size: 0.6875rem;
  }

  .summary-strip strong {
    overflow: hidden;
    color: var(--ink-strong);
    font-size: 0.9375rem;
    font-variant-numeric: tabular-nums;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .summary-strip .summary-balance strong {
    font-size: 1.125rem;
  }

  .summary-strip .income-value {
    color: var(--income);
  }

  .summary-strip .expense-value,
  .summary-strip strong.negative {
    color: var(--expense);
  }

  .table-toolbar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 10px;
    min-height: 48px;
    padding: 7px 10px;
    border-bottom: 1px solid var(--line);
  }

  .search-box {
    position: relative;
    display: flex;
    align-items: center;
    width: min(320px, 42vw);
    color: var(--ink-muted);
  }

  .search-box > :global(svg) {
    position: absolute;
    left: 9px;
    pointer-events: none;
  }

  .search-box input {
    width: 100%;
    height: var(--control-height-sm);
    padding: 6px 32px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    color: var(--ink-strong);
    background: var(--surface-subtle);
    font-size: 0.8125rem;
  }

  .search-box input::placeholder {
    color: var(--ink-muted);
  }

  .search-box button {
    position: absolute;
    right: 4px;
    display: grid;
    place-items: center;
    width: 28px;
    height: 28px;
    padding: 0;
    border: 0;
    color: var(--ink-muted);
    background: transparent;
  }

  .toolbar-actions {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .sort-control {
    display: grid;
    height: var(--control-height-sm);
    grid-template-columns: 20px minmax(0, 1fr);
    align-items: center;
    gap: 4px;
    padding: 0 8px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    color: var(--ink-muted);
    background: var(--surface-raised);
  }

  .sort-control select {
    height: 32px;
    padding: 0;
    border: 0;
    color: var(--ink);
    background: transparent;
    font-size: 0.75rem;
  }

  .add-entry {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-height: var(--control-height-sm);
    padding-block: 5px;
    font-size: 0.8125rem;
  }

  .load-more-row {
    display: flex;
    justify-content: center;
    padding: 8px;
    border-top: 1px solid var(--line);
  }

  .load-more-row .button {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-height: 34px;
    font-size: 0.8125rem;
  }

  :global(.spinning) {
    animation: spin 700ms linear infinite;
  }

  .mobile-nav,
  .mobile-backdrop {
    display: none;
  }

  @media (hover: hover) {
    .primary-nav button:hover,
    .sidebar-user > button:hover,
    .ledger-switcher > button:hover {
      color: oklch(94% 0.012 150);
      background: var(--surface-dark-raised);
    }
  }

  @media (pointer: coarse) {
    .search-box input,
    .sort-control {
      min-height: 44px;
    }

    .sort-control {
      min-width: 44px;
    }
  }

  @media (max-width: 820px) {
    .app-shell {
      display: block;
      padding-bottom: calc(58px + env(safe-area-inset-bottom));
    }

    .sidebar {
      position: fixed;
      top: 0;
      bottom: 0;
      left: 0;
      width: min(280px, 86vw);
      height: 100dvh;
      transform: translateX(-105%);
      transition: transform 300ms var(--ease-out);
    }

    .sidebar.mobile-open {
      transform: translateX(0);
    }

    .sidebar-close {
      display: grid;
      place-items: center;
      width: 34px;
      height: 34px;
      padding: 0;
      border: 0;
      color: oklch(74% 0.02 150);
      background: transparent;
    }

    .mobile-backdrop {
      position: fixed;
      z-index: 110;
      inset: 0;
      display: block;
      width: 100%;
      height: 100%;
      padding: 0;
      border: 0;
      background: oklch(18% 0.02 155 / 0.38);
    }

    .topbar {
      height: 54px;
      padding-inline: 8px 12px;
    }

    .menu-button,
    .more-button {
      display: grid;
    }

    .topbar-status {
      display: none;
    }

    main {
      min-height: calc(100dvh - 112px);
      padding: 0;
    }

    main > :global(*),
    .transactions-view {
      border-right: 0;
      border-left: 0;
      border-radius: 0;
    }

    .mobile-nav {
      position: fixed;
      z-index: 100;
      right: 0;
      bottom: 0;
      left: 0;
      display: grid;
      height: calc(58px + env(safe-area-inset-bottom));
      grid-template-columns: repeat(5, minmax(0, 1fr));
      padding: 4px 4px env(safe-area-inset-bottom);
      border-top: 1px solid var(--line);
      background: var(--surface-raised);
    }

    .mobile-nav button {
      position: relative;
      display: grid;
      place-items: center;
      align-content: center;
      gap: 2px;
      min-width: 0;
      padding: 3px 1px;
      border: 0;
      color: var(--ink-muted);
      background: transparent;
      font-size: 0.625rem;
    }

    .mobile-nav button.active {
      color: var(--accent-strong);
    }

    .mobile-nav button.active::before {
      position: absolute;
      top: -5px;
      width: 28px;
      height: 2px;
      background: var(--accent-strong);
      content: '';
    }
  }

  @media (max-width: 620px) {
    .summary-strip {
      grid-template-columns: repeat(3, minmax(0, 1fr));
      min-height: 68px;
    }

    .summary-strip .summary-balance {
      grid-column: 1 / -1;
      min-height: 72px;
      border-bottom: 1px solid var(--line);
    }

    .summary-strip > div {
      padding: 10px 12px;
    }

    .summary-strip > div:nth-child(2) {
      border-left: 0;
    }

    .summary-strip .summary-balance strong {
      font-size: 1.25rem;
    }

    .table-toolbar {
      align-items: stretch;
      gap: 8px;
      padding: 8px;
    }

    .search-box {
      width: 100%;
    }

    .sort-control {
      width: 40px;
      grid-template-columns: 1fr;
      place-items: center;
      padding: 0;
    }

    .sort-control select {
      position: absolute;
      width: 40px;
      opacity: 0;
    }

    .add-entry {
      width: 40px;
      padding: 0;
    }

    .add-entry span {
      display: none;
    }
  }
</style>
