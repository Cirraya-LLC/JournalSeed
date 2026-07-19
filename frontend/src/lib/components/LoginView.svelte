<script lang="ts">
  import { ArrowRight, LoaderCircle } from 'lucide-svelte';
  import AuthFrame from './AuthFrame.svelte';
  import { errorMessage } from '$lib/format';

  export let onLogin: (input: { username: string; password: string }) => Promise<void>;

  let username = '';
  let password = '';
  let submitting = false;
  let error = '';

  async function submit(): Promise<void> {
    error = '';
    if (!username.trim() || !password) {
      error = '请填写管理员名称和密码';
      return;
    }
    submitting = true;
    try {
      await onLogin({ username: username.trim(), password });
    } catch (reason) {
      error = errorMessage(reason);
    } finally {
      submitting = false;
    }
  }
</script>

<AuthFrame eyebrow="管理员登录" title="回到你的账本" subtitle="流水、账户与计算规则都在原处。">
  <form on:submit|preventDefault={submit} class="login-form">
    <header>
      <span>JournalSeed</span>
      <h2>登录</h2>
    </header>

    <div class="field">
      <label for="login-username">管理员名称</label>
      <input id="login-username" class="input" bind:value={username} autocomplete="username" />
    </div>
    <div class="field">
      <label for="login-password">密码</label>
      <input
        id="login-password"
        class="input"
        type="password"
        bind:value={password}
        autocomplete="current-password"
      />
    </div>
    {#if error}<p class="field-error" role="alert">{error}</p>{/if}
    <button class="button primary submit-button" type="submit" disabled={submitting}>
      {#if submitting}
        <LoaderCircle class="spinner-icon" size={18} /> 正在验证
      {:else}
        进入账本 <ArrowRight size={18} />
      {/if}
    </button>
  </form>
</AuthFrame>

<style>
  .login-form {
    display: grid;
    gap: 18px;
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
