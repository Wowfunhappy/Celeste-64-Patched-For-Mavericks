# Celeste 64 v1.1.0 on macOS 10.9.5 (Mavericks)

Celeste 64 is a .NET 8 self-contained application built for macOS 10.15+. This documents the work done to make it run on macOS 10.9.5 (Mavericks) on a Mac Pro with an NVIDIA GeForce GTX TITAN Black (OpenGL 4.1).

## Overview

The game ships as a self-contained .NET 8 bundle with an embedded runtime, SDL2-based rendering library (`libFosterPlatform.dylib`), and FMOD audio. Getting it running on 10.9 required:

1. **Binary patching** `libFosterPlatform.dylib` (compiled with modern Xcode for 10.15+)
2. **Stub libraries** for missing system APIs
3. **A launcher script** that sets up the compatibility environment

## Binary Patches to libFosterPlatform.dylib

The original library used modern Mach-O features and ObjC conventions that don't exist on 10.9. All patches were applied with a custom Python script and hex editing.

### 1. Chained Fixups → LC_DYLD_INFO_ONLY

macOS 10.9's dyld doesn't understand `LC_DYLD_INFO_ONLY` with chained fixups (introduced in 10.15). The chained fixup data (format `DYLD_CHAINED_PTR_64_OFFSET`, format 6) was decoded and converted into traditional rebase and bind opcodes. The `__LINKEDIT` segment was extended to hold the appended opcode data.

### 2. Section Type Normalization

Modern section types that 10.9's dyld doesn't recognize were changed to `S_REGULAR`:
- `__got` (`S_NON_LAZY_SYMBOL_POINTERS` → `S_REGULAR`)
- `__stubs` (`S_SYMBOL_STUBS` → `S_REGULAR`)

### 3. Relative → Absolute ObjC Method Lists

The modern ObjC compiler emits *relative* method lists (flag `0x80000000`, 12-byte entries with relative offsets). macOS 10.9's ObjC runtime only understands *absolute* method lists (24-byte entries with 3 pointers: name, types, imp).

All 20 method lists were converted to absolute format and placed in a new `__MLDATA` segment. The method list entry size was changed from 12 to 24.

### 4. `__DATA_CONST` → `__DATA` Segment Rename

macOS 10.9's ObjC runtime only searches the `__DATA` segment for `__objc_classlist`, `__objc_catlist`, etc. The `__DATA_CONST` segment (introduced later) and all its sections were renamed to `__DATA`.

### 5. GCController Binary Patch

SDL2 calls `+[GCController supportsHIDDevice:]` (added macOS 10.15) during joystick enumeration. The `IOS_SupportedHIDDevice` function was patched at offset `0x161b35`: changed `je` (`0x74`) to `jmp` (`0xEB`) to unconditionally skip the GCController code path and always return false, falling back to HID-based joystick handling.

## Stub Libraries (compat/)

### libcompat.dylib (stub_system.c)

A dynamically loaded shim inserted via `DYLD_INSERT_LIBRARIES` providing implementations of functions added after macOS 10.9:

| Function | Added In | Implementation |
|---|---|---|
| `mmap` wrapper | 10.14 | Strips `MAP_JIT` flag, calls kernel `__mmap` |
| `pthread_jit_write_protect_np` | 11.0 | No-op (x86_64 only) |
| `objc_alloc_init` | 10.14.4 | `[[cls alloc] init]` via `objc_msgSend` |
| `objc_alloc` | 10.14 | `[cls alloc]` via `objc_msgSend` |
| `objc_opt_class` | 11.0 | `[obj class]` via `objc_msgSend` |
| `objc_opt_isKindOfClass` | 11.0 | `[obj isKindOfClass:]` via `objc_msgSend` |
| `objc_opt_respondsToSelector` | 11.0 | `[obj respondsToSelector:]` via `objc_msgSend` |
| `objc_unsafeClaimAutoreleasedReturnValue` | 10.11 | Delegates to `objc_retainAutoreleasedReturnValue` |
| `____chkstk_darwin` | 10.12 | x86_64 assembly stack probe (page-by-page) |
| `thread_get_register_pointer_values` | 10.11 | Reads all GP registers via `thread_get_state` for .NET GC |
| `syslog$DARWIN_EXTSN` | — | Forwards to `vsyslog` |
| `CCRandomGenerateBytes` | 10.10 | Reads from `/dev/urandom` |
| `__isOSVersionAtLeast` | compiler-rt | Reports actual OS version for `@available()` checks |
| `__isPlatformVersionAtLeast` | compiler-rt | Same, newer Clang variant |
| `clonefile` | 10.12 | Returns `ENOTSUP` (triggers .NET fallback to traditional copy) |
| `SecKeyCopy*`, `SecKeyCreate*`, etc. | 10.12+ | Return NULL (stub Security framework) |
| `SSLCopyALPNProtocols`, `SSLSetALPNProtocols` | 10.13.4 | Return "unimplemented" status |
| `SecCertificateCopyKey` | 10.14 | Returns NULL |
| `kIOMainPortDefault` | 12.0 | Set to `MACH_PORT_NULL` (same as `kIOMasterPortDefault`) |
| 18 `kSecKeyAlgorithm*` constants | 10.12 | Defined with correct CFSTR values |

