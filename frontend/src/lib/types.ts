export type Money = string;
export type Direction = 'income' | 'expense';
export type RowKind = 'entry' | 'transfer' | 'note';
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

export interface LedgerSummary {
  balance: Money;
  income: Money;
  expense: Money;
  rowCount: number;
}

export interface Account {
  id: string;
  name: string;
  openingBalance: Money;
  balance: Money;
  archived: boolean;
}

export interface Category {
  id: string;
  name: string;
  direction: Direction;
  archived: boolean;
}

export type CellValue = string | boolean | null;

export interface JournalRow {
  id: string;
  date: string;
  description: string;
  kind: RowKind;
  amount: Money;
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
  kind: 'formula_recalc' | 'csv_import' | 'csv_export';
  status: 'queued' | 'running' | 'completed' | 'failed' | 'cancelled';
  done: number;
  total: number;
  error: Record<string, unknown> | null;
  createdAt: string;
}
