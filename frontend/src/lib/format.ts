export function formatMoney(value: string): string {
  const match = /^(-?)(\d+)(?:\.(\d{1,2}))?$/.exec(value);
  if (!match) return value;
  const [, sign, integer, fraction = ''] = match;
  const grouped = integer.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  return `${sign}${grouped}.${fraction.padEnd(2, '0')}`;
}

export function today(): string {
  const now = new Date();
  const local = new Date(now.getTime() - now.getTimezoneOffset() * 60_000);
  return local.toISOString().slice(0, 10);
}

export function normalizeMoneyInput(value: string): string {
  const trimmed = value.trim();
  const match = /^(-?)(0|[1-9]\d{0,35})(?:\.(\d{1,2}))?$/.exec(trimmed);
  if (!match) {
    throw new Error('金额需要是最多两位小数的数字');
  }
  const [, sign, integer, fraction = ''] = match;
  const normalizedSign = /^0+$/.test(integer) && /^0*$/.test(fraction) ? '' : sign;
  return `${normalizedSign}${integer}.${fraction.padEnd(2, '0')}`;
}

export function errorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  return '请求未完成，请检查连接后重试';
}