### libc.dylib (compat/)

A thin shim that re-exports the real `/usr/lib/libc.dylib` and adds `clonefile`. This is needed because .NET's P/Invoke looks up `clonefile` specifically in the `libc` library handle via `dlsym`, bypassing flat namespace resolution.

### Other Stubs

- **CryptoKit.framework** — Stub framework (CryptoKit was added in 10.15)
- **swift/libswiftCore.dylib** — Stub Swift runtime
- **swift/libswiftFoundation.dylib** — Stub Swift Foundation

## Launcher Script (run_celeste64.command)

```bash
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"

export DYLD_FRAMEWORK_PATH="$DIR/compat:${DYLD_FRAMEWORK_PATH}"
export DYLD_LIBRARY_PATH="$DIR/compat:$DIR/compat/swift:${DYLD_LIBRARY_PATH}"
export DYLD_FORCE_FLAT_NAMESPACE=1
export DYLD_INSERT_LIBRARIES="/usr/local/lib/libMacportsLegacySupport.dylib:$DIR/compat/libcompat.dylib"
export DOTNET_EnableWriteXorExecute=0
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

exec "$DIR/Celeste64" "$@"
```

Key environment variables:
- **DYLD_FRAMEWORK_PATH** — Finds stub CryptoKit framework
- **DYLD_LIBRARY_PATH** — Finds stub Swift libs and `libc.dylib` shim (with `clonefile`)
- **DYLD_FORCE_FLAT_NAMESPACE** — Allows libcompat symbols to override missing system symbols
- **DYLD_INSERT_LIBRARIES** — Loads MacPorts Legacy Support (provides `clock_gettime`, `fstatat`, `os_unfair_lock`, etc.) and libcompat
- **DOTNET_EnableWriteXorExecute=0** — Disables W^X double mapping (not supported pre-10.14)
- **DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1** — Skips ICU (system ICU is too old on 10.9)

## External Dependency

- **MacPorts Legacy Support** (`/usr/local/lib/libMacportsLegacySupport.dylib`) — Provides POSIX functions added after 10.9: `clock_gettime`, `clock_gettime_nsec_np`, `fstatat`, `futimens`, `utimensat`, `os_unfair_lock_lock/unlock/trylock`.

## Key Lessons Learned

- `segment_command_64` is 72 bytes, not 80 — getting this wrong corrupts all subsequent section headers
- `DYLD_CHAINED_PTR_64_OFFSET` (format 6): next pointer at bits 51–62 with stride 4, target at bits 0–35
- `__LINKEDIT` must be the last segment and its filesize must cover all linker data
- Method list rebase entries must target writable segments
- macOS 10.9's ObjC runtime doesn't understand relative method lists (flag `0x80000000`)
- macOS 10.9's ObjC runtime only looks in the `__DATA` segment for `__objc_classlist`
- Method list entsize must be 24 for absolute format (3 × 8-byte pointers), not 12 (3 × 4-byte relative offsets)
- `@available()` compiles to `__isOSVersionAtLeast` / `__isPlatformVersionAtLeast`, which don't exist in 10.9's compiler-rt — without stubs, any `@available` check crashes
- .NET P/Invoke uses `dlsym(handle, ...)` on the specific library handle, not `RTLD_DEFAULT` — flat namespace doesn't help; you need a shim library with the correct name
- `kern.osproductversion` sysctl doesn't exist on 10.9; use `kern.osrelease` or `sw_vers` as fallback
