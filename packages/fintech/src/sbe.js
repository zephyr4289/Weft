// sbe.js — schema-driven Simple Binary Encoding flyweight decoder.
//
// Schema descriptors are JSON (docs/adapters/MANAGED-SEAMS-V1.md §3). The
// golden fixture schema (sbe-schema.json) declares littleEndian: true; the
// decoder honors the declaration for every field read.
//
// Framing: records arrive as [u32 LE length][SBE message]. The decoder reads
// the 8-byte message header (blockLength, templateId, schemaId, version),
// resolves the template, decodes declared fixed fields, and SKIPS anything
// beyond blockLength (SBE extension fields) by the transport length —
// forward compatibility without schema churn.
//
// Zero-allocation contract: one decoder instance, one preallocated output
// vector; the caller's stable visitor callback receives (templateId, vector,
// fieldCount) with the SAME vector reused across records. No per-record
// objects.

export class SbeDecoder {
  constructor(schema) {
    this.schema = schema;
    this.littleEndian = schema.littleEndian !== false;
    this.blockLength = 0;
    this.templateId = 0;
    this.schemaId = 0;
    this.version = 0;
    this.truncated = 0;
    this.skipped = 0; // unknown templates
    this.bytesProcessed = 0;
    this._vec = new Float64Array(32); // reused output vector
  }

  #u16(dv, off) { return this.littleEndian ? dv.getUint16(off, true) : dv.getUint16(off, false); }
  #u32(dv, off) { return this.littleEndian ? dv.getUint32(off, true) : dv.getUint32(off, false); }
  #u64(dv, off) {
    if (this.littleEndian) {
      const lo = dv.getUint32(off, true);
      const hi = dv.getUint32(off + 4, true);
      return hi * 4294967296 + lo; // exact < 2^53
    }
    const hi = dv.getUint32(off, false);
    const lo = dv.getUint32(off + 4, false);
    return hi * 4294967296 + lo;
  }
  #i64(dv, off) {
    const raw = this.#u64(dv, off);
    // two's complement reinterpretation without BigInt
    return raw >= 9223372036854775808 ? raw - 18446744073709551616 : raw;
  }

  // Decode one record at `off` (after the transport length prefix).
  // Writes into the shared vector; returns the visitor arg triple via the
  // callback. Returns bytes consumed, or 0 on unknown template / short read.
  #decodeRecord(dv, off, visit) {
    if (off + 8 > dv.byteLength) { this.truncated++; return 0; }
    this.blockLength = this.#u16(dv, off);
    this.templateId = this.#u16(dv, off + 2);
    this.schemaId = this.#u16(dv, off + 4);
    this.version = this.#u16(dv, off + 6);
    const msg = this.schema.messages[this.templateId];
    if (!msg) { this.skipped++; return 0; }
    const fields = msg.fields;
    const vec = this._vec;
    const n = Math.min(fields.length, vec.length);
    for (let i = 0; i < n; i++) {
      const f = fields[i];
      const at = off + 8 + f.offset;
      switch (f.type) {
        case 'u8': vec[i] = dv.getUint8(at); break;
        case 'i8': vec[i] = dv.getInt8(at); break;
        case 'u16': vec[i] = this.#u16(dv, at); break;
        case 'i16': vec[i] = this.littleEndian ? dv.getInt16(at, true) : dv.getInt16(at, false); break;
        case 'u32': vec[i] = this.#u32(dv, at); break;
        case 'i32': vec[i] = this.littleEndian ? dv.getInt32(at, true) : dv.getInt32(at, false); break;
        case 'u64': case 'i64': vec[i] = f.type === 'u64' ? this.#u64(dv, at) : this.#i64(dv, at); break;
        case 'f32': vec[i] = this.littleEndian ? dv.getFloat32(at, true) : dv.getFloat32(at, false); break;
        case 'f64': vec[i] = this.littleEndian ? dv.getFloat64(at, true) : dv.getFloat64(at, false); break;
        default: vec[i] = 0;
      }
    }
    visit(this.templateId, vec, n);
    return 8 + this.blockLength;
  }

  // Walk the whole stream; visit(templateId, vector, fieldCount) per record.
  // Returns the number of records visited. `buffer` may be an ArrayBuffer,
  // a DataView, or a typed-array/Buffer view (byte offsets view-relative).
  process(buffer, byteLength, visit) {
    let dv;
    if (buffer instanceof DataView) {
      dv = buffer;
    } else if (buffer instanceof ArrayBuffer) {
      dv = new DataView(buffer, 0, byteLength);
    } else {
      dv = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
    }
    let off = 0;
    let visited = 0;
    while (off + 4 <= byteLength) {
      const len = this.#u32(dv, off);
      if (off + 4 + len > byteLength) { this.truncated++; break; }
      const consumed = this.#decodeRecord(dv, off + 4, visit);
      if (consumed > 0) {
        visited++;
        this.bytesProcessed += 4 + len;
      }
      off += 4 + len;
    }
    return visited;
  }
}
