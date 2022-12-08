/****************************************************************************
 *
 *   Copyright (c) 2017-2020 PX4 Development Team. All rights reserved.
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
/**
 * @file TECS.cpp
 *
 * @author Paul Riseborough
 */

#include "TECS.hpp"

#include <lib/geo/geo.h>

#include <px4_platform_common/defines.h>

#include "matrix/Matrix.hpp"
#include "matrix/Vector2.hpp"

using math::constrain;
using math::max;
using math::min;

static inline constexpr bool TIMESTAMP_VALID(float dt) { return (PX4_ISFINITE(dt) && dt > FLT_EPSILON);}

void TECSAirspeedFilter::initialize(const float equivalent_airspeed)
{


	_airspeed_state.speed = equivalent_airspeed;
	_airspeed_state.speed_rate = 0.0f;
}

void TECSAirspeedFilter::update(const float dt, const Input &input, const Param &param,
				const bool airspeed_sensor_available)
{
	// Input checking
	if (!TIMESTAMP_VALID(dt)) {
		// Do not update the states.
		PX4_WARN("Time intervall is not valid.");
		return;
	}

	float airspeed;

	if (PX4_ISFINITE(input.equivalent_airspeed) && airspeed_sensor_available) {
		airspeed = input.equivalent_airspeed;

	} else {
		airspeed = param.equivalent_airspeed_trim;
	}

	float airspeed_derivative;

	if (PX4_ISFINITE(input.equivalent_airspeed_rate) && airspeed_sensor_available) {
		airspeed_derivative = input.equivalent_airspeed_rate;

	} else {
		airspeed_derivative = 0.0f;
	}

	/* Filter airspeed and rate using a constant airspeed rate model in a steady state Kalman Filter.
	 We use the gains of the continuous Kalman filter Kc and approximate the discrete version Kalman gain Kd =dt*Kc,
	 since the continuous algebraic Riccatti equation is easier to solve.
	*/

	matrix::Vector2f new_state_predicted;

	new_state_predicted(0) = _airspeed_state.speed + dt * _airspeed_state.speed_rate;
	new_state_predicted(1) = _airspeed_state.speed_rate;

	const float airspeed_noise_inv{1.0f / param.airspeed_measurement_std_dev};
	const float airspeed_rate_noise_inv{1.0f / param.airspeed_rate_measurement_std_dev};
	const float airspeed_rate_noise_inv_squared_process_noise{airspeed_rate_noise_inv *airspeed_rate_noise_inv * param.airspeed_rate_noise_std_dev};
	const float denom{airspeed_noise_inv + airspeed_rate_noise_inv_squared_process_noise};
	const float common_nom{std::sqrt(param.airspeed_rate_noise_std_dev * (2.0f * airspeed_noise_inv + airspeed_rate_noise_inv_squared_process_noise))};

	matrix::Matrix<float, 2, 2> kalman_gain;
	kalman_gain(0, 0) = airspeed_noise_inv * common_nom / denom;
	kalman_gain(0, 1) = airspeed_rate_noise_inv_squared_process_noise / denom;
	kalman_gain(1, 0) = airspeed_noise_inv * airspeed_noise_inv * param.airspeed_rate_noise_std_dev / denom;
	kalman_gain(1, 1) = airspeed_rate_noise_inv_squared_process_noise * common_nom / denom;

	const matrix::Vector2f innovation{(airspeed - new_state_predicted(0)), (airspeed_derivative - new_state_predicted(1))};
	matrix::Vector2f new_state;
	new_state = new_state_predicted + dt * (kalman_gain * (innovation));

	// Clip airspeed at zero
	if (new_state(0) < 0.0f) {
		new_state(0) = 0.0f;
		// calculate input that would result in zero speed.
		const float desired_airspeed_innovation = (-new_state_predicted(0) / dt - kalman_gain(0,
				1) * innovation(1)) / kalman_gain(0,
						0);
		new_state(1) = new_state_predicted(1) + dt * (kalman_gain(1, 0) * desired_airspeed_innovation + kalman_gain(1,
				1) * innovation(1));
	}

	// Update states
	_airspeed_state.speed = new_state(0);
	_airspeed_state.speed_rate = new_state(1);
}

TECSAirspeedFilter::AirspeedFilterState TECSAirspeedFilter::getState() const
{
	return _airspeed_state;
}

