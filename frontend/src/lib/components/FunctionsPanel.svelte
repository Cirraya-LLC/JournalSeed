<script lang="ts">
  import {
    BookOpenText,
    Braces,
    Check,
    ChevronDown,
    Code2,
    LoaderCircle,
    PencilLine,
    Play,
    Plus,
    RotateCw,
    Save
  } from 'lucide-svelte';
  import { errorMessage } from '$lib/format';
  import type { LuaFunction } from '$lib/types';

  export let functions: LuaFunction[];
  export let onInvoke: (
    name: string,
    input: Record<string, unknown>
  ) => Promise<Record<string, unknown>>;
  export let onCreate: (source: string) => Promise<LuaFunction>;
  export let onUpdate: (name: string, source: string) => Promise<LuaFunction>;

  let selectedName = '';
  let values: Record<string, string | boolean> = {};
  let invoking = false;
  let result: Record<string, unknown> | null = null;
  let error = '';
  let mode: 'run' | 'edit' = 'run';
  let creatingFunction = false;
  let editorTargetName: string | null = null;
  let editorSource = '';
  let editorError = '';
  let savingFunction = false;
  let tutorialOpen = true;

  $: selected = creatingFunction
    ? null
    : (functions.find((item) => item.name === selectedName) ?? functions[0] ?? null);
  $: if (!creatingFunction && !selectedName && functions.length > 0)
    selectedName = functions[0].name;

  function uniqueFunctionName(base: string): string {
    const names = new Set(functions.map((item) => item.name));
    let index = 1;
    let name = base;
    while (names.has(name)) name = `${base}_${++index}`;
    return name;
  }

  function functionTemplate(): string {
    const name = uniqueFunctionName('custom_function');
    return `return {
  name = "${name}",
  version = "1.0.0",
  description = "自定义函数：预览后可把结果回填到流水。",
  params = {
    { name = "amount", type = "number", label = "金额", required = true },
    { name = "description", type = "text", label = "说明", required = false }
  },
  run = function(params)
    local amount = dec(params.amount or "0")
    return {
      amount = tostring(amount),
      description = params.description or ""
    }
  end
}
`;
  }

  function tutorialTemplate(): string {
    const name = uniqueFunctionName('tutorial_cashback');
    return `return {
  name = "${name}",
  version = "1.0.0",
  description = "教程示例：按比例计算返现，并返回可回填字段。",
  params = {
    { name = "amount", type = "number", label = "原始金额", required = true },
    { name = "rate", type = "number", label = "返现比例", required = true },
    { name = "memo", type = "text", label = "备注", required = false }
  },
  run = function(params)
    local amount = dec(params.amount or "0")
    local rate = dec(params.rate or "0")
    local cashback = amount * rate
    return {
      amount = tostring(cashback),
      description = params.memo or "返现",
      note = "预览不会直接修改流水，确认后再手动回填。"
    }
  end
}
`;
  }

  function useTutorialExample(): void {
    selectedName = '';
    creatingFunction = true;
    mode = 'edit';
    editorTargetName = null;
    editorSource = tutorialTemplate();
    editorError = '';
    error = '';
    result = null;
    tutorialOpen = true;
  }

  function choose(item: LuaFunction): void {
    selectedName = item.name;
    creatingFunction = false;
    mode = 'run';
    values = {};
    result = null;
    error = '';
    editorError = '';
  }

  function startCreate(): void {
    selectedName = '';
    creatingFunction = true;
    mode = 'edit';
    editorTargetName = null;
    editorSource = functionTemplate();
    editorError = '';
    error = '';
    result = null;
  }

  function startEdit(): void {
    if (!selected) return;
    creatingFunction = false;
    mode = 'edit';
    editorTargetName = selected.name;
    editorSource = selected.source || functionTemplate();
    editorError = '';
  }

  function cancelEdit(): void {
    creatingFunction = false;
    mode = 'run';
    editorTargetName = null;
    editorError = '';
    if (!selectedName && functions[0]) selectedName = functions[0].name;
  }

  async function saveSource(): Promise<void> {
    editorError = '';
    if (!editorSource.trim()) {
      editorError = '请填写 Lua 函数定义';
      return;
    }
    savingFunction = true;
    try {
      const saved = editorTargetName
        ? await onUpdate(editorTargetName, editorSource)
        : await onCreate(editorSource);
      selectedName = saved.name;
      creatingFunction = false;
      mode = 'run';
      editorTargetName = null;
      values = {};
      result = null;
    } catch (reason) {
      editorError = errorMessage(reason);
    } finally {
      savingFunction = false;
    }
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
    <div class="header-actions">
      <span class="registry-status"><i></i>{functions.length} 个函数已载入</span>
      <button class="button" type="button" on:click={startCreate}>
        <Plus size={16} />新建函数
      </button>
    </div>
  </header>

  <div class="functions-layout">
    <nav class="function-list" aria-label="函数列表">
      {#if creatingFunction}
        <button class="active" type="button" on:click={startCreate}>
          <span class="function-icon"><Code2 size={18} /></span>
          <span><strong>新函数草稿</strong><small>未保存</small></span>
        </button>
      {/if}
      {#each functions as item}
        <button
          class:active={!creatingFunction && selected?.name === item.name}
          type="button"
          on:click={() => choose(item)}
        >
          <span class="function-icon"><Braces size={18} /></span>
          <span><strong>{item.name}</strong><small>v{item.version}</small></span>
        </button>
      {:else}
        {#if !creatingFunction}
          <div class="function-empty">
            <Braces size={22} /><span>函数目录中还没有有效脚本</span>
          </div>
        {/if}
      {/each}
    </nav>

    <section class="runner">
      <details class="tutorial-panel" bind:open={tutorialOpen}>
        <summary>
          <span class="tutorial-icon"><BookOpenText size={18} /></span>
          <span class="tutorial-title">
            <strong>自定义函数教程</strong>
            <small>从模板到预览，照着 4 步写一个可保存的 Lua 函数</small>
          </span>
          <span class="tutorial-caret"><ChevronDown size={17} aria-hidden="true" /></span>
        </summary>
        <div class="tutorial-body">
          <ol class="tutorial-steps" aria-label="自定义函数步骤">
            <li>
              <span class="step-index">1</span>
              <span>
                <strong>写元数据</strong>
                <small>name 是函数唯一标识；version 和 description 会显示在列表与详情区。</small>
              </span>
            </li>
            <li>
              <span class="step-index">2</span>
              <span>
                <strong>定义 params</strong>
                <small>参数会自动生成表单，type 支持 text、number、date、boolean、option。</small>
              </span>
            </li>
            <li>
              <span class="step-index">3</span>
              <span>
                <strong>用 dec 处理数字</strong>
                <small>数字参数按字符串传入，金额或比例用 dec(params.amount) 做精确计算。</small>
              </span>
            </li>
            <li>
              <span class="step-index">4</span>
              <span>
                <strong>返回可预览结果</strong>
                <small>run 返回 table；amount、description 和自定义字段会一起预览。</small>
              </span>
            </li>
          </ol>
          <div class="tutorial-notes">
            <strong>保存规则</strong>
            <ul>
              <li>脚本必须 return 一个包含 run 的 table。</li>
              <li>保存会校验整个函数目录；任一脚本出错都会保留上一份有效快照。</li>
              <li>Lua 运行时关闭 os、io、package、debug、require、loadfile 等入口。</li>
            </ul>
            <button class="button" type="button" on:click={useTutorialExample}>
              <Code2 size={16} />用教程示例新建
            </button>
          </div>
        </div>
      </details>

      {#if mode === 'edit'}
        <header class="runner-header editor-header">
          <div>
            <span>{creatingFunction ? '新建 Lua 函数' : '编辑 Lua 函数'}</span>
            <h3>{creatingFunction ? '自定义函数' : editorTargetName}</h3>
          </div>
          <p>
            每个脚本需要返回包含 name、version、description、params 和 run 的 table。
            数字参数以字符串传入，精确计算使用 dec("0.1")。
          </p>
        </header>

        <form class="editor-form" on:submit|preventDefault={saveSource}>
          <label class="field source-field" for="function-source">
            <span class="field-label">Lua 源码</span>
            <textarea
              id="function-source"
              class="textarea source-editor"
              bind:value={editorSource}
              spellcheck="false"
              autocomplete="off"
            ></textarea>
          </label>
          {#if editorError}<p class="field-error" role="alert">{editorError}</p>{/if}
          <footer class="editor-actions">
            <button class="button" type="button" disabled={savingFunction} on:click={cancelEdit}>
              取消
            </button>
            <button class="button primary" type="submit" disabled={savingFunction}>
              {#if savingFunction}<LoaderCircle
                  class="spinner-icon"
                  size={18}
                />正在保存{:else}<Save size={17} />保存函数{/if}
            </button>
          </footer>
        </form>
      {:else if selected}
        <header class="runner-header">
          <div>
            <span>v{selected.version}</span>
            <h3>{selected.name}</h3>
          </div>
          <p>{selected.description}</p>
          <button class="button edit-button" type="button" on:click={startEdit}>
            <PencilLine size={16} />编辑脚本
          </button>
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
        <div class="runner-empty">
          <Braces size={28} />
          <strong>等待函数脚本</strong>
          <button class="button primary" type="button" on:click={startCreate}>
            <Plus size={17} />新建函数
          </button>
        </div>
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

  .header-actions {
    display: inline-flex;
    align-items: center;
    gap: 10px;
  }

  .header-actions .button,
  .edit-button,
  .editor-actions .button,
  .runner-empty .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    white-space: nowrap;
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
    display: grid;
    align-content: start;
    gap: 16px;
    min-width: 0;
    padding: 22px;
  }

  .tutorial-panel {
    overflow: hidden;
    border: 1px solid var(--line);
    border-radius: var(--radius-md);
    background: var(--surface-subtle);
  }

  .tutorial-panel summary {
    display: grid;
    grid-template-columns: 34px minmax(0, 1fr) 18px;
    align-items: center;
    gap: 9px;
    min-height: 48px;
    padding: 8px 12px;
    color: var(--ink);
    cursor: pointer;
    list-style: none;
  }

  .tutorial-panel summary::-webkit-details-marker {
    display: none;
  }

  .tutorial-title {
    display: grid;
    min-width: 0;
    gap: 1px;
  }

  .tutorial-panel summary strong {
    color: var(--ink-strong);
    font-size: 0.875rem;
  }

  .tutorial-panel summary small {
    overflow: hidden;
    color: var(--ink-muted);
    font-size: 0.75rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .tutorial-caret {
    display: grid;
    place-items: center;
    color: var(--ink-muted);
    transition: transform var(--duration-fast) var(--ease-out);
  }

  .tutorial-panel[open] .tutorial-caret {
    transform: rotate(180deg);
  }

  .tutorial-icon {
    display: grid;
    place-items: center;
    width: 30px;
    height: 30px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    color: var(--accent-strong);
    background: var(--surface-raised);
  }

  .tutorial-body {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(240px, 0.32fr);
    gap: 16px;
    padding: 0 12px 12px;
    border-top: 1px solid var(--line);
  }

  .tutorial-steps {
    display: grid;
    gap: 0;
    margin: 0;
    padding-top: 12px;
    padding-left: 0;
    list-style: none;
  }

  .tutorial-steps li {
    display: grid;
    grid-template-columns: 24px minmax(0, 1fr);
    gap: 9px;
    padding: 8px 0;
    border-bottom: 1px solid var(--line);
  }

  .tutorial-steps li:first-child {
    padding-top: 0;
  }

  .tutorial-steps li:last-child {
    border-bottom: 0;
    padding-bottom: 0;
  }

  .tutorial-steps li > span:last-child {
    display: grid;
    min-width: 0;
    gap: 2px;
  }

  .step-index {
    display: grid;
    place-items: center;
    width: 22px;
    height: 22px;
    border: 1px solid var(--line);
    border-radius: 50%;
    color: var(--accent-strong);
    background: var(--surface-raised);
    font-size: 0.75rem;
    font-weight: 750;
  }

  .tutorial-steps strong,
  .tutorial-notes > strong {
    color: var(--ink-strong);
    font-size: 0.8125rem;
  }

  .tutorial-steps small,
  .tutorial-notes li {
    color: var(--ink-muted);
    font-size: 0.75rem;
    line-height: 1.45;
  }

  .tutorial-notes {
    display: grid;
    gap: 4px;
    align-content: start;
    margin-top: 12px;
    min-width: 0;
    padding: 10px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    background: var(--surface-raised);
  }

  .tutorial-notes ul {
    display: grid;
    gap: 3px;
    margin: 0;
    padding-left: 16px;
  }

  .tutorial-notes .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    justify-self: start;
    min-height: var(--control-height-sm);
    margin-top: 3px;
    font-size: 0.8125rem;
  }

  .runner-header {
    display: grid;
    grid-template-columns: minmax(160px, 0.35fr) minmax(220px, 1fr) auto;
    align-items: end;
    gap: 18px;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--line);
  }

  .editor-header {
    grid-template-columns: minmax(160px, 0.35fr) minmax(220px, 1fr);
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

  .editor-form {
    display: grid;
    gap: 12px;
    padding-top: 16px;
  }

  .source-field {
    display: grid;
    gap: 6px;
  }

  .source-editor {
    min-height: min(52vh, 520px);
    padding: 12px;
    font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
    font-size: 0.8125rem;
    line-height: 1.55;
    tab-size: 2;
  }

  .editor-actions {
    display: flex;
    justify-content: flex-end;
    gap: 8px;
    padding-top: 10px;
    border-top: 1px solid var(--line);
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

    .tutorial-body,
    .tutorial-steps {
      grid-template-columns: 1fr;
    }

    .runner-header {
      grid-template-columns: 1fr;
      gap: 8px;
    }

    .header-actions {
      gap: 8px;
    }

    .registry-status {
      display: none;
    }
  }
</style>
