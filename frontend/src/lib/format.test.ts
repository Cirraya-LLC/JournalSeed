import { describe, expect, it } from 'vitest';
import { MONEY_PLACEHOLDER, formatMoney, moneyScale, normalizeMoneyInput } from './format';

describe('exact money formatting', () => {
  it('groups values without converting them to JavaScript numbers', () => {
    expect(formatMoney('999999999999999999999999999999999999.99')).toBe(
      '999,999,999,999,999,999,999,999,999,999,999,999.99'
    );
    expect(formatMoney('-12345678901234567890.5')).toBe('-12,345,678,901,234,567,890.50');
    expect(formatMoney('1.234567', 6)).toBe('1.234567');
  });

  it('normalizes scale and negative zero', () => {
    expect(normalizeMoneyInput(' 42.1 ')).toBe('42.10');
    expect(normalizeMoneyInput('-0.00')).toBe('0.00');
    expect(normalizeMoneyInput('-12')).toBe('-12.00');
    expect(normalizeMoneyInput('0.123456', 6)).toBe('0.123456');
  });

  it('rejects exponent syntax and excess scale', () => {
    expect(() => normalizeMoneyInput('1e3')).toThrow(/最多2位小数/);
    expect(() => normalizeMoneyInput('0.001')).toThrow(/最多2位小数/);
  });
});

describe('moneyScale clamping', () => {
  it('keeps in-range integer scales', () => {
    expect(moneyScale(0)).toBe(0);
    expect(moneyScale(1)).toBe(1);
    expect(moneyScale(2)).toBe(2);
    expect(moneyScale(6)).toBe(6);
    expect(moneyScale(18)).toBe(18);
  });

  it('clamps out-of-range scales instead of producing an invalid quantifier', () => {
    expect(moneyScale(-1)).toBe(0);
    expect(moneyScale(-99)).toBe(0);
    expect(moneyScale(19)).toBe(18);
    expect(moneyScale(1000)).toBe(18);
    expect(moneyScale(Number.POSITIVE_INFINITY)).toBe(18);
    expect(moneyScale(Number.NEGATIVE_INFINITY)).toBe(0);
  });

  it('truncates non-integer scales and defaults on unusable input', () => {
    expect(moneyScale(2.9)).toBe(2);
    expect(moneyScale(-0.5)).toBe(0);
    expect(moneyScale(Number.NaN)).toBe(2);
    expect(moneyScale(undefined)).toBe(2);
    expect(moneyScale(null)).toBe(2);
  });
});

describe('formatMoney across the documented 0..18 scale range', () => {
  it('formats zero-decimal assets as plain integers', () => {
    // Regression: `\d{1,0}` used to throw SyntaxError at RegExp construction time.
    expect(() => formatMoney('1234', 0)).not.toThrow();
    expect(formatMoney('1234', 0)).toBe('1,234');
    expect(formatMoney('0', 0)).toBe('0');
    expect(formatMoney('-9876543', 0)).toBe('-9,876,543');
    expect(formatMoney('7', 0)).toBe('7');
  });

  it('does not silently round a fraction that the scale cannot hold', () => {
    // Backend sending 1.5 for a 0-decimal asset is a data bug: surface it, do not print "2".
    expect(formatMoney('1.5', 0)).toBe('1.5');
    expect(formatMoney('1.234', 2)).toBe('1.234');
  });

  it('pads and preserves single-decimal assets', () => {
    expect(formatMoney('12.3', 1)).toBe('12.3');
    expect(formatMoney('12', 1)).toBe('12.0');
    expect(formatMoney('-1234.5', 1)).toBe('-1,234.5');
  });

  it('handles the 18-decimal maximum used by EVM chains', () => {
    expect(formatMoney('1', 18)).toBe('1.000000000000000000');
    expect(formatMoney('0.000000000000000001', 18)).toBe('0.000000000000000001');
    expect(formatMoney('-1234567.123456789012345678', 18)).toBe('-1,234,567.123456789012345678');
  });

  it('clamps out-of-range decimals rather than throwing', () => {
    expect(formatMoney('1.5', -3)).toBe('1.5');
    expect(formatMoney('1', -3)).toBe('1');
    expect(formatMoney('1', 99)).toBe('1.000000000000000000');
    expect(formatMoney('1.25', 2.9)).toBe('1.25');
    expect(formatMoney('1.25', Number.NaN)).toBe('1.25');
    expect(formatMoney('1.25', undefined)).toBe('1.25');
    expect(formatMoney('1.25', null)).toBe('1.25');
  });

  it('renders a placeholder for a missing amount instead of a fake zero', () => {
    expect(formatMoney('', 0)).toBe(MONEY_PLACEHOLDER);
    expect(formatMoney('', 2)).toBe(MONEY_PLACEHOLDER);
    expect(formatMoney('   ', 18)).toBe(MONEY_PLACEHOLDER);
    expect(formatMoney(undefined as unknown as string, 2)).toBe(MONEY_PLACEHOLDER);
    expect(formatMoney(null as unknown as string, 2)).toBe(MONEY_PLACEHOLDER);
    expect(formatMoney('', 2)).not.toBe('0.00');
  });

  it('passes non-numeric strings through so genuine data bugs stay visible', () => {
    expect(formatMoney('abc', 2)).toBe('abc');
    expect(formatMoney('1e3', 2)).toBe('1e3');
    expect(formatMoney('1,234.00', 2)).toBe('1,234.00');
    expect(formatMoney(' 42 ', 2)).toBe('42.00');
  });

  it('formats values near the NUMERIC(38,18) limit exactly', () => {
    expect(formatMoney('99999999999999999999.999999999999999999', 18)).toBe(
      '99,999,999,999,999,999,999.999999999999999999'
    );
    expect(formatMoney('-99999999999999999999.999999999999999999', 18)).toBe(
      '-99,999,999,999,999,999,999.999999999999999999'
    );
    expect(formatMoney('99999999999999999999999999999999999999', 0)).toBe(
      '99,999,999,999,999,999,999,999,999,999,999,999,999'
    );
  });
});

describe('normalizeMoneyInput across the documented 0..18 scale range', () => {
  it('accepts and emits integers for zero-decimal assets', () => {
    expect(() => normalizeMoneyInput('42', 0)).not.toThrow();
    expect(normalizeMoneyInput('42', 0)).toBe('42');
    expect(normalizeMoneyInput(' -7 ', 0)).toBe('-7');
    expect(normalizeMoneyInput('-0', 0)).toBe('0');
    expect(() => normalizeMoneyInput('1.5', 0)).toThrow(/整数/);
  });

  it('supports 1 and 18 decimal scales', () => {
    expect(normalizeMoneyInput('3', 1)).toBe('3.0');
    expect(normalizeMoneyInput('3.7', 1)).toBe('3.7');
    expect(() => normalizeMoneyInput('3.75', 1)).toThrow(/最多1位小数/);
    expect(normalizeMoneyInput('0.000000000000000001', 18)).toBe('0.000000000000000001');
    expect(normalizeMoneyInput('-2', 18)).toBe('-2.000000000000000000');
    expect(() => normalizeMoneyInput('0.0000000000000000001', 18)).toThrow(/最多18位小数/);
  });

  it('clamps out-of-range decimals', () => {
    expect(normalizeMoneyInput('5', -4)).toBe('5');
    expect(normalizeMoneyInput('5', 99)).toBe('5.000000000000000000');
    expect(normalizeMoneyInput('5.25', 2.9)).toBe('5.25');
    expect(normalizeMoneyInput('5.25', Number.NaN)).toBe('5.25');
    expect(normalizeMoneyInput('5.25', null)).toBe('5.25');
  });

  it('rejects blank and malformed input at every scale', () => {
    expect(() => normalizeMoneyInput('', 2)).toThrow(/最多2位小数/);
    expect(() => normalizeMoneyInput('   ', 2)).toThrow(/最多2位小数/);
    expect(() => normalizeMoneyInput('', 0)).toThrow(/整数/);
    expect(() => normalizeMoneyInput('abc', 18)).toThrow(/最多18位小数/);
    expect(() => normalizeMoneyInput(undefined as unknown as string, 2)).toThrow();
  });

  it('keeps negative amounts and large values exact', () => {
    expect(normalizeMoneyInput('-0.10', 2)).toBe('-0.10');
    expect(normalizeMoneyInput('-1234567890', 2)).toBe('-1234567890.00');
    expect(normalizeMoneyInput('99999999999999999999.999999999999999999', 18)).toBe(
      '99999999999999999999.999999999999999999'
    );
  });
});
