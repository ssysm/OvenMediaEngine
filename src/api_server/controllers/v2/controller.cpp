//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "./controller.h"

#include "./metrics/metrics_controller.h"

namespace api::v2
{
	Controller::Controller(const cfg::mgr::api::openmetrics::Cache &openmetrics_cache_config)
		: _openmetrics_cache_config(openmetrics_cache_config)
	{
	}

	void Controller::PrepareHandlers()
	{
		CreateSubController<MetricsController>(R"(\/metrics)", _openmetrics_cache_config);
	}
}  // namespace api::v2
