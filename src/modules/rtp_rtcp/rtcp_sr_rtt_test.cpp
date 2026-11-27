//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <base/ovlibrary/byte_io.h>
#include <gtest/gtest.h>

#include <chrono>

#include "rtcp_info/rtcp_sr_generator.h"

TEST(RtcpSrRttTest, ComputesExactRttFromCompactNtpLsrAndDlsr)
{
	RtcpSRGenerator gen(0x12345678, 90000);

	auto rtp = std::make_shared<RtpPacket>();
	rtp->SetSsrc(0x12345678);
	rtp->SetTimestamp(90000 * 10);  // 10 seconds
	rtp->SetNTPTimestamp((0xE8F11234ULL << 32) | 0x56780000ULL);
	rtp->SetPayloadSize(200);
	gen.AddRTPPacketInfo(rtp);

	auto sr_pkt = gen.PopRtcpSRPacket();
	ASSERT_NE(sr_pkt, nullptr);

	auto data = sr_pkt->GetData();
	ASSERT_NE(data, nullptr);
	// RTCP SR layout: 4-byte common header, 4-byte SSRC, 4-byte NTP MSW (offset 8), 4-byte NTP LSW (offset 12)
	ASSERT_GE(data->GetLength(), 16u);

	const uint8_t *bytes = data->GetDataAs<uint8_t>();
	uint32_t msw		 = ByteReader<uint32_t>::ReadBigEndian(bytes + 8);
	uint32_t lsw		 = ByteReader<uint32_t>::ReadBigEndian(bytes + 12);
	uint32_t compact_ntp = ((msw & 0xFFFFu) << 16) | ((lsw >> 16) & 0xFFFFu);
	ASSERT_NE(compact_ntp, 0u);

	auto now = std::chrono::steady_clock::now();

	// 120ms later with 20ms DLSR (0.02 * 65536 = 1311) -> RTT ~= 0.100s
	double rtt = 0.0;
	uint32_t dlsr_20ms = static_cast<uint32_t>(0.02 * 65536.0 + 0.5);
	ASSERT_TRUE(gen.ComputeRttSeconds(compact_ntp, dlsr_20ms, now + std::chrono::milliseconds(120), &rtt));
	EXPECT_NEAR(rtt, 0.100, 0.005);

	// LSR == 0 -> false
	EXPECT_FALSE(gen.ComputeRttSeconds(0, dlsr_20ms, now + std::chrono::milliseconds(120), &rtt));

	// Unknown LSR -> false
	EXPECT_FALSE(gen.ComputeRttSeconds(compact_ntp ^ 0xFFFFu, dlsr_20ms, now + std::chrono::milliseconds(120), &rtt));

	// DLSR larger than elapsed -> negative RTT -> false
	uint32_t dlsr_500ms = static_cast<uint32_t>(0.5 * 65536.0);
	EXPECT_FALSE(gen.ComputeRttSeconds(compact_ntp, dlsr_500ms, now + std::chrono::milliseconds(50), &rtt));
}
