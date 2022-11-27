#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>
#include <date_time.h>
#include "assistance.h"

LOG_MODULE_REGISTER(gnss_sample, 3);

static const char update_indicator[] = {'\\', '|', '/', '-'};

/* Opeational data */
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static uint64_t fix_timestamp;

/* Asynchronous object to handle GNSS event */
K_MSGQ_DEFINE(nmea_queue, sizeof(struct nrf_modem_gnss_nmea_data_frame *), 10, 4);
static K_SEM_DEFINE(pvt_data_sem, 0, 1);
static K_SEM_DEFINE(time_sem, 0, 1);

static struct k_poll_event events[2] = {
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
					K_POLL_MODE_NOTIFY_ONLY,
					&pvt_data_sem, 0),
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
					K_POLL_MODE_NOTIFY_ONLY,
					&nmea_queue, 0),
};


/* GNSS Assitance work queue */
static struct k_work_q gnss_work_q;
#define GNSS_WORKQ_THREAD_STACK_SIZE 2304
#define GNSS_WORKQ_THREAD_PRIORITY   5
K_THREAD_STACK_DEFINE(gnss_workq_stack_area, GNSS_WORKQ_THREAD_STACK_SIZE);
static struct k_work agps_data_get_work;
static volatile bool requesting_assistance;
static struct nrf_modem_gnss_agps_data_frame last_agps;

static void agps_data_get_work_fn(struct k_work *item)
{
	ARG_UNUSED(item);

	int err;

	/* With minimal assistance, the request should be ignored if no GPS time or position
	 * is requested.
	 */
	if (!(last_agps.data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) &&
	    !(last_agps.data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST)) {
		LOG_INF("Ignoring assistance request because no GPS time or position is requested");
		return;
	}

	requesting_assistance = true;

	LOG_INF("Assistance data needed, ephe 0x%08x, alm 0x%08x, flags 0x%02x",
		last_agps.sv_mask_ephe,
		last_agps.sv_mask_alm,
		last_agps.data_flags);

	err = assistance_request(&last_agps);
	if (err) {
		LOG_ERR("Failed to request assistance data");
	}

	requesting_assistance = false;
}

static int sample_init(void)
{
	int err = 0;

	struct k_work_queue_config cfg = {
		.name = "gnss_work_q",
		.no_yield = false
	};

	k_work_queue_start(
		&gnss_work_q,
		gnss_workq_stack_area,
		K_THREAD_STACK_SIZEOF(gnss_workq_stack_area),
		GNSS_WORKQ_THREAD_PRIORITY,
		&cfg);

	k_work_init(&agps_data_get_work, agps_data_get_work_fn);

	err = assistance_init(&gnss_work_q);

	return err;
}

static void gnss_event_handler(int event)
{
	int retval;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		retval = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
		if (retval == 0) {
			k_sem_give(&pvt_data_sem);
		}
		break;

	case NRF_MODEM_GNSS_EVT_NMEA:
		nmea_data = k_malloc(sizeof(struct nrf_modem_gnss_nmea_data_frame));
		if (nmea_data == NULL) {
			LOG_ERR("Failed to allocate memory for NMEA");
			break;
		}

		retval = nrf_modem_gnss_read(nmea_data,
					     sizeof(struct nrf_modem_gnss_nmea_data_frame),
					     NRF_MODEM_GNSS_DATA_NMEA);
		if (retval == 0) {
			retval = k_msgq_put(&nmea_queue, &nmea_data, K_NO_WAIT);
		}

		if (retval != 0) {
			k_free(nmea_data);
		}
		break;

	case NRF_MODEM_GNSS_EVT_AGPS_REQ:
		retval = nrf_modem_gnss_read(&last_agps,
					     sizeof(last_agps),
					     NRF_MODEM_GNSS_DATA_AGPS_REQ);
		if (retval == 0) {
			k_work_submit_to_queue(&gnss_work_q, &agps_data_get_work);
		}
		break;

	default:
		break;
	}
}

