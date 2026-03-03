#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(leds, LOG_LEVEL_DBG);

/* Map LED dt_specs into the respective const array */
#define DT_LED(i) GPIO_DT_SPEC_GET(DT_ALIAS(led##i), gpios)

#define LEDS_STARTUP_MATRIX_LOOPS_COUNT 8

static const struct gpio_dt_spec leds[] = {
    DT_LED(0),
    DT_LED(1),
    DT_LED(3),
    DT_LED(2),
};

#define LEDS_COUNT ARRAY_SIZE(leds)

void leds_init(void) {
  int ret = 0;

  /* Configure all LEDs as active outputs */
  for (int led_no = 0; led_no < LEDS_COUNT; led_no++) {
    ret = gpio_pin_configure_dt(&leds[led_no], GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
      LOG_ERR("Failed to configure gpio for LED%d (ret: %d)", led_no, ret);
    }
  }

  return;
}

void leds_set(int n, bool state) {
  gpio_pin_set_dt(&leds[n], state);
  return;
}

void leds_matrix_set(int n) {
  for (int led_no = 0; led_no < LEDS_COUNT; led_no++) {
    leds_set(led_no, (led_no == n));
  }
  return;
}

void leds_startup_animation() {
  for (int loops = 0; loops < LEDS_STARTUP_MATRIX_LOOPS_COUNT; loops++) {
    for (int led = 0; led < LEDS_COUNT; led++) {
      leds_matrix_set(led);
      k_sleep(K_MSEC(150));
    }
  }
  leds_matrix_set(-1);
}

void leds_signal_rrc_connected(void) { leds_matrix_set(0); }

void leds_signal_rrc_idle(void) { leds_matrix_set(-1); }