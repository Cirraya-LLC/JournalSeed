<script lang="ts">
  import {
    ArrowDown,
    ArrowUp,
    CalendarDays,
    CheckSquare,
    ChevronDown,
    FileDigit,
    Link2,
    ListChecks,
    LoaderCircle,
    Plus,
    Sigma,
    TextCursorInput,
    Trash2
  } from 'lucide-svelte';
  import { errorMessage } from '$lib/format';
  import type { Column, ColumnInput, ColumnType } from '$lib/types';

  export let columns: Column[];
  export let onCreate: (input: ColumnInput) => Promise<void>;
  export let onUpdate: (
    id: string,
    input: Partial<Pick<Column, 'name' | 'position' | 'width'>>
  ) => Promise<void>;
  export let onRecycle: (id: string) => Promise<void>;

  const typeOptions: Array<{
    value: Exclude<ColumnType, 'money'>;
    label: string;
  }> = [
    { value: 'text', label: '文本' },
    { value: 'number', label: '数字' },
    { value: 'date', label: '日期' },
    { value: 'boolean', label: '复选框' },
    { value: 'option', label: '单选' },
    { value: 'relation', label: '关联' },
    { value: 'formula', label: '公式' }
  ];

  let name = '';
  let type: Exclude<ColumnType, 'money'> = 'text';
  let decimalPlaces = 2;
  let formulaSource = '';
  let saving = false;
  let busyId = '';
  let error = '';

  $: ordered = [...columns].sort((left, right) => left.position - right.position);

  function iconFor(columnType: ColumnType) {
    return columnType === 'number' || columnType === 'money'
      ? FileDigit
      : columnType === 'date'
        ? CalendarDays
        : columnType === 'boolean'
          ? CheckSquare
          : columnType === 'option'
            ? ListChecks
            : columnType === 'relation'
              ? Link2
              : columnType === 'formula'
                ? Sigma
                : TextCursorInput;
  }

  function typeLabel(columnType: ColumnType): string {
    if (columnType === 'money') return '金额';
    return typeOptions.find((option) => option.value === columnType)?.label ?? columnType;
  }

  async function createColumn(): Promise<void> {
    error = '';
    if (!name.trim()) {
      error = '请填写列名称';
      return;
    }
    if (type === 'formula' && !formulaSource.trim()) {
      error = '请填写公式表达式';
      return;
    }
    saving = true;
    try {
      await onCreate({
        name: name.trim(),
        type,
        ...(type === 'number' ? { decimalPlaces } : {}),
        ...(type === 'formula'
          ? {
              formulaSource: formulaSource.trim(),
              formulaResultType: 'number',
              formulaDependencies: []
            }
          : {})
      });
      name = '';
      formulaSource = '';
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      saving = false;
    }
  }

  async function updateColumn(
    column: Column,
    input: Partial<Pick<Column, 'name' | 'position' | 'width'>>
  ): Promise<void> {
    busyId = column.id;
    error = '';
    try {
      await onUpdate(column.id, input);
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      busyId = '';
    }
  }

  async function move(column: Column, delta: -1 | 1): Promise<void> {
    const index = ordered.findIndex((item) => item.id === column.id);
    const other = ordered[index + delta];
    if (!other) return;
    // The server performs the position swap transactionally when it receives
    // a position occupied by another active column.
    await updateColumn(column, { position: other.position });
  }

  async function recycle(column: Column): Promise<void> {
    busyId = column.id;
    error = '';
    try {
      await onRecycle(column.id);
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      busyId = '';
    }
  }
</script>

