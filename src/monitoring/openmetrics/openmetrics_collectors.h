//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mon::openmetrics
{
	enum CollectorMask : uint32_t
	{
		kCollectorNone			 = 0,
		kCollectorCore			 = 1u << 0,
		kCollectorTraffic		 = 1u << 1,
		kCollectorConnection	 = 1u << 2,
		kCollectorStream		 = 1u << 3,
		kCollectorQueue			 = 1u << 4,
		kCollectorPush			 = 1u << 5,
		kCollectorSession		 = 1u << 6,
		kCollectorFrameLatency	 = 1u << 7,
		kCollectorSessionLatency = 1u << 8,
		kCollectorHttp			 = 1u << 9,

		kDefaultCollectors =
			kCollectorCore |
			kCollectorTraffic |
			kCollectorConnection |
			kCollectorStream |
			kCollectorQueue |
			kCollectorPush |
			kCollectorFrameLatency |
			kCollectorHttp,
	};

	// Returns 0 if unknown
	uint32_t ParseCollectorName(const ov::String &name);
	const char *ValidCollectorNamesList();

	// Parses `?collect[]=...` or `?exclude[]=...` from a raw query string (without leading '?').
	// `collect[]` and `exclude[]` are mutually exclusive.
	// `core` is always enabled in the output mask.
	// When `is_sessions_path` is true:
	// - No selection -> default collectors + session + session_latency
	// - `exclude[]`  -> (default collectors + session + session_latency) minus excluded, plus core
	// - `collect[]`  -> selected collectors + core + session_latency (per spec: `.../sessions` includes
	//                   session_latency unless explicitly named in exclude[])
	bool ParseCollectorSelection(const ov::String &raw_query,
								 bool is_sessions_path,
								 uint32_t *out_mask,
								 ov::String *out_error);
}  // namespace mon::openmetrics
