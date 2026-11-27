//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "frame_latency.h"
#include "http_request_timing.h"

namespace cfg
{
	namespace modules
	{
		namespace measurements
		{
			struct Measurements : public Item
			{
			protected:
				FrameLatency _frame_latency{false};
				HTTPRequestTiming _http_request_timing{false};

			public:
				CFG_DECLARE_CONST_REF_GETTER_OF(GetFrameLatency, _frame_latency)
				CFG_DECLARE_CONST_REF_GETTER_OF(GetHTTPRequestTiming, _http_request_timing)

			protected:
				void MakeList() override
				{
					Register<Optional>("FrameLatency", &_frame_latency);
					Register<Optional>("HTTPRequestTiming", &_http_request_timing);
				}
			};
		}  // namespace measurements
	}  // namespace modules
}  // namespace cfg
