/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se
	Copyright 2017 Nico Ackermann	added current ramping to to timeout and fuction to fire it

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#include "timeout.h"
#include "mc_interface.h"
#include "utils.h"
#include <math.h>

// Private variables
static volatile systime_t timeout_msec;
static volatile systime_t last_update_time;
static volatile float timeout_brake_current;
static volatile bool has_timeout;
static volatile bool fire_timeout;

// Threads
static THD_WORKING_AREA(timeout_thread_wa, 512);
static THD_FUNCTION(timeout_thread, arg);

void timeout_init(void) {
	timeout_msec = 1000;
	last_update_time = 0;
	timeout_brake_current = 0.0;
	has_timeout = true;
	fire_timeout = false;

	chThdCreateStatic(timeout_thread_wa, sizeof(timeout_thread_wa), NORMALPRIO, timeout_thread, NULL);
}

void timeout_configure(systime_t timeout, float brake_current) {
	timeout_msec = timeout;
	timeout_brake_current = brake_current;
}

void timeout_reset(void) {
	last_update_time = chVTGetSystemTime();
	fire_timeout = false;
}

void timeout_fire(void) {
	fire_timeout = true;
}

bool timeout_has_timeout(void) {
	return has_timeout;
}

systime_t timeout_get_timeout_msec(void) {
	return timeout_msec;
}

float timeout_get_brake_current(void) {
	return timeout_brake_current;
}

static THD_FUNCTION(timeout_thread, arg) {
	(void)arg;

	chRegSetThreadName("Timeout");

	for(;;) {
		if (timeout_msec != 0 && (chVTTimeElapsedSinceX(last_update_time) > MS2ST(timeout_msec) || fire_timeout)) {
			mc_interface_unlock();
			
			static float current = 0.0;
			static float direction = 0.0;
			static int stoppedCounter = 0;
			
			if(!has_timeout){
				// to know if drawing or generating and how much
				current = mc_interface_get_tot_current();
				// to know in which direction the motor goes
				direction = mc_interface_get_tot_current_directional();
				stoppedCounter = 1000;
			}
			
			if (stoppedCounter != 0) {
			
				if (fabsf(mc_interface_get_rpm()) < 250.0) {
					stoppedCounter -= 10;
				} else {
					stoppedCounter = 1000;
				}
						
				const float ramp_step = ((current < 0.0 && -timeout_brake_current > current) || (current > 0.0 && -timeout_brake_current < current)) ? 0.5 : 0.2;

				utils_step_towards(&current, -timeout_brake_current, ramp_step);
			
				if (current > 0.0) {
					mc_interface_set_current(SIGN(direction) * current);
				}else{
					mc_interface_set_brake_current(current);
				}
			} else {
				mc_interface_set_current(0.0);
			}
			
			has_timeout = true;
		} else {
			has_timeout = false;
		}

		chThdSleepMilliseconds(10);
	}
}
