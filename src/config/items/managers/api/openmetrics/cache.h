//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <config/engine/item.h>

namespace cfg
{
	namespace mgr
	{
		namespace api
		{
			namespace openmetrics
			{
				struct Cache : public Item
				{
				protected:
					bool _enable	 = true;
					int _duration_ms = 3000;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(IsEnabled, _enable)
					CFG_DECLARE_CONST_REF_GETTER_OF(GetDurationMs, _duration_ms)

				protected:
					void MakeList() override
					{
						Register<Optional>("Enable", &_enable);
						Register<Optional>("DurationMs", &_duration_ms);
					}
				};
			}  // namespace openmetrics
		}  // namespace api
	}  // namespace mgr
}  // namespace cfg
