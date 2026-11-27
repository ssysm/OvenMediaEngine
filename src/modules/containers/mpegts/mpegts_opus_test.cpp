//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "descriptors/opus_descriptor.h"
#include "mpegts_depacketizer.h"
#include "mpegts_packet.h"
#include "mpegts_section.h"

// Opus in MPEG-TS, as FFmpeg muxes it: stream_type 0x06 (PES private data), a registration descriptor
// "Opus", a DVB extension descriptor 0x80 with the channel_config_code, and PES of stream_id 0xBD
// (private_stream_1) whose payload is Opus packets behind control headers.
namespace
{
	constexpr uint16_t PMT_PID	= 0x1000;
	constexpr uint16_t OPUS_PID = 0x0101;

	std::shared_ptr<mpegts::Descriptor> MakeDescriptor(uint8_t tag, const std::vector<uint8_t> &bytes)
	{
		return std::make_shared<mpegts::Descriptor>(tag, std::make_shared<ov::Data>(bytes.data(), bytes.size()));
	}

	std::shared_ptr<mpegts::Descriptor> MakeRegistrationDescriptor(const char *format_identifier)
	{
		return MakeDescriptor(0x05, {static_cast<uint8_t>(format_identifier[0]), static_cast<uint8_t>(format_identifier[1]),
									 static_cast<uint8_t>(format_identifier[2]), static_cast<uint8_t>(format_identifier[3])});
	}

	std::shared_ptr<mpegts::Descriptor> MakeOpusExtensionDescriptor(uint8_t channel_config_code)
	{
		return MakeDescriptor(0x7F, {0x80, channel_config_code});
	}

	std::shared_ptr<const ov::Data> MakePatPacket()
	{
		mpegts::PAT pat;
		pat._table_id_extension		= 1;
		pat._version_number			= 0;
		pat._current_next_indicator = true;
		pat._section_number			= 0;
		pat._last_section_number	= 0;
		pat._program_num			= 1;
		pat._program_map_pid		= PMT_PID;

		return mpegts::Packet::Build(mpegts::Section::Build(pat), 0)->GetData();
	}

	// A PMT for program 1 with one PES private data stream on OPUS_PID that has `descriptors`
	std::shared_ptr<const ov::Data> MakePmtPacket(const std::vector<std::shared_ptr<mpegts::Descriptor>> &descriptors)
	{
		mpegts::PMT pmt;
		pmt._table_id_extension		= 1;
		pmt._version_number			= 0;
		pmt._current_next_indicator = true;
		pmt._section_number			= 0;
		pmt._last_section_number	= 0;
		pmt._pcr_pid				= OPUS_PID;
		pmt._program_info_length	= 0;

		auto es						= std::make_shared<mpegts::ESInfo>();
		es->_stream_type			= static_cast<uint8_t>(mpegts::WellKnownStreamTypes::PES_PRIVATE_DATA);
		es->_elementary_pid			= OPUS_PID;
		es->_es_descriptors			= descriptors;
		es->_es_info_length			= 0;
		for (const auto &descriptor : descriptors)
		{
			es->_es_info_length += descriptor->GetPacketLength();
		}
		pmt._es_info_list.push_back(es);

		return mpegts::Packet::Build(mpegts::Section::Build(PMT_PID, pmt), 0)->GetData();
	}

