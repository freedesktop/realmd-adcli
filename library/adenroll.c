
#include "config.h"

#include "adenroll.h"
#include "adprivate.h"

#include <gssapi/gssapi_krb5.h>
#include <krb5/krb5.h>
#include <ldap.h>
#include <sasl/sasl.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>

struct _adcli_enroll {
	int refs;
	adcli_conn *conn;

	char *host_fqdn;
	char *host_netbios;
	char *computer_ou;
#if 0
	char *host_password;
	krb5_keytab host_keytab;
#endif
};

static adcli_result
ensure_host_fqdn (adcli_result res,
                  adcli_enroll *enroll)
{
	const char *fqdn;

	if (res != ADCLI_SUCCESS)
		return res;

	if (enroll->host_fqdn) {
		_adcli_err (enroll->conn, "Using fully qualified name: %s",
		            enroll->host_fqdn);
		return ADCLI_SUCCESS;
	}

	/* By default use our actual host name discovered during connecting */
	fqdn = adcli_conn_get_host_fqdn (enroll->conn);
	return adcli_enroll_set_host_fqdn (enroll, fqdn);
}

static adcli_result
ensure_host_netbios (adcli_result res,
                     adcli_enroll *enroll)
{
	const char *dom;

	if (res != ADCLI_SUCCESS)
		return res;

	if (enroll->host_netbios) {
		_adcli_info (enroll->conn, "Using host netbios name: %s",
		             enroll->host_netbios);
		return ADCLI_SUCCESS;
	}

	assert (enroll->host_fqdn != NULL);

	/* Use the FQDN minus the last part */
	dom = strchr (enroll->host_fqdn, '.');

	/* If no dot, or dot is first or last, then fail */
	if (dom == NULL || dom == enroll->host_fqdn || dom[1] == '\0') {
		_adcli_err (enroll->conn,
		            "Couldn't determine the netbios name from host name: %s",
		            enroll->host_fqdn);
		return ADCLI_ERR_DNS;
	}

	enroll->host_netbios = strndup (enroll->host_fqdn,
	                                dom - enroll->host_fqdn);
	if (enroll->host_netbios) {
		_adcli_strup (enroll->host_netbios);
		_adcli_info (enroll->conn, "Calculated host netbios name from fqdn: %s",
		             enroll->host_netbios);
	}

	return enroll->host_netbios ? ADCLI_SUCCESS : ADCLI_ERR_MEMORY;
}

static adcli_result
validate_computer_ou (adcli_enroll *enroll)
{
	struct berval bv;
	LDAP *ldap;
	int ret;

	assert (enroll->computer_ou != NULL);

	ldap = adcli_conn_get_ldap_connection (enroll->conn);
	assert (ldap != NULL);

	bv.bv_val = enroll->computer_ou;
	bv.bv_len = strlen (enroll->computer_ou);

	ret = ldap_compare_ext_s (ldap, enroll->computer_ou,
	                          "organizationalUnit", &bv, NULL, NULL);

	if (ret == LDAP_COMPARE_FALSE) {
		_adcli_err (enroll->conn,
		            "The computer organizational unit is invalid: %s",
		            enroll->computer_ou);
		return ADCLI_ERR_DNS;

	} else if (ret == LDAP_COMPARE_TRUE) {
		return ADCLI_SUCCESS;

	} else {
		return _adcli_ldap_handle_failure (enroll->conn, ldap,
		                                   "Couldn't check organizational unit",
		                                   enroll->computer_ou, ADCLI_ERR_DNS);
	}
}

static adcli_result
lookup_preferred_computer_ou (adcli_enroll *enroll,
                              LDAP *ldap,
                              const char *base)
{
	char *attrs[] = { "preferredOU", NULL };
	LDAPMessage *results;
	int ret;

	assert (enroll->computer_ou == NULL);

	/*
	 * TODO: The objectClass here is documented, but seems like its wrong.
	 * Needs testing against a domain with the preferredOU attribute.
	 * FWIW, Windows 2003 functional level doesn't have preferredOU.
	 */
	ret = ldap_search_ext_s (ldap, base, LDAP_SCOPE_BASE, "(objectClass=computer)",
	                         attrs, 0, NULL, NULL, NULL, -1, &results);

	if (ret != LDAP_SUCCESS) {
		return _adcli_ldap_handle_failure (enroll->conn, ldap,
		                                   "Couldn't lookup preferred organizational unit",
		                                   NULL, ADCLI_ERR_CONNECTION);
	}

	enroll->computer_ou = _adcli_ldap_parse_value (ldap, results, "preferredOU");

	ldap_msgfree (results);
	return ADCLI_SUCCESS;
}

