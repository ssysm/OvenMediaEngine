//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <base/info/application.h>
#include <base/info/host.h>
#include <base/info/media_track.h>
#include <base/info/stream.h>
#include <base/mediarouter/media_buffer.h>
#include <gtest/gtest.h>
#include <modules/http/server/http_request_timing.h>
#include <monitoring/monitoring.h>
#include <monitoring/openmetrics/frame_latency.h>

#include "metrics_controller.h"
#include "openmetrics_renderer.h"

namespace
{
	class TestAppInfo : public info::Application
	{
	public:
		TestAppInfo(const info::Host &host, info::application_id_t id, const info::VHostAppName &name)
			: info::Application(host, id, name, false)
		{
		}
	};
}  // namespace

TEST(OpenMetricsEndpointTest, AcceptsGzipHeaderParsing)
{
	using api::v2::MetricsController;
	EXPECT_TRUE(MetricsController::AcceptsGzip("gzip"));
	EXPECT_TRUE(MetricsController::AcceptsGzip("deflate, gzip, br"));
	EXPECT_TRUE(MetricsController::AcceptsGzip("GZIP;q=0.8"));
	EXPECT_TRUE(MetricsController::AcceptsGzip("x-gzip"));
	EXPECT_FALSE(MetricsController::AcceptsGzip(""));
	EXPECT_FALSE(MetricsController::AcceptsGzip("br, deflate"));
	EXPECT_FALSE(MetricsController::AcceptsGzip("gzip;q=0"));
	EXPECT_FALSE(MetricsController::AcceptsGzip("gzip; q=0.000"));
	EXPECT_FALSE(MetricsController::AcceptsGzip("identity, gzip;q=0.0"));
}

