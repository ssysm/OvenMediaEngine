//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mon::openmetrics
{
	enum class MetricType : uint8_t
	{
		Counter,
		Gauge,
		Histogram,
		Summary,
		Info,
		Stateset,
	};

	const char *MetricTypeToString(MetricType type);

	struct Label
	{
		std::string name;
		std::string value;
	};

	std::string EscapeLabelValue(std::string_view value);
	std::string EscapeHelpText(std::string_view text);
	std::string FormatDouble(double value);
	std::string FormatLeBound(double seconds);

	class OpenMetricsBuilder
	{
	public:
		OpenMetricsBuilder() = default;

		// Declares a metric family (idempotent if already declared with the same name).
		// Note: per OpenMetrics 1.0.0, `family_name` must NOT include `_total` (for Counter)
		// or `_info` (for Info). If `unit` is non-empty, `family_name` should end with `_<unit>`.
		void DeclareFamily(std::string family_name, MetricType type, std::string unit, std::string help);

		void AddSample(const std::string &family_name,
					   std::string_view sample_suffix,
					   const std::vector<Label> &labels,
					   double value);

		void AddSampleUint64(const std::string &family_name,
							 std::string_view sample_suffix,
							 const std::vector<Label> &labels,
							 uint64_t value);

		void AddCounter(const std::string &family_name,
						const std::vector<Label> &labels,
						uint64_t value);

		void AddCounterDouble(const std::string &family_name,
							  const std::vector<Label> &labels,
							  double value);

		void AddGauge(const std::string &family_name,
					  const std::vector<Label> &labels,
					  double value);

		void AddGaugeUint64(const std::string &family_name,
							const std::vector<Label> &labels,
							uint64_t value);

		void AddInfo(const std::string &family_name,
					 const std::vector<Label> &labels);

		void AddStateset(const std::string &family_name,
						 const std::vector<Label> &labels,
						 const std::vector<std::string_view> &states,
						 std::string_view active_state);

		void AddHistogram(const std::string &family_name,
						  const std::vector<Label> &labels,
						  const std::vector<double> &bounds,
						  const std::vector<uint64_t> &cumulative_buckets,
						  uint64_t total_count,
						  double sum_seconds);

		void AddSummary(const std::string &family_name,
						const std::vector<Label> &labels,
						uint64_t count,
						double sum_seconds);

		// Renders all families that have at least one sample, ending with "# EOF\n".
		std::string Finish() const;

	private:
		struct Family
		{
			std::string name;
			MetricType type = MetricType::Gauge;
			std::string unit;
			std::string help;
			std::vector<std::string> lines;
		};

		Family *FindFamily(const std::string &family_name);
		static void AppendLabels(std::string &out, const std::vector<Label> &labels);
		static void AppendLabelsWithExtra(std::string &out, const std::vector<Label> &labels, std::string_view extra_name, std::string_view extra_value);

		std::vector<Family> _families;
		std::map<std::string, size_t> _family_index;
	};
}  // namespace mon::openmetrics
