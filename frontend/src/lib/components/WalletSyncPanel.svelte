<script lang="ts">
  import {
    BookUser,
    CircleAlert,
    LoaderCircle,
    Plus,
    RefreshCw,
    Save,
    Settings,
    Tag,
    Trash2,
    WalletCards
  } from 'lucide-svelte';
  import Drawer from './Drawer.svelte';
  import { errorMessage, formatMoney } from '$lib/format';
  import type {
    AddressLabel,
    AddressLabelInput,
    AddressLabelKind,
    AddressLabelPatch,
    ChainCode,
    ChainSettings,
    ChainSettingsPatch,
    ChainTransaction,
    Wallet,
    WalletInput,
    WalletPatch
  } from '$lib/types';

  export let settings: ChainSettings | null;
  export let wallets: Wallet[];
  export let transactions: ChainTransaction[];
  export let labels: AddressLabel[];
  export let loading = false;
  export let onCreateWallet: (input: WalletInput) => Promise<void>;
  export let onUpdateWallet: (walletId: string, patch: WalletPatch) => Promise<void>;
  export let onDeleteWallet: (walletId: string) => Promise<void>;
  export let onSyncWallet: (walletId: string) => Promise<void>;
  export let onSaveSettings: (patch: ChainSettingsPatch) => Promise<void>;
  export let onCreateLabel: (input: AddressLabelInput) => Promise<void>;
  export let onUpdateLabel: (labelId: string, patch: AddressLabelPatch) => Promise<void>;
  export let onDeleteLabel: (labelId: string) => Promise<void>;

  const labelKinds: Array<{ value: AddressLabelKind; label: string }> = [
    { value: 'customer', label: '客户' },
    { value: 'self', label: '自己' },
    { value: 'exchange', label: '交易所' },
    { value: 'merchant', label: '商户' },
    { value: 'contract', label: '合约' },
    { value: 'other', label: '其他' }
  ];

  const chainOptions: Array<{
    value: ChainCode;
    label: string;
    addressLabel: string;
    placeholder: string;
    help: string;
  }> = [
    {
      value: 'tron-mainnet',
      label: 'TRON Mainnet',
      addressLabel: 'TRON 地址',
      placeholder: 'T...',
      help: '支持 TRON Mainnet 地址，适合同步 TRX / TRC20 流水。'
    },
    {
      value: 'ethereum-mainnet',
      label: 'Ethereum Mainnet',
      addressLabel: 'Ethereum 地址',
      placeholder: '0x...',
      help: '支持 Ethereum Mainnet EVM 地址，标签会与 Polygon 共享。'
    },
    {
      value: 'polygon-mainnet',
      label: 'Polygon Mainnet',
      addressLabel: 'Polygon 地址',
      placeholder: '0x...',
      help: '支持 Polygon Mainnet EVM 地址，标签会与 Ethereum 共享。'
    },
    {
      value: 'solana-mainnet',
      label: 'Solana Mainnet',
      addressLabel: 'Solana 地址',
      placeholder: '例如：7YH... 或主钱包公钥',
      help: '支持 Solana Mainnet 钱包公钥，只读同步链上流水。'
    }
  ];

  let walletName = '';
  let walletChain: ChainCode = 'tron-mainnet';
  let walletAddress = '';
  let walletEnabled = true;
  let walletAutoSync = true;
  let creatingWallet = false;
  let walletError = '';
  let syncingWalletId = '';
  let mutatingWalletId = '';

  let apiKey = '';
  let clearApiKey = false;
  let etherscanApiKey = '';
  let clearEtherscanApiKey = false;
  let ethereumRpcEndpoint = '';
  let clearEthereumRpcEndpoint = false;
  let polygonRpcEndpoint = '';
  let clearPolygonRpcEndpoint = false;
  let solanaRpcEndpoint = '';
  let clearSolanaRpcEndpoint = false;
  let intervalText = '30';
  let settingsSaving = false;
  let settingsError = '';

  let labelDrawerOpen = false;
  let activeChain: ChainCode = 'tron-mainnet';
  let activeAddress = '';
  let activeLabel: AddressLabel | null = null;
  let labelName = '';
  let labelKind: AddressLabelKind = 'customer';
  let labelNote = '';
  let labelSaving = false;
  let labelDeleting = false;
  let labelError = '';

  $: if (settings) intervalText = String(settings.syncIntervalMinutes);
  $: selectedChain = chainOptions.find((chain) => chain.value === walletChain) ?? chainOptions[0];

  function kindLabel(kind: AddressLabelKind | null | undefined): string {
    return labelKinds.find((item) => item.value === kind)?.label ?? '未标记';
  }

  function text(value: string | null | undefined): string {
    return typeof value === 'string' ? value.trim() : '';
  }

  function joinParts(...parts: Array<string | null | undefined>): string {
    return parts.map(text).filter(Boolean).join(' · ');
  }

  function chainLabel(
    chain: ChainCode | string | null | undefined,
    fallback?: string | null
  ): string {
    const code = text(chain);
    return (
      text(fallback) || chainOptions.find((item) => item.value === code)?.label || code || '未知链'
    );
  }

  function isEvmChain(chain: ChainCode | string | null | undefined): boolean {
    const code = text(chain);
    return code === 'ethereum-mainnet' || code === 'polygon-mainnet' || code === 'evm';
  }

  // A label is EVM-shared when the backend says so via scope, or when it only
  // carries the shared `evm` chain code (scope may arrive empty).
  function isEvmScopedLabel(label: AddressLabel): boolean {
    return text(label.scope) === 'evm' || text(label.chain) === 'evm' || isEvmChain(label.chain);
  }

  function labelScopeText(chain: ChainCode, label: AddressLabel | null = activeLabel): string {
    if ((label && isEvmScopedLabel(label)) || isEvmChain(chain)) return 'Ethereum/Polygon 共享标签';
    return `${chainLabel(chain, label?.chainName)} 专属标签`;
  }

  function directionLabel(
    direction: ChainTransaction['direction'] | string | null | undefined
  ): string {
    const value = text(direction);
    if (value === 'incoming') return '收入';
    if (value === 'outgoing') return '支出';
    if (value === 'fee') return '手续费';
    if (value === 'internal') return '内部转账';
    // Never fall through to a real category: an unknown direction must look unknown.
    return value ? `未知方向（${value}）` : '未知方向';
  }

  function addressText(transaction: ChainTransaction, side: 'origin' | 'target'): string {
    const address = side === 'origin' ? transaction.origin : transaction.target;
    if (!address) return '—';
    return text(address.displayName) || text(address.addressShort) || text(address.address) || '—';
  }

  function labelFor(address: string, chain: ChainCode | string): AddressLabel | null {
    const wanted = text(address);
    if (!wanted) return null;
    const code = text(chain);
    return (
      labels.find(
        (label) =>
          text(label.address) === wanted &&
          (text(label.chain) === code || (isEvmChain(code) && isEvmScopedLabel(label)))
      ) ?? null
    );
  }

  function openLabel(address: string, chain: ChainCode): void {
    if (!text(address)) return;
    activeChain = chain;
    activeAddress = text(address);
    activeLabel = labelFor(address, chain);
    labelName = activeLabel?.displayName ?? '';
    labelKind = activeLabel?.kind ?? 'customer';
    labelNote = activeLabel?.note ?? '';
    labelError = '';
    labelDrawerOpen = true;
  }

  async function createWallet(): Promise<void> {
    walletError = '';
    if (!walletName.trim()) {
      walletError = '请填写钱包名称';
      return;
    }
    if (!walletAddress.trim()) {
      walletError = `请填写${selectedChain.addressLabel}`;
      return;
    }
    creatingWallet = true;
    try {
      await onCreateWallet({
        chain: walletChain,
        name: walletName.trim(),
        address: walletAddress.trim(),
        enabled: walletEnabled,
        autoSync: walletAutoSync
      });
      walletName = '';
      walletAddress = '';
      walletChain = 'tron-mainnet';
      walletEnabled = true;
      walletAutoSync = true;
    } catch (reason) {
      walletError = errorMessage(reason);
    } finally {
      creatingWallet = false;
    }
  }

  async function toggleWallet(wallet: Wallet, patch: WalletPatch): Promise<void> {
    mutatingWalletId = wallet.id;
    try {
      await onUpdateWallet(wallet.id, patch);
    } catch (reason) {
      walletError = errorMessage(reason);
    } finally {
      mutatingWalletId = '';
    }
  }

  async function deleteWallet(wallet: Wallet): Promise<void> {
    if (!confirm(`删除钱包“${wallet.name}”？已同步流水不会被删除。`)) return;
    mutatingWalletId = wallet.id;
    try {
      await onDeleteWallet(wallet.id);
    } catch (reason) {
      walletError = errorMessage(reason);
    } finally {
      mutatingWalletId = '';
    }
  }

  async function syncWallet(wallet: Wallet): Promise<void> {
    syncingWalletId = wallet.id;
    walletError = '';
    try {
      await onSyncWallet(wallet.id);
    } catch (reason) {
      walletError = errorMessage(reason);
    } finally {
      syncingWalletId = '';
    }
  }

  async function saveSettings(): Promise<void> {
    settingsError = '';
    const syncIntervalMinutes = Number(intervalText);
    if (
      !Number.isInteger(syncIntervalMinutes) ||
      syncIntervalMinutes < 5 ||
      syncIntervalMinutes > 1440
    ) {
      settingsError = '同步间隔需要是 5 到 1440 之间的整数分钟';
      return;
    }
    settingsSaving = true;
    try {
      await onSaveSettings({
        syncIntervalMinutes,
        tronGridApiKey: apiKey.trim() || undefined,
        clearTronGridApiKey: clearApiKey || undefined,
        etherscanApiKey: etherscanApiKey.trim() || undefined,
        clearEtherscanApiKey: clearEtherscanApiKey || undefined,
        ethereumRpcUrl: ethereumRpcEndpoint.trim() || undefined,
        clearEthereumRpcUrl: clearEthereumRpcEndpoint || undefined,
        polygonRpcUrl: polygonRpcEndpoint.trim() || undefined,
        clearPolygonRpcUrl: clearPolygonRpcEndpoint || undefined,
        solanaRpcUrl: solanaRpcEndpoint.trim() || undefined,
        clearSolanaRpcUrl: clearSolanaRpcEndpoint || undefined
      });
      apiKey = '';
      clearApiKey = false;
      etherscanApiKey = '';
      clearEtherscanApiKey = false;
      ethereumRpcEndpoint = '';
      clearEthereumRpcEndpoint = false;
      polygonRpcEndpoint = '';
      clearPolygonRpcEndpoint = false;
      solanaRpcEndpoint = '';
      clearSolanaRpcEndpoint = false;
    } catch (reason) {
      settingsError = errorMessage(reason);
    } finally {
      settingsSaving = false;
    }
  }

  async function saveLabel(): Promise<void> {
    labelError = '';
    if (!labelName.trim()) {
      labelError = '请填写显示名';
      return;
    }
    labelSaving = true;
    try {
      if (activeLabel) {
        await onUpdateLabel(activeLabel.id, {
          displayName: labelName.trim(),
          kind: labelKind,
          note: labelNote.trim()
        });
      } else {
        if (!activeAddress) {
          labelError = '缺少地址，无法创建标记';
          return;
        }
        await onCreateLabel({
          chain: activeChain,
          address: activeAddress,
          displayName: labelName.trim(),
          kind: labelKind,
          note: labelNote.trim()
        });
      }
      labelDrawerOpen = false;
    } catch (reason) {
      labelError = errorMessage(reason);
    } finally {
      labelSaving = false;
    }
  }

  async function deleteLabel(): Promise<void> {
    if (!activeLabel) return;
    labelDeleting = true;
    try {
      await onDeleteLabel(activeLabel.id);
      labelDrawerOpen = false;
    } catch (reason) {
      labelError = errorMessage(reason);
    } finally {
      labelDeleting = false;
    }
  }
