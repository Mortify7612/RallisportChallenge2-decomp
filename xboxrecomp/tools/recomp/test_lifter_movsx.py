"""movsx from a 16-bit register sign-extends for every register, bp and sp
included. bp used to fall through to a bare LO16 -- a zero extension -- which
RalliSport Challenge 2's Bink IDCT (0x003503FD, `movsx ebx, bp`) turned into
corrupt 8x8 blocks on every negative coefficient."""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _movsx(dst, src):
    i = Instruction(0, 3, "movsx", f"{dst}, {src}", "0fbfdd")
    i.operands = [Operand(type="reg", reg=dst), Operand(type="reg", reg=src)]
    return "\n".join(lift_basic_block(Lifter(), BasicBlock(start=0, instructions=[i]))[0])


class MovsxRegTest(unittest.TestCase):
    def test_every_16bit_register_sign_extends(self):
        for src in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            with self.subTest(src=src):
                self.assertIn("SX16(", _movsx("ebx", src))

    def test_8bit_sign_extends(self):
        self.assertIn("SX8(", _movsx("eax", "cl"))


if __name__ == "__main__":
    unittest.main()
