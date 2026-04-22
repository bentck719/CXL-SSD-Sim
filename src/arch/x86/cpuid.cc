/*
 * Copyright (c) 2008 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/x86/cpuid.hh"

#include "arch/x86/isa.hh"
#include "base/bitfield.hh"
#include "cpu/thread_context.hh"

namespace gem5
{

namespace X86ISA {
    enum StandardCpuidFunction
    {
        VendorAndLargestStdFunc,
        FamilyModelStepping,
        CacheAndTLB,
        SerialNumber,
        CacheParams,
        MonitorMwait,
        ThermalPowerMgmt,
        ExtendedFeatures,
        NumStandardCpuidFuncs
    };

    enum ExtendedCpuidFunctions
    {
        VendorAndLargestExtFunc,
        FamilyModelSteppingBrandFeatures,
        NameString1,
        NameString2,
        NameString3,
        L1CacheAndTLB,
        L2L3CacheAndL2TLB,
        APMInfo,
        LongModeAddressSize,

        /*
         * The following are defined by the spec but not yet implemented
         */
/*      // Function 9 is reserved
        SVMInfo = 10,
        // Functions 11-24 are reserved
        TLB1GBPageInfo = 25,
        PerformanceInfo,*/

        NumExtendedCpuidFuncs
    };

    static const int nameStringSize = 48;
    static const char nameString[nameStringSize] = "Fake M5 x86_64 CPU";

    uint64_t
    stringToRegister(const char *str)
    {
        uint64_t reg = 0;
        for (int pos = 3; pos >=0; pos--) {
            reg <<= 8;
            reg |= str[pos];
        }
        return reg;
    }

    bool
    doCpuid(ThreadContext * tc, uint32_t function,
            uint32_t index, CpuidResult &result)
    {
        uint16_t family = bits(function, 31, 16);
        uint16_t funcNum = bits(function, 15, 0);
        if (family == 0x8000) {
            // The extended functions
            switch (funcNum) {
              case VendorAndLargestExtFunc:
                {
                  ISA *isa = dynamic_cast<ISA *>(tc->getIsaPtr());
                  auto vendor_string = isa->getVendorString();
                  result = CpuidResult(
                          0x80000000 + NumExtendedCpuidFuncs - 1,
                          stringToRegister(vendor_string.c_str()),
                          stringToRegister(vendor_string.c_str() + 4),
                          stringToRegister(vendor_string.c_str() + 8));
                }
                break;
              case FamilyModelSteppingBrandFeatures:
                result = CpuidResult(0x00020f51, 0x00000405,
                                     0xebd3fbff, 0x00020001);
                break;
              case NameString1:
              case NameString2:
              case NameString3:
                {
                    // Zero fill anything beyond the end of the string. This
                    // should go away once the string is a vetted parameter.
                    char cleanName[nameStringSize];
                    memset(cleanName, '\0', nameStringSize);
                    strncpy(cleanName, nameString, nameStringSize);

                    int offset = (funcNum - NameString1) * 16;
                    assert(nameStringSize >= offset + 16);
                    result = CpuidResult(
                            stringToRegister(cleanName + offset + 0),
                            stringToRegister(cleanName + offset + 4),
                            stringToRegister(cleanName + offset + 12),
                            stringToRegister(cleanName + offset + 8));
                }
                break;
              case L1CacheAndTLB:
                result = CpuidResult(0xff08ff08, 0xff20ff20,
                                     0x40020140, 0x40020140);
                break;
              case L2L3CacheAndL2TLB:
                result = CpuidResult(0x00000000, 0x42004200,
                                     0x00000000, 0x04008140);
                break;
              case APMInfo:
                result = CpuidResult(0x80000018, 0x68747541,
                                     0x69746e65, 0x444d4163);
                break;
              case LongModeAddressSize:
                result = CpuidResult(0x00003030, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
/*            case SVMInfo:
              case TLB1GBPageInfo:
              case PerformanceInfo:*/
              case 29:
                // 0x8000001d: AMD Deterministic Cache Parameters.
                // Return null entry (EAX[4:0]=0) for all sub-leaves.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              default:
                warn("x86 cpuid family 0x8000: unimplemented function %u",
                    funcNum);
                return false;
            }
        } else if (family == 0x4000) {
            // Hypervisor CPUID range (0x40000000–0x4fffffff).
            // gem5 is not a paravirt hypervisor; silently return false so
            // the Linux KVM-guest driver skips all paravirt optimizations.
            return false;
        } else if (family == 0x0000) {
            // The standard functions
            switch (funcNum) {
              case VendorAndLargestStdFunc:
                {
                  ISA *isa = dynamic_cast<ISA *>(tc->getIsaPtr());
                  auto vendor_string = isa->getVendorString();
                  // EAX = 0xd: highest standard leaf we implement (function 13,
                  // XSAVE state enumeration).  Reporting this lets the kernel
                  // query leaf 0xd for the correct XSAVE area sizes.
                  result = CpuidResult(
                          0x0000000d,
                          stringToRegister(vendor_string.c_str()),
                          stringToRegister(vendor_string.c_str() + 4),
                          stringToRegister(vendor_string.c_str() + 8));
                }
                break;
              case FamilyModelStepping:
                // Restore original ECX feature flags (0xefdbfbff).
                // SSE4.2 (bit 20), PCLMULQDQ (bit 1), XSAVE (bit 26), and
                // OSXSAVE (bit 27) are all advertised as present so that
                // Ubuntu 24.04 glibc (x86-64-v2 baseline) loads correctly.
                // pcmpistri / pcmpistrm now have proper safe stubs in the
                // ISA, and CPUID leaf 0xd (function 13) is implemented below
                // so the kernel can determine the correct XSAVE area size.
                result = CpuidResult(0x00020f51, 0x00000805,
                                     0xefdbfbff, 0x00000209);
                break;
              case ExtendedFeatures:
                result = CpuidResult(0x00000000, 0x01800000,
                                     0x00000000, 0x00000000);
                break;
              case 13:
                // XSAVE Extended State Enumeration (leaf 0xd).
                // The 'index' parameter is the sub-leaf (ECX on entry).
                switch (index) {
                  case 0:
                    // Sub-leaf 0: supported XCR0 feature mask and total sizes.
                    // EAX[1:0] = 0x3 → x87 (bit 0) + SSE/XMM (bit 1).
                    // EBX = required XSAVE area size for current XCR0:
                    //   512 B legacy FXSAVE region + 64 B XSAVE header = 576 B.
                    // ECX = maximum XSAVE area size (same, no AVX/other exts).
                    // EDX = upper 32 bits of XCR0 supported mask (0).
                    result = CpuidResult(0x00000003, 0x00000240,
                                         0x00000240, 0x00000000);
                    break;
                  case 1:
                    // Sub-leaf 1: XSAVEOPT / XSAVEC / XGETBV(1) / XSAVES.
                    // None of these extensions are simulated; return all-zero.
                    result = CpuidResult(0x00000000, 0x00000240,
                                         0x00000000, 0x00000000);
                    break;
                  default:
                    // Sub-leaves 2+ describe individual state components
                    // (e.g. YMM at sub-leaf 2).  Return 0 for all unsupported.
                    result = CpuidResult(0x00000000, 0x00000000,
                                         0x00000000, 0x00000000);
                    break;
                }
                break;
              case CacheAndTLB:
                // Legacy cache/TLB descriptor leaf.  Return the
                // "no descriptors" sentinel so the kernel uses leaf 4.
                result = CpuidResult(0x00000001, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case SerialNumber:
                // Processor Serial Number — disabled on all modern CPUs.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case CacheParams:
                // Deterministic Cache Parameters (sub-leaf in ECX).
                // EAX[4:0] = 0 → null entry, terminates enumeration.
                // Returning zero for all sub-leaves tells the kernel and
                // glibc that no deterministic cache info is available.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case MonitorMwait:
                // MONITOR/MWAIT parameters.  Not supported; idle=poll is
                // passed on the cmdline so the kernel won't use MWAIT.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case ThermalPowerMgmt:
                // Thermal and Power Management — no features advertised.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case 8:
              case 9:
              case 10:
              case 12:
                // Reserved / unimplemented leaves — return all zeros.
                result = CpuidResult(0x00000000, 0x00000000,
                                     0x00000000, 0x00000000);
                break;
              case 11:
                // Extended Topology Enumeration (x2APIC).
                // Single-core, single-thread topology.
                switch (index) {
                  case 0:
                    // SMT level: 1 logical processor, shift = 0.
                    result = CpuidResult(0x00000000, 0x00000001,
                                         0x00000100, 0x00000000);
                    break;
                  case 1:
                    // Core level: 1 logical processor per package, shift = 1.
                    result = CpuidResult(0x00000001, 0x00000001,
                                         0x00000201, 0x00000000);
                    break;
                  default:
                    // Sub-leaf ≥ 2: level type = 0 (invalid), terminates.
                    result = CpuidResult(0x00000000, 0x00000000,
                                         (uint32_t)index, 0x00000000);
                    break;
                }
                break;
              default:
                warn("x86 cpuid family 0x0000: unimplemented function %u",
                    funcNum);
                return false;
            }
        } else {
            warn("x86 cpuid: unknown family %#x", family);
            return false;
        }

        return true;
    }
} // namespace X86ISA
} // namespace gem5
