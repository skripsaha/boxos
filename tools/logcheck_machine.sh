# shellcheck shell=bash
# Sourced by tools/logcheck.sh: the machine scenario.

# ---------------------------------------------------------------------------
# machine: what the machine calls itself, read from its firmware's SMBIOS
# table (drivers/smbios; boxlib machine_info, machine_shown; utility hw).
#
# QEMU is given the strings a firmware gives (-smbios), and the machine is
# started three times:
#   uefi       through OVMF, as the Zenbook's firmware names it: the maker
#              ASUSTeK, the product UX303LN, the family UX — read from
#              Gangway's copy of the table, and told as "ASUS UX303LN";
#   bios       through SeaBIOS, as a ThinkPad's names it: the maker LENOVO,
#              the product its machine type, the model in its version — the
#              entry point found in the F-segment, told as "Lenovo ThinkPad";
#   board      through OVMF, a desktop whose firmware left its system
#              strings as placeholders ("OEM", "N/A") and named its board:
#              the placeholders read as nothing, and the machine is told by
#              its board, "Gigabyte B365M".
# Each boot says the table it read; each `hw` says the machine first; no
# boot panics or faults.
# ---------------------------------------------------------------------------

# machine_boot TAG UEFI SMBIOS — the machine run to the shell with the SMBIOS
# strings SMBIOS, `hw` typed, the halt waited out; its log kept as
# serial.machine.TAG.log.
machine_boot() {
    local tag=$1 uefi=$2 smbios=$3 rc=1
    make run-stop >/dev/null 2>&1
    if make run-bg UEFI="$uefi" CORES=4 MEM=2G QEMUEXTRA="$smbios" >"$SCRATCH/machine.$tag.run-bg" 2>&1; then
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" && wait_for_prompt &&
            type_line "hw" && wait_for_prompt &&
            type_line "bye" && wait_for_line -E build/serial.log "$STAND_HALT_ENDS" "$WAIT_QUIET_S" && rc=0
    else
        harness "machine ($tag): run-bg would not start: $(tail -1 "$SCRATCH/machine.$tag.run-bg")"
    fi
    sleep 1
    make run-stop >/dev/null 2>&1
    tr -d '\r' < build/serial.log >"$SCRATCH/serial.machine.$tag.log"
    [ $rc = 0 ]
    chk $? "machine ($tag): the machine starts, hw answers, and the halt ends"
}

# machine_said TAG PATTERN WHAT — the log of TAG says PATTERN (grep -E).
machine_said() {
    grep -aqE "$2" "$SCRATCH/serial.machine.$1.log"
    chk $? "machine ($1): $3"
}

run_machine() {
    echo "== machine: what the machine calls itself, from its firmware's SMBIOS table =="
    build

    machine_boot uefi on "-smbios type=1,manufacturer=ASUSTeK,product=UX303LN,version=1.0,family=UX -smbios type=2,manufacturer=ASUSTeK,product=UX303LN"
    machine_said uefi '^Gangway: SMBIOS [0-9]+\.[0-9]+ copied to ACPI_NVS \([0-9]+ bytes\)' \
        "Gangway copies the table out of the firmware's memory before its boot services end"
    machine_said uefi '^\[SMBIOS\] [0-9]+\.[0-9]+: the machine calls itself "ASUSTeK" "UX303LN" \(version "1\.0", family "UX"\)' \
        "the kernel reads System Information off the copy"
    machine_said uefi '^Machine: ASUS UX303LN \(SMBIOS ' "hw tells the machine by its maker's brand and its product"

    machine_boot bios off "-smbios type=1,manufacturer=LENOVO,product=20HRCTO1WW,version=ThinkPad"
    machine_said bios '^\[SMBIOS\] [0-9]+\.[0-9]+: the machine calls itself "LENOVO" "20HRCTO1WW" \(version "ThinkPad"' \
        "a BIOS start finds the entry point in the F-segment and reads the table where it lies"
    machine_said bios '^Machine: Lenovo ThinkPad \(SMBIOS ' "a Lenovo is told by the model it keeps in its version"

    machine_boot board on "-smbios type=1,manufacturer=OEM,product=N/A,version=x.x -smbios type=2,manufacturer=Gigabyte,product=B365M"
    machine_said board '^\[SMBIOS\] [0-9]+\.[0-9]+: the machine calls itself "" "" \(version "", family ""\), its board "Gigabyte" "B365M"' \
        "a firmware's placeholders read as nothing"
    machine_said board '^Machine: Gigabyte B365M \(SMBIOS ' "and the machine is told by its board"

    ! grep -aqE 'KERNEL PANIC|\[EXCEPTION\]|DEFECT' "$SCRATCH"/serial.machine.*.log
    chk $? "machine: no panic, no fault, no defect said"
}