void TECSReferenceModel::update(const float dt, const AltitudeReferenceState &setpoint, float altitude,
				const Param &param)
{
	// Input checks
	if (!TIMESTAMP_VALID(dt)) {
		// Do not update the states.
		PX4_WARN("Time intervall is not valid.");
		return;
	}

	if (!PX4_ISFINITE(altitude)) {
		altitude = 0.0f;
	}

	if (PX4_ISFINITE(setpoint.alt_rate)) {
		_alt_rate_ref = setpoint.alt_rate;

	} else {
		_alt_rate_ref = 0.0f;
	}


	// Altitude setpoint reference
	const bool altitude_control_enable{PX4_ISFINITE(setpoint.alt)};
	_alt_control_traj_generator.setMaxJerk(param.jerk_max);
	_alt_control_traj_generator.setMaxAccel(param.vert_accel_limit);
	_alt_control_traj_generator.setMaxVel(fmax(param.max_climb_rate, param.max_sink_rate));

	if (altitude_control_enable) {
		const float target_climbrate = math::min(param.target_climbrate, param.max_climb_rate);
		const float target_sinkrate = math::min(param.target_sinkrate, param.max_sink_rate);

		const float delta_trajectory_to_target_m = setpoint.alt - _alt_control_traj_generator.getCurrentPosition();

		float altitude_rate_target = math::signNoZero<float>(delta_trajectory_to_target_m) *
					     math::trajectory::computeMaxSpeedFromDistance(
						     param.jerk_max, param.vert_accel_limit, fabsf(delta_trajectory_to_target_m), 0.0f);

		altitude_rate_target = math::constrain(altitude_rate_target, -target_sinkrate, target_climbrate);

		_alt_control_traj_generator.updateDurations(altitude_rate_target);
		_alt_control_traj_generator.updateTraj(dt);

	} else {
		_alt_control_traj_generator.reset(0.0f, 0.0f, altitude);
	}
}

TECSReferenceModel::AltitudeReferenceState TECSReferenceModel::getAltitudeReference() const
{
	TECSReferenceModel::AltitudeReferenceState ref{
		.alt = _alt_control_traj_generator.getCurrentPosition(),
		.alt_rate = _alt_control_traj_generator.getCurrentVelocity(),
	};

	return ref;
}

float TECSReferenceModel::getAltitudeRateReference() const
{
	return _alt_rate_ref;
}

void TECSReferenceModel::initialize(const AltitudeReferenceState &state)
{
	AltitudeReferenceState init_state{state};

	if (!PX4_ISFINITE(state.alt)) {
		init_state.alt = 0.0f;
	}

	if (!PX4_ISFINITE(state.alt_rate)) {
		init_state.alt_rate = 0.0f;
	}

	_alt_control_traj_generator.reset(0.0f, init_state.alt_rate, init_state.alt);
}

void TECSControl::initialize()
{
	_ste_rate_error_filter.reset(0.0f);
	resetIntegrals();
}

void TECSControl::update(const float dt, const Setpoint &setpoint, const Input &input, Param &param, const Flag &flag)
{
	// Input checking
	if (!TIMESTAMP_VALID(dt)) {
		// Do not update the states and output.
		PX4_WARN("Time intervall is not valid.");
		return;
	}

	AltitudePitchControl control_setpoint;

	control_setpoint.tas_rate_setpoint = _airspeedControl(setpoint, input, param, flag);

	control_setpoint.altitude_rate_setpoint = _altitudeControl(setpoint, input, param);

	SpecificEnergyRates se{_calcSpecificEnergyRates(control_setpoint, input)};

	_detectUnderspeed(input, param, flag);

	_updatePitchSetpoint(dt, input, se, param, flag);

	_updateThrottleSetpoint(dt, se, param, flag);

	_debug_output.altitude_rate_control = control_setpoint.altitude_rate_setpoint;
	_debug_output.true_airspeed_derivative_control = control_setpoint.tas_rate_setpoint;
	_debug_output.pitch_integrator = _pitch_integ_state;
	_debug_output.throttle_integrator = _throttle_integ_state;
}

TECSControl::STELimit TECSControl::_calculateTotalEnergyRateLimit(const Param &param) const
{
	TECSControl::STELimit limit;
	// Calculate the specific total energy rate limits from the max throttle limits
	limit.STE_rate_max = math::max(param.max_climb_rate, FLT_EPSILON) * CONSTANTS_ONE_G;
	limit.STE_rate_min = - math::max(param.max_sink_rate, FLT_EPSILON) * CONSTANTS_ONE_G;

	return limit;
}

