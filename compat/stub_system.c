/*
 * System compatibility shim for macOS 10.9.
 * Provides stub implementations of functions added in macOS 10.10+
 * that the .NET 8 runtime references. MacPorts Legacy Support handles
 * the POSIX functions (clock_gettime, fstatat, etc.).
 * This shim covers Security framework, CommonCrypto, and other gaps.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <pthread.h>
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <Security/Security.h>
#include <sys/sysctl.h>

/* ============================================================
 * @available() support - __isOSVersionAtLeast / __isPlatformVersionAtLeast
 * These compiler-rt builtins don't exist on 10.9. Without them,
 * @available(macOS 10.15, *) calls would crash. We implement them
 * to report the actual OS version so SDL2 correctly skips code
 * paths for CoreHaptics, newer GCController APIs, etc.
 * ============================================================ */
static int cached_major = 0, cached_minor = 0, cached_patch = 0;

static void ensure_os_version(void) {
    if (cached_major) return;
    char str[64];
    size_t len = sizeof(str);
    if (sysctlbyname("kern.osproductversion", str, &len, NULL, 0) == 0) {
        sscanf(str, "%d.%d.%d", &cached_major, &cached_minor, &cached_patch);
    }
    if (!cached_major) {
        /* Fallback: we know we're on 10.9 */
        cached_major = 10; cached_minor = 9; cached_patch = 5;
    }
}

int32_t __isOSVersionAtLeast(int32_t major, int32_t minor, int32_t patch) {
    ensure_os_version();
    if (cached_major != major) return cached_major > major;
    if (cached_minor != minor) return cached_minor > minor;
    return cached_patch >= patch;
}

/* Newer Clang uses this variant (platform 1 = macOS) */
int32_t __isPlatformVersionAtLeast(uint32_t platform, uint32_t major,
                                    uint32_t minor, uint32_t patch) {
    (void)platform; /* Assume macOS */
    return __isOSVersionAtLeast((int32_t)major, (int32_t)minor, (int32_t)patch);
}

/* ============================================================
 * clonefile (added 10.12) - used by .NET's File.Copy
 * Falls back to regular copy semantics by returning ENOTSUP,
 * which tells .NET to use the traditional copy path.
 * ============================================================ */
#include <errno.h>
int clonefile(const char *src, const char *dst, int flags) {
    (void)src; (void)dst; (void)flags;
    errno = ENOTSUP;
    return -1;
}

/* ============================================================
 * mmap wrapper - strip MAP_JIT flag (0x0800, added 10.14)
 * The .NET runtime uses MAP_JIT for JIT code pages.
 * On 10.9 this flag doesn't exist and causes mmap to fail.
 * We strip it and call the real mmap.
 * ============================================================ */
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

/* Call the kernel's raw __mmap (from libsystem_kernel) which bypasses
 * our override and preserves the 64-bit return value. */
extern void *__mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    /* Strip MAP_JIT flag - not supported on 10.9 */
    flags &= ~MAP_JIT;
    return __mmap(addr, len, prot, flags, fd, offset);
}

/* ============================================================
 * pthread_jit_write_protect_np (added macOS 11.0)
 * On x86_64, this is a no-op. On ARM it toggles W^X.
 * ============================================================ */
void pthread_jit_write_protect_np(int enabled) {
    (void)enabled;
    /* No-op on x86_64 */
}

/* ============================================================
 * ObjC runtime - objc_alloc_init (added ~10.14.4 / Xcode 10.2)
 * Combines [cls alloc] and [obj init] in one call.
 * ============================================================ */
id objc_alloc_init(Class cls) {
    id obj = ((id(*)(Class, SEL))objc_msgSend)(cls, sel_getUid("alloc"));
    return ((id(*)(id, SEL))objc_msgSend)(obj, sel_getUid("init"));
}

/* ============================================================
 * ObjC runtime - objc_alloc (added ~10.14)
 * Optimized version of [cls alloc].
 * ============================================================ */
id objc_alloc(Class cls) {
    return ((id(*)(Class, SEL))objc_msgSend)(cls, sel_getUid("alloc"));
}

/* ============================================================
 * ObjC runtime - objc_opt_class (added macOS 11)
 * Optimized version of [obj class].
 * ============================================================ */
Class objc_opt_class(id obj) {
    if (!obj) return Nil;
    return ((Class(*)(id, SEL))objc_msgSend)(obj, sel_getUid("class"));
}

/* ============================================================
 * ObjC runtime - objc_opt_isKindOfClass (added macOS 11)
 * Optimized version of [obj isKindOfClass:cls].
 * ============================================================ */
BOOL objc_opt_isKindOfClass(id obj, Class cls) {
    if (!obj) return NO;
    return ((BOOL(*)(id, SEL, Class))objc_msgSend)(obj, sel_getUid("isKindOfClass:"), cls);
}

/* ============================================================
 * ObjC runtime - objc_opt_respondsToSelector (added macOS 11)
 * Optimized version of [obj respondsToSelector:sel].
 * ============================================================ */
