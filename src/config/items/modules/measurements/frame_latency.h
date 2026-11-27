//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../module_template.h"

namespace cfg
{
	namespace modules
	{
		namespace measurements
		{
			struct FrameLatency : public ModuleTemplate
			{
			public:
				static constexpr size_t kMaxBuckets	   = 30;
				static constexpr double kMinBoundSec   = 0.0001;
				static constexpr double kMaxBoundSec   = 3600.0;
				static constexpr size_t kMaxDecimals   = 6;
				static constexpr const char *kDefaultBucketsStr =
					"0.001,0.005,0.01,0.02,0.03,0.05,0.07,0.1,0.15,0.2,0.3,0.5,1,3";

				static std::vector<double> DefaultBuckets()
				{
					return {0.001, 0.005, 0.01, 0.02, 0.03, 0.05, 0.07, 0.1, 0.15, 0.2, 0.3, 0.5, 1.0, 3.0};
				}

				// Shortest decimal representation, with ".0" for whole seconds (e.g., 0.2 -> "0.2", 1.0 -> "1.0")
				static ov::String FormatLeBound(double seconds)
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
					return ov::String(s.c_str());
				}

				// Shortest decimal representation without forced ".0" (matches canonical bounds label)
				static ov::String FormatCompactBound(double seconds)
				{
					char buf[64];
					std::snprintf(buf, sizeof(buf), "%.6f", seconds);
					std::string s(buf);

					auto dot = s.find('.');
					if (dot != std::string::npos)
					{
						while (!s.empty() && s.back() == '0')
						{
							s.pop_back();
						}
						if (!s.empty() && s.back() == '.')
						{
							s.pop_back();
						}
					}
					return ov::String(s.c_str());
				}

				static bool ParseBuckets(const ov::String &raw, std::vector<double> *out_bounds, ov::String *out_error = nullptr)
				{
					std::vector<double> parsed;
					ov::String trimmed = raw.Trim();

					if (trimmed.IsEmpty())
					{
						if (out_error != nullptr)
						{
							*out_error = "<Buckets> must not be empty";
						}
						return false;
					}

					auto tokens = trimmed.Split(",");
					if (tokens.empty() || tokens.size() > kMaxBuckets)
					{
						if (out_error != nullptr)
						{
							*out_error = ov::String::FormatString("<Buckets> must have between 1 and %zu bounds (got %zu)", kMaxBuckets, tokens.size());
						}
						return false;
					}

					double prev = -1.0;
					for (const auto &raw_token : tokens)
					{
						ov::String token = raw_token.Trim();
						if (token.IsEmpty())
						{
							if (out_error != nullptr)
							{
								*out_error = "<Buckets> contains an empty entry";
							}
							return false;
						}

						// Validate exact decimal syntax: digits, optional '.' followed by 1..6 digits
						const char *str	 = token.CStr();
						size_t len		 = token.GetLength();
						bool seen_dot	 = false;
						size_t int_digits = 0;
						size_t frac_digits = 0;
						for (size_t i = 0; i < len; ++i)
						{
							char c = str[i];
							if (c >= '0' && c <= '9')
							{
								if (seen_dot)
								{
									frac_digits++;
								}
								else
								{
									int_digits++;
								}
							}
							else if (c == '.' && !seen_dot)
							{
								seen_dot = true;
							}
							else
							{
								if (out_error != nullptr)
								{
									*out_error = ov::String::FormatString("Invalid bucket bound '%s' in <Buckets>", token.CStr());
								}
								return false;
							}
						}

						if (int_digits == 0 || (seen_dot && frac_digits == 0))
						{
							if (out_error != nullptr)
							{
								*out_error = ov::String::FormatString("Invalid bucket bound '%s' in <Buckets>", token.CStr());
							}
							return false;
						}

						if (frac_digits > kMaxDecimals)
						{
							if (out_error != nullptr)
							{
								*out_error = ov::String::FormatString("Bucket bound '%s' has more than %zu decimal places", token.CStr(), kMaxDecimals);
							}
							return false;
						}

						char *endptr = nullptr;
						double val	 = std::strtod(str, &endptr);
						if (endptr == nullptr || *endptr != '\0' || !std::isfinite(val))
						{
							if (out_error != nullptr)
							{
								*out_error = ov::String::FormatString("Could not parse bucket bound '%s'", token.CStr());
							}
							return false;
						}

						// Use microsecond integer rounding to avoid floating-point edge issues
						int64_t val_us = static_cast<int64_t>(std::llround(val * 1000000.0));
						int64_t min_us = static_cast<int64_t>(std::llround(kMinBoundSec * 1000000.0));
						int64_t max_us = static_cast<int64_t>(std::llround(kMaxBoundSec * 1000000.0));

						if (val_us < min_us || val_us > max_us)
						{
							if (out_error != nullptr)
							{
								*out_error = ov::String::FormatString("Bucket bound '%s' is outside allowed range [0.0001, 3600]", token.CStr());
							}
							return false;
						}

						if (prev >= 0.0 && val_us <= static_cast<int64_t>(std::llround(prev * 1000000.0)))
						{
							if (out_error != nullptr)
							{
								*out_error = ov::String::FormatString("Bucket bounds must be strictly increasing (failed at '%s')", token.CStr());
							}
							return false;
						}

						prev = static_cast<double>(val_us) / 1000000.0;
						parsed.push_back(prev);
					}

					if (out_bounds != nullptr)
					{
						*out_bounds = std::move(parsed);
					}
					return true;
				}

				static ov::String JoinCanonicalBounds(const std::vector<double> &bounds)
				{
					ov::String result;
					for (size_t i = 0; i < bounds.size(); ++i)
					{
						if (i > 0)
						{
							result.Append(",");
						}
						result.Append(FormatCompactBound(bounds[i]));
					}
					return result;
				}

			protected:
				ov::String _buckets;

			public:
				explicit FrameLatency(bool enable = false)
					: ModuleTemplate(enable)
				{
				}

				CFG_DECLARE_CONST_REF_GETTER_OF(GetBuckets, _buckets)

			protected:
				void MakeList() override
				{
					ModuleTemplate::MakeList();
					Register<Optional>("Buckets", &_buckets);
				}
			};
		}  // namespace measurements
	}  // namespace modules
}  // namespace cfg
