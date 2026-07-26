const DEFAULT_MONEY_SCALE = 2;
const MAX_MONEY_SCALE = 18;

/** Rendered when the backend sends no amount at all; never confuse this with "0". */
export const MONEY_PLACEHOLDER = '—';

// Fixed patterns. The scale is enforced in code rather than interpolated into a
// `{1,n}` quantifier, so no caller-supplied value can ever build an invalid regex.
const MONEY_PATTERN = /^(-?)(\d+)(?:\.(\d+))?$/;
const MONEY_INPUT_PATTERN = /^(-?)(0|[1-9]\d{0,35})(?:\.(\d+))?$/;
const GROUP_SEPARATOR_PATTERN = /\B(?=(\d{3})+(?!\d))/g;

/**
 * Clamp an asset scale coming from the API into the supported 0..18 range.
 * Anything that is not a usable number (null, undefined, NaN) falls back to the
 * documented default of 2.
 */
export function moneyScale(decimals: number | null | undefined = DEFAULT_MONEY_SCALE): number {
  if (typeof decimals !== 'number' || Number.isNaN(decimals)) return DEFAULT_MONEY_SCALE;
  return Math.min(Math.max(Math.trunc(decimals), 0), MAX_MONEY_SCALE);
}

/**
 * Group an exact decimal string without ever converting it to a JS number.
 * - missing / blank input renders the placeholder (never "0.00")
 * - input that is not an exact decimal is returned untouched so bad data stays visible
 * - a fraction longer than the scale is returned untouched rather than silently rounded
 */
export function formatMoney(
  value: string,
  decimals: number | null | undefined = DEFAULT_MONEY_SCALE
): string {
  const scale = moneyScale(decimals);
  if (typeof value !== 'string') return MONEY_PLACEHOLDER;
  const trimmed = value.trim();
  if (!trimmed) return MONEY_PLACEHOLDER;
  const match = MONEY_PATTERN.exec(trimmed);
  if (!match) return trimmed;
  const [, sign, integer, fraction = ''] = match;
  if (fraction.length > scale) return trimmed;
  const grouped = integer.replace(GROUP_SEPARATOR_PATTERN, ',');
  if (scale === 0) return `${sign}${grouped}`;
  return `${sign}${grouped}.${fraction.padEnd(scale, '0')}`;
}

export function today(): string {
  const now = new Date();
  const local = new Date(now.getTime() - now.getTimezoneOffset() * 60_000);
  return local.toISOString().slice(0, 10);
}

export function moneyScaleError(scale: number): string {
  return scale === 0 ? '金额需要是不带小数的整数' : `金额需要是最多${scale}位小数的数字`;
}

export function normalizeMoneyInput(
  value: string,
  decimals: number | null | undefined = DEFAULT_MONEY_SCALE
): string {
  const scale = moneyScale(decimals);
  const trimmed = typeof value === 'string' ? value.trim() : '';
  const match = trimmed ? MONEY_INPUT_PATTERN.exec(trimmed) : null;
  const fraction = match?.[3] ?? '';
  if (!match || fraction.length > scale) {
    throw new Error(moneyScaleError(scale));
  }
  const [, sign, integer] = match;
  const normalizedSign = /^0+$/.test(integer) && /^0*$/.test(fraction) ? '' : sign;
  if (scale === 0) return `${normalizedSign}${integer}`;
  return `${normalizedSign}${integer}.${fraction.padEnd(scale, '0')}`;
}

export function errorMessage(error: unknown): string {
  if (error instanceof Error) return error.message;
  return '请求未完成，请检查连接后重试';
}
