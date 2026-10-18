"""Stores become MEMn_W calls; reads, comments and comparisons do not."""
from tools.recomp.mem_stores import route_mem_stores


def test_plain_store():
    assert route_mem_stores("    MEM32(esi + 0x1B4) = edx;") == "    MEM32_W(esi + 0x1B4, edx);"


def test_nested_reads_kept():
    src = "MEM8(edi + MEM32(esi)) = (uint8_t)(MEM8(ebx + 1) | 2);"
    assert route_mem_stores(src) == "MEM8_W(edi + MEM32(esi), (uint8_t)(MEM8(ebx + 1) | 2));"


def test_every_width():
    for m in ("MEM8", "MEM16", "MEM32", "SMEM32", "SMEM64", "MEMF", "MEMD"):
        assert route_mem_stores(f"{m}(a) = b;") == f"{m}_W(a, b);"


def test_reads_and_comparisons_untouched():
    src = "if (MEM32(a) == 3) eax = MEM32(b); _fa = (uint32_t)(MEM32(c)) & 1;"
    assert route_mem_stores(src) == src


def test_comments_and_strings_untouched():
    src = '/* MEM32(x) = y; */ s = "MEM32(x) = y;"; // MEM8(z) = 1;\n'
    assert route_mem_stores(src) == src


def test_store_in_loop_body():
    src = "for (_i = 0; _i < ecx; _i++) MEM32(edi + _i*_st) = eax;"
    assert route_mem_stores(src) == "for (_i = 0; _i < ecx; _i++) MEM32_W(edi + _i*_st, eax);"


def test_identifier_suffix_not_matched():
    src = "XMEM32(a) = b; RECOMP_MEM8(c) = d;"
    assert route_mem_stores(src) == src


def test_value_with_ternary_and_call():
    src = "MEM32(esp) = f(a, b) ? c : d;"
    assert route_mem_stores(src) == "MEM32_W(esp, f(a, b) ? c : d);"
