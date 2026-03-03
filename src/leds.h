#ifndef __LEDS_H__
#define __LEDS_H__

void leds_init(void);
void leds_set(int n, bool state);
void leds_matrix_set(int n);
void leds_startup_animation(void);
void leds_signal_rrc_connected(void);
void leds_signal_rrc_idle(void);

#endif