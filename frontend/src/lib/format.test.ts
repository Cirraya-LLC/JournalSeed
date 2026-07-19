import { describe, expect, it } from 'vitest';
import { formatMoney, normalizeMoneyInput } from './format';

describe('exact money formatting', () => {
  it('groups values without converting them to JavaScript numbers', () => {
    expect(formatMoney('999999999999999999999999999999999999.99')).toBe(
      '999,999,999,999,999,999,999,999,999,999,999,999.99'
    );
    expect(formatMoney('-12345678901234567890.5')).toBe('-12,345,678,901,234,567,890.50');
  });

  it('normalizes scale and negative zero', () => {
    expect(normalizeMoneyInput(' 42.1 ')).toBe('42.10');
    expect(normalizeMoneyInput('-0.00')).toBe('0.00');
    expect(normalizeMoneyInput('-12')).toBe('-12.00');
  });

  it('rejects exponent syntax and excess scale', () => {
    expect(() => normalizeMoneyInput('1e3')).toThrow(/最多两位小数/);
    expect(() => normalizeMoneyInput('0.001')).toThrow(/最多两位小数/);
  });
});
