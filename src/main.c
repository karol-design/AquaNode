/*
 * TODO: Fix global variables, fix duplicated condvar for checking if connected
 */

#include <stdio.h>
#include <string.h>

#if defined(CONFIG_POSIX_API)
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>
#else
#include <zephyr/net/socket.h>
#endif /* CONFIG_POSIX_API */

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
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

#define MAX_TELEMETRY_SIZE_BYTES 128

static int data_upload_iterations = CONFIG_LISP_POC_DATA_UPLOAD_ITERATIONS;

static struct k_work_delayable measure_and_upload_work;

static struct sockaddr_storage server = {0};
static struct coap_client coap_client = {0};
static int sock;

/* Variable used to indicate if network is connected. */
static bool is_connected;

K_SEM_DEFINE(lte_connected_sem, 0, 1);
K_SEM_DEFINE(modem_shutdown_sem, 0, 1);

/* Mutex and conditional variable used to signal network connectivity. */
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

  // Generate random temperatures
  uint32_t t1 = (sys_rand32_get() % 10) + 15;
  uint32_t t2 = (sys_rand32_get() % 5) + 25;

  char temp_temperature_str[10] = "";

  char payload[MAX_TELEMETRY_SIZE_BYTES + 1] = "";  // "{\"t1\":\"1.15\", \"t2\":\"2.24\"}"
  strncat(payload, "{\"t1\":\"", MAX_TELEMETRY_SIZE_BYTES - strlen(payload));
  snprintf(temp_temperature_str, 10, "%u", t1);
  strncat(payload, temp_temperature_str, MAX_TELEMETRY_SIZE_BYTES - strlen(payload));
  strncat(payload, "\", \"t2\":\"", MAX_TELEMETRY_SIZE_BYTES - strlen(payload));
  snprintf(temp_temperature_str, 10, "%u", t2);
  strncat(payload, temp_temperature_str, MAX_TELEMETRY_SIZE_BYTES - strlen(payload));
  strncat(payload, "\"}", MAX_TELEMETRY_SIZE_BYTES - strlen(payload));

  req.payload = payload;
  req.len = strlen((char*)payload);

  /* Send request */
  err = coap_client_req(&coap_client, sock, (struct sockaddr*)&server, &req, NULL);
  if (err) {
    LOG_ERR("Failed to send request: %d", err);
  }

  LOG_INF("CoAP POST request sent to %s, resource: %s", CONFIG_LISP_POC_SERVER_HOSTNAME, CONFIG_LISP_POC_RESOURCE);

  /* Transmit a limited number of times and then shutdown. */
  if (data_upload_iterations > 0) {
    data_upload_iterations--;
  } else if (data_upload_iterations == 0) {
    k_sem_give(&modem_shutdown_sem);
    /* No need to schedule work if we're shutting down. */
    return;
  }

  /* Schedule work if we're either transmitting indefinitely or
   * there are more iterations left.
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
      k_sem_give(&lte_connected_sem);
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

int main(void) {
  int err;

  LOG_INF("LISP PoC started");

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

  /* Request PSM and eDRX before attempting to register to the network */
  // lte_lc_edrx_req(true);
  err = lte_lc_psm_req(true);
  if (err) {
    LOG_WRN("Failed to request the PSM in LTE LC, error: %d", err);
  }

  err = lte_lc_connect_async(lte_handler);
  if (err) {
    LOG_ERR("Failed to connect to LTE network, error: %d", err);
    FATAL_ERROR();
    return err;
  }

  /* Wait forever until the modem connects to the network */
  k_sem_take(&lte_connected_sem, K_FOREVER);

  err = coap_init();
  if (err) {
    LOG_ERR("Failed to initialize the CoAP or related sockets, error: %d", err);
    FATAL_ERROR();
    return err;
  }

  k_work_schedule(&measure_and_upload_work, K_NO_WAIT);

  k_sem_take(&modem_shutdown_sem, K_FOREVER);

  err = nrf_modem_lib_shutdown();
  if (err) {
    return err;
  }

  return 0;
}
