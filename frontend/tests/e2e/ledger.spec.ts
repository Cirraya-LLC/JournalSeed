import { expect, test, type APIResponse, type BrowserContext, type Page } from '@playwright/test';

const baseURL = process.env.JOURNALSEED_BASE_URL ?? 'http://127.0.0.1:8080';
const username = process.env.JOURNALSEED_TEST_USERNAME ?? 'admin';
const password = process.env.JOURNALSEED_TEST_PASSWORD ?? 'JournalSeed-Test-2026!';

type SessionResponse = {
  csrfToken: string;
};

type Entity = {
  id: string;
};

type JsonResponse = Pick<APIResponse, 'json' | 'ok' | 'text' | 'url'>;

async function responseJson<T>(response: JsonResponse): Promise<T> {
  expect(response.ok(), `${response.url()}: ${await response.text()}`).toBe(true);
  return (await response.json()) as T;
}

async function authenticate(page: Page): Promise<string> {
  const setupStatus = await page.context().request.get(`${baseURL}/api/v1/setup/status`);
  const { required } = await responseJson<{ required: boolean }>(setupStatus);

  await page.goto('/');
  const sessionResponse = page.waitForResponse((response) => {
    const path = new URL(response.url()).pathname;
    return (
      response.request().method() === 'POST' &&
      (path === '/api/v1/setup' || path === '/api/v1/auth/login')
    );
  });

  if (required) {
    await page.getByLabel('管理员名称').fill(username);
    await page.getByLabel('登录密码').fill(password);
    await page.getByLabel('首个账本').fill('Playwright 初始账本');
    await page.getByRole('button', { name: '创建并进入' }).click();
  } else {
    await page.getByLabel('管理员名称').fill(username);
    await page.getByLabel('密码').fill(password);
    await page.getByRole('button', { name: '进入账本' }).click();
  }

  const response = await sessionResponse;
  const session = await responseJson<SessionResponse>(response);
  await expect(page.getByRole('navigation', { name: '主要导航' })).toBeVisible();
  return session.csrfToken;
}

async function post<T>(
  context: BrowserContext,
  csrfToken: string,
  path: string,
  data: Record<string, unknown>
): Promise<T> {
  const response = await context.request.post(`${baseURL}${path}`, {
    headers: { 'X-JournalSeed-CSRF': csrfToken },
    data
  });
  return responseJson<T>(response);
}

async function seedLedger(
  context: BrowserContext,
  csrfToken: string
): Promise<Entity & { name: string }> {
  const runId = `${Date.now()}-${process.pid}`;
  const ledgerName = `Playwright 验收-${runId}`;
  const ledger = await post<Entity>(context, csrfToken, '/api/v1/ledgers', { name: ledgerName });
  const primary = await post<Entity>(context, csrfToken, `/api/v1/ledgers/${ledger.id}/accounts`, {
    name: '日常账户',
    openingBalance: '0.00'
  });
  const savings = await post<Entity>(context, csrfToken, `/api/v1/ledgers/${ledger.id}/accounts`, {
    name: '储蓄账户',
    openingBalance: '0.00'
  });
  const incomeCategory = await post<Entity>(
    context,
    csrfToken,
    `/api/v1/ledgers/${ledger.id}/categories`,
    { name: '工资', direction: 'income' }
  );
  const expenseCategory = await post<Entity>(
    context,
    csrfToken,
    `/api/v1/ledgers/${ledger.id}/categories`,
    { name: '餐饮', direction: 'expense' }
  );

  const rows: Array<Record<string, unknown>> = [
    {
      date: '2026-07-19',
      description: '七月工资',
      kind: 'entry',
      amount: '1250.50',
      accountId: primary.id,
      categoryId: incomeCategory.id,
      transferAccountId: null,
      cells: {}
    },
    {
      date: '2026-07-19',
      description: '午餐',
      kind: 'entry',
      amount: '-85.25',
      accountId: primary.id,
      categoryId: expenseCategory.id,
      transferAccountId: null,
      cells: {}
    },
    {
      date: '2026-07-19',
      description: '转入储蓄',
      kind: 'transfer',
      amount: '200.00',
      accountId: primary.id,
      categoryId: null,
      transferAccountId: savings.id,
      cells: {}
    },
    {
      date: '2026-07-19',
      description: '核对完成',
      kind: 'note',
      amount: '0.00',
      accountId: null,
      categoryId: null,
      transferAccountId: null,
      cells: {}
    }
  ];

  for (const row of rows) {
    await post<Entity>(context, csrfToken, `/api/v1/ledgers/${ledger.id}/rows`, row);
  }
  return { ...ledger, name: ledgerName };
}

