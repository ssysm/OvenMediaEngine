//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "../module_template.h"

namespace cfg
{
	namespace modules
	{
		namespace measurements
		{
			struct HTTPRequestTiming : public ModuleTemplate
			{
			public:
				explicit HTTPRequestTiming(bool enable = false)
					: ModuleTemplate(enable)
				{
				}
			};
		}  // namespace measurements
	}  // namespace modules
}  // namespace cfg
