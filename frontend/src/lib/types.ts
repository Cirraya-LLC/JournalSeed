/**
 * Exact decimal string; never a JavaScript number.
 *
 * The server renders every amount at the scale of its own asset — `DEFAULT` 2, TRX/USDT 6,
 * SOL 9, ETH/POL 18 — so the decimal places are always exactly the `assetDecimals` (or
 * `AssetSummary.decimals`) shipped alongside the value. On input, more decimal places than
 * the asset allows is rejected with a 422 carrying `fields.amount`, never rounded to fit.
 */
export type Money = string;
export type Direction = 'income' | 'expense';
export type RowKind = 'entry' | 'transfer' | 'note';
/** `money` is response-only: the server emits it for the system amount column, but
 *  `ColumnInput.type` cannot request it. */
export type ColumnType =
  'text' | 'number' | 'date' | 'boolean' | 'option' | 'relation' | 'formula' | 'money';

export interface SetupStatus {
  required: boolean;
}

export interface Session {
  user: { id: string; username: string };
  csrfToken: string;
  expiresAt: string;
}

export interface Ledger {
  id: string;
  name: string;
  createdAt: string;
}

/**
 * `balance` / `income` / `expense` describe the ledger's DEFAULT ASSET ONLY, at that asset's
 * scale — they are not ledger-wide totals, because amounts in different assets cannot be
 * added. Every asset, default first, is in `assetSummaries`. `rowCount` stays ledger-wide.
 *
 * `income`, `expense` and `rowCount` are flows bounded by both `from` and `to`; `balance` is
 * a stock and is the closing balance as of `to`, unaffected by `from`. `expense` is a signed
 * total and is therefore negative or zero.
 */
export interface LedgerSummary {
  balance: Money;
  income: Money;
  expense: Money;
  rowCount: number;
  assetSummaries: AssetSummary[];
}

/** One asset's balance and period flows, rendered at that asset's own `decimals`. Values
 *  from different assets must never be summed. */
export interface AssetSummary {
  assetId: string;
  symbol: string;
  name: string;
  decimals: number;
  balance: Money;
  income: Money;
  expense: Money;
}

export interface Account {
  id: string;
  name: string;
  openingBalance: Money;
  balance: Money;
  archived: boolean;
  assetId: string;
  assetSymbol: string;
  assetDecimals: number;
}

export interface Category {
  id: string;
  name: string;
  direction: Direction;
  archived: boolean;
}

export type CellValue = string | boolean | null;
export type ChainCode = 'tron-mainnet' | 'ethereum-mainnet' | 'polygon-mainnet' | 'solana-mainnet';
export type ChainMovementDirection = 'incoming' | 'outgoing' | 'internal' | 'fee';
export type AddressLabelKind = 'customer' | 'self' | 'exchange' | 'merchant' | 'contract' | 'other';
export type WalletSyncStatus = 'idle' | 'running' | 'failed';

export interface ChainSettings {
  tronGridApiKeyConfigured: boolean;
  etherscanApiKeyConfigured: boolean;
  syncIntervalMinutes: number;
  tronGridEndpoint: string;
  ethereumRpcUrlConfigured: boolean;
  ethereumRpcEndpoint: string;
  polygonRpcUrlConfigured: boolean;
  polygonRpcEndpoint: string;
  solanaRpcUrlConfigured: boolean;
  solanaRpcEndpoint: string;
}

export interface ChainSettingsPatch {
  tronGridApiKey?: string;
  clearTronGridApiKey?: boolean;
  etherscanApiKey?: string;
  clearEtherscanApiKey?: boolean;
  ethereumRpcUrl?: string;
  clearEthereumRpcUrl?: boolean;
  polygonRpcUrl?: string;
  clearPolygonRpcUrl?: boolean;
  solanaRpcUrl?: string;
  clearSolanaRpcUrl?: boolean;
  syncIntervalMinutes?: number;
}

/**
 * A currency a wallet accepts. `contract` is the normalized contract address, or `native`
 * for the chain's own coin (TRX / ETH / POL / SOL, which also pays the fees). With
 * `exchangeRate` set, each transfer is booked in the default asset as amount × rate on
 * `accountId` (the unallocated account when absent); otherwise it stays in the token's own
 * asset on the wallet's per-token account.
 */
export interface WalletTokenRule {
  contract: string;
  symbol: string;
  exchangeRate?: string;
  accountId?: string;
}

export interface TokenPreset {
  chain: ChainCode;
  symbol: string;
  name: string;
  contract: string;
}

export interface WalletInput {
  chain: ChainCode;
  name: string;
  address: string;
  enabled: boolean;
  autoSync: boolean;
  acceptedTokens?: WalletTokenRule[];
}

