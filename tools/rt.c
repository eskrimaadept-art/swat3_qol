// Minimal compiler runtime for i686 Windows builds made with plain clang (no libgcc/compiler-rt):
// 64-bit division helpers and the stack probe clang calls for large stack frames.
typedef unsigned long long u64; typedef long long s64;
static u64 udivmod(u64 n, u64 d, u64* rem) {
    if (d == 0) { if (rem) *rem = 0; return 0; }
    if ((n >> 32) == 0 && (d >> 32) == 0) {          // the usual case: one hardware division
        unsigned a = (unsigned)n, b = (unsigned)d;
        if (rem) *rem = a % b;
        return a / b;
    }
    if ((d >> 32) == 0 && (n >> 32) < d) {           // 64 / 32 with a 32-bit result: the CPU's divide
        unsigned lo = (unsigned)n, hi = (unsigned)(n >> 32), q, r;
        __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"((unsigned)d));
        if (rem) *rem = r;
        return q;
    }
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
u64 __udivdi3(u64 a, u64 b) { return udivmod(a, b, 0); }
u64 __umoddi3(u64 a, u64 b) { u64 r; udivmod(a, b, &r); return r; }
s64 __divdi3(s64 a, s64 b) { int neg = (a < 0) ^ (b < 0); u64 q = udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, 0); return neg ? -(s64)q : (s64)q; }
s64 __moddi3(s64 a, s64 b) { u64 r; udivmod(a < 0 ? -(u64)a : (u64)a, b < 0 ? -(u64)b : (u64)b, &r); return a < 0 ? -(s64)r : (s64)r; }
// ___chkstk_ms: probe each 4 KiB page of the new stack area (size in eax); preserves all registers.
__asm__(
    ".text\n.globl ___chkstk_ms\n___chkstk_ms:\n"
    "  pushl %ecx\n  pushl %eax\n  cmpl $0x1000, %eax\n  leal 12(%esp), %ecx\n  jb 2f\n"
    "1:\n  subl $0x1000, %ecx\n  orl $0, (%ecx)\n  subl $0x1000, %eax\n  cmpl $0x1000, %eax\n  ja 1b\n"
    "2:\n  subl %eax, %ecx\n  orl $0, (%ecx)\n  popl %eax\n  popl %ecx\n  ret\n"
    ".globl __alloca\n__alloca:\n  jmp ___chkstk_ms\n");
