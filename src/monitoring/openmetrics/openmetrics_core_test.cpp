//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>
#include <zlib.h>

#include <atomic>
#include <thread>

#include "openmetrics_cache.h"
#include "openmetrics_collectors.h"
#include "openmetrics_formatter.h"

namespace
{
	std::string DecompressGzip(const std::shared_ptr<ov::Data> &compressed)
	{
		z_stream zs{};
		if (inflateInit2(&zs, 15 + 16) != Z_OK)
		{
			return {};
		}

		zs.next_in	= const_cast<Bytef *>(compressed->GetDataAs<Bytef>());
		zs.avail_in = static_cast<uInt>(compressed->GetLength());

		std::string out;
		char buf[4096];
		int ret = Z_OK;
		do
		{
			zs.next_out	 = reinterpret_cast<Bytef *>(buf);
			zs.avail_out = sizeof(buf);
			ret			 = inflate(&zs, 0);
			if (out.size() < zs.total_out)
			{
				out.append(buf, zs.total_out - out.size());
			}
		} while (ret == Z_OK);

		inflateEnd(&zs);
		return (ret == Z_STREAM_END) ? out : std::string{};
	}
}  // namespace

TEST(OpenMetricsCoreTest, FormatterConformsToOpenMetrics100)
{
	using namespace mon::openmetrics;
	OpenMetricsBuilder b;

	b.DeclareFamily("ome_build", MetricType::Info, "", "OvenMediaEngine build information.");
	b.AddInfo("ome_build", {{"version", "0.21.0"}, {"git_version", "abc\"\\def\n"}});

	b.DeclareFamily("process_start_time_seconds", MetricType::Gauge, "seconds", "Start time of the process since unix epoch in seconds.");
	b.AddGauge("process_start_time_seconds", {}, 1783445283.19);

	b.DeclareFamily("ome_receive_bytes", MetricType::Counter, "bytes", "Media bytes received.");
	b.AddCounter("ome_receive_bytes", {{"vhost", "default"}, {"output_stream", ""}}, 123456);

	b.DeclareFamily("ome_push_state", MetricType::Stateset, "", "Push lifecycle state.");
	b.AddStateset("ome_push_state", {{"id", "p1"}}, {"ready", "pushing", "error"}, "pushing");

	b.DeclareFamily("ome_track_egress_latency_seconds", MetricType::Histogram, "seconds", "Egress latency.");
	b.AddHistogram("ome_track_egress_latency_seconds",
				   {{"track", "0"}},
				   {0.05, 0.2, 1.0},
				   {2, 5, 6},
				   7,
				   1.25);

	b.DeclareFamily("ome_session_egress_latency_seconds", MetricType::Summary, "seconds", "Session egress.");
	b.AddSummary("ome_session_egress_latency_seconds", {{"session", "10"}}, 4, 0.4);

	// Empty family must be omitted completely
	b.DeclareFamily("ome_unused_family", MetricType::Gauge, "", "Should not appear.");

	std::string out = b.Finish();

	EXPECT_NE(out.find("# TYPE ome_build info\n# HELP ome_build OvenMediaEngine build information.\nome_build_info{version=\"0.21.0\",git_version=\"abc\\\"\\\\def\\n\"} 1\n"), std::string::npos);
	EXPECT_NE(out.find("# TYPE process_start_time_seconds gauge\n# UNIT process_start_time_seconds seconds\n# HELP process_start_time_seconds Start time of the process since unix epoch in seconds.\n"), std::string::npos);
	EXPECT_NE(out.find("# TYPE ome_receive_bytes counter\n# UNIT ome_receive_bytes bytes\n# HELP ome_receive_bytes Media bytes received.\nome_receive_bytes_total{vhost=\"default\",output_stream=\"\"} 123456\n"), std::string::npos);
	EXPECT_NE(out.find("ome_push_state{id=\"p1\",ome_push_state=\"ready\"} 0\n"), std::string::npos);
	EXPECT_NE(out.find("ome_push_state{id=\"p1\",ome_push_state=\"pushing\"} 1\n"), std::string::npos);
	EXPECT_NE(out.find("ome_push_state{id=\"p1\",ome_push_state=\"error\"} 0\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_bucket{track=\"0\",le=\"0.05\"} 2\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_bucket{track=\"0\",le=\"0.2\"} 5\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_bucket{track=\"0\",le=\"1.0\"} 6\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_bucket{track=\"0\",le=\"+Inf\"} 7\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_count{track=\"0\"} 7\n"), std::string::npos);
	EXPECT_NE(out.find("ome_track_egress_latency_seconds_sum{track=\"0\"} 1.25\n"), std::string::npos);
	EXPECT_NE(out.find("ome_session_egress_latency_seconds_count{session=\"10\"} 4\n"), std::string::npos);
	EXPECT_NE(out.find("ome_session_egress_latency_seconds_sum{session=\"10\"} 0.4\n"), std::string::npos);
	EXPECT_EQ(out.find("ome_unused_family"), std::string::npos);
	ASSERT_GE(out.size(), 6u);
	EXPECT_EQ(out.substr(out.size() - 6), "# EOF\n");
}