/** `enabled: false` pauses syncing; resuming catches up from where it stopped. */
export interface WalletPatch {
  name?: string;
  enabled?: boolean;
  autoSync?: boolean;
  acceptedTokens?: WalletTokenRule[];
}

export interface Wallet {
  id: string;
  ledgerId: string;
  chain: ChainCode;
  chainName: string;
  name: string;
  address: string;
  addressShort: string;
  enabled: boolean;
  autoSync: boolean;
  lastSyncedAt: string | null;
  lastError: string | null;
  syncStatus: WalletSyncStatus;
  createdAt: string | null;
  /** Wallets added before accepted currencies existed sync every asset. */
  acceptAllTokens: boolean;
  acceptedTokens: WalletTokenRule[];
  /** Transfers before this moment are not booked; null means full history. */
  syncFrom?: string | null;
}

export interface AddressLabelInput {
  chain: ChainCode;
  address: string;
  displayName: string;
  kind: AddressLabelKind;
  note: string;
}

export interface AddressLabelPatch {
  displayName?: string;
  kind?: AddressLabelKind;
  note?: string;
}

export interface AddressLabel {
  id: string;
  ledgerId: string;
  chain: ChainCode;
  chainName: string;
  scope: 'chain' | 'evm';
  address: string;
  addressShort: string;
  displayName: string;
  kind: AddressLabelKind;
  note: string;
  updatedAt: string;
}

export interface ChainAddress {
  address: string;
  addressShort: string;
  labelId: string | null;
  displayName: string | null;
  kind: AddressLabelKind | null;
}

export interface ChainSource {
  txHash: string;
  txHashShort: string;
  chain: ChainCode;
  chainName?: string;
  direction: ChainMovementDirection;
  assetSymbol: string;
  assetDecimals: number;
  origin: ChainAddress;
  target: ChainAddress;
}

export interface ChainTransaction {
  id: string;
  txHash: string;
  txHashShort: string;
  blockTimestamp: string | null;
  chain: ChainCode;
  chainName?: string;
  assetId: string;
  assetSymbol: string;
  assetDecimals: number;
  direction: ChainMovementDirection;
  amount: Money;
  origin: ChainAddress;
  target: ChainAddress;
  rowId: string | null;
  rowDescription: string | null;
}

export interface JournalRow {
  id: string;
  date: string;
  description: string;
  kind: RowKind;
  amount: Money;
  assetId: string;
  assetSymbol: string;
  assetDecimals: number;
  accountId: string | null;
  accountName?: string | null;
  categoryId: string | null;
  categoryName?: string | null;
  transferAccountId: string | null;
  transferAccountName?: string | null;
  cells: Record<string, CellValue>;
  revision: number;
  createdAt: string;
  updatedAt: string;
  /** Present on rows created by wallet sync, absent on ordinary manual rows — so its
   *  presence is a reliable test for "this row came from a chain movement". */
  chainSource?: ChainSource | null;
}

export interface RowInput {
  date: string;
  description: string;
  kind: RowKind;
  amount: Money;
  accountId: string | null;
  categoryId: string | null;
  transferAccountId: string | null;
  cells: Record<string, CellValue>;
}

export interface RowPage {
  items: JournalRow[];
  nextCursor: string | null;
  hasMore: boolean;
}

export interface Column {
  id: string;
  name: string;
  type: ColumnType;
  system: 'date' | 'description' | 'account' | 'category' | 'amount' | null;
  position: number;
  width: number;
  decimalPlaces?: number;
  formulaSource?: string;
  formulaResultType?: string;
  formulaDependencies?: string[];
  recycled: boolean;
}

export interface ColumnInput {
  name: string;
  type: Exclude<ColumnType, 'money'>;
  decimalPlaces?: number;
  formulaSource?: string;
  formulaResultType?: string;
  formulaDependencies?: string[];
}

export interface LuaParam {
  name: string;
  type: 'text' | 'number' | 'date' | 'boolean' | 'option';
  label: string;
  required: boolean;
  options?: string[];
}

export interface LuaFunction {
  name: string;
  version: string;
  description: string;
  script: string;
  source: string;
  params: LuaParam[];
}

export interface LuaFunctionInput {
  source: string;
}

export interface ProblemDetails {
  type: string;
  title: string;
  status: number;
  code: string;
  detail?: string;
  fields?: Record<string, string>;
}

export interface Job {
  id: string;
  kind: 'formula_recalc' | 'csv_import' | 'csv_export' | 'wallet_backfill' | 'wallet_sync';
  status: 'queued' | 'running' | 'completed' | 'failed' | 'cancelled';
  done: number;
  total: number;
  error: Record<string, unknown> | null;
  createdAt: string;
}

export interface SyncResult {
  job: Job;
  transactionsSeen: number;
  movementsCreated: number;
  rowsCreated: number;
}
