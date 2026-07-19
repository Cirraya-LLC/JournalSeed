<script lang="ts">
  import { ArrowRight, Eye, EyeOff, LoaderCircle } from 'lucide-svelte';
  import AuthFrame from './AuthFrame.svelte';
  import { errorMessage } from '$lib/format';

  export let onSetup: (input: {
    username: string;
    password: string;
    ledgerName: string;
  }) => Promise<void>;

  let username = '';
  let password = '';
  let ledgerName = '我的账本';
  let revealPassword = false;
  let submitting = false;
  let error = '';

  async function submit(): Promise<void> {
    error = '';
    if (!username.trim()) {
      error = '请填写管理员名称';
      return;
    }
    if (password.length < 10) {
      error = '密码至少需要 10 个字符';
      return;
    }
    if (!ledgerName.trim()) {
      error = '请填写首个账本名称';
      return;
    }
    submitting = true;
    try {
      await onSetup({ username: username.trim(), password, ledgerName: ledgerName.trim() });
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      submitting = false;
    }
  }
</script>

<AuthFrame
  eyebrow="首次设置"
  title="先种下一本清楚的账"
  subtitle="创建唯一管理员与第一本账本，之后即可开始录入。"
>
  <form on:submit|preventDefault={submit} class="setup-form">
    <header>
      <span>01 / 初始化</span>
      <h2>建立管理员</h2>
    </header>

    <div class="field">
      <label for="setup-username">管理员名称</label>
      <input id="setup-username" class="input" bind:value={username} autocomplete="username" />
    </div>

    <div class="field">
      <label for="setup-password">登录密码</label>
      <div class="password-field">
        <input
          id="setup-password"
          class="input"
          type={revealPassword ? 'text' : 'password'}
          bind:value={password}
          autocomplete="new-password"
          aria-describedby="password-hint"
        />
        <button
          class="reveal-button"
          type="button"
          aria-label={revealPassword ? '隐藏密码' : '显示密码'}
          title={revealPassword ? '隐藏密码' : '显示密码'}
          on:click={() => (revealPassword = !revealPassword)}
        >
          {#if revealPassword}<EyeOff size={18} />{:else}<Eye size={18} />{/if}
        </button>
      </div>
      <span id="password-hint" class="hint">至少 10 个字符</span>
    </div>

    <div class="field">
      <label for="setup-ledger">首个账本</label>
      <input id="setup-ledger" class="input" bind:value={ledgerName} />
    </div>

    {#if error}<p class="field-error" role="alert">{error}</p>{/if}

    <button class="button primary submit-button" type="submit" disabled={submitting}>
      {#if submitting}
        <LoaderCircle class="spinner-icon" size={18} /> 正在创建
      {:else}
        创建并进入 <ArrowRight size={18} />
      {/if}
    </button>
  </form>
</AuthFrame>

<style>
  .setup-form {
    display: grid;
    gap: 20px;
  }

  header {
    display: grid;
    gap: 4px;
    margin-bottom: 8px;
  }

  header span {
    color: var(--accent-strong);
    font-size: 0.75rem;
    font-weight: 750;
  }

  h2 {
    margin: 0;
    color: var(--ink-strong);
    font-size: 1.5rem;
    line-height: 1.3;
  }

  .password-field {
    position: relative;
  }

  .password-field .input {
    padding-right: 46px;
  }

  .reveal-button {
    position: absolute;
    top: 1px;
    right: 1px;
    display: grid;
    place-items: center;
    width: 42px;
    height: 40px;
    padding: 0;
    border: 0;
    color: var(--ink-muted);
    background: transparent;
  }

  .hint {
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .submit-button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    width: 100%;
    margin-top: 4px;
  }

  :global(.spinner-icon) {
    animation: spin 700ms linear infinite;
  }
</style>