BOOL objc_opt_respondsToSelector(id obj, SEL sel) {
    if (!obj) return NO;
    return ((BOOL(*)(id, SEL, SEL))objc_msgSend)(obj, sel_getUid("respondsToSelector:"), sel);
}

/* ============================================================
 * ObjC runtime - objc_unsafeClaimAutoreleasedReturnValue (added 10.11)
 * Optimization that claims an autoreleased return value.
 * Equivalent to objc_retainAutoreleasedReturnValue on older systems.
 * ============================================================ */
extern id objc_retainAutoreleasedReturnValue(id obj);
id objc_unsafeClaimAutoreleasedReturnValue(id obj) {
    return objc_retainAutoreleasedReturnValue(obj);
}

/* ============================================================
 * ___chkstk_darwin - stack probe function (added ~10.12)
 * Called by compiler-generated code for large stack frames.
 * RAX contains the number of bytes to probe. This function
 * must touch each page to trigger guard page faults.
 * ============================================================ */
__asm__(
    ".globl ____chkstk_darwin\n"
    "____chkstk_darwin:\n"
    "  pushq  %rcx\n"
    "  pushq  %rax\n"
    "  cmpq   $0x1000, %rax\n"
    "  leaq   24(%rsp), %rcx\n"   /* rcx = original rsp */
    "  jb     .Ldone\n"
    ".Lloop:\n"
    "  subq   $0x1000, %rcx\n"
    "  testq  %rcx, (%rcx)\n"     /* probe the page */
    "  subq   $0x1000, %rax\n"
    "  cmpq   $0x1000, %rax\n"
    "  ja     .Lloop\n"
    ".Ldone:\n"
    "  subq   %rax, %rcx\n"
    "  testq  %rcx, (%rcx)\n"     /* probe last partial page */
    "  popq   %rax\n"
    "  popq   %rcx\n"
    "  retq\n"
);

/* ============================================================
 * thread_get_register_pointer_values (added ~10.11)
 * Used by .NET GC to scan thread registers for managed pointers.
 * ============================================================ */
kern_return_t thread_get_register_pointer_values(
    thread_t thread, uintptr_t *sp, size_t *count,
    uintptr_t *register_values)
{
    x86_thread_state64_t state;
    mach_msg_type_number_t state_count = x86_THREAD_STATE64_COUNT;
    kern_return_t kr = thread_get_state(thread, x86_THREAD_STATE64,
                                        (thread_state_t)&state, &state_count);
    if (kr != KERN_SUCCESS) return kr;

    if (sp) *sp = state.__rsp;

    /* Return all general-purpose registers that could hold pointers */
    if (register_values && count) {
        size_t i = 0;
        register_values[i++] = state.__rax;
        register_values[i++] = state.__rbx;
        register_values[i++] = state.__rcx;
        register_values[i++] = state.__rdx;
        register_values[i++] = state.__rdi;
        register_values[i++] = state.__rsi;
        register_values[i++] = state.__rbp;
        register_values[i++] = state.__r8;
        register_values[i++] = state.__r9;
        register_values[i++] = state.__r10;
        register_values[i++] = state.__r11;
        register_values[i++] = state.__r12;
        register_values[i++] = state.__r13;
        register_values[i++] = state.__r14;
        register_values[i++] = state.__r15;
        register_values[i++] = state.__rip;
        *count = i;
    }
    return KERN_SUCCESS;
}

/* ============================================================
 * syslog$DARWIN_EXTSN - newer ABI variant
 * ============================================================ */
void syslog_darwin_extsn(int priority, const char *message, ...)
    __asm("_syslog$DARWIN_EXTSN");
void syslog_darwin_extsn(int priority, const char *message, ...) {
    va_list ap;
    va_start(ap, message);
    vsyslog(priority, message, ap);
    va_end(ap);
}

/* ============================================================
 * CommonCrypto - CCRandomGenerateBytes (added 10.10)
 * ============================================================ */
int CCRandomGenerateBytes(void *bytes, size_t count) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, bytes, count);
    close(fd);
    return (r == (ssize_t)count) ? 0 : -1;
}

/* ============================================================
 * Security framework - SecKey functions (added 10.12+)
 * ============================================================ */

#define STUB_UNIMPLEMENTED (-4)

CFDataRef SecKeyCopyExternalRepresentation(SecKeyRef key, CFErrorRef *error) {
    (void)key;
    if (error) *error = NULL;
    return NULL;
}

SecKeyRef SecKeyCreateWithData(CFDataRef keyData, CFDictionaryRef attributes, CFErrorRef *error) {
    (void)keyData; (void)attributes;
    if (error) *error = NULL;
    return NULL;
}

SecKeyRef SecKeyCreateRandomKey(CFDictionaryRef parameters, CFErrorRef *error) {
    (void)parameters;
    if (error) *error = NULL;
    return NULL;
}

CFDataRef SecKeyCreateSignature(SecKeyRef key, SecPadding algorithm,
                                CFDataRef dataToSign, CFErrorRef *error) {
    (void)key; (void)algorithm; (void)dataToSign;
    if (error) *error = NULL;
    return NULL;
}

