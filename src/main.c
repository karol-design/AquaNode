/*
 * TODO: Fix global variables, improve the architecture, check for corner cases (how to add robustness), review the code
 */

#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/coap_client.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(lisp_poc, CONFIG_LISP_POC_LOG_LEVEL);

/* Macro called upon a fatal error, reboots the device. */
#define FATAL_ERROR()                            \
  LOG_ERR("Fatal error! Rebooting the device."); \
  LOG_PANIC();                                   \
  IF_ENABLED(CONFIG_REBOOT, (sys_reboot(0)))

#define MAX_TELEMETRY_SIZE_BYTES 256
#define MAX_OPERATOR_STRING_SIZE_BYTES 16
#define MAX_RAT_STRING_SIZE_BYTES 16

static int remaining_upload_iterations = CONFIG_LISP_POC_DATA_UPLOAD_ITERATIONS - 1;
static struct k_work_delayable measure_and_upload_work;
static struct sockaddr_storage server = {0};
static struct coap_client coap_client = {0};
static int sock;
static bool is_connected; /* Variable used to indicate if network is connected. */

/* Mutex and conditional variable used to signal network connectivity and shutdown request. */
K_SEM_DEFINE(modem_shutdown_sem, 0, 1);
K_MUTEX_DEFINE(network_connected_lock);
K_CONDVAR_DEFINE(network_connected);

static void wait_for_network(void) {
  k_mutex_lock(&network_connected_lock, K_FOREVER);

  if (!is_connected) {
    LOG_INF("Waiting for network connectivity");
    k_condvar_wait(&network_connected, &network_connected_lock, K_FOREVER);
  }

  k_mutex_unlock(&network_connected_lock);
}

static void response_cb(int16_t code, size_t offset, const uint8_t* payload, size_t len, bool last_block,
                        void* user_data) {
  if (code >= 0) {
    LOG_INF("CoAP response: code: 0x%x, payload: %s", code, (char*)(payload ? (char*)payload : "NULL"));
  } else {
    LOG_INF("Response received with error code: %d", code);
  }
}

static int server_resolve(struct sockaddr_storage* server) {
  int err;
  struct addrinfo* result;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
  char ipv4_addr[NET_IPV4_ADDR_LEN];

  err = getaddrinfo(CONFIG_LISP_POC_SERVER_HOSTNAME, NULL, &hints, &result);
  if (err) {
    LOG_ERR("getaddrinfo, error: %d", err);
    return err;
  }

  if (result == NULL) {
    LOG_ERR("Address not found");
    return -ENOENT;
  }

  /* IPv4 Address. */
  struct sockaddr_in* server4 = ((struct sockaddr_in*)server);

  server4->sin_addr.s_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr.s_addr;
  server4->sin_family = AF_INET;
  server4->sin_port = htons(CONFIG_LISP_POC_SERVER_PORT);

  inet_ntop(AF_INET, &server4->sin_addr.s_addr, ipv4_addr, sizeof(ipv4_addr));

  LOG_INF("IPv4 Address found %s", ipv4_addr);

  /* Free the address. */
  freeaddrinfo(result);

  return 0;
}

static int coap_init(void) {
  int err;

  err = server_resolve(&server);
  if (err) {
    LOG_ERR("Failed to resolve server name");
    return err;
  }

  sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    LOG_ERR("Failed to create CoAP socket: %d.", -errno);
    return -errno;
  }

  LOG_INF("Initializing CoAP client");

  err = coap_client_init(&coap_client, NULL);
  if (err) {
    LOG_ERR("Failed to initialize CoAP client: %d", err);
    return err;
  }

  return 0;
}

static int get_battery_mv(void) {
  int mv;
  if (modem_info_get_batt_voltage(&mv) == 0) {
    return mv;
  }
  return -1;
}

static int get_rsrp_dbm(void) {
  int rsrp;
  if (modem_info_get_rsrp(&rsrp) == 0) {
    return rsrp;
  }
  return -1;
}

static int get_temperature_degc(void) {
  int temperature;
  if (modem_info_get_temperature(&temperature) == 0) {
    return temperature;
  }
  return -1;
}

static int get_operator(char* buf, size_t len) { return modem_info_string_get(MODEM_INFO_OPERATOR, buf, len); }

static int get_rat(char* buf, size_t len) { return modem_info_string_get(MODEM_INFO_LTE_MODE, buf, len); }

static float get_ds18b20_temp(const struct device* ds18b20) {
  if (!device_is_ready(ds18b20)) {
    LOG_ERR("DS18B20 not ready");
    return -1000.0f;
  }

  struct sensor_value temp;

  if (sensor_sample_fetch(ds18b20) < 0) {
    LOG_ERR("Sample fetch failed");
    return -1000.0f;
  }

  if (sensor_channel_get(ds18b20, SENSOR_CHAN_AMBIENT_TEMP, &temp) < 0) {
    LOG_ERR("Channel get failed");
    return -1000.0f;
  }

  float temp_float = sensor_value_to_double(&temp);

  LOG_INF("Temp ds18b20: %.3f C", temp_float);
  return temp_float;
}

