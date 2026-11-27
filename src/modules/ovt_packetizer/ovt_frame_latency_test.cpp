//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <base/ovlibrary/byte_io.h>
#include <gtest/gtest.h>

#include <vector>

#include "ovt_depacketizer.h"
#include "ovt_packet.h"
#include "ovt_packetizer.h"
#include "ovt_wire.h"

namespace
{
	class CollectorInterface : public OvtPacketizerInterface
	{
	public:
		bool OnOvtPacketized(std::shared_ptr<OvtPacket> &packet) override
		{
			packets.push_back(packet);
			return true;
		}
		std::vector<std::shared_ptr<OvtPacket>> packets;
	};
}  // namespace

TEST(OvtFrameLatencyTest, RoundTripsPt50TimestampsAndFirstFragmentArrival)
{
	// 1. Packetizer carries frame arrival_us in memory on OvtPacket
	auto collector = std::make_shared<CollectorInterface>();
	OvtPacketizer packetizer(collector);

	std::vector<uint8_t> big_payload(100000, 0x5A);  // Multi-fragment (> 65500 bytes)
	auto src_pkt = std::make_shared<MediaPacket>(cmn::MediaType::Video, 7, big_payload.data(), static_cast<int32_t>(big_payload.size()),
												 123456, 123456, 3000, MediaPacketFlag::Key,
												 cmn::BitstreamFormat::H264_ANNEXB, cmn::PacketType::NALU);

	FrameLatencyStamps src_stamps;
	src_stamps.arrival_us = 1700000001000000LL;
	src_pkt->SetFrameLatencyStamps(src_stamps);

	ASSERT_TRUE(packetizer.PacketizeMediaPacket(12345, src_pkt));
	ASSERT_GT(collector->packets.size(), 1u);
	EXPECT_EQ(collector->packets.front()->GetFrameArrivalUs(), 1700000001000000LL);

	// 2. Build OVT2 PT 50 FrameLatency packet and prepend before media fragments
	OvtPacket pt50;
	pt50.SetSessionId(1);
	pt50.SetPayloadType(OvtPayloadType::FrameLatency);
	pt50.SetMarker(true);
	pt50.SetSequenceNumber(0);
	pt50.SetTimestampNow();

	uint8_t pl[16];
	constexpr int64_t kOriginArrival = 1700000001000000LL;
	constexpr int64_t kOriginSend	 = 1700000001012345LL;
	ByteWriter<uint64_t>::WriteBigEndian(&pl[0], static_cast<uint64_t>(kOriginArrival));
	ByteWriter<uint64_t>::WriteBigEndian(&pl[8], static_cast<uint64_t>(kOriginSend));
	pt50.SetPayload(pl, sizeof(pl));
	EXPECT_EQ(pt50.GetDataLength(), 34u);

	OvtDepacketizer depacketizer;
	ASSERT_TRUE(depacketizer.AppendPacket(pt50.GetData()));
	for (const auto &frag : collector->packets)
	{
		ASSERT_TRUE(depacketizer.AppendPacket(frag->GetData()));
	}

	ASSERT_TRUE(depacketizer.IsAvailableMediaPacket());
	auto out_pkt = depacketizer.PopMediaPacket();
	ASSERT_NE(out_pkt, nullptr);

	const auto &out_stamps = out_pkt->GetFrameLatencyStamps();
	EXPECT_EQ(out_stamps.origin_arrival_us, kOriginArrival);
	EXPECT_EQ(out_stamps.origin_send_us, kOriginSend);
	EXPECT_GT(out_stamps.arrival_us, 0);
	EXPECT_TRUE(out_stamps.from_ovt_origin);

	// 3. Subsequent media packet WITHOUT PT 50 must not inherit stale origin timestamps
	for (const auto &frag : collector->packets)
	{
		ASSERT_TRUE(depacketizer.AppendPacket(frag->GetData()));
	}
	ASSERT_TRUE(depacketizer.IsAvailableMediaPacket());
	auto out_pkt2 = depacketizer.PopMediaPacket();
	ASSERT_NE(out_pkt2, nullptr);
	const auto &out_stamps2 = out_pkt2->GetFrameLatencyStamps();
	EXPECT_EQ(out_stamps2.origin_arrival_us, 0);
	EXPECT_EQ(out_stamps2.origin_send_us, 0);
	EXPECT_GT(out_stamps2.arrival_us, 0);
	EXPECT_TRUE(out_stamps2.from_ovt_origin);
}
