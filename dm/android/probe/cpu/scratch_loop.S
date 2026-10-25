// P0.3 hot loop, hand-written so sp stays fixed and the only writes to [sp-112, sp)
// come from signal frames (there is no red zone on Linux). Mirrors the rewriter's
// scratch saves below sp (patch_nso.py: -16/-24, or -112 when the instruction moves sp).
//
// void probe_sp_loop(volatile int *stop, uint64_t *corrupt, uint64_t *iter, volatile uint64_t *tracked);
//   x0=stop x1=corrupt-count x2=iteration-count x3=tracked page (a store that may fault)
// Each iteration: write 14 distinct sentinels into [sp-8 .. sp-112], store to the tracked
// page (may fault; the SIGSEGV handler unprotects and the store retries), spin, then check
// all 14 sentinels and add the number of mismatches to *corrupt.
    .text
    .globl probe_sp_loop
    .balign 16
probe_sp_loop:
    movz    x5, #0x5CA7, lsl #48        // sentinel base
.Lloop:
    ldr     w9, [x0]
    cbnz    w9, .Ldone

    add     x13, x5, #1  ; stur x13, [sp, #-8]
    add     x13, x5, #2  ; stur x13, [sp, #-16]
    add     x13, x5, #3  ; stur x13, [sp, #-24]
    add     x13, x5, #4  ; stur x13, [sp, #-32]
    add     x13, x5, #5  ; stur x13, [sp, #-40]
    add     x13, x5, #6  ; stur x13, [sp, #-48]
    add     x13, x5, #7  ; stur x13, [sp, #-56]
    add     x13, x5, #8  ; stur x13, [sp, #-64]
    add     x13, x5, #9  ; stur x13, [sp, #-72]
    add     x13, x5, #10 ; stur x13, [sp, #-80]
    add     x13, x5, #11 ; stur x13, [sp, #-88]
    add     x13, x5, #12 ; stur x13, [sp, #-96]
    add     x13, x5, #13 ; stur x13, [sp, #-104]
    add     x13, x5, #14 ; stur x13, [sp, #-112]

    str     xzr, [x3]                   // write the tracked page (dirty-tracker fault pattern)

    mov     x11, #512                   // spin: a window for a signal to arrive
.Lspin:
    subs    x11, x11, #1
    bne     .Lspin

    mov     x12, #0                     // mismatch counter for this iteration
    add     x13, x5, #1  ; ldur x15, [sp, #-8]   ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #2  ; ldur x15, [sp, #-16]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #3  ; ldur x15, [sp, #-24]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #4  ; ldur x15, [sp, #-32]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #5  ; ldur x15, [sp, #-40]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #6  ; ldur x15, [sp, #-48]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #7  ; ldur x15, [sp, #-56]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #8  ; ldur x15, [sp, #-64]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #9  ; ldur x15, [sp, #-72]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #10 ; ldur x15, [sp, #-80]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #11 ; ldur x15, [sp, #-88]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #12 ; ldur x15, [sp, #-96]  ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #13 ; ldur x15, [sp, #-104] ; cmp x15, x13 ; cinc x12, x12, ne
    add     x13, x5, #14 ; ldur x15, [sp, #-112] ; cmp x15, x13 ; cinc x12, x12, ne

    ldr     x17, [x1] ; add x17, x17, x12 ; str x17, [x1]    // *corrupt += mismatches
    ldr     x16, [x2] ; add x16, x16, #1  ; str x16, [x2]    // (*iter)++
    b       .Lloop
.Ldone:
    ret
