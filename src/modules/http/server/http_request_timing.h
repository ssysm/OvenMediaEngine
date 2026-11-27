//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/common_types.h>
#include <base/ovlibrary/delay_queue.h>
#include <base/ovlibrary/ovlibrary.h>
#include <modules/http/http_datastructure.h>

#include <array>
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

namespace cfg
{
	class Server;
}

namespace http::svr
{
	enum class HttpTimingHandler : uint8_t
	{
		Unknown	  = 0,
		None	  = 1,
		LLHls	  = 2,
		HlsV3	  = 3,
		Thumbnail = 4,
		Whip	  = 5,
		Api		  = 6,
		Count	  = 7,
	};

	enum class HttpTimingKind : uint8_t
	{
		Other		   = 0,
		Preflight	   = 1,
		MasterPlaylist = 2,
		MediaPlaylist  = 3,
		PartialSegment = 4,
		Segment		   = 5,
		InitSegment	   = 6,
		Image		   = 7,
		Offer		   = 8,
		Ice			   = 9,
		Teardown	   = 10,
		Rest		   = 11,
		Metrics		   = 12,
		Count		   = 13,
	};

	enum class HttpUnansweredReason : uint8_t
	{
		Cancelled		 = 0,
		ConnectionClosed = 1,
		SendFailed		 = 2,
		Dropped			 = 3,
		Count			 = 4,
	};

	const char *HttpTimingHandlerToString(HttpTimingHandler handler);
	const char *HttpTimingKindToString(HttpTimingKind kind);
	const char *HttpUnansweredReasonToString(HttpUnansweredReason reason);
	std::string HttpStatusCodeLabel(StatusCode status_code);
	std::string SanitizeTimingPath(const ov::String &uri);

	class HttpRequestTimingContext
	{
	public:
		HttpRequestTimingContext();

		void MarkFirstDataIfUnset();
		bool HasFirstData() const { return _has_first_data; }

		void MarkHeadersComplete(HttpTimingHandler handler,
								 HttpTimingKind default_kind,
								 const ov::String &method,
								 const ov::String &uri,
								 const ov::String &http_version,
								 const ov::String &client_addr);

		void SetHandler(HttpTimingHandler handler);
		void SetKind(HttpTimingKind kind);
		void SetHeld(bool held);
		void SetDueTime(std::chrono::steady_clock::time_point due_time);
		void SetStream(uint32_t stream_id, const std::string &stream_label);
		void SetSession(uint32_t session_id);

		// Called right before HttpResponse::SendHeader() starts sending the response
		std::chrono::steady_clock::time_point CaptureResponseStart();
		void FinishAnswered(StatusCode status_code, std::chrono::steady_clock::time_point response_start);
		void FinishUnanswered(HttpUnansweredReason reason);

		bool IsInProgress() const
		{
			return _state.load(std::memory_order_acquire) == State::InProgress;
		}

		const std::string &GetRequestId() const { return _request_id; }

	private:
		enum class State : uint8_t
		{
			Init	   = 0,
			InProgress = 1,
			Finished   = 2,
		};

		std::atomic<State> _state{State::Init};
		bool _has_first_data = false;
		std::chrono::steady_clock::time_point _arrival_steady{};
		std::chrono::system_clock::time_point _arrival_system{};

		mutable std::mutex _meta_mutex;
		std::string _request_id;
		HttpTimingHandler _handler = HttpTimingHandler::Unknown;
		HttpTimingKind _kind	   = HttpTimingKind::Other;
		bool _held				   = false;
		bool _has_due_time		   = false;
		std::chrono::steady_clock::time_point _due_steady{};
		uint32_t _stream_id	 = 0;
		std::string _stream_label;
		uint32_t _session_id = 0;
		std::string _method;
		std::string _path;
		std::string _http_version;
		std::string _client_addr;
	};

	struct HttpTimingHistogramKey
	{
		HttpTimingHandler handler = HttpTimingHandler::Unknown;
		HttpTimingKind kind		  = HttpTimingKind::Other;
		bool held				  = false;
		std::string status_code;

