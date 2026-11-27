//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include "cache.h"

namespace cfg
{
	namespace mgr
	{
		namespace api
		{
			namespace openmetrics
			{
				struct OpenMetrics : public Item
				{
				protected:
					Cache _cache;

				public:
					CFG_DECLARE_CONST_REF_GETTER_OF(GetCache, _cache)

				protected:
					void MakeList() override
					{
						Register<Optional>("Cache", &_cache);
					}
				};
			}  // namespace openmetrics
		}  // namespace api
	}  // namespace mgr
}  // namespace cfg
