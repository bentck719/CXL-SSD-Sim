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

microcode = """
def macroop PSHUFW_MMX_MMX_I {
    shuffle mmx, mmxm, mmxm, size=2, ext=imm
};

def macroop PSHUFW_MMX_M_I {
    ldfp ufp1, seg, sib, disp, dataSize=8
    shuffle mmx, ufp1, ufp1, size=2, ext=imm
};

def macroop PSHUFW_MMX_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    shuffle mmx, ufp1, ufp1, size=2, ext=imm
};

# PALIGNR mm1, mm2/m64, imm8
# Concatenates [src:dst] (128 bits), shifts right imm8 bytes, stores low 64 bits.

def macroop PALIGNR_MMX_MMX_I {
    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mxi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mxi_le16"), flags=(nCECF,)
    lfpimm mmx, 0
    br label("palignr_mxi_end")

palignr_mxi_le16:
    msrli mmx, mmxm, "(IMMEDIATE-8)<<3", size=8, ext=0
    br label("palignr_mxi_end")

palignr_mxi_le8:
    msrli ufp1, mmx, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp2, mmxm, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor mmx, ufp1, ufp2

palignr_mxi_end:
    fault "NoFault"
};

def macroop PALIGNR_MMX_M_I {
    ldfp ufp1, seg, sib, disp, dataSize=8
    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mmi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mmi_le16"), flags=(nCECF,)
    lfpimm mmx, 0
    br label("palignr_mmi_end")

palignr_mmi_le16:
    msrli mmx, ufp1, "(IMMEDIATE-8)<<3", size=8, ext=0
    br label("palignr_mmi_end")

palignr_mmi_le8:
    msrli ufp2, mmx, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp3, ufp1, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor mmx, ufp2, ufp3

palignr_mmi_end:
    fault "NoFault"
};

def macroop PALIGNR_MMX_P_I {
    rdip t7
    ldfp ufp1, seg, riprel, disp, dataSize=8
    limm t2, 8
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mpi_le8"), flags=(nCECF,)
    limm t2, 16
    subi t1, t2, imm, flags=(ECF,), dataSize=1
    br label("palignr_mpi_le16"), flags=(nCECF,)
    lfpimm mmx, 0
    br label("palignr_mpi_end")

palignr_mpi_le16:
    msrli mmx, ufp1, "(IMMEDIATE-8)<<3", size=8, ext=0
    br label("palignr_mpi_end")

palignr_mpi_le8:
    msrli ufp2, mmx, "IMMEDIATE<<3", size=8, ext=0
    mslli ufp3, ufp1, "(8-IMMEDIATE)<<3", size=8, ext=0
    mor mmx, ufp2, ufp3

palignr_mpi_end:
    fault "NoFault"
};

"""
# PSWAPD
