/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se
	Copyright 2017 Nico Ackermann	changed timeout detection and handling by adding a ramping,  
									added average erpm detection, 
									backwards via second brake, 
									individual erpm for cruise control and cruise control signal over can via slave

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

#include "app.h"

#include "ch.h"
#include "hal.h"
#include "stm32f4xx_conf.h"
#include "servo_dec.h"
#include "mc_interface.h"
#include "timeout.h"
#include "utils.h"
#include "comm_can.h"
#include <math.h>

// Only available if servo output is not active
#if !SERVO_OUT_ENABLE

// Settings
#define MAX_CAN_AGE						0.1
#define MIN_PULSES_WITHOUT_POWER		50
#define RPM_FILTER_SAMPLES				4

// Threads
static THD_FUNCTION(ppm_thread, arg);
static THD_WORKING_AREA(ppm_thread_wa, 1024);
static thread_t *ppm_tp;
virtual_timer_t vt;

// Private functions
static void servodec_func(bool is_valid_signal);

// Private variables
static volatile bool is_running = false;
static volatile bool stop_now = true;
static volatile float pid_rpm = 0;
static volatile ppm_config config;
static volatile int pulses_without_power = 0;

static float input_val = 0.0;
static volatile float direction_hyst = 0;

// Private functions
static void update(void *p);
#endif

void app_ppm_configure(ppm_config *conf) {
#if !SERVO_OUT_ENABLE
	config = *conf;
	pulses_without_power = 0;
	
	mc_interface_set_cruise_control_status(CRUISE_CONTROL_INACTIVE);

	if (is_running) {
		servodec_set_pulse_options(config.pulse_start, config.pulse_end, config.median_filter);
	}
	
	direction_hyst = config.max_erpm_for_dir * 0.20;
#else
	(void)conf;
#endif
}

void app_ppm_start(void) {
#if !SERVO_OUT_ENABLE
	stop_now = false;
	chThdCreateStatic(ppm_thread_wa, sizeof(ppm_thread_wa), NORMALPRIO, ppm_thread, NULL);

	chSysLock();
	chVTSetI(&vt, MS2ST(1), update, NULL);
	chSysUnlock();
#endif
}

void app_ppm_stop(void) {
#if !SERVO_OUT_ENABLE
	stop_now = true;

	if (is_running) {
		chEvtSignalI(ppm_tp, (eventmask_t) 1);
		servodec_stop();
	}

	while (is_running) {
		chThdSleepMilliseconds(1);
	}
#endif
}

float app_ppm_get_decoded_level(void) {
#if !SERVO_OUT_ENABLE
	return input_val;
#else
	return 0.0;
#endif
}

#if !SERVO_OUT_ENABLE
static void servodec_func(bool is_valid_signal) {
	chSysLockFromISR();
	if (is_valid_signal && config.ctrl_type != PPM_CTRL_TYPE_CRUISE_CONTROL_SECONDARY_CHANNEL && config.ctrl_type != PPM_CTRL_TYPE_NONE) {
		timeout_reset();
	}
	chEvtSignalI(ppm_tp, (eventmask_t) 1);
	chSysUnlockFromISR();
}

static void update(void *p) {
	if (!is_running) {
		return;
	}

	chSysLockFromISR();
	chVTSetI(&vt, MS2ST(2), update, p);
	chEvtSignalI(ppm_tp, (eventmask_t) 1);
	chSysUnlockFromISR();
}

static bool set_or_update_pid_rpm(float mid_rpm, float servo_val, float passed_time, float max_erpm){
	if (pid_rpm == 0) {
		// are we too fast
		if (mid_rpm > max_erpm) {
			return false;
		}
		pid_rpm = mid_rpm;
	}else{
		pid_rpm += (servo_val * 3000.0) * (passed_time / 1000.0);
			
		if (pid_rpm > (mid_rpm + 3000.0)) {
			pid_rpm = mid_rpm + 3000.0;
		}
	}
	
	if (pid_rpm > max_erpm) {
		pid_rpm = max_erpm;
	}
	return true;
}