static void build_json_payload(char* payload) {
  // Generate random temperatures
  // float base = 21.0f + ((sys_rand32_get() % 20) / 10.0f);  // 21.0 – 23.0

  // float t1 = base + ((int)(sys_rand32_get() % 10) - 5) / 10.0f;  // ±0.5
  // float t2 = base + ((int)(sys_rand32_get() % 15) - 5) / 10.0f;
  // float t3 = base + ((int)(sys_rand32_get() % 7) - 5) / 10.0f;

  const struct device* ds0 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_0));
  const struct device* ds1 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_1));
  const struct device* ds2 = DEVICE_DT_GET(DT_NODELABEL(ds18b20_2));
  float t1 = get_ds18b20_temp(ds0);
  float t2 = get_ds18b20_temp(ds1);
  float t3 = get_ds18b20_temp(ds2);

  // Collect system metrics
  uint32_t uptime_s = k_uptime_get() / 1000;
  int voltage_mv = get_battery_mv();
  int temperature_degc = get_temperature_degc();
  int rsrp_dbm = get_rsrp_dbm();

  char operator[MAX_OPERATOR_STRING_SIZE_BYTES];
  char rat[MAX_RAT_STRING_SIZE_BYTES];
  if (get_operator(operator, sizeof(operator)) < 0) {
    strncpy(operator, "unknown", sizeof(operator));
  }
  if (get_rat(rat, sizeof(rat)) < 0) {
    strncpy(rat, "unknown", sizeof(rat));
  }

  // Construct the json payload in the {"key1":"val1",...}" format
  snprintk(payload, MAX_TELEMETRY_SIZE_BYTES,
           "{\"t1\":%.2f, \"t2\":%.2f, \"t3\":%.2f, "
           "\"up_s\":%u, \"temp_c\":%d, "
           "\"bat_mv\":%d, \"rsrp\":%d, "
           "\"plmn\":\"%s\", \"rat\":\"%s\"}",
           t1, t2, t3, uptime_s, temperature_degc, voltage_mv, rsrp_dbm, operator, rat);
  LOG_INF("CoAP payload ready: %s", payload);

  return;
}

static void measure_and_upload_work_fn(struct k_work* work) {
  int err;
  struct coap_client_request req = {
      .method = COAP_METHOD_POST,
      .confirmable = true,
      .fmt = COAP_CONTENT_FORMAT_APP_JSON,
      .cb = response_cb,
      .path = CONFIG_LISP_POC_RESOURCE,
  };

  wait_for_network();

  char payload[MAX_TELEMETRY_SIZE_BYTES + 1] = "";
  build_json_payload(payload);

  req.payload = payload;
  req.len = strlen((char*)payload);

  /* Send request */
  err = coap_client_req(&coap_client, sock, (struct sockaddr*)&server, &req, NULL);
  if (err) {
    LOG_ERR("Failed to send request: %d", err);
  }

  LOG_INF("CoAP POST request sent to %s, resource: %s", CONFIG_LISP_POC_SERVER_HOSTNAME, CONFIG_LISP_POC_RESOURCE);

  /* Transmit a limited number of times and then shutdown. */
  if (remaining_upload_iterations > 0) {
    remaining_upload_iterations--;
  } else if (remaining_upload_iterations == 0) {
    k_sem_give(&modem_shutdown_sem);
    /* No need to schedule work if we're shutting down. */
    return;
  }

  /* Schedule work if we're either transmitting indefinitely or there are more iterations left.
   */
  k_work_schedule(&measure_and_upload_work, K_SECONDS(CONFIG_LISP_POC_UPLOAD_FREQUENCY_SECONDS));
}