		bool operator<(const HttpTimingHistogramKey &o) const
		{
			if (handler != o.handler) return handler < o.handler;
			if (kind != o.kind) return kind < o.kind;
			if (held != o.held) return held < o.held;
			return status_code < o.status_code;
		}
	};

	struct HttpTimingSummaryKey
	{
		HttpTimingKind kind = HttpTimingKind::Other;
		bool held			= false;
		std::string status_code;

		bool operator<(const HttpTimingSummaryKey &o) const
		{
			if (kind != o.kind) return kind < o.kind;
			if (held != o.held) return held < o.held;
			return status_code < o.status_code;
		}
	};

	struct HttpTimingGroupKey
	{
		HttpTimingHandler handler = HttpTimingHandler::Unknown;
		HttpTimingKind kind		  = HttpTimingKind::Other;
		bool held				  = false;

		bool operator<(const HttpTimingGroupKey &o) const
		{
			if (handler != o.handler) return handler < o.handler;
			if (kind != o.kind) return kind < o.kind;
			return held < o.held;
		}
	};

	struct HttpTimingUnansweredKey
	{
		HttpTimingHandler handler	= HttpTimingHandler::Unknown;
		HttpTimingKind kind			= HttpTimingKind::Other;
		bool held					= false;
		HttpUnansweredReason reason = HttpUnansweredReason::Dropped;

		bool operator<(const HttpTimingUnansweredKey &o) const
		{
			if (handler != o.handler) return handler < o.handler;
			if (kind != o.kind) return kind < o.kind;
			if (held != o.held) return held < o.held;
			return reason < o.reason;
		}
	};

	struct HttpHistogramSnapshot
	{
		std::vector<uint64_t> cumulative_buckets;
		uint64_t total_count = 0;
		double sum_seconds	 = 0.0;
	};

	struct HttpSummarySnapshot
	{
		uint64_t count	   = 0;
		double sum_seconds = 0.0;
	};

	class HttpRequestTimingManager
	{
	public:
		static constexpr size_t kNumBuckets = 18;
		static const std::vector<double> &FixedBounds();

		static HttpRequestTimingManager *GetInstance()
		{
			static HttpRequestTimingManager instance;
			return &instance;
		}

		void Start(const std::shared_ptr<const cfg::Server> &server_config);
		void StartForTest(bool enabled);
		void Stop();

		bool IsEnabled() const
		{
			return _enabled.load(std::memory_order_acquire);
		}

		void PrepopulateHandler(HttpTimingHandler handler);

		void IncrementInProgress(HttpTimingHandler handler);
		void DecrementInProgress(HttpTimingHandler handler);
		void TransferInProgress(HttpTimingHandler from_handler, HttpTimingHandler to_handler);

		struct AnsweredEvent
		{
			HttpTimingHandler handler = HttpTimingHandler::Unknown;
			HttpTimingKind kind		  = HttpTimingKind::Other;
			bool held				  = false;
			std::string status_code;
			int64_t duration_us		= 0;
			int64_t wait_us			= 0;
			int64_t processing_us	= 0;
			uint32_t stream_id		= 0;
			uint32_t session_id		= 0;
			std::string request_id;
			std::string stream_label;
			std::string client_addr;
			std::string http_version;
			std::string method;
			std::string path;
			std::chrono::system_clock::time_point arrival_system{};
		};

		struct UnansweredEvent
		{
			HttpTimingHandler handler	= HttpTimingHandler::Unknown;
			HttpTimingKind kind			= HttpTimingKind::Other;
			bool held					= false;
			HttpUnansweredReason reason = HttpUnansweredReason::Dropped;
			int64_t waited_us			= 0;
			uint32_t stream_id			= 0;
			uint32_t session_id			= 0;
			std::string request_id;
			std::string stream_label;
			std::string client_addr;
			std::string http_version;
			std::string method;
			std::string path;
			std::chrono::system_clock::time_point arrival_system{};
		};

		void RecordAnswered(const AnsweredEvent &ev);
		void RecordUnanswered(const UnansweredEvent &ev);

		void OnStreamDeleted(uint32_t stream_id);
		void OnSessionDeleted(uint32_t session_id);

