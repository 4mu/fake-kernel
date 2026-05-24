# Linux i386 ELF Userspace ABI Notes for Emulator + ELF→Win32 Translation Layer

## Overview

This document explains the Linux i386 userspace ABI expectations required to run GCC-compiled ELF binaries, particularly those linked against glibc or musl, inside a custom emulator and syscall translation layer.

The primary goal is to help debug failures that occur before `main()` executes, especially:

* startup halts
* invalid instructions
* segmentation faults
* hangs during libc initialization
* mysterious crashes inside `_start`
* failures in TLS or dynamic linker setup

This README is intended both as:

* implementation documentation
* debugging reference
* AI context for vibecoding assistance

---

# High-Level Execution Flow

A Linux ELF userspace process does **not** begin execution at `main()`.

The actual execution path is:

```text
kernel ELF loader
    ->
initial userspace stack setup
    ->
ELF entrypoint (_start)
    ->
__libc_start_main
    ->
libc internal initialization
    ->
constructors/init_array
    ->
main(argc, argv, envp)
```

For dynamically linked binaries:

```text
kernel
    ->
dynamic linker (/lib/ld-linux.so.2)
    ->
relocations
    ->
program _start
    ->
__libc_start_main
    ->
main
```

If any assumptions made by libc are violated, execution usually fails before `main()`.

---

# Critical Reality About glibc

glibc is tightly coupled to Linux kernel behavior.

It assumes:

* exact Linux syscall semantics
* correct ELF auxiliary vectors
* correct TLS setup
* Linux signal frame layouts
* valid vdso behavior
* precise memory mapping semantics
* Linux-compatible threading assumptions

glibc is not portable in the same sense as ordinary applications.

musl libc is significantly easier to emulate.

---

# Recommended Bring-Up Strategy

Do NOT begin with dynamic glibc binaries.

Recommended progression:

```text
1. Static musl binary
2. Static glibc binary
3. Dynamic musl binary
4. Dynamic glibc binary
```

This isolates:

* syscall layer problems
* TLS problems
* dynamic linker problems
* relocation problems

---

# Initial Stack Layout

At process entry:

```text
EIP = ELF entrypoint (_start)
ESP = initial userspace stack
```

The stack MUST be laid out exactly like Linux expects.

## Required Stack Layout

```text
ESP ->
    argc

    argv[0]
    argv[1]
    ...
    NULL

    envp[0]
    envp[1]
    ...
    NULL

    auxv[0].a_type
    auxv[0].a_val

    auxv[1].a_type
    auxv[1].a_val
    ...

    AT_NULL
    0
```

After the auxv table, Linux typically places:

* argument strings
* environment strings
* random bytes
* platform strings

glibc expects these memory regions to exist and remain valid.

---

# Stack Alignment Requirements

Modern i386 userspace often assumes:

```text
ESP % 16 == 0
```

before function calls.

If alignment is wrong:

* SSE instructions may fault
* `movaps` may crash
* glibc startup may fail mysteriously

This is a very common emulator bug.

---

# Auxiliary Vector (auxv)

The auxiliary vector is extremely important.

glibc parses auxv very early during startup.

Missing or invalid entries frequently cause:

* immediate aborts
* SIGSEGV
* TLS failures
* vdso failures

---

# Minimal Required auxv Entries

## Essential

| Entry             | Purpose                |
| ----------------- | ---------------------- |
| `AT_PAGESZ`       | Page size              |
| `AT_PHDR`         | Program header address |
| `AT_PHENT`        | Size of phdr entry     |
| `AT_PHNUM`        | Number of phdr entries |
| `AT_ENTRY`        | ELF entrypoint         |
| `AT_BASE`         | Dynamic linker base    |
| `AT_RANDOM`       | 16 random bytes        |
| `AT_SYSINFO_EHDR` | vdso base              |
| `AT_HWCAP`        | CPU capabilities       |
| `AT_CLKTCK`       | clock tick constant    |

## Identity-related

| Entry       | Purpose               |
| ----------- | --------------------- |
| `AT_UID`    | UID                   |
| `AT_EUID`   | Effective UID         |
| `AT_GID`    | GID                   |
| `AT_EGID`   | Effective GID         |
| `AT_SECURE` | Secure execution mode |

