//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <arpa/inet.h>
#include <base/info/application.h>
#include <base/info/host.h>
#include <base/info/media_track.h>
#include <base/info/stream.h>
#include <base/info/track_stats.h>
#include <base/mediarouter/media_buffer.h>
#include <base/ovsocket/ovsocket.h>
#include <gtest/gtest.h>
#include <modules/http/server/http_request_timing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "frame_latency.h"

namespace
{
	class TestApplicationInfo : public info::Application
	{
	public:
		TestApplicationInfo(const info::Host &host, info::application_id_t id, const info::VHostAppName &name)
			: info::Application(host, id, name, false)
		{
		}
	};

	std::shared_ptr<MediaPacket> MakeVideoPacket(int64_t pts, int64_t dts, bool keyframe, size_t size = 1000)
	{
		std::vector<uint8_t> payload(size, 0xAA);
		return std::make_shared<MediaPacket>(cmn::MediaType::Video, 0, payload.data(), static_cast<int32_t>(payload.size()),
											 pts, dts, 3000,
											 keyframe ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag,
											 cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);
	}
}  // namespace

TEST(OpenMetricsFeaturesTest, TrackStatsKeyframeIntervalSemantics)
{
	TrackStats stats;
	cmn::Timebase tb(1, 90000);

	// Absent before first keyframe
	EXPECT_FALSE(stats.GetKeyframeIntervalSeconds().has_value());

	// Non-keyframe before first keyframe -> still absent
	stats.OnFrameAdded(MakeVideoPacket(0, 0, false), tb, cmn::MediaType::Video);
	EXPECT_FALSE(stats.GetKeyframeIntervalSeconds().has_value());

	// First keyframe -> still absent until a subsequent frame arrives
	stats.OnFrameAdded(MakeVideoPacket(90000, 90000, true), tb, cmn::MediaType::Video);
	EXPECT_FALSE(stats.GetKeyframeIntervalSeconds().has_value());

	// Subsequent delta frame 0.5s after first keyframe -> reports 0.5s
	stats.OnFrameAdded(MakeVideoPacket(135000, 135000, false), tb, cmn::MediaType::Video);
	ASSERT_TRUE(stats.GetKeyframeIntervalSeconds().has_value());
	EXPECT_NEAR(*stats.GetKeyframeIntervalSeconds(), 0.5, 1e-6);

	// Second keyframe at 2.0s after first keyframe -> reports 2.0s
	stats.OnFrameAdded(MakeVideoPacket(270000, 270000, true), tb, cmn::MediaType::Video);
	ASSERT_TRUE(stats.GetKeyframeIntervalSeconds().has_value());
	EXPECT_NEAR(*stats.GetKeyframeIntervalSeconds(), 2.0, 1e-6);

	// Next delta frame only 1.0s after second keyframe -> keeps 2.0s (last KF-to-KF duration)
	stats.OnFrameAdded(MakeVideoPacket(360000, 360000, false), tb, cmn::MediaType::Video);
	ASSERT_TRUE(stats.GetKeyframeIntervalSeconds().has_value());
	EXPECT_NEAR(*stats.GetKeyframeIntervalSeconds(), 2.0, 1e-6);

	// Next delta frame 3.5s after second keyframe -> expands to 3.5s once longer than previous interval
	stats.OnFrameAdded(MakeVideoPacket(585000, 585000, false), tb, cmn::MediaType::Video);
	ASSERT_TRUE(stats.GetKeyframeIntervalSeconds().has_value());
	EXPECT_NEAR(*stats.GetKeyframeIntervalSeconds(), 3.5, 1e-6);

	// Reconnect reset clears the series back to absent
	stats.ResetKeyframeIntervalForReconnect();
	EXPECT_FALSE(stats.GetKeyframeIntervalSeconds().has_value());
}

