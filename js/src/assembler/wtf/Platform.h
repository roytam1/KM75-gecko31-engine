/*
 * Copyright (C) 2006, 2007, 2008, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2007-2009 Torch Mobile, Inc.
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef assembler_wtf_Platform_h
#define assembler_wtf_Platform_h

/* ==== PLATFORM handles OS, operating environment, graphics API, and
   CPU. This macro will be phased out in favor of platform adaptation
   macros, policy decision macros, and top-level port definitions. ==== */
#define PLATFORM(WTF_FEATURE) (defined WTF_PLATFORM_##WTF_FEATURE  && WTF_PLATFORM_##WTF_FEATURE)


/* ==== Platform adaptation macros: these describe properties of the target environment. ==== */

/* COMPILER() - the compiler being used to build the project */
#define COMPILER(WTF_FEATURE) (defined WTF_COMPILER_##WTF_FEATURE  && WTF_COMPILER_##WTF_FEATURE)
/* CPU() - the target CPU architecture */
#define CPU(WTF_FEATURE) (defined WTF_CPU_##WTF_FEATURE  && WTF_CPU_##WTF_FEATURE)
/* HAVE() - specific system features (headers, functions or similar) that are present or not */
#define HAVE(WTF_FEATURE) (defined HAVE_##WTF_FEATURE  && HAVE_##WTF_FEATURE)
/* OS() - underlying operating system; only to be used for mandated low-level services like
   virtual memory, not to choose a GUI toolkit */
#define OS(WTF_FEATURE) (defined WTF_OS_##WTF_FEATURE  && WTF_OS_##WTF_FEATURE)


/* ==== Policy decision macros: these define policy choices for a particular port. ==== */

/* USE() - use a particular third-party library or optional OS service */
#define USE(WTF_FEATURE) (defined WTF_USE_##WTF_FEATURE  && WTF_USE_##WTF_FEATURE)
/* ENABLE() - turn on a specific feature of WebKit */
#define ENABLE(WTF_FEATURE) (defined ENABLE_##WTF_FEATURE  && ENABLE_##WTF_FEATURE)



/* ==== COMPILER() - the compiler being used to build the project ==== */

/* WTF_COMPILER_MSVC Microsoft Visual C++ */
/* WTF_COMPILER_MSVC7_OR_LOWER Microsoft Visual C++ 2003 or lower*/
/* WTF_COMPILER_MSVC9_OR_LOWER Microsoft Visual C++ 2008 or lower*/
#if defined(_MSC_VER)
#define WTF_COMPILER_MSVC 1
#if _MSC_VER < 1400
#define WTF_COMPILER_MSVC7_OR_LOWER 1
#elif _MSC_VER < 1600
#define WTF_COMPILER_MSVC9_OR_LOWER 1
#endif
#endif

/* WTF_COMPILER_RVCT  - ARM RealView Compilation Tools */
/* WTF_COMPILER_RVCT4_OR_GREATER - ARM RealView Compilation Tools 4.0 or greater */
#if defined(__CC_ARM) || defined(__ARMCC__)
#define WTF_COMPILER_RVCT 1
#define RVCT_VERSION_AT_LEAST(major, minor, patch, build) (__ARMCC_VERSION >= (major * 100000 + minor * 10000 + patch * 1000 + build))
#else
/* Define this for !RVCT compilers, just so we can write things like RVCT_VERSION_AT_LEAST(3, 0, 0, 0). */
#define RVCT_VERSION_AT_LEAST(major, minor, patch, build) 0
#endif

/* WTF_COMPILER_GCC - GNU Compiler Collection */
/* --gnu option of the RVCT compiler also defines __GNUC__ */
#if defined(__GNUC__) && !WTF_COMPILER_RVCT
#define WTF_COMPILER_GCC 1
#define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#define GCC_VERSION_AT_LEAST(major, minor, patch) (GCC_VERSION >= (major * 10000 + minor * 100 + patch))
#else
/* Define this for !GCC compilers, just so we can write things like GCC_VERSION_AT_LEAST(4, 1, 0). */
#define GCC_VERSION_AT_LEAST(major, minor, patch) 0
#endif

/* WTF_COMPILER_MINGW - MinGW GCC */
/* WTF_COMPILER_MINGW64 - mingw-w64 GCC - only used as additional check to exclude mingw.org specific functions */
#if defined(__MINGW32__)
#define WTF_COMPILER_MINGW 1
#include <_mingw.h> /* private MinGW header */
    #if defined(__MINGW64_VERSION_MAJOR) /* best way to check for mingw-w64 vs mingw.org */
        #define WTF_COMPILER_MINGW64 1
    #endif /* __MINGW64_VERSION_MAJOR */
#endif /* __MINGW32__ */

/* WTF_COMPILER_WINSCW - CodeWarrior for Symbian emulator */
#if defined(__WINSCW__)
#define WTF_COMPILER_WINSCW 1
/* cross-compiling, it is not really windows */
#undef WIN32
#undef _WIN32
#endif

/* WTF_COMPILER_INTEL - Intel C++ Compiler */
#if defined(__INTEL_COMPILER)
#define WTF_COMPILER_INTEL 1
#endif

/* WTF_COMPILER_SUNCC */
#if defined(__SUNPRO_CC) || defined(__SUNPRO_C)
#define WTF_COMPILER_SUNCC 1
#endif

/* ==== CPU() - the target CPU architecture ==== */

/* This also defines WTF_CPU_BIG_ENDIAN or WTF_CPU_MIDDLE_ENDIAN or neither, as appropriate. */

/* WTF_CPU_ALPHA - DEC Alpha */
#if defined(__alpha__)
#define WTF_CPU_ALPHA 1
#endif

/* WTF_CPU_IA64 - Itanium / IA-64 */
#if defined(__ia64__)
#define WTF_CPU_IA64 1
/* 32-bit mode on Itanium */
#if !defined(__LP64__)
#define WTF_CPU_IA64_32 1
#endif
#endif

/* WTF_CPU_MIPS - MIPS 32-bit */
/* Note: Only O32 ABI is tested, so we enable it for O32 ABI for now.  */
#if (defined(mips) || defined(__mips__) || defined(MIPS) || defined(_MIPS_)) \
    && defined(_ABIO32)
