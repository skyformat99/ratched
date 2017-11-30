/**
 *	ratched - TLS connection router that performs a man-in-the-middle attack
 *	Copyright (C) 2017-2017 Johannes Bauer
 *
 *	This file is part of ratched.
 *
 *	ratched is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; this program is ONLY licensed under
 *	version 3 of the License, later versions are explicitly excluded.
 *
 *	ratched is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with ratched; if not, write to the Free Software
 *	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *	Johannes Bauer <JohannesBauer@gmx.de>
**/

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>

#include "pgmopts.h"
#include "certforgery.h"
#include "openssl_certs.h"
#include "ipfwd.h"
#include "tools.h"

#define MAX_PATH_LEN		1024

struct server_certificate_t {
	const char *hostname;
	uint32_t ipv4_nbo;
	X509 *certificate;
};

static X509 *root_ca;
static EVP_PKEY *root_ca_key;
static EVP_PKEY *server_key;
static EVP_PKEY *client_key;
static unsigned int server_certificate_cnt;
static struct server_certificate_t *server_certificates;

static struct server_certificate_t* get_server_certificate(const char *hostname, uint32_t ipv4_nbo) {
	if (!hostname) {
		/* IP only search */
		for (int i = 0; i < server_certificate_cnt; i++) {
			if ((!server_certificates[i].hostname) && (ipv4_nbo == server_certificates[i].ipv4_nbo)) {
				return &server_certificates[i];
			}
		}
	} else {
		/* Hostname + IP search */
		for (int i = 0; i < server_certificate_cnt; i++) {
			if (server_certificates[i].hostname && (!strcmp(hostname, server_certificates[i].hostname)) && (ipv4_nbo == server_certificates[i].ipv4_nbo)) {
				return &server_certificates[i];
			}
		}
	}
	return NULL;
}

static struct server_certificate_t* add_server_certificate(const char *hostname, uint32_t ipv4_nbo, X509 *cert) {
	struct server_certificate_t *new_server_certificates = realloc(server_certificates, sizeof(struct server_certificate_t) * (server_certificate_cnt + 1));
	if (!new_server_certificates) {
		logmsg(LLVL_FATAL, "Failed to realloc(3) server_certificates: %s", strerror(errno));
		return NULL;
	}
	server_certificates = new_server_certificates;
	memset(&server_certificates[server_certificate_cnt], 0, sizeof(struct server_certificate_t));
	if (hostname) {
		server_certificates[server_certificate_cnt].hostname = strdup(hostname);
	}
	server_certificates[server_certificate_cnt].ipv4_nbo = ipv4_nbo;
	server_certificates[server_certificate_cnt].certificate = cert;
	server_certificate_cnt += 1;
	return &server_certificates[server_certificate_cnt - 1];
}

static bool get_config_filename(char filename[static MAX_PATH_LEN], const char *suffix) {
	return strxcat(filename, MAX_PATH_LEN, pgm_options->config_dir, "/", suffix, NULL);
}

static bool get_root_ca_filename(char filename[static MAX_PATH_LEN]) {
	return get_config_filename(filename, "root.crt");
}

static bool get_root_key_filename(char filename[static MAX_PATH_LEN]) {
	return get_config_filename(filename, "root.key");
}

static bool get_server_key_filename(char filename[static MAX_PATH_LEN]) {
	return get_config_filename(filename, "server.key");
}

static bool get_client_key_filename(char filename[static MAX_PATH_LEN]) {
	return get_config_filename(filename, "client.key");
}

static void fill_pgmopts_keyspec(struct keyspec_t *keyspec) {
	if (pgm_options->keyspec.keytype == KEYTYPE_RSA) {
		keyspec->cryptosystem = CRYPTOSYSTEM_RSA;
		keyspec->rsa.bitlength = pgm_options->keyspec.rsa.modulus_length_bits;
	} else if (pgm_options->keyspec.keytype == KEYTYPE_ECC) {
		keyspec->cryptosystem = CRYPTOSYSTEM_ECC_FP;
		keyspec->ecc_fp.curve_name = pgm_options->keyspec.ecc.curvename;
	} else {
		logmsg(LLVL_FATAL, "Unknown keyspec.keytype given in program options (0x%x)", pgm_options->keyspec.keytype);
	}
}