	// A bounded PES of stream_id 0xBD with a PTS, in one TS packet padded by adaptation-field stuffing
	std::shared_ptr<const ov::Data> MakePrivateStreamPesPacket(uint16_t pid, uint8_t cc, int64_t pts, const std::vector<uint8_t> &payload)
	{
		std::vector<uint8_t> pes = {
			0x00, 0x00, 0x01, 0xBD,
			// PES_packet_length: the optional header (3), the PTS (5) and the payload
			static_cast<uint8_t>((8 + payload.size()) >> 8), static_cast<uint8_t>((8 + payload.size()) & 0xFF),
			// '10', no scrambling, data_alignment_indicator; PTS only; PES_header_data_length 5
			0x84, 0x80, 0x05,
			static_cast<uint8_t>(0x21 | ((pts >> 29) & 0x0E)),
			static_cast<uint8_t>((pts >> 22) & 0xFF),
			static_cast<uint8_t>(((pts >> 14) & 0xFE) | 0x01),
			static_cast<uint8_t>((pts >> 7) & 0xFF),
			static_cast<uint8_t>(((pts << 1) & 0xFE) | 0x01)};
		pes.insert(pes.end(), payload.begin(), payload.end());

		std::vector<uint8_t> packet(188, 0xFF);
		const size_t adaptation_size = 188 - 4 - pes.size();

		packet[0]					 = 0x47;
		packet[1]					 = static_cast<uint8_t>(0x40 | ((pid >> 8) & 0x1F));
		packet[2]					 = static_cast<uint8_t>(pid & 0xFF);
		packet[3]					 = static_cast<uint8_t>(0x30 | (cc & 0x0F));
		packet[4]					 = static_cast<uint8_t>(adaptation_size - 1);
		packet[5]					 = 0x00;

		std::copy(pes.begin(), pes.end(), packet.begin() + 4 + adaptation_size);

		return std::make_shared<ov::Data>(packet.data(), packet.size());
	}

	// Two Opus access units: a 20 ms stereo CELT packet of 10 bytes, then one of 6 bytes
	const std::vector<uint8_t> OPUS_PAYLOAD = {
		0x7F, 0xE0, 0x0A, 0xFC, 1, 2, 3, 4, 5, 6, 7, 8, 9,
		0x7F, 0xE0, 0x06, 0xFC, 1, 2, 3, 4, 5};

	std::shared_ptr<MediaTrack> GetOpusTrack(mpegts::MpegTsDepacketizer &depacketizer)
	{
		std::map<uint16_t, std::shared_ptr<MediaTrack>> tracks;
		depacketizer.GetTrackList(&tracks);
		auto it = tracks.find(OPUS_PID);
		return (it != tracks.end()) ? it->second : nullptr;
	}
}  // namespace

TEST(MpegTsOpus, OpusStreamBecomesAnOpusTrack)
{
	mpegts::MpegTsDepacketizer depacketizer;
	depacketizer.AddPacket(MakePatPacket());
	depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("Opus"), MakeOpusExtensionDescriptor(0x02)}));

	ASSERT_TRUE(depacketizer.IsTrackInfoAvailable());
	auto track = GetOpusTrack(depacketizer);
	ASSERT_NE(track, nullptr);
	EXPECT_EQ(track->GetMediaType(), cmn::MediaType::Audio);
	EXPECT_EQ(track->GetCodecId(), cmn::MediaCodecId::Opus);
	EXPECT_EQ(track->GetOriginBitstream(), cmn::BitstreamFormat::OPUS);
	EXPECT_EQ(track->GetSampleRate(), 48000);
	EXPECT_EQ(track->GetChannel().GetCounts(), 2u);
	EXPECT_EQ(track->GetTimeBase().GetNum(), 1);
	EXPECT_EQ(track->GetTimeBase().GetDen(), 90000);
}

TEST(MpegTsOpus, MonoChannelConfiguration)
{
	mpegts::MpegTsDepacketizer depacketizer;
	depacketizer.AddPacket(MakePatPacket());
	depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("Opus"), MakeOpusExtensionDescriptor(0x01)}));

	auto track = GetOpusTrack(depacketizer);
	ASSERT_NE(track, nullptr);
	EXPECT_EQ(track->GetChannel().GetCounts(), 1u);
}

// Without the extension descriptor the track is still created; the channel count comes from the TOC later.
TEST(MpegTsOpus, MissingExtensionDescriptorLeavesTheChannelsUnset)
{
	mpegts::MpegTsDepacketizer depacketizer;
	depacketizer.AddPacket(MakePatPacket());
	depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("Opus")}));

	auto track = GetOpusTrack(depacketizer);
	ASSERT_NE(track, nullptr);
	EXPECT_EQ(track->GetCodecId(), cmn::MediaCodecId::Opus);
	EXPECT_EQ(track->GetChannel().GetLayout(), cmn::AudioChannel::Layout::LayoutUnknown);
}

