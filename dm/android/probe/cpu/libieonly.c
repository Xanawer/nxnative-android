// P0.2(b): a shared library whose ONLY TLS variable uses the initial-exec access model.
// We dlopen this at runtime to see whether bionic will load a dlopen'd .so that references an
// initial-exec __thread symbol (the port would need this for candidate (a) in the APK).
#include <stdint.h>
__attribute__((tls_model("initial-exec"))) static __thread uint64_t ie_var;
void *ie_tls_addr(void) { ie_var = 0; return &ie_var; }