Boolean SecKeyVerifySignature(SecKeyRef key, SecPadding algorithm,
                              CFDataRef signedData, CFDataRef signature,
                              CFErrorRef *error) {
    (void)key; (void)algorithm; (void)signedData; (void)signature;
    if (error) *error = NULL;
    return false;
}

CFDataRef SecKeyCreateEncryptedData(SecKeyRef key, SecPadding algorithm,
                                    CFDataRef plaintext, CFErrorRef *error) {
    (void)key; (void)algorithm; (void)plaintext;
    if (error) *error = NULL;
    return NULL;
}

CFDataRef SecKeyCreateDecryptedData(SecKeyRef key, SecPadding algorithm,
                                    CFDataRef ciphertext, CFErrorRef *error) {
    (void)key; (void)algorithm; (void)ciphertext;
    if (error) *error = NULL;
    return NULL;
}

SecKeyRef SecKeyCopyPublicKey(SecKeyRef key) {
    (void)key;
    return NULL;
}

CFDictionaryRef SecKeyCopyAttributes(SecKeyRef key) {
    (void)key;
    return NULL;
}

CFDataRef SecKeyCopyKeyExchangeResult(SecKeyRef publicKey, void *algorithm,
                                      SecKeyRef parameters, CFDictionaryRef requestedSize,
                                      CFErrorRef *error) {
    (void)publicKey; (void)algorithm; (void)parameters; (void)requestedSize;
    if (error) *error = NULL;
    return NULL;
}

/* SecCertificateCopyKey - added 10.14 */
SecKeyRef SecCertificateCopyKey(SecCertificateRef certificate) {
    (void)certificate;
    return NULL;
}

/* ============================================================
 * SSL/TLS ALPN functions (added 10.13.4)
 * ============================================================ */

OSStatus SSLCopyALPNProtocols(void *context, CFArrayRef *protocols) {
    (void)context;
    if (protocols) *protocols = NULL;
    return STUB_UNIMPLEMENTED;
}

OSStatus SSLSetALPNProtocols(void *context, CFArrayRef protocols) {
    (void)context; (void)protocols;
    return STUB_UNIMPLEMENTED;
}

/* ============================================================
 * Security framework constants (added 10.12+)
 * ============================================================ */

const CFStringRef kSecAttrKeyTypeECSECPrimeRandom = CFSTR("73");
const CFStringRef kSecUseDataProtectionKeychain = CFSTR("u-DataProtectionKeychain");

/* SecKeyAlgorithm constants (added 10.12) */
const CFStringRef kSecKeyAlgorithmECDHKeyExchangeStandard = CFSTR("algid:ecdh:standard");
const CFStringRef kSecKeyAlgorithmECDSASignatureDigestX962 = CFSTR("algid:ecdsa:digest-x962");
const CFStringRef kSecKeyAlgorithmRSAEncryptionOAEPSHA1 = CFSTR("algid:encrypt:RSA:OAEP-SHA1");
const CFStringRef kSecKeyAlgorithmRSAEncryptionOAEPSHA256 = CFSTR("algid:encrypt:RSA:OAEP-SHA256");
const CFStringRef kSecKeyAlgorithmRSAEncryptionOAEPSHA384 = CFSTR("algid:encrypt:RSA:OAEP-SHA384");
const CFStringRef kSecKeyAlgorithmRSAEncryptionOAEPSHA512 = CFSTR("algid:encrypt:RSA:OAEP-SHA512");
const CFStringRef kSecKeyAlgorithmRSAEncryptionPKCS1 = CFSTR("algid:encrypt:RSA:PKCS1");
const CFStringRef kSecKeyAlgorithmRSAEncryptionRaw = CFSTR("algid:encrypt:RSA:raw");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1 = CFSTR("algid:sign:RSA:digest-PKCS1v15:SHA1");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256 = CFSTR("algid:sign:RSA:digest-PKCS1v15:SHA256");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384 = CFSTR("algid:sign:RSA:digest-PKCS1v15:SHA384");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512 = CFSTR("algid:sign:RSA:digest-PKCS1v15:SHA512");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPSSSHA1 = CFSTR("algid:sign:RSA:digest-PSS:SHA1");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPSSSHA256 = CFSTR("algid:sign:RSA:digest-PSS:SHA256");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPSSSHA384 = CFSTR("algid:sign:RSA:digest-PSS:SHA384");
const CFStringRef kSecKeyAlgorithmRSASignatureDigestPSSSHA512 = CFSTR("algid:sign:RSA:digest-PSS:SHA512");
const CFStringRef kSecKeyAlgorithmRSASignatureRaw = CFSTR("algid:sign:RSA:raw");

/* ============================================================
 * IOKit - kIOMainPortDefault (added macOS 12, replaces kIOMasterPortDefault)
 * Both are MACH_PORT_NULL (0).
 * ============================================================ */
#include <mach/mach_port.h>
const mach_port_t kIOMainPortDefault = 0;

/* Note: +[GCController supportsHIDDevice:] (added macOS 10.15) is patched
 * directly in libFosterPlatform.dylib (IOS_SupportedHIDDevice always returns
 * false, skipping GCController and using HID joystick handling instead). */
