// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/efi.h>
#include <linux/slab.h>
#include <keys/asymmetric-type.h>
#include <keys/system_keyring.h>
#include "../integrity.h"
#include "keyring_handler.h"

/*
 * On T2 Macs reading the db and dbx efi variables to load UEFI Secure Boot
 * certificates causes occurrence of a page fault in Apple's firmware and
 * a crash disabling EFI runtime services. The following quirk skips reading
 * these variables.
 */
static const struct dmi_system_id uefi_skip_cert[] = {
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro15,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro15,2") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro15,3") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro15,4") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro16,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro16,2") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro16,3") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookPro16,4") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookAir8,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookAir8,2") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacBookAir9,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "Macmini8,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "MacPro7,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "iMac20,1") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "iMac20,2") },
	{ UEFI_QUIRK_SKIP_CERT("Apple Inc.", "iMacPro1,1") },
	{ }
};

/*
 * Look to see if a UEFI variable called MokIgnoreDB exists and return true if
 * it does.
 *
 * This UEFI variable is set by the shim if a user tells the shim to not use
 * the certs/hashes in the UEFI db variable for verification purposes.  If it
 * is set, we should ignore the db variable also and the true return indicates
 * this.
 */
static __init bool uefi_check_ignore_db(void)
{
	efi_status_t status;
	unsigned int db = 0;
	unsigned long size = sizeof(db);
	efi_guid_t guid = EFI_SHIM_LOCK_GUID;

	status = efi.get_variable(L"MokIgnoreDB", &guid, NULL, &size, &db);
	return status == EFI_SUCCESS;
}

/*
 * Get a certificate list blob from the named EFI variable.
 */
static __init void *get_cert_list(efi_char16_t *name, efi_guid_t *guid,
				  unsigned long *size, efi_status_t *status)
{
	unsigned long lsize = 4;
	unsigned long tmpdb[4];
	void *db;

	*status = efi.get_variable(name, guid, NULL, &lsize, &tmpdb);
	if (*status == EFI_NOT_FOUND)
		return NULL;

	if (*status != EFI_BUFFER_TOO_SMALL) {
		pr_err("Couldn't get size: 0x%lx\n", *status);
		return NULL;
	}

	db = kmalloc(lsize, GFP_KERNEL);
	if (!db)
		return NULL;

	*status = efi.get_variable(name, guid, NULL, &lsize, db);
	if (*status != EFI_SUCCESS) {
		kfree(db);
		pr_err("Error reading db var: 0x%lx\n", *status);
		return NULL;
	}

	*size = lsize;
	return db;
}

/*
 * Load the certs contained in the UEFI databases into the platform trusted
 * keyring and the UEFI blacklisted X.509 cert SHA256 hashes into the blacklist
 * keyring.
 */
static int __init load_uefi_certs(void)
{
	efi_guid_t secure_var = EFI_IMAGE_SECURITY_DATABASE_GUID;
	efi_guid_t mok_var = EFI_SHIM_LOCK_GUID;
	void *db = NULL, *dbx = NULL, *mok = NULL;
	unsigned long dbsize = 0, dbxsize = 0, moksize = 0;
	efi_status_t status;
	int rc = 0;
	const struct dmi_system_id *dmi_id;

	dmi_id = dmi_first_match(uefi_skip_cert);
	if (dmi_id) {
		pr_err("Reading UEFI Secure Boot Certs is not supported on T2 Macs.\n");
		return false;
	}

	if (!efi.get_variable)
		return false;

	/* Get db, MokListRT, and dbx.  They might not exist, so it isn't
	 * an error if we can't get them.
	 */
	if (!uefi_check_ignore_db()) {
		db = get_cert_list(L"db", &secure_var, &dbsize, &status);
		if (!db) {
			if (status == EFI_NOT_FOUND)
				pr_debug("MODSIGN: db variable wasn't found\n");
			else
				pr_err("MODSIGN: Couldn't get UEFI db list\n");
		} else {
			rc = parse_efi_signature_list("UEFI:db",
					db, dbsize, get_handler_for_db);
			if (rc)
				pr_err("Couldn't parse db signatures: %d\n",
				       rc);
			kfree(db);
		}
	}

	mok = get_cert_list(L"MokListRT", &mok_var, &moksize, &status);
	if (!mok) {
		if (status == EFI_NOT_FOUND)
			pr_debug("MokListRT variable wasn't found\n");
		else
			pr_info("Couldn't get UEFI MokListRT\n");
	} else {
		rc = parse_efi_signature_list("UEFI:MokListRT",
					      mok, moksize, get_handler_for_db);
		if (rc)
			pr_err("Couldn't parse MokListRT signatures: %d\n", rc);
		kfree(mok);
	}

	dbx = get_cert_list(L"dbx", &secure_var, &dbxsize, &status);
	if (!dbx) {
		if (status == EFI_NOT_FOUND)
			pr_debug("dbx variable wasn't found\n");
		else
			pr_info("Couldn't get UEFI dbx list\n");
	} else {
		rc = parse_efi_signature_list("UEFI:dbx",
					      dbx, dbxsize,
					      get_handler_for_dbx);
		if (rc)
			pr_err("Couldn't parse dbx signatures: %d\n", rc);
		kfree(dbx);
	}

	return rc;
}
late_initcall(load_uefi_certs);
