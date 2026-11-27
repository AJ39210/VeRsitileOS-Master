# Kernel write protection

The kernel enables x86 CR0.WP during early startup. This makes supervisor-mode writes respect read-only page-table entries, including the kernel text/rodata permissions from the linker and section permissions applied to dynamically loaded kernel modules.

A supervisor-mode write-protection page fault stops the system with the existing panic background and a **Security Panic** screen. It reports the fault address, instruction pointer, CPU error code, a bounded stack trace, and recovery tips. This indicates a blocked write, not proof of malicious activity; kernel or driver bugs can cause the same fault.

This is a defense-in-depth measure, not complete isolation from hostile ring-0 code. Kernel code can still modify writable kernel data; writable aliases such as a direct physical-memory map, if available, can also bypass read-only virtual mappings. Code able to change page tables or CR0 can bypass these checks. Drivers execute with kernel privilege and are not sandboxed from one another.

## Signed kernel modules

The module loader requires an Ed25519 signature from the pinned public key in [`security/cert/module_signing_public_key.bin`](../../Kernel/security/cert/module_signing_public_key.bin); this is a raw trust anchor, not an X.509 certificate chain. The signed file is the original relocatable ELF followed by a 72-byte trailer: the 8-byte `VOSMOD01` marker and a 64-byte signature. The Ed25519 message is the domain string `VeRsitileOS kernel module signature v1`, a NUL byte, the payload length as an unsigned little-endian 64-bit integer, and the SHA-256 digest of the ELF payload. Verification happens before ELF sections are allocated or made executable. Unsigned or modified modules are rejected.

`make -C Kernel module-signing-key` (requires the host OpenSSL CLI) generates a local Ed25519 private key at `~/.config/versitileos/module-signing-key.pem`, outside the source tree, and replaces the public trust key. Keep the private key private; rebuild and distribute the kernel containing the matching public key. `make -C Kernel modules` signs each built module with that key. Set `MODULE_SIGNING_KEY=/path/to/key.pem` to point the build at the matching private key. The checked-in public key is only a development trust anchor; production releases should use a maintainer-controlled key. The no-std verifier uses the locked `ed25519-dalek` and `sha2` crates and forces Curve25519's scalar backend so it does not rely on SIMD state.

This is some old feature i added but i didnt realesed it. so yh
