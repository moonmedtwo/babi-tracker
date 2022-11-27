/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <stdlib.h>
#include <zephyr/net/socket.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/net/tls_credentials.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <net/rest_client.h>
#include "commit_hash.h"

#define HTTPS_PORT 443

#define HTTPS_HOSTNAME "consumer-dev.itspersonalservices.com"

#define RECV_BUF_SIZE 2048
#define TLS_SEC_TAG 42

static char recv_buf[RECV_BUF_SIZE];

/* Certificate for `example.com` */
static const char cert[] = {
	#include "../cert/DigiCertGlobalRootCA.pem"
};

BUILD_ASSERT(sizeof(cert) < KB(4), "Certificate too large");

/* Provision certificate to modem */
int cert_provision(void)
{
	int err;
	bool exists;
	int mismatch;

	/* It may be sufficient for you application to check whether the correct
	 * certificate is provisioned with a given tag directly using modem_key_mgmt_cmp().
	 * Here, for the sake of the completeness, we check that a certificate exists
	 * before comparing it with what we expect it to be.
	 */
	err = modem_key_mgmt_exists(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
	if (err) {
		printk("Failed to check for certificates err %d\n", err);
		return err;
	}

	if (exists) {
		mismatch = modem_key_mgmt_cmp(TLS_SEC_TAG,
					      MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					      cert, strlen(cert));
		if (!mismatch) {
			printk("Certificate match\n");
			return 0;
		}

		printk("Certificate mismatch\n");
		err = modem_key_mgmt_delete(TLS_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
		if (err) {
			printk("Failed to delete existing certificate, err %d\n", err);
		}
	}

	printk("Provisioning certificate\n");

	/*  Provision certificate to the modem */
	err = modem_key_mgmt_write(TLS_SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   cert, sizeof(cert) - 1);
	if (err) {
		printk("Failed to provision certificate, err %d\n", err);
		return err;
	}

	return 0;
}

/* Setup TLS options on a given socket */
int tls_setup(int fd)
{
	int err;
	int verify;

	/* Security tag that we have provisioned the certificate with */
	const sec_tag_t tls_sec_tag[] = {
		TLS_SEC_TAG,
	};

#if defined(CONFIG_SAMPLE_TFM_MBEDTLS)
	err = tls_credential_add(tls_sec_tag[0], TLS_CREDENTIAL_CA_CERTIFICATE, cert, sizeof(cert));
	if (err) {
		return err;
	}
#endif

	/* Set up TLS peer verification */
	enum {
		NONE = 0,
		OPTIONAL = 1,
		REQUIRED = 2,
	};

	verify = NONE;

	err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
	if (err) {
		printk("Failed to setup peer verification, err %d\n", errno);
		return err;
	}

	/* Associate the socket with the security tag
	 * we have provisioned the certificate with.
	 */
	err = setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag,
			 sizeof(tls_sec_tag));
	if (err) {
		printk("Failed to setup TLS sec tag, err %d\n", errno);
		return err;
	}

	err = setsockopt(fd, SOL_TLS, TLS_HOSTNAME, HTTPS_HOSTNAME, sizeof(HTTPS_HOSTNAME) - 1);
	if (err) {
		printk("Failed to setup TLS hostname, err %d\n", errno);
		return err;
	}
	return 0;
}

K_THREAD_STACK_DEFINE(stack_gnss, 4096);
struct k_thread thread_data_gnss;
#define THREAD_PRIORITY_GNSS 5

extern void entrypoint_gnss(void *arg1, void *arg2, void *arg3);

void create_gnss_thread()
{
    k_tid_t tid_mqtt = k_thread_create(&thread_data_gnss, stack_gnss,
                                 K_THREAD_STACK_SIZEOF(stack_gnss),
                                 entrypoint_gnss,
                                 NULL, NULL, NULL,
                                 THREAD_PRIORITY_GNSS, 0, K_NO_WAIT);
}

void app_main_handler()
{
	const int polling_interval = 60;
	printk("Wake up every %d\n", polling_interval);
	k_sleep(K_SECONDS(polling_interval));
}

void main(void)
{
	create_gnss_thread();
	while (true)	
	{
		app_main_handler();
	}

	int err;
	int fd;
	char *p;
	int bytes;
	size_t off;
	struct addrinfo *res = 0;

	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};

	printk("Build %s\n", HASH);

#if !defined(CONFIG_SAMPLE_TFM_MBEDTLS)
	/* Provision certificates before connecting to the LTE network */
	err = cert_provision();
	if (err) {
		return;
	}
#endif

	printk("Waiting for network.. ");
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
	{
		nrf_modem_at_printf("AT+COPS=1,2\"45202\"");
	}
	else
	{
		err = lte_lc_init_and_connect();
		if (err) {
			printk("Failed to connect to the LTE network, err %d\n", err);
			return;
		}
	}

	printk("OK\n");

	/* Setting google dns to resolve the hostname */
	struct nrf_in_addr dns;
	dns.s_addr = 134744072; // Google DNS, 8.8.8.8
	const int dns_err = nrf_setdnsaddr(NRF_AF_INET, &dns, sizeof(dns));
	printk("DNS set result %d\n", dns_err);

	/* Resolve the hostname into IP address */
	printk("getaddrinfo of %s\n", HTTPS_HOSTNAME);
	err = getaddrinfo(HTTPS_HOSTNAME, NULL, &hints, &res);
	if (err) {
		printk("getaddrinfo() of %s failed, err %d\n",HTTPS_HOSTNAME, errno);
		return;
	}

	((struct sockaddr_in *)res->ai_addr)->sin_port = htons(HTTPS_PORT);
	
	/* Try to open the socket with tls protocol enabled */
	if (IS_ENABLED(CONFIG_SAMPLE_TFM_MBEDTLS)) {
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NATIVE_TLS, IPPROTO_TLS_1_2);
	} else {
		fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	}
	if (fd == -1) {
		printk("Failed to open socket!\n");
		goto clean_up;
	}

	/* Setup TLS socket options */
	err = tls_setup(fd);
	if (err) {
		goto clean_up;
	}

	/* Connecting to the host with provided socket */
	char* ip = &res->ai_addr->data[2];
	printk("Connecting to %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
	err = connect(fd, res->ai_addr, sizeof(struct sockaddr_in));
	if (err) {
		printk("connect() failed, err: %d\n", errno);
		goto clean_up;
	}
	printk("Connected\n");

	struct rest_client_req_context req_ctx = {
		/** Socket identifier for the connection. When using the default value,
		 *  the library will open a new socket connection. Default: REST_CLIENT_SCKT_CONNECT.
		 */
		.connect_socket = fd,
		/** Defines whether the connection should remain after API call. Default: false. */
		.keep_alive = true,
		/** Security tag. Default: REST_CLIENT_SEC_TAG_NO_SEC. */
		.sec_tag = TLS_SEC_TAG,
		/** Indicates the preference for peer verification.
		 *  Initialize to REST_CLIENT_TLS_DEFAULT_PEER_VERIFY
		 *  and the default (TLS_PEER_VERIFY_REQUIRED) is used.
		 */
		.tls_peer_verify = 0, // No build verification
		/** Used HTTP method. */
		.http_method = HTTP_PUT,
		/** Hostname or IP address to be used in the request. */
		.host = HTTPS_HOSTNAME,
		/** Port number to be used in the request. */
		.port = HTTPS_PORT,
		/** The URL for this request, for example: /index.html */
		.url = "/api/v1/devices/battery",
		/** The HTTP header fields. Similar to the Zephyr HTTP client.
		 *  This is a NULL-terminated list of header fields. May be NULL.
		 */
		.header_fields = NULL,
		/** Payload/body, may be NULL. */
		.body = NULL,
		/** User-defined timeout value for REST request. The timeout is set individually
		 *  for socket connection creation and data transfer meaning REST request can take
		 *  longer than this given timeout. To disable, set the timeout duration to SYS_FOREVER_MS.
		 *  A value of zero will result in an immediate timeout.
		 *  Default: CONFIG_REST_CLIENT_REST_REQUEST_TIMEOUT.
		 */
		.timeout_ms = 30000,
		/** User-allocated buffer for receiving API response. */
		.resp_buff = recv_buf,
		/** User-defined size of resp_buff. */
		.resp_buff_len = RECV_BUF_SIZE,
	};
	struct rest_client_resp_context resp;
	memset(&resp, 0, sizeof(resp));

	create_gnss_thread();
	while (true)	
	{
		app_main_handler();
	}

clean_up:
	printk("Finished, closing socket.\n");
	if (res != 0)
	{
		freeaddrinfo(res);
	}
	(void)close(fd);

	lte_lc_power_off();
	printk("Cleaned up, shutting down ...\n");
}
