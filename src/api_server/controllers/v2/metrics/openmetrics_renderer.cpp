//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "openmetrics_renderer.h"

#include <base/info/ome_version.h>
#include <base/info/push.h>
#include <base/info/record.h>
#include <base/provider/stream.h>
#include <base/publisher/session.h>
#include <base/publisher/stream.h>
#include <modules/http/server/http_request_timing.h>
#include <monitoring/openmetrics/frame_latency.h>
#include <orchestrator/orchestrator.h>
#include <publishers/publishers.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>

namespace api::v2
{
	using mon::openmetrics::Label;
	using mon::openmetrics::MetricType;
	using mon::openmetrics::OpenMetricsBuilder;

	namespace
	{
		std::string ToLowerAscii(const char *str)
		{
			if (str == nullptr)
			{
				return {};
			}
			std::string out(str);
			for (char &c : out)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return out;
		}

		struct ScopedStreamEntry
		{
			std::string vhost_name;
			std::string app_name;
			std::shared_ptr<mon::StreamMetrics> input_stream;
			std::vector<std::shared_ptr<mon::StreamMetrics>> output_streams;
		};

		struct ScopedAppEntry
		{
			std::string vhost_name;
			std::string app_name;
			std::shared_ptr<mon::ApplicationMetrics> app_metrics;
		};

		std::vector<Label> MakeStreamLabels(const std::string &vhost_name,
											const std::string &app_name,
											const std::shared_ptr<mon::StreamMetrics> &input_stream,
											const std::string &output_stream_name)
		{
			return {
				{"vhost", vhost_name},
				{"app", app_name},
				{"stream", input_stream->GetName().CStr()},
				{"stream_id", std::to_string(input_stream->GetId())},
				{"output_stream", output_stream_name},
				{"provider", OpenMetricsRenderer::ProviderLabel(input_stream->GetProviderType())},
			};
		}

		std::vector<Label> MakeTrackLabels(const std::vector<Label> &stream_labels,
										   const std::shared_ptr<const MediaTrack> &track)
		{
			auto labels = stream_labels;
			labels.push_back({"track", std::to_string(track->GetId())});
			labels.push_back({"media", OpenMetricsRenderer::MediaTypeLabel(track->GetMediaType())});
			labels.push_back({"codec", OpenMetricsRenderer::CodecLabel(track->GetCodecId())});
			return labels;
		}

		std::vector<Label> MakeSessionLabels(const std::vector<Label> &stream_labels,
											 PublisherType pub_type,
											 const std::shared_ptr<pub::Session> &session)
		{
			auto labels = stream_labels;
			labels.push_back({"publisher", OpenMetricsRenderer::PublisherLabel(pub_type)});
			std::string sess_name = session->GetName().has_value()
										? std::string(session->GetName()->CStr())
										: std::string(session->GetUUID().CStr());
			labels.push_back({"session", std::move(sess_name)});
			labels.push_back({"session_id", std::to_string(session->GetId())});
			return labels;
		}

		const char *PushProtocolLabel(info::Push::ProtocolType proto, const ov::String &raw_proto)
		{
			switch (proto)
			{
				case info::Push::ProtocolType::RTMP:
					return "rtmp";
				case info::Push::ProtocolType::SRT:
					return "srt";
				case info::Push::ProtocolType::MPEGTS:
					return "mpegts";
				default:
					break;
			}
			auto lower = raw_proto.LowerCaseString();
			if (lower == "rtmp") return "rtmp";
			if (lower == "srt") return "srt";
			if (lower == "mpegts") return "mpegts";
			return "unknown";
		}

		const char *PushStateLabel(info::Push::PushState state)
		{
			switch (state)
			{
				case info::Push::PushState::Ready:
					return "ready";
				case info::Push::PushState::Connecting:
					return "connecting";
				case info::Push::PushState::Pushing:
					return "pushing";
				case info::Push::PushState::Stopping:
					return "stopping";
				case info::Push::PushState::Stopped:
					return "stopped";
				case info::Push::PushState::Error:
					return "error";
			}
			return "error";
		}

		const char *RecordStateLabel(info::Record::RecordState state)
		{
			switch (state)
			{
				case info::Record::RecordState::Ready:
					return "ready";
				case info::Record::RecordState::Recording:
					return "recording";
				case info::Record::RecordState::Stopping:
					return "stopping";
				case info::Record::RecordState::Stopped:
					return "stopped";
				case info::Record::RecordState::Error:
					return "error";
			}
			return "error";
		}

		constexpr PublisherType kAllEgressPublishers[] = {
			PublisherType::Webrtc,
			PublisherType::LLHls,
			PublisherType::Hls,
			PublisherType::Srt,
			PublisherType::Ovt,
			PublisherType::Thumbnail,
			PublisherType::Push,
			PublisherType::File,
		};

		constexpr PublisherType kSessionPublishers[] = {
			PublisherType::Webrtc,
			PublisherType::LLHls,
			PublisherType::Hls,
			PublisherType::Srt,
			PublisherType::Ovt,
		};
	}  // namespace

	const char *OpenMetricsRenderer::ProviderLabel(ProviderType type)
	{
		switch (type)
		{
			case ProviderType::Srt:
				return "srt";
			case ProviderType::Rtmp:
				return "rtmp";
			case ProviderType::Mpegts:
				return "mpegts";
			case ProviderType::Ovt:
				return "ovt";
			case ProviderType::WebRTC:
				return "webrtc";
			case ProviderType::RtspPull:
				return "rtsppull";
			case ProviderType::Rtsp:
				return "rtsp";
			case ProviderType::Scheduled:
				return "scheduled";
			case ProviderType::Multiplex:
				return "multiplex";
			case ProviderType::File:
				return "file";
			case ProviderType::Unknown:
			default:
				return "unknown";
		}
	}

