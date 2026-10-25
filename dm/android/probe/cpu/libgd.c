// P0.2(b): a shared library whose ONLY TLS variable uses the default (global-dynamic) access model.
// dlopen'd at runtime; we check whether its address is a constant offset from tpidr_el0 (expected: no,
// because bionic resolves it through __tls_get_addr into a per-thread, dynamically allocated block).
#include <stdint.h>
static __thread uint64_t gd_var;
void *gd_tls_addr(void) { gd_var = 0; return &gd_var; }