<div class="columns-view">
  <header class="view-header">
    <div>
      <span>表格结构</span>
      <h2>列设置</h2>
    </div>
  </header>

  <div class="columns-layout">
    <section class="column-list-section">
      <header class="section-header">
        <div>
          <h3>当前列</h3>
          <span>金额列固定保留，其余列可恢复删除</span>
        </div>
      </header>

      <div class="column-list">
        <div class="column-heading">
          <span>顺序</span><span>列名</span><span>类型</span><span>宽度</span><span>操作</span>
        </div>
        {#each ordered as column, index (column.id)}
          {@const Icon = iconFor(column.type)}
          <div class="column-row" class:busy={busyId === column.id}>
            <div class="order-actions">
              <button
                type="button"
                aria-label={`上移 ${column.name}`}
                title="上移"
                disabled={index === 0 || busyId === column.id}
                on:click={() => move(column, -1)}><ArrowUp size={15} /></button
              >
              <button
                type="button"
                aria-label={`下移 ${column.name}`}
                title="下移"
                disabled={index === ordered.length - 1 || busyId === column.id}
                on:click={() => move(column, 1)}><ArrowDown size={15} /></button
              >
            </div>
            <div class="name-field">
              <Icon size={17} />
              <input
                aria-label={`${column.name} 列名`}
                value={column.name}
                on:change={(event) => {
                  const nextName = (event.currentTarget as HTMLInputElement).value.trim();
                  if (nextName && nextName !== column.name)
                    updateColumn(column, { name: nextName });
                }}
              />
            </div>
            <span class="type-value">{typeLabel(column.type)}</span>
            <div class="width-field">
              <input
                aria-label={`${column.name} 列宽`}
                type="number"
                min="72"
                max="720"
                value={column.width}
                on:change={(event) =>
                  updateColumn(column, {
                    width: Number((event.currentTarget as HTMLInputElement).value)
                  })}
              />
              <span>px</span>
            </div>
            <button
              class="delete-button"
              type="button"
              aria-label={`移除 ${column.name}`}
              title={column.system === 'amount' ? '金额列始终保留' : '移入回收站'}
              disabled={column.system === 'amount' || busyId === column.id}
              on:click={() => recycle(column)}
            >
              {#if busyId === column.id}<LoaderCircle
                  class="spinner-icon"
                  size={16}
                />{:else}<Trash2 size={16} />{/if}
            </button>
          </div>
        {/each}
      </div>
    </section>

    <section class="new-column-section">
      <header class="section-header">
        <div>
          <h3>新增自定义列</h3>
          <span>选择与内容匹配的数据类型</span>
        </div>
      </header>
      <form class="new-column-form" on:submit|preventDefault={createColumn}>
        <div class="field">
          <label for="column-name">列名称</label>
          <input id="column-name" class="input" bind:value={name} placeholder="例如：项目" />
        </div>
        <div class="field select-wrap">
          <label for="column-type">数据类型</label>
          <select id="column-type" class="select" bind:value={type}>
            {#each typeOptions as option}<option value={option.value}>{option.label}</option>{/each}
          </select>
          <ChevronDown size={16} aria-hidden="true" />
        </div>
        {#if type === 'number'}
          <div class="field">
            <label for="decimal-places">小数位数</label>
            <input
              id="decimal-places"
              class="input"
              type="number"
              min="0"
              max="18"
              bind:value={decimalPlaces}
            />
          </div>
        {/if}
        {#if type === 'formula'}
          <div class="field">
            <label for="formula-source">公式表达式</label>
            <textarea
              id="formula-source"
              class="textarea formula-input"
              bind:value={formulaSource}
              placeholder={'return sum({ field = "金额", where = { op = "gt", value = dec("0") } })'}
            ></textarea>
          </div>
        {/if}
        {#if error}<p class="field-error" role="alert">{error}</p>{/if}
        <button class="button primary create-button" type="submit" disabled={saving}>
          {#if saving}<LoaderCircle class="spinner-icon" size={17} />正在创建{:else}<Plus
              size={17}
            />创建列{/if}
        </button>
      </form>
    </section>
  </div>
</div>

<style>
  .columns-view {
    min-height: 100%;
    background: var(--surface-raised);
  }

  .view-header {
    display: flex;
    align-items: center;
    min-height: 64px;
    padding: 12px 20px;
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
  h3 {
    margin: 0;
    color: var(--ink-strong);
  }

  h2 {
    font-size: 1.25rem;
  }

  h3 {
    font-size: 1rem;
  }

  .columns-layout {
    display: grid;
    grid-template-columns: minmax(560px, 1fr) minmax(280px, 360px);
  }

  section {
    min-width: 0;
    padding: 18px 20px;
  }

  .new-column-section {
    border-left: 1px solid var(--line);
    background: var(--surface-subtle);
  }

  .section-header {
    min-height: 38px;
    margin-bottom: 14px;
  }

  .column-list {
    font-size: 0.8125rem;
  }

  .column-heading,
  .column-row {
    display: grid;
    grid-template-columns: 72px minmax(150px, 1fr) 80px 100px 40px;
    align-items: center;
    gap: 8px;
    min-height: 40px;
    border-bottom: 1px solid var(--line);
  }

  .column-heading {
    min-height: 34px;
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .column-row.busy {
    opacity: 0.62;
  }

  .order-actions {
    display: flex;
    gap: 2px;
  }

  .order-actions button,
  .delete-button {
    display: grid;
    place-items: center;
    width: 30px;
    height: 30px;
    padding: 0;
    border: 0;
    border-radius: var(--radius-sm);
    color: var(--ink-muted);
    background: transparent;
  }

  .name-field {
    display: grid;
    grid-template-columns: 20px minmax(0, 1fr);
    align-items: center;
    gap: 6px;
    color: var(--accent-strong);
  }

  .name-field input,
  .width-field input {
    width: 100%;
    min-width: 0;
    height: 32px;
    padding: 4px 6px;
    border: 1px solid transparent;
    border-radius: var(--radius-sm);
    color: var(--ink-strong);
    background: transparent;
  }

  .name-field input:focus,
  .width-field input:focus {
    border-color: var(--line-strong);
    background: var(--surface-raised);
  }

  .type-value {
    color: var(--ink-muted);
  }

  .width-field {
    display: grid;
    grid-template-columns: minmax(0, 1fr) 20px;
    align-items: center;
    gap: 3px;
    color: var(--ink-muted);
    font-variant-numeric: tabular-nums;
  }

  .delete-button:hover:not(:disabled) {
    color: var(--expense);
    background: var(--expense-soft);
  }

  .new-column-form {
    display: grid;
    gap: 12px;
  }

  .select-wrap {
    position: relative;
  }

  .select-wrap :global(svg) {
    position: absolute;
    right: 10px;
    bottom: 10px;
    color: var(--ink-muted);
    pointer-events: none;
  }

  .select-wrap select {
    appearance: none;
  }

  .formula-input {
    min-height: 116px;
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.8125rem;
  }

  .create-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }

  @media (max-width: 1060px) {
    .columns-layout {
      grid-template-columns: 1fr;
    }

    .new-column-section {
      border-top: 1px solid var(--line);
      border-left: 0;
    }
  }

  @media (max-width: 680px) {
    .view-header,
    section {
      padding-inline: 16px;
    }

    .column-list {
      overflow-x: auto;
    }

    .column-heading,
    .column-row {
      width: 620px;
    }
  }
</style>
