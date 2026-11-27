//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <monitoring/monitoring.h>
#include <monitoring/openmetrics/openmetrics_collectors.h>
#include <monitoring/openmetrics/openmetrics_formatter.h>

#include <memory>
#include <string>
#include <vector>

namespace api::v2
{
	struct OpenMetricsScope
	{
		bool is_root = false;
		std::shared_ptr<mon::HostMetrics> vhost;
		std::shared_ptr<mon::ApplicationMetrics> app;
		std::shared_ptr<mon::StreamMetrics> stream;
		std::vector<std::shared_ptr<mon::StreamMetrics>> output_streams;
		uint32_t collector_mask = mon::openmetrics::kDefaultCollectors;
	};

	class OpenMetricsRenderer
	{
	public:
		static std::string Render(const OpenMetricsScope &scope);

		static const char *ProviderLabel(ProviderType type);
		static const char *PublisherLabel(PublisherType type);
		static const char *MediaTypeLabel(cmn::MediaType type);
		static std::string CodecLabel(cmn::MediaCodecId codec_id);
	};
}  // namespace api::v2
