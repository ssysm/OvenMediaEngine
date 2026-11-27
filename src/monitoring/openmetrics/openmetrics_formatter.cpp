//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "openmetrics_formatter.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace mon::openmetrics
{
	const char *MetricTypeToString(MetricType type)
	{
		switch (type)
		{
			case MetricType::Counter:
				return "counter";
			case MetricType::Gauge:
				return "gauge";
			case MetricType::Histogram:
				return "histogram";
			case MetricType::Summary:
				return "summary";
			case MetricType::Info:
				return "info";
			case MetricType::Stateset:
				return "stateset";
		}
		return "unknown";
	}

	std::string EscapeLabelValue(std::string_view value)
	{
		std::string out;
		out.reserve(value.size());
		for (char c : value)
		{
			switch (c)
			{
				case '\\':
					out += "\\\\";
					break;
				case '"':
					out += "\\\"";
					break;
				case '\n':
					out += "\\n";
					break;
				default:
					out += c;
					break;
			}
		}
		return out;
	}

	std::string EscapeHelpText(std::string_view text)
	{
		return EscapeLabelValue(text);
	}

	std::string FormatDouble(double value)
	{
		if (std::isnan(value))
		{
			return "NaN";
		}
		if (std::isinf(value))
		{
			return (value > 0.0) ? "+Inf" : "-Inf";
		}
		if (value == 0.0)
		{
			return "0";
		}

		char buf[64];
		auto res = std::to_chars(buf, buf + sizeof(buf), value);
		if (res.ec == std::errc{})
		{
			return std::string(buf, res.ptr);
		}

		std::snprintf(buf, sizeof(buf), "%.15g", value);
		return std::string(buf);
	}

	std::string FormatLeBound(double seconds)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6f", seconds);
		std::string s(buf);
		auto dot = s.find('.');
		if (dot != std::string::npos)
		{
			while (s.size() > dot + 2 && s.back() == '0')
			{
				s.pop_back();
			}
		}
		return s;
	}

	void OpenMetricsBuilder::DeclareFamily(std::string family_name, MetricType type, std::string unit, std::string help)
	{
		if (_family_index.find(family_name) != _family_index.end())
		{
			return;
		}

		size_t idx = _families.size();
		_family_index.emplace(family_name, idx);
		_families.push_back(Family{std::move(family_name), type, std::move(unit), std::move(help), {}});
	}

	OpenMetricsBuilder::Family *OpenMetricsBuilder::FindFamily(const std::string &family_name)
	{
		auto it = _family_index.find(family_name);
		if (it == _family_index.end())
		{
			return nullptr;
		}
		return &_families[it->second];
	}

	void OpenMetricsBuilder::AppendLabels(std::string &out, const std::vector<Label> &labels)
	{
		if (labels.empty())
		{
			return;
		}
		out += '{';
		for (size_t i = 0; i < labels.size(); ++i)
		{
			if (i > 0)
			{
				out += ',';
			}
			out += labels[i].name;
			out += "=\"";
			out += EscapeLabelValue(labels[i].value);
			out += '"';
		}
		out += '}';
	}

	void OpenMetricsBuilder::AppendLabelsWithExtra(std::string &out,
												   const std::vector<Label> &labels,
												   std::string_view extra_name,
												   std::string_view extra_value)
	{
		out += '{';
		for (size_t i = 0; i < labels.size(); ++i)
		{
			if (i > 0)
			{
				out += ',';
			}
			out += labels[i].name;
			out += "=\"";
			out += EscapeLabelValue(labels[i].value);
			out += '"';
		}
		if (!labels.empty())
		{
			out += ',';
		}
		out.append(extra_name.data(), extra_name.size());
		out += "=\"";
		out += EscapeLabelValue(extra_value);
		out += "\"}";
	}

	void OpenMetricsBuilder::AddSample(const std::string &family_name,
									   std::string_view sample_suffix,
									   const std::vector<Label> &labels,
									   double value)
	{
		auto *fam = FindFamily(family_name);
		if (fam == nullptr)
		{
			return;
		}

		std::string line;
		line.reserve(family_name.size() + sample_suffix.size() + 64);
		line += family_name;
		line.append(sample_suffix.data(), sample_suffix.size());
		AppendLabels(line, labels);
		line += ' ';
		line += FormatDouble(value);
		fam->lines.push_back(std::move(line));
	}

	void OpenMetricsBuilder::AddSampleUint64(const std::string &family_name,
											 std::string_view sample_suffix,
											 const std::vector<Label> &labels,
											 uint64_t value)
	{
		auto *fam = FindFamily(family_name);
		if (fam == nullptr)
		{
			return;
		}

		std::string line;
		line.reserve(family_name.size() + sample_suffix.size() + 64);
		line += family_name;
		line.append(sample_suffix.data(), sample_suffix.size());
		AppendLabels(line, labels);
		line += ' ';
		line += std::to_string(value);
		fam->lines.push_back(std::move(line));
	}

	void OpenMetricsBuilder::AddCounter(const std::string &family_name,
										const std::vector<Label> &labels,
										uint64_t value)
	{
		AddSampleUint64(family_name, "_total", labels, value);
	}

	void OpenMetricsBuilder::AddCounterDouble(const std::string &family_name,
											  const std::vector<Label> &labels,
											  double value)
	{
		AddSample(family_name, "_total", labels, value);
	}

	void OpenMetricsBuilder::AddGauge(const std::string &family_name,
									  const std::vector<Label> &labels,
									  double value)
	{
		AddSample(family_name, "", labels, value);
	}

	void OpenMetricsBuilder::AddGaugeUint64(const std::string &family_name,
											const std::vector<Label> &labels,
											uint64_t value)
	{
		AddSampleUint64(family_name, "", labels, value);
	}

	void OpenMetricsBuilder::AddInfo(const std::string &family_name,
									 const std::vector<Label> &labels)
	{
		AddSampleUint64(family_name, "_info", labels, 1);
	}

	void OpenMetricsBuilder::AddStateset(const std::string &family_name,
										 const std::vector<Label> &labels,
										 const std::vector<std::string_view> &states,
										 std::string_view active_state)
	{
		auto *fam = FindFamily(family_name);
		if (fam == nullptr)
		{
			return;
		}

		for (auto state : states)
		{
			std::string line;
			line.reserve(family_name.size() + 64);
			line += family_name;
			AppendLabelsWithExtra(line, labels, family_name, state);
			line += (state == active_state) ? " 1" : " 0";
			fam->lines.push_back(std::move(line));
		}
	}

	void OpenMetricsBuilder::AddHistogram(const std::string &family_name,
										  const std::vector<Label> &labels,
										  const std::vector<double> &bounds,
										  const std::vector<uint64_t> &cumulative_buckets,
										  uint64_t total_count,
										  double sum_seconds)
	{
		auto *fam = FindFamily(family_name);
		if (fam == nullptr)
		{
			return;
		}

		for (size_t i = 0; i < bounds.size(); ++i)
		{
			uint64_t bucket_val = (i < cumulative_buckets.size()) ? cumulative_buckets[i] : total_count;
			std::string line;
			line.reserve(family_name.size() + 80);
			line += family_name;
			line += "_bucket";
			AppendLabelsWithExtra(line, labels, "le", FormatLeBound(bounds[i]));
			line += ' ';
			line += std::to_string(bucket_val);
			fam->lines.push_back(std::move(line));
		}

		{
			std::string line;
			line.reserve(family_name.size() + 80);
			line += family_name;
			line += "_bucket";
			AppendLabelsWithExtra(line, labels, "le", "+Inf");
			line += ' ';
			line += std::to_string(total_count);
			fam->lines.push_back(std::move(line));
		}

		AddSampleUint64(family_name, "_count", labels, total_count);
		AddSample(family_name, "_sum", labels, sum_seconds);
	}

	void OpenMetricsBuilder::AddSummary(const std::string &family_name,
										const std::vector<Label> &labels,
										uint64_t count,
										double sum_seconds)
	{
		AddSampleUint64(family_name, "_count", labels, count);
		AddSample(family_name, "_sum", labels, sum_seconds);
	}

	std::string OpenMetricsBuilder::Finish() const
	{
		size_t est = 64;
		for (const auto &fam : _families)
		{
			if (fam.lines.empty())
			{
				continue;
			}
			est += fam.name.size() * 3 + fam.help.size() + 64;
			for (const auto &line : fam.lines)
			{
				est += line.size() + 1;
			}
		}

		std::string out;
		out.reserve(est);

		for (const auto &fam : _families)
		{
			if (fam.lines.empty())
			{
				continue;
			}

			out += "# TYPE ";
			out += fam.name;
			out += ' ';
			out += MetricTypeToString(fam.type);
			out += '\n';

			if (!fam.unit.empty())
			{
				out += "# UNIT ";
				out += fam.name;
				out += ' ';
				out += fam.unit;
				out += '\n';
			}

			out += "# HELP ";
			out += fam.name;
			out += ' ';
			out += EscapeHelpText(fam.help);
			out += '\n';

			for (const auto &line : fam.lines)
			{
				out += line;
				out += '\n';
			}
		}

		out += "# EOF\n";
		return out;
	}
}  // namespace mon::openmetrics
