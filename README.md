# LISP | Low-Power & Low-Cost IoT Sensing Platform (CoAP Telemetry PoC)

## Overview

The **LISP PoC** is a proof-of-concept project for collecting and sending telemetry data over LTE (Cat-M / NB-IoT) using the **nRF9151 DK**. The application periodically gathers system metrics (battery, modem temperature, LTE signal quality) and custom sensor data, formats it as JSON, and sends it to a CoAP server.  

This project demonstrates:
- Asynchronous LTE connection with **PSM and eDRX support**  
- Periodic measurement and CoAP POST requests  
- Lightweight JSON payload construction  
- Safe handling of network connectivity and workqueues in Zephyr OS

---

## Configuration

Configure parameters in `prj.conf` or via Kconfig:

| Option | Description |
|--------|-------------|
| `CONFIG_LISP_POC_STARTUP_DELAY_SECONDS` | Delay before starting measurements (for logging/tracing) |
| `CONFIG_LISP_POC_UPLOAD_FREQUENCY_SECONDS` | Interval between CoAP uploads |
| `CONFIG_LISP_POC_DATA_UPLOAD_ITERATIONS` | Total number of telemetry uploads (set -1 for infinite) |
| `CONFIG_LISP_POC_SERVER_HOSTNAME` | CoAP server hostname |
| `CONFIG_LISP_POC_SERVER_PORT` | CoAP server port |
| `CONFIG_LISP_POC_RESOURCE` | Resource path on CoAP server (e.g., `/telemetry`) |

---

## Building and Flashing

```bash
# Build the project (for debugging)
west build --pristine --board nrf9151dk/nrf9151/ns -S nrf91-modem-trace-uart -S tfm-enable-share-uart -- -DCONFIG_DEBUG_OPTIMIZATIONS=y -DCONFIG_DEBUG_THREAD_INFO=y

# Flash the application
west flash --dev-id <dev_id> --recover (e.g. west flash --dev-id 1051277391 --recover)
