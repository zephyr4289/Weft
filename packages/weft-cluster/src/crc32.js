// crc32.js — IEEE CRC-32 (reflected, poly 0xEDB88320), byte-exact with
// Python zlib.crc32 and the core kernel CRC used by WTR1 ring headers.
// Table built once at module load; hot path is a pure table walk.

const TABLE = new Int32Array(256);
{
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) !== 0 ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    TABLE[n] = c | 0;
  }
}

/**
 * IEEE CRC-32 over a byte range of a typed array. Zero allocation.
 * @param {Int8Array|Uint8Array} u8 byte source (Uint8Array view of the buffer)
 * @param {number} start inclusive byte offset
 * @param {number} end exclusive byte offset
 * @returns {number} unsigned CRC-32
 */
export function crc32Range(u8, start, end) {
  let c = -1;
  for (let i = start; i < end; i++) c = TABLE[(c ^ u8[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

/**
 * IEEE CRC-32 of a byte array (convenience / cold path).
 * @param {Uint8Array} bytes
 * @returns {number}
 */
export function crc32(bytes) { return crc32Range(bytes, 0, bytes.length); }
