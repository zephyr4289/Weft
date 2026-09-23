// types.mjs — the scalar type model shared by every weftc managed backend.
//
// One table of truth: byte size, natural alignment, and the per-language
// reader/writer primitives. The backends never hardcode a size or an offset
// expression; they derive everything from here so the parity audit can
// re-derive it independently.

export const TYPES = {
  u8:   { size: 1, align: 1, int: true, unsigned: true },
  u16:  { size: 2, align: 2, int: true, unsigned: true },
  u32:  { size: 4, align: 4, int: true, unsigned: true },
  u64:  { size: 8, align: 8, int: true, unsigned: true },
  i8:   { size: 1, align: 1, int: true, unsigned: false },
  i16:  { size: 2, align: 2, int: true, unsigned: false },
  i32:  { size: 4, align: 4, int: true, unsigned: false },
  i64:  { size: 8, align: 8, int: true, unsigned: false },
  f32:  { size: 4, align: 4, float: true },
  f64:  { size: 8, align: 8, float: true },
  bool: { size: 1, align: 1, int: true, unsigned: true },
};

/** DataView accessor suffixes (Law 2: littleEndian=true is emitted explicitly). */
export const TS_ACCESS = {
  u8:   { get: 'getUint8',    set: 'setUint8' },
  u16:  { get: 'getUint16',   set: 'setUint16' },
  u32:  { get: 'getUint32',   set: 'setUint32' },
  u64:  { get: 'getBigUint64', set: 'setBigUint64' },
  i8:   { get: 'getInt8',     set: 'setInt8' },
  i16:  { get: 'getInt16',    set: 'setInt16' },
  i32:  { get: 'getInt32',    set: 'setInt32' },
  i64:  { get: 'getBigInt64', set: 'setBigInt64' },
  f32:  { get: 'getFloat32',  set: 'setFloat32' },
  f64:  { get: 'getFloat64',  set: 'setFloat64' },
  bool: { get: 'getUint8',    set: 'setUint8' },
};

/** struct format characters (Python). Endian prefix '<' is mandatory (Law 2). */
export const PY_FMT = {
  u8: 'B', u16: 'H', u32: 'I', u64: 'Q',
  i8: 'b', i16: 'h', i32: 'i', i64: 'q',
  f32: 'f', f64: 'd',
  bool: 'B',
};

/** NumPy dtype strings — explicit little-endian byte order (Law 2). */
export const NP_DTYPE = {
  u8: 'u1', u16: '<u2', u32: '<u4', u64: '<u8',
  i8: 'i1', i16: '<i2', i32: '<i4', i64: '<i8',
  f32: '<f4', f64: '<f8',
  bool: 'u1',
};

/** Swift primitive types + explicit little-endian decode pattern. */
export const SWIFT_TYPE = {
  u8: 'UInt8', u16: 'UInt16', u32: 'UInt32', u64: 'UInt64',
  i8: 'Int8', i16: 'Int16', i32: 'Int32', i64: 'Int64',
  f32: 'Float', f64: 'Double',
  bool: 'UInt8',
};

/** dart:ffi payload types for Struct fields. */
export const DART_FFI_TYPE = {
  u8: 'ffi.UnsignedChar', u16: 'ffi.UnsignedShort', u32: 'ffi.UnsignedInt', u64: 'ffi.Uint64',
  i8: 'ffi.SignedChar', i16: 'ffi.Short', i32: 'ffi.Int', i64: 'ffi.Int64',
  f32: 'ffi.Float', f64: 'ffi.Double',
  bool: 'ffi.UnsignedChar',
};

/** Dart ByteData accessors — Endian.little is explicit (Law 2). */
export const DART_ACCESS = {
  u8:   { get: 'getUint8',   set: 'setUint8',   le: false },
  u16:  { get: 'getUint16',  set: 'setUint16',  le: true },
  u32:  { get: 'getUint32',  set: 'setUint32',  le: true },
  u64:  { get: 'getUint64',  set: 'setUint64',  le: true },
  i8:   { get: 'getInt8',    set: 'setInt8',    le: false },
  i16:  { get: 'getInt16',   set: 'setInt16',   le: true },
  i32:  { get: 'getInt32',   set: 'setInt32',   le: true },
  i64:  { get: 'getInt64',   set: 'setInt64',   le: true },
  f32:  { get: 'getFloat32', set: 'setFloat32', le: true },
  f64:  { get: 'getFloat64', set: 'setFloat64', le: true },
  bool: { get: 'getUint8',   set: 'setUint8',   le: false },
};

export function sizeOf(type, count) {
  return TYPES[type].size * (count ?? 1);
}