TEST(OpenMetricsFeaturesTest, SocketTcpRttOnLoopbackAndNonTcpRejection)
{
	int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
	ASSERT_GE(listen_fd, 0);

	int opt = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	sockaddr_in addr{};
	addr.sin_family		 = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port		 = 0;
	ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), 0);
	ASSERT_EQ(::listen(listen_fd, 4), 0);

	socklen_t len = sizeof(addr);
	ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &len), 0);
	uint16_t port = ntohs(addr.sin_port);

	auto tcp_pool = ov::SocketPool::Create("test-tcp-rtt", ov::SocketType::Tcp, false);
	ASSERT_NE(tcp_pool, nullptr);
	ASSERT_TRUE(tcp_pool->Initialize(1));

	auto client = tcp_pool->AllocSocket(ov::SocketFamily::Inet);
	ASSERT_NE(client, nullptr);
	ASSERT_EQ(client->Connect(ov::SocketAddress::CreateAndGetFirst("127.0.0.1", port), 2000), nullptr);

	int server_fd = ::accept(listen_fd, nullptr, nullptr);
	ASSERT_GE(server_fd, 0);

	const char ping[] = "ping";
	ASSERT_GT(client->Send(ping, sizeof(ping)), 0);
	char buf[16]{};
	ASSERT_EQ(::recv(server_fd, buf, sizeof(buf), 0), static_cast<ssize_t>(sizeof(ping)));

	ov::Socket::TcpRttInfo rtt{};
	EXPECT_TRUE(client->GetTcpRtt(&rtt));
	EXPECT_GE(rtt.rtt_seconds, 0.0);
	EXPECT_GE(rtt.min_rtt_seconds, 0.0);

	client->CloseImmediately();
	tcp_pool->Uninitialize();
	::close(server_fd);
	::close(listen_fd);

	// Non-TCP socket returns false
	auto udp_pool = ov::SocketPool::Create("test-udp-rtt", ov::SocketType::Udp, false);
	ASSERT_NE(udp_pool, nullptr);
	ASSERT_TRUE(udp_pool->Initialize(1));
	auto udp_sock = udp_pool->AllocSocket(ov::SocketFamily::Inet);
	ASSERT_NE(udp_sock, nullptr);
	EXPECT_FALSE(udp_sock->GetTcpRtt(&rtt));
	udp_sock->CloseImmediately();
	udp_pool->Uninitialize();
}