static THD_FUNCTION(ppm_thread, arg) {
	(void)arg;

	chRegSetThreadName("APP_PPM");
	ppm_tp = chThdGetSelfX();

	servodec_set_pulse_options(config.pulse_start, config.pulse_end, config.median_filter);
	servodec_init(servodec_func);
	is_running = true;

	for(;;) {
		chEvtWaitAny((eventmask_t) 1);

		if (stop_now) {
			is_running = false;
			return;
		}

		const volatile mc_configuration *mcconf = mc_interface_get_configuration();
		float servo_val = servodec_get_servo(0);
		float servo_ms = utils_map(servo_val, -1.0, 1.0, config.pulse_start, config.pulse_end);

		switch (config.ctrl_type) {
		case PPM_CTRL_TYPE_CURRENT_NOREV:
		case PPM_CTRL_TYPE_DUTY_NOREV:
		case PPM_CTRL_TYPE_PID_NOREV:
			input_val = servo_val;
			servo_val += 1.0;
			servo_val /= 2.0;
			break;

		default:
			// Mapping with respect to center pulsewidth
			if (servo_ms < config.pulse_center) {
				servo_val = utils_map(servo_ms, config.pulse_start,
						config.pulse_center, -1.0, 0.0);
			} else {
				servo_val = utils_map(servo_ms, config.pulse_center,
						config.pulse_end, 0.0, 1.0);
			}
			input_val = servo_val;
			break;
		}
		
		// Apply deadband
		utils_deadband(&servo_val, config.hyst, 1.0);
		
		if (config.ctrl_type == PPM_CTRL_TYPE_CRUISE_CONTROL_SECONDARY_CHANNEL) {
			if (servo_val < -0.3 && config.cruise_left != CRUISE_CONTROL_INACTIVE) {
				mc_interface_set_cruise_control_status(config.cruise_left);
			} else if (servo_val > 0.3 && config.cruise_right != CRUISE_CONTROL_INACTIVE) {
				mc_interface_set_cruise_control_status(config.cruise_right);
			} else {
				mc_interface_set_cruise_control_status(CRUISE_CONTROL_INACTIVE);
			}
			// Run this loop at 500Hz
			chThdSleepMilliseconds(2);

			continue;
		}
		
		static float servo_val_ramp = 0.0;
		static bool ramp_up_from_timeout = false;

		// Apply ramping
		static systime_t last_time = 0;
		
		if (timeout_has_timeout() || chVTTimeElapsedSinceX(servodec_get_last_update_time()) > MS2ST(timeout_get_timeout_msec())) {
			//pulses_without_power = 0;
		
			if (!ramp_up_from_timeout) {
				
				timeout_fire();
				
				if (config.multi_esc) {
					for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
						can_status_msg *msg = comm_can_get_status_msg_index(i);

						if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {					
							comm_can_timeout_fire(msg->id);
						}
					}
				}
				
				ramp_up_from_timeout = true;
			}
					
			const float actual_current = mc_interface_get_tot_current();
			
			const float max_min_c = actual_current < 0.0 ? mcconf->lo_current_motor_min_now : mcconf->lo_current_motor_max_now;
			
			if (max_min_c != 0.0) {
				servo_val_ramp = actual_current / max_min_c * SIGN(mc_interface_get_tot_current_directional()); //sign for direction
				utils_truncate_number(&servo_val_ramp, -1.0, 1.0);
			} else {
				servo_val_ramp = 0.0;
			}

			last_time = chVTGetSystemTimeX();
			
			continue;
		}

		if (mc_interface_get_fault() != FAULT_CODE_NONE) {
			pulses_without_power = 0;
			servo_val_ramp = 0.0;
			last_time = chVTGetSystemTimeX();
			continue;
		}
		
		// Apply throttle curve
		servo_val = utils_throttle_curve(servo_val, config.throttle_exp, config.throttle_exp_brake, config.throttle_exp_mode);
		
		const float ramp_time = ((servo_val_ramp < 0.0 && servo_val > servo_val_ramp) || (servo_val_ramp > 0.0 && servo_val < servo_val_ramp))
								? (ramp_up_from_timeout ? mcconf->lo_current_motor_max_now / 50 : config.ramp_time_neg)
								: (ramp_up_from_timeout ? fabsf(mcconf->lo_current_motor_min_now) / 20 : config.ramp_time_pos);
		
		float passed_time = (float)ST2MS(chVTTimeElapsedSinceX(last_time));
		if (ramp_time > 0.01) {
			const float ramp_step = passed_time / (ramp_time * 1000.0);
			utils_step_towards(&servo_val_ramp, servo_val, ramp_step);
			last_time = chVTGetSystemTimeX();
			if (servo_val == servo_val_ramp) {
				ramp_up_from_timeout = false;
			}
			servo_val = servo_val_ramp;			
		}

		float current = 0;
		bool current_mode = false;
		bool current_mode_brake = false;
		bool send_current = false;
		bool send_pid = false;
		
		// Find lowest RPM and cruise control
		float rpm_local = mc_interface_get_rpm();
		float rpm_lowest = rpm_local;
		float mid_rpm = rpm_local;
		int motor_count = 1;
		ppm_cruise cruise_control_status = CRUISE_CONTROL_INACTIVE;
		if (config.multi_esc) {
			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);

				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {					
					// add to middle rpm count
					mid_rpm += msg->rpm;
					motor_count += 1;

					if (fabsf(msg->rpm) < fabsf(rpm_lowest)) {
						rpm_lowest = msg->rpm;
					}

					// if any controller (VESC) sends the cruise contol status
					if (msg->cruise_control_status != CRUISE_CONTROL_INACTIVE) {
						cruise_control_status = msg->cruise_control_status;
					}
				}
			}
		}
		
		// get middle rpm
		mid_rpm /= motor_count;
		
		switch (config.ctrl_type) {
		case PPM_CTRL_TYPE_CURRENT:
			current_mode = true;
			
			if (config.max_erpm_for_dir_active) { // advanced backwards
				static bool force_brake = true;

				static int8_t did_idle_once = 0;

				// Hysteresis 20 % of actual RPM
				if (force_brake) {
					if (rpm_local < config.max_erpm_for_dir - direction_hyst) { // for 2500 it's 2000
						force_brake = false;
						did_idle_once = 0;
					}
				} else {	
					if (rpm_local > config.max_erpm_for_dir + direction_hyst) { // for 2500 it's 3000
						force_brake = true;
						did_idle_once = 0;
					}
				}
				
				if (servo_val >= 0.0) {
					if (servo_val == 0.0) {
						// if there was a idle in between then allow going backwards
						if (did_idle_once == 1 && !force_brake) {
							did_idle_once = 2;
						}
					}else{
						// accelerated forward or fast enough at least
						if (rpm_local > -config.max_erpm_for_dir){ // for 2500 it's -2500
							did_idle_once = 0;
						}
					}

					// check of can bus send cruise control command
					// needs to move forwared to activate cruise while accelerating
					if (cruise_control_status != CRUISE_CONTROL_INACTIVE && servo_val >= 0.0) {
						// is rpm in range for cruise control
						if (fabsf(rpm_lowest) > mcconf->s_pid_min_erpm
							&& set_or_update_pid_rpm(mid_rpm, servo_val, passed_time, mcconf->l_max_erpm)) {
							current_mode = false;
							send_pid = true;
							
							mc_interface_set_pid_speed_with_cruise_status(rpm_local + pid_rpm - mid_rpm, cruise_control_status);
						} else {
							pid_rpm = 0;
							current = 0.0;
							servo_val = 0.0;
						}
					}else{
							current = servo_val * mcconf->lo_current_motor_max_now;
					}
				} else {

					// too fast
					if (force_brake){
						current_mode_brake = true;
					}else{
						// not too fast backwards
						if (rpm_local > -config.max_erpm_for_dir) { // for 2500 it's -2500
							// first time that we brake and we are not too fast
							if (did_idle_once != 2) {
								did_idle_once = 1;
								current_mode_brake = true;
							}
						// too fast backwards
						} else {
							// if brake was active already
							if (did_idle_once == 1) {
								current_mode_brake = true;
							} else {	
								// it's ok to go backwards now braking would be strange now
								did_idle_once = 2;
							}
						}
					}

					if (current_mode_brake) {
						// braking
						current = fabsf(servo_val * mcconf->lo_current_motor_min_now);
					}else {
						// reverse acceleration
						current = servo_val * fabsf(mcconf->lo_current_motor_min_now);
					}
				}
			} else { // normal backwards
				if ((servo_val >= 0.0 && rpm_local > 0.0) || (servo_val < 0.0 && rpm_local < 0.0)) {
					// check of can bus send cruise control command
					if (cruise_control_status != CRUISE_CONTROL_INACTIVE && servo_val >= 0.0) {
						// is rpm in range for cruise control
						if (rpm_lowest > mcconf->s_pid_min_erpm
							&& set_or_update_pid_rpm(mid_rpm, servo_val, passed_time, mcconf->l_max_erpm)) {
							current_mode = false;
							send_pid = true;
							mc_interface_set_pid_speed_with_cruise_status(rpm_local + pid_rpm - mid_rpm, cruise_control_status);
						} else {
							pid_rpm = 0;
							current = 0.0;
							servo_val = 0.0;
						}
					}else{
						current = servo_val * mcconf->lo_current_motor_max_now;
					}
				} else {
					current = servo_val * fabsf(mcconf->lo_current_motor_min_now);
				}
			}

			if (servo_val < 0.001) {
				pulses_without_power++;
			}
			break;
		case PPM_CTRL_TYPE_CURRENT_NOREV:
			current_mode = true;
			if ((servo_val >= 0.0 && rpm_local > 0.0) || (servo_val < 0.0 && rpm_local < 0.0)) {
				// check of can bus send cruise control command
				if (cruise_control_status != CRUISE_CONTROL_INACTIVE && servo_val >= 0.0) {
					// is rpm in range for cruise control
					if (rpm_lowest > mcconf->s_pid_min_erpm
						&& set_or_update_pid_rpm(mid_rpm, servo_val, passed_time, mcconf->l_max_erpm)) {
						current_mode = false;
						send_pid = true;
						mc_interface_set_pid_speed_with_cruise_status(rpm_local + pid_rpm - mid_rpm, cruise_control_status);
					} else {
						pid_rpm = 0;
						current = 0.0;
						servo_val = 0.0;
					}
				}else{
					current = servo_val * mcconf->lo_current_motor_max_now;
				}
			} else {
				current = servo_val * fabsf(mcconf->lo_current_motor_min_now);
			}

			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}
			break;

		case PPM_CTRL_TYPE_CURRENT_NOREV_BRAKE:
			current_mode = true;		
			if (servo_val >= 0.0) {
				// check of can bus send cruise control command
				if (cruise_control_status != CRUISE_CONTROL_INACTIVE && servo_val >= 0.0) {
					// is rpm in range for cruise control
					if (rpm_lowest > mcconf->s_pid_min_erpm
						&& set_or_update_pid_rpm(mid_rpm, servo_val, passed_time, mcconf->l_max_erpm)) {
						current_mode = false;
						send_pid = true;
						mc_interface_set_pid_speed_with_cruise_status(rpm_local + pid_rpm - mid_rpm, cruise_control_status);
					} else {
						pid_rpm = 0;
						current = 0.0;
						servo_val = 0.0;
					}
				}else{
					current = servo_val * mcconf->lo_current_motor_max_now;
				}
				
			} else {
				current = fabsf(servo_val * mcconf->lo_current_motor_min_now);
				current_mode_brake = true;
			}
			if (servo_val < 0.001) {
				pulses_without_power++;
			}
			break;
		case PPM_CTRL_TYPE_PID_NOACCELERATION:
			current_mode = true;
			
			static volatile float rpm_filter_buffer[RPM_FILTER_SAMPLES];
			static volatile int rpm_filter_ptr = 0;
			static volatile float rpm_sum = 0.0;
			
			//update the array to get the average rpm
			rpm_sum += mid_rpm - rpm_filter_buffer[rpm_filter_ptr];
	        rpm_filter_buffer[rpm_filter_ptr++] = mid_rpm;
	        if(rpm_filter_ptr == RPM_FILTER_SAMPLES) rpm_filter_ptr = 0;
	        float mid_rpm_filtered = rpm_sum / RPM_FILTER_SAMPLES;
			if (servo_val >= 0.0) {
				// check if pid needs to be lowered
				if (servo_val > 0.0) {
					// needs to be set first ?
					if (pid_rpm == 0 && mid_rpm_filtered < mcconf->l_max_erpm) {
						pid_rpm = mid_rpm_filtered;
					}
					
					if(mid_rpm_filtered > mcconf->s_pid_min_erpm){
						float diff = pid_rpm - mid_rpm_filtered;
						if (diff > 1500) {
							pid_rpm -= 10;
						}else if(diff > 500 && mid_rpm_filtered < 1500) {
							pid_rpm -= 10;
						}	
					}else{
						pid_rpm = 0;
					}
					
					// if not too slow than set the pid
					if(pid_rpm > 0) {
						current_mode = false;
						
						send_pid = true;
						mc_interface_set_pid_speed(rpm_local + pid_rpm - mid_rpm);

					} else {
						servo_val = 0.0;
						current = 0.0;
					}
				}else{
					current = 0.0;
					servo_val = 0.0;
				}				
			} else {
				current = fabsf(servo_val * mcconf->lo_current_motor_min_now);
				current_mode_brake = true;
			}
			
			if (servo_val < 0.001) {
				pulses_without_power++;
			}
			break;
		case PPM_CTRL_TYPE_DUTY:
		case PPM_CTRL_TYPE_DUTY_NOREV:
			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}

			if (!(pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start)) {
				mc_interface_set_duty(utils_map(servo_val, -1.0, 1.0, -mcconf->l_max_duty, mcconf->l_max_duty));
				send_current = true;
			}
			break;

		case PPM_CTRL_TYPE_PID:
		case PPM_CTRL_TYPE_PID_NOREV:
			if (fabsf(servo_val) < 0.001) {
				pulses_without_power++;
			}

			if (!(pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start)) {
				mc_interface_set_pid_speed(servo_val * config.pid_max_erpm);
				send_current = true;
			}
			break;

		default:
			continue;
		}

		if (pulses_without_power < MIN_PULSES_WITHOUT_POWER && config.safe_start) {
			static int pulses_without_power_before = 0;
			if (pulses_without_power == pulses_without_power_before) {
				pulses_without_power = 0;
			}
			pulses_without_power_before = pulses_without_power;
			mc_interface_set_brake_current(timeout_get_brake_current());
			continue;
		}

		if (send_current && config.multi_esc) {
			float current = mc_interface_get_tot_current_directional_filtered();

			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);

				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
					comm_can_set_current(msg->id, current);
				}
			}
		}

		if (send_pid && config.multi_esc) {
			
			for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
				can_status_msg *msg = comm_can_get_status_msg_index(i);

				if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
					comm_can_set_rpm(msg->id, msg->rpm + pid_rpm - mid_rpm, cruise_control_status);
				}
			}
		}

		if (current_mode) {

			pid_rpm = 0; // always reset in current, not that something stupid happens

			if (current_mode_brake) {
				mc_interface_set_brake_current(current);

				if (config.multi_esc) {
					// Send brake command to all ESCs seen recently on the CAN bus
					for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
						can_status_msg *msg = comm_can_get_status_msg_index(i);

						if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
							comm_can_set_current_brake_rel(msg->id, fabsf(servo_val));
						}
					}
				}
			} else {
				float current_out = current;
				float servo_val_out = servo_val;
				bool is_reverse = false;
				if (current_out < 0.0) {
					is_reverse = true;
					current_out = -current_out;
					current = -current;
					servo_val_out = -servo_val_out;
					servo_val = -servo_val;
					rpm_local = -rpm_local;
					rpm_lowest = -rpm_lowest;
				}

				// Traction control
				if (config.multi_esc) {
					for (int i = 0;i < CAN_STATUS_MSGS_TO_STORE;i++) {
						can_status_msg *msg = comm_can_get_status_msg_index(i);

						if (msg->id >= 0 && UTILS_AGE_S(msg->rx_time) < MAX_CAN_AGE) {
							if (config.tc) {
								float rpm_tmp = msg->rpm;
								if (is_reverse) {
									rpm_tmp = -rpm_tmp;
								}

								float diff = rpm_tmp - rpm_lowest;
								if (diff > config.tc_offset) {
									current_out = utils_map(diff - config.tc_offset, 0.0, config.tc_max_diff - config.tc_offset, current, 0.0);
									servo_val_out = utils_map(diff - config.tc_offset, 0.0, config.tc_max_diff - config.tc_offset, servo_val, 0.0);
								} else {
									current_out = current;
									servo_val_out = servo_val;
								}
							}

							if (is_reverse) {
								comm_can_set_current_rel(msg->id, -servo_val_out);
							} else {
								comm_can_set_current_rel(msg->id, servo_val_out);
							}
						}
					}

					if (config.tc) {
						float diff = rpm_local - rpm_lowest;
						if (diff > config.tc_offset) {
							current_out = utils_map(diff - config.tc_offset, 0.0, config.tc_max_diff - config.tc_offset, current, 0.0);
						} else {
							current_out = current;
						}
						if (current_out < mcconf->cc_min_current) {
							current_out = 0.0;
						}
					}
				}

				if (is_reverse) {
					mc_interface_set_current(-current_out);
				} else {
					mc_interface_set_current(current_out);
				}
			}
		}
	}
}
#endif