float TECSControl::_airspeedControl(const Setpoint &setpoint, const Input &input, const Param &param,
				    const Flag &flag) const
{
	float airspeed_rate_output{0.0f};

	const STELimit limit{_calculateTotalEnergyRateLimit(param)};

	// calculate the demanded true airspeed rate of change based on first order response of true airspeed error
	// if airspeed measurement is not enabled then always set the rate setpoint to zero in order to avoid constant rate setpoints
	if (flag.airspeed_enabled) {
		// Calculate limits for the demanded rate of change of speed based on physical performance limits
		// with a 50% margin to allow the total energy controller to correct for errors.
		const float max_tas_rate_sp = 0.5f * limit.STE_rate_max / math::max(input.tas, FLT_EPSILON);
		const float min_tas_rate_sp = 0.5f * limit.STE_rate_min / math::max(input.tas, FLT_EPSILON);
		airspeed_rate_output = constrain((setpoint.tas_setpoint - input.tas) * param.airspeed_error_gain, min_tas_rate_sp,
						 max_tas_rate_sp);
	}

	return airspeed_rate_output;
}

float TECSControl::_altitudeControl(const Setpoint &setpoint, const Input &input, const Param &param) const
{
	float altitude_rate_output;
	altitude_rate_output = (setpoint.altitude_reference.alt - input.altitude) * param.altitude_error_gain +
			       param.altitude_setpoint_gain_ff * setpoint.altitude_reference.alt_rate + setpoint.altitude_rate_setpoint;
	altitude_rate_output = math::constrain(altitude_rate_output, -param.max_sink_rate, param.max_climb_rate);

	return altitude_rate_output;
}

TECSControl::SpecificEnergyRates TECSControl::_calcSpecificEnergyRates(const AltitudePitchControl &control_setpoint,
		const Input &input) const
{
	SpecificEnergyRates se;
	// Calculate specific energy rate demands in units of (m**2/sec**3)
	se.spe.rate_setpoint = control_setpoint.altitude_rate_setpoint * CONSTANTS_ONE_G; // potential energy rate of change
	se.ske.rate_setpoint = input.tas * control_setpoint.tas_rate_setpoint; // kinetic energy rate of change

	// Calculate specific energy rates in units of (m**2/sec**3)
	se.spe.rate = input.altitude_rate * CONSTANTS_ONE_G; // potential energy rate of change
	se.ske.rate = input.tas * input.tas_rate;// kinetic energy rate of change

	// Calculate energy rate error
	se.spe.rate_error = se.spe.rate_setpoint - se.spe.rate;
	se.ske.rate_error = se.ske.rate_setpoint - se.ske.rate;

	return se;
}

void TECSControl::_detectUnderspeed(const Input &input, const Param &param, const Flag &flag)
{
	if (!flag.detect_underspeed_enabled) {
		_percent_undersped = 0.0f;
		return;
	}

	// this is the expected (something like standard) deviation from the airspeed setpoint that we allow the airspeed
	// to vary in before ramping in underspeed mitigation
	const float tas_error_bound = param.tas_error_percentage * param.equivalent_airspeed_trim;

	// this is the soft boundary where underspeed mitigation is ramped in
	// NOTE: it's currently the same as the error bound, but separated here to indicate these values do not in general
	// need to be the same
	const float tas_underspeed_soft_bound = param.tas_error_percentage * param.equivalent_airspeed_trim;

	const float tas_fully_undersped = math::max(param.tas_min - tas_error_bound - tas_underspeed_soft_bound, 0.0f);
	const float tas_starting_to_underspeed = math::max(param.tas_min - tas_error_bound, tas_fully_undersped);

	_percent_undersped = 1.0f - math::constrain((input.tas - tas_fully_undersped) /
			     math::max(tas_starting_to_underspeed - tas_fully_undersped, FLT_EPSILON), 0.0f, 1.0f);
}

