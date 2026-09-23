# sbe.py — schema-driven SBE flyweight decoder (managed Python side).
#
# Mirrors packages/fintech/src/sbe.js: [u32 LE length][SBE message] framing,
# 8-byte message header, blockLength-gated fixed fields, extension bytes
# skipped by transport length, unknown templates counted and skipped.
# Field values land in a preallocated shared list (reused across records).

import struct

_U32 = struct.Struct('<I')
_U16 = struct.Struct('<H')

_READERS = {}


def _reader(type_name, little):
    e = '<' if little else '>'
    fn = {
        'u8': lambda mv, off: mv[off],
        'i8': lambda mv, off: struct.unpack_from(e + 'b', mv, off)[0],
        'u16': lambda mv, off: struct.unpack_from(e + 'H', mv, off)[0],
        'i16': lambda mv, off: struct.unpack_from(e + 'h', mv, off)[0],
        'u32': lambda mv, off: struct.unpack_from(e + 'I', mv, off)[0],
        'i32': lambda mv, off: struct.unpack_from(e + 'i', mv, off)[0],
        'u64': lambda mv, off: struct.unpack_from(e + 'Q', mv, off)[0],
        'i64': lambda mv, off: struct.unpack_from(e + 'q', mv, off)[0],
        'f32': lambda mv, off: struct.unpack_from(e + 'f', mv, off)[0],
        'f64': lambda mv, off: struct.unpack_from(e + 'd', mv, off)[0],
    }[type_name]
    return fn


class SbeDecoder:
    __slots__ = ('schema', 'little', 'block_length', 'template_id',
                 'schema_id', 'version', 'truncated', 'skipped',
                 'bytes_processed', '_vec', '_templates')

    def __init__(self, schema):
        self.schema = schema
        self.little = schema.get('littleEndian', True)
        self.block_length = 0
        self.template_id = 0
        self.schema_id = 0
        self.version = 0
        self.truncated = 0
        self.skipped = 0
        self.bytes_processed = 0
        self._vec = [0] * 32
        self._templates = {}
        for tid, msg in schema.get('messages', {}).items():
            fields = msg['fields']
            self._templates[int(tid)] = (
                msg['blockLength'],
                [(f['offset'], _reader(f['type'], self.little)) for f in fields],
            )

    def process(self, buffer, byte_length, visit):
        mv = memoryview(buffer)
        off = 0
        visited = 0
        vec = self._vec
        while off + 4 <= byte_length:
            (length,) = _U32.unpack_from(mv, off)
            if off + 4 + length > byte_length:
                self.truncated += 1
                break
            rec = off + 4
            if rec + 8 <= len(mv):
                self.block_length = _U16.unpack_from(mv, rec)[0]
                self.template_id = _U16.unpack_from(mv, rec + 2)[0]
                self.schema_id = _U16.unpack_from(mv, rec + 4)[0]
                self.version = _U16.unpack_from(mv, rec + 6)[0]
                tpl = self._templates.get(self.template_id)
                if tpl is None:
                    self.skipped += 1
                else:
                    block_length, fields = tpl
                    n = min(len(fields), len(vec))
                    for i in range(n):
                        foff, fn = fields[i]
                        vec[i] = fn(mv, rec + 8 + foff)
                    visit(self.template_id, vec, n)
                    visited += 1
                    self.bytes_processed += 4 + length
            off += 4 + length
        return visited
