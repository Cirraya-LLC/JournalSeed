import type {
  Account,
  Category,
  Column,
  ColumnInput,
  Job,
  JournalRow,
  Ledger,
  LedgerSummary,
  LuaFunction,
  ProblemDetails,
  RowInput,
  RowPage,
  Session,
  SetupStatus
} from '$lib/types';

const API_ROOT = '/api/v1';

export class ApiError extends Error {
  constructor(
    readonly status: number,
    readonly problem: ProblemDetails
  ) {
    super(problem.detail || problem.title);
    this.name = 'ApiError';
  }
}

class JournalSeedApi {
  private csrfToken = '';

  setSession(session: Session | null): void {
    this.csrfToken = session?.csrfToken ?? '';
  }

  private async request<T>(path: string, init: RequestInit = {}): Promise<T> {
    const method = init.method?.toUpperCase() ?? 'GET';
    const headers = new Headers(init.headers);
    if (init.body && !headers.has('Content-Type')) {
      headers.set('Content-Type', 'application/json');
    }
    if (!['GET', 'HEAD', 'OPTIONS'].includes(method) && this.csrfToken) {
      headers.set('X-JournalSeed-CSRF', this.csrfToken);
    }

    const response = await fetch(`${API_ROOT}${path}`, {
      ...init,
      headers,
      credentials: 'same-origin'
    });

    if (!response.ok) {
      const fallback: ProblemDetails = {
        type: 'about:blank',
        title: '请求未完成',
        status: response.status,
        code: 'http_error',
        detail: `服务返回了 ${response.status}`
      };
      let problem = fallback;
      try {
        problem = (await response.json()) as ProblemDetails;
      } catch {
        // The fallback keeps network/proxy failures actionable for the UI.
      }
      throw new ApiError(response.status, problem);
    }

    if (response.status === 204) return undefined as T;
    return (await response.json()) as T;
  }

  setupStatus(): Promise<SetupStatus> {
    return this.request('/setup/status');
  }

  async setup(input: { username: string; password: string; ledgerName: string }): Promise<Session> {
    const session = await this.request<Session>('/setup', {
      method: 'POST',
      body: JSON.stringify(input)
    });
    this.setSession(session);
    return session;
  }

  async login(input: { username: string; password: string }): Promise<Session> {
    const session = await this.request<Session>('/auth/login', {
      method: 'POST',
      body: JSON.stringify(input)
    });
    this.setSession(session);
    return session;
  }

  async session(): Promise<Session> {
    const session = await this.request<Session>('/auth/session');
    this.setSession(session);
    return session;
  }

  async logout(): Promise<void> {
    await this.request('/auth/logout', { method: 'POST' });
    this.setSession(null);
  }

  ledgers(): Promise<Ledger[]> {
    return this.request('/ledgers');
  }

  createLedger(name: string): Promise<Ledger> {
    return this.request('/ledgers', { method: 'POST', body: JSON.stringify({ name }) });
  }

  summary(ledgerId: string): Promise<LedgerSummary> {
    return this.request(`/ledgers/${ledgerId}/summary`);
  }

  accounts(ledgerId: string): Promise<Account[]> {
    return this.request(`/ledgers/${ledgerId}/accounts`);
  }

  createAccount(ledgerId: string, name: string, openingBalance: string): Promise<Account> {
    return this.request(`/ledgers/${ledgerId}/accounts`, {
      method: 'POST',
      body: JSON.stringify({ name, openingBalance })
    });
  }

  categories(ledgerId: string): Promise<Category[]> {
    return this.request(`/ledgers/${ledgerId}/categories`);
  }

  createCategory(
    ledgerId: string,
    name: string,
    direction: 'income' | 'expense'
  ): Promise<Category> {
    return this.request(`/ledgers/${ledgerId}/categories`, {
      method: 'POST',
      body: JSON.stringify({ name, direction })
    });
  }

  columns(ledgerId: string, recycled = false): Promise<Column[]> {
    return this.request(`/ledgers/${ledgerId}/columns?recycled=${recycled}`);
  }

  createColumn(ledgerId: string, input: ColumnInput): Promise<Column> {
    return this.request(`/ledgers/${ledgerId}/columns`, {
      method: 'POST',
      body: JSON.stringify(input)
    });
  }

  updateColumn(
    columnId: string,
    input: Partial<Pick<Column, 'name' | 'position' | 'width'>>
  ): Promise<Column> {
    return this.request(`/columns/${columnId}`, {
      method: 'PATCH',
      body: JSON.stringify(input)
    });
  }

  recycleColumn(columnId: string): Promise<void> {
    return this.request(`/columns/${columnId}`, { method: 'DELETE' });
  }

  restoreColumn(columnId: string): Promise<void> {
    return this.request(`/columns/${columnId}/restore`, { method: 'POST' });
  }

  rows(
    ledgerId: string,
    options: { cursor?: string; limit?: number; sort?: string; recycled?: boolean } = {}
  ): Promise<RowPage> {
    const query = new URLSearchParams();
    query.set('limit', String(options.limit ?? 100));
    query.set('sort', options.sort ?? 'date:desc');
    if (options.cursor) query.set('cursor', options.cursor);
    if (options.recycled) query.set('recycled', 'true');
    return this.request(`/ledgers/${ledgerId}/rows?${query}`);
  }

  createRow(ledgerId: string, input: RowInput): Promise<JournalRow> {
    return this.request(`/ledgers/${ledgerId}/rows`, {
      method: 'POST',
      body: JSON.stringify(input)
    });
  }

  updateRow(rowId: string, input: RowInput): Promise<JournalRow> {
    return this.request(`/rows/${rowId}`, { method: 'PATCH', body: JSON.stringify(input) });
  }

  recycleRow(rowId: string): Promise<void> {
    return this.request(`/rows/${rowId}`, { method: 'DELETE' });
  }

  restoreRow(rowId: string): Promise<void> {
    return this.request(`/rows/${rowId}/restore`, { method: 'POST' });
  }

  functions(): Promise<LuaFunction[]> {
    return this.request('/functions');
  }

  invokeFunction(name: string, input: Record<string, unknown>): Promise<Record<string, unknown>> {
    return this.request(`/functions/${encodeURIComponent(name)}/invoke`, {
      method: 'POST',
      body: JSON.stringify(input)
    });
  }

  jobs(): Promise<Job[]> {
    return this.request('/jobs');
  }
}

export const api = new JournalSeedApi();
