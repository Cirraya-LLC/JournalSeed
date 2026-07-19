import { afterEach, describe, expect, it, vi } from 'vitest';
import { api, ApiError } from './client';

afterEach(() => {
  api.setSession(null);
  vi.unstubAllGlobals();
});

describe('same-origin API client', () => {
  it('adds the rotated CSRF token to writes and preserves exact JSON strings', async () => {
    const fetchMock = vi.fn().mockResolvedValue(
      new Response(JSON.stringify({ id: 'ledger-1', name: '主账本', createdAt: '2026-07-19' }), {
        status: 201,
        headers: { 'Content-Type': 'application/json' }
      })
    );
    vi.stubGlobal('fetch', fetchMock);
    api.setSession({
      user: { id: 'user-1', username: 'admin' },
      csrfToken: 'csrf-token',
      expiresAt: '2026-08-19T00:00:00.000Z'
    });

    await api.createLedger('主账本');

    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/v1/ledgers');
    expect(init.credentials).toBe('same-origin');
    expect(new Headers(init.headers).get('X-JournalSeed-CSRF')).toBe('csrf-token');
    expect(init.body).toBe(JSON.stringify({ name: '主账本' }));
  });

  it('surfaces application/problem+json details', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn().mockResolvedValue(
        new Response(
          JSON.stringify({
            type: '/problems/validation_error',
            title: '内容需要调整',
            status: 422,
            code: 'validation_error',
            detail: '金额格式不正确'
          }),
          { status: 422, headers: { 'Content-Type': 'application/problem+json' } }
        )
      )
    );

    const expectedError = {
      status: 422,
      message: '金额格式不正确'
    } satisfies Partial<ApiError>;
    await expect(api.createLedger('')).rejects.toMatchObject(expectedError);
  });
});
