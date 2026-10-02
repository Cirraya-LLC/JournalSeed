<script lang="ts">
  import { Plus, X } from 'lucide-svelte';
  import type { Account, ChainCode, TokenPreset, WalletTokenRule } from '$lib/types';

  export let chain: ChainCode;
  export let presets: TokenPreset[];
  export let accounts: Account[];
  /** Rules to start from; null means the chain's USDT/USDC presets, unconverted. */
  export let initial: WalletTokenRule[] | null = null;
  /** Output: the checked currencies, in the shape the API takes. */
  export let rules: WalletTokenRule[] = [];
  /** Output: a message when the current input cannot be saved, '' otherwise. */
  export let error = '';
  export let idPrefix = 'tokens';

  interface Entry {
    contract: string;
    symbol: string;
    detail: string;
    checked: boolean;
    rate: string;
    accountId: string;
    custom: boolean;
  }

  const NATIVE = 'native';
  const nativeSymbols: Record<ChainCode, string> = {
    'tron-mainnet': 'TRX',
    'ethereum-mainnet': 'ETH',
    'polygon-mainnet': 'POL',
    'solana-mainnet': 'SOL'
  };

  let entries: Entry[] = [];
  let seededFor = '';
  let customContract = '';
  let customSymbol = '';
  let customError = '';
  let bulkRate = '';
  let bulkAccountId = '';

  // Converted amounts land in the default asset, so only live default-asset accounts fit.
  $: bookable = accounts.filter((account) => !account.archived && account.assetSymbol === 'DEFAULT');

  function comparable(contract: string): string {
    const value = contract.trim();
    return value.startsWith('0x') || value.startsWith('0X') ? value.toLowerCase() : value;
  }

  function shortContract(contract: string): string {
    return contract.length > 14 ? `${contract.slice(0, 6)}...${contract.slice(-4)}` : contract;
  }

  function seed(): void {
    const chainPresets = presets.filter((preset) => preset.chain === chain);
    const existing = new Map((initial ?? []).map((rule) => [comparable(rule.contract), rule]));
    const next: Entry[] = chainPresets.map((preset) => {
      const rule = existing.get(comparable(preset.contract));
      return {
        contract: preset.contract,
        symbol: preset.symbol,
        detail: `${preset.name} · ${shortContract(preset.contract)}`,
        checked: initial ? Boolean(rule) : preset.symbol === 'USDT' || preset.symbol === 'USDC',
        rate: rule?.exchangeRate ?? '',
        accountId: rule?.accountId ?? '',
        custom: false
      };
    });
    const native = existing.get(NATIVE);
    next.push({
      contract: NATIVE,
      symbol: nativeSymbols[chain],
      detail: '链原生币，含转账手续费',
      checked: Boolean(native),
      rate: native?.exchangeRate ?? '',
      accountId: native?.accountId ?? '',
      custom: false
    });
    for (const rule of initial ?? []) {
      const key = comparable(rule.contract);
      if (next.some((entry) => comparable(entry.contract) === key)) continue;
      next.push({
        contract: rule.contract,
        symbol: rule.symbol,
        detail: `自定义合约 · ${shortContract(rule.contract)}`,
        checked: true,
        rate: rule.exchangeRate ?? '',
        accountId: rule.accountId ?? '',
        custom: true
      });
    }
    entries = next;
  }

  // Re-seed when the chain changes (add form) or presets arrive after mount.
  $: seedKey = `${chain}|${presets.length}`;
  $: if (seedKey !== seededFor) {
    seededFor = seedKey;
    seed();
  }

  function validRate(value: string): boolean {
    return /^\d{1,12}(\.\d{1,10})?$/.test(value) && /[1-9]/.test(value);
  }

  $: checked = entries.filter((entry) => entry.checked);
  $: rules = checked.map((entry) => {
    const rate = entry.rate.trim();
    const rule: WalletTokenRule = { contract: entry.contract, symbol: entry.symbol };
    if (rate) {
      rule.exchangeRate = rate;
      if (entry.accountId) rule.accountId = entry.accountId;
    }
    return rule;
  });
  $: error =
    checked.length === 0
      ? '请至少选择一种接受的货币'
      : (checked
          .filter((entry) => entry.rate.trim() && !validRate(entry.rate.trim()))
          .map((entry) => `${entry.symbol} 的汇率需要是大于 0 的数字`)[0] ?? '');

  function applyBulk(): void {
    const rate = bulkRate.trim();
    if (rate && !validRate(rate)) return;
    entries = entries.map((entry) =>
      entry.checked ? { ...entry, rate, accountId: rate ? bulkAccountId : '' } : entry
    );
  }

  function addCustom(): void {
    customError = '';
    const contract = customContract.trim();
    if (!contract) {
      customError = '请填写代币合约地址';
      return;
    }
    if (entries.some((entry) => comparable(entry.contract) === comparable(contract))) {
      customError = '这个合约已经在列表里了';
      return;
    }
    entries = [
      ...entries,
      {
        contract,
        symbol: customSymbol.trim(),
        detail: `自定义合约 · ${shortContract(contract)}`,
        checked: true,
        rate: '',
        accountId: '',
        custom: true
      }
    ];
    customContract = '';
    customSymbol = '';
  }

  function removeCustom(contract: string): void {
    entries = entries.filter((entry) => entry.contract !== contract);
  }
</script>

