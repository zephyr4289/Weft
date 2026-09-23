// rules.ts — per-language hot-annotation and heap-allocation-primitive tables.

import type { MaskConfig } from './mask.ts';

export type SigLineExemption = 'skip' | 'strip-suffix' | 'none';

export interface Rule {
  id: string;
  name: string;
  re: RegExp;
  message: string;
  hint: string;
  /** How to treat the signature line (the line holding the body brace). */
  sig: SigLineExemption;
}

export interface LangConfig {
  id: string;
  label: string;
  exts: string[];
  mask: MaskConfig;
  /** Hot-path annotation regex (applied to the semi-masked view). */
  annotationRe: RegExp;
  /** See HotConfig.prefixRe. Omitted for C/C++ attribute syntax. */
  prefixRe?: RegExp;
  rules: Rule[];
}

const SIGNATURE_TAIL_ARROW = /\)\s*=>\s*\{?\s*$/;

function tsRules(): Rule[] {
  return [
    {
      id: 'WV-TS-001',
      name: 'new-expr',
      re: /\bnew\s+[A-Za-z_$]/g,
      message: "heap allocation via 'new' in @hot function",
      hint: 'preallocate outside the hot scope or write into preallocated slot views',
      sig: 'none',
    },
    {
      id: 'WV-TS-002',
      name: 'object-literal',
      re: /(?:\breturn\b|[=(,])\s*\{/g,
      message: 'heap boxing via object literal in @hot function',
      hint: 'reuse a preallocated state object or encode into an existing buffer',
      sig: 'none',
    },
    {
      id: 'WV-TS-003',
      name: 'array-literal',
      re: /(?:\breturn\b|[=(,])\s*\[/g,
      message: 'heap allocation via array literal in @hot function',
      hint: 'write into a preallocated typed array instead',
      sig: 'none',
    },
    {
      id: 'WV-TS-004',
      name: 'spread',
      re: /\.\.\./g,
      message: 'rest/spread materializes an array or object in @hot function',
      hint: 'use explicit indexing over preallocated storage',
      sig: 'none',
    },
    {
      id: 'WV-TS-005',
      name: 'arrow-closure',
      re: /=>/g,
      message: 'dynamic closure allocated in @hot function',
      hint: 'hoist the closure to module scope or use static dispatch',
      sig: 'strip-suffix',
    },
    {
      id: 'WV-TS-006',
      name: 'function-expr',
      re: /\bfunction\b/g,
      message: 'function expression allocated in @hot function',
      hint: 'declare named functions at module scope',
      sig: 'skip',
    },
    {
      id: 'WV-TS-007',
      name: 'json-api',
      re: /\bJSON\s*\.\s*(?:parse|stringify)\b/g,
      message: 'JSON.parse/stringify allocates in @hot function',
      hint: 'decode binary wire formats directly over the buffer',
      sig: 'none',
    },
    {
      id: 'WV-TS-008',
      name: 'object-api',
      re: /\bObject\s*\.\s*(?:assign|create|entries|fromEntries|keys|values|groupBy)\b/g,
      message: 'Object.* call allocates in @hot function',
      hint: 'operate on fixed-shape preallocated records',
      sig: 'none',
    },
    {
      id: 'WV-TS-009',
      name: 'array-alloc-api',
      re: /\bArray\s*\.\s*(?:from|of)\b|\.\s*(?:map|filter|flatMap|slice|concat|toSorted|toReversed|with)\s*\(/g,
      message: 'allocating array method in @hot function',
      hint: 'iterate manually into preallocated output',
      sig: 'none',
    },
    {
      id: 'WV-TS-010',
      name: 'structured-clone',
      re: /\bstructuredClone\s*\(/g,
      message: 'structuredClone deep-copies onto the heap in @hot function',
      hint: 'share memory via the ring instead of copying',
      sig: 'none',
    },
  ];
}

function cRules(): Rule[] {
  return [
    {
      id: 'WV-C-001',
      name: 'malloc',
      re: /\bmalloc\s*\(/g,
      message: 'malloc in weft_hot function (hot path must be allocation-free)',
      hint: 'use caller-provided storage or a preallocated arena slab',
      sig: 'none',
    },
    {
      id: 'WV-C-002',
      name: 'calloc',
      re: /\bcalloc\s*\(/g,
      message: 'calloc in weft_hot function',
      hint: 'zero once during init, not per tick',
      sig: 'none',
    },
    {
      id: 'WV-C-003',
      name: 'realloc',
      re: /\brealloc\s*\(/g,
      message: 'realloc in weft_hot function',
      hint: 'bound capacity up front; grow is a cold-path operation',
      sig: 'none',
    },
    {
      id: 'WV-C-004',
      name: 'aligned-alloc',
      re: /\b(?:aligned_alloc|posix_memalign|memalign|pvalloc|reallocarray)\s*\(/g,
      message: 'dynamic aligned allocation in weft_hot function',
      hint: 'align slabs once at setup',
      sig: 'none',
    },
    {
      id: 'WV-C-005',
      name: 'strdup',
      re: /\bstrn?dup\s*\(/g,
      message: 'strdup/strndup copies onto the heap in weft_hot function',
      hint: 'store offsets into the ring payload instead of copies',
      sig: 'none',
    },
  ];
}

function cppExtraRules(): Rule[] {
  return [
    {
      id: 'WV-CPP-001',
      name: 'new-expr',
      re: /\bnew\b/g,
      message: "'new' in weft_hot function",
      hint: 'use placement into a preallocated slab or a value on the stack',
      sig: 'none',
    },
    {
      id: 'WV-CPP-002',
      name: 'make-shared',
      re: /\bmake_shared\b/g,
      message: 'make_shared control-block + object allocation in weft_hot function',
      hint: 'share via the zero-copy ring; ownership belongs to the buffer',
      sig: 'none',
    },
    {
      id: 'WV-CPP-003',
      name: 'make-unique',
      re: /\bmake_unique\b/g,
      message: 'make_unique heap allocation in weft_hot function',
      hint: 'preallocate a pool and hand out indices',
      sig: 'none',
    },
  ];
}

function rustRules(): Rule[] {
  return [
    {
      id: 'WV-RS-001',
      name: 'box-new',
      re: /\bBox\s*::\s*new\b/g,
      message: 'Box::new heap allocation in #[weft_hot] function',
      hint: 'store by value in a preallocated slot array',
      sig: 'none',
    },
    {
      id: 'WV-RS-002',
      name: 'rc-arc-new',
      re: /\b(?:Rc|Arc)\s*::\s*new\b/g,
      message: 'Rc/Arc reference-count allocation in #[weft_hot] function',
      hint: 'share the ring buffer itself instead of refcounted payloads',
      sig: 'none',
    },
    {
      id: 'WV-RS-003',
      name: 'vec-macro',
      re: /\bvec\s*!/g,
      message: 'vec![] allocates in #[weft_hot] function',
      hint: 'use a preallocated [T; N] or with_capacity buffer created at init',
      sig: 'none',
    },
    {
      id: 'WV-RS-004',
      name: 'vec-new',
      re: /\bVec\s*::\s*new\b/g,
      message: 'Vec::new defers a heap growth into the hot path on first push',
      hint: 'reserve capacity at init (Vec::with_capacity) outside the hot scope',
      sig: 'none',
    },
    {
      id: 'WV-RS-005',
      name: 'string-from',
      re: /\bString\s*::\s*(?:from|with_capacity|from_utf8|from_utf8_lossy|from_str)\b/g,
      message: 'String construction allocates in #[weft_hot] function',
      hint: 'log via fixed char buffers or numeric channels',
      sig: 'none',
    },
    {
      id: 'WV-RS-006',
      name: 'to-string',
      re: /\.\s*to_string\s*\(\s*\)/g,
      message: 'to_string() allocates in #[weft_hot] function',
      hint: 'defer formatting to the cold path',
      sig: 'none',
    },
    {
      id: 'WV-RS-007',
      name: 'to-owned',
      re: /\.\s*to_owned\s*\(\s*\)/g,
      message: 'to_owned() copies onto the heap in #[weft_hot] function',
      hint: 'borrow instead of owning inside the tick',
      sig: 'none',
    },
    {
      id: 'WV-RS-008',
      name: 'clone',
      re: /\.\s*clone\s*\(\s*\)/g,
      message: '.clone() may deep-copy in #[weft_hot] function',
      hint: 'pass references or indices; clone nothing per tick',
      sig: 'none',
    },
    {
      id: 'WV-RS-009',
      name: 'format-macro',
      re: /\b(?:format|println|eprintln|print|eprint)\s*!/g,
      message: 'formatting macro allocates in #[weft_hot] function',
      hint: 'emit structured binary records; format at the consumer',
      sig: 'none',
    },
    {
      id: 'WV-RS-010',
      name: 'collect',
      re: /\.\s*collect\b/g,
      message: '.collect() materializes a heap container in #[weft_hot] function',
      hint: 'fold into preallocated storage',
      sig: 'none',
    },
    {
      id: 'WV-RS-011',
      name: 'rust-closure',
      re: /(?:\.\s*[A-Za-z_]\w*\s*\(\s*\|)|(?:=\s*\|[^|]*\|)|(?:\bmove\s+\|)/g,
      message: 'closure captured/allocated in #[weft_hot] function',
      hint: 'use plain loops; reserve iterator adaptors for the cold path',
      sig: 'none',
    },
  ];
}

function swiftRules(): Rule[] {
  return [
    {
      id: 'WV-SW-001',
      name: 'pointer-allocate',
      re: /\.\s*allocate\s*\(|\bmalloc\s*\(|\bcalloc\s*\(/g,
      message: 'heap allocation in @weft_hot function',
      hint: 'allocate capacity once at init; hand out preallocated buffers',
      sig: 'none',
    },
    {
      id: 'WV-SW-002',
      name: 'collection-init',
      re: /(?<!:\s)\b(?:Array|Dictionary|Set|String|Data)\s*[<(]/g,
      message: 'collection/string construction allocates in @weft_hot function',
      hint: 'reuse preallocated storage created outside the hot scope',
      sig: 'none',
    },
    {
      id: 'WV-SW-003',
      name: 'swift-closure',
      re: /\{\s*\(|\{\s*\w+\s+(?:\w+\s+)?in\b|\.\s*\w+\s*\{/g,
      message: 'closure literal allocated in @weft_hot function',
      hint: 'hoist closures to scope level or use plain indexed loops',
      sig: 'none',
    },
  ];
}

function dartRules(): Rule[] {
  return [
    {
      id: 'WV-DT-001',
      name: 'dart-new',
      re: /\bnew\s+\w/g,
      message: "'new' heap allocation in @weftHot function",
      hint: 'preallocate and reuse; Dart never reclaims mid-frame for free',
      sig: 'none',
    },
    {
      id: 'WV-DT-002',
      name: 'list-ctor',
      re: /\bList\s*[<(]|\bList\s*\.\s*(?:filled|generate|from|of)\b/g,
      message: 'List construction allocates in @weftHot function',
      hint: 'use a preallocated Float64List/Uint8List field',
      sig: 'none',
    },
    {
      id: 'WV-DT-003',
      name: 'map-ctor',
      re: /\bMap\s*[<(]|\bMap\s*\.\s*(?:from|of|fromIterables)\b/g,
      message: 'Map construction allocates in @weftHot function',
      hint: 'index into parallel preallocated lists instead',
      sig: 'none',
    },
    {
      id: 'WV-DT-004',
      name: 'dart-literal',
      re: /(?:\breturn\b|[=(,])\s*[\[{]/g,
      message: 'collection literal allocates in @weftHot function',
      hint: 'fill preallocated typed lists',
      sig: 'none',
    },
    {
      id: 'WV-DT-005',
      name: 'stringbuffer',
      re: /\bStringBuffer\s*\(/g,
      message: 'StringBuffer allocates in @weftHot function',
      hint: 'format on the UI/cold side only',
      sig: 'none',
    },
    {
      id: 'WV-DT-006',
      name: 'ffi-alloc',
      re: /\b(?:calloc|malloc|posix_memalign)\s*[<(]|\.\s*allocate\s*\(/g,
      message: 'native heap allocation in @weftHot function',
      hint: 'carve from an arena created at engine init',
      sig: 'none',
    },
    {
      id: 'WV-DT-007',
      name: 'dart-closure-block',
      re: /=\s*\([^()]*\)\s*\{/g,
      message: 'closure literal assigned in @weftHot function',
      hint: 'declare named functions at library scope',
      sig: 'none',
    },
    {
      id: 'WV-DT-008',
      name: 'dart-arrow',
      re: /\)\s*=>/g,
      message: 'arrow closure allocated in @weftHot function',
      hint: 'use static dispatch or hoisted top-level functions',
      sig: 'skip',
    },
  ];
}

const TS_MASK: MaskConfig = {
  lineComments: ['//'],
  blockComments: [{ open: '/*', close: '*/', nested: false }],
  strings: [
    { open: '"', close: '"', escape: true },
    { open: "'", close: "'", escape: true },
    { open: '`', close: '`', escape: true },
  ],
};

const C_MASK: MaskConfig = {
  lineComments: ['//'],
  blockComments: [{ open: '/*', close: '*/', nested: false }],
  strings: [
    { open: '"', close: '"', escape: true },
    { open: "'", close: "'", escape: true },
  ],
};

const RUST_MASK: MaskConfig = {
  lineComments: ['//'],
  blockComments: [{ open: '/*', close: '*/', nested: true }],
  strings: [{ open: '"', close: '"', escape: true, rawPrefixes: ['r'] }],
};

const SWIFT_MASK: MaskConfig = {
  lineComments: ['//'],
  blockComments: [{ open: '/*', close: '*/', nested: true }],
  strings: [{ open: '"', close: '"', escape: true }],
  tripleQuote: '"""',
};

const DART_MASK: MaskConfig = {
  lineComments: ['//'],
  blockComments: [{ open: '/*', close: '*/', nested: true }],
  strings: [
    { open: '"', close: '"', escape: true, rawPrefixes: ['r'] },
    { open: "'", close: "'", escape: true, rawPrefixes: ['r'] },
  ],
};

const COMMENT_PREFIX = /^(?:\s|\/|\*)*$/;
const RUST_PREFIX = /^(?:\s|#)*$/;

export const LANGS: LangConfig[] = [
  {
    id: 'ts',
    label: 'TypeScript/JavaScript',
    exts: ['.ts', '.tsx', '.mts', '.cts', '.js', '.mjs', '.cjs', '.jsx'],
    mask: TS_MASK,
    annotationRe: /@(?:weft_)?hot\b/,
    prefixRe: COMMENT_PREFIX,
    rules: tsRules(),
  },
  {
    id: 'c',
    label: 'C',
    exts: ['.c', '.h'],
    mask: C_MASK,
    annotationRe: /__attribute__\s*\(\s*\(\s*weft_hot\s*\)\s*\)|\[\[weft::hot\]\]/,
    rules: cRules(),
  },
  {
    id: 'cpp',
    label: 'C++',
    exts: ['.cc', '.cpp', '.cxx', '.hpp', '.hh', '.hxx'],
    mask: C_MASK,
    annotationRe: /__attribute__\s*\(\s*\(\s*weft_hot\s*\)\s*\)|\[\[weft::hot\]\]/,
    rules: [...cRules(), ...cppExtraRules()],
  },
  {
    id: 'rust',
    label: 'Rust',
    exts: ['.rs'],
    mask: RUST_MASK,
    annotationRe: /#\[\s*weft_hot\s*\]/,
    prefixRe: RUST_PREFIX,
    rules: rustRules(),
  },
  {
    id: 'swift',
    label: 'Swift',
    exts: ['.swift'],
    mask: SWIFT_MASK,
    annotationRe: /@weft_hot\b/,
    prefixRe: COMMENT_PREFIX,
    rules: swiftRules(),
  },
  {
    id: 'dart',
    label: 'Dart',
    exts: ['.dart'],
    mask: DART_MASK,
    annotationRe: /@weftHot\b/,
    prefixRe: COMMENT_PREFIX,
    rules: dartRules(),
  },
];

export function languageForPath(filePath: string): LangConfig | null {
  const dot = filePath.lastIndexOf('.');
  if (dot < 0) return null;
  const ext = filePath.slice(dot).toLowerCase();
  for (const lang of LANGS) {
    if (lang.exts.includes(ext)) return lang;
  }
  return null;
}

export { SIGNATURE_TAIL_ARROW };