TECSControl::SpecificEnergyWeighting TECSControl::_updateSpeedAltitudeWeights(const Param &param, const Flag &flag)
{

	SpecificEnergyWeighting weight;
	// Calculate the weight applied to control of specific kinetic energy error
	float pitch_speed_weight = constrain(param.pitch_speed_weight, 0.0f, 2.0f);

	if (flag.climbout_mode_active && flag.airspeed_enabled) {
		pitch_speed_weight = 2.0f;

	} else if (_percent_undersped > FLT_EPSILON && flag.airspeed_enabled) {
		pitch_speed_weight = 2.0f * _percent_undersped + (1.0f - _percent_undersped) * pitch_speed_weight;

	} else if (!flag.airspeed_enabled) {
		pitch_speed_weight = 0.0f;

	}

	// don't allow any weight to be larger than one, as it has the same effect as reducing the control
	// loop time constant and therefore can lead to a destabilization of that control loop
	weight.spe_weighting = constrain(2.0f - pitch_speed_weight, 0.f, 1.f);
	weight.ske_weighting = constrain(pitch_speed_weight, 0.f, 1.f);

	return weight;
}

void TECSControl::_updatePitchSetpoint(float dt, const Input &input, const SpecificEnergyRates &se, Param &param,
				       const Flag &flag)
{
	const SpecificEnergyWeighting weight{_updateSpeedAltitudeWeights(param, flag)};
	/*
	 * The SKE_weighting variable controls how speed and altitude control are prioritised by the pitch demand calculation.
	 * A weighting of 1 givea equal speed and altitude priority
	 * A weighting of 0 gives 100% priority to altitude control and must be used when no airspeed measurement is available.
	 * A weighting of 2 provides 100% priority to speed control and is used when:
	 * a) an underspeed condition is detected.
	 * b) during climbout where a minimum pitch angle has been set to ensure altitude is gained. If the airspeed
	 * rises above the demanded value, the pitch angle demand is increased by the TECS controller to prevent the vehicle overspeeding.
	 * The weighting can be adjusted between 0 and 2 depending on speed and altitude accuracy requirements.
	*/
	// Calculate the specific energy balance rate demand
	const float seb_rate_setpoint = se.spe.rate_setpoint * weight.spe_weighting - se.ske.rate_setpoint *
					weight.ske_weighting;

	// Calculate the specific energy balance rate error
	const float seb_rate_error = (se.spe.rate_error * weight.spe_weighting) - (se.ske.rate_error * weight.ske_weighting);

	_debug_output.energy_balance_rate_error = seb_rate_error;
	_debug_output.energy_balance_rate_sp = seb_rate_setpoint;

	if (param.integrator_gain_pitch > 0.0f) {
		// Calculate pitch integrator input term
		float pitch_integ_input = seb_rate_error * param.integrator_gain_pitch;

		// Prevent the integrator changing in a direction that will increase pitch demand saturation
		if (_pitch_setpoint >= param.pitch_max) {
			pitch_integ_input = min(pitch_integ_input, 0.f);

		} else if (_pitch_setpoint <= param.pitch_min) {
			pitch_integ_input = max(pitch_integ_input, 0.f);
		}

		// Update the pitch integrator state.
		_pitch_integ_state = _pitch_integ_state + pitch_integ_input * dt;

	} else {
		_pitch_integ_state = 0.0f;
	}

	// Calculate a specific energy correction that doesn't include the integrator contribution
	float SEB_rate_correction = seb_rate_error * param.pitch_damping_gain + _pitch_integ_state + param.seb_rate_ff *
				    seb_rate_setpoint;

	// Calculate derivative from change in climb angle to rate of change of specific energy balance
	const float climb_angle_to_SEB_rate = input.tas * CONSTANTS_ONE_G;

	// During climbout, bias the demanded pitch angle so that a zero speed error produces a pitch angle
	// demand equal to the minimum pitch angle set by the mission plan. This prevents the integrator
	// having to catch up before the nose can be raised to reduce excess speed during climbout.
	if (flag.climbout_mode_active) {
		SEB_rate_correction += param.pitch_min * climb_angle_to_SEB_rate;
	}

	// Convert the specific energy balance rate correction to a target pitch angle. This calculation assumes:
	// a) The climb angle follows pitch angle with a lag that is small enough not to destabilise the control loop.
	// b) The offset between climb angle and pitch angle (angle of attack) is constant, excluding the effect of
	// pitch transients due to control action or turbulence.
	const float pitch_setpoint_unc = SEB_rate_correction / climb_angle_to_SEB_rate;

	const float pitch_setpoint = constrain(pitch_setpoint_unc, param.pitch_min, param.pitch_max);

	// Comply with the specified vertical acceleration limit by applying a pitch rate limit
	// NOTE: at zero airspeed, the pitch increment is unbounded
	const float pitch_increment = dt * param.vert_accel_limit / input.tas;
	_pitch_setpoint = constrain(pitch_setpoint, _pitch_setpoint - pitch_increment,
				    _pitch_setpoint + pitch_increment);
}