TEST(OpenMetricsFeaturesTest, FrameLatencySpansExclusionsSessionCapAndDebugLog)
{
	using namespace mon::openmetrics;

	// Build a minimal FrameLatency config with enabled = true and custom buckets
	auto doc = std::make_shared<pugi::xml_document>();
	ASSERT_TRUE(doc->load_string(R"(<FrameLatency><Enable>true</Enable><Buckets>0.01,0.05,0.2,1</Buckets></FrameLatency>)"));
	cfg::DataSource ds("", "Server.xml", doc, doc->child("FrameLatency"));
	cfg::modules::measurements::FrameLatency fl_cfg;
	fl_cfg.FromDataSource("FrameLatency", cfg::ItemName("FrameLatency"), ds);

	auto *mgr = FrameLatencyManager::GetInstance();
	ov_log_set_enable("FrameLatency", OVLogLevelDebug, true);
	mgr->Start(fl_cfg);
	ASSERT_TRUE(mgr->IsEnabled());
	EXPECT_EQ(mgr->GetCanonicalBoundsString(), "0.01,0.05,0.2,1");

	// Build a dummy output stream
	cfg::vhost::VirtualHost vhost_cfg;
	info::Host host("OME", "srv-1", vhost_cfg);
	TestApplicationInfo app(host, 1, info::VHostAppName("default", "app"));
	info::Stream out_stream(app, 42, StreamSourceType::Ovt);
	out_stream.SetName("stream");

	// Frame 1: OVT2 origin timestamps -> ingest = 20ms, relay = 15ms
	auto pkt1 = MakeVideoPacket(30000, 30000, true);
	FrameLatencyStamps st1;
	st1.origin_arrival_us = 1000000;
	st1.origin_send_us	  = 1020000;  // +20ms
	st1.arrival_us		  = 1035000;  // +15ms
	st1.has_pts			  = true;
	st1.pts				  = 30000;
	st1.from_ovt_origin	  = true;
	pkt1->SetFrameLatencyStamps(st1);
	mgr->OnOutputTrackFrame(out_stream, pkt1);

	// Frame 2: Negative relay (-5ms, clock skew), valid ingest (5ms)
	auto pkt2 = MakeVideoPacket(33000, 33000, false);
	FrameLatencyStamps st2;
	st2.origin_arrival_us = 2000000;
	st2.origin_send_us	  = 2005000;  // +5ms
	st2.arrival_us		  = 2000000;  // -5ms -> negative!
	st2.from_ovt_origin	  = true;
	pkt2->SetFrameLatencyStamps(st2);
	mgr->OnOutputTrackFrame(out_stream, pkt2);

	// Frame 3: Missing origin timestamps AFTER origin stamps were already seen -> unmeasured!
	auto pkt3 = MakeVideoPacket(36000, 36000, false);
	FrameLatencyStamps st3;
	st3.arrival_us		= 3000000;
	st3.from_ovt_origin = false;
	pkt3->SetFrameLatencyStamps(st3);
	mgr->OnOutputTrackFrame(out_stream, pkt3);

	// WebRTC sends for Frame 1 & Frame 3
	SessionEgressLatency sess_lat;
	// Frame 1 sent at +40ms after edge arrival -> egress=40ms, end_to_end=75ms
	mgr->OnWebRtcFrameSent(out_stream, 0, cmn::MediaType::Video, 100, st1, 1075000, &sess_lat);
	// Frame 3 sent at +10ms -> egress=10ms, end_to_end=unmeasured!
	mgr->OnWebRtcFrameSent(out_stream, 0, cmn::MediaType::Video, 100, st3, 3010000, &sess_lat);
	// Negative egress (-5ms) -> excluded from both track egress and session egress!
	mgr->OnWebRtcFrameSent(out_stream, 0, cmn::MediaType::Video, 100, st3, 2995000, &sess_lat);

	auto snaps = mgr->GetStreamSnapshots(42);
	ASSERT_EQ(snaps.size(), 1u);
	const auto &ts = snaps[0];
	EXPECT_EQ(ts.track_id, 0u);

	// Ingest: 2 valid (20ms, 5ms), 0 neg, 1 unmeasured
	const auto &ing = ts.spans[static_cast<size_t>(LatencySpan::Ingest)];
	EXPECT_EQ(ing.total_count, 2u);
	EXPECT_NEAR(ing.sum_seconds, 0.025, 1e-6);
	EXPECT_EQ(ing.excluded_neg, 0u);
	EXPECT_EQ(ing.excluded_unm, 1u);
	// Buckets: 0.01->1 (5ms), 0.05->2 (20ms), 0.2->2, 1->2
	ASSERT_EQ(ing.cumulative_buckets.size(), 4u);
	EXPECT_EQ(ing.cumulative_buckets[0], 1u);
	EXPECT_EQ(ing.cumulative_buckets[1], 2u);

	// Relay: 1 valid (15ms), 1 neg, 1 unmeasured
	const auto &rel = ts.spans[static_cast<size_t>(LatencySpan::Relay)];
	EXPECT_EQ(rel.total_count, 1u);
	EXPECT_NEAR(rel.sum_seconds, 0.015, 1e-6);
	EXPECT_EQ(rel.excluded_neg, 1u);
	EXPECT_EQ(rel.excluded_unm, 1u);

	// Egress: 2 valid (40ms, 10ms), 1 neg, 0 unmeasured
	const auto &egr = ts.spans[static_cast<size_t>(LatencySpan::Egress)];
	EXPECT_EQ(egr.total_count, 2u);
	EXPECT_NEAR(egr.sum_seconds, 0.050, 1e-6);
	EXPECT_EQ(egr.excluded_neg, 1u);
	EXPECT_EQ(egr.excluded_unm, 0u);

	// End-to-end: 1 valid (75ms), 0 neg, 2 unmeasured (frame 3 sent twice without origin_arrival_us)
	const auto &e2e = ts.spans[static_cast<size_t>(LatencySpan::EndToEnd)];
	EXPECT_EQ(e2e.total_count, 1u);
	EXPECT_NEAR(e2e.sum_seconds, 0.075, 1e-6);
	EXPECT_EQ(e2e.excluded_unm, 2u);

	// SessionEgressLatency: 2 valid frames (40ms + 10ms = 50ms), negative excluded
	auto sess_snap = sess_lat.GetSnapshot();
	ASSERT_EQ(sess_snap.size(), 1u);
	EXPECT_EQ(sess_snap[0].track_id, 0u);
	EXPECT_EQ(sess_snap[0].count, 2u);
	EXPECT_NEAR(sess_snap[0].sum_seconds, 0.050, 1e-6);

	// Verify 32-track cap on SessionEgressLatency
	SessionEgressLatency cap_test;
	for (uint32_t tid = 0; tid < 40; ++tid)
	{
		cap_test.Observe(tid, 1000);
	}
	EXPECT_EQ(cap_test.GetSnapshot().size(), SessionEgressLatency::kMaxTracks);

	// Debug log lines: first line has no `w=`, second line has `w=<ms>ms`
	auto t0	   = std::chrono::steady_clock::now();
	auto lines1 = mgr->CollectDebugLogLines(1792479271, t0);
	ASSERT_EQ(lines1.size(), 1u);
	EXPECT_NE(lines1[0].find("sec=1792479271:"), std::string::npos);
	EXPECT_EQ(lines1[0].find(" w="), std::string::npos);
	EXPECT_NE(lines1[0].find("end_to_end n=1 max=75000us(session 100, pts 30000) count=1 d=1 mean=75000us"), std::string::npos);
	EXPECT_NE(lines1[0].find("unm=2"), std::string::npos);

	// Observe another frame and collect second line 1005ms later -> must include w=1005ms
	mgr->OnWebRtcFrameSent(out_stream, 0, cmn::MediaType::Video, 100, st1, 1085000, &sess_lat);
	auto lines2 = mgr->CollectDebugLogLines(1792479272, t0 + std::chrono::milliseconds(1005));
	ASSERT_EQ(lines2.size(), 1u);
	EXPECT_NE(lines2[0].find("sec=1792479272 w=1005ms:"), std::string::npos);
	EXPECT_NE(lines2[0].find("end_to_end n=1 max=85000us(session 100, pts 30000) count=2 d=1 mean=85000us"), std::string::npos);

	mgr->OnStreamDeleted(42);
	EXPECT_TRUE(mgr->GetStreamSnapshots(42).empty());
	mgr->Stop();
}

