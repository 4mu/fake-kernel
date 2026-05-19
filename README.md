## Project Still In The Works !!!

---

## x86-to-Win32 Syscall Translator & i386 Emulator

A project designed to run **Linux i386 ELF binaries** on **Windows** by combining a custom i386 instruction set emulator with a syscall translation layer. This project is architecturally inspired by [iSH](https://ish.app/), but targets the Win32 API as its primary backend (mainly for ease of development, with the intention to later port to POSIX/Bionic).

---

### Project Architecture

The project is designed with strict modularity to allow for easy swapping of the host backend (eg. moving from Win32 to a POSIX or Bionic layer in the future)

```text
emu/
├── cpu/
│   ├── cpu.h          # CPU state (GPRs, EIP, EFLAGS) and FDE loop definitions
│   ├── cpu.c          # Core Fetch Decode Execute logic
│   └── ops.c          # Instruction handlers & ModR/M decoding via function table
├── mem/
│   ├── mem.h          # Guest memory abstraction (read/write helpers)
│   └── mem.c          # Backing store and bounds checking
├── syscall/
│   ├── syscall.h      # Syscall dispatch signatures & backend interfaces
│   ├── syscall.c      # Linux INT 0x80 dispatcher (EAX translation)
│   └── backend_win32.c # Win32 implementation of Linux syscalls
├── loader/
│   ├── loader.h
│   └── loader.c       # ELF32 parser and initial stack (argc/argv) setup
└── main.c             # Entry point: initializes subsystems and starts emulation
```

---

### Features

*   **Custom i386 Emulator**: A clean room implementation of the i386 instruction set using an efficient function pointer table for opcode dispatching.
*   **ModR/M Decoding**: A robust decoding engine capable of handling complex x86 addressing modes, including displacements and (planned) SIB bytes.
*   **Linux Syscall Translation**: Bridges the gap between the Linux ABI and Windows by intercepting `INT 0x80` and mapping registers (EAX, EBX, etc.) to Win32 API calls.
*   **Modular Backend**: The syscall layer is abstracted so that the Win32 backend can be replaced with minimal friction.

---

### Implementation Notes

*   **CPU State**: The emulator maintains an `i386` struct representing the eight general-purpose registers, EIP, and EFLAGS.
*   **Instruction Dispatch**: Mirroring the style of my earlier i8080 emulator, the system uses a 256 entry jump table initialized at startup to handle opcodes.
*   **Memory Model**: Implements a flat 32 bit address space for the guest, allowing the emulator to safely read and write to guest allocated regions while preventing host memory corruption.

---

### Credits
Inspired by the architecture of the **i8080** and the system design of **iSH**.
