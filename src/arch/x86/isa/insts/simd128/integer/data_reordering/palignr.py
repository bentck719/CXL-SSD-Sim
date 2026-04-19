# Copyright (c) 2007 The Hewlett-Packard Development Company
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# PALIGNR xmm1, xmm2/m128, imm8
# Concatenates [xmm2_high:xmm2_low : xmm1_high:xmm1_low] (256 bits),
# shifts right by imm8 bytes, stores low 128 bits into xmm1.
# xmml/xmmh = dst low/high; xmmlm/xmmhm = src low/high (register source).
# ufp1,ufp2 = shift intermediates; ufp3,ufp4 = used in M/P variants.

microcode = """
def macroop PALIGNR_XMM_XMM_I {
    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xxi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xxi_le16"), flags=(nCECF,)
    limm t2, 24
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xxi_le24"), flags=(nCECF,)
    limm t2, 32
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xxi_le32"), flags=(nCECF,)
    lfpimm xmml, 0
    lfpimm xmmh, 0
    br label("palignr_xxi_end")

palignr_xxi_le32:
    msrli xmml, xmmhm, "(IMMEDIATE-24)<<3", size=8, ext=0
    lfpimm xmmh, 0
    br label("palignr_xxi_end")

palignr_xxi_le24:
    msrli ufp1, xmmlm, "(IMMEDIATE-16)<<3", size=8, ext=0
    mslli ufp2, xmmhm, "(24-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp1, ufp2
    msrli xmmh, xmmhm, "(IMMEDIATE-16)<<3", size=8, ext=0
    br label("palignr_xxi_end")

palignr_xxi_le16:
    msrli ufp1, xmmh, "(IMMEDIATE-8)<<3", size=8, ext=0
    mslli ufp2, xmmlm, "(16-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp1, ufp2
    mslli ufp1, xmmhm, "(16-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, xmmlm, "(IMMEDIATE-8)<<3", size=8, ext=0
    mor xmmh, xmmh, ufp1
    br label("palignr_xxi_end")

palignr_xxi_le8:
    msrli ufp1, xmml, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp2, xmmh, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp1, ufp2
    mslli ufp1, xmmlm, "(8-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, xmmh, "IMMEDIATE<<3", size=8, ext=0
    mor xmmh, xmmh, ufp1

palignr_xxi_end:
    fault "NoFault"
};

def macroop PALIGNR_XMM_M_I {
    ldfp ufp1, seg, sib, "DISPLACEMENT", dataSize=8
    ldfp ufp2, seg, sib, "DISPLACEMENT + 8", dataSize=8

    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xmi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xmi_le16"), flags=(nCECF,)
    limm t2, 24
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xmi_le24"), flags=(nCECF,)
    limm t2, 32
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xmi_le32"), flags=(nCECF,)
    lfpimm xmml, 0
    lfpimm xmmh, 0
    br label("palignr_xmi_end")

palignr_xmi_le32:
    msrli xmml, ufp2, "(IMMEDIATE-24)<<3", size=8, ext=0
    lfpimm xmmh, 0
    br label("palignr_xmi_end")

palignr_xmi_le24:
    msrli ufp3, ufp1, "(IMMEDIATE-16)<<3", size=8, ext=0
    mslli ufp4, ufp2, "(24-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    msrli xmmh, ufp2, "(IMMEDIATE-16)<<3", size=8, ext=0
    br label("palignr_xmi_end")

palignr_xmi_le16:
    msrli ufp3, xmmh, "(IMMEDIATE-8)<<3", size=8, ext=0
    mslli ufp4, ufp1, "(16-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    mslli ufp3, ufp2, "(16-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, ufp1, "(IMMEDIATE-8)<<3", size=8, ext=0
    mor xmmh, xmmh, ufp3
    br label("palignr_xmi_end")

palignr_xmi_le8:
    msrli ufp3, xmml, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp4, xmmh, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    mslli ufp3, ufp1, "(8-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, xmmh, "IMMEDIATE<<3", size=8, ext=0
    mor xmmh, xmmh, ufp3

palignr_xmi_end:
    fault "NoFault"
};

def macroop PALIGNR_XMM_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, "DISPLACEMENT", dataSize=8
    ldfp ufp2, seg, riprel, "DISPLACEMENT + 8", dataSize=8

    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xpi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xpi_le16"), flags=(nCECF,)
    limm t2, 24
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xpi_le24"), flags=(nCECF,)
    limm t2, 32
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_xpi_le32"), flags=(nCECF,)
    lfpimm xmml, 0
    lfpimm xmmh, 0
    br label("palignr_xpi_end")

palignr_xpi_le32:
    msrli xmml, ufp2, "(IMMEDIATE-24)<<3", size=8, ext=0
    lfpimm xmmh, 0
    br label("palignr_xpi_end")

palignr_xpi_le24:
    msrli ufp3, ufp1, "(IMMEDIATE-16)<<3", size=8, ext=0
    mslli ufp4, ufp2, "(24-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    msrli xmmh, ufp2, "(IMMEDIATE-16)<<3", size=8, ext=0
    br label("palignr_xpi_end")

palignr_xpi_le16:
    msrli ufp3, xmmh, "(IMMEDIATE-8)<<3", size=8, ext=0
    mslli ufp4, ufp1, "(16-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    mslli ufp3, ufp2, "(16-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, ufp1, "(IMMEDIATE-8)<<3", size=8, ext=0
    mor xmmh, xmmh, ufp3
    br label("palignr_xpi_end")

palignr_xpi_le8:
    msrli ufp3, xmml, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp4, xmmh, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor xmml, ufp3, ufp4
    mslli ufp3, ufp1, "(8-IMMEDIATE)<<3", size=8, ext=0
    msrli xmmh, xmmh, "IMMEDIATE<<3", size=8, ext=0
    mor xmmh, xmmh, ufp3

palignr_xpi_end:
    fault "NoFault"
};
"""
