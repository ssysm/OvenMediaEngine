//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/common_types.h>
#include <base/info/stream.h>
#include <base/ovlibrary/ovlibrary.h>
#include <config/items/modules/measurements/frame_latency.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mon::openmetrics
{
	inline int64_t NowSystemMicros()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
				   std::chrono::system_clock::now().time_since_epoch())
			.count();
	}

	enum class LatencySpan : uint8_t
	{
		Ingest	 = 0,
		Relay	 = 1,
		Egress	 = 2,
		EndToEnd = 3,
		Count	 = 4,
	};

	const char *LatencySpanToString(LatencySpan span);

	// Per-track histogram + excluded counters + 1-second debug log tracker for one span
	class SpanHistogram
	{
	public:
		explicit SpanHistogram(const std::vector<double> &bounds_seconds);

		void Observe(int64_t latency_us, bool has_pts, int64_t pts, uint32_t session_id = 0);
		void RecordExcludedNegative();
		void RecordExcludedUnmeasured();

		struct Snapshot
		{
			bool has_any_observation = false;
			std::vector<uint64_t> cumulative_buckets;
			uint64_t total_count	 = 0;
			double sum_seconds		 = 0.0;
			uint64_t excluded_neg	 = 0;
			uint64_t excluded_unm	 = 0;
		};

		Snapshot GetSnapshot() const;

		// Produces and advances the per-second debug-log delta segment; returns empty string if never observed
		std::string FormatDebugDelta(LatencySpan span, const std::vector<double> &bounds_seconds);

	private:
		std::vector<int64_t> _bounds_us;
		// Non-cumulative per-bucket counts (size = bounds.size() + 1, last is overflow / +Inf)
		std::vector<std::atomic<uint64_t>> _buckets;
		std::atomic<uint64_t> _count{0};
		std::atomic<uint64_t> _sum_us{0};
		std::atomic<uint64_t> _excluded_neg{0};
		std::atomic<uint64_t> _excluded_unm{0};

		// Current 1-second window max tracker for debug log
		mutable std::mutex _window_mutex;
		uint64_t _window_n		  = 0;
		bool _window_has_max	  = false;
		int64_t _window_max_us	  = 0;
		bool _window_max_has_pts  = false;
		int64_t _window_max_pts	  = 0;
		uint32_t _window_max_sess = 0;

		// Previous debug log snapshot (only accessed from debug log timer thread)
		std::vector<uint64_t> _prev_buckets;
		uint64_t _prev_count = 0;
		uint64_t _prev_sum_us = 0;
		uint64_t _prev_neg	 = 0;
		uint64_t _prev_unm	 = 0;
	};

	struct TrackLatencyEntry
	{
		TrackLatencyEntry(const std::vector<double> &bounds,
						  std::string stream_label,
						  uint32_t track_id,
						  cmn::MediaType media_type);

		std::string stream_label;
		uint32_t track_id = 0;
		cmn::MediaType media_type = cmn::MediaType::Unknown;
		std::atomic<bool> origin_stamps_seen{false};
		std::unique_ptr<SpanHistogram> spans[static_cast<size_t>(LatencySpan::Count)];

		// Debug-log previous write time (steady_clock)
		bool debug_line_written = false;
		std::chrono::steady_clock::time_point last_debug_log_steady{};
	};

	// Per-WebRTC-session egress latency summary (up to 32 tracks, negative spans excluded)
	class SessionEgressLatency
	{
	public:
		static constexpr size_t kMaxTracks = 32;

		struct TrackSummary
		{
			uint32_t track_id = 0;
			uint64_t count	  = 0;
			double sum_seconds = 0.0;
		};

		void Observe(uint32_t track_id, int64_t egress_us);
		std::vector<TrackSummary> GetSnapshot() const;

	private:
		struct Slot
		{
			std::atomic<uint32_t> track_id{UINT32_MAX};
			std::atomic<uint64_t> count{0};
			std::atomic<uint64_t> sum_us{0};
		};

		std::array<Slot, kMaxTracks> _slots;
		mutable std::mutex _alloc_mutex;
		size_t _slot_count = 0;
	};

	// Global coordinator for FrameLatency configuration, per-output-stream track histograms, and debug logger
	class FrameLatencyManager
	{
	public:
		static FrameLatencyManager *GetInstance()
		{
			static FrameLatencyManager instance;
			return &instance;
		}

		void Start(const cfg::modules::measurements::FrameLatency &config);
		void Stop();

		bool IsEnabled() const
		{
			return _enabled.load(std::memory_order_acquire);
		}

		const std::vector<double> &GetBounds() const
		{
			return _bounds;
		}

		const std::string &GetCanonicalBoundsString() const
		{
			return _canonical_bounds_str;
		}

		// Called on edge output stream frames (MediaRouter OutboundWorkerThread)
		void OnOutputTrackFrame(const info::Stream &output_stream, const std::shared_ptr<const MediaPacket> &packet);

		// Called when WebRTC viewer successfully hands the last RTP packet of a frame to the send path
		void OnWebRtcFrameSent(const info::Stream &output_stream,
							   uint32_t track_id,
							   cmn::MediaType media_type,
							   uint32_t session_id,
							   const FrameLatencyStamps &stamps,
							   int64_t send_us,
							   SessionEgressLatency *session_latency);

		void OnStreamDeleted(info::stream_id_t output_stream_id);

		// Snapshot of all tracks recorded for a given output stream ID
		struct TrackSnapshot
		{
			uint32_t track_id = 0;
			SpanHistogram::Snapshot spans[static_cast<size_t>(LatencySpan::Count)];
		};
		std::vector<TrackSnapshot> GetStreamSnapshots(info::stream_id_t output_stream_id) const;

		// Exposed for unit testing debug log line formatting
		std::vector<std::string> CollectDebugLogLines(int64_t unix_sec, std::chrono::steady_clock::time_point now_steady);

	private:
		FrameLatencyManager();
		~FrameLatencyManager();

		std::shared_ptr<TrackLatencyEntry> GetOrCreateTrack(const info::Stream &output_stream,
															uint32_t track_id,
															cmn::MediaType media_type);
		void OnDebugTimer();

		std::atomic<bool> _enabled{false};
		std::vector<double> _bounds;
		std::string _canonical_bounds_str;

		mutable std::shared_mutex _tracks_mutex;
		// Key: (output_stream_id << 32) | track_id
		std::unordered_map<uint64_t, std::shared_ptr<TrackLatencyEntry>> _tracks;

		ov::DelayQueue _debug_timer{"FrameLatencyLog"};
		std::atomic<bool> _timer_running{false};
	};
}  // namespace mon::openmetrics