	const char *OpenMetricsRenderer::PublisherLabel(PublisherType type)
	{
		switch (type)
		{
			case PublisherType::Webrtc:
				return "webrtc";
			case PublisherType::LLHls:
				return "llhls";
			case PublisherType::Hls:
				return "hlsv3";
			case PublisherType::Srt:
				return "srt";
			case PublisherType::Ovt:
				return "ovt";
			case PublisherType::Thumbnail:
				return "thumbnail";
			case PublisherType::Push:
				return "push";
			case PublisherType::File:
				return "file";
			case PublisherType::MpegtsPush:
			case PublisherType::RtmpPush:
			case PublisherType::SrtPush:
				return "push";
			default:
				return "unknown";
		}
	}

	const char *OpenMetricsRenderer::MediaTypeLabel(cmn::MediaType type)
	{
		switch (type)
		{
			case cmn::MediaType::Video:
				return "video";
			case cmn::MediaType::Audio:
				return "audio";
			case cmn::MediaType::Data:
				return "data";
			case cmn::MediaType::Subtitle:
				return "subtitle";
			case cmn::MediaType::Attachment:
				return "attachment";
			default:
				return "unknown";
		}
	}

	std::string OpenMetricsRenderer::CodecLabel(cmn::MediaCodecId codec_id)
	{
		return ToLowerAscii(cmn::GetCodecIdString(codec_id));
	}

