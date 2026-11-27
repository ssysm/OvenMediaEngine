//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include "config_manager.h"
#include "items/items.h"

namespace
{
	cfg::Server ParseServerXml(const char *xml)
	{
		auto doc = std::make_shared<pugi::xml_document>();
		auto res = doc->load_string(xml);
		EXPECT_TRUE(res) << res.description();

		cfg::DataSource ds("", "Server.xml", doc, doc->child("Server"));
		cfg::Server server;
		server.FromDataSource("Server", cfg::ItemName("Server"), ds);
		return server;
	}
}  // namespace

TEST(OpenMetricsConfigTest, ApiOpenMetricsCacheDefaultsAndOverrides)
{
	auto server_default = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind><Managers><API><Port>8081</Port></API></Managers></Bind>
			<Managers><API><AccessToken>user:pass</AccessToken></API></Managers>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");

	EXPECT_TRUE(server_default.GetManagers().GetApi().GetOpenMetrics().GetCache().IsEnabled());
	EXPECT_EQ(server_default.GetManagers().GetApi().GetOpenMetrics().GetCache().GetDurationMs(), 3000);

	auto server_custom = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind><Managers><API><Port>8081</Port></API></Managers></Bind>
			<Managers>
				<API>
					<AccessToken>user:pass</AccessToken>
					<OpenMetrics>
						<Cache>
							<Enable>false</Enable>
							<DurationMs>1500</DurationMs>
						</Cache>
					</OpenMetrics>
				</API>
			</Managers>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");

	EXPECT_FALSE(server_custom.GetManagers().GetApi().GetOpenMetrics().GetCache().IsEnabled());
	EXPECT_EQ(server_custom.GetManagers().GetApi().GetOpenMetrics().GetCache().GetDurationMs(), 1500);
}

TEST(OpenMetricsConfigTest, MeasurementsFrameLatencyAndHttpTiming)
{
	auto server = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind></Bind>
			<Modules>
				<Measurements>
					<FrameLatency>
						<Enable>true</Enable>
						<Buckets>0.001,0.01,0.2,0.5,1,3</Buckets>
					</FrameLatency>
					<HTTPRequestTiming>
						<Enable>true</Enable>
					</HTTPRequestTiming>
				</Measurements>
			</Modules>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");

	EXPECT_NO_THROW(cfg::ConfigManager::ValidateMeasurementsConfig(server));
	EXPECT_TRUE(server.GetModules().GetEffectiveFrameLatency().IsEnabled());
	EXPECT_TRUE(server.GetModules().GetMeasurements().GetHTTPRequestTiming().IsEnabled());

	std::vector<double> bounds;
	ASSERT_TRUE(cfg::modules::measurements::FrameLatency::ParseBuckets(
		server.GetModules().GetEffectiveFrameLatency().GetBuckets(), &bounds));
	ASSERT_EQ(bounds.size(), 6u);
	EXPECT_DOUBLE_EQ(bounds[0], 0.001);
	EXPECT_DOUBLE_EQ(bounds[4], 1.0);
	EXPECT_DOUBLE_EQ(bounds[5], 3.0);
}

TEST(OpenMetricsConfigTest, LegacyFrameLatencyAllowedAndConflictRejected)
{
	auto legacy_only = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind></Bind>
			<Modules>
				<FrameLatency>
					<Enable>true</Enable>
				</FrameLatency>
			</Modules>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");

	EXPECT_NO_THROW(cfg::ConfigManager::ValidateMeasurementsConfig(legacy_only));
	EXPECT_TRUE(legacy_only.GetModules().GetEffectiveFrameLatency().IsEnabled());

	auto both = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind></Bind>
			<Modules>
				<FrameLatency>
					<Enable>true</Enable>
				</FrameLatency>
				<Measurements>
					<FrameLatency>
						<Enable>true</Enable>
					</FrameLatency>
				</Measurements>
			</Modules>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");

	EXPECT_THROW(cfg::ConfigManager::ValidateMeasurementsConfig(both), cfg::ConfigError);
}