static int gnss_init_and_start(void)
{
	/* Enable GNSS. */
	if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS) != 0) {
		LOG_ERR("Failed to activate GNSS functional mode");
		return -1;
	}

	/* Configure GNSS. */
	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0) {
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}

	/* This use case flag should always be set. */
	uint8_t use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;
    use_case |= NRF_MODEM_GNSS_USE_CASE_SCHED_DOWNLOAD_DISABLE;
    use_case |= NRF_MODEM_GNSS_USE_CASE_LOW_ACCURACY;

	if (nrf_modem_gnss_use_case_set(use_case) != 0) {
		LOG_WRN("Failed to set GNSS use case");
	}

#if defined(CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK)
	if (nrf_modem_gnss_elevation_threshold_set(CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK) != 0) {
		LOG_ERR("Failed to set elevation threshold");
		return -1;
	}
	LOG_DBG("Set elevation threshold to %u", CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK);
#endif

	/* Default to no power saving. */
	uint8_t power_mode = NRF_MODEM_GNSS_PSM_DISABLED;

	if (nrf_modem_gnss_power_mode_set(power_mode) != 0) {
		LOG_ERR("Failed to set GNSS power saving mode");
		return -1;
	}

    /* Setting GNSS to get data at fix_retry timeout and fix_interval interval*/
	uint16_t fix_retry = 120;
	uint16_t fix_interval = 120;

	if (nrf_modem_gnss_fix_retry_set(fix_retry) != 0) {
		LOG_ERR("Failed to set GNSS fix retry");
		return -1;
	}

	if (nrf_modem_gnss_fix_interval_set(fix_interval) != 0) {
		LOG_ERR("Failed to set GNSS fix interval");
		return -1;
	}

	if (nrf_modem_gnss_start() != 0) {
		LOG_ERR("Failed to start GNSS");
		return -1;
	}

    /* Priotize GNSS to fix NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME */
    if (nrf_modem_gnss_prio_mode_enable() != 0)
    {
        LOG_ERR("Failed to prioritize GNSS over LTE");
    }

	return 0;
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	printf("Latitude:       %.06f\n", pvt_data->latitude);
	printf("Longitude:      %.06f\n", pvt_data->longitude);
	printf("Altitude:       %.01f m\n", pvt_data->altitude);
	printf("Accuracy:       %.01f m\n", pvt_data->accuracy);
	printf("Speed:          %.01f m/s\n", pvt_data->speed);
	printf("Speed accuracy: %.01f m/s\n", pvt_data->speed_accuracy);
	printf("Heading:        %.01f deg\n", pvt_data->heading);
	printf("Date:           %04u-%02u-%02u\n",
	       pvt_data->datetime.year,
	       pvt_data->datetime.month,
	       pvt_data->datetime.day);
	printf("Time (UTC):     %02u:%02u:%02u.%03u\n",
	       pvt_data->datetime.hour,
	       pvt_data->datetime.minute,
	       pvt_data->datetime.seconds,
	       pvt_data->datetime.ms);
	printf("PDOP:           %.01f\n", pvt_data->pdop);
	printf("HDOP:           %.01f\n", pvt_data->hdop);
	printf("VDOP:           %.01f\n", pvt_data->vdop);
	printf("TDOP:           %.01f\n", pvt_data->tdop);
}

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_sem);
}

static int modem_init(void)
{
	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
	{
		if (IS_ENABLED(CONFIG_DATE_TIME)) {
			date_time_register_handler(date_time_evt_handler);
		}

		lte_lc_psm_req(true);

		if (IS_ENABLED(CONFIG_DATE_TIME)) {
			LOG_INF("Waiting for current time");

			/* Wait for an event from the Date Time library. */
			k_sem_take(&time_sem, K_MINUTES(10));

			if (!date_time_is_valid()) {
				LOG_WRN("Failed to get current time, continuing anyway");
			}
		}

		return 0;
	}

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		date_time_register_handler(date_time_evt_handler);
	}

	if (lte_lc_init() != 0) {
		LOG_ERR("Failed to initialize LTE link controller");
		return -1;
	}

	lte_lc_psm_req(true);

	LOG_INF("Connecting to LTE network");

	if (lte_lc_connect() != 0) {
		LOG_ERR("Failed to connect to LTE network");
		return -1;
	}

	LOG_INF("Connected to LTE network");

	if (IS_ENABLED(CONFIG_DATE_TIME)) {
		LOG_INF("Waiting for current time");

		/* Wait for an event from the Date Time library. */
		k_sem_take(&time_sem, K_MINUTES(10));

		if (!date_time_is_valid()) {
			LOG_WRN("Failed to get current time, continuing anyway");
		}
	}
 
	return 0;
}

