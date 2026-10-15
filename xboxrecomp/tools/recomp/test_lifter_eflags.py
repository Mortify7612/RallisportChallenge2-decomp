"""pushfd / popfd / cpuid: the cpuid-availability probe.

    pushfd; pop eax; mov ecx, eax; xor eax, 0x200000; push eax; popfd;
    pushfd; pop eax; xor eax, ecx; jz no_cpuid; ... cpuid

All three used to lift to nothing. pushfd then pushed nothing for the pop to
take, so the probe walked esp off by four -- RalliSport Challenge 2's zlib
inflate_fast (inffas32.asm) crashed on the corrupt frame -- and cpuid left
eax..edx as they were."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _lift(*insns):
    return "\n".join(lift_basic_block(Lifter(), BasicBlock(
        start=insns[0].address, instructions=list(insns)))[0])


def _insn(addr, size, mnem, op_str="", ops=()):
    i = Instruction(addr, size, mnem, op_str, "90" * size)
    i.operands = list(ops)
    return i


class EflagsTest(unittest.TestCase):
    def test_pushfd_pushes_shadow_and_tracked_flags(self):
        cmp = _insn(0, 3, "cmp", "eax, 1",
                    [Operand(type="reg", reg="eax"), Operand(type="imm", imm=1)])
        out = _lift(cmp, _insn(3, 1, "pushfd"))
        self.assertIn("PUSH32(esp, (g_eflags_shadow & RECOMP_EFLAGS_SHADOW_MASK)", out)
        self.assertIn("0x040u", out)  # ZF rebuilt from the cmp

    def test_pushfd_without_a_setter_still_pushes(self):
        out = _lift(_insn(0, 1, "pushfd"))
        self.assertIn("PUSH32(esp, (g_eflags_shadow & RECOMP_EFLAGS_SHADOW_MASK));", out)

    def test_popfd_pops_into_the_shadow(self):
        out = _lift(_insn(0, 1, "popfd"))
        self.assertIn("POP32(esp, _efl)", out)
        self.assertIn("g_eflags_shadow = (_efl & RECOMP_EFLAGS_SHADOW_MASK)", out)

    def test_cpuid_answers(self):
        out = _lift(_insn(0, 2, "cpuid"))
        self.assertIn("RECOMP_CPUID();", out)
        self.assertNotIn("RECOMP_UNIMPL", out)


if __name__ == "__main__":
    unittest.main()