// Dual mono and multichannel configurations need a multistream decoder setup, so no track is made.
TEST(MpegTsOpus, UnsupportedChannelConfigurationsMakeNoTrack)
{
	for (uint8_t code : {0x00, 0x03, 0x06, 0x08, 0x82, 0xFF})
	{
		mpegts::MpegTsDepacketizer depacketizer;
		depacketizer.AddPacket(MakePatPacket());
		depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("Opus"), MakeOpusExtensionDescriptor(code)}));

		EXPECT_TRUE(depacketizer.IsTrackInfoAvailable());
		EXPECT_EQ(GetOpusTrack(depacketizer), nullptr) << "channel_config_code 0x" << std::hex << static_cast<int>(code);
	}
}

// Other private data stays unsupported, and its PES is not assembled.
TEST(MpegTsOpus, OtherPrivateDataMakesNoTrack)
{
	mpegts::MpegTsDepacketizer depacketizer;
	depacketizer.AddPacket(MakePatPacket());
	depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("KLVA")}));
	depacketizer.AddPacket(MakePrivateStreamPesPacket(OPUS_PID, 0, 90000, OPUS_PAYLOAD));

	EXPECT_TRUE(depacketizer.IsTrackInfoAvailable());
	EXPECT_EQ(GetOpusTrack(depacketizer), nullptr);
	EXPECT_FALSE(depacketizer.IsESAvailable());
}

// A private_stream_1 PES has the optional PES header: the PTS is read and the payload starts at the
// first control header.
TEST(MpegTsOpus, PrivateStreamPesHasItsPtsAndPayload)
{
	constexpr int64_t PTS = 0x123456789LL & 0x1FFFFFFFFLL;

	mpegts::MpegTsDepacketizer depacketizer;
	depacketizer.AddPacket(MakePatPacket());
	depacketizer.AddPacket(MakePmtPacket({MakeRegistrationDescriptor("Opus"), MakeOpusExtensionDescriptor(0x02)}));
	depacketizer.AddPacket(MakePrivateStreamPesPacket(OPUS_PID, 0, PTS, OPUS_PAYLOAD));

	auto pes = depacketizer.PopES();
	ASSERT_NE(pes, nullptr);
	EXPECT_EQ(pes->PID(), OPUS_PID);
	EXPECT_EQ(pes->StreamId(), 0xBD);
	EXPECT_EQ(pes->Pts(), PTS);
	ASSERT_EQ(pes->PayloadLength(), OPUS_PAYLOAD.size());
	EXPECT_EQ(0, memcmp(pes->Payload(), OPUS_PAYLOAD.data(), OPUS_PAYLOAD.size()));
}

TEST(MpegTsOpus, DescriptorHelpers)
{
	using mpegts::OpusDescriptor;

	EXPECT_TRUE(OpusDescriptor::IsOpus({MakeRegistrationDescriptor("Opus")}));
	EXPECT_FALSE(OpusDescriptor::IsOpus({MakeRegistrationDescriptor("opus")}));
	EXPECT_FALSE(OpusDescriptor::IsOpus({MakeDescriptor(0x05, {'O', 'p', 'u'})}));
	EXPECT_FALSE(OpusDescriptor::IsOpus({MakeDescriptor(0x0A, {'O', 'p', 'u', 's'})}));

	EXPECT_EQ(OpusDescriptor::GetChannelConfigCode({MakeOpusExtensionDescriptor(0x02)}), std::optional<uint8_t>(0x02));
	EXPECT_EQ(OpusDescriptor::GetChannelConfigCode({MakeDescriptor(0x7F, {0x81, 0x02})}), std::nullopt);
	EXPECT_EQ(OpusDescriptor::GetChannelConfigCode({MakeDescriptor(0x7F, {0x80})}), std::nullopt);

	EXPECT_EQ(OpusDescriptor::GetSupportedChannelCount(0x01), 1u);
	EXPECT_EQ(OpusDescriptor::GetSupportedChannelCount(0x02), 2u);
	EXPECT_EQ(OpusDescriptor::GetSupportedChannelCount(0x00), 0u);
	EXPECT_EQ(OpusDescriptor::GetSupportedChannelCount(0x06), 0u);
}
