//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "openmetrics_collectors.h"

#include <base/ovlibrary/url.h>

namespace mon::openmetrics
{
	uint32_t ParseCollectorName(const ov::String &name)
	{
		if (name == "core") return kCollectorCore;
		if (name == "traffic") return kCollectorTraffic;
		if (name == "connection") return kCollectorConnection;
		if (name == "stream") return kCollectorStream;
		if (name == "queue") return kCollectorQueue;
		if (name == "push") return kCollectorPush;
		if (name == "session") return kCollectorSession;
		if (name == "frame_latency") return kCollectorFrameLatency;
		if (name == "session_latency") return kCollectorSessionLatency;
		if (name == "http") return kCollectorHttp;
		return kCollectorNone;
	}

	const char *ValidCollectorNamesList()
	{
		return "core, traffic, connection, stream, queue, push, session, frame_latency, session_latency, http";
	}

	bool ParseCollectorSelection(const ov::String &raw_query,
								 bool is_sessions_path,
								 uint32_t *out_mask,
								 ov::String *out_error)
	{
		uint32_t collect_mask = 0;
		uint32_t exclude_mask = 0;
		bool has_collect	  = false;
		bool has_exclude	  = false;

		if (raw_query.IsEmpty() == false)
		{
			auto pairs = raw_query.Split("&");
			for (const auto &pair : pairs)
			{
				if (pair.IsEmpty())
				{
					continue;
				}

				ov::String raw_key;
				ov::String raw_val;
				auto eq_pos = pair.IndexOf('=');
				if (eq_pos >= 0)
				{
					raw_key = pair.Substring(0, eq_pos);
					raw_val = pair.Substring(eq_pos + 1);
				}
				else
				{
					raw_key = pair;
				}

				ov::String key = ov::Url::Decode(raw_key);
				ov::String val = ov::Url::Decode(raw_val);

				if (key == "collect[]")
				{
					has_collect = true;
					uint32_t bit = ParseCollectorName(val);
					if (bit == kCollectorNone)
					{
						if (out_error != nullptr)
						{
							*out_error = ov::String::FormatString(
								"Unknown collector '%s'. Valid collectors: %s",
								val.CStr(), ValidCollectorNamesList());
						}
						return false;
					}
					collect_mask |= bit;
				}
				else if (key == "exclude[]")
				{
					has_exclude = true;
					uint32_t bit = ParseCollectorName(val);
					if (bit == kCollectorNone)
					{
						if (out_error != nullptr)
						{
							*out_error = ov::String::FormatString(
								"Unknown collector '%s'. Valid collectors: %s",
								val.CStr(), ValidCollectorNamesList());
						}
						return false;
					}
					exclude_mask |= bit;
				}
			}
		}

		if (has_collect && has_exclude)
		{
			if (out_error != nullptr)
			{
				*out_error = "Query parameters 'collect[]' and 'exclude[]' are mutually exclusive";
			}
			return false;
		}

		uint32_t result = 0;
		if (has_collect)
		{
			result = collect_mask | kCollectorCore;
			if (is_sessions_path)
			{
				// Per spec: The .../streams/{stream}/sessions path includes session_latency
				// unless the request names it in exclude[], so a request with collect[] on that path gets it too.
				result |= kCollectorSessionLatency;
			}
		}
		else if (has_exclude)
		{
			uint32_t base = kDefaultCollectors;
			if (is_sessions_path)
			{
				base |= kCollectorSession | kCollectorSessionLatency;
			}
			result = (base & ~exclude_mask) | kCollectorCore;
		}
		else
		{
			result = kDefaultCollectors;
			if (is_sessions_path)
			{
				result |= kCollectorSession | kCollectorSessionLatency;
			}
		}

		if (out_mask != nullptr)
		{
			*out_mask = result;
		}
		return true;
	}
}  // namespace mon::openmetrics