void TECSControl::_updateThrottleSetpoint(float dt, const SpecificEnergyRates &se, const Param &param, const Flag &flag)
{
	const STELimit limit{_calculateTotalEnergyRateLimit(param)};

	float STE_rate_setpoint = se.spe.rate_setpoint + se.ske.rate_setpoint;

	// Adjust the demanded total energy rate to compensate for induced drag rise in turns.
	// Assume induced drag scales linearly with normal load factor.
	// The additional normal load factor is given by (1/cos(bank angle) - 1)
	STE_rate_setpoint += param.load_factor_correction * (param.load_factor - 1.f);

	STE_rate_setpoint = constrain(STE_rate_setpoint, limit.STE_rate_min, limit.STE_rate_max);

	_ste_rate = se.spe.rate + se.ske.rate;

	const float STE_rate_error_raw = se.spe.rate_error + se.ske.rate_error;
	_ste_rate_error_filter.setParameters(dt, param.ste_rate_time_const);
	_ste_rate_error_filter.update(STE_rate_error_raw);
	const float STE_rate_error{_ste_rate_error_filter.getState()};

	_debug_output.total_energy_rate_error = STE_rate_error;
	_debug_output.total_energy_rate_sp = STE_rate_setpoint;

	// Calculate gain scaler from specific energy rate error to throttle
	const float STE_rate_to_throttle = 1.0f / (limit.STE_rate_max - limit.STE_rate_min);

	// Integral handling
	if (flag.airspeed_enabled) {
		if (param.integrator_gain_throttle > 0.0f) {
			// underspeed conditions zero out integration
			float throttle_integ_input = (STE_rate_error * param.integrator_gain_throttle) * dt *
						     STE_rate_to_throttle * (1.0f - _percent_undersped);

			// only allow integrator propagation into direction which unsaturates throttle
			if (_throttle_setpoint >= param.throttle_max) {
				throttle_integ_input = math::min(0.f, throttle_integ_input);

			} else if (_throttle_setpoint <= param.throttle_min) {
				throttle_integ_input = math::max(0.f, throttle_integ_input);
			}

			// Calculate a throttle demand from the integrated total energy rate error
			// This will be added to the total throttle demand to compensate for steady state errors
			_throttle_integ_state = _throttle_integ_state + throttle_integ_input;

			if (flag.climbout_mode_active) {
				// During climbout, set the integrator to maximum throttle to prevent transient throttle drop
				// at end of climbout when we transition to closed loop throttle control
				_throttle_integ_state = param.throttle_max - _throttle_setpoint;
			}

		} else {
			_throttle_integ_state = 0.0f;
		}

	}

	// Calculate a predicted throttle from the demanded rate of change of energy, using the cruise throttle
	// as the starting point. Assume:
	// Specific total energy rate = _STE_rate_max is achieved when throttle is set to _throttle_setpoint_max
	// Specific total energy rate = 0 at cruise throttle
	// Specific total energy rate = _STE_rate_min is achieved when throttle is set to _throttle_setpoint_min
	float throttle_predicted = 0.0f;

	if (STE_rate_setpoint >= 0) {
		// throttle is between trim and maximum
		throttle_predicted = param.throttle_trim + STE_rate_setpoint / limit.STE_rate_max *
				     (param.throttle_max - param.throttle_trim);

	} else {
		// throttle is between trim and minimum
		throttle_predicted = param.throttle_trim + STE_rate_setpoint / limit.STE_rate_min *
				     (param.throttle_min - param.throttle_trim);

	}

	// Add proportional and derivative control feedback to the predicted throttle and constrain to throttle limits
	float throttle_setpoint = (STE_rate_error * param.throttle_damping_gain) * STE_rate_to_throttle + throttle_predicted;

	if (flag.airspeed_enabled) {
		// Add the integrator feedback during closed loop operation with an airspeed sensor
		throttle_setpoint += _throttle_integ_state;

	}

	// ramp in max throttle setting with underspeediness value
	throttle_setpoint = _percent_undersped * param.throttle_max + (1.0f - _percent_undersped) * throttle_setpoint;

	// Rate limit the throttle demand
	if (fabsf(param.throttle_slewrate) > 0.01f) {
		const float throttle_increment_limit = dt * (param.throttle_max - param.throttle_min) * param.throttle_slewrate;
		throttle_setpoint = constrain(throttle_setpoint, _throttle_setpoint - throttle_increment_limit,
					      _throttle_setpoint + throttle_increment_limit);
	}

	_throttle_setpoint = constrain(throttle_setpoint, param.throttle_min, param.throttle_max);
}