<div class="token-rules">
  <div class="bulk">
    <span>统一设置已选币种</span>
    <input
      class="input"
      inputmode="decimal"
      bind:value={bulkRate}
      placeholder="汇率，如 6.7"
      aria-label="统一汇率"
    />
    <select class="select" bind:value={bulkAccountId} aria-label="统一记入账户">
      <option value="">未分配账户</option>
      {#each bookable as account (account.id)}<option value={account.id}>{account.name}</option
        >{/each}
    </select>
    <button class="button" type="button" on:click={applyBulk}>应用</button>
  </div>

  <div class="rule-head" aria-hidden="true">
    <span>接受的货币</span><span>汇率（留空不换算）</span><span>记入账户</span>
  </div>
  {#each entries as entry, index (entry.contract)}
    <div class="rule" class:off={!entry.checked}>
      <label class="token">
        <input type="checkbox" bind:checked={entry.checked} />
        <span>
          {#if entry.custom}
            <input
              class="input symbol-input"
              bind:value={entry.symbol}
              placeholder="名称"
              aria-label="自定义币种名称"
            />
          {:else}<strong>{entry.symbol}</strong>{/if}
          <small title={entry.contract}>{entry.detail}</small>
        </span>
      </label>
      <input
        id={`${idPrefix}-rate-${index}`}
        class="input"
        inputmode="decimal"
        bind:value={entry.rate}
        disabled={!entry.checked}
        placeholder="不换算"
        aria-label={`${entry.symbol || '代币'} 汇率`}
      />
      <div class="account">
        <select
          class="select"
          bind:value={entry.accountId}
          disabled={!entry.checked || !entry.rate.trim()}
          title={entry.rate.trim() ? '' : '填写汇率后，按本位币记入所选账户'}
          aria-label={`${entry.symbol || '代币'} 记入账户`}
        >
          {#if entry.rate.trim()}
            <option value="">未分配账户</option>
            {#each bookable as account (account.id)}<option value={account.id}
                >{account.name}</option
              >{/each}
          {:else}
            <option value="">钱包自身 {entry.symbol || '代币'} 账户</option>
          {/if}
        </select>
        {#if entry.custom}
          <button
            class="icon-button"
            type="button"
            aria-label="移除自定义币种"
            title="移除"
            on:click={() => removeCustom(entry.contract)}><X size={15} /></button
          >
        {/if}
      </div>
    </div>
  {/each}

  <div class="custom">
    <input
      class="input"
      bind:value={customContract}
      placeholder="其他代币合约地址"
      aria-label="自定义代币合约地址"
    />
    <input
      class="input"
      bind:value={customSymbol}
      placeholder="名称（可选）"
      aria-label="自定义代币名称"
    />
    <button class="button" type="button" on:click={addCustom}><Plus size={16} />添加币种</button>
  </div>
  {#if customError}<p class="field-error" role="alert">{customError}</p>{/if}
  <p class="hint">
    收款自动记为收入，转出和手续费记为支出，分类先记为“未分配”，管理员之后可以再改。填写汇率后，流水金额 =
    数量 × 汇率，按本位币记入所选账户；留空则按原币记在钱包自己的账户里。
  </p>
</div>

<style>
  .token-rules {
    display: grid;
    gap: 6px;
    min-width: 0;
    container-type: inline-size;
  }

  .bulk,
  .rule,
  .rule-head,
  .custom {
    display: grid;
    grid-template-columns: minmax(0, 1.4fr) minmax(0, 0.8fr) minmax(0, 1fr);
    align-items: center;
    gap: 8px;
  }

  .bulk {
    grid-template-columns: auto minmax(0, 0.8fr) minmax(0, 1fr) auto;
    padding: 8px;
    border: 1px dashed var(--line);
    border-radius: var(--radius-sm);
    background: var(--surface-subtle);
  }

  .bulk > span,
  .rule-head span {
    color: var(--ink-muted);
    font-size: 0.75rem;
    font-weight: 650;
  }

  .rule {
    padding: 6px 0;
    border-bottom: 1px solid var(--line);
  }

  .rule.off .token strong,
  .rule.off small {
    opacity: 0.55;
  }

  .token {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }

  .token input[type='checkbox'] {
    width: 16px;
    height: 16px;
    flex: none;
    accent-color: var(--accent-strong);
  }

  .token > span {
    display: grid;
    min-width: 0;
  }

  .token strong {
    color: var(--ink-strong);
    font-size: 0.875rem;
  }

  .token small {
    overflow: hidden;
    color: var(--ink-muted);
    font-size: 0.6875rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .symbol-input {
    max-width: 140px;
    min-height: 28px;
  }

  .account {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }

  .account .select {
    flex: 1;
    min-width: 0;
  }

  .custom {
    grid-template-columns: minmax(0, 1.4fr) minmax(0, 0.8fr) auto;
    padding-top: 4px;
  }

  .custom .button,
  .bulk .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 5px;
  }

  .hint {
    margin: 2px 0 0;
    color: var(--ink-muted);
    font-size: 0.75rem;
    line-height: 1.5;
  }

  /* Sized by the space the editor gets (page form or drawer), not by the viewport. */
  @container (max-width: 640px) {
    .bulk,
    .rule,
    .custom {
      grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
    }

    .bulk {
      grid-template-columns: minmax(0, 1fr) minmax(0, 1fr) auto;
    }

    .bulk > span,
    .token,
    .custom input:first-child {
      grid-column: 1 / -1;
    }

    .rule-head {
      display: none;
    }
  }
</style>
