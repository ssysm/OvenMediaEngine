//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "http_request_timing.h"

#include <base/ovlibrary/log.h>
#include <config/config.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace http::svr
{
	namespace
	{
		constexpr const char *kHttpTimingLogTag = "HTTP.Timing";
		constexpr int64_t kSlowThresholdUs		= 250000;  // 0.25 seconds

		constexpr std::array<int64_t, HttpRequestTimingManager::kNumBuckets> kFixedBoundsUs = {
			100,
			250,
			500,
			1000,
			2500,
			5000,
			10000,
			25000,
			50000,
			100000,
			250000,
			500000,
			1000000,
			2500000,
			5000000,
			10000000,
			30000000,
			60000000,
		};

		constexpr const char *kFixedBoundLabels[HttpRequestTimingManager::kNumBuckets] = {
			"0.0001", "0.00025", "0.0005", "0.001", "0.0025", "0.005",
			"0.01", "0.025", "0.05", "0.1", "0.25", "0.5",
			"1", "2.5", "5", "10", "30", "60",
		};

		std::string GenerateShortRequestId()
		{
			static constexpr char kChars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
			static std::atomic<uint64_t> counter{1};
			uint64_t v = counter.fetch_add(1, std::memory_order_relaxed) * 6364136223846793005ULL + 1442695040888963407ULL;
			std::string id(7, '0');
			for (int i = 6; i >= 0; --i)
			{
				id[i] = kChars[v % 62];
				v /= 62;
			}
			return id;
		}

		std::string FormatTimeHMS(std::chrono::system_clock::time_point tp, bool with_ms)
		{
			auto ms_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
			time_t secs			= static_cast<time_t>(ms_since_epoch / 1000);
			int ms				= static_cast<int>(ms_since_epoch % 1000);
			if (ms < 0) ms += 1000;

			struct tm tm_buf{};
			::localtime_r(&secs, &tm_buf);

			char buf[32];
			if (with_ms)
			{
				std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
			}
			else
			{
				std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
			}
			return std::string(buf);
		}

		std::string FormatMs1(int64_t us)
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(us) / 1000.0);
			return std::string(buf);
		}
	}  // namespace

	const char *HttpTimingHandlerToString(HttpTimingHandler handler)
	{
		switch (handler)
		{
			case HttpTimingHandler::None:
				return "none";
			case HttpTimingHandler::LLHls:
				return "llhls";
			case HttpTimingHandler::HlsV3:
				return "hlsv3";
			case HttpTimingHandler::Thumbnail:
				return "thumbnail";
			case HttpTimingHandler::Whip:
				return "whip";
			case HttpTimingHandler::Api:
				return "api";
			case HttpTimingHandler::Unknown:
			default:
				return "unknown";
		}
	}

	const char *HttpTimingKindToString(HttpTimingKind kind)
	{
		switch (kind)
		{
			case HttpTimingKind::Preflight:
				return "preflight";
			case HttpTimingKind::MasterPlaylist:
				return "master_playlist";
			case HttpTimingKind::MediaPlaylist:
				return "media_playlist";
			case HttpTimingKind::PartialSegment:
				return "partial_segment";
			case HttpTimingKind::Segment:
				return "segment";
			case HttpTimingKind::InitSegment:
				return "init_segment";
			case HttpTimingKind::Image:
				return "image";
			case HttpTimingKind::Offer:
				return "offer";
			case HttpTimingKind::Ice:
				return "ice";
			case HttpTimingKind::Teardown:
				return "teardown";
			case HttpTimingKind::Rest:
				return "rest";
			case HttpTimingKind::Metrics:
				return "metrics";
			case HttpTimingKind::Other:
			default:
				return "other";
		}
	}

	const char *HttpUnansweredReasonToString(HttpUnansweredReason reason)
	{
		switch (reason)
		{
			case HttpUnansweredReason::Cancelled:
				return "cancelled";
			case HttpUnansweredReason::ConnectionClosed:
				return "connection_closed";
			case HttpUnansweredReason::SendFailed:
				return "send_failed";
			case HttpUnansweredReason::Dropped:
				return "dropped";
			default:
				return "dropped";
		}
	}

	std::string HttpStatusCodeLabel(StatusCode status_code)
	{
		if (IsValidStatusCode(status_code))
		{
			return std::to_string(static_cast<int>(ov::ToUnderlyingType(status_code)));
		}
		return "other";
	}

	std::string SanitizeTimingPath(const ov::String &uri)
	{
		std::string s(uri.CStr(), uri.GetLength());

		// Strip scheme://authority prefix if a full URL was passed
		auto scheme_pos = s.find("://");
		if (scheme_pos != std::string::npos)
		{
			auto slash_pos = s.find('/', scheme_pos + 3);
			if (slash_pos != std::string::npos)
			{
				s = s.substr(slash_pos);
			}
			else
			{
				auto q_only = s.find('?', scheme_pos + 3);
				s = (q_only != std::string::npos) ? ("/" + s.substr(q_only)) : "/";
			}
		}

		auto qpos = s.find('?');
		if (qpos == std::string::npos)
		{
			return s.empty() ? "-" : s;
		}

		std::string path  = s.substr(0, qpos);
		std::string query = s.substr(qpos + 1);
		std::string kept;

		size_t start = 0;
		while (start <= query.size())
		{
			auto amp   = query.find('&', start);
			auto token = (amp == std::string::npos) ? query.substr(start) : query.substr(start, amp - start);
			auto eq	   = token.find('=');
			auto key   = (eq == std::string::npos) ? token : token.substr(0, eq);
			if (key == "_HLS_msn" || key == "_HLS_part")
			{
				if (!kept.empty())
				{
					kept += '&';
				}
				kept += token;
			}
			if (amp == std::string::npos)
			{
				break;
			}
			start = amp + 1;
		}

		if (kept.empty())
		{
			return path.empty() ? "-" : path;
		}
		return path + "?" + kept;
	}

	//--------------------------------------------------------------------------
	// HttpRequestTimingContext
	//--------------------------------------------------------------------------
	HttpRequestTimingContext::HttpRequestTimingContext()
	{
	}

	void HttpRequestTimingContext::MarkFirstDataIfUnset()
	{
		if (_has_first_data)
		{
			return;
		}
		if (!HttpRequestTimingManager::GetInstance()->IsEnabled())
		{
			return;
		}
		_arrival_steady = std::chrono::steady_clock::now();
		_arrival_system = std::chrono::system_clock::now();
		_has_first_data = true;
	}

	void HttpRequestTimingContext::MarkHeadersComplete(HttpTimingHandler handler,
													   HttpTimingKind default_kind,
													   const ov::String &method,
													   const ov::String &uri,
													   const ov::String &http_version,
													   const ov::String &client_addr)
	{
		if (!HttpRequestTimingManager::GetInstance()->IsEnabled())
		{
			return;
		}

		if (!_has_first_data)
		{
			_arrival_steady = std::chrono::steady_clock::now();
			_arrival_system = std::chrono::system_clock::now();
			_has_first_data = true;
		}

		State expected = State::Init;
		if (!_state.compare_exchange_strong(expected, State::InProgress, std::memory_order_acq_rel))
		{
			return;
		}

		{
			std::lock_guard<std::mutex> lock(_meta_mutex);
			_request_id	  = GenerateShortRequestId();
			_handler	  = handler;
			_kind		  = default_kind;
			_method		  = method.CStr();
			_path		  = SanitizeTimingPath(uri);
			_http_version = http_version.CStr();
			_client_addr  = client_addr.CStr();
		}

		HttpRequestTimingManager::GetInstance()->IncrementInProgress(handler);
	}

	void HttpRequestTimingContext::SetHandler(HttpTimingHandler handler)
	{
		if (!IsInProgress())
		{
			return;
		}

		HttpTimingHandler prev = HttpTimingHandler::Unknown;
		{
			std::lock_guard<std::mutex> lock(_meta_mutex);
			if (_handler == handler)
			{
				return;
			}
			prev	 = _handler;
			_handler = handler;
		}
		HttpRequestTimingManager::GetInstance()->TransferInProgress(prev, handler);
	}

	void HttpRequestTimingContext::SetKind(HttpTimingKind kind)
	{
		std::lock_guard<std::mutex> lock(_meta_mutex);
		_kind = kind;
	}

	void HttpRequestTimingContext::SetHeld(bool held)
	{
		std::lock_guard<std::mutex> lock(_meta_mutex);
		_held = held;
	}

	void HttpRequestTimingContext::SetDueTime(std::chrono::steady_clock::time_point due_time)
	{
		std::lock_guard<std::mutex> lock(_meta_mutex);
		_has_due_time = true;
		_due_steady	  = due_time;
	}

	void HttpRequestTimingContext::SetStream(uint32_t stream_id, const std::string &stream_label)
	{
		std::lock_guard<std::mutex> lock(_meta_mutex);
		_stream_id	  = stream_id;
		_stream_label = stream_label;
	}

	void HttpRequestTimingContext::SetSession(uint32_t session_id)
	{
		std::lock_guard<std::mutex> lock(_meta_mutex);
		_session_id = session_id;
	}

	std::chrono::steady_clock::time_point HttpRequestTimingContext::CaptureResponseStart()
	{
		return std::chrono::steady_clock::now();
	}

	void HttpRequestTimingContext::FinishAnswered(StatusCode status_code, std::chrono::steady_clock::time_point response_start)
	{
		State expected = State::InProgress;
		if (!_state.compare_exchange_strong(expected, State::Finished, std::memory_order_acq_rel))
		{
			return;
		}

		HttpRequestTimingManager::AnsweredEvent ev;
		{
			std::lock_guard<std::mutex> lock(_meta_mutex);
			ev.handler		  = _handler;
			ev.kind			  = _kind;
			ev.held			  = _held;
			ev.status_code	  = HttpStatusCodeLabel(status_code);
			ev.stream_id	  = _stream_id;
			ev.session_id	  = _session_id;
			ev.request_id	  = _request_id;
			ev.stream_label	  = _stream_label;
			ev.client_addr	  = _client_addr;
			ev.http_version	  = _http_version;
			ev.method		  = _method;
			ev.path			  = _path;
			ev.arrival_system = _arrival_system;

			int64_t dur_us = std::chrono::duration_cast<std::chrono::microseconds>(response_start - _arrival_steady).count();
			if (dur_us < 0) dur_us = 0;
			ev.duration_us = dur_us;

			if (_held && _has_due_time)
			{
				int64_t wait_us = std::chrono::duration_cast<std::chrono::microseconds>(_due_steady - _arrival_steady).count();
				int64_t proc_us = std::chrono::duration_cast<std::chrono::microseconds>(response_start - _due_steady).count();
				ev.wait_us		 = std::max<int64_t>(0, wait_us);
				ev.processing_us = std::max<int64_t>(0, proc_us);
			}
			else
			{
				ev.wait_us		 = 0;
				ev.processing_us = dur_us;
			}
		}

		auto *mgr = HttpRequestTimingManager::GetInstance();
		mgr->DecrementInProgress(ev.handler);
		mgr->RecordAnswered(ev);
	}

	void HttpRequestTimingContext::FinishUnanswered(HttpUnansweredReason reason)
	{
		State expected = State::InProgress;
		if (!_state.compare_exchange_strong(expected, State::Finished, std::memory_order_acq_rel))
		{
			return;
		}

		auto now = std::chrono::steady_clock::now();
		HttpRequestTimingManager::UnansweredEvent ev;
		{
			std::lock_guard<std::mutex> lock(_meta_mutex);
			ev.handler		  = _handler;
			ev.kind			  = _kind;
			ev.held			  = _held;
			ev.reason		  = reason;
			ev.stream_id	  = _stream_id;
			ev.session_id	  = _session_id;
			ev.request_id	  = _request_id;
			ev.stream_label	  = _stream_label;
			ev.client_addr	  = _client_addr;
			ev.http_version	  = _http_version;
			ev.method		  = _method;
			ev.path			  = _path;
			ev.arrival_system = _arrival_system;

			int64_t waited = std::chrono::duration_cast<std::chrono::microseconds>(now - _arrival_steady).count();
			ev.waited_us   = std::max<int64_t>(0, waited);
		}

		auto *mgr = HttpRequestTimingManager::GetInstance();
		mgr->DecrementInProgress(ev.handler);
		mgr->RecordUnanswered(ev);
	}

	//--------------------------------------------------------------------------
	// HttpRequestTimingManager
	//--------------------------------------------------------------------------
	const std::vector<double> &HttpRequestTimingManager::FixedBounds()
	{
		static const std::vector<double> kBounds = {
			0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
			0.01, 0.025, 0.05, 0.1, 0.25, 0.5,
			1.0, 2.5, 5.0, 10.0, 30.0, 60.0};
		return kBounds;
	}

	HttpRequestTimingManager::HttpRequestTimingManager()
	{
		for (auto &v : _in_progress)
		{
			v.store(0, std::memory_order_relaxed);
		}
	}

	HttpRequestTimingManager::~HttpRequestTimingManager()
	{
		Stop();
	}

	void HttpRequestTimingManager::HistogramData::Observe(int64_t value_us)
	{
		if (value_us < 0) value_us = 0;
		size_t idx = kNumBuckets;
		for (size_t i = 0; i < kNumBuckets; ++i)
		{
			if (value_us <= kFixedBoundsUs[i])
			{
				idx = i;
				break;
			}
		}
		buckets[idx]++;
		count++;
		sum_us += static_cast<uint64_t>(value_us);
	}

	HttpHistogramSnapshot HttpRequestTimingManager::HistogramData::Snapshot() const
	{
		HttpHistogramSnapshot snap;
		snap.total_count = count;
		snap.sum_seconds = static_cast<double>(sum_us) / 1000000.0;
		snap.cumulative_buckets.resize(kNumBuckets, 0);
		uint64_t running = 0;
		for (size_t i = 0; i < kNumBuckets; ++i)
		{
			running += buckets[i];
			snap.cumulative_buckets[i] = running;
		}
		return snap;
	}

	void HttpRequestTimingManager::Start(const std::shared_ptr<const cfg::Server> &server_config)
	{
		Stop();

		bool enabled = (server_config != nullptr) &&
					   server_config->GetModules().GetMeasurements().GetHTTPRequestTiming().IsEnabled();
		StartForTest(enabled);

		if (!enabled || server_config == nullptr)
		{
			return;
		}

		// Pre-populate the usual series for each enabled HTTP handler (per spec)
		const auto &bind = server_config->GetBind();
		const auto &pubs = bind.GetPublishers();
		if (pubs.GetLLHls().IsParsed())
		{
			PrepopulateHandler(HttpTimingHandler::LLHls);
		}
		if (pubs.GetHls().IsParsed())
		{
			PrepopulateHandler(HttpTimingHandler::HlsV3);
		}
		if (pubs.GetThumbnail().IsParsed())
		{
			PrepopulateHandler(HttpTimingHandler::Thumbnail);
		}
		if (bind.GetProviders().GetWebrtc().IsParsed())
		{
			PrepopulateHandler(HttpTimingHandler::Whip);
		}
		if (bind.GetManagers().GetApi().IsParsed())
		{
			PrepopulateHandler(HttpTimingHandler::Api);
		}
	}

	void HttpRequestTimingManager::StartForTest(bool enabled)
	{
		Stop();

		{
			std::unique_lock<std::shared_mutex> lock(_data_mutex);
			_duration_histograms.clear();
			_processing_histograms.clear();
			_unanswered_counts.clear();
			_stream_summaries.clear();
			_session_summaries.clear();
		}
		{
			std::lock_guard<std::mutex> lock(_log_mutex);
			_group_logs.clear();
			_warn_window_start_system = std::chrono::system_clock::now();
			_tick_counter			  = 0;
		}
		for (auto &v : _in_progress)
		{
			v.store(0, std::memory_order_relaxed);
		}

		_enabled.store(enabled, std::memory_order_release);

		if (enabled)
		{
			_timer_running.store(true, std::memory_order_release);
			_timer.Start();
			_timer.Push([this](void *) -> ov::DelayQueueAction {
				OnTimerTick();
				return _timer_running.load(std::memory_order_acquire) ? ov::DelayQueueAction::Repeat : ov::DelayQueueAction::Stop;
			},
						1000);
		}
	}

	void HttpRequestTimingManager::Stop()
	{
		_enabled.store(false, std::memory_order_release);
		if (_timer_running.exchange(false, std::memory_order_acq_rel))
		{
			_timer.Stop();
		}
	}

	void HttpRequestTimingManager::PrepopulateHandler(HttpTimingHandler handler)
	{
		std::unique_lock<std::shared_mutex> lock(_data_mutex);
		auto add_dur = [&](HttpTimingKind kind, bool held, const char *code) {
			_duration_histograms[HttpTimingHistogramKey{handler, kind, held, code}];
		};
		auto add_llhls = [&](HttpTimingKind kind, bool held, const char *code) {
			_duration_histograms[HttpTimingHistogramKey{handler, kind, held, code}];
			_processing_histograms[HttpTimingHistogramKey{handler, kind, held, code}];
		};

		switch (handler)
		{
			case HttpTimingHandler::LLHls:
				add_llhls(HttpTimingKind::MasterPlaylist, false, "200");
				add_llhls(HttpTimingKind::MediaPlaylist, false, "200");
				add_llhls(HttpTimingKind::MediaPlaylist, true, "200");
				add_llhls(HttpTimingKind::PartialSegment, false, "200");
				add_llhls(HttpTimingKind::PartialSegment, true, "200");
				add_llhls(HttpTimingKind::Segment, false, "200");
				add_llhls(HttpTimingKind::InitSegment, false, "200");
				break;
			case HttpTimingHandler::HlsV3:
				add_dur(HttpTimingKind::MasterPlaylist, false, "200");
				add_dur(HttpTimingKind::MediaPlaylist, false, "200");
				add_dur(HttpTimingKind::Segment, false, "200");
				break;
			case HttpTimingHandler::Thumbnail:
				add_dur(HttpTimingKind::Image, false, "200");
				break;
			case HttpTimingHandler::Whip:
				add_dur(HttpTimingKind::Offer, false, "201");
				add_dur(HttpTimingKind::Ice, false, "204");
				add_dur(HttpTimingKind::Teardown, false, "200");
				break;
			case HttpTimingHandler::Api:
				add_dur(HttpTimingKind::Rest, false, "200");
				add_dur(HttpTimingKind::Metrics, false, "200");
				break;
			default:
				break;
		}
	}

	void HttpRequestTimingManager::IncrementInProgress(HttpTimingHandler handler)
	{
		size_t idx = static_cast<size_t>(handler);
		if (idx < _in_progress.size())
		{
			_in_progress[idx].fetch_add(1, std::memory_order_relaxed);
		}
	}

	void HttpRequestTimingManager::DecrementInProgress(HttpTimingHandler handler)
	{
		size_t idx = static_cast<size_t>(handler);
		if (idx < _in_progress.size())
		{
			_in_progress[idx].fetch_sub(1, std::memory_order_relaxed);
		}
	}

	void HttpRequestTimingManager::TransferInProgress(HttpTimingHandler from_handler, HttpTimingHandler to_handler)
	{
		if (from_handler == to_handler)
		{
			return;
		}
		DecrementInProgress(from_handler);
		IncrementInProgress(to_handler);
	}

	void HttpRequestTimingManager::RecordAnswered(const AnsweredEvent &ev)
	{
		if (!_enabled.load(std::memory_order_acquire))
		{
			return;
		}

		const bool is_llhls = (ev.handler == HttpTimingHandler::LLHls);
		const bool is_hls	= (ev.handler == HttpTimingHandler::HlsV3);

		{
			std::unique_lock<std::shared_mutex> lock(_data_mutex);
			HttpTimingHistogramKey hkey{ev.handler, ev.kind, ev.held, ev.status_code};
			_duration_histograms[hkey].Observe(ev.duration_us);
			if (is_llhls)
			{
				_processing_histograms[hkey].Observe(ev.processing_us);
			}

			if (ev.stream_id != 0)
			{
				HttpTimingSummaryKey skey{ev.kind, ev.held, ev.status_code};
				auto &ent = _stream_summaries[ev.stream_id];
				ent.duration[skey].count++;
				ent.duration[skey].sum_us += static_cast<uint64_t>(ev.duration_us);
				if (is_llhls)
				{
					ent.processing[skey].count++;
					ent.processing[skey].sum_us += static_cast<uint64_t>(ev.processing_us);
				}
			}

			if (ev.session_id != 0 && (is_llhls || is_hls))
			{
				HttpTimingSummaryKey skey{ev.kind, ev.held, ev.status_code};
				auto &ent = _session_summaries[ev.session_id];
				ent.duration[skey].count++;
				ent.duration[skey].sum_us += static_cast<uint64_t>(ev.duration_us);
				if (is_llhls)
				{
					ent.processing[skey].count++;
					ent.processing[skey].sum_us += static_cast<uint64_t>(ev.processing_us);
				}
			}
		}

		// Update log window stats (processing time for LL-HLS, duration for others)
		const int64_t metric_us = is_llhls ? ev.processing_us : ev.duration_us;
		size_t bidx				= kNumBuckets;
		for (size_t i = 0; i < kNumBuckets; ++i)
		{
			if (metric_us <= kFixedBoundsUs[i])
			{
				bidx = i;
				break;
			}
		}

		{
			std::lock_guard<std::mutex> lock(_log_mutex);
			auto &g = _group_logs[HttpTimingGroupKey{ev.handler, ev.kind, ev.held}];
			g.total_buckets[bidx]++;
			g.total_count++;
			g.total_sum_us += static_cast<uint64_t>(metric_us);

			g.debug_n++;
			if (!g.debug_max.valid || metric_us > g.debug_max.metric_us)
			{
				g.debug_max.valid		  = true;
				g.debug_max.is_dropped	  = false;
				g.debug_max.metric_us	  = metric_us;
				g.debug_max.total_us	  = ev.duration_us;
				g.debug_max.wait_us		  = ev.wait_us;
				g.debug_max.processing_us = ev.processing_us;
				g.debug_max.id			  = ev.request_id;
				g.debug_max.status		  = ev.status_code;
				g.debug_max.stream		  = ev.stream_label;
				g.debug_max.session_id	  = ev.session_id;
				g.debug_max.client		  = ev.client_addr;
				g.debug_max.http_version  = ev.http_version;
				g.debug_max.method		  = ev.method;
				g.debug_max.path		  = ev.path;
				g.debug_max.arrival_system = ev.arrival_system;
			}

			if (metric_us > kSlowThresholdUs)
			{
				g.warn_slow++;
				if (!g.warn_max_slow.valid || metric_us > g.warn_max_slow.metric_us)
				{
					g.warn_max_slow.valid		   = true;
					g.warn_max_slow.is_dropped	   = false;
					g.warn_max_slow.metric_us	   = metric_us;
					g.warn_max_slow.total_us	   = ev.duration_us;
					g.warn_max_slow.wait_us		   = ev.wait_us;
					g.warn_max_slow.processing_us  = ev.processing_us;
					g.warn_max_slow.id			   = ev.request_id;
					g.warn_max_slow.status		   = ev.status_code;
					g.warn_max_slow.stream		   = ev.stream_label;
					g.warn_max_slow.session_id	   = ev.session_id;
					g.warn_max_slow.client		   = ev.client_addr;
					g.warn_max_slow.http_version   = ev.http_version;
					g.warn_max_slow.method		   = ev.method;
					g.warn_max_slow.path		   = ev.path;
					g.warn_max_slow.arrival_system = ev.arrival_system;
				}
			}
		}
	}

	void HttpRequestTimingManager::RecordUnanswered(const UnansweredEvent &ev)
	{
		if (!_enabled.load(std::memory_order_acquire))
		{
			return;
		}

		{
			std::unique_lock<std::shared_mutex> lock(_data_mutex);
			_unanswered_counts[HttpTimingUnansweredKey{ev.handler, ev.kind, ev.held, ev.reason}]++;
		}

		if (ev.reason == HttpUnansweredReason::Dropped)
		{
			std::lock_guard<std::mutex> lock(_log_mutex);
			auto &g = _group_logs[HttpTimingGroupKey{ev.handler, ev.kind, ev.held}];
			g.warn_dropped++;
			if (!g.warn_dropped_sample.valid || ev.waited_us > g.warn_dropped_sample.metric_us)
			{
				g.warn_dropped_sample.valid			 = true;
				g.warn_dropped_sample.is_dropped	 = true;
				g.warn_dropped_sample.metric_us		 = ev.waited_us;
				g.warn_dropped_sample.total_us		 = ev.waited_us;
				g.warn_dropped_sample.id			 = ev.request_id;
				g.warn_dropped_sample.stream		 = ev.stream_label;
				g.warn_dropped_sample.session_id	 = ev.session_id;
				g.warn_dropped_sample.client		 = ev.client_addr;
				g.warn_dropped_sample.http_version	 = ev.http_version;
				g.warn_dropped_sample.method		 = ev.method;
				g.warn_dropped_sample.path			 = ev.path;
				g.warn_dropped_sample.arrival_system = ev.arrival_system;
			}
		}
	}

	void HttpRequestTimingManager::OnStreamDeleted(uint32_t stream_id)
	{
		std::unique_lock<std::shared_mutex> lock(_data_mutex);
		_stream_summaries.erase(stream_id);
	}

	void HttpRequestTimingManager::OnSessionDeleted(uint32_t session_id)
	{
		std::unique_lock<std::shared_mutex> lock(_data_mutex);
		_session_summaries.erase(session_id);
	}

	HttpRequestTimingManager::ServerSnapshot HttpRequestTimingManager::GetServerSnapshot() const
	{
		ServerSnapshot snap;
		{
			std::shared_lock<std::shared_mutex> lock(_data_mutex);
			for (const auto &[k, v] : _duration_histograms)
			{
				snap.duration.emplace(k, v.Snapshot());
			}
			for (const auto &[k, v] : _processing_histograms)
			{
				snap.processing.emplace(k, v.Snapshot());
			}
			snap.unanswered = _unanswered_counts;
		}
		for (size_t i = 0; i < _in_progress.size(); ++i)
		{
			int64_t val = _in_progress[i].load(std::memory_order_relaxed);
			if (val > 0 || static_cast<HttpTimingHandler>(i) == HttpTimingHandler::Api)
			{
				snap.in_progress[static_cast<HttpTimingHandler>(i)] = std::max<int64_t>(0, val);
			}
		}
		return snap;
	}

	HttpRequestTimingManager::EntitySummarySnapshot HttpRequestTimingManager::GetStreamSnapshot(uint32_t stream_id) const
	{
		EntitySummarySnapshot out;
		std::shared_lock<std::shared_mutex> lock(_data_mutex);
		auto it = _stream_summaries.find(stream_id);
		if (it == _stream_summaries.end())
		{
			return out;
		}
		for (const auto &[k, v] : it->second.duration)
		{
			out.duration.emplace(k, HttpSummarySnapshot{v.count, static_cast<double>(v.sum_us) / 1000000.0});
		}
		for (const auto &[k, v] : it->second.processing)
		{
			out.processing.emplace(k, HttpSummarySnapshot{v.count, static_cast<double>(v.sum_us) / 1000000.0});
		}
		return out;
	}

	HttpRequestTimingManager::EntitySummarySnapshot HttpRequestTimingManager::GetSessionSnapshot(uint32_t session_id) const
	{
		EntitySummarySnapshot out;
		std::shared_lock<std::shared_mutex> lock(_data_mutex);
		auto it = _session_summaries.find(session_id);
		if (it == _session_summaries.end())
		{
			return out;
		}
		for (const auto &[k, v] : it->second.duration)
		{
			out.duration.emplace(k, HttpSummarySnapshot{v.count, static_cast<double>(v.sum_us) / 1000000.0});
		}
		for (const auto &[k, v] : it->second.processing)
		{
			out.processing.emplace(k, HttpSummarySnapshot{v.count, static_cast<double>(v.sum_us) / 1000000.0});
		}
		return out;
	}

	HttpRequestTimingManager::LogOutput HttpRequestTimingManager::CollectLogLines(int64_t unix_sec,
																				  std::chrono::steady_clock::time_point now_steady,
																				  bool flush_warn_window)
	{
		LogOutput out;
		std::lock_guard<std::mutex> lock(_log_mutex);

		auto window_start_str = FormatTimeHMS(_warn_window_start_system, false);
		if (flush_warn_window)
		{
			_warn_window_start_system = std::chrono::system_clock::now();
		}

		for (auto &[key, g] : _group_logs)
		{
			// 1-second debug line (when any answered requests arrived in this window)
			if (g.debug_n > 0)
			{
				uint64_t d_count = g.total_count - g.prev_count;
				uint64_t d_sum	 = g.total_sum_us - g.prev_sum_us;

				std::string b_str;
				for (size_t i = 0; i < kNumBuckets + 1; ++i)
				{
					uint64_t db = g.total_buckets[i] - g.prev_buckets[i];
					g.prev_buckets[i] = g.total_buckets[i];
					if (db > 0)
					{
						if (!b_str.empty()) b_str += ',';
						b_str += (i < kNumBuckets) ? kFixedBoundLabels[i] : "+Inf";
						b_str += ':';
						b_str += std::to_string(db);
					}
				}
				g.prev_count  = g.total_count;
				g.prev_sum_us = g.total_sum_us;

				std::string line;
				line += HttpTimingHandlerToString(key.handler);
				line += '/';
				line += HttpTimingKindToString(key.kind);
				line += " held=";
				line += key.held ? "true" : "false";
				line += " sec=";
				line += std::to_string(unix_sec);
				if (g.debug_line_written)
				{
					auto w_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_steady - g.last_debug_steady).count();
					line += " w=";
					line += std::to_string(w_ms);
					line += "ms";
				}
				line += ": n=";
				line += std::to_string(g.debug_n);
				line += " max=";
				line += FormatMs1(g.debug_max.metric_us);
				line += "ms (id=";
				line += g.debug_max.id.empty() ? "-" : g.debug_max.id;
				line += " status=";
				line += g.debug_max.status.empty() ? "-" : g.debug_max.status;
				line += " stream=";
				line += g.debug_max.stream.empty() ? "-" : g.debug_max.stream;
				line += " session=";
				line += (g.debug_max.session_id != 0) ? std::to_string(g.debug_max.session_id) : "-";
				line += ") count=";
				line += std::to_string(g.total_count);
				line += " d=";
				line += std::to_string(d_count);
				line += " mean=";
				line += FormatMs1(d_count > 0 ? static_cast<int64_t>(d_sum / d_count) : 0);
				line += "ms";
				if (!b_str.empty())
				{
					line += " b=";
					line += b_str;
				}

				g.debug_line_written = true;
				g.last_debug_steady	 = now_steady;
				g.debug_n			 = 0;
				g.debug_max			 = SlowSample{};
				out.debug_lines.push_back(std::move(line));
			}

			// 10-second warn line
			if (flush_warn_window && (g.warn_slow > 0 || g.warn_dropped > 0))
			{
				const SlowSample &sample = g.warn_max_slow.valid ? g.warn_max_slow : g.warn_dropped_sample;
				std::string line;
				line += "Slow HTTP requests in ";
				line += window_start_str;
				line += " +10s: handler=";
				line += HttpTimingHandlerToString(key.handler);
				line += " kind=";
				line += HttpTimingKindToString(key.kind);
				line += " held=";
				line += key.held ? "true" : "false";
				line += " slow=";
				line += std::to_string(g.warn_slow);
				line += " dropped=";
				line += std::to_string(g.warn_dropped);
				line += " max: id=";
				line += sample.id.empty() ? "-" : sample.id;

				if (!sample.is_dropped)
				{
					line += " status=";
					line += sample.status.empty() ? "-" : sample.status;
					line += " total=";
					line += FormatMs1(sample.total_us);
					line += "ms wait=";
					line += FormatMs1(sample.wait_us);
					line += "ms processing=";
					line += FormatMs1(sample.processing_us);
					line += "ms";
				}
				else
				{
					line += " waited=";
					line += FormatMs1(sample.total_us);
					line += "ms";
				}

				line += " arrived=";
				line += (sample.arrival_system.time_since_epoch().count() > 0) ? FormatTimeHMS(sample.arrival_system, true) : "-";
				line += " stream=";
				line += sample.stream.empty() ? "-" : sample.stream;
				line += " session=";
				line += (sample.session_id != 0) ? std::to_string(sample.session_id) : "-";
				line += " client=";
				line += sample.client.empty() ? "-" : sample.client;
				line += " http=";
				line += sample.http_version.empty() ? "-" : sample.http_version;
				line += ' ';
				line += sample.method.empty() ? "-" : sample.method;
				line += ' ';
				line += sample.path.empty() ? "-" : sample.path;

				g.warn_slow			  = 0;
				g.warn_dropped		  = 0;
				g.warn_max_slow		  = SlowSample{};
				g.warn_dropped_sample = SlowSample{};
				out.warn_lines.push_back(std::move(line));
			}
		}

		return out;
	}

	void HttpRequestTimingManager::OnTimerTick()
	{
		if (!_enabled.load(std::memory_order_acquire))
		{
			return;
		}

		bool debug_on = ov_log_get_enabled(kHttpTimingLogTag, OVLogLevelDebug);
		bool warn_on  = ov_log_get_enabled(kHttpTimingLogTag, OVLogLevelWarning);

		_tick_counter++;
		bool flush_warn = (_tick_counter >= 10);
		if (flush_warn)
		{
			_tick_counter = 0;
		}

		if (!debug_on && !flush_warn)
		{
			return;
		}

		int64_t unix_sec = std::chrono::duration_cast<std::chrono::seconds>(
							   std::chrono::system_clock::now().time_since_epoch())
							   .count();
		auto now_steady = std::chrono::steady_clock::now();

		auto logs = CollectLogLines(unix_sec, now_steady, flush_warn);
		if (warn_on)
		{
			for (const auto &line : logs.warn_lines)
			{
				logw(kHttpTimingLogTag, "%s", line.c_str());
			}
		}
		if (debug_on)
		{
			for (const auto &line : logs.debug_lines)
			{
				logd(kHttpTimingLogTag, "%s", line.c_str());
			}
		}
	}
}  // namespace http::svr
