// CommandBar — Bloomberg-style command input.
//
// Behavior:
//   - Forced uppercase as user types.
//   - Autocomplete dropdown opens as soon as input is non-empty.
//   - Up/Down move highlight; Enter accepts highlighted suggestion (or, if
//     none highlighted, dispatches whatever the user typed via the router).
//   - Tab also accepts the highlighted suggestion without dispatching.
//   - Esc closes the dropdown.
//   - Ctrl+K (or Cmd+K) from anywhere on the page focuses the input. The
//     handler is wired here using a global keydown listener so the parent
//     doesn't need to forward refs.

import { useEffect, useMemo, useRef, useState } from 'react';
import { resolveCode, suggestCodes } from '@/router/functionCodes';
import type { FunctionCode, PanelDescriptor } from '@/types';

interface Props {
  /** Called when the user dispatches a code (Enter on resolved input). */
  onDispatch: (code: FunctionCode) => void;
}

export function CommandBar({ onDispatch }: Props) {
  const [query, setQuery] = useState<string>('');
  const [open, setOpen] = useState<boolean>(false);
  const [highlight, setHighlight] = useState<number>(0);
  const [error, setError] = useState<string | null>(null);
  const inputRef = useRef<HTMLInputElement | null>(null);

  const suggestions: PanelDescriptor[] = useMemo(
    () => (query.length === 0 ? [] : suggestCodes(query, 8)),
    [query]
  );

  // Global Ctrl/Cmd+K -> focus.
  useEffect(() => {
    function onKey(e: KeyboardEvent) {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        inputRef.current?.focus();
        inputRef.current?.select();
      }
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  function dispatchRaw(raw: string) {
    const result = resolveCode(raw);
    if (!result.matched) {
      setError(`Unknown code: "${raw.trim()}". Showing HELP.`);
    } else {
      setError(null);
    }
    onDispatch(result.code);
    setOpen(false);
    setQuery('');
    setHighlight(0);
  }

  function handleInput(e: React.ChangeEvent<HTMLInputElement>) {
    const next = e.target.value.toUpperCase();
    setQuery(next);
    setOpen(next.length > 0);
    setHighlight(0);
    if (error) setError(null);
  }

  function handleKeyDown(e: React.KeyboardEvent<HTMLInputElement>) {
    if (e.key === 'ArrowDown') {
      if (suggestions.length === 0) return;
      e.preventDefault();
      setOpen(true);
      setHighlight((h) => (h + 1) % suggestions.length);
      return;
    }
    if (e.key === 'ArrowUp') {
      if (suggestions.length === 0) return;
      e.preventDefault();
      setOpen(true);
      setHighlight((h) => (h - 1 + suggestions.length) % suggestions.length);
      return;
    }
    if (e.key === 'Tab') {
      if (open && suggestions[highlight]) {
        e.preventDefault();
        setQuery(suggestions[highlight].code);
        return;
      }
    }
    if (e.key === 'Enter') {
      e.preventDefault();
      if (open && suggestions[highlight]) {
        dispatchRaw(suggestions[highlight].code);
        return;
      }
      if (query.trim().length > 0) {
        dispatchRaw(query);
      }
      return;
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      setOpen(false);
      return;
    }
  }

  function handleSuggestionClick(desc: PanelDescriptor) {
    dispatchRaw(desc.code);
  }

  return (
    <div className="relative w-full">
      <div className="flex items-center gap-2 border-b border-amber-700/60 bg-black px-3 py-2">
        <span
          aria-hidden="true"
          className="select-none font-mono text-sm font-bold text-amber-500"
        >
          &gt;
        </span>
        <input
          ref={inputRef}
          type="text"
          spellCheck={false}
          autoComplete="off"
          autoCorrect="off"
          autoCapitalize="characters"
          value={query}
          onChange={handleInput}
          onKeyDown={handleKeyDown}
          onFocus={() => setOpen(query.length > 0)}
          onBlur={() => {
            // small delay so suggestion clicks register first
            setTimeout(() => setOpen(false), 100);
          }}
          placeholder="Type a function code (e.g. CC, TRADE, HELP)  —  Ctrl+K to focus"
          className="w-full bg-transparent font-mono text-sm uppercase tracking-wider text-amber-300 caret-amber-300 placeholder:text-amber-700 focus:outline-none"
          aria-label="Command bar"
          aria-expanded={open}
          aria-controls="command-bar-suggestions"
        />
        {error && (
          <span className="font-mono text-xs text-red-400">{error}</span>
        )}
      </div>

      {open && suggestions.length > 0 && (
        <ul
          id="command-bar-suggestions"
          role="listbox"
          className="absolute left-0 right-0 z-20 max-h-72 overflow-y-auto border border-amber-700/60 bg-black shadow-lg"
        >
          {suggestions.map((s, i) => {
            const isActive = i === highlight;
            return (
              <li
                key={s.code}
                role="option"
                aria-selected={isActive}
                onMouseDown={(e) => {
                  // mousedown so onBlur on input doesn't kill us first
                  e.preventDefault();
                  handleSuggestionClick(s);
                }}
                onMouseEnter={() => setHighlight(i)}
                className={`flex cursor-pointer items-baseline gap-3 px-3 py-2 font-mono text-sm ${
                  isActive
                    ? 'bg-amber-900/50 text-amber-200'
                    : 'text-amber-400 hover:bg-amber-950/50'
                }`}
              >
                <span className="w-16 font-bold text-amber-300">{s.code}</span>
                <span className="text-amber-400">{s.title}</span>
                <span className="ml-auto truncate text-xs text-amber-500/70">
                  {s.description}
                </span>
              </li>
            );
          })}
        </ul>
      )}
    </div>
  );
}
