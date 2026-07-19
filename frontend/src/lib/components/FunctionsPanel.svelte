<script lang="ts">
  import { Braces, Check, LoaderCircle, Play, RotateCw } from 'lucide-svelte';
  import { errorMessage } from '$lib/format';
  import type { LuaFunction } from '$lib/types';

  export let functions: LuaFunction[];
  export let onInvoke: (
    name: string,
    input: Record<string, unknown>
  ) => Promise<Record<string, unknown>>;

  let selectedName = '';
  let values: Record<string, string | boolean> = {};
  let invoking = false;
  let result: Record<string, unknown> | null = null;
  let error = '';

  $: selected = functions.find((item) => item.name === selectedName) ?? functions[0] ?? null;
  $: if (!selectedName && functions.length > 0) selectedName = functions[0].name;

  function choose(item: LuaFunction): void {
    selectedName = item.name;
    values = {};
    result = null;
    error = '';
  }

  async function invoke(): Promise<void> {
    if (!selected) return;
    error = '';
    result = null;
    for (const parameter of selected.params) {
      if (parameter.required && (values[parameter.name] === '' || values[parameter.name] == null)) {
        error = `请填写${parameter.label}`;
        return;
      }
    }
    invoking = true;
    try {
      result = await onInvoke(selected.name, values);
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      invoking = false;
    }
  }
</script>

