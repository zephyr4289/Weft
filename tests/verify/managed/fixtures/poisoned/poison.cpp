#include <memory>

// poison.cpp — poisoned C++ fixture. Every commented rule id MUST fire.

__attribute__((weft_hot))
void apply_tick(const Order &o) {
    auto *sink = new Sink(o);                    // expect: WV-CPP-001
    auto sp = std::make_shared<Legs>(o.legs);    // expect: WV-CPP-002
    auto up = std::make_unique<Tick>();          // expect: WV-CPP-003
    (void)sink; (void)sp; (void)up;
}
