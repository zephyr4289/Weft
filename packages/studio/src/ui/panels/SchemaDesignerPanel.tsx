'use client';

/**
 * Weft Studio — Panel A: Schema Designer & ABI Inspector.
 * .weft editor with Darcula syntax highlighting, live error squiggles,
 * semantic gutter markers (struct size / alignment), breadcrumbs, Ctrl+Space
 * autocomplete, and the Real-Time Multi-Target Codegen Drawer (7 languages,
 * <1 ms per keystroke). COLD plane: typing is user interaction; React state
 * is permitted here by the two-plane law.
 */

import { getReact, spy, SPY } from '../../engine/react-adapter';
import { markRender } from '../hooks';
import { STUDIO_THEME as T } from '../theme';
import {
  highlightDocument, completionsFor,
} from '../highlight';
import type { ParseResult, SchemaDoc } from '../../engine/schema';
import type { LayoutResult } from '../../engine/layout';
import { generate, CODEGEN_TARGETS, type CodegenTarget } from '../../engine/codegen';

interface Props {
  src: string;
  onSrc: (next: string) => void;
  parse: ParseResult;
  layout: LayoutResult;
  schemaHash: string;
}

interface ACState {
  open: boolean;
  prefix: string;
  start: number;
  items: string[];
  sel: number;
}

