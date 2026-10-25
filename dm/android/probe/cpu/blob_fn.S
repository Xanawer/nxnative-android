// Two small position-independent functions used by P0.1.
// int f(int a, int b):
//   code1 returns a + b + 0x1234
//   code2 returns a + b + 0x5678
// No PC-relative references, no literal loads, no relocations: the raw bytes
// can be copied anywhere and executed. The same bytes are shipped as the flat
// binary libblob.so (built by run_cpu.sh with llvm-objcopy -O binary).
    .text

    .globl probe_code1_begin
    .globl probe_code1_end
    .balign 4
probe_code1_begin:
    add     w0, w0, w1
    add     w0, w0, #0x234
    add     w0, w0, #0x1, lsl #12   // +0x1234 total
    ret
probe_code1_end:

    .globl probe_code2_begin
    .globl probe_code2_end
    .balign 4
probe_code2_begin:
    add     w0, w0, w1
    add     w0, w0, #0x678
    add     w0, w0, #0x5, lsl #12   // +0x5678 total
    ret
probe_code2_end:
