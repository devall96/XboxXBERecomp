#pragma once
// Force-included into the Linux executable and SDL (sdk/cmake/XbeRecomp.cmake): glibc 2.43 gave new versions to a
// family of math functions, and a build on it would refuse to start on any older system. These bind to the versions
// every x86-64 glibc has, so a release needs glibc 2.38 (its other newest symbols) rather than the build machine's.
#if defined(__linux__) && defined(__x86_64__)
#define XBE_OLD_SYMBOL(f) __asm__(".symver " #f "," #f "@GLIBC_2.2.5");
XBE_OLD_SYMBOL(acosf)
XBE_OLD_SYMBOL(acoshf)
XBE_OLD_SYMBOL(asinf)
XBE_OLD_SYMBOL(atan2f)
XBE_OLD_SYMBOL(atanhf)
XBE_OLD_SYMBOL(coshf)
XBE_OLD_SYMBOL(sinhf)
XBE_OLD_SYMBOL(sqrtf)
XBE_OLD_SYMBOL(log10f)
XBE_OLD_SYMBOL(lgammaf)
XBE_OLD_SYMBOL(lgammaf_r)
XBE_OLD_SYMBOL(tgammaf)
XBE_OLD_SYMBOL(remainder)
XBE_OLD_SYMBOL(remainderf)
XBE_OLD_SYMBOL(j0f)
XBE_OLD_SYMBOL(j1f)
XBE_OLD_SYMBOL(jnf)
XBE_OLD_SYMBOL(y0f)
XBE_OLD_SYMBOL(y1f)
XBE_OLD_SYMBOL(ynf)
#undef XBE_OLD_SYMBOL
#endif