static adcli_result
lookup_wellknown_computer_ou (adcli_enroll *enroll,
                              LDAP *ldap,
                              const char *base)
{
	char *attrs[] = { "wellKnownObjects", NULL };
	char *prefix = "B:32:AA312825768811D1ADED00C04FD8D5CD:";
	int prefix_len;
	LDAPMessage *results;
	char **values;
	int ret;
	int i;

	assert (enroll->computer_ou == NULL);

	ret = ldap_search_ext_s (ldap, base, LDAP_SCOPE_BASE, "(objectClass=*)",
	                         attrs, 0, NULL, NULL, NULL, -1, &results);

	if (ret != LDAP_SUCCESS) {
		return _adcli_ldap_handle_failure (enroll->conn, ldap,
		                                   "Couldn't lookup well known organizational unit",
		                                   NULL, ADCLI_ERR_CONNECTION);
	}

	values = _adcli_ldap_parse_values (ldap, results, "wellKnownObjects");
	ldap_msgfree (results);

	prefix_len = strlen (prefix);
	for (i = 0; values && values[i]; i++) {
		if (strncmp (values[i], prefix, prefix_len) == 0)
			enroll->computer_ou = strdup (values[i] + prefix_len);
	}

	_adcli_strv_free (values);
	return ADCLI_SUCCESS;
}

static adcli_result
lookup_computer_ou (adcli_enroll *enroll)
{
	adcli_result res;
	const char *base;
	LDAP *ldap;

	assert (enroll->computer_ou == NULL);

	ldap = adcli_conn_get_ldap_connection (enroll->conn);
	assert (ldap != NULL);

	base = adcli_conn_get_naming_context (enroll->conn);
	assert (base != NULL);

	res = lookup_preferred_computer_ou (enroll, ldap, base);
	if (res == ADCLI_SUCCESS && enroll->computer_ou == NULL)
		res = lookup_wellknown_computer_ou (enroll, ldap, base);

	if (res != ADCLI_SUCCESS)
		return res;

	if (enroll->computer_ou == NULL) {
		_adcli_err (enroll->conn, "No preferred organizational unit found");
		return ADCLI_ERR_DNS;
	}

	return res;
}

static void
enroll_clear_state (adcli_enroll *enroll)
{

}

adcli_result
adcli_enroll_join (adcli_enroll *enroll)
{
	adcli_result res = ADCLI_SUCCESS;

	res = adcli_conn_connect (enroll->conn);
	if (res != ADCLI_SUCCESS)
		return res;

	/* Basic discovery and figuring out enroll params */
	res = ensure_host_fqdn (res, enroll);
	res = ensure_host_netbios (res, enroll);

	if (res != ADCLI_SUCCESS)
		return res;

	/* Now we need to find or validate the computer ou */
	if (enroll->computer_ou)
		res = validate_computer_ou (enroll);
	else
		res = lookup_computer_ou (enroll);
	if (res != ADCLI_SUCCESS)
		return res;

	/* TODO: Create a valid password */

	/* - Figure out the domain short name */

	/* - Search for computer account */

	/* - Update computer account or create */

	/* - Write out password to host keytab */

	return res;
}

adcli_enroll *
adcli_enroll_new (adcli_conn *conn)
{
	adcli_enroll *enroll;

	enroll = calloc (1, sizeof (adcli_enroll));
	if (enroll == NULL)
		return NULL;

	enroll->conn = adcli_conn_ref (conn);
	enroll->refs = 1;
	return enroll;
}

adcli_enroll *
adcli_enroll_ref (adcli_enroll *enroll)
{
	enroll->refs++;
	return enroll;
}

static void
enroll_free (adcli_enroll *enroll)
{
	if (enroll == NULL)
		return;

	free (enroll->host_fqdn);
	free (enroll->host_netbios);
	free (enroll->computer_ou);

	enroll_clear_state (enroll);
	adcli_conn_unref (enroll->conn);
	free (enroll);
}

void
adcli_enroll_unref (adcli_enroll *enroll)
{
	if (enroll == NULL)
		return;

	if (--(enroll->refs) > 0)
		return;

	enroll_free (enroll);
}

const char *
adcli_enroll_get_host_fqdn (adcli_enroll *enroll)
{
	return enroll->host_fqdn;
}

adcli_result
adcli_enroll_set_host_fqdn (adcli_enroll *enroll,
                            const char *value)
{
	return _adcli_set_str_field (&enroll->host_fqdn, value);
}

const char *
adcli_enroll_get_host_netbios (adcli_enroll *enroll)
{
	return enroll->host_netbios;
}

adcli_result
adcli_enroll_set_host_netbios (adcli_enroll *enroll,
                               const char *value)
{
	return _adcli_set_str_field (&enroll->host_netbios, value);
}

const char *
adcli_enroll_get_computer_ou (adcli_enroll *enroll)
{
	return enroll->computer_ou;
}

adcli_result
adcli_enroll_set_computer_ou (adcli_enroll *enroll,
                              const char *value)
{
	return _adcli_set_str_field (&enroll->computer_ou, value);
}