## Optional but Common

| Entry         | Purpose             |
| ------------- | ------------------- |
| `AT_EXECFN`   | executable path     |
| `AT_PLATFORM` | CPU platform string |
| `AT_HWCAP2`   | extended CPU caps   |

---

# Critical auxv Entries

The most important entries are:

```text
AT_RANDOM
AT_SYSINFO_EHDR
AT_PAGESZ
AT_PHDR
```

## AT_RANDOM

glibc uses this for:

* stack canaries
* security initialization
* entropy

Provide:

* pointer to 16 valid random bytes

If missing:

* newer glibc may abort immediately

---

# VDSO / vsyscall

glibc may attempt to use:

* `__kernel_vsyscall`
* vdso helper functions

through:

```text
AT_SYSINFO
AT_SYSINFO_EHDR
```

Without these:

* some versions fallback to `int 0x80`
* others may fail

---

# Recommended vdso Strategy

Simplest working solution:

```text
fake vdso page
    ->
export __kernel_vsyscall
    ->
forward internally to syscall dispatcher
```

This avoids many compatibility problems.

---

# Thread Local Storage (TLS)

TLS is one of the hardest parts of i386 userspace emulation.

glibc heavily depends on:

```text
set_thread_area
```

and:

```text
GS segment register
```

---

# i386 TLS Model

glibc expects:

```text
GS -> Thread Control Block (TCB)
```

Common accesses:

```asm
mov eax, gs:0x0
mov eax, gs:0x8
```

If `%gs` is invalid:

* startup immediately crashes

---

# Required TLS Features

Your emulator likely needs:

* GDT emulation
* segment register emulation
* `set_thread_area`
* TLS descriptors
* valid `%gs`
* thread pointer support

---

# Common TLS Failure Symptoms

## Immediate crash in libc

Usually:

```text
gs base invalid
```

## Random memory faults

Usually:

```text
TLS offsets wrong
```

## Crashes before main()

Usually:

```text
set_thread_area missing/broken
```

---

# Required Early Syscalls

Minimum viable syscall set:

| Syscall           | Notes             |
| ----------------- | ----------------- |
| `exit`            | required          |
| `exit_group`      | required          |
| `read`            | stdio             |
| `write`           | stdio             |
| `brk`             | malloc            |
| `mmap2`           | memory allocator  |
| `munmap`          | memory allocator  |
| `mprotect`        | ELF permissions   |
| `set_thread_area` | TLS               |
| `rt_sigaction`    | signal setup      |
| `rt_sigprocmask`  | signal setup      |
| `sigreturn`       | signal trampoline |
| `fstat64`         | libc startup      |
| `close`           | fd handling       |
| `open/openat`     | file access       |
| `access`          | startup checks    |
| `uname`           | libc queries      |
| `gettimeofday`    | time APIs         |

---

# Important i386 Notes

## arch_prctl

Not used on i386.

That is x86_64-only.

---

# Memory Mapping Expectations

glibc assumes Linux-compatible memory behavior.

Important requirements:

* page alignment correct
* mmap permissions accurate
* anonymous mappings work
* fixed mappings respected
* executable mappings supported
* `brk` grows correctly

---

# brk() Semantics

`brk()` behavior must closely match Linux.

glibc malloc depends on:

* predictable heap growth
* contiguous expansion
* Linux return conventions

Incorrect `brk()` behavior causes:

* allocator corruption
* startup crashes
* mysterious heap failures

---

# Dynamic Linker Requirements

For dynamically linked binaries:

```text
/lib/ld-linux.so.2
```

must execute before the program itself.

The loader performs:

* relocations
* GOT setup
* PLT setup
* symbol resolution
* TLS initialization

---

# Required ELF Features

Your loader likely needs:

## ELF Parsing

* program headers
* section headers
* PT_LOAD
* PT_DYNAMIC
* PT_INTERP

## Relocations

Common i386 relocations:

```text
R_386_RELATIVE
R_386_GLOB_DAT
R_386_JMP_SLOT
R_386_COPY
```

---

# GOT/PLT Mechanics

Dynamic binaries use:

* Global Offset Table (GOT)
* Procedure Linkage Table (PLT)

Incorrect PLT/GOT handling often appears as:

* jump to invalid addresses
* crashes during first function call
* faults inside libc

---

# Signal Handling

glibc installs signal handlers very early.

Your emulator eventually needs:

* Linux signal frame layout
* signal trampolines
* `sigreturn`
* correct stack restoration

---

# CPU State Assumptions

glibc assumes CPU state is initialized correctly.

---

# Direction Flag

DF should normally be clear.

If not:

* string operations break
* memcpy/memset fail
* startup corrupts memory

---

# SSE/x87 State

Userspace expects:

* x87 initialized
* SSE enabled
* MXCSR sane

---

# CPUID Consistency

glibc may compare:

* CPUID
* auxv HWCAP values

Inconsistent values can break optimized code paths.

---

# Common Emulator Bugs

---

## 1. Incorrect Initial Stack

Most common failure source.

Symptoms:

* crash in `_start`
* invalid argc/argv
* auxv parsing failures

---

## 2. Broken TLS

Very common.

Symptoms:

* crash before main()
* invalid `%gs`
* libc startup faults

---

## 3. Incorrect mmap Semantics

Symptoms:

* loader crashes
* allocator corruption
* relocation failures

---

## 4. Broken brk()

Symptoms:

* malloc crashes
* heap corruption

---

## 5. Missing vdso

Symptoms:

* syscall wrapper crashes
* glibc aborts

---

## 6. Stack Misalignment

Symptoms:

* `movaps` faults
* SSE crashes

---

## 7. Incorrect Signal Frames

Symptoms:

* crashes after signal handling
* return-to-user corruption

---

# Recommended Debugging Strategy

---

# Step 1: Use Static musl

Compile:

```bash
musl-gcc -static hello.c
```

Advantages:

* no dynamic linker
* simpler startup
* easier TLS
* fewer syscalls

---

# Step 2: Trace Every Syscall

Log:

* syscall number
* arguments
* return values
* errno

This is essential.

---

# Step 3: Instrument Startup

Trace:

* initial ESP
* argc
* argv
* auxv parsing
* `_start`
* `__libc_start_main`

---

# Step 4: Validate TLS

Verify:

* `set_thread_area`
* `%gs`
* TLS offsets

---

# Step 5: Compare Against Real Linux

Use:

* `strace`
* `gdb`
* QEMU
* rr
* ptrace

Compare:

* memory maps
* syscall sequences
* auxv
* stack layout

---

# Useful Linux Inspection Commands

## View auxv

```bash
cat /proc/self/auxv
```

## View memory maps

```bash
cat /proc/self/maps
```

## Trace syscalls

```bash
strace ./program
```

## Disassemble startup

```bash
objdump -d program
```

---

# Useful Breakpoints

When debugging libc startup:

```text
_start
__libc_start_main
_dl_start
_dl_runtime_resolve
```

---

# Extremely Common Reality

Most failures blamed on:

* ELF parsing
* relocations
* libc

are actually:

* TLS bugs
* bad stack setup
* incorrect auxv
* mmap semantic mismatches

---

# Strong Recommendation

If the project goal is:

```text
run Linux userspace binaries on Windows
```

then:

* musl is dramatically easier
* static binaries are dramatically easier
* glibc compatibility is a major undertaking

glibc effectively expects:

```text
Linux kernel behavior
```

not merely POSIX behavior.

---

# Practical Minimal Viable Environment

For initial success:

## Use:

* static musl
* single-threaded programs
* int 0x80 syscall path
* no signals initially
* no vdso initially

## Implement:

* correct stack
* correct auxv
* mmap/brk
* TLS
* basic file descriptors

Only after that:

* dynamic linking
* pthreads
* vdso
* signal delivery
* advanced glibc support

---

# Final Notes

If execution halts before `main()`, the highest-probability causes are:

1. broken TLS
2. invalid auxv
3. stack misalignment
4. broken mmap/brk semantics
5. missing vdso support
6. broken dynamic relocations
7. invalid `%gs`

These are substantially more likely than:

* compiler bugs
* libc bugs
* ELF parsing bugs

for emulator bring-up.
