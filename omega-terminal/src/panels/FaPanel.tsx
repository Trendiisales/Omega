// FaPanel — Financial Analysis (3-tab table: income / balance / cash).
//
// Step 6 deliverable. Renders the merged FaEnvelope returned by
// /api/v1/omega/fa, which is built engine-side from three OpenBB calls
// (`/equity/fundamental/{income,balance,cash}`). Args: `FA <symbol>`.
// Polling cadence: 5 minutes (engine cache TTL 250 s).

import { useCallback, useState } from 'react';
import { getFa } from '@/api/omega';
import type {
  BalanceSheet,
  CashFlow,
  FaEnvelope,
  IncomeStatement,
} from '@/api/types';
import { usePanelData } from '@/hooks/usePanelData';
import {
  FetchStatusBar,
  MissingSymbolPrompt,
  PanelHeader,
  SkeletonRows,
  WarningsBanner,
  detectMock,
  formatLargeNum,
  formatNum,
} from './_shared/PanelChrome';

interface Props {
  args: string[];
  onNavigate?: (target: string) => void;
}

const FA_POLL_MS = 5 * 60 * 1000;

type Tab = 'income' | 'balance' | 'cash';

export function FaPanel({ args }: Props) {
  const symbol = (args[0] ?? '').toUpperCase();
  const [tab, setTab] = useState<Tab>('income');

  const fetcher = useCallback(
    (s: AbortSignal) => getFa({ symbol }, { signal: s }),
    [symbol],
  );
  const { state, refetch } = usePanelData<FaEnvelope>(
    fetcher,
    [symbol],
    { pollMs: symbol ? FA_POLL_MS : 0 },
  );

  if (!symbol) return <MissingSymbolPrompt code="FA" exampleArgs="AAPL" />;

  const env = state.status === 'ok' ? state.data : (state.data ?? null);
  const provider = env?.provider ?? 'unknown';
  const isMock = detectMock(env);
  const warnings = (env?.warnings ?? []) as Array<{ message?: string }>;
  const income = env?.income ?? [];
  const balance = env?.balance ?? [];
  const cash = env?.cash ?? [];

  const periods = currentRows(tab, income, balance, cash).length;

  return (
    <section
      className="flex h-full w-full flex-col overflow-y-auto p-6 gap-4"
      aria-label="Financial Analysis"
    >
      <PanelHeader
        code="FA"
        title="Financial Analysis"
        subtitle={
          <>
            Income / balance / cash via OpenBB &middot; symbol{' '}
            <span className="text-amber-300">{symbol}</span> &middot; poll 5m
          </>
        }
        provider={provider}
        isMock={isMock}
      />

      <div className="flex items-center gap-2">
        <Tab current={tab} value="income"  onClick={setTab} label={`INCOME (${income.length})`} />
        <Tab current={tab} value="balance" onClick={setTab} label={`BALANCE (${balance.length})`} />
        <Tab current={tab} value="cash"    onClick={setTab} label={`CASH (${cash.length})`} />
      </div>

      <WarningsBanner warnings={warnings} />

      <div className="flex-1 border border-amber-800/50 bg-black/40">
        <div className="flex items-baseline justify-between border-b border-amber-800/40 px-4 py-2">
          <h3 className="font-mono text-xs uppercase tracking-[0.3em] text-amber-500">
            {tab}
          </h3>
          <span className="font-mono text-[10px] uppercase tracking-widest text-amber-700">
            {periods} {periods === 1 ? 'period' : 'periods'}
          </span>
        </div>

        {state.status === 'loading' && periods === 0 ? (
          <table className="w-full border-collapse font-mono text-xs">
            <tbody><SkeletonRows cols={4} rows={6} /></tbody>
          </table>
        ) : tab === 'income' ? (
          <IncomeTable rows={income} />
        ) : tab === 'balance' ? (
          <BalanceTable rows={balance} />
        ) : (
          <CashTable rows={cash} />
        )}

        <FetchStatusBar state={state} onRetry={refetch} />
      </div>
    </section>
  );
}

function Tab({
  current,
  value,
  onClick,
  label,
}: {
  current: Tab;
  value: Tab;
  onClick: (t: Tab) => void;
  label: string;
}) {
  const active = current === value;
  return (
    <button
      type="button"
      onClick={() => onClick(value)}
      className={
        'rounded border px-3 py-1 font-mono text-[10px] uppercase tracking-widest ' +
        (active
          ? 'border-amber-300 bg-amber-900/40 text-amber-200'
          : 'border-amber-800 text-amber-400 hover:bg-amber-950/40')
      }
    >
      {label}
    </button>
  );
}

function currentRows(
  tab: Tab,
  income: IncomeStatement[],
  balance: BalanceSheet[],
  cash: CashFlow[],
): unknown[] {
  if (tab === 'income') return income;
  if (tab === 'balance') return balance;
  return cash;
}

/* ---------- Tables ---------- */