#define WTF_CPU_MIPS 1
#if defined(__MIPSEB__)
#define WTF_CPU_BIG_ENDIAN 1
#endif
#define WTF_MIPS_PIC (defined __PIC__)
#define WTF_MIPS_ARCH __mips
#define WTF_MIPS_ISA(v) (defined WTF_MIPS_ARCH && WTF_MIPS_ARCH == v)
#define WTF_MIPS_ISA_AT_LEAST(v) (defined WTF_MIPS_ARCH && WTF_MIPS_ARCH >= v)
#define WTF_MIPS_ARCH_REV __mips_isa_rev
#define WTF_MIPS_ISA_REV(v) (defined WTF_MIPS_ARCH_REV && WTF_MIPS_ARCH_REV == v)
#define WTF_MIPS_DOUBLE_FLOAT (defined __mips_hard_float && !defined __mips_single_float)
#define WTF_MIPS_FP64 (defined __mips_fpr && __mips_fpr == 64)
/* MIPS requires allocators to use aligned memory */
#define WTF_USE_ARENA_ALLOC_ALIGNMENT_INTEGER 1
#endif /* MIPS */

/* WTF_CPU_PPC - PowerPC 32-bit */
#if   defined(__ppc__)     \
    || defined(__PPC__)     \
    || defined(__powerpc__) \
    || defined(__powerpc)   \
    || defined(__POWERPC__) \
    || defined(_M_PPC)      \
    || defined(__PPC)
#if !defined(__ppc64__) && !defined(__PPC64__)
#define WTF_CPU_PPC 1
#endif
#if !defined(__LITTLE_ENDIAN__)
#define WTF_CPU_BIG_ENDIAN 1
#endif
#endif

/* WTF_CPU_PPC64 - PowerPC 64-bit */
#if   defined(__ppc64__) \
    || defined(__PPC64__)
#define WTF_CPU_PPC64 1
#if !defined(__LITTLE_ENDIAN__)
#define WTF_CPU_BIG_ENDIAN 1
#endif
#endif

/* WTF_CPU_SH4 - SuperH SH-4 */
#if defined(__SH4__)
#define WTF_CPU_SH4 1
#endif

/* WTF_CPU_SPARC32 - SPARC 32-bit */
#if defined(__sparc) && !defined(__arch64__) || defined(__sparcv8)
#define WTF_CPU_SPARC32 1
#define WTF_CPU_BIG_ENDIAN 1
#endif

/* WTF_CPU_SPARC64 - SPARC 64-bit */
#if defined(__sparc__) && defined(__arch64__) || defined (__sparcv9)
#define WTF_CPU_SPARC64 1
#define WTF_CPU_BIG_ENDIAN 1
#endif

/* WTF_CPU_SPARC - any SPARC, true for WTF_CPU_SPARC32 and WTF_CPU_SPARC64 */
#if WTF_CPU_SPARC32 || WTF_CPU_SPARC64
#define WTF_CPU_SPARC 1
#endif

/* WTF_CPU_S390X - S390 64-bit */
#if defined(__s390x__)
#define WTF_CPU_S390X 1
#define WTF_CPU_BIG_ENDIAN 1
#endif

/* WTF_CPU_S390 - S390 32-bit */
#if defined(__s390__)
#define WTF_CPU_S390 1
#define WTF_CPU_BIG_ENDIAN 1
#endif

#if defined(__aarch64__)
#define WTF_CPU_AARCH64 1
#if defined(__AARCH64EB__)
#define WTF_CPU_BIG_ENDIAN 1
#endif
#endif

/* WTF_CPU_X86 - i386 / x86 32-bit */
#if   defined(__i386__) \
    || defined(i386)     \
    || defined(_M_IX86)  \
    || defined(_X86_)    \
    || defined(__THW_INTEL)
#define WTF_CPU_X86 1
#endif

/* WTF_CPU_X86_64 - AMD64 / Intel64 / x86_64 64-bit */
#if   defined(__x86_64__) \
    || defined(_M_X64)
#define WTF_CPU_X86_64 1
#endif

/* WTF_CPU_ARM - ARM, any version*/
#if   defined(arm) \
    || defined(__arm__) \
    || defined(ARM) \
    || defined(_ARM_)
#define WTF_CPU_ARM 1

#if defined(__ARMEB__) || (WTF_COMPILER_RVCT && defined(__BIG_ENDIAN))
#define WTF_CPU_BIG_ENDIAN 1

#elif !defined(__ARM_EABI__) \
    && !defined(__EABI__) \
    && !defined(__VFP_FP__) \
    && !defined(_WIN32_WCE) \
    && !defined(ANDROID)
#define WTF_CPU_MIDDLE_ENDIAN 1

#endif

#define WTF_ARM_ARCH_AT_LEAST(N) (CPU(ARM) && WTF_ARM_ARCH_VERSION >= N)
#define WTF_ARM_ARCH_AT_LEAST_5 (WTF_CPU_ARM && WTF_ARM_ARCH_VERSION >= 5)

/* Set WTF_ARM_ARCH_VERSION */
#if   defined(__ARM_ARCH_4__) \
    || defined(__ARM_ARCH_4T__) \
    || defined(__MARM_ARMV4__) \
    || defined(_ARMV4I_)
#define WTF_ARM_ARCH_VERSION 4

#elif defined(__ARM_ARCH_5__) \
    || defined(__ARM_ARCH_5T__) \
    || defined(__MARM_ARMV5__)
#define WTF_ARM_ARCH_VERSION 5

#elif defined(__ARM_ARCH_5E__) \
    || defined(__ARM_ARCH_5TE__) \
    || defined(__ARM_ARCH_5TEJ__)
#define WTF_ARM_ARCH_VERSION 5
/*ARMv5TE requires allocators to use aligned memory*/
#define WTF_USE_ARENA_ALLOC_ALIGNMENT_INTEGER 1

#elif defined(__ARM_ARCH_6__) \
    || defined(__ARM_ARCH_6J__) \
    || defined(__ARM_ARCH_6K__) \
    || defined(__ARM_ARCH_6Z__) \
    || defined(__ARM_ARCH_6ZK__) \
    || defined(__ARM_ARCH_6T2__) \
    || defined(__ARMV6__)
#define WTF_ARM_ARCH_VERSION 6

#elif defined(__ARM_ARCH_7A__) \
    || defined(__ARM_ARCH_7R__)
#define WTF_ARM_ARCH_VERSION 7

/* RVCT sets _TARGET_ARCH_ARM */
#elif defined(__TARGET_ARCH_ARM)
#define WTF_ARM_ARCH_VERSION __TARGET_ARCH_ARM

#if defined(__TARGET_ARCH_5E) \
    || defined(__TARGET_ARCH_5TE) \
    || defined(__TARGET_ARCH_5TEJ)
/*ARMv5TE requires allocators to use aligned memory*/
#define WTF_USE_ARENA_ALLOC_ALIGNMENT_INTEGER 1
#endif

#else
#define WTF_ARM_ARCH_VERSION 0

#endif

/* Set WTF_THUMB_ARCH_VERSION */
#if   defined(__ARM_ARCH_4T__)
#define WTF_THUMB_ARCH_VERSION 1

#elif defined(__ARM_ARCH_5T__) \
    || defined(__ARM_ARCH_5TE__) \
    || defined(__ARM_ARCH_5TEJ__)
#define WTF_THUMB_ARCH_VERSION 2

#elif defined(__ARM_ARCH_6J__) \
    || defined(__ARM_ARCH_6K__) \
    || defined(__ARM_ARCH_6Z__) \
    || defined(__ARM_ARCH_6ZK__) \
    || defined(__ARM_ARCH_6M__)
#define WTF_THUMB_ARCH_VERSION 3

#elif defined(__ARM_ARCH_6T2__) \
    || defined(__ARM_ARCH_7__) \
    || defined(__ARM_ARCH_7A__) \
    || defined(__ARM_ARCH_7R__) \
    || defined(__ARM_ARCH_7M__)
#define WTF_THUMB_ARCH_VERSION 4

/* RVCT sets __TARGET_ARCH_THUMB */
#elif defined(__TARGET_ARCH_THUMB)
#define WTF_THUMB_ARCH_VERSION __TARGET_ARCH_THUMB

#else
#define WTF_THUMB_ARCH_VERSION 0
#endif


/* WTF_CPU_ARMV5_OR_LOWER - ARM instruction set v5 or earlier */
/* On ARMv5 and below the natural alignment is required.
   And there are some other differences for v5 or earlier. */
#if !defined(ARMV5_OR_LOWER) && WTF_CPU_ARM && !(WTF_ARM_ARCH_VERSION >= 6)
#define WTF_CPU_ARMV5_OR_LOWER 1
#endif


/* WTF_CPU_ARM_TRADITIONAL - Thumb2 is not available, only traditional ARM (v4 or greater) */
/* WTF_CPU_ARM_THUMB2 - Thumb2 instruction set is available */
/* Only one of these will be defined. */
#if !defined(WTF_CPU_ARM_TRADITIONAL) && !defined(WTF_CPU_ARM_THUMB2)
#  if defined(thumb2) || defined(__thumb2__) \
    || ((defined(__thumb) || defined(__thumb__)) && WTF_THUMB_ARCH_VERSION == 4)
#    define WTF_CPU_ARM_TRADITIONAL 1
#    define WTF_CPU_ARM_THUMB2 0
#  elif WTF_CPU_ARM && WTF_ARM_ARCH_VERSION >= 4
#    define WTF_CPU_ARM_TRADITIONAL 1
#    define WTF_CPU_ARM_THUMB2 0
#  else
#    error "Not supported ARM architecture"
#  endif
#elif WTF_CPU_ARM_TRADITIONAL && WTF_CPU_ARM_THUMB2 /* Sanity Check */
#  error "Cannot use both of WTF_CPU_ARM_TRADITIONAL and WTF_CPU_ARM_THUMB2 platforms"
#endif /* !defined(WTF_CPU_ARM_TRADITIONAL) && !defined(WTF_CPU_ARM_THUMB2) */

#if defined(__ARM_NEON__) && !defined(WTF_CPU_ARM_NEON)
#define WTF_CPU_ARM_NEON 1
#endif

#endif /* ARM */

#if defined(JS_ARM_SIMULATOR)
#  undef WTF_CPU_X86
#  undef WTF_CPU_X64
#  define WTF_CPU_ARM_TRADITIONAL 1
#  define WTF_CPU_ARM 1
#endif

#if WTF_CPU_ARM || WTF_CPU_MIPS
#define WTF_CPU_NEEDS_ALIGNED_ACCESS 1
#endif

/* ==== OS() - underlying operating system; only to be used for mandated low-level services like
   virtual memory, not to choose a GUI toolkit ==== */

/* WTF_OS_ANDROID - Android */
#ifdef ANDROID
#define WTF_OS_ANDROID 1
#endif

/* WTF_OS_AIX - AIX */
#ifdef _AIX
#define WTF_OS_AIX 1
#endif

/* WTF_OS_DARWIN - Any Darwin-based OS, including Mac OS X and iPhone OS */
#ifdef __APPLE__
#define WTF_OS_DARWIN 1

/* FIXME: BUILDING_ON_.., and TARGETING... macros should be folded into the OS() system */
#include <AvailabilityMacros.h>
#if !defined(MAC_OS_X_VERSION_10_5) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_5
#define BUILDING_ON_TIGER 1
#elif !defined(MAC_OS_X_VERSION_10_6) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_6
#define BUILDING_ON_LEOPARD 1
#elif !defined(MAC_OS_X_VERSION_10_7) || MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_7
#define BUILDING_ON_SNOW_LEOPARD 1
#endif
#if !defined(MAC_OS_X_VERSION_10_5) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_5
#define TARGETING_TIGER 1
#elif !defined(MAC_OS_X_VERSION_10_6) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_6
#define TARGETING_LEOPARD 1
#elif !defined(MAC_OS_X_VERSION_10_7) || MAC_OS_X_VERSION_MIN_REQUIRED < MAC_OS_X_VERSION_10_7
#define TARGETING_SNOW_LEOPARD 1
#endif
#include <TargetConditionals.h>

#endif

/* WTF_OS_IOS - iOS */
/* WTF_OS_MAC_OS_X - Mac OS X (not including iOS) */
#if WTF_OS_DARWIN && ((defined(TARGET_OS_EMBEDDED) && TARGET_OS_EMBEDDED)  \
    || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)                   \
    || (defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR))
#define WTF_OS_IOS 1
#elif WTF_OS_DARWIN && defined(TARGET_OS_MAC) && TARGET_OS_MAC
#define WTF_OS_MAC_OS_X 1
#endif


/* WTF_OS_FREEBSD - FreeBSD */
#if defined(__FreeBSD__) || defined(__DragonFly__)
#define WTF_OS_FREEBSD 1
#endif

/* WTF_OS_HAIKU - Haiku */
#ifdef __HAIKU__
#define WTF_OS_HAIKU 1
#endif

/* WTF_OS_LINUX - Linux */
#if defined(__linux__) && !defined(ANDROID)
#define WTF_OS_LINUX 1
#endif

/* WTF_OS_NETBSD - NetBSD */
#if defined(__NetBSD__)
#define WTF_OS_NETBSD 1
#endif

/* WTF_OS_OPENBSD - OpenBSD */
#ifdef __OpenBSD__
#define WTF_OS_OPENBSD 1
#endif

/* WTF_OS_QNX - QNX */
#if defined(__QNXNTO__)
#define WTF_OS_QNX 1
#endif

/* WTF_OS_SOLARIS - Solaris */
#if defined(sun) || defined(__sun)
#define WTF_OS_SOLARIS 1
#endif

/* WTF_OS_WINCE - Windows CE; note that for this platform WTF_OS_WINDOWS is also defined */
#if defined(_WIN32_WCE)
#define WTF_OS_WINCE 1
#endif

/* WTF_OS_WINDOWS - Any version of Windows */
#if defined(WIN32) || defined(_WIN32)
#define WTF_OS_WINDOWS 1
#endif

/* WTF_OS_SYMBIAN - Symbian */
#if defined (__SYMBIAN32__)
#define WTF_OS_SYMBIAN 1
#endif

/* WTF_OS_UNIX - Any Unix-like system */
#if   WTF_OS_AIX              \
    || WTF_OS_ANDROID          \
    || WTF_OS_DARWIN           \
    || WTF_OS_FREEBSD          \
    || WTF_OS_HAIKU            \
    || WTF_OS_LINUX            \
    || WTF_OS_NETBSD           \
    || WTF_OS_OPENBSD          \
    || WTF_OS_QNX              \
    || WTF_OS_SOLARIS          \
    || WTF_OS_SYMBIAN          \
    || defined(unix)        \
    || defined(__unix)      \
    || defined(__unix__)
#define WTF_OS_UNIX 1
#endif

/* WTF_OS_OS2 - OS/2 */
#if defined (__OS2__)
#define WTF_OS_OS2 1
#endif

/* Operating environments */

/* FIXME: these are all mixes of OS, operating environment and policy choices. */
/* WTF_PLATFORM_CHROMIUM */
/* WTF_PLATFORM_QT */
/* WTF_PLATFORM_WX */
/* WTF_PLATFORM_GTK */
/* WTF_PLATFORM_HAIKU */
/* WTF_PLATFORM_MAC */
/* WTF_PLATFORM_WIN */
#if defined(BUILDING_CHROMIUM__)
#define WTF_PLATFORM_CHROMIUM 1
#elif defined(BUILDING_QT__)
#define WTF_PLATFORM_QT 1
#elif defined(BUILDING_WX__)
#define WTF_PLATFORM_WX 1
#elif defined(BUILDING_GTK__)
#define WTF_PLATFORM_GTK 1
#elif defined(BUILDING_HAIKU__)
#define WTF_PLATFORM_HAIKU 1
#elif defined(BUILDING_BREWMP__)
#define WTF_PLATFORM_BREWMP 1
#if defined(AEE_SIMULATOR)
#define WTF_PLATFORM_BREWMP_SIMULATOR 1
#else
#define WTF_PLATFORM_BREWMP_SIMULATOR 0
#endif
#undef WTF_OS_WINDOWS
#undef WTF_PLATFORM_WIN
#elif WTF_OS_DARWIN
#define WTF_PLATFORM_MAC 1
#elif WTF_OS_WINDOWS
#define WTF_PLATFORM_WIN 1
#endif

/* WTF_PLATFORM_IOS */
/* FIXME: this is sometimes used as an OS switch and sometimes for higher-level things */
#if (defined(TARGET_OS_EMBEDDED) && TARGET_OS_EMBEDDED) || (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#define WTF_PLATFORM_IOS 1
#endif

/* WTF_PLATFORM_IOS_SIMULATOR */
#if defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR
#define WTF_PLATFORM_IOS 1
#define WTF_PLATFORM_IOS_SIMULATOR 1
#else
#define WTF_PLATFORM_IOS_SIMULATOR 0
#endif

#if !defined(WTF_PLATFORM_IOS)
#define WTF_PLATFORM_IOS 0
#endif

/* WTF_PLATFORM_ANDROID */
/* FIXME: this is sometimes used as an OS() switch, and other times to drive
   policy choices */
#if defined(ANDROID)
#define WTF_PLATFORM_ANDROID 1
#endif

/* Graphics engines */

/* WTF_USE_CG and WTF_PLATFORM_CI */
#if WTF_PLATFORM_MAC || WTF_PLATFORM_IOS
#define WTF_USE_CG 1
#endif
#if WTF_PLATFORM_MAC || WTF_PLATFORM_IOS || (WTF_PLATFORM_WIN && WTF_USE_CG)
#define WTF_USE_CA 1
#endif

/* WTF_USE_SKIA for Win/Linux, CG for Mac */
#if WTF_PLATFORM_CHROMIUM
#if WTF_OS_DARWIN
#define WTF_USE_CG 1
#define WTF_USE_ATSUI 1
#define WTF_USE_CORE_TEXT 1
#define WTF_USE_ICCJPEG 1
#else
#define WTF_USE_SKIA 1
#define WTF_USE_CHROMIUM_NET 1
#endif
#endif

#if WTF_PLATFORM_BREWMP
#define WTF_USE_SKIA 1
#endif

#if WTF_PLATFORM_GTK
#define WTF_USE_CAIRO 1
#endif


#if WTF_OS_WINCE
#include <ce_time.h>
#define WTF_USE_MERSENNE_TWISTER_19937 1
#endif

#if WTF_PLATFORM_QT && WTF_OS_UNIX && !WTF_OS_SYMBIAN && !WTF_OS_DARWIN
#define WTF_USE_PTHREAD_BASED_QT 1
#endif

#if (WTF_PLATFORM_GTK || WTF_PLATFORM_IOS || WTF_PLATFORM_MAC || WTF_PLATFORM_WIN || (WTF_PLATFORM_QT && (WTF_OS_DARWIN || WTF_USE_PTHREAD_BASED_QT) && !ENABLE_SINGLE_THREADED)) && !defined(ENABLE_JSC_MULTIPLE_THREADS)
#define ENABLE_JSC_MULTIPLE_THREADS 1
#endif

#if ENABLE_JSC_MULTIPLE_THREADS
#define ENABLE_WTF_MULTIPLE_THREADS 1
#endif

/* On Windows, use QueryPerformanceCounter by default */
#if WTF_OS_WINDOWS
#define WTF_USE_QUERY_PERFORMANCE_COUNTER  1
#endif

#if WTF_OS_WINCE && !WTF_PLATFORM_QT
#define NOMINMAX       /* Windows min and max conflict with standard macros */
#define NOSHLWAPI      /* shlwapi.h not available on WinCe */

/* MSDN documentation says these functions are provided with uspce.lib.  But we cannot find this file. */
#define __usp10__      /* disable "usp10.h" */

#define _INC_ASSERT    /* disable "assert.h" */
#define assert(x)

#endif  /* WTF_OS_WINCE && !WTF_PLATFORM_QT */

#if WTF_PLATFORM_QT
#define WTF_USE_QT4_UNICODE 1
#elif WTF_OS_WINCE
#define WTF_USE_WINCE_UNICODE 1
#elif WTF_PLATFORM_BREWMP
#define WTF_USE_BREWMP_UNICODE 1
#elif WTF_PLATFORM_GTK
/* The GTK+ Unicode backend is configurable */
#else
#define WTF_USE_ICU_UNICODE 1
#endif

#if WTF_PLATFORM_MAC && !WTF_PLATFORM_IOS
#if !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_TIGER) && WTF_CPU_X86_64
#define WTF_USE_PLUGIN_HOST_PROCESS 1
#endif
#if !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
#define ENABLE_GESTURE_EVENTS 1
#define ENABLE_RUBBER_BANDING 1
#define WTF_USE_WK_SCROLLBAR_PAINTER 1
#endif
#if !defined(ENABLE_JAVA_BRIDGE)
#define ENABLE_JAVA_BRIDGE 1
#endif
#if !defined(ENABLE_DASHBOARD_SUPPORT)
#define ENABLE_DASHBOARD_SUPPORT 1
#endif
#define WTF_USE_CF 1
#define WTF_USE_PTHREADS 1
#define HAVE_PTHREAD_RWLOCK 1
#define HAVE_READLINE 1
#define HAVE_RUNLOOP_TIMER 1
#define ENABLE_FULLSCREEN_API 1
#define ENABLE_SMOOTH_SCROLLING 1
#define ENABLE_WEB_ARCHIVE 1
#endif /* WTF_PLATFORM_MAC && !WTF_PLATFORM_IOS */

#if WTF_PLATFORM_CHROMIUM && WTF_OS_DARWIN
#define WTF_USE_CF 1
#define WTF_USE_PTHREADS 1
#define HAVE_PTHREAD_RWLOCK 1
#endif

#if WTF_PLATFORM_BREWMP
#define ENABLE_SINGLE_THREADED 1
#endif

#if WTF_PLATFORM_QT && WTF_OS_DARWIN
#define WTF_USE_CF 1
#endif

#if WTF_OS_DARWIN && !defined(BUILDING_ON_TIGER) && !WTF_PLATFORM_GTK && !WTF_PLATFORM_QT
#define ENABLE_PURGEABLE_MEMORY 1
#endif

#if WTF_PLATFORM_IOS
#define ENABLE_CONTEXT_MENUS 0
#define ENABLE_DRAG_SUPPORT 0
#define ENABLE_DATA_TRANSFER_ITEMS 0
#define ENABLE_FTPDIR 1
#define ENABLE_GEOLOCATION 1
#define ENABLE_ICONDATABASE 0
#define ENABLE_INSPECTOR 0
#define ENABLE_JAVA_BRIDGE 0
#define ENABLE_NETSCAPE_PLUGIN_API 0
#define ENABLE_ORIENTATION_EVENTS 1
#define ENABLE_REPAINT_THROTTLING 1
#define HAVE_READLINE 1
#define WTF_USE_CF 1
#define WTF_USE_PTHREADS 1
#define HAVE_PTHREAD_RWLOCK 1
#define ENABLE_WEB_ARCHIVE 1
#endif

#if WTF_PLATFORM_ANDROID
#define WTF_USE_PTHREADS 1
#define USE_SYSTEM_MALLOC 1
#define ENABLE_JAVA_BRIDGE 1
#define LOG_DISABLED 1
/* Prevents Webkit from drawing the caret in textfields and textareas
   This prevents unnecessary invals. */
#define ENABLE_TEXT_CARET 1
#define ENABLE_JAVASCRIPT_DEBUGGER 0
#endif

#if WTF_PLATFORM_WIN && !WTF_OS_WINCE
#define WTF_USE_CF 1
#define WTF_USE_PTHREADS 0
#endif

#if WTF_PLATFORM_WIN && !WTF_OS_WINCE && !WTF_PLATFORM_CHROMIUM && !defined(WIN_CAIRO)
#define WTF_USE_CFNETWORK 1
#endif

#if WTF_USE_CFNETWORK || WTF_PLATFORM_MAC
#define WTF_USE_CFURLCACHE 1
#define WTF_USE_CFURLSTORAGESESSIONS 1
#endif

#if WTF_PLATFORM_WIN && !WTF_OS_WINCE && !WTF_PLATFORM_CHROMIUM && !WTF_PLATFORM_QT
#define ENABLE_WEB_ARCHIVE 1
#endif

#if WTF_PLATFORM_WX
#define ENABLE_ASSEMBLER 1
#define ENABLE_GLOBAL_FASTMALLOC_NEW 0
#if WTF_OS_DARWIN
#define WTF_USE_CF 1
#ifndef BUILDING_ON_TIGER
#define WTF_USE_CORE_TEXT 1
#define ENABLE_WEB_ARCHIVE 1
#else
#define WTF_USE_ATSUI 1
#endif
#endif
#endif

#if WTF_PLATFORM_GTK
#if HAVE_PTHREAD_H
#define WTF_USE_PTHREADS 1
#define HAVE_PTHREAD_RWLOCK 1
#endif
#endif

#if WTF_PLATFORM_HAIKU
#define HAVE_POSIX_MEMALIGN 1
#define WTF_USE_CURL 1
#define WTF_USE_PTHREADS 1
#define HAVE_PTHREAD_RWLOCK 1
#define USE_SYSTEM_MALLOC 1
#define ENABLE_NETSCAPE_PLUGIN_API 0
#endif

#if WTF_PLATFORM_BREWMP
#define USE_SYSTEM_MALLOC 1
#endif

#if WTF_PLATFORM_BREWMP_SIMULATOR
#define ENABLE_JIT 0
#endif

#if !defined(HAVE_ACCESSIBILITY)
#if WTF_PLATFORM_IOS || WTF_PLATFORM_MAC || WTF_PLATFORM_WIN || WTF_PLATFORM_GTK || WTF_PLATFORM_CHROMIUM
#define HAVE_ACCESSIBILITY 1
#endif
#endif /* !defined(HAVE_ACCESSIBILITY) */

#if WTF_OS_UNIX && !WTF_OS_SYMBIAN
#define HAVE_SIGNAL_H 1
#endif

#if !defined(HAVE_STRNSTR)
#if WTF_OS_DARWIN || WTF_OS_FREEBSD
#define HAVE_STRNSTR 1
#endif
#endif

#if !WTF_OS_WINDOWS && !WTF_OS_SOLARIS && !WTF_OS_QNX \
    && !WTF_OS_SYMBIAN && !WTF_OS_HAIKU && !WTF_OS_RVCT \
    && !WTF_OS_ANDROID && !WTF_PLATFORM_BREWMP
#define HAVE_TM_GMTOFF 1
#define HAVE_TM_ZONE 1
#define HAVE_TIMEGM 1
#endif

#if WTF_OS_DARWIN

#define HAVE_ERRNO_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_MMAP 1
#define HAVE_MERGESORT 1
#define HAVE_SBRK 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TIMEB_H 1
#define WTF_USE_ACCELERATE 1

#if !defined(TARGETING_TIGER) && !defined(TARGETING_LEOPARD)

#define HAVE_DISPATCH_H 1
#define HAVE_HOSTED_CORE_ANIMATION 1

#if !WTF_PLATFORM_IOS
#define HAVE_MADV_FREE_REUSE 1
#define HAVE_MADV_FREE 1
#define HAVE_PTHREAD_SETNAME_NP 1
#endif

#endif

#if WTF_PLATFORM_IOS
#define HAVE_MADV_FREE 1
#endif

#elif WTF_OS_WINDOWS

#if WTF_OS_WINCE
#define HAVE_ERRNO_H 0
#else
#define HAVE_SYS_TIMEB_H 1
#define HAVE_ALIGNED_MALLOC 1
#define HAVE_ISDEBUGGERPRESENT 1
#endif
#define HAVE_VIRTUALALLOC 1

#elif WTF_OS_SYMBIAN

#define HAVE_ERRNO_H 1
#define HAVE_MMAP 0
#define HAVE_SBRK 1

#define HAVE_SYS_TIME_H 1
#define HAVE_STRINGS_H 1

#if !WTF_COMPILER_RVCT
#define HAVE_SYS_PARAM_H 1
#endif

#elif WTF_PLATFORM_BREWMP

#define HAVE_ERRNO_H 1

#elif WTF_OS_QNX

#define HAVE_ERRNO_H 1
#define HAVE_MMAP 1
#define HAVE_SBRK 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1

#elif WTF_OS_ANDROID

#define HAVE_ERRNO_H 1
#define HAVE_LANGINFO_H 0
#define HAVE_MMAP 1
#define HAVE_SBRK 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1

#elif WTF_OS_OS2

#define USE_SYSTEM_MALLOC 1
#define HAVE_ERRNO_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_MMAP 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_SBRK 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_STRINGS_H 1

#else

/* FIXME: is this actually used or do other platforms generate their own config.h? */

#define HAVE_ERRNO_H 1
/* As long as Haiku doesn't have a complete support of locale this will be disabled. */
#if !WTF_OS_HAIKU
#define HAVE_LANGINFO_H 1
#endif
#define HAVE_MMAP 1
#define HAVE_SBRK 1
#define HAVE_STRINGS_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1

#endif

/* ENABLE macro defaults */

#if WTF_PLATFORM_QT
/* We must not customize the global operator new and delete for the Qt port. */
#define ENABLE_GLOBAL_FASTMALLOC_NEW 0
#if !WTF_OS_UNIX || WTF_OS_SYMBIAN
#define USE_SYSTEM_MALLOC 1
#endif
#endif

/* fastMalloc match validation allows for runtime verification that
   new is matched by delete, fastMalloc is matched by fastFree, etc. */
#if !defined(ENABLE_FAST_MALLOC_MATCH_VALIDATION)
#define ENABLE_FAST_MALLOC_MATCH_VALIDATION 0
#endif

#if !defined(ENABLE_ICONDATABASE)
#define ENABLE_ICONDATABASE 1
#endif

#if !defined(ENABLE_DATABASE)
#define ENABLE_DATABASE 1
#endif

#if !defined(ENABLE_JAVASCRIPT_DEBUGGER)
#define ENABLE_JAVASCRIPT_DEBUGGER 1
#endif

#if !defined(ENABLE_FTPDIR)
#define ENABLE_FTPDIR 1
#endif

#if !defined(ENABLE_CONTEXT_MENUS)
#define ENABLE_CONTEXT_MENUS 1
#endif

#if !defined(ENABLE_DRAG_SUPPORT)
#define ENABLE_DRAG_SUPPORT 1
#endif

#if !defined(ENABLE_DATA_TRANSFER_ITEMS)
#define ENABLE_DATA_TRANSFER_ITEMS 0
#endif

#if !defined(ENABLE_DASHBOARD_SUPPORT)
#define ENABLE_DASHBOARD_SUPPORT 0
#endif

#if !defined(ENABLE_INSPECTOR)
#define ENABLE_INSPECTOR 1
#endif

#if !defined(ENABLE_JAVA_BRIDGE)
#define ENABLE_JAVA_BRIDGE 0
#endif

#if !defined(ENABLE_NETSCAPE_PLUGIN_API)
#define ENABLE_NETSCAPE_PLUGIN_API 1
#endif

#if !defined(ENABLE_NETSCAPE_PLUGIN_METADATA_CACHE)
#define ENABLE_NETSCAPE_PLUGIN_METADATA_CACHE 0
#endif

#if !defined(ENABLE_PURGEABLE_MEMORY)
#define ENABLE_PURGEABLE_MEMORY 0
#endif

