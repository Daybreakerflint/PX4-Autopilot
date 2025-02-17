/****************************************************************************
 *
 *   Copyright (c) 2019-2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
// ToDo: 20230503 Implement a battery check for a tethered system
#include "batteryCheck.hpp"

#include <px4_platform_common/events.h>

using namespace time_literals;

using battery_fault_reason_t = events::px4::enums::battery_fault_reason_t;
static_assert(battery_status_s::BATTERY_FAULT_COUNT == (static_cast<uint8_t>(battery_fault_reason_t::_max) + 1)
	      , "Battery fault flags mismatch!");

static constexpr const char *battery_fault_reason_str(battery_fault_reason_t battery_fault_reason)
{
	switch (battery_fault_reason) {
	case battery_fault_reason_t::deep_discharge: return "under voltage";

	case battery_fault_reason_t::voltage_spikes: return "over voltage";

	case battery_fault_reason_t::cell_fail: return "cell fault";

	case battery_fault_reason_t::over_current: return "over current";

	case battery_fault_reason_t::fault_temperature: return "critical temperature";

	case battery_fault_reason_t::under_temperature: return "under temperature";

	case battery_fault_reason_t::incompatible_voltage: return "voltage mismatch";

	case battery_fault_reason_t::incompatible_firmware: return "incompatible firmware";

	case battery_fault_reason_t::incompatible_model: return "incompatible model";

	case battery_fault_reason_t::hardware_fault: return "hardware fault";

	case battery_fault_reason_t::over_temperature: return "near temperature limit";

	}

	return "";
};


using battery_mode_t = events::px4::enums::battery_mode_t;
static_assert(battery_status_s::BATTERY_MODE_COUNT == (static_cast<uint8_t>(battery_mode_t::_max) + 1)
	      , "Battery mode flags mismatch!");
static constexpr const char *battery_mode_str(battery_mode_t battery_mode)
{
	switch (battery_mode) {
	case battery_mode_t::autodischarging: return "auto discharging";

	case battery_mode_t::hotswap: return "hot-swap";

	default: return "unknown";
	}
}


BatteryChecks::BatteryChecks()
{
	// ToDo: Set default to false - 20230503 used for debugging the system
	int32_t value = true;

	param_t ph = param_find("COM_IS_TETHERED");
	if (ph != PARAM_INVALID && param_get(ph, &value) == PX4_OK) {

		_is_tethered = (bool)value;
	}

	if (_is_tethered) {
		ph = param_find("TETHER_CURR_TOL");
		float fvalue = 0.90f;
		if (ph != PARAM_INVALID && param_get(ph, &fvalue) == PX4_OK) {
			_tether_current_tolerance = fvalue;
		}
	}
}


void BatteryChecks::checkAndReport(const Context &context, Report &reporter)
{
	int battery_required_count = 0;
	bool battery_has_fault = false;
	// There are possibly multiple batteries, and we can't know which ones serve which purpose. So the safest
	// option is to check if ANY of them have a warning, and specifically find which one has the most
	// urgent warning.
	uint8_t worst_warning = battery_status_s::BATTERY_WARNING_NONE;
	float worst_battery_remaining = 1.f;
	uint8_t tether_warning = battery_status_s::BATTERY_WARNING_NONE;
	// To make sure that all connected batteries are being regularly reported, we check which one has the
	// oldest timestamp.
	hrt_abstime oldest_update = hrt_absolute_time();
	float worst_battery_time_s{NAN};
	int num_connected_batteries{0};


	for (auto &battery_sub : _battery_status_subs) {
		int index = battery_sub.get_instance();
		battery_status_s battery;

		if (!battery_sub.copy(&battery)) {
			continue;
		}

		if (battery.is_required) {
			battery_required_count++;
		}

		if (!_last_armed && context.isArmed()) {
			_battery_connected_at_arming[index] = battery.connected;
		}

		if (context.isArmed()) {

			if (!battery.connected && _battery_connected_at_arming[index]) { // If disconnected after arming
				/* EVENT
				 */
				reporter.healthFailure<uint8_t>(NavModes::All, health_component_t::battery, events::ID("check_battery_disconnected"),
								events::Log::Emergency, "Battery {1} disconnected", index + 1);

				if (reporter.mavlink_log_pub()) {
					mavlink_log_critical(reporter.mavlink_log_pub(), "Battery %i disconnected\t", index + 1);
				}

				// trigger a battery failsafe action if a battery disconnects in flight
				worst_warning = battery_status_s::BATTERY_WARNING_CRITICAL;
			}

			if (battery.mode != 0) {
				/* EVENT
				 */
				reporter.healthFailure<uint8_t, events::px4::enums::battery_mode_t>(NavModes::All, health_component_t::battery,
						events::ID("check_battery_mode"),
						events::Log::Critical, "Battery {1} mode: {2}", index + 1, static_cast<battery_mode_t>(battery.mode));

				if (reporter.mavlink_log_pub()) {
					mavlink_log_critical(reporter.mavlink_log_pub(), "Battery %d is in %s mode!\t", index + 1,
							     battery_mode_str(static_cast<battery_mode_t>(battery.mode)));
				}
			}
		}

		if (battery.connected) {
			if (_is_tethered) {
				_battery_currents[num_connected_batteries] = battery.current_filtered_a;
			}

			++num_connected_batteries;

			if (battery.warning > worst_warning) {
				worst_warning = battery.warning;
			}

			if (battery.remaining < worst_battery_remaining) {
				worst_battery_remaining = battery.remaining;
			}

			if (battery.timestamp < oldest_update) {
				oldest_update = battery.timestamp;
			}

			if (battery.faults > 0) {
				battery_has_fault = true;

				for (uint8_t fault_index = 0; fault_index <= static_cast<uint8_t>(battery_fault_reason_t::_max); fault_index++) {
					if (battery.faults & (1 << fault_index)) {
						events::px4::enums::suggested_action_t action = context.isArmed() ?
								events::px4::enums::suggested_action_t::land :
								events::px4::enums::suggested_action_t::none;

						/* EVENT
						 * @description
						 * The battery reported a failure which might be dangerous to fly with.
						 * Manufacturer error code: {4}
						 */
						reporter.healthFailure<uint8_t, battery_fault_reason_t, events::px4::enums::suggested_action_t, uint32_t>
						(NavModes::All, health_component_t::battery, events::ID("check_battery_fault"), {events::Log::Emergency, events::LogInternal::Warning},
						 "Battery {1}: {2}. {3}", index + 1, static_cast<battery_fault_reason_t>(fault_index), action, battery.custom_faults);

						if (reporter.mavlink_log_pub()) {
							mavlink_log_emergency(reporter.mavlink_log_pub(), "Battery %d: %s. %s \t", index + 1,
									      battery_fault_reason_str(
										      static_cast<battery_fault_reason_t>(fault_index)),
									      context.isArmed() ? "Land now!" : "");
						}
					}
				}
			}

			if (PX4_ISFINITE(battery.time_remaining_s)
			    && (!PX4_ISFINITE(worst_battery_time_s)
				|| (PX4_ISFINITE(worst_battery_time_s) && (battery.time_remaining_s < worst_battery_time_s)))) {
				worst_battery_time_s = battery.time_remaining_s;
			}
		}
	}

	if (_is_tethered) {
		// check if there are 2 batteries connected
		if (num_connected_batteries == 2) {
			if (_battery_currents[0] <= _battery_currents[1]) {
				float temp = _battery_currents[0];
				_battery_currents[0] = _battery_currents[1];
				_battery_currents[1] = temp;
			}

			if ((_battery_currents[0] - _battery_currents[1]) < (_battery_currents[0] * _tether_current_tolerance)) {
				tether_warning = battery_status_s::BATTERY_WARNING_EMERGENCY;
				if (reporter.mavlink_log_pub()) {
					mavlink_log_emergency(reporter.mavlink_log_pub(), "Tether Fail: Running on emergency battery");
				}
			}

		} else if (num_connected_batteries == 1) {
			// Tether system or battery not connected
			// Something is either disconnected or faulty
			tether_warning = battery_status_s::BATTERY_WARNING_EMERGENCY;

			if (reporter.mavlink_log_pub()) {
				mavlink_log_emergency(reporter.mavlink_log_pub(), "Tether Fail: Tether or battery disconnected");
			}
		} else if (num_connected_batteries > 2) {
			// Cannot handle more then 2 sources as of now
			tether_warning = battery_status_s::BATTERY_WARNING_FAILED;

			if (reporter.mavlink_log_pub()) {
				mavlink_log_critical(reporter.mavlink_log_pub(), "Tether fail: %d connected -> Max 2 allowed", num_connected_batteries);
			}
		} else {
			// No battery is detected
			tether_warning = battery_status_s::BATTERY_WARNING_FAILED;

			if (reporter.mavlink_log_pub()) {
				mavlink_log_critical(reporter.mavlink_log_pub(), "Tether fail: No battery detected");
			}
		}

		if (tether_warning > worst_warning) {
			worst_warning = tether_warning;
		}
	}

	if (context.isArmed()) {
		// if armed, only allow increase of battery warning severity
		if (worst_warning > reporter.failsafeFlags().battery_warning) {
			reporter.failsafeFlags().battery_warning = worst_warning;
		}

	} else {
		reporter.failsafeFlags().battery_warning = worst_warning;
	}

	if (reporter.failsafeFlags().battery_warning > battery_status_s::BATTERY_WARNING_NONE
	    && reporter.failsafeFlags().battery_warning < battery_status_s::BATTERY_WARNING_FAILED) {
		const bool critical_or_higher = reporter.failsafeFlags().battery_warning >= battery_status_s::BATTERY_WARNING_CRITICAL;
		NavModes affected_modes = critical_or_higher ? NavModes::All : NavModes::None;
		events::LogLevel log_level = critical_or_higher ? events::Log::Critical : events::Log::Warning;
		/* EVENT
		 * @description
		 * The battery state of charge of the worst battery is below the warning threshold.
		 *
		 * <profile name="dev">
		 * This check can be configured via <param>BAT_LOW_THR</param>, <param>BAT_CRIT_THR</param> and <param>BAT_EMERGEN_THR</param> parameters.
		 * </profile>
		 */
		reporter.armingCheckFailure(affected_modes, health_component_t::battery, events::ID("check_battery_low"), log_level,
					    "Low battery");

		if (reporter.mavlink_log_pub()) {
			mavlink_log_emergency(reporter.mavlink_log_pub(), "Low battery level\t");
		}

	} else if (!context.isArmed() && _param_arm_battery_level_min.get() > FLT_EPSILON
		   && worst_battery_remaining < _param_arm_battery_level_min.get()) {
		// if not armed, additionally check if the battery is below the separately configurable preflight threshold
		/* EVENT
		 * @description
		 * The battery state of charge of the worst battery is below the preflight threshold.
		 *
		 * <profile name="dev">
		 * This check can be configured via <param>COM_ARM_BAT_MIN</param> parameter.
		 * </profile>
		 */
		reporter.armingCheckFailure(NavModes::All, health_component_t::battery, events::ID("check_battery_preflight_low"),
					    events::Log::Critical,
					    "Low battery");

		if (reporter.mavlink_log_pub()) {
			mavlink_log_emergency(reporter.mavlink_log_pub(), "Low battery level\t");
		}
	}

	rtlEstimateCheck(context, reporter, worst_battery_time_s);

	reporter.failsafeFlags().battery_unhealthy =
		// All connected batteries are regularly being published
		hrt_elapsed_time(&oldest_update) > 5_s
		// There is at least one connected battery (in any slot)
		|| num_connected_batteries < battery_required_count
		// No currently-connected batteries have any fault
		|| battery_has_fault
		|| reporter.failsafeFlags().battery_warning == battery_status_s::BATTERY_WARNING_FAILED;

	if (reporter.failsafeFlags().battery_unhealthy && !battery_has_fault) { // faults are reported above already
		/* EVENT
		 * @description
		 * Make sure all batteries are connected and operational.
		 */
		reporter.healthFailure(NavModes::All, health_component_t::battery, events::ID("check_battery_unhealthy"),
				       events::Log::Error, "Battery unhealthy");

		if (reporter.mavlink_log_pub()) {
			mavlink_log_critical(reporter.mavlink_log_pub(), "Preflight Fail: Battery unhealthy");
		}
	}

	if (num_connected_batteries > 0) {
		reporter.setIsPresent(health_component_t::battery);
	}

	_last_armed = context.isArmed();
}

void BatteryChecks::rtlEstimateCheck(const Context &context, Report &reporter, float worst_battery_time_s)
{
	rtl_time_estimate_s rtl_time_estimate;

	// Compare estimate of RTL time to estimate of remaining flight time
	reporter.failsafeFlags().battery_low_remaining_time = _rtl_time_estimate_sub.copy(&rtl_time_estimate)
			&& (hrt_absolute_time() - rtl_time_estimate.timestamp) < 2_s
			&& rtl_time_estimate.valid
			&& context.isArmed()
			&& PX4_ISFINITE(worst_battery_time_s)
			&& rtl_time_estimate.safe_time_estimate >= worst_battery_time_s;

	if (reporter.failsafeFlags().battery_low_remaining_time) {
		/* EVENT
		 */
		reporter.armingCheckFailure(NavModes::All, health_component_t::battery, events::ID("check_battery_rem_flight_time_low"),
					    events::Log::Error, "Remaining flight time low");
	}

}