TEST(OpenMetricsConfigTest, BucketValidationRules)
{
	using FL = cfg::modules::measurements::FrameLatency;
	std::vector<double> bounds;

	// Default list valid
	EXPECT_TRUE(FL::ParseBuckets(FL::kDefaultBucketsStr, &bounds));
	EXPECT_EQ(bounds.size(), 14u);

	// Boundary values valid
	EXPECT_TRUE(FL::ParseBuckets("0.0001,3600", &bounds));
	EXPECT_EQ(FL::FormatLeBound(0.2), "0.2");
	EXPECT_EQ(FL::FormatLeBound(1.0), "1.0");
	EXPECT_EQ(FL::FormatLeBound(3.0), "3.0");
	EXPECT_EQ(FL::FormatLeBound(0.0001), "0.0001");

	// Out of range
	EXPECT_FALSE(FL::ParseBuckets("0.00005,0.1", &bounds));
	EXPECT_FALSE(FL::ParseBuckets("0.1,3600.1", &bounds));

	// More than 6 decimal places
	EXPECT_FALSE(FL::ParseBuckets("0.0001001", &bounds));

	// Not strictly increasing
	EXPECT_FALSE(FL::ParseBuckets("0.1,0.1", &bounds));
	EXPECT_FALSE(FL::ParseBuckets("0.2,0.1", &bounds));

	// More than 30 bounds
	ov::String thirty_one;
	for (int i = 1; i <= 31; ++i)
	{
		if (i > 1) thirty_one.Append(",");
		thirty_one.AppendFormat("%d", i);
	}
	EXPECT_FALSE(FL::ParseBuckets(thirty_one, &bounds));

	// Empty or bad syntax
	EXPECT_FALSE(FL::ParseBuckets("", &bounds));
	EXPECT_FALSE(FL::ParseBuckets("abc", &bounds));
	EXPECT_FALSE(FL::ParseBuckets("0.1,,0.2", &bounds));

	// Server validation throws ConfigError on invalid buckets
	auto bad_server = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind></Bind>
			<Modules>
				<Measurements>
					<FrameLatency>
						<Enable>true</Enable>
						<Buckets>0.5,0.2</Buckets>
					</FrameLatency>
				</Measurements>
			</Modules>
			<VirtualHosts></VirtualHosts>
		</Server>
	)XML");
	EXPECT_THROW(cfg::ConfigManager::ValidateMeasurementsConfig(bad_server), cfg::ConfigError);
}

TEST(OpenMetricsConfigTest, OvtPublisherFrameLatencyDefaultsTrueAndOverrides)
{
	auto server = ParseServerXml(R"XML(
		<Server version="10">
			<Name>OME</Name>
			<Type>origin</Type>
			<IP>*</IP>
			<Bind></Bind>
			<VirtualHosts>
				<VirtualHost>
					<Name>default</Name>
					<Host><Names><Name>*</Name></Names></Host>
					<Applications>
						<Application>
							<Name>app1</Name>
							<Type>live</Type>
							<Publishers>
								<OVT />
							</Publishers>
						</Application>
						<Application>
							<Name>app2</Name>
							<Type>live</Type>
							<Publishers>
								<OVT>
									<FrameLatency>
										<Enable>false</Enable>
									</FrameLatency>
								</OVT>
							</Publishers>
						</Application>
					</Applications>
				</VirtualHost>
			</VirtualHosts>
		</Server>
	)XML");

	const auto &apps = server.GetVirtualHostList()[0].GetApplicationList();
	ASSERT_EQ(apps.size(), 2u);
	EXPECT_TRUE(apps[0].GetPublishers().GetOvtPublisher().GetFrameLatency().IsEnabled());
	EXPECT_FALSE(apps[1].GetPublishers().GetOvtPublisher().GetFrameLatency().IsEnabled());
}