bool certforgery_init(void) {
	makedirs(pgm_options->config_dir);
	{
		struct keyspec_t keyspec = {
			.description = "root",
		};
		fill_pgmopts_keyspec(&keyspec);
		char filename[MAX_PATH_LEN];
		if (get_root_key_filename(filename)) {
			root_ca_key = openssl_load_stored_key(&keyspec, filename);
			if (!root_ca_key) {
				logmsg(LLVL_FATAL, "Unable to load or create root CA private keypair.");
				return false;
			}
		} else {
			logmsg(LLVL_FATAL, "Could not get root CA private key filename.");
			return false;
		}
	}
	{
		struct keyspec_t keyspec = {
			.description = "TLS server",
		};
		fill_pgmopts_keyspec(&keyspec);
		char filename[MAX_PATH_LEN];
		if (get_server_key_filename(filename)) {
			server_key = openssl_load_stored_key(&keyspec, filename);
			if (!server_key) {
				logmsg(LLVL_FATAL, "Unable to load or create server private keypair.");
				return false;
			}
		} else {
			logmsg(LLVL_FATAL, "Could not get server private key filename.");
			return false;
		}
	}
	{
		struct keyspec_t keyspec = {
			.description = "TLS client",
		};
		fill_pgmopts_keyspec(&keyspec);
		char filename[MAX_PATH_LEN];
		if (get_client_key_filename(filename)) {
			client_key = openssl_load_stored_key(&keyspec, filename);
			if (!client_key) {
				logmsg(LLVL_FATAL, "Unable to load or create client private keypair.");
				return false;
			}
		} else {
			logmsg(LLVL_FATAL, "Could not get client private key filename.");
			return false;
		}
	}
	{
		struct certificatespec_t certspec = {
			.description = "root",
			.subject_pubkey = root_ca_key,
			.issuer_privkey = root_ca_key,
			.common_name = "Evil root certificate",
			.mark_certificate = pgm_options->forged_certs.mark_forged_certificates,
			.is_ca_certificate = true,
			.validity_predate_seconds = 86400,
			.validity_seconds = 86400 * 365 * 5,
		};
		char filename[MAX_PATH_LEN];
		if (get_root_ca_filename(filename)) {
			root_ca = openssl_load_stored_certificate(&certspec, filename, true, true);
		} else {
			logmsg(LLVL_FATAL, "Could not get root CA filename.");
			return false;
		}
		log_cert(LLVL_DEBUG, root_ca, "Used root certificate");
	}
	return true;
}

X509 *get_forged_root_certificate(void) {
	X509_up_ref(root_ca);
	return root_ca;
}

EVP_PKEY *get_forged_root_key(void) {
	EVP_PKEY_up_ref(root_ca_key);
	return root_ca_key;
}

EVP_PKEY *get_tls_server_key(void) {
	EVP_PKEY_up_ref(server_key);
	return server_key;
}

EVP_PKEY *get_tls_client_key(void) {
	EVP_PKEY_up_ref(client_key);
	return client_key;
}

X509 *forge_certificate_for_server(const char *hostname, uint32_t ipv4_nbo) {
	struct server_certificate_t* entry = get_server_certificate(hostname, ipv4_nbo);
	if (!entry) {
		if (hostname) {
			logmsg(LLVL_DEBUG, "Forging certificate for %s (" PRI_IPv4 ")", hostname, FMT_IPv4(ipv4_nbo));
		} else {
			logmsg(LLVL_DEBUG, "Forging certificate for " PRI_IPv4, FMT_IPv4(ipv4_nbo));
		}
		char ipv4[16];
		struct certificatespec_t certspec = {
			.description = "TLS server",
			.subject_pubkey = server_key,
			.issuer_privkey = root_ca_key,
			.issuer_certificate = root_ca,
			.mark_certificate = pgm_options->forged_certs.mark_forged_certificates,
			.subject_alternative_ipv4_address = ipv4_nbo,
			.is_ca_certificate = false,
			.validity_predate_seconds = 86400,
			.validity_seconds = 86400 * 365 * 1,
			.crl_uri = pgm_options->forged_certs.crl_uri,
			.ocsp_responder_uri = pgm_options->forged_certs.ocsp_responder_uri,
		};
		if (hostname) {
			certspec.subject_alternative_dns_hostname = hostname;
			certspec.common_name = hostname;
		} else {
			snprintf(ipv4, sizeof(ipv4), PRI_IPv4, FMT_IPv4(ipv4_nbo));
			certspec.common_name = ipv4;
		}
		X509 *cert = openssl_create_certificate(&certspec);
		if (cert) {
			entry = add_server_certificate(hostname, ipv4_nbo, cert);
			if (!entry) {
				logmsg(LLVL_ERROR, "Failed to add server certificate for %s to database.", hostname);
				X509_free(cert);
			} else if (pgm_options->log.dump_certificates) {
				log_cert(LLVL_DEBUG, entry->certificate, "Created forged server certificate");
			}
		}
	}

	if (!entry) {
		logmsg(LLVL_FATAL, "Could not create server certificate.");
		return NULL;
	}
	X509_up_ref(entry->certificate);
	return entry->certificate;
}

void certforgery_deinit(void) {
	X509_free(root_ca);
	EVP_PKEY_free(root_ca_key);
	EVP_PKEY_free(server_key);
	EVP_PKEY_free(client_key);
}