// Transmission Code
static uint32_t switch_erpm = 100000;
static bool turbo = false;
virtual_timer_t* ppm_timer;

static THD_FUNCTION(transmission_thread, arg);
static THD_WORKING_AREA(transmission_thread_wa, 2048);

void app_transmission_start() {
	palSetPadMode(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_MODE_OUTPUT_OPENDRAIN);
	chThdCreateStatic(transmission_thread_wa, sizeof(transmission_thread_wa), NORMALPRIO, transmission_thread, NULL);
}

void app_transmission_stop(void) {
	turbo = false;
}

void app_transmission_configure(uint32_t erpm) {
	switch_erpm = erpm;
}

static THD_FUNCTION(transmission_thread, arg) {
	(void)arg;

	chRegSetThreadName("APP_TRANSMISSION");

	for (;;) {
		if (mc_interface_get_rpm() > switch_erpm && !turbo) {
			// pause thread
			chSysLockFromISR();
			chVTResetI(&vt);
			chSysUnlockFromISR();
			// cut power
			mc_interface_set_current(0);
			// output signal
			palWritePad(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_HIGH);
			// wait for relay
			chThdSleepMilliseconds(25);
			// resume thread
			chSysLockFromISR();
			chVTSetI(&vt, MS2ST(1), update, NULL);
			chSysUnlockFromISR();

			turbo = true;
		} else {
			if (mc_interface_get_rpm() < switch_erpm && turbo) {
				// pause thread
				chSysLockFromISR();
				chVTResetI(&vt);
				chSysUnlockFromISR();
				// cut power
				mc_interface_set_current(0);
				// output signal
				palWritePad(HW_UART_TX_PORT, HW_UART_TX_PIN, PAL_LOW);
				// wait for relay
				chThdSleepMilliseconds(25);
				// resume thread
				chSysLockFromISR();
				chVTSetI(&vt, MS2ST(1), update, NULL);
				chSysUnlockFromISR();

				turbo = false;
			}
		}

		chThdSleepMilliseconds(250);
	}
}