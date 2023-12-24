/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  fw_tables.h - Parsing support for ACPI and ACPI-like tables provided by
 *                platform or device firmware
 *
 *  Copyright (C) 2001 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2023 Intel Corp.
 */
#ifndef _FW_TABLE_H_
#define _FW_TABLE_H_

union acpi_subtable_headers;

typedef int (*acpi_tbl_entry_handler)(union acpi_subtable_headers *header,
				      const unsigned long end);

typedef int (*acpi_tbl_entry_handler_arg)(union acpi_subtable_headers *header,
					  void *arg, const unsigned long end);

struct acpi_subtable_proc {
	int id;
	acpi_tbl_entry_handler handler;
	acpi_tbl_entry_handler_arg handler_arg;
	void *arg;
	int count;
};

union acpi_subtable_headers {
	struct acpi_subtable_header common;
	struct acpi_hmat_structure hmat;
	struct acpi_prmt_module_header prmt;
	struct acpi_cedt_header cedt;
};

int acpi_parse_entries_array(char *id, unsigned long table_size,
			     struct acpi_table_header *table_header,
			     struct acpi_subtable_proc *proc,
			     int proc_num, unsigned int max_entries);

#endif
