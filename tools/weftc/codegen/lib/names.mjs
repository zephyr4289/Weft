// names.mjs — identifier casing + per-language reserved-word avoidance.
//
// Generated code must compile on first try in every target language, so a
// schema field named `class` or `extension` must never leak through raw.
// Weft schemas use camelCase logical names; backends transliterate.

const RESERVED = new Map([
  ['swift', new Set(['self', 'Self', 'class', 'struct', 'enum', 'extension', 'import', 'return',
    'var', 'let', 'func', 'static', 'public', 'private', 'internal', 'init', 'deinit', 'in', 'out',
    'if', 'else', 'guard', 'switch', 'case', 'default', 'for', 'while', 'repeat', 'break', 'continue',
    'as', 'is', 'nil', 'true', 'false', 'typealias', 'associatedtype', 'where', 'operator', 'defer'])],
  ['dart', new Set(['this', 'class', 'extends', 'implements', 'with', 'abstract', 'final', 'const',
    'var', 'late', 'static', 'void', 'return', 'if', 'else', 'for', 'while', 'do', 'switch', 'case',
    'default', 'break', 'continue', 'new', 'is', 'as', 'in', 'null', 'true', 'false', 'super', 'get',
    'set', 'factory', 'operator', 'typedef', 'enum', 'mixin', 'extension', 'await', 'async', 'yield'])],
  ['ts', new Set(['class', 'const', 'let', 'var', 'function', 'return', 'if', 'else', 'switch',
    'case', 'default', 'break', 'continue', 'for', 'while', 'do', 'new', 'delete', 'typeof', 'void',
    'this', 'super', 'extends', 'implements', 'interface', 'type', 'enum', 'namespace', 'module',
    'declare', 'public', 'private', 'protected', 'static', 'readonly', 'get', 'set', 'await', 'yield',
    'null', 'undefined', 'true', 'false', 'import', 'export', 'from', 'as', 'in', 'of'])],
  ['python', new Set(['False', 'None', 'True', 'and', 'as', 'assert', 'async', 'await', 'break',
    'class', 'continue', 'def', 'del', 'elif', 'else', 'except', 'finally', 'for', 'from', 'global',
    'if', 'import', 'in', 'is', 'lambda', 'nonlocal', 'not', 'or', 'pass', 'raise', 'return', 'try',
    'while', 'with', 'yield', 'self'])],
]);

export function pascal(name) {
  return name
    .replace(/([a-z0-9])([A-Z])/g, '$1 $2')
    .split(/[^A-Za-z0-9]+/)
    .filter(Boolean)
    .map((w) => w[0].toUpperCase() + w.slice(1))
    .join('');
}

export function camel(name) {
  const p = pascal(name);
  return p[0].toLowerCase() + p.slice(1);
}

export function snake(name) {
  return pascal(name)
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2')
    .toLowerCase();
}

export function screaming(name) {
  return snake(name).toUpperCase();
}

/** Escape a logical field name for a given language's identifier rules. */
export function safeIdent(name, lang) {
  const reserved = RESERVED.get(lang);
  if (!reserved) return name;
  if (reserved.has(name)) return name + '_';
  return name;
}