#if !defined(WTF_USE_PLUGIN_HOST_PROCESS)
#define WTF_USE_PLUGIN_HOST_PROCESS 0
#endif

#if !defined(ENABLE_ORIENTATION_EVENTS)
#define ENABLE_ORIENTATION_EVENTS 0
#endif

#if !defined(ENABLE_OPCODE_STATS)
#define ENABLE_OPCODE_STATS 0
#endif

#if !defined(ENABLE_GLOBAL_FASTMALLOC_NEW)
#define ENABLE_GLOBAL_FASTMALLOC_NEW 1
#endif

#define ENABLE_DEBUG_WITH_BREAKPOINT 0
#define ENABLE_SAMPLING_COUNTERS 0
#define ENABLE_SAMPLING_FLAGS 0
#define ENABLE_OPCODE_SAMPLING 0
#define ENABLE_CODEBLOCK_SAMPLING 0
#if ENABLE_CODEBLOCK_SAMPLING && !ENABLE_OPCODE_SAMPLING
#error "CODEBLOCK_SAMPLING requires OPCODE_SAMPLING"
#endif
#if ENABLE_OPCODE_SAMPLING || ENABLE_SAMPLING_FLAGS
#define ENABLE_SAMPLING_THREAD 1
#endif

#if !defined(ENABLE_GEOLOCATION)
#define ENABLE_GEOLOCATION 0
#endif

#if !defined(ENABLE_GESTURE_RECOGNIZER)
#define ENABLE_GESTURE_RECOGNIZER 0
#endif

#if !defined(ENABLE_NOTIFICATIONS)
#define ENABLE_NOTIFICATIONS 0
#endif

#if WTF_PLATFORM_IOS
#define ENABLE_TEXT_CARET 0
#endif

#if !defined(ENABLE_TEXT_CARET)
#define ENABLE_TEXT_CARET 1
#endif

#if !defined(ENABLE_ON_FIRST_TEXTAREA_FOCUS_SELECT_ALL)
#define ENABLE_ON_FIRST_TEXTAREA_FOCUS_SELECT_ALL 0
#endif

#if !defined(ENABLE_FULLSCREEN_API)
#define ENABLE_FULLSCREEN_API 0
#endif

#if !defined(WTF_USE_JSVALUE64) && !defined(WTF_USE_JSVALUE32_64)
#if (WTF_CPU_X86_64 && (WTF_OS_UNIX || WTF_OS_WINDOWS)) \
    || (WTF_CPU_IA64 && !WTF_CPU_IA64_32) \
    || WTF_CPU_ALPHA \
    || WTF_CPU_SPARC64 \
    || WTF_CPU_S390X \
    || WTF_CPU_PPC64
#define WTF_USE_JSVALUE64 1
#else
#define WTF_USE_JSVALUE32_64 1
#endif
#endif /* !defined(WTF_USE_JSVALUE64) && !defined(WTF_USE_JSVALUE32_64) */

#if !defined(ENABLE_REPAINT_THROTTLING)
#define ENABLE_REPAINT_THROTTLING 0
#endif

/* Disable the JIT on versions of GCC prior to 4.1 */
#if !defined(ENABLE_JIT) && WTF_COMPILER_GCC && !GCC_VERSION_AT_LEAST(4, 1, 0)
#define ENABLE_JIT 0
#endif

/* The JIT is enabled by default on all x86, x64-64, ARM platforms. */
#if !defined(ENABLE_JIT) \
    && (WTF_CPU_X86 || WTF_CPU_X86_64 || WTF_CPU_ARM || WTF_CPU_SPARC32 || WTF_CPU_MIPS) \
    && (WTF_OS_DARWIN || !WTF_COMPILER_GCC || GCC_VERSION_AT_LEAST(4, 1, 0)) \
    && !WTF_OS_WINCE
#define ENABLE_JIT 1
#endif

/* Currently only implemented for JSVALUE64, only tested on WTF_PLATFORM_MAC */
#if ENABLE_JIT && WTF_USE_JSVALUE64 && WTF_PLATFORM_MAC
#define ENABLE_DFG_JIT 1
/* Enabled with restrictions to circumvent known performance regressions. */
#define ENABLE_DFG_JIT_RESTRICTIONS 1
#endif

/* Ensure that either the JIT or the interpreter has been enabled. */
#if !defined(ENABLE_INTERPRETER) && !ENABLE_JIT
#define ENABLE_INTERPRETER 1
#endif
#if !(ENABLE_JIT || ENABLE_INTERPRETER)
#error You have to have at least one execution model enabled to build JSC
#endif

#if WTF_CPU_SH4 && WTF_PLATFORM_QT
#define ENABLE_JIT 1
#define ENABLE_YARR 1
#define ENABLE_YARR_JIT 1
#define WTF_USE_JIT_STUB_ARGUMENT_REGISTER 1
#define ENABLE_ASSEMBLER 1
#endif

/* Configure the JIT */
#if ENABLE_JIT
    #if WTF_CPU_ARM
    #if !defined(ENABLE_JIT_USE_SOFT_MODULO) && WTF_CPU_ARM && WTF_ARM_ARCH_VERSION >= 5
    #define ENABLE_JIT_USE_SOFT_MODULO 1
    #endif
    #endif

    #ifndef ENABLE_JIT_OPTIMIZE_CALL
    #define ENABLE_JIT_OPTIMIZE_CALL 1
    #endif
    #ifndef ENABLE_JIT_OPTIMIZE_NATIVE_CALL
    #define ENABLE_JIT_OPTIMIZE_NATIVE_CALL 1
    #endif
    #ifndef ENABLE_JIT_OPTIMIZE_PROPERTY_ACCESS
    #define ENABLE_JIT_OPTIMIZE_PROPERTY_ACCESS 1
    #endif
    #ifndef ENABLE_JIT_OPTIMIZE_METHOD_CALLS
    #define ENABLE_JIT_OPTIMIZE_METHOD_CALLS 1
    #endif
#endif

#if WTF_CPU_X86 && WTF_COMPILER_MSVC
#define JSC_HOST_CALL __fastcall
#elif WTF_CPU_X86 && WTF_COMPILER_GCC
#define JSC_HOST_CALL __attribute__ ((fastcall))
#else
#define JSC_HOST_CALL
#endif

