#include "ch.h" // ChibiOS
#include "hal.h" // ChibiOS HAL
#include "mc_interface.h" // Motor control functions
#include "hw.h" // Pin mapping on this hardware
#include "timeout.h" // To reset the timeout

static THD_FUNCTION(transmission_thread, arg);
static THD_WORKING_AREA(transmission_thread_wa, 2048);

void app_transmission_start(void) {
	palSetPadMode(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_MODE_OUTPUT_OPENDRAIN);
	chThdCreateStatic(transmission_thread_wa, sizeof(transmission_thread_wa), NORMALPRIO, transmission_thread, NULL);
}

void app_transmission_stop(void) {
}

void app_transmission_configure(uint32_t erpm) {
}

static THD_FUNCTION(transmission_thread, arg) {
	(void)arg;

	chRegSetThreadName("APP_TRANSMISSION");

	for (;;) {
	}
}