void TECSControl::resetIntegrals()
{
	_pitch_integ_state = 0.0f;
	_throttle_integ_state = 0.0f;
}

float TECS::_update_speed_setpoint(const float tas_min, const float tas_max, const float tas_setpoint, const float tas)
{
	float new_setpoint{tas_setpoint};
	const float percent_undersped = _control.getPercentUndersped();

	// Set the TAS demand to the minimum value if an underspeed or
	// or a uncontrolled descent condition exists to maximise climb rate
	if (_uncommanded_descent_recovery) {
		new_setpoint = tas_min;

	} else if (percent_undersped > FLT_EPSILON) {
		// TAS setpoint is reset from external setpoint every time tecs is called, so the interpolation is still
		// between current setpoint and mininimum airspeed here (it's not feeding the newly adjusted setpoint
		// from this line back into this method each time).
		// TODO: WOULD BE GOOD to "functionalize" this library a bit and remove many of these internal states to
		// avoid the fear of side effects in simple operations like this.
		new_setpoint = tas_min * percent_undersped + (1.0f - percent_undersped) * tas_setpoint;
	}

	new_setpoint = constrain(new_setpoint, tas_min, tas_max);

	return new_setpoint;
}

void TECS::_detect_uncommanded_descent(float throttle_setpoint_max, float altitude, float altitude_setpoint, float tas,
				       float tas_setpoint)
{
	/*
	 * This function detects a condition that can occur when the demanded airspeed is greater than the
	 * aircraft can achieve in level flight. When this occurs, the vehicle will continue to reduce altitude
	 * while attempting to maintain speed.
	*/

	// Calculate specific energy demands in units of (m**2/sec**2)
	const float SPE_setpoint = altitude_setpoint * CONSTANTS_ONE_G; // potential energy
	const float SKE_setpoint = 0.5f * altitude_setpoint * altitude_setpoint; // kinetic energy

	// Calculate specific energies in units of (m**2/sec**2)
	const float SPE_estimate = altitude * CONSTANTS_ONE_G; // potential energy
	const float SKE_estimate = 0.5f * tas * tas; // kinetic energy

	// Calculate total energy error
	const float SPE_error = SPE_setpoint - SPE_estimate;
	const float SKE_error = SKE_setpoint - SKE_estimate;
	const float STE_error = SPE_error + SKE_error;

	const bool underspeed_detected = _control.getPercentUndersped() > FLT_EPSILON;

	// If total energy is very low and reducing, throttle is high, and we are not in an underspeed condition, then enter uncommanded descent recovery mode
	const bool enter_mode = !_uncommanded_descent_recovery && !underspeed_detected && (STE_error > 200.0f)
				&& (_control.getSteRate() < 0.0f)
				&& (_control.getThrottleSetpoint() >= throttle_setpoint_max * 0.9f);

	// If we enter an underspeed condition or recover the required total energy, then exit uncommanded descent recovery mode
	const bool exit_mode = _uncommanded_descent_recovery && (underspeed_detected || (STE_error < 0.0f));

	if (enter_mode) {
		_uncommanded_descent_recovery = true;

	} else if (exit_mode) {
		_uncommanded_descent_recovery = false;

	}
}

void TECS::initialize(const float altitude, const float altitude_rate, const float equivalent_airspeed)
{
	// Init subclasses
	TECSReferenceModel::AltitudeReferenceState current_state{	.alt = altitude,
			.alt_rate = altitude_rate};
	_reference_model.initialize(current_state);
	_airspeed_filter.initialize(equivalent_airspeed);
	_control.initialize();
}