<div class="functions-view">
  <header class="view-header">
    <div>
      <span>自动化</span>
      <h2>命名函数</h2>
    </div>
    <span class="registry-status"><i></i>{functions.length} 个函数已载入</span>
  </header>

  <div class="functions-layout">
    <nav class="function-list" aria-label="函数列表">
      {#each functions as item}
        <button
          class:active={selected?.name === item.name}
          type="button"
          on:click={() => choose(item)}
        >
          <span class="function-icon"><Braces size={18} /></span>
          <span><strong>{item.name}</strong><small>v{item.version}</small></span>
        </button>
      {:else}
        <div class="function-empty"><Braces size={22} /><span>函数目录中还没有有效脚本</span></div>
      {/each}
    </nav>

    <section class="runner">
      {#if selected}
        <header class="runner-header">
          <div>
            <span>v{selected.version}</span>
            <h3>{selected.name}</h3>
          </div>
          <p>{selected.description}</p>
        </header>

        <form on:submit|preventDefault={invoke} class="runner-form">
          <div class="parameter-grid">
            {#each selected.params as parameter}
              <div class="field">
                <label for={`lua-${parameter.name}`}>{parameter.label}</label>
                {#if parameter.type === 'boolean'}
                  <label class="checkbox-row" for={`lua-${parameter.name}`}>
                    <input
                      id={`lua-${parameter.name}`}
                      type="checkbox"
                      checked={values[parameter.name] === true}
                      on:change={(event) =>
                        (values[parameter.name] = (
                          event.currentTarget as HTMLInputElement
                        ).checked)}
                    />
                    <span>启用</span>
                  </label>
                {:else if parameter.type === 'option'}
                  <select
                    id={`lua-${parameter.name}`}
                    class="select"
                    value={String(values[parameter.name] ?? '')}
                    on:change={(event) =>
                      (values[parameter.name] = (event.currentTarget as HTMLSelectElement).value)}
                    required={parameter.required}
                  >
                    <option value="">选择</option>
                    {#each parameter.options ?? [] as option}<option value={option}>{option}</option
                      >{/each}
                  </select>
                {:else}
                  <input
                    id={`lua-${parameter.name}`}
                    class="input"
                    type={parameter.type === 'date' ? 'date' : 'text'}
                    inputmode={parameter.type === 'number' ? 'decimal' : 'text'}
                    value={String(values[parameter.name] ?? '')}
                    on:input={(event) =>
                      (values[parameter.name] = (event.currentTarget as HTMLInputElement).value)}
                    required={parameter.required}
                  />
                {/if}
              </div>
            {/each}
          </div>
          {#if error}<p class="field-error" role="alert">{error}</p>{/if}
          <button class="button primary run-button" type="submit" disabled={invoking}>
            {#if invoking}<LoaderCircle class="spinner-icon" size={18} />正在执行{:else}<Play
                size={17}
              />预览结果{/if}
          </button>
        </form>

        <section class="result-section" aria-live="polite">
          <header>
            <span>执行结果</span>{#if result}<Check size={16} />{/if}
          </header>
          {#if result}
            <pre>{JSON.stringify(result, null, 2)}</pre>
          {:else}
            <div class="result-empty"><RotateCw size={20} /><span>结果将在这里预览</span></div>
          {/if}
        </section>
      {:else}
        <div class="runner-empty"><Braces size={28} /><strong>等待函数脚本</strong></div>
      {/if}
    </section>
  </div>
</div>

<style>
  .functions-view {
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
  }

  .view-header > div,
  .runner-header > div {
    display: grid;
    gap: 2px;
  }

  .view-header > div > span,
  .runner-header span {
    color: var(--accent-strong);
    font-size: 0.75rem;
    font-weight: 750;
  }

  h2,
  h3,
  p {
    margin: 0;
  }

  h2 {
    font-size: 1.25rem;
  }

  h3 {
    font-size: 1rem;
  }

  .registry-status {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .registry-status i {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--income);
    box-shadow: 0 0 0 3px var(--income-soft);
  }

  .functions-layout {
    display: grid;
    min-height: calc(100vh - 116px);
    grid-template-columns: 244px minmax(0, 1fr);
  }

  .function-list {
    padding: 10px;
    border-right: 1px solid var(--line);
    background: var(--surface-subtle);
  }

  .function-list > button {
    display: grid;
    width: 100%;
    min-height: 48px;
    grid-template-columns: 32px minmax(0, 1fr);
    align-items: center;
    gap: 8px;
    padding: 6px 8px;
    border: 0;
    border-radius: var(--radius-sm);
    color: var(--ink);
    background: transparent;
    text-align: left;
  }

  .function-list > button.active {
    color: var(--ink-strong);
    background: var(--surface-raised);
    box-shadow: 0 1px 3px oklch(22% 0.02 155 / 0.08);
  }

  .function-icon {
    display: grid;
    place-items: center;
    width: 30px;
    height: 30px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    color: var(--accent-strong);
  }

  .function-list button > span:last-child {
    display: grid;
    min-width: 0;
  }

  .function-list strong,
  .function-list small {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .function-list strong {
    font-size: 0.875rem;
  }

  .function-list small {
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .function-empty,
  .runner-empty,
  .result-empty {
    display: grid;
    place-items: center;
    gap: 8px;
    color: var(--ink-muted);
    font-size: 0.8125rem;
    text-align: center;
  }

  .function-empty {
    padding: 32px 14px;
  }

  .runner {
    min-width: 0;
    padding: 22px;
  }

  .runner-header {
    display: grid;
    grid-template-columns: minmax(160px, 0.4fr) minmax(220px, 1fr);
    gap: 18px;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--line);
  }

  .runner-header p {
    align-self: end;
    max-width: 65ch;
    color: var(--ink-muted);
    font-size: 0.875rem;
  }

  .runner-form {
    display: grid;
    gap: 14px;
    padding-block: 18px;
  }

  .parameter-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 12px;
  }

  .checkbox-row {
    display: flex;
    align-items: center;
    gap: 9px;
    min-height: var(--control-height);
    padding: 7px 9px;
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

  .run-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    justify-self: start;
    gap: 7px;
    min-width: 132px;
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }

  .result-section {
    border-top: 1px solid var(--line);
  }

  .result-section > header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 42px;
    color: var(--ink-muted);
    font-size: 0.75rem;
    font-weight: 700;
  }

  .result-section > header :global(svg) {
    color: var(--income);
  }

  pre {
    min-height: 148px;
    overflow: auto;
    margin: 0;
    padding: 16px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    color: oklch(89% 0.035 145);
    background: var(--surface-dark);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.8125rem;
    line-height: 1.6;
  }

  .result-empty {
    min-height: 148px;
    border: 1px dashed var(--line-strong);
    border-radius: var(--radius-sm);
  }

  .runner-empty {
    min-height: 280px;
  }

  @media (max-width: 760px) {
    .view-header {
      padding-inline: 16px;
    }

    .functions-layout {
      grid-template-columns: 1fr;
    }

    .function-list {
      display: flex;
      overflow-x: auto;
      border-right: 0;
      border-bottom: 1px solid var(--line);
    }

    .function-list > button {
      width: 210px;
      flex: 0 0 210px;
    }

    .runner {
      padding: 20px 16px;
    }

    .runner-header {
      grid-template-columns: 1fr;
      gap: 8px;
    }
  }
</style>