	std::string OpenMetricsRenderer::Render(const OpenMetricsScope &scope)
	{
		OpenMetricsBuilder b;
		const uint32_t mask = scope.collector_mask | mon::openmetrics::kCollectorCore;

		auto *monitoring	 = mon::Monitoring::GetInstance();
		auto *orchestrator	 = ocst::Orchestrator::GetInstance();
		auto server_metrics	 = monitoring->GetServerMetrics();

		//----------------------------------------------------------------------
		// Gather in-scope apps & streams
		//----------------------------------------------------------------------
		std::vector<ScopedAppEntry> scoped_apps;
		std::vector<ScopedStreamEntry> scoped_streams;

		auto collect_app_streams = [&](const std::string &vhost_name,
									   const std::shared_ptr<mon::ApplicationMetrics> &app_metrics) {
			std::string app_name = app_metrics->GetVHostAppName().GetAppName().CStr();
			scoped_apps.push_back(ScopedAppEntry{vhost_name, app_name, app_metrics});

			auto all_streams = app_metrics->GetStreamMetricsMap();
			std::map<info::stream_id_t, ScopedStreamEntry> inputs;
			for (const auto &[sid, sm] : all_streams)
			{
				if (sm == nullptr) continue;
				if (sm->GetLinkedInputStream() == nullptr)
				{
					inputs.emplace(sid, ScopedStreamEntry{vhost_name, app_name, sm, {}});
				}
			}
			for (const auto &[sid, sm] : all_streams)
			{
				if (sm == nullptr) continue;
				auto linked = sm->GetLinkedInputStream();
				if (linked != nullptr)
				{
					auto it = inputs.find(linked->GetId());
					if (it != inputs.end())
					{
						it->second.output_streams.push_back(sm);
					}
				}
			}
			for (auto &[sid, entry] : inputs)
			{
				scoped_streams.push_back(std::move(entry));
			}
		};

		if (scope.stream != nullptr && scope.app != nullptr && scope.vhost != nullptr)
		{
			std::string vhost_name = scope.vhost->GetName().CStr();
			std::string app_name   = scope.app->GetVHostAppName().GetAppName().CStr();
			scoped_apps.push_back(ScopedAppEntry{vhost_name, app_name, scope.app});
			scoped_streams.push_back(ScopedStreamEntry{vhost_name, app_name, scope.stream, scope.output_streams});
		}
		else if (scope.app != nullptr && scope.vhost != nullptr)
		{
			collect_app_streams(scope.vhost->GetName().CStr(), scope.app);
		}
		else if (scope.vhost != nullptr)
		{
			std::string vhost_name = scope.vhost->GetName().CStr();
			for (const auto &[aid, app_metrics] : scope.vhost->GetApplicationMetricsList())
			{
				if (app_metrics != nullptr)
				{
					collect_app_streams(vhost_name, app_metrics);
				}
			}
		}
		else
		{
			for (const auto &[hid, host_metrics] : monitoring->GetHostMetricsList())
			{
				if (host_metrics == nullptr) continue;
				std::string vhost_name = host_metrics->GetName().CStr();
				for (const auto &[aid, app_metrics] : host_metrics->GetApplicationMetricsList())
				{
					if (app_metrics != nullptr)
					{
						collect_app_streams(vhost_name, app_metrics);
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// 1. core (always on)
		//----------------------------------------------------------------------
		b.DeclareFamily("ome_build", MetricType::Info, "", "OvenMediaEngine build information.");
		{
			auto *ver = info::OmeVersion::GetInstance();
			b.AddInfo("ome_build", {
									   {"version", ver->GetVersion().CStr()},
									   {"git_version", ver->GetGitVersion().CStr()},
								   });
		}

		b.DeclareFamily("process_start_time_seconds", MetricType::Gauge, "seconds",
						"Start time of the process since unix epoch in seconds.");
		if (server_metrics != nullptr)
		{
			auto started = server_metrics->GetServerStartedTime();
			double sec	 = std::chrono::duration<double>(started.time_since_epoch()).count();
			b.AddGauge("process_start_time_seconds", {}, std::round(sec * 100.0) / 100.0);
		}
		else
		{
			static const double fallback_start = std::chrono::duration<double>(
													 std::chrono::system_clock::now().time_since_epoch())
													 .count();
			b.AddGauge("process_start_time_seconds", {}, std::round(fallback_start * 100.0) / 100.0);
		}

		//----------------------------------------------------------------------
		// 2. traffic
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorTraffic) != 0)
		{
			b.DeclareFamily("ome_receive_bytes", MetricType::Counter, "bytes",
							"Media bytes received from the provider for this stream.");
			b.DeclareFamily("ome_transmit_bytes", MetricType::Counter, "bytes",
							"Bytes transmitted to subscribers, by publisher.");

			auto emit_stream_traffic = [&](const std::vector<Label> &labels, const std::shared_ptr<mon::StreamMetrics> &sm) {
				b.AddCounter("ome_receive_bytes", labels, sm->GetTotalBytesIn());
				for (PublisherType pt : kAllEgressPublishers)
				{
					uint64_t bytes = sm->GetBytesOut(pt);
					uint64_t conns = sm->GetConnections(pt);
					if (bytes > 0 || conns > 0)
					{
						auto pub_labels = labels;
						pub_labels.push_back({"publisher", PublisherLabel(pt)});
						b.AddCounter("ome_transmit_bytes", pub_labels, bytes);
					}
				}
			};

			for (const auto &entry : scoped_streams)
			{
				emit_stream_traffic(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, ""), entry.input_stream);
				for (const auto &out_sm : entry.output_streams)
				{
					emit_stream_traffic(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr()), out_sm);
				}
			}
		}

		//----------------------------------------------------------------------
		// 3. connection
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorConnection) != 0)
		{
			b.DeclareFamily("ome_connections", MetricType::Gauge, "",
							"Current number of connected sessions, by publisher.");
			b.DeclareFamily("ome_connections_max", MetricType::Gauge, "",
							"Peak concurrent connections observed for this stream.");

			auto emit_stream_connections = [&](const std::vector<Label> &labels, const std::shared_ptr<mon::StreamMetrics> &sm) {
				for (PublisherType pt : kAllEgressPublishers)
				{
					uint64_t conns = sm->GetConnections(pt);
					uint64_t bytes = sm->GetBytesOut(pt);
					if (conns > 0 || bytes > 0)
					{
						auto pub_labels = labels;
						pub_labels.push_back({"publisher", PublisherLabel(pt)});
						b.AddGaugeUint64("ome_connections", pub_labels, conns);
					}
				}
				b.AddGaugeUint64("ome_connections_max", labels, sm->GetMaxTotalConnections());
			};

			for (const auto &entry : scoped_streams)
			{
				emit_stream_connections(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, ""), entry.input_stream);
				for (const auto &out_sm : entry.output_streams)
				{
					emit_stream_connections(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr()), out_sm);
				}
			}
		}

		//----------------------------------------------------------------------
		// 4. stream
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorStream) != 0)
		{
			b.DeclareFamily("ome_stream_source_connect_seconds", MetricType::Gauge, "seconds",
							"Time taken to connect to the origin.");
			b.DeclareFamily("ome_stream_rtt_seconds", MetricType::Gauge, "seconds",
							"Ingest round-trip time, split by method.");
			b.DeclareFamily("ome_stream_srt_lost_packets", MetricType::Counter, "packets",
							"SRT ingest packets detected as lost by the receiver.");
			b.DeclareFamily("ome_stream_srt_dropped_packets", MetricType::Counter, "packets",
							"SRT ingest packets dropped too-late-to-play (TLPKTDROP).");
			b.DeclareFamily("ome_track_bitrate_bps", MetricType::Gauge, "",
							"Measured track bitrate in bits per second.");
			b.DeclareFamily("ome_track_framerate_fps", MetricType::Gauge, "",
							"Measured video frame rate in frames per second.");
			b.DeclareFamily("ome_track_keyframe_interval_seconds", MetricType::Gauge, "seconds",
							"Measured video keyframe interval from timestamps.");
			b.DeclareFamily("ome_track_width", MetricType::Gauge, "",
							"Video frame width in pixels.");
			b.DeclareFamily("ome_track_height", MetricType::Gauge, "",
							"Video frame height in pixels.");
			b.DeclareFamily("ome_track_has_bframes", MetricType::Gauge, "",
							"1 if the video track carries B-frames, 0 otherwise.");
			b.DeclareFamily("ome_track_samplerate_hertz", MetricType::Gauge, "hertz",
							"Audio sample rate in hertz.");
			b.DeclareFamily("ome_track_channels", MetricType::Gauge, "",
							"Number of audio channels.");

			for (const auto &entry : scoped_streams)
			{
				const auto &in_sm	   = entry.input_stream;
				auto prov_type		   = in_sm->GetProviderType();
				auto pvd_stream		   = (orchestrator != nullptr) ? orchestrator->GetProviderStream(in_sm) : nullptr;
				bool is_pull_reconn	   = (pvd_stream != nullptr) && pvd_stream->IsPullReconnecting();

				// Sample ingest transport RTT / SRT stats once per input stream
				bool has_tcp_rtt = false;
				ov::Socket::TcpRttInfo tcp_rtt{};
				bool has_srt_stats = false;
				ov::Socket::SrtTransportStats srt_stats{};
				std::optional<double> stun_rtt;

				if (pvd_stream != nullptr)
				{
					stun_rtt = pvd_stream->GetIngestStunRttSeconds();
					if (auto sock = pvd_stream->GetIngestSocket(); sock != nullptr)
					{
						if (sock->GetType() == ov::SocketType::Tcp)
						{
							has_tcp_rtt = sock->GetTcpRtt(&tcp_rtt);
						}
						else if (sock->GetType() == ov::SocketType::Srt)
						{
							has_srt_stats = sock->GetSrtStats(&srt_stats);
						}
					}
				}
				else if (prov_type == ProviderType::WebRTC)
				{
					stun_rtt = 0.0;
				}

				auto emit_stream_level = [&](const std::vector<Label> &stream_labels, const std::shared_ptr<mon::StreamMetrics> &sm, bool is_input) {
					if (is_input &&
						(prov_type == ProviderType::Ovt || prov_type == ProviderType::RtspPull) &&
						in_sm->HasOriginConnectionTime())
					{
						b.AddGauge("ome_stream_source_connect_seconds", stream_labels,
								   static_cast<double>(in_sm->GetOriginConnectionTimeMSec()) / 1000.0);
					}

					if (stun_rtt.has_value())
					{
						auto l = stream_labels;
						l.push_back({"method", "stun"});
						b.AddGauge("ome_stream_rtt_seconds", l, *stun_rtt);
					}
					if (has_tcp_rtt)
					{
						auto l_tcp = stream_labels;
						l_tcp.push_back({"method", "tcp"});
						b.AddGauge("ome_stream_rtt_seconds", l_tcp, tcp_rtt.rtt_seconds);

						auto l_min = stream_labels;
						l_min.push_back({"method", "tcp_min"});
						b.AddGauge("ome_stream_rtt_seconds", l_min, tcp_rtt.min_rtt_seconds);
					}
					if (has_srt_stats && prov_type == ProviderType::Srt)
					{
						auto l_srt = stream_labels;
						l_srt.push_back({"method", "srt"});
						b.AddGauge("ome_stream_rtt_seconds", l_srt, srt_stats.rtt_seconds);
						if (is_input)
						{
							b.AddCounter("ome_stream_srt_lost_packets", stream_labels, srt_stats.recv_lost_packets_total);
							b.AddCounter("ome_stream_srt_dropped_packets", stream_labels, srt_stats.recv_dropped_packets_total);
						}
					}

					// Tracks
					for (const auto &[track_id, track] : sm->GetTracks())
					{
						if (track == nullptr) continue;
						auto mt = track->GetMediaType();
						if (mt != cmn::MediaType::Video && mt != cmn::MediaType::Audio)
						{
							continue;
						}

						auto tlabels = MakeTrackLabels(stream_labels, track);
						auto stats	 = sm->GetTrackStats(track_id);

						int32_t bitrate = (stats != nullptr && stats->GetBitrateByMeasured() > 0)
											  ? stats->GetBitrateByMeasured()
											  : track->GetBitrate();
						b.AddGauge("ome_track_bitrate_bps", tlabels, static_cast<double>(bitrate));

						if (mt == cmn::MediaType::Video)
						{
							double fps = (stats != nullptr && stats->GetFrameRateByMeasured() > 0.0)
											 ? stats->GetFrameRateByMeasured()
											 : track->GetFrameRate();
							b.AddGauge("ome_track_framerate_fps", tlabels, fps);

							if (!is_pull_reconn && stats != nullptr)
							{
								if (auto kf_sec = stats->GetKeyframeIntervalSeconds(); kf_sec.has_value())
								{
									b.AddGauge("ome_track_keyframe_interval_seconds", tlabels, *kf_sec);
								}
							}

							auto res = track->GetResolution();
							b.AddGaugeUint64("ome_track_width", tlabels, res.width > 0 ? static_cast<uint64_t>(res.width) : 0u);
							b.AddGaugeUint64("ome_track_height", tlabels, res.height > 0 ? static_cast<uint64_t>(res.height) : 0u);
							bool has_bframes = (stats != nullptr) ? stats->HasBframes() : false;
							b.AddGaugeUint64("ome_track_has_bframes", tlabels, has_bframes ? 1u : 0u);
						}
						else if (mt == cmn::MediaType::Audio)
						{
							b.AddGauge("ome_track_samplerate_hertz", tlabels, static_cast<double>(track->GetSampleRate()));
							b.AddGaugeUint64("ome_track_channels", tlabels, track->GetChannel().GetCounts());
						}
					}
				};

				emit_stream_level(MakeStreamLabels(entry.vhost_name, entry.app_name, in_sm, ""), in_sm, true);
				for (const auto &out_sm : entry.output_streams)
				{
					emit_stream_level(MakeStreamLabels(entry.vhost_name, entry.app_name, in_sm, out_sm->GetName().CStr()), out_sm, false);
				}
			}
		}

		//----------------------------------------------------------------------
		// 5. queue
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorQueue) != 0)
		{
			b.DeclareFamily("ome_queue_size", MetricType::Gauge, "",
							"Current number of messages in the managed queue.");
			b.DeclareFamily("ome_queue_peak", MetricType::Gauge, "",
							"Peak number of messages observed.");
			b.DeclareFamily("ome_queue_threshold", MetricType::Gauge, "",
							"Message-count threshold of the managed queue.");
			b.DeclareFamily("ome_queue_dropped", MetricType::Counter, "",
							"Total messages dropped by the queue.");
			b.DeclareFamily("ome_queue_input_messages_per_second", MetricType::Gauge, "",
							"Messages enqueued per second.");
			b.DeclareFamily("ome_queue_output_messages_per_second", MetricType::Gauge, "",
							"Messages dequeued per second.");
			b.DeclareFamily("ome_queue_waiting_seconds", MetricType::Gauge, "seconds",
							"EWMA waiting time of dequeued messages in seconds.");

			std::string filter_vhost  = (scope.vhost != nullptr) ? scope.vhost->GetName().CStr() : "";
			std::string filter_app	  = (scope.app != nullptr) ? scope.app->GetVHostAppName().GetAppName().CStr() : "";
			std::string filter_stream = (scope.stream != nullptr) ? scope.stream->GetName().CStr() : "";

			for (const auto &[qid, qm] : monitoring->GetQueueMetricsList())
			{
				if (qm == nullptr) continue;

				auto urn = qm->GetUrn();
				std::string q_vhost;
				std::string q_app;
				std::string q_stream;
				std::string q_urn_str;
				if (urn != nullptr)
				{
					q_urn_str = urn->ToString().CStr();
					if (urn->GetVHostAppName().IsValid())
					{
						q_vhost = urn->GetVHostAppName().GetVHostName().CStr();
						q_app	= urn->GetVHostAppName().GetAppName().CStr();
					}
					q_stream = urn->GetStreamName().CStr();
				}

				if (!filter_vhost.empty() && q_vhost != filter_vhost) continue;
				if (!filter_app.empty() && q_app != filter_app) continue;
				if (!filter_stream.empty() && q_stream != filter_stream) continue;

				std::vector<Label> qlabels;
				if (!q_vhost.empty()) qlabels.push_back({"vhost", q_vhost});
				if (!q_app.empty()) qlabels.push_back({"app", q_app});
				if (!q_stream.empty()) qlabels.push_back({"stream", q_stream});
				qlabels.push_back({"queue_id", std::to_string(qm->GetId())});
				qlabels.push_back({"urn", q_urn_str});
				qlabels.push_back({"type", qm->GetTypeName().CStr()});

				b.AddGaugeUint64("ome_queue_size", qlabels, qm->GetSize());
				b.AddGaugeUint64("ome_queue_peak", qlabels, qm->GetPeak());
				b.AddGaugeUint64("ome_queue_threshold", qlabels, qm->GetThreshold());
				b.AddCounter("ome_queue_dropped", qlabels, qm->GetDropCount());
				b.AddGaugeUint64("ome_queue_input_messages_per_second", qlabels, qm->GetInputMessagePerSecond());
				b.AddGaugeUint64("ome_queue_output_messages_per_second", qlabels, qm->GetOutputMessagePerSecond());
				b.AddGauge("ome_queue_waiting_seconds", qlabels, static_cast<double>(qm->GetWaitingTime()) / 1000000.0);
			}
		}

		//----------------------------------------------------------------------
		// 6. push
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorPush) != 0 && orchestrator != nullptr)
		{
			b.DeclareFamily("ome_push_transmit_bytes", MetricType::Counter, "bytes",
							"Media bytes transmitted to the push/record target.");
			b.DeclareFamily("ome_push_state", MetricType::Stateset, "",
							"Push lifecycle state.");

			static const std::vector<std::string_view> kPushStates = {
				"ready", "connecting", "pushing", "recording", "stopping", "stopped", "error"};

			std::string filter_stream = (scope.stream != nullptr) ? scope.stream->GetName().CStr() : "";

			auto push_pub = std::dynamic_pointer_cast<pub::PushPublisher>(orchestrator->GetPublisherFromType(PublisherType::Push));
			auto file_pub = std::dynamic_pointer_cast<pub::FilePublisher>(orchestrator->GetPublisherFromType(PublisherType::File));

			for (const auto &app_entry : scoped_apps)
			{
				const auto &vhost_app_name = app_entry.app_metrics->GetVHostAppName();

				if (push_pub != nullptr)
				{
					auto push_app = std::dynamic_pointer_cast<pub::PushApplication>(push_pub->GetApplicationByName(vhost_app_name));
					if (push_app != nullptr)
					{
						auto query = std::make_shared<info::Push>();
						std::vector<std::shared_ptr<info::Push>> pushes;
						if (push_app->GetPushes(query, pushes) == nullptr)
						{
							for (const auto &p : pushes)
							{
								if (p == nullptr) continue;
								std::string sname = p->GetStreamName().CStr();
								if (!filter_stream.empty() && sname != filter_stream) continue;

								std::vector<Label> plabels = {
									{"vhost", app_entry.vhost_name},
									{"app", app_entry.app_name},
									{"stream", sname},
									{"id", p->GetId().CStr()},
									{"protocol", PushProtocolLabel(p->GetProtocolType(), p->GetProtocol())},
								};

								b.AddCounter("ome_push_transmit_bytes", plabels, p->GetPushTotalBytes());
								b.AddStateset("ome_push_state", plabels, kPushStates, PushStateLabel(p->GetState()));
							}
						}
					}
				}

				if (file_pub != nullptr)
				{
					auto file_app = std::dynamic_pointer_cast<pub::FileApplication>(file_pub->GetApplicationByName(vhost_app_name));
					if (file_app != nullptr)
					{
						auto query = std::make_shared<info::Record>();
						std::vector<std::shared_ptr<info::Record>> records;
						if (file_app->GetRecords(query, records) == nullptr)
						{
							for (const auto &r : records)
							{
								if (r == nullptr) continue;
								std::string sname = r->GetStreamName().CStr();
								if (!filter_stream.empty() && sname != filter_stream) continue;

								std::vector<Label> rlabels = {
									{"vhost", app_entry.vhost_name},
									{"app", app_entry.app_name},
									{"stream", sname},
									{"id", r->GetId().CStr()},
									{"protocol", "file"},
								};

								b.AddCounter("ome_push_transmit_bytes", rlabels, r->GetRecordTotalBytes());
								b.AddStateset("ome_push_state", rlabels, kPushStates, RecordStateLabel(r->GetState()));
							}
						}
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// 7. session (opt-in)
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorSession) != 0 && orchestrator != nullptr)
		{
			b.DeclareFamily("ome_session_transmit_bytes", MetricType::Counter, "bytes",
							"Bytes transmitted to this subscriber session.");
			b.DeclareFamily("ome_session_transmit_bps", MetricType::Gauge, "",
							"Current transmit throughput to the session in bits per second.");
			b.DeclareFamily("ome_session_rtt_seconds", MetricType::Gauge, "seconds",
							"Per-session round-trip time, split by publisher and method.");
			b.DeclareFamily("ome_session_srt_lost_packets", MetricType::Counter, "packets",
							"SRT egress packets reported lost via NAK by the subscriber.");
			b.DeclareFamily("ome_session_srt_retransmitted_packets", MetricType::Counter, "packets",
							"SRT egress packets retransmitted to the subscriber.");
			b.DeclareFamily("ome_session_srt_dropped_packets", MetricType::Counter, "packets",
							"SRT egress packets dropped too-late-to-send (TLPKTDROP).");
			b.DeclareFamily("ome_session_http_request_duration_seconds", MetricType::Summary, "seconds",
							"LL-HLS and HLS session HTTP request duration summary.");
			b.DeclareFamily("ome_session_http_request_processing_seconds", MetricType::Summary, "seconds",
							"LL-HLS session HTTP request processing time summary.");

			auto *http_timing = http::svr::HttpRequestTimingManager::GetInstance();
			bool http_enabled = http_timing->IsEnabled();

			for (const auto &entry : scoped_streams)
			{
				for (const auto &out_sm : entry.output_streams)
				{
					auto stream_labels = MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr());
					for (PublisherType pt : kSessionPublishers)
					{
						auto pub_stream = orchestrator->GetPublisherStream(pt, out_sm);
						if (pub_stream == nullptr) continue;

						auto sessions = pub_stream->GetAllSessions();
						for (const auto &[sid, session] : sessions)
						{
							if (session == nullptr) continue;
							auto slabels = MakeSessionLabels(stream_labels, pt, session);

							b.AddCounter("ome_session_transmit_bytes", slabels, session->GetSessionSentBytes());
							b.AddGauge("ome_session_transmit_bps", slabels, session->GetSessionTransmitBps());

							auto sock = session->GetSessionTransportSocket();
							if (pt == PublisherType::Webrtc)
							{
								auto l_rtcp = slabels;
								l_rtcp.push_back({"method", "rtcp"});
								b.AddGauge("ome_session_rtt_seconds", l_rtcp, session->GetSessionRtcpRttSeconds().value_or(0.0));

								auto l_stun = slabels;
								l_stun.push_back({"method", "stun"});
								b.AddGauge("ome_session_rtt_seconds", l_stun, session->GetSessionStunRttSeconds().value_or(0.0));

								if (sock != nullptr && sock->GetType() == ov::SocketType::Tcp)
								{
									ov::Socket::TcpRttInfo tcp_rtt{};
									if (sock->GetTcpRtt(&tcp_rtt))
									{
										auto l_tcp = slabels;
										l_tcp.push_back({"method", "tcp"});
										b.AddGauge("ome_session_rtt_seconds", l_tcp, tcp_rtt.rtt_seconds);

										auto l_min = slabels;
										l_min.push_back({"method", "tcp_min"});
										b.AddGauge("ome_session_rtt_seconds", l_min, tcp_rtt.min_rtt_seconds);
									}
								}
							}
							else if (pt == PublisherType::LLHls || pt == PublisherType::Hls || pt == PublisherType::Ovt)
							{
								if (sock != nullptr && sock->GetType() == ov::SocketType::Tcp)
								{
									ov::Socket::TcpRttInfo tcp_rtt{};
									if (sock->GetTcpRtt(&tcp_rtt))
									{
										auto l_tcp = slabels;
										l_tcp.push_back({"method", "tcp"});
										b.AddGauge("ome_session_rtt_seconds", l_tcp, tcp_rtt.rtt_seconds);

										auto l_min = slabels;
										l_min.push_back({"method", "tcp_min"});
										b.AddGauge("ome_session_rtt_seconds", l_min, tcp_rtt.min_rtt_seconds);
									}
								}
							}
							else if (pt == PublisherType::Srt)
							{
								if (sock != nullptr && sock->GetType() == ov::SocketType::Srt)
								{
									ov::Socket::SrtTransportStats srt_st{};
									if (sock->GetSrtStats(&srt_st))
									{
										auto l_srt = slabels;
										l_srt.push_back({"method", "srt"});
										b.AddGauge("ome_session_rtt_seconds", l_srt, srt_st.rtt_seconds);
										b.AddCounter("ome_session_srt_lost_packets", slabels, srt_st.send_lost_packets_total);
										b.AddCounter("ome_session_srt_retransmitted_packets", slabels, srt_st.send_retransmitted_packets_total);
										b.AddCounter("ome_session_srt_dropped_packets", slabels, srt_st.send_dropped_packets_total);
									}
								}
							}

							if (http_enabled && (pt == PublisherType::LLHls || pt == PublisherType::Hls))
							{
								auto snap = http_timing->GetSessionSnapshot(session->GetId());
								for (const auto &[k, v] : snap.duration)
								{
									auto hlabels = slabels;
									hlabels.push_back({"kind", http::svr::HttpTimingKindToString(k.kind)});
									hlabels.push_back({"held", k.held ? "true" : "false"});
									hlabels.push_back({"status_code", k.status_code});
									b.AddSummary("ome_session_http_request_duration_seconds", hlabels, v.count, v.sum_seconds);
								}
								if (pt == PublisherType::LLHls)
								{
									for (const auto &[k, v] : snap.processing)
									{
										auto hlabels = slabels;
										hlabels.push_back({"kind", http::svr::HttpTimingKindToString(k.kind)});
										hlabels.push_back({"held", k.held ? "true" : "false"});
										hlabels.push_back({"status_code", k.status_code});
										b.AddSummary("ome_session_http_request_processing_seconds", hlabels, v.count, v.sum_seconds);
									}
								}
							}
						}
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// 8. frame_latency
		//----------------------------------------------------------------------
		auto *fl_mgr = mon::openmetrics::FrameLatencyManager::GetInstance();
		if ((mask & mon::openmetrics::kCollectorFrameLatency) != 0 && fl_mgr->IsEnabled())
		{
			b.DeclareFamily("ome_frame_latency_buckets", MetricType::Info, "",
							"The histogram bounds this server uses for frame latency.");
			b.DeclareFamily("ome_track_ingest_latency_seconds", MetricType::Histogram, "seconds",
							"Origin receiving the frame to writing it to this edge.");
			b.DeclareFamily("ome_track_relay_latency_seconds", MetricType::Histogram, "seconds",
							"Origin writing the frame to this edge receiving its first fragment.");
			b.DeclareFamily("ome_track_egress_latency_seconds", MetricType::Histogram, "seconds",
							"This server receiving the frame to handing its last RTP packet to a WebRTC viewer.");
			b.DeclareFamily("ome_track_end_to_end_latency_seconds", MetricType::Histogram, "seconds",
							"Origin receiving the frame to this server handing its last RTP packet to a WebRTC viewer.");
			b.DeclareFamily("ome_track_ingest_latency_excluded_observations", MetricType::Counter, "",
							"Excluded ingest latency observations by reason.");
			b.DeclareFamily("ome_track_relay_latency_excluded_observations", MetricType::Counter, "",
							"Excluded relay latency observations by reason.");
			b.DeclareFamily("ome_track_egress_latency_excluded_observations", MetricType::Counter, "",
							"Excluded egress latency observations by reason.");
			b.DeclareFamily("ome_track_end_to_end_latency_excluded_observations", MetricType::Counter, "",
							"Excluded end-to-end latency observations by reason.");

			if (!scoped_streams.empty())
			{
				b.AddInfo("ome_frame_latency_buckets", {{"bounds", fl_mgr->GetCanonicalBoundsString()}});
			}

			static const char *kSpanHistFamily[4] = {
				"ome_track_ingest_latency_seconds",
				"ome_track_relay_latency_seconds",
				"ome_track_egress_latency_seconds",
				"ome_track_end_to_end_latency_seconds",
			};
			static const char *kSpanExclFamily[4] = {
				"ome_track_ingest_latency_excluded_observations",
				"ome_track_relay_latency_excluded_observations",
				"ome_track_egress_latency_excluded_observations",
				"ome_track_end_to_end_latency_excluded_observations",
			};

			const auto &bounds = fl_mgr->GetBounds();
			for (const auto &entry : scoped_streams)
			{
				for (const auto &out_sm : entry.output_streams)
				{
					auto stream_labels = MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr());
					auto snapshots	   = fl_mgr->GetStreamSnapshots(out_sm->GetId());
					for (const auto &ts : snapshots)
					{
						auto track = out_sm->GetTrack(static_cast<int32_t>(ts.track_id));
						if (track == nullptr) continue;
						auto tlabels = MakeTrackLabels(stream_labels, track);

						for (size_t i = 0; i < 4; ++i)
						{
							const auto &sp = ts.spans[i];
							if (sp.total_count > 0)
							{
								b.AddHistogram(kSpanHistFamily[i], tlabels, bounds, sp.cumulative_buckets, sp.total_count, sp.sum_seconds);
							}
							if (sp.excluded_neg > 0)
							{
								auto el = tlabels;
								el.push_back({"reason", "negative"});
								b.AddCounter(kSpanExclFamily[i], el, sp.excluded_neg);
							}
							if (sp.excluded_unm > 0 && i != static_cast<size_t>(mon::openmetrics::LatencySpan::Egress))
							{
								auto el = tlabels;
								el.push_back({"reason", "unmeasured"});
								b.AddCounter(kSpanExclFamily[i], el, sp.excluded_unm);
							}
						}
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// 9. session_latency (opt-in; WebRTC sessions only)
		//----------------------------------------------------------------------
		if ((mask & mon::openmetrics::kCollectorSessionLatency) != 0 && fl_mgr->IsEnabled() && orchestrator != nullptr)
		{
			b.DeclareFamily("ome_session_egress_latency_seconds", MetricType::Summary, "seconds",
							"Per-session WebRTC egress latency summary per track.");

			for (const auto &entry : scoped_streams)
			{
				for (const auto &out_sm : entry.output_streams)
				{
					auto pub_stream = orchestrator->GetPublisherStream(PublisherType::Webrtc, out_sm);
					if (pub_stream == nullptr) continue;

					auto stream_labels = MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr());
					for (const auto &[sid, session] : pub_stream->GetAllSessions())
					{
						if (session == nullptr) continue;
						auto summaries = session->GetSessionEgressLatencySnapshot();
						if (summaries.empty()) continue;

						auto slabels = MakeSessionLabels(stream_labels, PublisherType::Webrtc, session);
						for (const auto &sum : summaries)
						{
							auto track = out_sm->GetTrack(static_cast<int32_t>(sum.track_id));
							if (track == nullptr) continue;
							auto labels = slabels;
							labels.push_back({"track", std::to_string(track->GetId())});
							labels.push_back({"media", MediaTypeLabel(track->GetMediaType())});
							labels.push_back({"codec", CodecLabel(track->GetCodecId())});
							b.AddSummary("ome_session_egress_latency_seconds", labels, sum.count, sum.sum_seconds);
						}
					}
				}
			}
		}

		//----------------------------------------------------------------------
		// 10. http
		//----------------------------------------------------------------------
		auto *http_timing = http::svr::HttpRequestTimingManager::GetInstance();
		if ((mask & mon::openmetrics::kCollectorHttp) != 0 && http_timing->IsEnabled())
		{
			if (scope.is_root)
			{
				b.DeclareFamily("ome_http_request_duration_seconds", MetricType::Histogram, "seconds",
								"Time from an HTTP request arriving to the start of its response, server-wide.");
				b.DeclareFamily("ome_http_request_processing_seconds", MetricType::Histogram, "seconds",
								"LL-HLS HTTP request processing time from due to response start, server-wide.");
				b.DeclareFamily("ome_http_requests_unanswered", MetricType::Counter, "",
								"HTTP requests that ended before a response was sent, server-wide, by reason.");
				b.DeclareFamily("ome_http_requests_in_progress", MetricType::Gauge, "",
								"HTTP requests currently in progress, by handler.");

				auto srv_snap	   = http_timing->GetServerSnapshot();
				const auto &bounds = http::svr::HttpRequestTimingManager::FixedBounds();

				for (const auto &[k, v] : srv_snap.duration)
				{
					std::vector<Label> labels = {
						{"handler", http::svr::HttpTimingHandlerToString(k.handler)},
						{"kind", http::svr::HttpTimingKindToString(k.kind)},
						{"held", k.held ? "true" : "false"},
						{"status_code", k.status_code},
					};
					b.AddHistogram("ome_http_request_duration_seconds", labels, bounds, v.cumulative_buckets, v.total_count, v.sum_seconds);
				}

				for (const auto &[k, v] : srv_snap.processing)
				{
					std::vector<Label> labels = {
						{"handler", http::svr::HttpTimingHandlerToString(k.handler)},
						{"kind", http::svr::HttpTimingKindToString(k.kind)},
						{"held", k.held ? "true" : "false"},
						{"status_code", k.status_code},
					};
					b.AddHistogram("ome_http_request_processing_seconds", labels, bounds, v.cumulative_buckets, v.total_count, v.sum_seconds);
				}

				for (const auto &[k, count] : srv_snap.unanswered)
				{
					std::vector<Label> labels = {
						{"handler", http::svr::HttpTimingHandlerToString(k.handler)},
						{"kind", http::svr::HttpTimingKindToString(k.kind)},
						{"held", k.held ? "true" : "false"},
						{"reason", http::svr::HttpUnansweredReasonToString(k.reason)},
					};
					b.AddCounter("ome_http_requests_unanswered", labels, count);
				}

				for (const auto &[h, val] : srv_snap.in_progress)
				{
					b.AddGauge("ome_http_requests_in_progress", {{"handler", http::svr::HttpTimingHandlerToString(h)}}, static_cast<double>(val));
				}
			}

			// Per-stream HTTP summaries (all scopes covering the stream)
			b.DeclareFamily("ome_stream_http_request_duration_seconds", MetricType::Summary, "seconds",
							"Per-stream HTTP request duration summary.");
			b.DeclareFamily("ome_stream_http_request_processing_seconds", MetricType::Summary, "seconds",
							"Per-stream LL-HLS HTTP request processing time summary.");

			auto emit_stream_http = [&](const std::vector<Label> &stream_labels, info::stream_id_t stream_id) {
				auto snap = http_timing->GetStreamSnapshot(stream_id);
				if (snap.duration.empty() && snap.processing.empty()) return;

				for (const auto &[k, v] : snap.duration)
				{
					auto labels = stream_labels;
					labels.push_back({"kind", http::svr::HttpTimingKindToString(k.kind)});
					labels.push_back({"held", k.held ? "true" : "false"});
					labels.push_back({"status_code", k.status_code});
					b.AddSummary("ome_stream_http_request_duration_seconds", labels, v.count, v.sum_seconds);
				}
				for (const auto &[k, v] : snap.processing)
				{
					auto labels = stream_labels;
					labels.push_back({"kind", http::svr::HttpTimingKindToString(k.kind)});
					labels.push_back({"held", k.held ? "true" : "false"});
					labels.push_back({"status_code", k.status_code});
					b.AddSummary("ome_stream_http_request_processing_seconds", labels, v.count, v.sum_seconds);
				}
			};

			for (const auto &entry : scoped_streams)
			{
				emit_stream_http(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, ""), entry.input_stream->GetId());
				for (const auto &out_sm : entry.output_streams)
				{
					emit_stream_http(MakeStreamLabels(entry.vhost_name, entry.app_name, entry.input_stream, out_sm->GetName().CStr()), out_sm->GetId());
				}
			}
		}

		return b.Finish();
	}
}  // namespace api::v2