</script>

<div class="wallet-view">
  <header class="view-header">
    <div>
      <span>TRON / Ethereum / Polygon / Solana · Watch-only</span>
      <h2>钱包同步</h2>
    </div>
    <WalletCards size={22} />
  </header>

  <section class="wallet-section add-wallet">
    <header class="section-title">
      <div>
        <h3>添加钱包地址</h3>
        <p>{selectedChain.help} 不需要私钥、助记词或签名权限。</p>
      </div>
    </header>
    <form class="wallet-form" on:submit|preventDefault={createWallet}>
      <div class="field">
        <label for="wallet-chain">链</label>
        <select id="wallet-chain" class="select" bind:value={walletChain}>
          {#each chainOptions as chain}<option value={chain.value}>{chain.label}</option>{/each}
        </select>
      </div>
      <div class="field">
        <label for="wallet-name">钱包名称</label>
        <input
          id="wallet-name"
          class="input"
          bind:value={walletName}
          placeholder="例如：收款钱包"
        />
      </div>
      <div class="field address-field">
        <label for="wallet-address">{selectedChain.addressLabel}</label>
        <input
          id="wallet-address"
          class="input"
          bind:value={walletAddress}
          placeholder={selectedChain.placeholder}
        />
      </div>
      <label class="check-line">
        <input type="checkbox" bind:checked={walletEnabled} />启用
      </label>
      <label class="check-line">
        <input type="checkbox" bind:checked={walletAutoSync} />自动同步
      </label>
      <button class="button primary" type="submit" disabled={creatingWallet}>
        {#if creatingWallet}<LoaderCircle class="spinner-icon" size={17} />{:else}<Plus
            size={17}
          />{/if}
        添加钱包
      </button>
    </form>
    {#if walletError}<p class="field-error" role="alert">{walletError}</p>{/if}
  </section>

  <div class="wallet-grid">
    <section class="wallet-section">
      <header class="section-title">
        <div>
          <h3>钱包列表</h3>
          <p>{wallets.length} 个地址</p>
        </div>
      </header>
      <div class="wallet-list" aria-label="钱包列表">
        {#if loading}
          {#each Array(3) as _}<div class="wallet-card skeleton" aria-hidden="true">
              <i></i><i></i>
            </div>{/each}
        {:else}
          {#each wallets as wallet (wallet.id)}
            <article class="wallet-card" class:disabled={!wallet.enabled}>
              <div class="wallet-main">
                <strong>{wallet.name}</strong>
                <span
                  >{joinParts(
                    chainLabel(wallet.chain, wallet.chainName),
                    text(wallet.addressShort) || text(wallet.address)
                  )}</span
                >
              </div>
              <div class="wallet-state">
                <span class:running={wallet.syncStatus === 'running'}
                  >{text(wallet.syncStatus) || 'idle'}</span
                >
                {#if wallet.lastSyncedAt}<small
                    >{new Date(wallet.lastSyncedAt).toLocaleString('zh-CN')}</small
                  >{:else}<small>尚未同步</small>{/if}
              </div>
              {#if wallet.lastError}<p class="wallet-error">
                  <CircleAlert size={14} />{wallet.lastError}
                </p>{/if}
              <div class="wallet-switches">
                <label
                  ><input
                    type="checkbox"
                    checked={wallet.enabled}
                    disabled={mutatingWalletId === wallet.id}
                    on:change={(event) =>
                      toggleWallet(wallet, { enabled: event.currentTarget.checked })}
                  />启用</label
                >
                <label
                  ><input
                    type="checkbox"
                    checked={wallet.autoSync}
                    disabled={mutatingWalletId === wallet.id}
                    on:change={(event) =>
                      toggleWallet(wallet, { autoSync: event.currentTarget.checked })}
                  />自动</label
                >
              </div>
              <div class="wallet-actions">
                <button
                  class="button"
                  type="button"
                  disabled={syncingWalletId === wallet.id}
                  on:click={() => syncWallet(wallet)}
                >
                  <RefreshCw
                    class={syncingWalletId === wallet.id ? 'spinning' : ''}
                    size={16}
                  />立即同步
                </button>
                <button
                  class="icon-button"
                  type="button"
                  aria-label="删除钱包"
                  title="删除钱包"
                  on:click={() => deleteWallet(wallet)}><Trash2 size={16} /></button
                >
              </div>
            </article>
          {:else}
            <div class="empty-card">
              <WalletCards size={22} /><strong>还没有钱包</strong><span
                >添加任一支持链的钱包地址后即可同步链上流水。</span
              >
            </div>
          {/each}
        {/if}
      </div>
    </section>

    <section class="wallet-section settings-card">
      <header class="section-title">
        <div>
          <h3>同步设置</h3>
          <p>API Key 与 RPC URL 会按链独立使用</p>
        </div>
        <Settings size={18} />
      </header>
      <form class="settings-form" on:submit|preventDefault={saveSettings}>
        <div class="field">
          <label for="trongrid-key">TronGrid API Key</label>
          <input
            id="trongrid-key"
            class="input"
            bind:value={apiKey}
            placeholder={settings?.tronGridApiKeyConfigured
              ? '已配置；填写新 Key 可替换'
              : '可选；公共 endpoint 默认可用'}
          />
        </div>
        <label class="check-line clear-key"
          ><input type="checkbox" bind:checked={clearApiKey} />清除已保存 Key</label
        >
        <div class="settings-endpoint">
          <span>TronGrid Endpoint</span>
          <strong>{settings?.tronGridEndpoint ?? 'https://api.trongrid.io'}</strong>
        </div>
        <div class="field">
          <label for="etherscan-key">Etherscan API Key</label>
          <input
            id="etherscan-key"
            class="input"
            bind:value={etherscanApiKey}
            placeholder={settings?.etherscanApiKeyConfigured
              ? '已配置；填写新 Key 可替换'
              : 'Ethereum / Polygon 浏览器 API Key'}
          />
        </div>
        <label class="check-line clear-key"
          ><input type="checkbox" bind:checked={clearEtherscanApiKey} />清除 Etherscan Key</label
        >
        <div class="field">
          <label for="ethereum-rpc">Ethereum RPC URL</label>
          <input
            id="ethereum-rpc"
            class="input"
            bind:value={ethereumRpcEndpoint}
            placeholder={settings?.ethereumRpcUrlConfigured
              ? settings.ethereumRpcEndpoint || '已配置；填写新 URL 可替换'
              : 'https://...'}
          />
        </div>
        <label class="check-line clear-key"
          ><input type="checkbox" bind:checked={clearEthereumRpcEndpoint} />清除 Ethereum RPC URL</label
        >
        <div class="field">
          <label for="polygon-rpc">Polygon RPC URL</label>
          <input
            id="polygon-rpc"
            class="input"
            bind:value={polygonRpcEndpoint}
            placeholder={settings?.polygonRpcUrlConfigured
              ? settings.polygonRpcEndpoint || '已配置；填写新 URL 可替换'
              : 'https://...'}
          />
        </div>
        <label class="check-line clear-key"
          ><input type="checkbox" bind:checked={clearPolygonRpcEndpoint} />清除 Polygon RPC URL</label
        >
        <div class="field">
          <label for="solana-rpc">Solana RPC URL</label>
          <input
            id="solana-rpc"
            class="input"
            bind:value={solanaRpcEndpoint}
            placeholder={settings?.solanaRpcUrlConfigured
              ? settings.solanaRpcEndpoint || '已配置；填写新 URL 可替换'
              : 'https://api.mainnet-beta.solana.com'}
          />
        </div>
        <label class="check-line clear-key"
          ><input type="checkbox" bind:checked={clearSolanaRpcEndpoint} />清除 Solana RPC URL</label
        >
        <div class="field">
          <label for="sync-interval">自动同步间隔（分钟）</label>
          <input id="sync-interval" class="input" inputmode="numeric" bind:value={intervalText} />
        </div>
        {#if settingsError}<p class="field-error" role="alert">{settingsError}</p>{/if}
        <button class="button" type="submit" disabled={settingsSaving}>
          {#if settingsSaving}<LoaderCircle class="spinner-icon" size={17} />{:else}<Save
              size={17}
            />{/if}
          保存设置
        </button>
      </form>
    </section>
  </div>

  <section class="wallet-section transactions-card">
    <header class="section-title">
      <div>
        <h3>链上交易</h3>
        <p>origin / target 可直接标记为客户、自己、交易所等。</p>
      </div>
      <BookUser size={18} />
    </header>
    <div class="chain-table" role="region" aria-label="链上交易表格">
      <table>
        <thead>
          <tr>
            <th>时间</th>
            <th>链</th>
            <th>资产</th>
            <th>方向</th>
            <th class="amount-head">金额</th>
            <th>Origin</th>
            <th>Target</th>
            <th>Tx</th>
            <th>流水</th>
          </tr>
        </thead>
        <tbody>
          {#each transactions as transaction (transaction.id)}
            <tr class={transaction.direction}>
              <td
                >{transaction.blockTimestamp
                  ? new Date(transaction.blockTimestamp).toLocaleString('zh-CN')
                  : '—'}</td
              >
              <td>{chainLabel(transaction.chain, transaction.chainName)}</td>
              <td><strong>{text(transaction.assetSymbol) || '—'}</strong></td>
              <td><span class="direction-chip">{directionLabel(transaction.direction)}</span></td>
              <td class="amount-cell"
                >{formatMoney(transaction.amount, transaction.assetDecimals)}</td
              >
              <td>
                {#if text(transaction.origin?.address)}
                  <button
                    class="address-button"
                    type="button"
                    on:click={() => openLabel(transaction.origin.address, transaction.chain)}
                  >
                    <span>{addressText(transaction, 'origin')}</span>
                    {#if transaction.origin.kind}<small>{kindLabel(transaction.origin.kind)}</small
                      >{/if}
                  </button>
                {:else}<span class="empty-value">—</span>{/if}
              </td>
              <td>
                {#if text(transaction.target?.address)}
                  <button
                    class="address-button"
                    type="button"
                    on:click={() => openLabel(transaction.target.address, transaction.chain)}
                  >
                    <span>{addressText(transaction, 'target')}</span>
                    {#if transaction.target.kind}<small>{kindLabel(transaction.target.kind)}</small
                      >{/if}
                  </button>
                {:else}<span class="empty-value">—</span>{/if}
              </td>
              <td
                ><span class="hash" title={text(transaction.txHash)}
                  >{text(transaction.txHashShort) || text(transaction.txHash) || '—'}</span
                ></td
              >
              <td>{text(transaction.rowDescription) || '—'}</td>
            </tr>
          {:else}
            <tr><td colspan="9"><div class="table-empty">同步后链上交易会显示在这里。</div></td></tr
            >
          {/each}
        </tbody>
      </table>
    </div>
  </section>
</div>

{#if labelDrawerOpen}
  <Drawer
    title={activeLabel ? '修改地址标记' : '标记地址'}
    eyebrow={joinParts(chainLabel(activeChain, activeLabel?.chainName), activeAddress)}
    onClose={() => (labelDrawerOpen = false)}
  >
    <form class="label-form" on:submit|preventDefault={saveLabel}>
      <div class="label-scope">
        <span>{labelScopeText(activeChain)}</span>
      </div>
      <div class="field">
        <label for="label-name">显示名</label>
        <input id="label-name" class="input" bind:value={labelName} placeholder="例如：客户 A" />
      </div>
      <div class="field">
        <label for="label-kind">类型</label>
        <select id="label-kind" class="select" bind:value={labelKind}>
          {#each labelKinds as kind}<option value={kind.value}>{kind.label}</option>{/each}
        </select>
      </div>
      <div class="field">
        <label for="label-note">备注</label>
        <textarea
          id="label-note"
          class="textarea"
          bind:value={labelNote}
          placeholder="可记录客户、商户或交易所说明"
        ></textarea>
      </div>
      {#if labelError}<p class="field-error" role="alert">{labelError}</p>{/if}
      <footer class="label-actions">
        {#if activeLabel}
          <button
            class="button danger"
            type="button"
            disabled={labelDeleting}
            on:click={deleteLabel}><Trash2 size={17} />清除标记</button
          >
        {/if}
        <button class="button primary" type="submit" disabled={labelSaving}>
          {#if labelSaving}<LoaderCircle class="spinner-icon" size={17} />{:else}<Tag
              size={17}
            />{/if}
          保存标记
        </button>
      </footer>
    </form>
  </Drawer>
{/if}

<style>
  .wallet-view {
    display: grid;
    gap: 12px;
    padding: 12px;
    background: var(--surface-subtle);
  }

  .view-header,
  .wallet-section {
    border: 1px solid var(--line);
    border-radius: var(--radius-md);
    background: var(--surface-raised);
  }

  .view-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    min-height: 68px;
    padding: 12px 16px;
  }

  .view-header span,
  .section-title p {
    margin: 0;
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .view-header h2,
  .section-title h3 {
    margin: 0;
    color: var(--ink-strong);
  }

  .section-title {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 12px 14px;
    border-bottom: 1px solid var(--line);
  }

  .wallet-form {
    display: grid;
    grid-template-columns: 140px minmax(130px, 0.75fr) minmax(220px, 1.5fr) auto auto auto;
    align-items: end;
    gap: 9px;
    padding: 12px 14px;
  }

  .check-line,
  .wallet-switches label {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    min-height: var(--control-height);
    color: var(--ink);
    font-size: 0.8125rem;
    font-weight: 650;
  }

  .check-line input,
  .wallet-switches input {
    width: 16px;
    height: 16px;
    accent-color: var(--accent-strong);
  }

  .wallet-grid {
    display: grid;
    grid-template-columns: minmax(0, 1fr) minmax(280px, 360px);
    gap: 12px;
  }

  .wallet-list {
    display: grid;
    gap: 8px;
    padding: 10px;
  }

  .wallet-card {
    display: grid;
    grid-template-columns: minmax(0, 1fr) auto;
    gap: 8px 12px;
    padding: 10px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    background: var(--surface-raised);
  }

  .wallet-card.disabled {
    opacity: 0.7;
  }

  .wallet-main,
  .wallet-state,
  .section-title > div {
    display: grid;
    min-width: 0;
  }

  .wallet-main strong {
    color: var(--ink-strong);
  }

  .wallet-main span,
  .wallet-state small {
    overflow: hidden;
    color: var(--ink-muted);
    font-size: 0.75rem;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .wallet-state {
    justify-items: end;
  }

  .wallet-state > span {
    color: var(--ink-muted);
    font-size: 0.75rem;
    font-weight: 750;
  }

  .wallet-state > span.running {
    color: var(--accent-strong);
  }

  .wallet-error {
    display: inline-flex;
    grid-column: 1 / -1;
    align-items: center;
    gap: 5px;
    margin: 0;
    color: var(--expense);
    font-size: 0.75rem;
  }

  .wallet-switches,
  .wallet-actions {
    display: flex;
    align-items: center;
    gap: 8px;
  }

  .wallet-actions {
    justify-content: flex-end;
  }

  .wallet-actions .button,
  .settings-form .button,
  .wallet-form .button,
  .label-actions .button {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
  }

  .empty-card,
  .table-empty {
    display: grid;
    place-items: center;
    gap: 6px;
    min-height: 132px;
    color: var(--ink-muted);
    text-align: center;
  }

  .empty-card strong {
    color: var(--ink-strong);
  }

  .settings-form,
  .label-form {
    display: grid;
    gap: 12px;
    padding: 12px 14px;
  }

  .clear-key {
    min-height: 24px;
  }

  .settings-endpoint,
  .label-scope {
    display: grid;
    gap: 3px;
    padding: 9px;
    border: 1px solid var(--line);
    border-radius: var(--radius-sm);
    background: var(--surface-subtle);
    color: var(--ink-muted);
    font-size: 0.75rem;
  }

  .settings-endpoint strong,
  .label-scope span {
    overflow: hidden;
    color: var(--ink-strong);
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .chain-table {
    overflow: auto;
  }

  table {
    width: max(100%, 1080px);
    border-spacing: 0;
    table-layout: fixed;
    font-size: 0.8125rem;
    font-variant-numeric: tabular-nums;
  }

  th,
  td {
    height: 38px;
    padding: 7px 9px;
    border-bottom: 1px solid var(--line);
    color: var(--ink);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    text-align: left;
  }

  th {
    position: sticky;
    top: 0;
    z-index: 1;
    color: var(--ink-muted);
    background: var(--surface-subtle);
    font-size: 0.6875rem;
  }

  .amount-head,
  .amount-cell {
    text-align: right;
  }

  tr.incoming .amount-cell {
    color: var(--income);
  }

  tr.outgoing .amount-cell,
  tr.fee .amount-cell {
    color: var(--expense);
  }

  .direction-chip,
  .address-button small {
    display: inline-grid;
    place-items: center;
    min-height: 20px;
    padding: 2px 6px;
    border-radius: 999px;
    background: var(--surface-subtle);
    color: var(--ink-muted);
    font-size: 0.6875rem;
    font-weight: 750;
  }

  .address-button {
    display: inline-flex;
    align-items: center;
    max-width: 100%;
    gap: 5px;
    padding: 0;
    border: 0;
    color: var(--accent-strong);
    background: transparent;
    font-size: 0.8125rem;
  }

  .address-button span {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .hash {
    color: var(--ink-muted);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 0.75rem;
  }

  .empty-value {
    color: var(--ink-muted);
  }

  .label-form {
    padding: 16px;
  }

  .label-actions {
    display: flex;
    gap: 8px;
    margin-top: auto;
    padding-top: 10px;
    border-top: 1px solid var(--line);
  }

  .label-actions .button.primary {
    flex: 1;
  }

  .skeleton i {
    height: 16px;
    border-radius: 999px;
    background: var(--surface-subtle);
  }

  :global(.spinner-icon),
  :global(.spinning) {
    animation: spin 700ms linear infinite;
  }

  @media (max-width: 900px) {
    .wallet-grid,
    .wallet-form {
      grid-template-columns: 1fr;
    }

    .wallet-card {
      grid-template-columns: 1fr;
    }

    .wallet-state {
      justify-items: start;
    }
  }

  @media (max-width: 620px) {
    .wallet-view {
      padding: 0;
    }

    .view-header,
    .wallet-section {
      border-right: 0;
      border-left: 0;
      border-radius: 0;
    }
  }
</style>
