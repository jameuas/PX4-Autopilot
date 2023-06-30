/****************************************************************************
 *
 *   Copyright (c) 2022 PX4 Development Team. All rights reserved.
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

#ifndef MOTOR_ELECTRICAL_SPEED_HPP
#define MOTOR_ELECTRICAL_SPEED_HPP

#include <uORB/topics/rpm.h>

class MavlinkStreamMotorElectricalSpeed : public MavlinkStream
{
public:
	static MavlinkStream *new_instance(Mavlink *mavlink) { return new MavlinkStreamMotorElectricalSpeed(mavlink); }

	static constexpr const char *get_name_static() { return "MOTOR_ELECTRICAL_SPEED"; }
	static constexpr uint16_t get_id_static() { return MAVLINK_MSG_ID_MOTOR_ELECTRICAL_SPEED; }

	const char *get_name() const override { return get_name_static(); }
	uint16_t get_id() override { return get_id_static(); }

	unsigned get_size() override
	{
		return _rpm_sub.advertised() ? MAVLINK_MSG_ID_MOTOR_ELECTRICAL_SPEED_LEN + MAVLINK_NUM_NON_PAYLOAD_BYTES : 0;
	}

private:
	explicit MavlinkStreamMotorElectricalSpeed(Mavlink *mavlink) : MavlinkStream(mavlink) {}

	uORB::Subscription _rpm_sub{ORB_ID(rpm)};

	bool send() override
	{
		rpm_s rpm;

		if (_rpm_sub.update(&rpm)) {
			mavlink_motor_electrical_speed_t msg{};
			msg.front_right_motor = (int)rpm.electrical_speed_rpm[0];
			msg.back_left_motor = (int)rpm.electrical_speed_rpm[1];
			msg.front_left_motor = (int)rpm.electrical_speed_rpm[2];
			msg.back_right_motor = (int)rpm.electrical_speed_rpm[3];
			msg.fixed_wing_motor = (int)rpm.electrical_speed_rpm[4];

			mavlink_msg_motor_electrical_speed_send_struct(_mavlink->get_channel(), &msg);

			return true;
		}

		return false;
	}
};

#endif // MOTOR_ELECTRICAL_SPEED_HPP
