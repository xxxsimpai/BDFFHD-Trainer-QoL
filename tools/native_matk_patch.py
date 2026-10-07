"""Apply the version-locked native M.Atk weapon-proficiency patch."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
from pathlib import Path

import pefile


GAME_HASH = "AD04CC75DAF0B80381627E2DF84DE936760EFF99E901E9ADEF1B6B19AA1E3858"
HOOKS = {
    0x63E058: (bytes.fromhex("4C8D442438BAEF030000"), "native M.Atk proficiency call"),
    0x63E065: (bytes.fromhex("8BD8"), "preserve adjusted M.Atk"),
}
GAME_METHODS = {"ItemTableLookup": 0x666550, "ItemProperCalc": 0x63D970}
SECTION_ALIGNMENT = 0x1000
FILE_ALIGNMENT = 0x200


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def export_rvas(stub_path: Path) -> tuple[bytes, dict[str, int]]:
    stub = pefile.PE(str(stub_path), fast_load=False)
    names: dict[str, int] = {}
    for exp in stub.DIRECTORY_ENTRY_EXPORT.symbols:
        if exp.name:
            names[exp.name.decode("ascii")] = exp.address
    required = {"NativeMatkAdjust", *GAME_METHODS}
    if not required.issubset(names):
        raise RuntimeError(f"Stub exports missing: {sorted(required - names.keys())}")
    text = next(s for s in stub.sections if s.Name.rstrip(b"\0") == b".text")
    blob = bytearray(stub.get_data(text.VirtualAddress, text.Misc_VirtualSize))
    return bytes(blob), names


def add_code_section(data: bytearray, pe: pefile.PE, code: bytes, exports: dict[str, int]) -> tuple[int, int]:
    section_table_end = pe.sections[-1].get_file_offset() + 40
    if section_table_end + 40 > pe.OPTIONAL_HEADER.SizeOfHeaders:
        raise RuntimeError("No spare PE header room for the native code section")

    old_end = max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections)
    raw_ptr = align(max(old_end, len(data)), pe.OPTIONAL_HEADER.FileAlignment)
    va = align(max(s.VirtualAddress + s.Misc_VirtualSize for s in pe.sections), pe.OPTIONAL_HEADER.SectionAlignment)
    raw_size = align(len(code), pe.OPTIONAL_HEADER.FileAlignment)
    if len(data) < raw_ptr:
        data.extend(b"\0" * (raw_ptr - len(data)))
    data.extend(code)
    data.extend(b"\0" * (raw_size - len(code)))

    # Redirect the helper's two internal call stubs into GameAssembly methods.
    for name, target_rva in GAME_METHODS.items():
        stub_offset = exports[name] - next(s.VirtualAddress for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
        call_stub = bytearray(b"\xE9\0\0\0\0")
        source_next = va + stub_offset + 5
        struct.pack_into("<i", call_stub, 1, target_rva - source_next)
        data[raw_ptr + stub_offset:raw_ptr + stub_offset + 5] = call_stub

    header = struct.pack("<8sIIIIIIHHI", b".bdffmat", len(code), va, raw_size, raw_ptr, 0, 0, 0, 0, 0x60000020)
    data[section_table_end:section_table_end + 40] = header
    pe.FILE_HEADER.NumberOfSections += 1
    pe.OPTIONAL_HEADER.SizeOfImage = align(va + len(code), pe.OPTIONAL_HEADER.SectionAlignment)
    pe.OPTIONAL_HEADER.SizeOfCode += raw_size
    pe.OPTIONAL_HEADER.SizeOfInitializedData += raw_size
    struct.pack_into("<I", data, pe.FILE_HEADER.get_file_offset() + 2, pe.FILE_HEADER.NumberOfSections)
    struct.pack_into("<I", data, pe.OPTIONAL_HEADER.get_file_offset() + 56, pe.OPTIONAL_HEADER.SizeOfImage)
    struct.pack_into("<I", data, pe.OPTIONAL_HEADER.get_file_offset() + 4, pe.OPTIONAL_HEADER.SizeOfCode)
    struct.pack_into("<I", data, pe.OPTIONAL_HEADER.get_file_offset() + 8, pe.OPTIONAL_HEADER.SizeOfInitializedData)
    return va, raw_ptr


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("gameassembly", type=Path)
    ap.add_argument("stub", type=Path, help="MASM-built native_matk_stub.dll")
    ap.add_argument("--backup", type=Path)
    args = ap.parse_args()
    path = args.gameassembly.resolve()
    data = bytearray(path.read_bytes())
    before_hash = hashlib.sha256(data).hexdigest().upper()
    if before_hash != GAME_HASH:
        raise RuntimeError(f"Unsupported GameAssembly.dll hash: {before_hash}")
    pe = pefile.PE(data=data, fast_load=False)
    for rva, (expected, _) in HOOKS.items():
        off = pe.get_offset_from_rva(rva)
        if data[off:off + len(expected)] != expected:
            raise RuntimeError(f"Unexpected bytes at RVA 0x{rva:X}")
    text_va = next(s.VirtualAddress for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code, exports = export_rvas(args.stub)
    code = bytearray(code)
    trampoline_offset = len(code)
    trampoline = bytearray.fromhex(
        "48 89 F9 E8 00 00 00 00 45 33 C9 C6 44 24 38 00 "
        "4C 8D 44 24 38 BA EF 03 00 00 E9 00 00 00 00"
    )
    helper_offset = exports["NativeMatkAdjust"] - text_va
    struct.pack_into("<i", trampoline, 4, helper_offset - (trampoline_offset + 8))
    code += trampoline
    # Append the helper and its argument-preserving trampoline, then redirect
    # the internal call stubs to verified game methods.
    native_rva, native_raw = add_code_section(data, pe, code, exports)
    resume = 0x63E058 + 10
    struct.pack_into("<i", data, native_raw + trampoline_offset + len(trampoline) - 4,
                     resume - (native_rva + trampoline_offset + len(trampoline)))
    helper_rva = native_rva + exports["NativeMatkAdjust"] - text_va
    off = pe.get_offset_from_rva(0x63E058)
    call = bytearray(b"\xE9\0\0\0\0\x90\x90\x90\x90\x90")
    struct.pack_into("<i", call, 1, (native_rva + trampoline_offset) - (0x63E058 + 5))
    data[off:off + 10] = call

    if args.backup:
        args.backup.parent.mkdir(parents=True, exist_ok=True)
        if args.backup.exists():
            raise RuntimeError(f"Refusing to overwrite backup: {args.backup}")
        shutil.copy2(path, args.backup)
    tmp = path.with_suffix(path.suffix + ".native.tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)
    print(json.dumps({
        "file": str(path),
        "original_sha256": before_hash,
        "patched_sha256": hashlib.sha256(data).hexdigest().upper(),
        "helper_rva": hex(helper_rva),
        "backup": str(args.backup) if args.backup else None,
    }, indent=2))


if __name__ == "__main__":
    main()
