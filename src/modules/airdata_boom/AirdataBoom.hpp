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

#pragma once

#include <px4_platform_common/defines.h>
#include <px4_platform_common/module.h>
#include <px4_platform_common/module_params.h>
#include <px4_platform_common/px4_work_queue/ScheduledWorkItem.hpp>

#include <drivers/drv_hrt.h>
#include <lib/perf/perf_counter.h>

#include <uORB/Subscription.hpp>
#include <uORB/SubscriptionMultiArray.hpp>
#include <uORB/topics/sensor_position.h>
#include <uORB/topics/airspeed.h>
#include <uORB/topics/airspeed_validated.h>
#include <uORB/Publication.hpp>
#include <uORB/topics/airdata_boom.h>

using namespace time_literals;

static constexpr uint32_t SCHEDULE_INTERVAL{50_ms};	/**< The schedule interval in usec (20 Hz) */

class AirdataBoom : public ModuleBase<AirdataBoom>, public ModuleParams, public px4::ScheduledWorkItem
{
public:
	AirdataBoom();
	~AirdataBoom() override;

	/** @see ModuleBase */
	static int task_spawn(int argc, char *argv[]);

	/** @see ModuleBase */
	static int custom_command(int argc, char *argv[]);

	/** @see ModuleBase */
	static int print_usage(const char *reason = nullptr);

	bool init();

	int print_status() override;

private:
	void Run() override;

	static constexpr int MAX_NUM_POSITION_SENSORS = 2; /**< Support max 2 position sensors */
	static constexpr int MAX_NUM_AIRSPEED_SENSORS = 1; /**< Support max 1 airspeed sensors */

	// Publications
	uORB::Publication<airdata_boom_s> _airdata_boom_pub{ORB_ID(airdata_boom)}; /**< airdata boom topic*/

	// Subscriptions
	uORB::SubscriptionMultiArray<sensor_position_s, MAX_NUM_POSITION_SENSORS> _sensor_position_sub{ORB_ID::sensor_position}; // regular subscription for additional data
	uORB::SubscriptionMultiArray<airspeed_s, MAX_NUM_AIRSPEED_SENSORS> _airspeed_sub{ORB_ID::airspeed};
	uORB::Subscription _airspeed_validated_sub{ORB_ID(airspeed_validated)};

	sensor_position_s aoa;
	sensor_position_s aos;
	airspeed_s airspeed_raw;
	airspeed_validated_s airspeed_validated;

	// Performance (perf) counters
	perf_counter_t	_loop_perf{perf_alloc(PC_ELAPSED, MODULE_NAME": cycle")};
	perf_counter_t	_loop_interval_perf{perf_alloc(PC_INTERVAL, MODULE_NAME": interval")};

	void publish(); /**< publish airdata */
};
