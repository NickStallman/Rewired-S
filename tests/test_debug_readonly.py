#!/usr/bin/env python3
"""The debug build's flash tooling must stay read-only.

components/sungrow_debug may reference NO flash write / erase / OTA-write / NVS-write symbol. The
one write the debug build can trigger, the switch-gated rollback to the stock slot
(esp_ota_set_boot_partition), lives in components/sungrow_board/boot_slot.cpp; boot_switch.cpp only
checks its arming flag and delegates. Comments are stripped before scanning, so the warnings in the
code may name the forbidden calls. Run: python3 -I tests/test_debug_readonly.py
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
COMP = ROOT / "components" / "sungrow_debug"

FORBIDDEN = [
    r"esp_flash_write\w*", r"esp_flash_erase\w*", r"esp_flash_set_chip_write_protect",
    r"esp_partition_write\w*", r"esp_partition_erase\w*", r"esp_partition_register_external",
    r"spi_flash_write\w*", r"spi_flash_erase\w*", r"esp_rom_spiflash_write\w*", r"esp_rom_spiflash_erase\w*",
    r"esp_ota_begin", r"esp_ota_write\w*", r"esp_ota_end", r"esp_ota_abort", r"esp_ota_erase_last_boot_app_partition",
    r"esp_ota_set_boot_partition", r"esp_ota_mark_app_\w+", r"esp_ota_revert_for_reboot\w*",
    r"nvs_set_\w+", r"nvs_erase_\w+", r"nvs_commit", r"nvs_flash_erase\w*",
    r"esp_littlefs_format\w*", r"fopen\s*\([^)]*\"[wa]", r"\bunlink\s*\(", r"\brename\s*\(", r"\bmkdir\s*\(",
    r"write_registers", r"submit_write",
]
ALLOWED = {}


def strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    # Python/YAML style comments in __init__.py
    return src


def main() -> int:
    failures = []
    files = sorted(p for p in COMP.iterdir() if p.suffix in (".cpp", ".h"))
    assert files, "no sources found"
    for p in files:
        code = strip_comments(p.read_text())
        for pat in FORBIDDEN:
            for m in re.finditer(pat, code):
                sym = m.group(0)
                if sym in ALLOWED.get(p.name, set()):
                    continue
                failures.append(f"{p.name}: forbidden write/erase reference '{sym}'")
    # The boot switch is gated by the debug flag and delegates to the board (the only place with
    # the otadata write).
    bs = strip_comments((COMP / "boot_switch.cpp").read_text())
    if "allow_boot_switch_" not in bs:
        failures.append("boot_switch.cpp: the boot switch must check allow_boot_switch_")
    if "global_board->boot_other_slot(" not in bs:
        failures.append("boot_switch.cpp: expected the delegation to sungrow_board::global_board->boot_other_slot()")
    board = strip_comments((ROOT / "components" / "sungrow_board" / "boot_slot.cpp").read_text())
    if board.count("esp_ota_set_boot_partition(") != 1:
        failures.append("sungrow_board/boot_slot.cpp: expected exactly one esp_ota_set_boot_partition() call")
    # The dump code uses read APIs.
    ft = strip_comments((COMP / "flash_tools.cpp").read_text())
    for need in ("esp_flash_read(", "esp_partition_read("):
        if need not in ft:
            failures.append(f"flash_tools.cpp: expected {need}")
    for f in failures:
        print("FAIL", f)
    print(f"{len(files)} files scanned, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