function PeriodHeader({
  label,
  rows,
}: {
  label: string;
  rows: Array<{ period_ending?: string; fiscal_period?: string }>;
}) {
  return (
    <thead>
      <tr className="border-b border-amber-900/60 text-left text-amber-500">
        <th className="px-3 py-2 uppercase">{label}</th>
        {rows.map((r, i) => (
          <th
            key={`${r.period_ending ?? 'p'}-${i}`}
            className="px-3 py-2 text-right uppercase"
          >
            {r.period_ending?.slice(0, 10) ?? r.fiscal_period ?? `P${i + 1}`}
          </th>
        ))}
      </tr>
    </thead>
  );
}

function MoneyRow({
  label,
  rows,
  pick,
  decimals = false,
}: {
  label: string;
  rows: Array<Record<string, unknown>>;
  pick: (r: Record<string, unknown>) => number | undefined;
  decimals?: boolean;
}) {
  return (
    <tr className="border-b border-amber-900/40">
      <td className="px-3 py-2 text-amber-500">{label}</td>
      {rows.map((r, i) => {
        const v = pick(r);
        return (
          <td key={i} className="px-3 py-2 text-right text-amber-400">
            {v == null
              ? '—'
              : decimals
                ? formatNum(v, 2)
                : formatLargeNum(v)}
          </td>
        );
      })}
    </tr>
  );
}

function IncomeTable({ rows }: { rows: IncomeStatement[] }) {
  if (rows.length === 0) {
    return (
      <div className="px-4 py-8 text-center font-mono text-xs text-amber-600">
        No income-statement rows returned.
      </div>
    );
  }
  const r = rows as Array<Record<string, unknown>>;
  return (
    <table className="w-full border-collapse font-mono text-xs">
      <PeriodHeader label="Income" rows={rows} />
      <tbody>
        <MoneyRow label="Revenue"          rows={r} pick={(x) => x.revenue as number | undefined} />
        <MoneyRow label="Cost of revenue"  rows={r} pick={(x) => x.cost_of_revenue as number | undefined} />
        <MoneyRow label="Gross profit"     rows={r} pick={(x) => x.gross_profit as number | undefined} />
        <MoneyRow label="Operating income" rows={r} pick={(x) => x.operating_income as number | undefined} />
        <MoneyRow label="EBITDA"           rows={r} pick={(x) => x.ebitda as number | undefined} />
        <MoneyRow label="Net income"       rows={r} pick={(x) => x.net_income as number | undefined} />
        <MoneyRow label="EPS basic"        rows={r} pick={(x) => x.eps_basic as number | undefined}   decimals />
        <MoneyRow label="EPS diluted"      rows={r} pick={(x) => x.eps_diluted as number | undefined} decimals />
      </tbody>
    </table>
  );
}

function BalanceTable({ rows }: { rows: BalanceSheet[] }) {
  if (rows.length === 0) {
    return (
      <div className="px-4 py-8 text-center font-mono text-xs text-amber-600">
        No balance-sheet rows returned.
      </div>
    );
  }
  const r = rows as Array<Record<string, unknown>>;
  return (
    <table className="w-full border-collapse font-mono text-xs">
      <PeriodHeader label="Balance" rows={rows} />
      <tbody>
        <MoneyRow label="Total assets"           rows={r} pick={(x) => x.total_assets as number | undefined} />
        <MoneyRow label="Current assets"         rows={r} pick={(x) => x.total_current_assets as number | undefined} />
        <MoneyRow label="Cash + ST inv"          rows={r} pick={(x) => x.cash_and_short_term_investments as number | undefined} />
        <MoneyRow label="Total liabilities"      rows={r} pick={(x) => x.total_liabilities as number | undefined} />
        <MoneyRow label="Current liabilities"    rows={r} pick={(x) => x.total_current_liabilities as number | undefined} />
        <MoneyRow label="Long-term debt"         rows={r} pick={(x) => x.long_term_debt as number | undefined} />
        <MoneyRow label="Short-term debt"        rows={r} pick={(x) => x.short_term_debt as number | undefined} />
        <MoneyRow label="Total equity"           rows={r} pick={(x) => x.total_equity as number | undefined} />
      </tbody>
    </table>
  );
}

function CashTable({ rows }: { rows: CashFlow[] }) {
  if (rows.length === 0) {
    return (
      <div className="px-4 py-8 text-center font-mono text-xs text-amber-600">
        No cash-flow rows returned.
      </div>
    );
  }
  const r = rows as Array<Record<string, unknown>>;
  return (
    <table className="w-full border-collapse font-mono text-xs">
      <PeriodHeader label="Cash" rows={rows} />
      <tbody>
        <MoneyRow label="Operating CF"   rows={r} pick={(x) => x.cash_from_operating_activities as number | undefined} />
        <MoneyRow label="Investing CF"   rows={r} pick={(x) => x.cash_from_investing_activities as number | undefined} />
        <MoneyRow label="Financing CF"   rows={r} pick={(x) => x.cash_from_financing_activities as number | undefined} />
        <MoneyRow label="Capex"          rows={r} pick={(x) => x.capital_expenditure as number | undefined} />
        <MoneyRow label="Free Cash Flow" rows={r} pick={(x) => x.free_cash_flow as number | undefined} />
      </tbody>
    </table>
  );
}