static void lte_handler(const struct lte_lc_evt* const evt) {
  switch (evt->type) {
    case LTE_LC_EVT_NW_REG_STATUS:
      if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
          (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
        LOG_INF("Network registration status: %d (not registered)", evt->nw_reg_status);
        /* Update the is_connected */
        k_mutex_lock(&network_connected_lock, K_FOREVER);
        is_connected = false;
        k_mutex_unlock(&network_connected_lock);
        break;
      }

      LOG_INF("Network registration status: %s",
              evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ? "Connected - home" : "Connected - roaming");
      /* Update the is_connected and signal it via the cond_va r*/
      k_mutex_lock(&network_connected_lock, K_FOREVER);
      is_connected = true;
      k_condvar_signal(&network_connected);
      k_mutex_unlock(&network_connected_lock);
      break;
      break;
    case LTE_LC_EVT_PSM_UPDATE:
      LOG_INF("PSM parameter update: TAU: %d s, Active time: %d s", evt->psm_cfg.tau, evt->psm_cfg.active_time);
      break;
    case LTE_LC_EVT_EDRX_UPDATE:
      LOG_INF("eDRX parameter update: eDRX: %.2f s, PTW: %.2f s", (double)evt->edrx_cfg.edrx,
              (double)evt->edrx_cfg.ptw);
      break;
    case LTE_LC_EVT_RRC_UPDATE:
      LOG_INF("RRC mode: %s", evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
      break;
    case LTE_LC_EVT_CELL_UPDATE:
      LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d", evt->cell.id, evt->cell.tac);
      break;
    case LTE_LC_EVT_LTE_MODE_UPDATE:
      /* Note: None = 0; LTE Cat.M = 7; NB-IoT = 9 */
      LOG_INF("RAT (LTE mode) update: %d", evt->lte_mode);
      break;
    case LTE_LC_EVT_TAU_PRE_WARNING:
      LOG_INF("TAU in %d miliseconds", CONFIG_LTE_LC_TAU_PRE_WARNING_TIME_MS);
      break;
    case LTE_LC_EVT_MODEM_SLEEP_ENTER:
      LOG_INF("Modem entering sleep mode: sleep type: %d, duration: %lld", evt->modem_sleep.type,
              evt->modem_sleep.time);
      break;
    case LTE_LC_EVT_MODEM_SLEEP_EXIT:
      LOG_INF("Modem just woke up");
      break;
    case LTE_LC_EVT_PDN:
      LOG_INF("PDN event: type: %d, cid: %u", evt->pdn.type, evt->pdn.cid);
      break;
    case LTE_LC_EVT_CELLULAR_PROFILE_ACTIVE:
      LOG_INF("Cellular profile change: %hd", evt->cellular_profile.profile_id);
      break;
    case LTE_LC_EVT_RAI_UPDATE:
      /* RAI notification is supported by modem firmware releases >= 2.0.2 */
      LOG_INF("RAI configuration update: Cell ID: %d, MCC: %d, MNC: %d, AS-RAI: %d, CP-RAI: %d", evt->rai_cfg.cell_id,
              evt->rai_cfg.mcc, evt->rai_cfg.mnc, evt->rai_cfg.as_rai, evt->rai_cfg.cp_rai);
      break;
    default:
      LOG_WRN("Unknown LTE LC event: %d", evt->type);
      break;
  }
}

static void log_build_cfg(void) {
  LOG_INF("System Config | startup delay: %d", CONFIG_LISP_POC_STARTUP_DELAY_SECONDS);
  LOG_INF("Data uploads Config | itterations: %d; interval: %d s", CONFIG_LISP_POC_DATA_UPLOAD_ITERATIONS,
          CONFIG_LISP_POC_UPLOAD_FREQUENCY_SECONDS);
  LOG_INF("Network Config | PSM req: %s (sleep: %s, active: %s); eDRX req: %s (LTE-M: %s, NB-IoT: %s)",
          CONFIG_LTE_PSM_REQ ? "On" : "Off", CONFIG_LTE_PSM_REQ_RPTAU, CONFIG_LTE_PSM_REQ_RAT,
          CONFIG_LTE_EDRX_REQ ? "On" : "Off", CONFIG_LTE_EDRX_REQ_VALUE_LTE_M, CONFIG_LTE_EDRX_REQ_VALUE_NBIOT);
}

int main(void) {
  int err;

  LOG_INF("LISP PoC started");

  /* Print key parameters from the build configuration */
  log_build_cfg();

  /* Delay the execution of the actual code for a few seconds, e.g. for connecting the tracing software */
  k_sleep(K_SECONDS(CONFIG_LISP_POC_STARTUP_DELAY_SECONDS));

  /* Initialize the workqueue that will handle periodic measurements and uploads to the cloud */
  k_work_init_delayable(&measure_and_upload_work, measure_and_upload_work_fn);

  /* Initialize the nRF Modem library */
  err = nrf_modem_lib_init();
  if (err) {
    LOG_ERR("Failed to initialize modem library, error: %d", err);
    FATAL_ERROR();
    return err;
  }

  err = modem_info_init();
  if (err) {
    LOG_ERR("modem_info_init failed: %d", err);
  }

  err = lte_lc_connect_async(lte_handler);
  if (err) {
    LOG_ERR("Failed to connect to LTE network, error: %d", err);
    FATAL_ERROR();
    return err;
  }

  /* Wait forever until the modem connects to the network */
  wait_for_network();

  err = coap_init();
  if (err) {
    LOG_ERR("Failed to initialize the CoAP or related sockets, error: %d", err);
    FATAL_ERROR();
    return err;
  }

  k_work_schedule(&measure_and_upload_work, K_NO_WAIT);

  k_sem_take(&modem_shutdown_sem, K_FOREVER);
  LOG_INF("Shuting down the modem and exiting main()");

  err = nrf_modem_lib_shutdown();
  if (err) {
    LOG_ERR("Failed to shutdown the nrf modem lib, error: %d", err);
    return err;
  }

  return 0;
}
