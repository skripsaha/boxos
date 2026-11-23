#ifndef DRIVERS_SMBIOS_H
#define DRIVERS_SMBIOS_H

#include "ktypes.h"
#include "boxos_system_info.h"

/*
 * What the machine calls itself, read from its firmware's SMBIOS structure
 * table (DMTF DSP0134 3.8.0) once at boot: System Information (type 1), the
 * Baseboard (type 2) and the System Enclosure's type (type 3), into one
 * MachineInfo (boxos_system_info.h) every program may ask for.
 *
 * Through UEFI the table is Gangway's copy in ACPI NVS (boot_info.h
 * smbios_copy_*); through a BIOS the entry point is found on a paragraph of
 * the F-segment (§5.2.1, §5.2.2) and the table read where it lies. A string a
 * vendor left as its placeholder names no machine and reads as empty.
 */
void SmbiosInit(void);

/* What SmbiosInit read; all empty, `from` MACHINE_FROM_NONE, before it or for
 * a firmware that published no table. */
void SmbiosMachine(MachineInfo* out);

#endif /* DRIVERS_SMBIOS_H */
