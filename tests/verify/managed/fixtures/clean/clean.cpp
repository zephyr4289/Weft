#include <cstddef>

// clean.cpp — clean C++ fixture: ZERO findings expected.

__attribute__((weft_hot))
void apply_tick(const Order &o, Stats *stats) {
    double acc = 0.0;
    for (int i = 0; i < o.count; i++) {
        acc += o.legs[i].px;
    }
    stats->sum = acc;
    stats->count = o.count;
}