void TECS::update(float pitch, float altitude, float hgt_setpoint, float EAS_setpoint, float equivalent_airspeed,
		  float eas_to_tas, bool climb_out_setpoint, float pitch_min_climbout, float throttle_min, float throttle_setpoint_max,
		  float throttle_trim, float pitch_limit_min, float pitch_limit_max, float target_climbrate, float target_sinkrate,
		  const float speed_deriv_forward, float hgt_rate, float hgt_rate_sp)
{
	// Calculate the time since last update (seconds)
	const uint64_t now(hrt_absolute_time());
	const float dt = (now - _update_timestamp) * 1e-6f;

	if (dt < DT_MIN) {
		// Update intervall too small, do not update. Assume constant states/output in this case.
		return;
	}

	if (dt > DT_MAX || _update_timestamp == 0UL) {
		// Update time intervall too large, can't guarantee sanity of state updates anymore. reset the control loop.
		initialize(altitude, hgt_rate, equivalent_airspeed);

	} else {
		// Update airspeedfilter submodule
		const TECSAirspeedFilter::Input airspeed_input{ .equivalent_airspeed = equivalent_airspeed,
				.equivalent_airspeed_rate = speed_deriv_forward / eas_to_tas};

		_airspeed_param.equivalent_airspeed_trim = _equivalent_airspeed_trim;

		_airspeed_filter.update(dt, airspeed_input, _airspeed_param, _airspeed_enabled);
		const TECSAirspeedFilter::AirspeedFilterState eas = _airspeed_filter.getState();

		// Update Reference model submodule
		const TECSReferenceModel::AltitudeReferenceState setpoint{ .alt = hgt_setpoint,
				.alt_rate = hgt_rate_sp};

		_reference_param.target_climbrate = target_climbrate;
		_reference_param.target_sinkrate = target_sinkrate;

		_reference_model.update(dt, setpoint, altitude, _reference_param);

		TECSControl::Setpoint control_setpoint;
		control_setpoint.altitude_reference = _reference_model.getAltitudeReference();
		control_setpoint.altitude_rate_setpoint = _reference_model.getAltitudeRateReference();

		// Calculate the demanded true airspeed
		// TODO this function should not be in the module. Only give feedback that the airspeed can't be achieved.
		control_setpoint.tas_setpoint = _update_speed_setpoint(eas_to_tas * _equivalent_airspeed_min,
						eas_to_tas * _equivalent_airspeed_max, EAS_setpoint * eas_to_tas, eas_to_tas * eas.speed);

		const TECSControl::Input control_input{ .altitude = altitude,
							.altitude_rate = hgt_rate,
							.tas = eas_to_tas * eas.speed,
							.tas_rate = eas_to_tas * eas.speed_rate}
		;
		_control_param.equivalent_airspeed_trim = _equivalent_airspeed_trim;
		_control_param.tas_min = eas_to_tas * _equivalent_airspeed_min;
		_control_param.pitch_max = pitch_limit_max;
		_control_param.pitch_min = pitch_limit_min;
		_control_param.throttle_trim = throttle_trim;
		_control_param.throttle_max = throttle_setpoint_max;
		_control_param.throttle_min = throttle_min;

		const TECSControl::Flag control_flag{ .airspeed_enabled = _airspeed_enabled,
						      .climbout_mode_active = climb_out_setpoint,
						      .detect_underspeed_enabled = _detect_underspeed_enabled
						    };
		_control.update(dt, control_setpoint, control_input, _control_param, control_flag);

		// Detect an uncommanded descent caused by an unachievable airspeed demand
		_detect_uncommanded_descent(throttle_setpoint_max, altitude, hgt_setpoint, equivalent_airspeed * eas_to_tas,
					    control_setpoint.tas_setpoint);

		_debug_status.control = _control.getDebugOutput();
		_debug_status.true_airspeed_filtered = eas_to_tas * eas.speed;
		_debug_status.true_airspeed_derivative = eas_to_tas * eas.speed_rate;
		_debug_status.altitude_sp = control_setpoint.altitude_reference.alt;
		_debug_status.altitude_rate = control_setpoint.altitude_reference.alt_rate;
		_debug_status.altitude_rate_setpoint = control_setpoint.altitude_rate_setpoint;
	}

	// Update time stamps
	_update_timestamp = now;

	// Set TECS mode for next frame
	if (_control.getPercentUndersped() > FLT_EPSILON) {
		_tecs_mode = ECL_TECS_MODE_UNDERSPEED;

	} else if (_uncommanded_descent_recovery) {
		_tecs_mode = ECL_TECS_MODE_BAD_DESCENT;

	} else if (climb_out_setpoint) {
		_tecs_mode = ECL_TECS_MODE_CLIMBOUT;

	} else {
		// This is the default operation mode
		_tecs_mode = ECL_TECS_MODE_NORMAL;
	}

	_debug_status.tecs_mode = _tecs_mode;
}

