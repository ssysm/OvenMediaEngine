//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2020 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <config/items/managers/api/openmetrics/cache.h>

#include "controller_base.h"

namespace api
{
	class RootController : public ControllerBase<RootController>
	{
	public:
		RootController(const ov::String &access_token, const cfg::mgr::api::openmetrics::Cache &openmetrics_cache_config);

		void PrepareHandlers() override;

	protected:
		void PrepareAccessTokenHandler();
		ApiResponse OnNotFound(const std::shared_ptr<http::svr::HttpExchange> &client);

		ov::String _access_token;
		cfg::mgr::api::openmetrics::Cache _openmetrics_cache_config;
	};
}  // namespace api
