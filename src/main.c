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
#include <cJSON.h>

LOG_MODULE_REGISTER(App, 3);

#define ENDPOINT_BATTERY "/api/v1/devices/battery"
#define ENDPOINT_LOCATION "/api/v1/devices/location"

#define HTTPS_PORT 443

#define HTTPS_HOSTNAME "consumer-dev.itspersonalservices.com"

#define RECV_BUF_SIZE 2048
#define TLS_SEC_TAG 42

static char recv_buf[RECV_BUF_SIZE];
static char DEV_UUID[40];
static char MODEM_FW_VER[50];

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

K_SEM_DEFINE(lte_ready, 0, 1);

volatile bool lte_connected = false;

static void lte_lc_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Connected to LTE network");
            lte_connected = true;
			k_sem_give(&lte_ready);
		}
		break;

	default:
		break;
	}
}

void lte_connect(void)
{
    if (lte_connected == true)
    {
        return;
    }

	int err;

	LOG_INF("Connecting to LTE network");

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("Failed to activate LTE, error: %d", err);
		return;
	}

	if(k_sem_take(&lte_ready, K_SECONDS(10)) != 0)
    {
        LOG_ERR("Connecting timeout");
    }
    else
    {
	    /* Wait for a while, because with IPv4v6 PDN the IPv6 activation takes a bit more time. */
	    k_sleep(K_SECONDS(1));
    }
}

void lte_disconnect(void)
{
	int err;

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE);
	if (err) {
		LOG_ERR("Failed to deactivate LTE, error: %d", err);
		return;
	}

    lte_connected = false;
	LOG_INF("LTE disconnected");
}

extern volatile double last_latitude;
extern volatile double last_longtitude;

static uint8_t msg_buffer[128];

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