/* Configure the interpreter */
#if WTF_COMPILER_GCC || (RVCT_VERSION_AT_LEAST(4, 0, 0, 0) && defined(__GNUC__))
#define HAVE_COMPUTED_GOTO 1
#endif
#if HAVE_COMPUTED_GOTO && ENABLE_INTERPRETER
#define ENABLE_COMPUTED_GOTO_INTERPRETER 1
#endif

/* Regular Expression Tracing - Set to 1 to trace RegExp's in jsc.  Results dumped at exit */
#define ENABLE_REGEXP_TRACING 0

/* Yet Another Regex Runtime - turned on by default for JIT enabled ports. */
#if WTF_PLATFORM_CHROMIUM
#define ENABLE_YARR_JIT 0

#elif ENABLE_YARR_JIT
/* Setting this flag compares JIT results with interpreter results. */
#define ENABLE_YARR_JIT_DEBUG 0
#endif

#if ENABLE_JIT || ENABLE_YARR_JIT
#define ENABLE_ASSEMBLER 1
#endif
/* Setting this flag prevents the assembler from using RWX memory; this may improve
   security but currectly comes at a significant performance cost. */
#if WTF_PLATFORM_IOS
//XXX: this doesn't currently compile in the spidermonkey build
#define ENABLE_ASSEMBLER_WX_EXCLUSIVE 0
#endif

/* Pick which allocator to use; we only need an executable allocator if the assembler is compiled in.
   On x86-64 we use a single fixed mmap, on other platforms we mmap on demand. */
#if ENABLE_ASSEMBLER
#if WTF_CPU_X86_64
#define ENABLE_EXECUTABLE_ALLOCATOR_FIXED 1
#else
#define ENABLE_EXECUTABLE_ALLOCATOR_DEMAND 1
#endif
#endif

#if !defined(ENABLE_PAN_SCROLLING) && WTF_OS_WINDOWS
#define ENABLE_PAN_SCROLLING 1
#endif

#if !defined(ENABLE_SMOOTH_SCROLLING)
#define ENABLE_SMOOTH_SCROLLING 0
#endif

#if !defined(ENABLE_WEB_ARCHIVE)
#define ENABLE_WEB_ARCHIVE 0
#endif

/* Use the QXmlStreamReader implementation for XMLDocumentParser */
/* Use the QXmlQuery implementation for XSLTProcessor */
#if WTF_PLATFORM_QT
#define WTF_USE_QXMLSTREAM 1
#define WTF_USE_QXMLQUERY 1
#endif

#if WTF_PLATFORM_MAC
/* Complex text framework */
#if !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)
#define WTF_USE_ATSUI 0
#define WTF_USE_CORE_TEXT 1
#else
#define WTF_USE_ATSUI 1
#define WTF_USE_CORE_TEXT 0
#endif
#endif

/* Accelerated compositing */
#if (WTF_PLATFORM_MAC && !defined(BUILDING_ON_TIGER)) || WTF_PLATFORM_IOS || WTF_PLATFORM_QT || (WTF_PLATFORM_WIN && !WTF_OS_WINCE&&!defined(WIN_CAIRO))
#define WTF_USE_ACCELERATED_COMPOSITING 1
#endif

#if (WTF_PLATFORM_MAC && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD)) || WTF_PLATFORM_IOS
#define WTF_USE_PROTECTION_SPACE_AUTH_CALLBACK 1
#endif

#if WTF_PLATFORM_MAC && !defined(BUILDING_ON_TIGER) && !defined(BUILDING_ON_LEOPARD) && !defined(BUILDING_ON_SNOW_LEOPARD)
#define WTF_USE_AVFOUNDATION 1
#endif

#if WTF_COMPILER_GCC
#define WARN_UNUSED_RETURN __attribute__ ((warn_unused_result))
#else
#define WARN_UNUSED_RETURN
#endif

/* COMPILER(CLANG) - Clang  */
#if defined(__clang__)
#define WTF_COMPILER_CLANG 1
#endif

#if !ENABLE_NETSCAPE_PLUGIN_API || (ENABLE_NETSCAPE_PLUGIN_API && ((WTF_OS_UNIX && (WTF_PLATFORM_QT || WTF_PLATFORM_WX)) || WTF_PLATFORM_GTK))
#define ENABLE_PLUGIN_PACKAGE_SIMPLE_HASH 1
#endif

/* Set up a define for a common error that is intended to cause a build error -- thus the space after Error. */
#define WTF_PLATFORM_CFNETWORK Error USE_macro_should_be_used_with_CFNETWORK

#define ENABLE_JSC_ZOMBIES 0

/* FIXME: Eventually we should enable this for all platforms and get rid of the define. */
#if WTF_PLATFORM_MAC || WTF_PLATFORM_WIN || WTF_PLATFORM_QT
#define WTF_USE_PLATFORM_STRATEGIES 1
#endif

#if WTF_PLATFORM_WIN
#define WTF_USE_CROSS_PLATFORM_CONTEXT_MENUS 1
#endif

/* Geolocation request policy. pre-emptive policy is to acquire user permission before acquiring location.
   Client based implementations will have option to choose between pre-emptive and nonpre-emptive permission policy.
   pre-emptive permission policy is enabled by default for all client-based implementations. */
#if ENABLE_CLIENT_BASED_GEOLOCATION
#define WTF_USE_PREEMPT_GEOLOCATION_PERMISSION 1
#endif

#if WTF_CPU_ARM_THUMB2
#define ENABLE_BRANCH_COMPACTION 1
#endif

#if !defined(ENABLE_THREADING_OPENMP) && defined(_OPENMP)
#define ENABLE_THREADING_OPENMP 1
#endif

#if !defined(ENABLE_PARALLEL_JOBS) && !ENABLE_SINGLE_THREADED && (ENABLE_THREADING_GENERIC || ENABLE_THREADING_LIBDISPATCH || ENABLE_THREADING_OPENMP)
#define ENABLE_PARALLEL_JOBS 1
#endif

#if ENABLE_GLIB_SUPPORT
//#include "GTypedefs.h"
#endif

/* FIXME: This define won't be needed once #27551 is fully landed. However,
   since most ports try to support sub-project independence, adding new headers
   to WTF causes many ports to break, and so this way we can address the build
   breakages one port at a time. */
#define WTF_USE_EXPORT_MACROS 0

#if WTF_PLATFORM_QT || WTF_PLATFORM_GTK
#define WTF_USE_UNIX_DOMAIN_SOCKETS 1
#endif

#endif /* assembler_wtf_Platform_h */