TEST(OpenMetricsEndpointTest, RendersAllCoreTrafficConnectionStreamLatencyAndHttpCollectors)
{
	// Configure FrameLatency & HTTPRequestTiming
	auto doc = std::make_shared<pugi::xml_document>();
	ASSERT_TRUE(doc->load_string(R"(<FrameLatency><Enable>true</Enable><Buckets>0.01,0.05,0.2,1</Buckets></FrameLatency>)"));
	cfg::DataSource ds("", "Server.xml", doc, doc->child("FrameLatency"));
	cfg::modules::measurements::FrameLatency fl_cfg;
	fl_cfg.FromDataSource("FrameLatency", cfg::ItemName("FrameLatency"), ds);
	auto *fl_mgr = mon::openmetrics::FrameLatencyManager::GetInstance();
	fl_mgr->Start(fl_cfg);

	auto *http_mgr = http::svr::HttpRequestTimingManager::GetInstance();
	http_mgr->StartForTest(true);
	http_mgr->PrepopulateHandler(http::svr::HttpTimingHandler::Api);
	http_mgr->IncrementInProgress(http::svr::HttpTimingHandler::Api);

	// Construct live HostMetrics -> ApplicationMetrics -> input & output StreamMetrics
	auto vhost_doc = std::make_shared<pugi::xml_document>();
	ASSERT_TRUE(vhost_doc->load_string(R"(<VirtualHost><Name>default</Name></VirtualHost>)"));
	cfg::DataSource vhost_ds("", "Server.xml", vhost_doc, vhost_doc->child("VirtualHost"));
	cfg::vhost::VirtualHost vhost_cfg;
	vhost_cfg.FromDataSource("VirtualHost", cfg::ItemName("VirtualHost"), vhost_ds);

	info::Host host_info("OME", "srv-test", vhost_cfg);
	auto host_metrics = std::make_shared<mon::HostMetrics>(host_info);

	TestAppInfo app_info(host_info, 10, info::VHostAppName("default", "app"));
	auto app_metrics = std::make_shared<mon::ApplicationMetrics>(host_metrics, app_info);

	// Input stream (OVT pull)
	auto in_stream = std::make_shared<info::Stream>(app_info, 1001, StreamSourceType::Ovt);
	in_stream->SetName("live");
	auto vtrack = std::make_shared<MediaTrack>();
	vtrack->SetId(0);
	vtrack->SetMediaType(cmn::MediaType::Video);
	vtrack->SetCodecId(cmn::MediaCodecId::H264);
	vtrack->SetResolution(1920, 1080);
	vtrack->SetTimeBase(1, 90000);
	in_stream->AddTrack(vtrack);

	auto atrack = std::make_shared<MediaTrack>();
	atrack->SetId(1);
	atrack->SetMediaType(cmn::MediaType::Audio);
	atrack->SetCodecId(cmn::MediaCodecId::Opus);
	atrack->SetSampleRate(48000);
	atrack->SetChannelCount(2);
	atrack->SetTimeBase(1, 48000);
	in_stream->AddTrack(atrack);

	auto in_metrics = std::make_shared<mon::StreamMetrics>(app_metrics, *in_stream);
	in_metrics->SetOriginConnectionTimeMSec(45);
	in_metrics->IncreaseBytesIn(98765);
	in_metrics->OnSessionConnected(PublisherType::Webrtc);
	in_metrics->IncreaseBytesOut(PublisherType::Webrtc, 54321);

	// Feed keyframe + delta frame into TrackStats to populate keyframe interval
	auto in_vstats = in_metrics->GetTrackStats(0);
	ASSERT_NE(in_vstats, nullptr);
	in_vstats->SetHasBframes(true);
	uint8_t dummy[4] = {0, 0, 0, 1};
	auto kf = std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, dummy, sizeof(dummy), 0, 0, 3000, MediaPacketFlag::Key, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
	auto df = std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, dummy, sizeof(dummy), 180000, 180000, 3000, MediaPacketFlag::NoFlag, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
	in_vstats->OnFrameAdded(kf, cmn::Timebase(1, 90000), cmn::MediaType::Video);
	in_vstats->OnFrameAdded(df, cmn::Timebase(1, 90000), cmn::MediaType::Video);

	// Output rendition stream
	auto out_stream = std::make_shared<info::Stream>(app_info, 1002, StreamSourceType::Ovt);
	out_stream->SetName("live_720p");
	out_stream->LinkInputStream(in_stream);
	out_stream->AddTrack(vtrack);
	auto out_metrics = std::make_shared<mon::StreamMetrics>(app_metrics, *out_stream);
	out_metrics->IncreaseBytesOut(PublisherType::LLHls, 11111);

	// Record a FrameLatency sample on output stream
	auto fl_pkt = std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, dummy, sizeof(dummy), 90000, 90000, 3000, MediaPacketFlag::Key, cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
	FrameLatencyStamps st;
	st.origin_arrival_us = 1000000;
	st.origin_send_us	 = 1015000;
	st.arrival_us		 = 1025000;
	st.from_ovt_origin	 = true;
	fl_pkt->SetFrameLatencyStamps(st);
	fl_mgr->OnOutputTrackFrame(*out_stream, fl_pkt);

	// Record an HTTP request matched to out_stream
	http::svr::HttpRequestTimingManager::AnsweredEvent ev;
	ev.handler		  = http::svr::HttpTimingHandler::LLHls;
	ev.kind			  = http::svr::HttpTimingKind::PartialSegment;
	ev.held			  = true;
	ev.status_code	  = "200";
	ev.duration_us	  = 120000;
	ev.wait_us		  = 100000;
	ev.processing_us  = 20000;
	ev.stream_id	  = 1002;
	ev.stream_label	  = "default/app/live_720p";
	ev.arrival_system = std::chrono::system_clock::now();
	http_mgr->RecordAnswered(ev);

	// Render at Stream scope (is_root = false): MUST omit server-wide HTTP families, MUST include per-stream HTTP summary!
	api::v2::OpenMetricsScope stream_scope;
	stream_scope.is_root		= false;
	stream_scope.vhost			= host_metrics;
	stream_scope.app			= app_metrics;
	stream_scope.stream			= in_metrics;
	stream_scope.output_streams = {out_metrics};
	stream_scope.collector_mask = mon::openmetrics::kDefaultCollectors;

	std::string stream_out = api::v2::OpenMetricsRenderer::Render(stream_scope);

	// Core
	EXPECT_NE(stream_out.find("# TYPE ome_build info\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_build_info{version="), std::string::npos);
	EXPECT_NE(stream_out.find("process_start_time_seconds "), std::string::npos);

	// Traffic & connection (exact direction labels!)
	EXPECT_NE(stream_out.find("ome_receive_bytes_total{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\"} 98765\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_transmit_bytes_total{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",publisher=\"webrtc\"} 54321\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_transmit_bytes_total{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"live_720p\",provider=\"ovt\",publisher=\"llhls\"} 11111\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_connections{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",publisher=\"webrtc\"} 1\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_connections_max{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\"} 1\n"), std::string::npos);

	// Stream & track metrics
	EXPECT_NE(stream_out.find("ome_stream_source_connect_seconds{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\"} 0.045\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_keyframe_interval_seconds{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 2\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_width{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 1920\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_height{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 1080\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_has_bframes{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 1\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_samplerate_hertz{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"1\",media=\"audio\",codec=\"opus\"} 48000\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_channels{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"\",provider=\"ovt\",track=\"1\",media=\"audio\",codec=\"opus\"} 2\n"), std::string::npos);

	// Frame latency
	EXPECT_NE(stream_out.find("ome_frame_latency_buckets_info{bounds=\"0.01,0.05,0.2,1\"} 1\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_ingest_latency_seconds_count{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"live_720p\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 1\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_track_relay_latency_seconds_count{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"live_720p\",provider=\"ovt\",track=\"0\",media=\"video\",codec=\"h264\"} 1\n"), std::string::npos);

	// Non-root scope: server-wide HTTP families MUST be absent; per-stream HTTP summary MUST be present!
	EXPECT_EQ(stream_out.find("ome_http_request_duration_seconds"), std::string::npos);
	EXPECT_EQ(stream_out.find("ome_http_requests_in_progress"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_stream_http_request_duration_seconds_count{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"live_720p\",provider=\"ovt\",kind=\"partial_segment\",held=\"true\",status_code=\"200\"} 1\n"), std::string::npos);
	EXPECT_NE(stream_out.find("ome_stream_http_request_processing_seconds_sum{vhost=\"default\",app=\"app\",stream=\"live\",stream_id=\"1001\",output_stream=\"live_720p\",provider=\"ovt\",kind=\"partial_segment\",held=\"true\",status_code=\"200\"} 0.02\n"), std::string::npos);

	// Ends with # EOF\n
	ASSERT_GE(stream_out.size(), 6u);
	EXPECT_EQ(stream_out.substr(stream_out.size() - 6), "# EOF\n");

	// Render at Root scope (is_root = true): MUST include server-wide HTTP families!
	api::v2::OpenMetricsScope root_scope;
	root_scope.is_root		  = true;
	root_scope.collector_mask = mon::openmetrics::kCollectorCore | mon::openmetrics::kCollectorHttp;
	std::string root_out	  = api::v2::OpenMetricsRenderer::Render(root_scope);

	EXPECT_NE(root_out.find("ome_http_request_duration_seconds_bucket{handler=\"llhls\",kind=\"partial_segment\",held=\"true\",status_code=\"200\",le=\"1.0\"}"), std::string::npos);
	EXPECT_NE(root_out.find("ome_http_request_processing_seconds_count{handler=\"llhls\",kind=\"partial_segment\",held=\"true\",status_code=\"200\"} 1\n"), std::string::npos);
	EXPECT_NE(root_out.find("ome_http_requests_in_progress{handler=\"api\"} 1\n"), std::string::npos);
	// Collector filter respected: traffic omitted
	EXPECT_EQ(root_out.find("ome_receive_bytes"), std::string::npos);

	// Cleanup
	http_mgr->DecrementInProgress(http::svr::HttpTimingHandler::Api);
	http_mgr->OnStreamDeleted(1002);
	http_mgr->Stop();
	fl_mgr->OnStreamDeleted(1002);
	fl_mgr->Stop();
}
