/*
 * adcli
 *
 * Copyright (C) 2012 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Author: Stef Walter <stefw@gnome.org>
 */

#include "config.h"

#include "adcli.h"
#include "adprivate.h"

#include <gssapi/gssapi_krb5.h>
#include <krb5/krb5.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>

krb5_error_code
_adcli_krb5_keytab_clear (krb5_context k5,
                          krb5_keytab keytab,
                          krb5_boolean (* match_func) (krb5_context,
                                                       krb5_keytab_entry *,
                                                       void *),
                          void *match_data)
{
	krb5_kt_cursor cursor;
	krb5_keytab_entry entry;
	krb5_error_code code;

	code = krb5_kt_start_seq_get (k5, keytab, &cursor);
	if (code == KRB5_KT_END || code == ENOENT)
		return 0;
	else if (code != 0)
		return code;

	for (;;) {
		code = krb5_kt_next_entry (k5, keytab, &entry, &cursor);
		if (code != 0)
			break;

		/* See if we should remove this entry */
		if (!match_func (k5, &entry, match_data)) {
			krb5_free_keytab_entry_contents (k5, &entry);
			continue;
		}

		/*
		 * Here we close the cursor, remove the entry and then
		 * start all over again from the beginning. Dumb but works.
		 */

		code = krb5_kt_end_seq_get (k5, keytab, &cursor);
		return_val_if_fail (code == 0, code);

		code = krb5_kt_remove_entry (k5, keytab, &entry);
		krb5_free_keytab_entry_contents (k5, &entry);

		if (code != 0)
			return code;

		code = krb5_kt_start_seq_get (k5, keytab, &cursor);
		return_val_if_fail (code == 0, code);
	}

	if (code == KRB5_KT_END)
		code = 0;

	krb5_kt_end_seq_get (k5, keytab, &cursor);
	return code;
}

static krb5_boolean
match_all_entries (krb5_context k5,
                   krb5_keytab_entry *entry,
                   void *data)
{
	return TRUE;
}

krb5_error_code
_adcli_krb5_keytab_clear_all (krb5_context k5,
                              krb5_keytab keytab)
{
	return _adcli_krb5_keytab_clear (k5, keytab,
	                                 match_all_entries,
	                                 NULL);
}

krb5_error_code
_adcli_krb5_keytab_add_entries (krb5_context k5,
                                krb5_keytab keytab,
                                krb5_principal principal,
                                krb5_kvno kvno,
                                krb5_data *password,
                                krb5_enctype *enctypes,
                                krb5_data *salt)
{
	krb5_keytab_entry entry;
	krb5_error_code code;
	int i;

	for (i = 0; enctypes[i] != 0; i++) {
		memset (&entry, 0, sizeof(entry));

		code = krb5_c_string_to_key (k5, enctypes[i], password, salt, &entry.key);
		if (code != 0)
			return code;

		entry.principal = principal;
		entry.vno = kvno;

		code = krb5_kt_add_entry (k5, keytab, &entry);

		entry.principal = NULL;
		krb5_free_keytab_entry_contents (k5, &entry);

		if (code != 0)
			return code;
	}

	return 0;
}

krb5_error_code
_adcli_krb5_keytab_test_salt (krb5_context k5,
                              krb5_keytab scratch,
                              krb5_principal principal,
                              krb5_kvno kvno,
                              krb5_data *password,
                              krb5_enctype *enctypes,
                              krb5_data *salt)
{
	krb5_error_code code;
	krb5_creds creds;

	code = _adcli_krb5_keytab_clear_all (k5, scratch);
	return_val_if_fail (code == 0, code);

	code = _adcli_krb5_keytab_add_entries (k5, scratch, principal, kvno,
	                                       password, enctypes, salt);
	return_val_if_fail (code == 0, code);

	memset(&creds, 0, sizeof (creds));
	code = krb5_get_init_creds_keytab (k5, &creds, principal, scratch, 0, NULL, NULL);

	krb5_free_cred_contents (k5, &creds);

	return code;
}

krb5_error_code
_adcli_krb5_keytab_discover_salt (krb5_context k5,
                                  krb5_principal principal,
                                  krb5_kvno kvno,
                                  krb5_data *password,
                                  krb5_enctype *enctypes,
                                  krb5_data *salts,
                                  int *discovered)
{
	krb5_keytab scratch;
	krb5_error_code code;
	int i;

	/* TODO: This should be a unique name */

	code = krb5_kt_resolve (k5, "MEMORY:adcli-discover-salt", &scratch);
	return_val_if_fail (code == 0, code);

	for (i = 0; salts[i].data != NULL; i++) {
		code = _adcli_krb5_keytab_test_salt (k5, scratch, principal, kvno,
		                                     password, enctypes, &salts[i]);
		if (code == 0) {
			*discovered = i;
			break;
		} else if (code != KRB5_PREAUTH_FAILED && code != KRB5KDC_ERR_PREAUTH_FAILED) {
			break;
		}
	}

	krb5_kt_close (k5, scratch);
	return code;
}

krb5_error_code
_adcli_krb5_w2k3_salt (krb5_context k5,
                       krb5_principal principal,
                       const char *host_netbios,
                       krb5_data *salt)
{
	krb5_data *realm;
	size_t size = 0;
	size_t host_length = 0;
	size_t at = 0;
	int i;

	/*
	 * The format for the w2k3 computer account salt is:
	 * REALM | "host" | SAM-Account-Name-Without-$ | "." | realm
	 */

	realm = krb5_princ_realm (k5, principal);
	host_length = strlen (host_netbios);

	size += realm->length;
	size += 4; /* "host" */
	size += host_length;
	size += 1; /* "." */
	size += realm->length;

	salt->data = malloc (size);
	return_val_if_fail (salt->data != NULL, ENOMEM);

	/* Upper case realm */
	for (i = 0; i < realm->length; i++)
		salt->data[at + i] = toupper (realm->data[i]);
	at += realm->length;

	/* The string "host" */
	memcpy (salt->data + at, "host", 4);
	at += 4;

	/* The netbios name in lower case */
	for (i = 0; i < host_length; i++)
		salt->data[at + i] = tolower (host_netbios[i]);
	at += host_length;

	/* The dot */
	memcpy (salt->data + at, ".", 1);
	at += 1;

	/* Lower case realm */
	for (i = 0; i < realm->length; i++)
		salt->data[at + i] = tolower (realm->data[i]);
	at += realm->length;

	assert (at == size);
	salt->length = size;
	return 0;
}