export function SchemaDesignerPanel({ src, onSrc, parse, layout, schemaHash }: Props): unknown {
  markRender(SPY.SCHEMA_DESIGNER);
  const React = getReact() as unknown as {
    useRef: (v: never) => { current: never };
    useState: <S>(v: S) => [S, (v: S) => void];
    useEffect: (fn: () => void | (() => void), deps?: unknown[]) => void;
    useMemo: <T>(fn: () => T, deps: unknown[]) => T;
    useCallback: <T extends (...a: never[]) => unknown>(fn: T, deps: unknown[]) => T;
  };
  const taRef = React.useRef(null) as unknown as { current: HTMLTextAreaElement | null };
  const preRef = React.useRef(null) as unknown as { current: HTMLPreElement | null };
  const gutRef = React.useRef(null) as unknown as { current: HTMLDivElement | null };
  const [caretLine, setCaretLine] = React.useState(0);
  const [lang, setLang] = React.useState<CodegenTarget>('c');
  const [ac, setAc] = React.useState<ACState>({ open: false, prefix: '', start: 0, items: [], sel: 0 });
  const [copied, setCopied] = React.useState(false);

  const lines = React.useMemo(() => highlightDocument(src), [src]);
  const structNames = React.useMemo(
    () => (parse.doc ? parse.doc.structs.map((s) => s.name) : []),
    [parse],
  );
  const codegen = React.useMemo(() => {
    if (!parse.doc || !parse.ok) return null;
    const t0 = (typeof performance !== 'undefined' ? performance.now() : Date.now());
    const out: Record<string, string> = {};
    for (const t of CODEGEN_TARGETS) out[t] = generate(t, parse.doc as SchemaDoc, layout);
    const t1 = (typeof performance !== 'undefined' ? performance.now() : Date.now());
    return { out, ms: Math.max(0.01, t1 - t0) };
  }, [parse, layout]);

  // gutter info per line: struct size markers + diagnostic markers
  const gutter = React.useMemo(() => {
    const info = new Map<number, { text: string; kind: 'size' | 'error' | 'warn' }>();
    for (const name of layout.order) {
      const L = layout.layouts.get(name)!;
      // struct declaration line: find "struct <name>" in src
      const re = new RegExp(`^\\s*struct\\s+${name}\\b`, 'm');
      const m = re.exec(src);
      if (m) {
        const line = src.slice(0, m.index).split('\n').length;
        info.set(line, { text: `${L.size}B·${L.alignment}B`, kind: 'size' });
      }
    }
    for (const d of parse.diagnostics) {
      const existing = info.get(d.line);
      if (d.severity === 'error' || (!existing || existing.kind === 'warn')) {
        info.set(d.line, { text: d.severity === 'error' ? '⨯' : '⚠', kind: d.severity === 'error' ? 'error' : 'warn' });
      }
    }
    return info;
  }, [src, parse, layout]);

  // breadcrumbs from caret line
  const crumbs = React.useMemo(() => {
    const upTo = src.split('\n').slice(0, caretLine).join('\n');
    const structMatches = Array.from(upTo.matchAll(/struct\s+(\w+)/g));
    const crumb: string[] = ['main.weft'];
    if (structMatches.length) crumb.push(structMatches[structMatches.length - 1][1]);
    const fieldMatch = Array.from(upTo.matchAll(/(\w+)\s*:\s*(\w+)/g)).pop();
    if (fieldMatch && structMatches.length) crumb.push(fieldMatch[1]);
    return crumb;
  }, [src, caretLine]);

  const syncScroll = () => {
    const ta = taRef.current;
    if (!ta) return;
    if (preRef.current) {
      preRef.current.scrollTop = ta.scrollTop;
      preRef.current.scrollLeft = ta.scrollLeft;
    }
    if (gutRef.current) gutRef.current.scrollTop = ta.scrollTop;
  };

  const updateCaret = () => {
    const ta = taRef.current;
    if (!ta) return;
    const line = ta.value.slice(0, ta.selectionStart).split('\n').length;
    setCaretLine(line);
  };

  const tryAutocomplete = (text: string, caret: number, manual: boolean) => {
    // word prefix before caret
    let i = caret;
    while (i > 0 && /[A-Za-z0-9_(@]/.test(text[i - 1])) i--;
    const prefix = text.slice(i, caret);
    const manualPrefix = manual && !prefix ? '' : prefix;
    if (!manual && manualPrefix !== '@' && manualPrefix.length < 2) {
      setAc((a) => ({ ...a, open: false }));
      return;
    }
    const items = completionsFor(manualPrefix, structNames);
    if (items.length === 0) { setAc((a) => ({ ...a, open: false })); return; }
    setAc({ open: true, prefix: manualPrefix, start: caret - manualPrefix.length, items, sel: 0 });
  };

  const insertCompletion = (item: string) => {
    const ta = taRef.current;
    if (!ta) return;
    const caret = ta.selectionStart;
    const next = src.slice(0, ac.start) + item + src.slice(caret);
    onSrc(next);
    setAc((a) => ({ ...a, open: false }));
    const pos = ac.start + item.length;
    requestAnimationFrame(() => {
      if (taRef.current) { taRef.current.focus(); taRef.current.setSelectionRange(pos, pos); }
    });
  };

  const onKeyDown = (e: KeyboardEvent) => {
    const ta = taRef.current;
    if (!ta) return;
    if (ac.open) {
      if (e.key === 'ArrowDown') { e.preventDefault(); setAc((a) => ({ ...a, sel: (a.sel + 1) % a.items.length })); return; }
      if (e.key === 'ArrowUp') { e.preventDefault(); setAc((a) => ({ ...a, sel: (a.sel + a.items.length - 1) % a.items.length })); return; }
      if (e.key === 'Enter' || e.key === 'Tab') { e.preventDefault(); insertCompletion(ac.items[ac.sel]); return; }
      if (e.key === 'Escape') { e.preventDefault(); setAc((a) => ({ ...a, open: false })); return; }
    }
    if (e.key === ' ' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      tryAutocomplete(ta.value, ta.selectionStart, true);
      return;
    }
    if (e.key === 'Tab') {
      e.preventDefault();
      const s = ta.selectionStart;
      const next = src.slice(0, s) + '  ' + src.slice(ta.selectionEnd);
      onSrc(next);
      requestAnimationFrame(() => ta.setSelectionRange(s + 2, s + 2));
      return;
    }
  };

  const onChange = (e: Event) => {
    const ta = e.target as HTMLTextAreaElement;
    onSrc(ta.value);
    const caret = ta.selectionStart;
    const before = ta.value[caret - 1];
    tryAutocomplete(ta.value, caret, before === '@');
  };

  const copyCode = () => {
    if (!codegen) return;
    const text = codegen.out[lang];
    try {
      void navigator.clipboard.writeText(text);
      setCopied(true);
      setTimeout(() => setCopied(false), 1200);
    } catch { /* clipboard unavailable — non-fatal */ }
  };

  const errCount = parse.diagnostics.filter((d) => d.severity === 'error').length;
  const warnCount = parse.diagnostics.length - errCount;
  const structCount = parse.doc ? parse.doc.structs.length : 0;
  const lineCount = lines.length;

  return h('div', { style: { display: 'flex', flexDirection: 'row', height: '100%', minHeight: 0, background: T.bg } },
    // ---- editor column ----
    h('div', { style: { flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' } },
      // breadcrumb bar
      h('div', { style: crumbBarStyle },
        crumbs.map((c, i) => h('span', { key: i, style: { display: 'flex', alignItems: 'center', gap: 6 } },
          i > 0 ? h('span', { style: { color: T.textDim } }, '›') : null,
          h('span', { style: { color: i === crumbs.length - 1 ? T.textBright : T.textDim, fontSize: 11, fontFamily: FONT } }, c),
        )),
        h('span', { style: { flex: 1 } }),
        h('span', { style: { color: schemaHash ? T.textDim : T.red, fontSize: 10, fontFamily: FONT } },
          `sha ${schemaHash.slice(0, 12)} · ${structCount} structs · ${lineCount} lines`),
      ),
      // editor + gutter
      h('div', { style: { flex: 1, display: 'flex', minHeight: 0, position: 'relative' } },
        // gutter
        h('div', { ref: gutRef as never, style: gutterStyle },
          lines.map((_, i) => {
            const g = gutter.get(i + 1);
            const color = g?.kind === 'error' ? T.red : g?.kind === 'warn' ? T.yellow : g?.kind === 'size' ? T.greenBright : T.textDim;
            return h('div', { key: i, style: { ...gutterLine, color } },
              h('span', null, String(i + 1)),
              g ? h('span', { style: { fontSize: 9, marginLeft: 4, color } }, g.kind === 'size' ? g.text : g.text) : null,
            );
          }),
        ),
        // highlight + squiggles + input
        h('div', { style: { position: 'relative', flex: 1, minWidth: 0 } },
          h('pre', {
            ref: preRef as never,
            style: preStyle,
            'aria-hidden': true,
            dangerouslySetInnerHTML: { __html: lines.join('\n') },
          }),
          // squiggle layer
          h('div', { style: { position: 'absolute', inset: 0, pointerEvents: 'none', overflow: 'hidden', paddingTop: PAD_Y, paddingLeft: PAD_X } },
            parse.diagnostics.filter((d) => d.severity === 'error').slice(0, 12).map((d, i) =>
              h('div', {
                key: i,
                title: d.message,
                style: {
                  position: 'absolute', top: (d.line - 1) * LINE_H, left: (d.col - 1) * 7.8,
                  width: Math.min(360, Math.max(40, d.message.length * 5)), height: LINE_H,
                  borderBottom: `2px dotted ${T.red}`, opacity: 0.9,
                },
              })),
          ),
          h('textarea', {
            ref: taRef as never,
            value: src,
            onScroll: syncScroll,
            onKeyDown: onKeyDown as never,
            onInput: onChange as never,
            onClick: updateCaret,
            onKeyUp: updateCaret,
            spellCheck: false,
            style: { ...preStyle, color: 'transparent', caretColor: T.textBright, background: 'transparent', resize: 'none', outline: 'none' },
          }),
        ),
      ),
      // status strip
      h('div', { style: stripStyle },
        h('span', { style: { color: errCount ? T.red : T.textDim, fontSize: 10 } }, `${errCount} errors`),
        h('span', { style: { color: warnCount ? T.yellow : T.textDim, fontSize: 10 } }, `${warnCount} warnings`),
        h('span', { style: { flex: 1 } }),
        h('span', { style: { color: T.textDim, fontSize: 10 } }, 'Ln ' + caretLine + '  ·  Ctrl+Space autocomplete  ·  Tab indents'),
      ),
    ),
    // ---- codegen drawer ----
    h('div', { style: { width: 380, flexShrink: 0, borderLeft: `1px solid ${T.border}`, display: 'flex', flexDirection: 'column', background: T.chromeAlt } },
      h('div', { style: { ...headStyle, display: 'flex', alignItems: 'center', gap: 8 } },
        h('span', { style: { color: T.textBright, fontSize: 11, fontWeight: 600, letterSpacing: 0.4 } }, 'CODEGEN — MULTI-TARGET'),
        h('span', { style: { flex: 1 } }),
        codegen ? h('span', { style: { color: T.greenBright, fontSize: 10, fontFamily: FONT } }, codegen.ms.toFixed(2) + ' ms') : null,
        h('button', { onClick: copyCode as never, style: miniBtn }, copied ? 'COPIED' : 'COPY'),
      ),
      h('div', { style: { display: 'flex', flexWrap: 'wrap', gap: 2, padding: '6px 8px', borderBottom: `1px solid ${T.borderSoft}` } },
        CODEGEN_TARGETS.map((t) => h('button', {
          key: t,
          onClick: () => setLang(t),
          style: {
            ...langTab,
            background: lang === t ? T.accentSoft : 'transparent',
            color: lang === t ? T.textBright : T.textDim,
            borderColor: lang === t ? T.accent : 'transparent',
          },
        }, t)),
      ),
      h('pre', { style: { flex: 1, margin: 0, padding: 12, overflow: 'auto', fontSize: 11, lineHeight: '17px', fontFamily: FONT, color: T.tokText, whiteSpace: 'pre-wrap', wordBreak: 'break-word' } },
        codegen ? codegen.out[lang] : '// fix schema errors to enable codegen'),
    ),
    // ---- autocomplete popup ----
    ac.open ? h('div', { style: acPopup }, ac.items.map((it, i) => h('div', {
      key: it,
      onMouseDown: (e: Event) => { e.preventDefault(); insertCompletion(it); },
      style: { ...acItem, background: i === ac.sel ? T.selection : 'transparent' },
    }, it))) : null,
  );
}

// ---------------------------------------------------------------- helpers --

function h(type: string, props: Record<string, unknown> | null, ...children: unknown[]): unknown {
  const React = getReact() as unknown as { createElement: (...a: unknown[]) => unknown };
  return React.createElement(type, props ?? null, ...children);
}

export const FONT = 'ui-monospace, "JetBrains Mono", "SF Mono", Menlo, Consolas, monospace';
const LINE_H = 20;
const PAD_X = 12;
const PAD_Y = 10;

const preStyle: Record<string, string | number> = {
  position: 'absolute', inset: 0, margin: 0, padding: `${PAD_Y}px ${PAD_X}px`,
  fontFamily: FONT, fontSize: 13, lineHeight: `${LINE_H}px`, color: T.tokText,
  overflow: 'auto', whiteSpace: 'pre', tabSize: 2, pointerEvents: 'none',
};

const gutterStyle: Record<string, string | number> = {
  width: 84, flexShrink: 0, overflow: 'hidden', paddingTop: PAD_Y,
  background: T.bg, borderRight: `1px solid ${T.borderSoft}`,
  fontFamily: FONT, fontSize: 11, lineHeight: `${LINE_H}px`, textAlign: 'right',
  userSelect: 'none',
};

const gutterLine: Record<string, string | number> = {
  paddingRight: 8, paddingLeft: 6, display: 'flex', justifyContent: 'flex-end', gap: 2,
};

const crumbBarStyle: Record<string, string | number> = {
  display: 'flex', alignItems: 'center', gap: 6, height: 28, padding: '0 12px',
  background: T.chrome, borderBottom: `1px solid ${T.border}`, flexShrink: 0,
};

const stripStyle: Record<string, string | number> = {
  display: 'flex', gap: 14, alignItems: 'center', height: 24, padding: '0 12px',
  background: T.chrome, borderTop: `1px solid ${T.border}`, flexShrink: 0,
};

const headStyle: Record<string, string | number> = {
  height: 30, padding: '0 10px', background: T.chrome, borderBottom: `1px solid ${T.border}`,
  flexShrink: 0,
};

const langTab: Record<string, string | number> = {
  padding: '2px 10px', fontSize: 10.5, fontFamily: FONT, borderRadius: 4,
  border: '1px solid transparent', cursor: 'pointer', textTransform: 'uppercase' as never,
};

const miniBtn: Record<string, string | number> = {
  padding: '2px 10px', fontSize: 10, fontFamily: FONT, background: T.accentSoft,
  color: T.textBright, border: `1px solid ${T.accent}`, borderRadius: 4, cursor: 'pointer',
};

const acPopup: Record<string, string | number> = {
  position: 'absolute', left: '30%', top: '18%', width: 220, zIndex: 40,
  background: T.chromeDeep, border: `1px solid ${T.accent}`, borderRadius: 6,
  boxShadow: '0 8px 24px rgba(0,0,0,.5)', overflow: 'hidden', padding: 4,
};

const acItem: Record<string, string | number> = {
  padding: '4px 8px', fontSize: 11.5, fontFamily: FONT, color: T.text, borderRadius: 4,
  cursor: 'pointer',
};

void spy;