		// Scrape snapshots
		struct ServerSnapshot
		{
			std::map<HttpTimingHistogramKey, HttpHistogramSnapshot> duration;
			std::map<HttpTimingHistogramKey, HttpHistogramSnapshot> processing;
			std::map<HttpTimingUnansweredKey, uint64_t> unanswered;
			std::map<HttpTimingHandler, int64_t> in_progress;
		};

		ServerSnapshot GetServerSnapshot() const;

		struct EntitySummarySnapshot
		{
			std::map<HttpTimingSummaryKey, HttpSummarySnapshot> duration;
			std::map<HttpTimingSummaryKey, HttpSummarySnapshot> processing;
		};

		EntitySummarySnapshot GetStreamSnapshot(uint32_t stream_id) const;
		EntitySummarySnapshot GetSessionSnapshot(uint32_t session_id) const;

		// Exposed for unit tests
		struct LogOutput
		{
			std::vector<std::string> warn_lines;
			std::vector<std::string> debug_lines;
		};
		LogOutput CollectLogLines(int64_t unix_sec, std::chrono::steady_clock::time_point now_steady, bool flush_warn_window);

	private:
		HttpRequestTimingManager();
		~HttpRequestTimingManager();

		struct HistogramData
		{
			std::array<uint64_t, kNumBuckets + 1> buckets{};
			uint64_t count	= 0;
			uint64_t sum_us = 0;

			void Observe(int64_t value_us);
			HttpHistogramSnapshot Snapshot() const;
		};

		struct SummaryData
		{
			uint64_t count	= 0;
			uint64_t sum_us = 0;
		};

		struct SlowSample
		{
			bool valid			= false;
			bool is_dropped		= false;
			int64_t metric_us	= 0;  // processing_us for llhls, duration_us for others, waited_us for dropped
			int64_t total_us	= 0;
			int64_t wait_us		= 0;
			int64_t processing_us = 0;
			std::string id;
			std::string status;
			std::string stream;
			uint32_t session_id = 0;
			std::string client;
			std::string http_version;
			std::string method;
			std::string path;
			std::chrono::system_clock::time_point arrival_system{};
		};

		struct GroupLogState
		{
			// 1-second debug window
			uint64_t debug_n = 0;
			SlowSample debug_max;
			// Cumulative counters (summed across status codes) for debug delta
			std::array<uint64_t, kNumBuckets + 1> total_buckets{};
			uint64_t total_count  = 0;
			uint64_t total_sum_us = 0;

			std::array<uint64_t, kNumBuckets + 1> prev_buckets{};
			uint64_t prev_count	 = 0;
			uint64_t prev_sum_us = 0;
			bool debug_line_written = false;
			std::chrono::steady_clock::time_point last_debug_steady{};

			// 10-second warn window
			uint64_t warn_slow	  = 0;
			uint64_t warn_dropped = 0;
			SlowSample warn_max_slow;
			SlowSample warn_dropped_sample;
		};

		void OnTimerTick();

		std::atomic<bool> _enabled{false};
		std::array<std::atomic<int64_t>, static_cast<size_t>(HttpTimingHandler::Count)> _in_progress{};

		mutable std::shared_mutex _data_mutex;
		std::map<HttpTimingHistogramKey, HistogramData> _duration_histograms;
		std::map<HttpTimingHistogramKey, HistogramData> _processing_histograms;
		std::map<HttpTimingUnansweredKey, uint64_t> _unanswered_counts;

		struct EntitySummaries
		{
			std::map<HttpTimingSummaryKey, SummaryData> duration;
			std::map<HttpTimingSummaryKey, SummaryData> processing;
		};
		std::unordered_map<uint32_t, EntitySummaries> _stream_summaries;
		std::unordered_map<uint32_t, EntitySummaries> _session_summaries;

		mutable std::mutex _log_mutex;
		std::map<HttpTimingGroupKey, GroupLogState> _group_logs;
		std::chrono::system_clock::time_point _warn_window_start_system{};
		int _tick_counter = 0;

		ov::DelayQueue _timer{"HttpTimingLog"};
		std::atomic<bool> _timer_running{false};
	};
}  // namespace http::svr
