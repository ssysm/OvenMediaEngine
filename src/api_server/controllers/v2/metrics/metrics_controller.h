//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <config/items/managers/api/openmetrics/cache.h>
#include <monitoring/openmetrics/openmetrics_cache.h>

#include "../../controller_base.h"
#include "openmetrics_renderer.h"

namespace api::v2
{
	class MetricsController : public ControllerBase<MetricsController>
	{
	public:
		MetricsController();
		explicit MetricsController(const cfg::mgr::api::openmetrics::Cache &cache_config);

		void PrepareHandlers() override;

		static bool AcceptsGzip(const ov::String &accept_encoding);
		static ov::String ExtractRawQuery(const std::shared_ptr<http::svr::HttpRequest> &request);

	protected:
		enum class ScopeLevel
		{
			Root,
			VHost,
			App,
			Stream,
			Sessions
		};

		void RegisterMetricsRoute(const ov::String &pattern, ScopeLevel level);
		http::svr::NextHandler HandleMetricsRequest(
			const std::shared_ptr<http::svr::HttpExchange> &client,
			ScopeLevel level);

		mon::openmetrics::ScrapeCache _cache;
	};
}  // namespace api::v2
