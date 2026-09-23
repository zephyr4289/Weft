#!/usr/bin/env python3
# gen_model.py — generate the AC-O fixture: a minimal ONNX Identity graph.
#
# WHY HAND-ENCODED: the fixture must ship in-tree with ZERO download or
# build dependencies (the no-network CI leg discipline). The model is a
# single Identity node over float32[1,64] — the smallest graph that
# exercises the full zero-copy round trip: wrap input view -> RunWithBinding
# -> BindOutput span -> assert output bytes == input bytes, all in WEFT
# memory. Protobuf wire format is hand-rolled below (varint / tag /
# length-delimited only — ~40 lines, no protobuf dependency).
#
# Verification: onnx.checker (when the `onnx` wheel is available) parses
# and validates the bytes; otherwise the byte layout is reviewed against
# the ONNX protobuf schema and the ORT real-lib leg (where runtimes
# exist) is the runtime gate.

import struct
import sys
import os


def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            out += bytes([b])
            return out


def tag(field, wire):
    return varint((field << 3) | wire)


def vint(field, value):
    return tag(field, 0) + varint(value)


def ld(field, payload):
    return tag(field, 2) + varint(len(payload)) + payload


def s(field, value):
    return ld(field, value.encode())


# --- TypeProto.Tensor(shape=[1,64], elem_type=FLOAT) --------------------
def tensor_shape(dims):
    body = b""
    for d in dims:
        body += ld(1, vint(1, d))          # Dimension.dim_value
    return body                              # TensorShapeProto


def tensor_type(elem_type, dims):
    return vint(1, elem_type) + ld(2, tensor_shape(dims))  # TypeProto.Tensor


def type_proto(elem_type, dims):
    return ld(1, tensor_type(elem_type, dims))             # TypeProto


def value_info(name, elem_type, dims):
    return s(1, name) + ld(2, type_proto(elem_type, dims))  # ValueInfoProto


FLOAT = 1

# --- NodeProto: Identity(X) -> Y ----------------------------------------
node = (s(1, "X") +            # input
        s(2, "Y") +            # output
        s(3, "identity_node") +
        s(4, "Identity"))      # op_type

# --- GraphProto ----------------------------------------------------------
graph = (ld(1, node) +                                    # node
         s(2, "weft_fixture_graph") +                     # name
         ld(11, value_info("X", FLOAT, [1, 64])) +        # input
         ld(12, value_info("Y", FLOAT, [1, 64])))         # output

# --- OperatorSetIdProto: domain "", version 13 ---------------------------
opset = s(1, "") + vint(2, 13)

# --- ModelProto -----------------------------------------------------------
model = (vint(1, 8) +            # ir_version 8 (onnx 1.16 era)
         s(2, "weft-tensor") +   # producer_name
         ld(7, graph) +          # graph
         ld(8, opset))           # opset_import


def main():
    out_path = os.path.join(os.path.dirname(__file__),
                            "weft_fixture_identity.onnx")
    with open(out_path, "wb") as f:
        f.write(model)
    print("wrote %s (%d bytes)" % (out_path, len(model)))

    # Verify with the real onnx library when available (evidence-grade).
    try:
        import onnx  # type: ignore
        m = onnx.load_model_from_string(model)
        onnx.checker.check_model(m)
        g = m.graph
        assert len(g.node) == 1 and g.node[0].op_type == "Identity"
        assert len(g.input) == 1 and g.input[0].name == "X"
        assert len(g.output) == 1 and g.output[0].name == "Y"
        dims = [d.dim_value for d in
                g.input[0].type.tensor_type.shape.dim]
        assert dims == [1, 64], dims
        print("onnx.checker: VALID (Identity, X f32[1,64] -> Y)")
    except ImportError:
        print("onnx wheel absent — byte-layout review + real-lib leg only "
              "(the honest split, Law 4)")
        sys.exit(0)


if __name__ == "__main__":
    main()
