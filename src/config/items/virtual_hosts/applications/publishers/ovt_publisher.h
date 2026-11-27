//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "publisher.h"

namespace cfg
{
	namespace vhost
	{
		namespace app
		{
			namespace pub
			{
				struct OvtPublisher : public Publisher
				{
					struct FrameLatency : public Item
					{
					protected:
						bool _enable = true;

					public:
						CFG_DECLARE_CONST_REF_GETTER_OF(IsEnabled, _enable)

					protected:
						void MakeList() override
						{
							Register<Optional>("Enable", &_enable);
						}
					};

					PublisherType GetType() const override
					{
						return PublisherType::Ovt;
					}

					CFG_DECLARE_CONST_REF_GETTER_OF(GetFrameLatency, _frame_latency)

				protected:
					void MakeList() override
					{
						Publisher::MakeList();
						Register<Optional>("FrameLatency", &_frame_latency);
					}

					FrameLatency _frame_latency;
				};
			}  // namespace pub
		}  // namespace app
	}  // namespace vhost
}  // namespace cfg