TEST(OpenMetricsCoreTest, CollectorSelectionRules)
{
	using namespace mon::openmetrics;
	uint32_t mask = 0;
	ov::String err;

	// Default non-sessions path: all defaults, no session / session_latency
	ASSERT_TRUE(ParseCollectorSelection("", false, &mask, &err));
	EXPECT_EQ(mask, kDefaultCollectors);
	EXPECT_EQ(mask & kCollectorSession, 0u);
	EXPECT_EQ(mask & kCollectorSessionLatency, 0u);

	// Default sessions path: defaults + session + session_latency
	ASSERT_TRUE(ParseCollectorSelection("", true, &mask, &err));
	EXPECT_EQ(mask, kDefaultCollectors | kCollectorSession | kCollectorSessionLatency);

	// Explicit collect[] (raw and percent-encoded)
	ASSERT_TRUE(ParseCollectorSelection("collect[]=traffic&collect%5B%5D=stream", false, &mask, &err));
	EXPECT_EQ(mask, kCollectorCore | kCollectorTraffic | kCollectorStream);

	// Explicit collect[] on sessions path still includes session_latency
	ASSERT_TRUE(ParseCollectorSelection("collect[]=session", true, &mask, &err));
	EXPECT_EQ(mask, kCollectorCore | kCollectorSession | kCollectorSessionLatency);

	// exclude[] on sessions path can exclude session_latency, cannot exclude core
	ASSERT_TRUE(ParseCollectorSelection("exclude[]=session_latency&exclude[]=core&exclude[]=queue", true, &mask, &err));
	EXPECT_NE(mask & kCollectorCore, 0u);
	EXPECT_NE(mask & kCollectorSession, 0u);
	EXPECT_EQ(mask & kCollectorSessionLatency, 0u);
	EXPECT_EQ(mask & kCollectorQueue, 0u);

	// Mutual exclusion rejected
	EXPECT_FALSE(ParseCollectorSelection("collect[]=traffic&exclude[]=queue", false, &mask, &err));
	EXPECT_FALSE(err.IsEmpty());

	// Unknown collector rejected with list of valid names
	EXPECT_FALSE(ParseCollectorSelection("collect[]=bogus", false, &mask, &err));
	EXPECT_GE(err.IndexOf("traffic"), 0);
	EXPECT_GE(err.IndexOf("session_latency"), 0);
}

TEST(OpenMetricsCoreTest, ScrapeCacheAndGzipRoundTrip)
{
	using namespace mon::openmetrics;

	ScrapeCache cache(true, 500);
	std::atomic<int> renders{0};

	std::string a = cache.GetOrRender("/v2/metrics|", [&]() {
		renders++;
		return std::string("payload-1\n# EOF\n");
	});
	std::string b = cache.GetOrRender("/v2/metrics|", [&]() {
		renders++;
		return std::string("payload-2\n# EOF\n");
	});
	EXPECT_EQ(a, "payload-1\n# EOF\n");
	EXPECT_EQ(b, "payload-1\n# EOF\n");
	EXPECT_EQ(renders.load(), 1);

	ScrapeCache disabled_cache(false, 5000);
	std::string c = disabled_cache.GetOrRender("/v2/metrics|", [&]() {
		renders++;
		return std::string("fresh-1\n# EOF\n");
	});
	std::string d = disabled_cache.GetOrRender("/v2/metrics|", [&]() {
		renders++;
		return std::string("fresh-2\n# EOF\n");
	});
	EXPECT_EQ(c, "fresh-1\n# EOF\n");
	EXPECT_EQ(d, "fresh-2\n# EOF\n");
	EXPECT_EQ(renders.load(), 3);

	std::string sample = "# TYPE ome_build info\nome_build_info{version=\"1\"} 1\n# EOF\n";
	std::shared_ptr<ov::Data> gz;
	ASSERT_TRUE(CompressGzip(sample, &gz));
	ASSERT_NE(gz, nullptr);
	ASSERT_GT(gz->GetLength(), 10u);
	EXPECT_EQ(gz->At(0), 0x1f);
	EXPECT_EQ(gz->At(1), 0x8b);
	EXPECT_EQ(DecompressGzip(gz), sample);
}
