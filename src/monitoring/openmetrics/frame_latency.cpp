//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "frame_latency.h"

#include <base/info/application.h>
#include <base/mediarouter/media_buffer.h>
#include <base/ovlibrary/log.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mon::openmetrics
{
	namespace
	{
		constexpr const char *kFrameLatencyLogTag = "FrameLatency";

		uint64_t MakeTrackKey(info::stream_id_t stream_id, uint32_t track_id)
		{
			return (static_cast<uint64_t>(stream_id) << 32) | static_cast<uint64_t>(track_id);
		}
	}  // namespace

	const char *LatencySpanToString(LatencySpan span)
	{
		switch (span)
		{
			case LatencySpan::Ingest:
				return "ingest";
			case LatencySpan::Relay:
				return "relay";
			case LatencySpan::Egress:
				return "egress";
			case LatencySpan::EndToEnd:
				return "end_to_end";
			default:
				return "unknown";
		}
	}

	SpanHistogram::SpanHistogram(const std::vector<double> &bounds_seconds)
		: _buckets(bounds_seconds.size() + 1),
		  _prev_buckets(bounds_seconds.size() + 1, 0)
	{
		_bounds_us.reserve(bounds_seconds.size());
		for (double b : bounds_seconds)
		{
			_bounds_us.push_back(static_cast<int64_t>(std::llround(b * 1000000.0)));
		}
		for (auto &slot : _buckets)
		{
			slot.store(0, std::memory_order_relaxed);
		}
	}

	void SpanHistogram::Observe(int64_t latency_us, bool has_pts, int64_t pts, uint32_t session_id)
	{
		if (latency_us < 0)
		{
			RecordExcludedNegative();
			return;
		}

		size_t bucket_idx = _bounds_us.size();
		for (size_t i = 0; i < _bounds_us.size(); ++i)
		{
			if (latency_us <= _bounds_us[i])
			{
				bucket_idx = i;
				break;
			}
		}

		_buckets[bucket_idx].fetch_add(1, std::memory_order_relaxed);
		_count.fetch_add(1, std::memory_order_relaxed);
		_sum_us.fetch_add(static_cast<uint64_t>(latency_us), std::memory_order_relaxed);

		if (ov_log_get_enabled(kFrameLatencyLogTag, OVLogLevelDebug))
		{
			std::lock_guard<std::mutex> lock(_window_mutex);
			_window_n++;
			if (!_window_has_max || latency_us > _window_max_us)
			{
				_window_has_max	  = true;
				_window_max_us	  = latency_us;
				_window_max_has_pts = has_pts;
				_window_max_pts	  = pts;
				_window_max_sess  = session_id;
			}
		}
	}

	void SpanHistogram::RecordExcludedNegative()
	{
		_excluded_neg.fetch_add(1, std::memory_order_relaxed);
	}

	void SpanHistogram::RecordExcludedUnmeasured()
	{
		_excluded_unm.fetch_add(1, std::memory_order_relaxed);
	}

	SpanHistogram::Snapshot SpanHistogram::GetSnapshot() const
	{
		Snapshot snap;
		snap.total_count  = _count.load(std::memory_order_relaxed);
		snap.sum_seconds  = static_cast<double>(_sum_us.load(std::memory_order_relaxed)) / 1000000.0;
		snap.excluded_neg = _excluded_neg.load(std::memory_order_relaxed);
		snap.excluded_unm = _excluded_unm.load(std::memory_order_relaxed);
		snap.has_any_observation = (snap.total_count > 0 || snap.excluded_neg > 0 || snap.excluded_unm > 0);

		snap.cumulative_buckets.resize(_bounds_us.size(), 0);
		uint64_t running = 0;
		for (size_t i = 0; i < _bounds_us.size(); ++i)
		{
			running += _buckets[i].load(std::memory_order_relaxed);
			snap.cumulative_buckets[i] = running;
		}
		return snap;
	}

	std::string SpanHistogram::FormatDebugDelta(LatencySpan span, const std::vector<double> &bounds_seconds)
	{
		uint64_t cur_count = _count.load(std::memory_order_relaxed);
		uint64_t cur_sum   = _sum_us.load(std::memory_order_relaxed);
		uint64_t cur_neg   = _excluded_neg.load(std::memory_order_relaxed);
		uint64_t cur_unm   = _excluded_unm.load(std::memory_order_relaxed);

		if (cur_count == 0 && cur_neg == 0 && cur_unm == 0)
		{
			return {};
		}

		uint64_t win_n		= 0;
		bool win_has_max	= false;
		int64_t win_max_us	= 0;
		bool win_has_pts	= false;
		int64_t win_pts		= 0;
		uint32_t win_sess	= 0;
		{
			std::lock_guard<std::mutex> lock(_window_mutex);
			win_n			= _window_n;
			win_has_max		= _window_has_max;
			win_max_us		= _window_max_us;
			win_has_pts		= _window_max_has_pts;
			win_pts			= _window_max_pts;
			win_sess		= _window_max_sess;
			_window_n		= 0;
			_window_has_max = false;
			_window_max_us	= 0;
		}

		uint64_t d_count = (cur_count >= _prev_count) ? (cur_count - _prev_count) : 0;
		uint64_t d_sum	 = (cur_sum >= _prev_sum_us) ? (cur_sum - _prev_sum_us) : 0;
		uint64_t d_neg	 = (cur_neg >= _prev_neg) ? (cur_neg - _prev_neg) : 0;
		uint64_t d_unm	 = (cur_unm >= _prev_unm) ? (cur_unm - _prev_unm) : 0;

		std::string buckets_str;
		for (size_t i = 0; i < _buckets.size(); ++i)
		{
			uint64_t cur_b = _buckets[i].load(std::memory_order_relaxed);
			uint64_t db	   = (cur_b >= _prev_buckets[i]) ? (cur_b - _prev_buckets[i]) : 0;
			_prev_buckets[i] = cur_b;
			if (db > 0)
			{
				if (!buckets_str.empty())
				{
					buckets_str += ',';
				}
				if (i < bounds_seconds.size())
				{
					buckets_str += cfg::modules::measurements::FrameLatency::FormatCompactBound(bounds_seconds[i]);
				}
				else
				{
					buckets_str += "+Inf";
				}
				buckets_str += ':';
				buckets_str += std::to_string(db);
			}
		}

		_prev_count	 = cur_count;
		_prev_sum_us = cur_sum;
		_prev_neg	 = cur_neg;
		_prev_unm	 = cur_unm;

		if (win_n == 0 && d_count == 0 && d_neg == 0 && d_unm == 0)
		{
			return {};
		}

		std::string out;
		out += LatencySpanToString(span);
		out += " n=";
		out += std::to_string(win_n);

		bool has_session = (span == LatencySpan::Egress || span == LatencySpan::EndToEnd);
		if (win_has_max)
		{
			out += " max=";
			out += std::to_string(win_max_us);
			out += "us(";
			if (has_session)
			{
				out += "session ";
				out += std::to_string(win_sess);
				out += ", ";
			}
			if (win_has_pts)
			{
				out += "pts ";
				out += std::to_string(win_pts);
			}
			else
			{
				out += "pts n/a";
			}
			out += ')';
		}
		else
		{
			out += " max=n/a";
		}

		out += " count=";
		out += std::to_string(cur_count);
		out += " d=";
		out += std::to_string(d_count);
		out += " mean=";
		uint64_t mean_us = (d_count > 0) ? (d_sum / d_count) : 0;
		out += std::to_string(mean_us);
		out += "us";

		if (d_neg > 0)
		{
			out += " neg=";
			out += std::to_string(d_neg);
		}
		if (d_unm > 0)
		{
			out += " unm=";
			out += std::to_string(d_unm);
		}
		if (!buckets_str.empty())
		{
			out += " b=";
			out += buckets_str;
		}
		return out;
	}

	TrackLatencyEntry::TrackLatencyEntry(const std::vector<double> &bounds,
										 std::string stream_label_in,
										 uint32_t track_id_in,
										 cmn::MediaType media_type_in)
		: stream_label(std::move(stream_label_in)),
		  track_id(track_id_in),
		  media_type(media_type_in)
	{
		for (size_t i = 0; i < static_cast<size_t>(LatencySpan::Count); ++i)
		{
			spans[i] = std::make_unique<SpanHistogram>(bounds);
		}
	}

	void SessionEgressLatency::Observe(uint32_t track_id, int64_t egress_us)
	{
		if (egress_us < 0)
		{
			return;
		}

		// Lock-free fast path scan
		for (size_t i = 0; i < kMaxTracks; ++i)
		{
			uint32_t existing = _slots[i].track_id.load(std::memory_order_acquire);
			if (existing == track_id)
			{
				_slots[i].count.fetch_add(1, std::memory_order_relaxed);
				_slots[i].sum_us.fetch_add(static_cast<uint64_t>(egress_us), std::memory_order_relaxed);
				return;
			}
			if (existing == UINT32_MAX)
			{
				break;
			}
		}

		std::lock_guard<std::mutex> lock(_alloc_mutex);
		for (size_t i = 0; i < _slot_count; ++i)
		{
			if (_slots[i].track_id.load(std::memory_order_relaxed) == track_id)
			{
				_slots[i].count.fetch_add(1, std::memory_order_relaxed);
				_slots[i].sum_us.fetch_add(static_cast<uint64_t>(egress_us), std::memory_order_relaxed);
				return;
			}
		}

		if (_slot_count >= kMaxTracks)
		{
			return;
		}

		size_t idx = _slot_count++;
		_slots[idx].count.store(1, std::memory_order_relaxed);
		_slots[idx].sum_us.store(static_cast<uint64_t>(egress_us), std::memory_order_relaxed);
		_slots[idx].track_id.store(track_id, std::memory_order_release);
	}

	std::vector<SessionEgressLatency::TrackSummary> SessionEgressLatency::GetSnapshot() const
	{
		std::vector<TrackSummary> out;
		for (size_t i = 0; i < kMaxTracks; ++i)
		{
			uint32_t tid = _slots[i].track_id.load(std::memory_order_acquire);
			if (tid == UINT32_MAX)
			{
				break;
			}
			uint64_t c = _slots[i].count.load(std::memory_order_relaxed);
			if (c > 0)
			{
				uint64_t s = _slots[i].sum_us.load(std::memory_order_relaxed);
				out.push_back(TrackSummary{tid, c, static_cast<double>(s) / 1000000.0});
			}
		}
		return out;
	}

	FrameLatencyManager::FrameLatencyManager()
	{
		_bounds				  = cfg::modules::measurements::FrameLatency::DefaultBuckets();
		_canonical_bounds_str = cfg::modules::measurements::FrameLatency::JoinCanonicalBounds(_bounds);
	}

	FrameLatencyManager::~FrameLatencyManager()
	{
		Stop();
	}

	void FrameLatencyManager::Start(const cfg::modules::measurements::FrameLatency &config)
	{
		Stop();

		std::vector<double> parsed;
		if (!config.GetBuckets().IsEmpty() &&
			cfg::modules::measurements::FrameLatency::ParseBuckets(config.GetBuckets(), &parsed))
		{
			_bounds = std::move(parsed);
		}
		else
		{
			_bounds = cfg::modules::measurements::FrameLatency::DefaultBuckets();
		}
		_canonical_bounds_str = cfg::modules::measurements::FrameLatency::JoinCanonicalBounds(_bounds);

		{
			std::unique_lock<std::shared_mutex> lock(_tracks_mutex);
			_tracks.clear();
		}

		bool enable = config.IsEnabled();
		_enabled.store(enable, std::memory_order_release);

		if (enable)
		{
			_timer_running.store(true, std::memory_order_release);
			_debug_timer.Start();
			_debug_timer.Push([this](void *) -> ov::DelayQueueAction {
				OnDebugTimer();
				return _timer_running.load(std::memory_order_acquire) ? ov::DelayQueueAction::Repeat : ov::DelayQueueAction::Stop;
			},
							  1000);
		}
	}

	void FrameLatencyManager::Stop()
	{
		_enabled.store(false, std::memory_order_release);
		if (_timer_running.exchange(false, std::memory_order_acq_rel))
		{
			_debug_timer.Stop();
		}
		std::unique_lock<std::shared_mutex> lock(_tracks_mutex);
		_tracks.clear();
	}

	std::shared_ptr<TrackLatencyEntry> FrameLatencyManager::GetOrCreateTrack(const info::Stream &output_stream,
																			 uint32_t track_id,
																			 cmn::MediaType media_type)
	{
		uint64_t key = MakeTrackKey(output_stream.GetId(), track_id);
		{
			std::shared_lock<std::shared_mutex> lock(_tracks_mutex);
			auto it = _tracks.find(key);
			if (it != _tracks.end())
			{
				return it->second;
			}
		}

		std::unique_lock<std::shared_mutex> lock(_tracks_mutex);
		auto it = _tracks.find(key);
		if (it != _tracks.end())
		{
			return it->second;
		}

		std::string label = ov::String::FormatString("%s/%s",
													 output_stream.GetApplicationInfo().GetVHostAppName().CStr(),
													 output_stream.GetName().CStr())
								.CStr();

		auto entry = std::make_shared<TrackLatencyEntry>(_bounds, std::move(label), track_id, media_type);
		_tracks.emplace(key, entry);
		return entry;
	}

	void FrameLatencyManager::OnOutputTrackFrame(const info::Stream &output_stream,
												 const std::shared_ptr<const MediaPacket> &packet)
	{
		if (!_enabled.load(std::memory_order_acquire) || packet == nullptr)
		{
			return;
		}

		auto mt = packet->GetMediaType();
		if (mt != cmn::MediaType::Video && mt != cmn::MediaType::Audio)
		{
			return;
		}

		const auto &stamps = packet->GetFrameLatencyStamps();
		if (stamps.from_ovt_origin && stamps.origin_arrival_us > 0 && stamps.origin_send_us > 0 && stamps.arrival_us > 0)
		{
			auto entry = GetOrCreateTrack(output_stream, packet->GetTrackId(), mt);
			entry->origin_stamps_seen.store(true, std::memory_order_release);
			entry->spans[static_cast<size_t>(LatencySpan::Ingest)]->Observe(
				stamps.origin_send_us - stamps.origin_arrival_us, stamps.has_pts, stamps.pts);
			entry->spans[static_cast<size_t>(LatencySpan::Relay)]->Observe(
				stamps.arrival_us - stamps.origin_send_us, stamps.has_pts, stamps.pts);
			return;
		}

		// If this track previously received origin timestamps, a frame missing them is counted as unmeasured
		uint64_t key = MakeTrackKey(output_stream.GetId(), packet->GetTrackId());
		std::shared_ptr<TrackLatencyEntry> entry;
		{
			std::shared_lock<std::shared_mutex> lock(_tracks_mutex);
			auto it = _tracks.find(key);
			if (it != _tracks.end())
			{
				entry = it->second;
			}
		}
		if (entry != nullptr && entry->origin_stamps_seen.load(std::memory_order_acquire))
		{
			entry->spans[static_cast<size_t>(LatencySpan::Ingest)]->RecordExcludedUnmeasured();
			entry->spans[static_cast<size_t>(LatencySpan::Relay)]->RecordExcludedUnmeasured();
		}
	}

	void FrameLatencyManager::OnWebRtcFrameSent(const info::Stream &output_stream,
												uint32_t track_id,
												cmn::MediaType media_type,
												uint32_t session_id,
												const FrameLatencyStamps &stamps,
												int64_t send_us,
												SessionEgressLatency *session_latency)
	{
		if (!_enabled.load(std::memory_order_acquire) || stamps.arrival_us <= 0)
		{
			return;
		}

		auto entry = GetOrCreateTrack(output_stream, track_id, media_type);

		int64_t egress_us = send_us - stamps.arrival_us;
		entry->spans[static_cast<size_t>(LatencySpan::Egress)]->Observe(egress_us, stamps.has_pts, stamps.pts, session_id);
		if (session_latency != nullptr)
		{
			session_latency->Observe(track_id, egress_us);
		}

		if (stamps.from_ovt_origin && stamps.origin_arrival_us > 0)
		{
			entry->origin_stamps_seen.store(true, std::memory_order_release);
			int64_t e2e_us = send_us - stamps.origin_arrival_us;
			entry->spans[static_cast<size_t>(LatencySpan::EndToEnd)]->Observe(e2e_us, stamps.has_pts, stamps.pts, session_id);
		}
		else if (entry->origin_stamps_seen.load(std::memory_order_acquire))
		{
			entry->spans[static_cast<size_t>(LatencySpan::EndToEnd)]->RecordExcludedUnmeasured();
		}
	}

	void FrameLatencyManager::OnStreamDeleted(info::stream_id_t output_stream_id)
	{
		std::unique_lock<std::shared_mutex> lock(_tracks_mutex);
		for (auto it = _tracks.begin(); it != _tracks.end();)
		{
			if ((it->first >> 32) == static_cast<uint64_t>(output_stream_id))
			{
				it = _tracks.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	std::vector<FrameLatencyManager::TrackSnapshot> FrameLatencyManager::GetStreamSnapshots(info::stream_id_t output_stream_id) const
	{
		std::vector<TrackSnapshot> out;
		std::shared_lock<std::shared_mutex> lock(_tracks_mutex);
		for (const auto &[key, entry] : _tracks)
		{
			if ((key >> 32) != static_cast<uint64_t>(output_stream_id))
			{
				continue;
			}

			TrackSnapshot ts;
			ts.track_id = entry->track_id;
			for (size_t i = 0; i < static_cast<size_t>(LatencySpan::Count); ++i)
			{
				ts.spans[i] = entry->spans[i]->GetSnapshot();
			}
			out.push_back(std::move(ts));
		}
		std::sort(out.begin(), out.end(), [](const TrackSnapshot &a, const TrackSnapshot &b) {
			return a.track_id < b.track_id;
		});
		return out;
	}

	std::vector<std::string> FrameLatencyManager::CollectDebugLogLines(int64_t unix_sec, std::chrono::steady_clock::time_point now_steady)
	{
		std::vector<std::shared_ptr<TrackLatencyEntry>> entries;
		{
			std::shared_lock<std::shared_mutex> lock(_tracks_mutex);
			entries.reserve(_tracks.size());
			for (const auto &[key, entry] : _tracks)
			{
				entries.push_back(entry);
			}
		}

		std::vector<std::string> lines;
		for (const auto &entry : entries)
		{
			std::string spans_part;
			for (size_t i = 0; i < static_cast<size_t>(LatencySpan::Count); ++i)
			{
				std::string seg = entry->spans[i]->FormatDebugDelta(static_cast<LatencySpan>(i), _bounds);
				if (!seg.empty())
				{
					if (!spans_part.empty())
					{
						spans_part += ' ';
					}
					spans_part += seg;
				}
			}

			if (spans_part.empty())
			{
				continue;
			}

			const char *mt_str = (entry->media_type == cmn::MediaType::Video) ? "Video" :
								 (entry->media_type == cmn::MediaType::Audio) ? "Audio" : "Unknown";

			std::string line;
			line += entry->stream_label;
			line += " track ";
			line += std::to_string(entry->track_id);
			line += " (";
			line += mt_str;
			line += ") sec=";
			line += std::to_string(unix_sec);
			if (entry->debug_line_written)
			{
				auto w_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_steady - entry->last_debug_log_steady).count();
				line += " w=";
				line += std::to_string(w_ms);
				line += "ms";
			}
			line += ": ";
			line += spans_part;

			entry->debug_line_written	 = true;
			entry->last_debug_log_steady = now_steady;
			lines.push_back(std::move(line));
		}

		return lines;
	}

	void FrameLatencyManager::OnDebugTimer()
	{
		if (!_enabled.load(std::memory_order_acquire) || !ov_log_get_enabled(kFrameLatencyLogTag, OVLogLevelDebug))
		{
			return;
		}

		int64_t unix_sec = std::chrono::duration_cast<std::chrono::seconds>(
							   std::chrono::system_clock::now().time_since_epoch())
							   .count();
		auto now_steady = std::chrono::steady_clock::now();

		auto lines = CollectDebugLogLines(unix_sec, now_steady);
		for (const auto &line : lines)
		{
			logd(kFrameLatencyLogTag, "%s", line.c_str());
		}
	}
}  // namespace mon::openmetrics