test('登录、桌面流水语义、详情抽屉与手机布局形成闭环', async ({ page }) => {
  const consoleErrors: string[] = [];
  const pageErrors: string[] = [];
  page.on('console', (message) => {
    if (message.type() !== 'error') return;
    const location = message.location().url;
    const expectedSessionProbe = location.endsWith('/api/v1/auth/session');
    if (!expectedSessionProbe) consoleErrors.push(message.text());
  });
  page.on('pageerror', (error) => pageErrors.push(error.message));

  await page.setViewportSize({ width: 1440, height: 960 });
  const csrfToken = await authenticate(page);
  const ledger = await seedLedger(page.context(), csrfToken);
  await page.evaluate(
    (ledgerId) => localStorage.setItem('journalseed.ledger', ledgerId),
    ledger.id
  );

  const eventResponsePromise = page.waitForResponse(
    (response) => new URL(response.url()).pathname === '/api/v1/events'
  );
  await page.reload();
  const eventResponse = await eventResponsePromise;
  expect(eventResponse.status()).toBe(200);
  expect(eventResponse.headers()['content-type']).toContain('text/event-stream');

  await expect(page.getByRole('banner').getByText(ledger.name)).toBeVisible();
  await expect(page.getByText('账户总余额').locator('..')).toContainText('1,165.25');
  await expect(page.getByText('期间收入').locator('..')).toContainText('+1,250.50');
  await expect(page.getByText('期间支出').locator('..')).toContainText('-85.25');
  await expect(page.getByText('流水数量').locator('..')).toContainText('4');

  const table = page.getByRole('region', { name: '流水表格' });
  await expect(table.getByRole('row', { name: /七月工资/ })).toHaveClass(/income/);
  await expect(table.getByRole('row', { name: /午餐/ })).toHaveClass(/expense/);
  await expect(table.getByRole('row', { name: /转入储蓄/ })).toHaveClass(/transfer/);
  await expect(table.getByRole('row', { name: /核对完成/ })).toHaveClass(/note/);

  await table.getByRole('row', { name: /午餐/ }).click();
  const desktopDialog = page.getByRole('dialog');
  await expect(desktopDialog.getByRole('heading', { name: '编辑流水' })).toBeVisible();
  await expect(desktopDialog.getByLabel('金额')).toHaveValue('-85.25');
  await desktopDialog.getByRole('button', { name: '关闭' }).click();

  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.getByRole('navigation', { name: '手机导航' })).toBeVisible();
  const mobileList = page.getByLabel('流水列表');
  await expect(mobileList).toBeVisible();
  await expect(mobileList.getByRole('button', { name: /七月工资/ })).toContainText('1,250.50');
  await expect(mobileList.getByRole('button', { name: /午餐/ })).toContainText('-85.25');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(
    true
  );

  await mobileList.getByRole('button', { name: /午餐/ }).click();
  const mobileDialog = page.getByRole('dialog');
  await expect(mobileDialog).toBeVisible();
  await expect(mobileDialog.getByRole('button', { name: '保存流水' })).toBeVisible();

  const typeButtonHeight = await mobileDialog
    .getByRole('button', { name: '收支' })
    .evaluate((element) => element.getBoundingClientRect().height);
  const amountInputHeight = await mobileDialog
    .getByLabel('金额')
    .evaluate((element) => element.getBoundingClientRect().height);
  const [dateBox, amountBox] = await Promise.all([
    mobileDialog.getByLabel('日期').boundingBox(),
    mobileDialog.getByLabel('金额').boundingBox()
  ]);
  expect(typeButtonHeight).toBeLessThanOrEqual(48);
  expect(amountInputHeight).toBeLessThanOrEqual(48);
  expect(dateBox).not.toBeNull();
  expect(amountBox).not.toBeNull();
  expect(dateBox!.x + dateBox!.width).toBeLessThanOrEqual(amountBox!.x);

  expect(consoleErrors).toEqual([]);
  expect(pageErrors).toEqual([]);
});
