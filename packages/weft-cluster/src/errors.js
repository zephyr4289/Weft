// errors.js — WC error taxonomy (Law 4: honest boundary validation).
//
// Spec: docs/weft-cluster/WIRE-V1.md §5 (NORMATIVE).
// Codes are byte-frozen: appending is allowed, renumbering is NOT.
// Mirrored 1:1 by python/weft_cluster/errors.py.

/** @type {Readonly<Record<string, number>>} */
export const WC = Object.freeze({
  WC_OK: 0,
  WC_E_BAD_MAGIC: 1,
  WC_E_BAD_VERSION: 2,
  WC_E_TRUNCATED: 3,
  WC_E_BAD_HEADER: 4,
  WC_E_BAD_CRC: 5,
  WC_E_UNKNOWN_NODE: 6,
  WC_E_STALE_SEQ: 7,
  WC_E_TOPIC_UNROUTED: 8,
  WC_E_PARTITION: 9,
  WC_E_NODE_LOST: 10,
  WC_E_SHUTDOWN: 11,
  WC_E_SCHEMA_MISMATCH: 12,
});

const NAMES = Object.freeze(Object.fromEntries(
  Object.entries(WC).map(([name, code]) => [code, name]),
));

/** Stable name for a numeric code (e.g. 5 -> "WC_E_BAD_CRC"). */
export function wcName(code) { return NAMES[code] ?? `WC_E_UNKNOWN_${code}`; }

/** Error carrying a stable WC code. `code` is always numeric and stable. */
export class WeftClusterError extends Error {
  /**
   * @param {number} code one of WC_*
   * @param {string} [detail] human context (never parsed by machines)
   */
  constructor(code, detail) {
    super(detail ? `${wcName(code)}: ${detail}` : wcName(code));
    this.name = 'WeftClusterError';
    this.code = code;
  }
}

/** True when `err` is a boundary error with the given stable code. */
export function hasCode(err, code) {
  return err instanceof Error && /** @type {any} */(err).code === code;
}
