//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <config/items/managers/api/openmetrics/cache.h>

#include "../controller_base.h"

namespace api
{
	namespace v2
	{
		class Controller : public ControllerBase<Controller>
		{
		public:
			Controller() = default;
			explicit Controller(const cfg::mgr::api::openmetrics::Cache &openmetrics_cache_config);

			void PrepareHandlers() override;

		protected:
			cfg::mgr::api::openmetrics::Cache _openmetrics_cache_config;
		};
	}  // namespace v2
}  // namespace api