TEST(OpenMetricsFeaturesTest, HttpRequestTimingHistogramsSummariesSanitizationAndLogs)
{
	using namespace http::svr;

	// Query string sanitization keeps _HLS_msn & _HLS_part, strips secret policy tokens
	EXPECT_EQ(SanitizeTimingPath("/app/stream/llhls.m3u8?policy=secret123&_HLS_msn=42&signature=abc&_HLS_part=3"),
			  "/app/stream/llhls.m3u8?_HLS_msn=42&_HLS_part=3");
	EXPECT_EQ(SanitizeTimingPath("/app/stream/chunk.m4s?token=topsecret"),
			  "/app/stream/chunk.m4s");
	EXPECT_EQ(SanitizeTimingPath("http://example.com:3333/v2/metrics?collect[]=traffic"),
			  "/v2/metrics");

	// Fixed 18 bounds check
	const auto &bounds = HttpRequestTimingManager::FixedBounds();
	ASSERT_EQ(bounds.size(), 18u);
	EXPECT_DOUBLE_EQ(bounds.front(), 0.0001);
	EXPECT_DOUBLE_EQ(bounds.back(), 60.0);

	auto *mgr = HttpRequestTimingManager::GetInstance();
	mgr->StartForTest(true);
	ASSERT_TRUE(mgr->IsEnabled());

	// Prepopulate LL-HLS usual series
	mgr->PrepopulateHandler(HttpTimingHandler::LLHls);
	auto pre_snap = mgr->GetServerSnapshot();
	HttpTimingHistogramKey llhls_part_key{HttpTimingHandler::LLHls, HttpTimingKind::PartialSegment, false, "200"};
	EXPECT_NE(pre_snap.duration.find(llhls_part_key), pre_snap.duration.end());
	EXPECT_NE(pre_snap.processing.find(llhls_part_key), pre_snap.processing.end());

	// Simulate an LL-HLS held slow request (duration=600ms, wait=250ms, processing=350ms > 250ms threshold)
	HttpRequestTimingManager::AnsweredEvent ev;
	ev.handler		  = HttpTimingHandler::LLHls;
	ev.kind			  = HttpTimingKind::PartialSegment;
	ev.held			  = true;
	ev.status_code	  = "200";
	ev.duration_us	  = 600000;
	ev.wait_us		  = 250000;
	ev.processing_us  = 350000;
	ev.stream_id	  = 77;
	ev.session_id	  = 999;
	ev.request_id	  = "Ab3xK9q";
	ev.stream_label	  = "default/app/stream";
	ev.client_addr	  = "127.0.0.1:51234/tcp";
	ev.http_version	  = "2";
	ev.method		  = "GET";
	ev.path			  = "/app/stream/part.m4s?_HLS_msn=10&_HLS_part=2";
	ev.arrival_system = std::chrono::system_clock::now();
	mgr->RecordAnswered(ev);

	// Simulate an unanswered dropped request
	HttpRequestTimingManager::UnansweredEvent uev;
	uev.handler		   = HttpTimingHandler::LLHls;
	uev.kind		   = HttpTimingKind::PartialSegment;
	uev.held		   = true;
	uev.reason		   = HttpUnansweredReason::Dropped;
	uev.waited_us	   = 120000;
	uev.request_id	   = "Drop001";
	uev.stream_label   = "default/app/stream";
	uev.session_id	   = 999;
	uev.client_addr	   = "127.0.0.1:51234/tcp";
	uev.http_version   = "1.1";
	uev.method		   = "GET";
	uev.path		   = "/app/stream/part.m4s";
	uev.arrival_system = std::chrono::system_clock::now();
	mgr->RecordUnanswered(uev);

	// In-progress tracking
	mgr->IncrementInProgress(HttpTimingHandler::Api);

	auto snap = mgr->GetServerSnapshot();
	HttpTimingHistogramKey held_key{HttpTimingHandler::LLHls, HttpTimingKind::PartialSegment, true, "200"};
	ASSERT_NE(snap.duration.find(held_key), snap.duration.end());
	EXPECT_EQ(snap.duration.at(held_key).total_count, 1u);
	EXPECT_NEAR(snap.duration.at(held_key).sum_seconds, 0.6, 1e-6);

	ASSERT_NE(snap.processing.find(held_key), snap.processing.end());
	EXPECT_EQ(snap.processing.at(held_key).total_count, 1u);
	EXPECT_NEAR(snap.processing.at(held_key).sum_seconds, 0.35, 1e-6);

	HttpTimingUnansweredKey ukey{HttpTimingHandler::LLHls, HttpTimingKind::PartialSegment, true, HttpUnansweredReason::Dropped};
	EXPECT_EQ(snap.unanswered.at(ukey), 1u);
	EXPECT_EQ(snap.in_progress.at(HttpTimingHandler::Api), 1);
	mgr->DecrementInProgress(HttpTimingHandler::Api);

	// Per-stream & per-session summaries
	auto stream_snap = mgr->GetStreamSnapshot(77);
	HttpTimingSummaryKey skey{HttpTimingKind::PartialSegment, true, "200"};
	ASSERT_NE(stream_snap.duration.find(skey), stream_snap.duration.end());
	EXPECT_EQ(stream_snap.duration.at(skey).count, 1u);
	EXPECT_NEAR(stream_snap.duration.at(skey).sum_seconds, 0.6, 1e-6);
	EXPECT_NEAR(stream_snap.processing.at(skey).sum_seconds, 0.35, 1e-6);

	auto sess_snap = mgr->GetSessionSnapshot(999);
	ASSERT_NE(sess_snap.duration.find(skey), sess_snap.duration.end());
	EXPECT_EQ(sess_snap.duration.at(skey).count, 1u);

	// Collect both 1s debug and 10s slow/dropped warn lines
	auto t0	  = std::chrono::steady_clock::now();
	auto logs = mgr->CollectLogLines(1793723164, t0, true);
	ASSERT_EQ(logs.debug_lines.size(), 1u);
	EXPECT_NE(logs.debug_lines[0].find("llhls/partial_segment held=true sec=1793723164: n=1 max=350.0ms (id=Ab3xK9q status=200 stream=default/app/stream session=999)"), std::string::npos);

	ASSERT_EQ(logs.warn_lines.size(), 1u);
	EXPECT_NE(logs.warn_lines[0].find("handler=llhls kind=partial_segment held=true slow=1 dropped=1 max: id=Ab3xK9q status=200 total=600.0ms wait=250.0ms processing=350.0ms"), std::string::npos);

	mgr->OnStreamDeleted(77);
	mgr->OnSessionDeleted(999);
	EXPECT_TRUE(mgr->GetStreamSnapshot(77).duration.empty());
	EXPECT_TRUE(mgr->GetSessionSnapshot(999).duration.empty());
	mgr->Stop();
}