void app_main_handler(int socket)
{
    struct rest_client_req_context req_ctx = {
        /** Socket identifier for the connection. When using the default value,
         *  the library will open a new socket connection. Default: REST_CLIENT_SCKT_CONNECT.
         */
        .connect_socket = socket,
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
        .body = msg_buffer,
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
    struct rest_client_resp_context resp_ctx;
    memset(&resp_ctx, 0, sizeof(resp_ctx));

    const int polling_interval = 60;
    while (1)
    {
        LOG_INF("Do scheduled job ...");

        cJSON* sensorDataObj = cJSON_CreateObject(); cJSON_AddItemToObject(sensorDataObj, "uuid", cJSON_CreateString(DEV_UUID));
        cJSON_AddItemToObject(sensorDataObj, "bat", cJSON_CreateNumber(1.0));
        cJSON_PrintPreallocated(sensorDataObj, msg_buffer, sizeof(msg_buffer), false);
        int err = -1;
        req_ctx.url = ENDPOINT_BATTERY;
        err = rest_client_request(&req_ctx, &resp_ctx);
        if(err == 0)
        {
            LOG_INF("Put msg [%s] to endpoint \"%s\" succeeded", msg_buffer, req_ctx.url);
        }
        else
        {
            LOG_ERR("Put msg [%s] to endpoint \"%s\" failed with err %d", msg_buffer, req_ctx.url, err);
            if(resp_ctx.response != NULL)
            {
                LOG_INF("Response: %s", resp_ctx.response);
            }
        }

        memset(&resp_ctx, 0, sizeof(resp_ctx));

        cJSON_DetachItemFromObject(sensorDataObj, "bat");
        cJSON_AddItemToObject(sensorDataObj, "lat", cJSON_CreateNumber(last_latitude));
        cJSON_AddItemToObject(sensorDataObj, "long", cJSON_CreateNumber(last_longtitude));
        cJSON_PrintPreallocated(sensorDataObj, msg_buffer, sizeof(msg_buffer), false);
        req_ctx.url = ENDPOINT_LOCATION;
        err = rest_client_request(&req_ctx, &resp_ctx);
        if(err == 0)
        {
            LOG_INF("Put msg [%s] to endpoint \"%s\" succeeded", msg_buffer, req_ctx.url);
        }
        else
        {
            LOG_ERR("Put msg [%s] to endpoint \"%s\" failed with err %d", msg_buffer, req_ctx.url, err);
            if(resp_ctx.response != NULL)
            {
                LOG_INF("Response: %s", resp_ctx.response);
            }
        }
        cJSON_Delete(sensorDataObj);
        k_sleep(K_SECONDS(polling_interval));
    }
}

void main(void)
{
    int err;
    int fd;
    char *p;
    int bytes;
    size_t off;

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    LOG_INF("Build %s", HASH);
    LOG_INF("Using APN: %s", CONFIG_PDN_DEFAULT_APN);
    int ret_scanf = nrf_modem_at_scanf("AT+CGMR", "%s", &DEV_UUID);

#if !defined(CONFIG_SAMPLE_TFM_MBEDTLS)
    /* Provision certificates before connecting to the LTE network */
    err = cert_provision();
    if (err) {
        return;
    }
#endif

    LOG_INF("Waiting for network...");
    if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
    {
        LOG_INF("Enabled CONFIG_PDN_DEFAULT_APN");
        nrf_modem_at_printf("AT+COPS=1,2\"45202\"");
    }
    else
    {
        if (lte_lc_init() != 0) {
            LOG_ERR("Failed to initialize LTE link controller");
            return -1;
        }

        lte_lc_register_handler(lte_lc_event_handler);
        lte_lc_psm_req(true);
    }

    ret_scanf = nrf_modem_at_scanf("AT%XMODEMUUID", "%%XMODEMUUID: %s", &DEV_UUID);
    if (ret_scanf != 1)
    {
        LOG_ERR("Failed to read device uuid");
        return -1;
    }
    LOG_INF("Device uuid: %s", DEV_UUID);

    create_gnss_thread();

    while ((fabs(last_latitude - 0) < 0.5) 
        && (fabs(last_longtitude - 0) < 0.5))
    {
        LOG_INF("Waiting for first GPS fix ...");
        k_sleep(K_SECONDS(10));
    }

    lte_connect();

    struct dns_server_lookup {
        uint32_t addr;
        uint8_t addr_u8[4];
    };

    struct dns_server_lookup dns_servers[] =
    {
        // Google DNS primary
        {
            .addr = 134744072,
            .addr_u8 = {8,8,8,8}
        },
        // Google DNS secondary
        {
            .addr = 134743044,
            .addr_u8 = {8,8,4,4}
        },
        // OpenDNS primary
        {
            .addr = 3494108894,
            .addr_u8 = {208,67,222,222}
        },
        // OpenDNS secondary
        {
            .addr = 3494108894,
            .addr_u8 = {208,67,222,222}
        },
        {
            .addr = 0,
            .addr_u8 = {0,0,0,}
        }
    };

    err = -1;
    int dns_idx = 0;

    struct addrinfo *res = 0;
    while (err != 0 && dns_servers[dns_idx].addr != 0)
    {
        uint8_t * p_ipu8 = dns_servers[dns_idx].addr_u8;
        LOG_INF("Resolving %s using DNS server %d.%d.%d.%d", HTTPS_HOSTNAME, p_ipu8[0],  p_ipu8[1],  p_ipu8[2],  p_ipu8[3]);
        /* Setting google dns to resolve the hostname */
        struct nrf_in_addr dns;
        // dns.s_addr = 134744072; // Google DNS primary, 8.8.8.8
        // dns.s_addr = 134743044; // Google DNS secondary, 8.8.4.4
        dns.s_addr = dns_servers[dns_idx].addr; // OpenDNS, 208.67.222.222
        int dns_err = nrf_setdnsaddr(NRF_AF_INET, &dns, sizeof(dns));
        LOG_INF("DNS set result %d", dns_err);

        err = getaddrinfo(HTTPS_HOSTNAME, NULL, &hints, &res);
        if (err == 0) {
            break;
        }
            
        LOG_INF("DNS server %d.%d.%d.%d failed with err %d", p_ipu8[0],  p_ipu8[1],  p_ipu8[2],  p_ipu8[3], err);
        freeaddrinfo(res);
        dns_idx++;
    }

    if (err) {
        LOG_ERR("Resolving %s failed with all DNS servers\n",HTTPS_HOSTNAME);
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
        LOG_ERR("Failed to open socket!");
        goto clean_up;
    }

    /* Setup TLS socket options */
    err = tls_setup(fd);
    if (err) {
        goto clean_up;
    }

    /* Connecting to the host with provided socket */
    LOG_INF("Connecting to %s", HTTPS_HOSTNAME);
    err = connect(fd, res->ai_addr, sizeof(struct sockaddr_in));
    if (err) {
        LOG_ERR("connect() failed, err: %d", errno);
        goto clean_up;
    }
    LOG_INF("Connected");

    app_main_handler(fd);

clean_up:
    LOG_INF("Finished, closing socket.");
    if (res != 0)
    {
        freeaddrinfo(res);
    }
    (void)close(fd);

    lte_lc_power_off();
    LOG_INF("Cleaned up, shutting down ...");
}