static bool output_paused(void)
{
	return (requesting_assistance || assistance_is_active()) ? true : false;
}

static void print_satellite_stats(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	uint8_t tracked   = 0;
	uint8_t in_fix    = 0;
	uint8_t unhealthy = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i) {
		if (pvt_data->sv[i].sv > 0) {
			tracked++;

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX) {
				in_fix++;
			}

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY) {
				unhealthy++;
			}
		}
	}

	printf("Tracking: %2d Using: %2d Unhealthy: %d\n", tracked, in_fix, unhealthy);
}

void entrypoint_gnss(void *arg1, void *arg2, void *arg3)
{
	uint8_t cnt = 0;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;

	LOG_INF("Starting GNSS sample");

    // TODO solve this
	if (modem_init() != 0) {
		LOG_ERR("Failed to initialize modem");
		return -1;
	}

	if (sample_init() != 0) {
		LOG_ERR("Failed to initialize sample");
		return -1;
	}

	if (gnss_init_and_start() != 0) {
		LOG_ERR("Failed to initialize and start GNSS");
		return -1;
	}

	fix_timestamp = k_uptime_get();
    
    unsigned insuffcient_time_window_cnt = 0;
    bool gnss_prioritized = false;

	for (;;) {
		(void)k_poll(events, 2, K_FOREVER);

		if (events[0].state == K_POLL_STATE_SEM_AVAILABLE && k_sem_take(events[0].sem, K_NO_WAIT) == 0) {
			/* New PVT data available */

			if (IS_ENABLED(CONFIG_GNSS_SAMPLE_NMEA_ONLY)) {
				/* NMEA-only output mode. */

				if (output_paused()) {
					goto handle_nmea;
				}

			} else {
				/* PVT and NMEA output mode. */

				if (output_paused()) {
					goto handle_nmea;
				}

				printf("\033[1;1H");
				printf("\033[2J");
				print_satellite_stats(&last_pvt);

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED) {
					printf("GNSS operation blocked by LTE\n");
				}
				if (last_pvt.flags &
				    NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME) {
					printf("Insufficient GNSS time windows\n");
                    insuffcient_time_window_cnt++;
                    if(insuffcient_time_window_cnt >= 3)
                    {
                        printf("Priortizing GNSS over LTE\n");
                        if (gnss_prioritized == false)
                        {
                            /* Priotize GNSS to fix NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME */
                            if (nrf_modem_gnss_prio_mode_enable() != 0)
                            {
		                        LOG_ERR("Failed to prioritize GNSS over LTE");
                            }
                            gnss_prioritized = true;
                        }
                    }
				}
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_SLEEP_BETWEEN_PVT) {
					printf("Sleep period(s) between PVT notifications\n");
				}
				printf("-----------------------------------\n");

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
					fix_timestamp = k_uptime_get();
					print_fix_data(&last_pvt);
                    insuffcient_time_window_cnt = 0;
                    gnss_prioritized = false;
				} else {
					printf("Seconds since last fix: %d\n",
					       (uint32_t)((k_uptime_get() - fix_timestamp) / 1000));
					cnt++;
					printf("Searching [%c]\n", update_indicator[cnt%4]);
				}

				printf("\nNMEA strings:\n\n");
			}
		}

handle_nmea:
		if (events[1].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE && k_msgq_get(events[1].msgq, &nmea_data, K_NO_WAIT) == 0) {
			/* New NMEA data available */

			if (!output_paused()) {
				printf("%s", nmea_data->nmea_str);
			}
			k_free(nmea_data);
		}

		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
	}

	return 0;
}
