//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "opus_ts_splitter.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <vector>

#include "opus_parser.h"

namespace
{
	constexpr int32_t MPEGTS_TIMESCALE = 90000;

	// TOC bytes: config << 3 | stereo << 2 | frame count code
	constexpr uint8_t CELT_FB_20MS_STEREO = (31 << 3) | (1 << 2) | 0;  // one 20 ms frame
	constexpr uint8_t CELT_FB_10MS_MONO	  = (30 << 3) | (0 << 2) | 0;  // one 10 ms frame
	constexpr uint8_t SILK_NB_60MS_TWO	  = (3 << 3) | (0 << 2) | 1;   // two 60 ms frames

	// An Opus packet of `size` bytes that starts with `toc`
	std::vector<uint8_t> MakeOpusPacket(uint8_t toc, size_t size)
	{
		std::vector<uint8_t> packet(size, 0x55);
		packet[0] = toc;
		return packet;
	}

	// One access unit: control header, then the Opus packet
	std::vector<uint8_t> MakeAccessUnit(const std::vector<uint8_t> &packet,
										std::optional<uint16_t> start_trim = std::nullopt,
										std::optional<uint16_t> end_trim   = std::nullopt,
										const std::vector<uint8_t> *extension = nullptr)
	{
		std::vector<uint8_t> au = {0x7F, 0xE0};

		if (start_trim.has_value())
		{
			au[1] |= 0x10;
		}
		if (end_trim.has_value())
		{
			au[1] |= 0x08;
		}
		if (extension != nullptr)
		{
			au[1] |= 0x04;
		}

		// As FFmpeg writes it: a run of 0xFF, then the remainder (0 when the size is a multiple of 255)
		int remaining = static_cast<int>(packet.size());
		do
		{
			au.push_back(static_cast<uint8_t>(std::min(remaining, 255)));
			remaining -= 255;
		} while (remaining >= 0);

		if (start_trim.has_value())
		{
			// The 3 reserved bits are set, as a careless writer might; they must be ignored
			au.push_back(static_cast<uint8_t>(0xE0 | (*start_trim >> 8)));
			au.push_back(static_cast<uint8_t>(*start_trim & 0xFF));
		}
		if (end_trim.has_value())
		{
			au.push_back(static_cast<uint8_t>(*end_trim >> 8));
			au.push_back(static_cast<uint8_t>(*end_trim & 0xFF));
		}
		if (extension != nullptr)
		{
			au.push_back(static_cast<uint8_t>(extension->size()));
			au.insert(au.end(), extension->begin(), extension->end());
		}

		au.insert(au.end(), packet.begin(), packet.end());
		return au;
	}

	std::vector<uint8_t> Concat(const std::vector<std::vector<uint8_t>> &parts)
	{
		std::vector<uint8_t> out;
		for (const auto &part : parts)
		{
			out.insert(out.end(), part.begin(), part.end());
		}
		return out;
	}
}  // namespace

// -----------------------------------------------------------------------------
// OPUSParser::GetPacketSamples (RFC 6716 section 3.1)
// -----------------------------------------------------------------------------

TEST(OpusPacketSamples, FrameSizeFollowsTheConfiguration)
{
	const struct
	{
		uint8_t config;
		int samples;
	} cases[] = {
		{0, 480}, {1, 960}, {2, 1920}, {3, 2880},	   // SILK-only NB: 10, 20, 40, 60 ms
		{11, 2880},									   // SILK-only WB 60 ms
		{12, 480}, {13, 960}, {15, 960},			   // Hybrid: 10, 20 ms
		{16, 120}, {17, 240}, {18, 480}, {19, 960},	   // CELT-only NB: 2.5, 5, 10, 20 ms
		{31, 960},									   // CELT-only FB 20 ms
	};

	for (const auto &c : cases)
	{
		const uint8_t toc = static_cast<uint8_t>(c.config << 3);
		EXPECT_EQ(OPUSParser::GetPacketSamples(&toc, 1), c.samples) << "config " << static_cast<int>(c.config);
	}
}

TEST(OpusPacketSamples, FrameCountCodeMultipliesTheFrameSize)
{
	const uint8_t one[]	  = {(31 << 3) | 0};
	const uint8_t two_a[] = {(31 << 3) | 1};
	const uint8_t two_b[] = {(31 << 3) | 2};
	const uint8_t five[]  = {(19 << 3) | 3, 0x05};	// code 3, five 20 ms frames

	EXPECT_EQ(OPUSParser::GetPacketSamples(one, sizeof(one)), 960);
	EXPECT_EQ(OPUSParser::GetPacketSamples(two_a, sizeof(two_a)), 1920);
	EXPECT_EQ(OPUSParser::GetPacketSamples(two_b, sizeof(two_b)), 1920);
	EXPECT_EQ(OPUSParser::GetPacketSamples(five, sizeof(five)), 4800);
}

TEST(OpusPacketSamples, MalformedPacketsAreRejected)
{
	const uint8_t code3_without_count[] = {(31 << 3) | 3};
	const uint8_t code3_zero_frames[]	= {(31 << 3) | 3, 0x00};
	const uint8_t over_120ms[]			= {(3 << 3) | 3, 0x03};	 // three 60 ms frames

	EXPECT_EQ(OPUSParser::GetPacketSamples(nullptr, 0), -1);
	EXPECT_EQ(OPUSParser::GetPacketSamples(code3_without_count, sizeof(code3_without_count)), -1);
	EXPECT_EQ(OPUSParser::GetPacketSamples(code3_zero_frames, sizeof(code3_zero_frames)), -1);
	EXPECT_EQ(OPUSParser::GetPacketSamples(over_120ms, sizeof(over_120ms)), -1);
}

// -----------------------------------------------------------------------------
// OpusTsSplitter
// -----------------------------------------------------------------------------

TEST(OpusTsSplitter, SingleAccessUnit)
{
	const auto packet  = MakeOpusPacket(CELT_FB_20MS_STEREO, 40);
	const auto payload = MakeAccessUnit(packet);

	const auto result  = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	EXPECT_TRUE(result.stop_reason.IsEmpty()) << result.stop_reason.CStr();
	ASSERT_EQ(result.frames.size(), 1u);
	EXPECT_EQ(result.frames[0].offset, 3u);	 // 2 header bytes + 1 size byte
	EXPECT_EQ(result.frames[0].length, packet.size());
	EXPECT_EQ(result.frames[0].timestamp_offset, 0);
	EXPECT_EQ(result.frames[0].samples, 960);
	EXPECT_EQ(0, memcmp(payload.data() + result.frames[0].offset, packet.data(), packet.size()));
}

// FFmpeg packs several access units into one audio PES; each is timed by the packets before it.
TEST(OpusTsSplitter, SeveralAccessUnitsAreTimedByTheirDurations)
{
	const auto p0	   = MakeOpusPacket(CELT_FB_20MS_STEREO, 30);
	const auto p1	   = MakeOpusPacket(CELT_FB_10MS_MONO, 20);
	const auto p2	   = MakeOpusPacket(SILK_NB_60MS_TWO, 25);
	const auto p3	   = MakeOpusPacket(CELT_FB_20MS_STEREO, 35);
	const auto payload = Concat({MakeAccessUnit(p0), MakeAccessUnit(p1), MakeAccessUnit(p2), MakeAccessUnit(p3)});

	const auto result  = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	EXPECT_TRUE(result.stop_reason.IsEmpty()) << result.stop_reason.CStr();
	ASSERT_EQ(result.frames.size(), 4u);

	// 20 ms, then 10 ms, then 120 ms, in 90 kHz ticks
	EXPECT_EQ(result.frames[0].timestamp_offset, 0);
	EXPECT_EQ(result.frames[1].timestamp_offset, 1800);
	EXPECT_EQ(result.frames[2].timestamp_offset, 2700);
	EXPECT_EQ(result.frames[3].timestamp_offset, 13500);

	EXPECT_EQ(result.frames[2].samples, 5760);
	EXPECT_EQ(result.frames[3].length, p3.size());
	EXPECT_EQ(0, memcmp(payload.data() + result.frames[3].offset, p3.data(), p3.size()));
}

TEST(OpusTsSplitter, TimestampOffsetsFollowTheTimescale)
{
	const auto payload = Concat({MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 10)),
								 MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 10))});

	const auto at_48k  = OpusTsSplitter::Split(payload.data(), payload.size(), 48000);
	const auto at_1k   = OpusTsSplitter::Split(payload.data(), payload.size(), 1000);

	ASSERT_EQ(at_48k.frames.size(), 2u);
	ASSERT_EQ(at_1k.frames.size(), 2u);
	EXPECT_EQ(at_48k.frames[1].timestamp_offset, 960);
	EXPECT_EQ(at_1k.frames[1].timestamp_offset, 20);
}

// au_size continues over 0xFF bytes: 255 is written as 0xFF 0x00, and 300 as 0xFF 0x2D.
TEST(OpusTsSplitter, AccessUnitSizesOf255AndMore)
{
	const auto p255	   = MakeOpusPacket(CELT_FB_20MS_STEREO, 255);
	const auto p300	   = MakeOpusPacket(CELT_FB_20MS_STEREO, 300);
	const auto au255   = MakeAccessUnit(p255);
	const auto payload = Concat({au255, MakeAccessUnit(p300)});

	ASSERT_EQ(au255[2], 0xFF);
	ASSERT_EQ(au255[3], 0x00);

	const auto result = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	EXPECT_TRUE(result.stop_reason.IsEmpty()) << result.stop_reason.CStr();
	ASSERT_EQ(result.frames.size(), 2u);
	EXPECT_EQ(result.frames[0].offset, 4u);
	EXPECT_EQ(result.frames[0].length, 255u);
	EXPECT_EQ(result.frames[1].length, 300u);
	EXPECT_EQ(result.frames[1].offset, au255.size() + 4);
}

// Trim values are 13 bits behind 3 reserved bits, and a control extension is skipped.
TEST(OpusTsSplitter, TrimsAndControlExtension)
{
	const std::vector<uint8_t> extension = {0x01, 0x02, 0x03};
	const auto packet					 = MakeOpusPacket(CELT_FB_20MS_STEREO, 50);
	const auto payload					 = Concat({MakeAccessUnit(packet, 312, 100, &extension),
													   MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 50))});

	const auto result					 = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	EXPECT_TRUE(result.stop_reason.IsEmpty()) << result.stop_reason.CStr();
	ASSERT_EQ(result.frames.size(), 2u);
	EXPECT_EQ(result.frames[0].start_trim, 312);
	EXPECT_EQ(result.frames[0].end_trim, 100);
	// 2 header + 1 size + 2 start trim + 2 end trim + 1 extension length + 3 extension bytes
	EXPECT_EQ(result.frames[0].offset, 11u);
	EXPECT_EQ(result.frames[0].length, packet.size());
	EXPECT_EQ(result.frames[1].start_trim, 0);
	EXPECT_EQ(result.frames[1].end_trim, 0);
	EXPECT_EQ(result.frames[1].timestamp_offset, 1800);
}

// A payload without the control header is left to the caller, who forwards it as one packet.
TEST(OpusTsSplitter, PayloadWithoutControlHeaderIsNotSplit)
{
	const auto packet = MakeOpusPacket(CELT_FB_20MS_STEREO, 40);

	const auto result = OpusTsSplitter::Split(packet.data(), packet.size(), MPEGTS_TIMESCALE);

	EXPECT_TRUE(result.frames.empty());
	EXPECT_FALSE(result.stop_reason.IsEmpty());
	EXPECT_FALSE(OpusTsSplitter::HasControlHeader(packet.data(), packet.size()));
}

// A truncated last access unit keeps the complete ones before it.
TEST(OpusTsSplitter, TruncatedAccessUnitKeepsThePreviousOnes)
{
	auto payload = Concat({MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 40)),
						   MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 40))});
	payload.resize(payload.size() - 10);

	const auto result = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	ASSERT_EQ(result.frames.size(), 1u);
	EXPECT_FALSE(result.stop_reason.IsEmpty());
}

TEST(OpusTsSplitter, GarbageAfterAnAccessUnitStopsSplitting)
{
	auto payload = MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 40));
	payload.insert(payload.end(), {0x12, 0x34, 0x56});

	const auto result = OpusTsSplitter::Split(payload.data(), payload.size(), MPEGTS_TIMESCALE);

	ASSERT_EQ(result.frames.size(), 1u);
	EXPECT_FALSE(result.stop_reason.IsEmpty());
}

TEST(OpusTsSplitter, MalformedAccessUnitsStopSplitting)
{
	// The size bytes never end
	const std::vector<uint8_t> unterminated_size = {0x7F, 0xE0, 0xFF, 0xFF};
	// A zero-length Opus packet
	const std::vector<uint8_t> empty_packet = {0x7F, 0xE0, 0x00};
	// The trim fields are missing
	const std::vector<uint8_t> missing_trim = {0x7F, 0xF0, 0x01};
	// A code 3 packet without its frame count byte cannot be timed
	const auto untimed = MakeAccessUnit({static_cast<uint8_t>((31 << 3) | 3)});

	for (const auto *payload : {&unterminated_size, &empty_packet, &missing_trim, &untimed})
	{
		const auto result = OpusTsSplitter::Split(payload->data(), payload->size(), MPEGTS_TIMESCALE);
		EXPECT_TRUE(result.frames.empty());
		EXPECT_FALSE(result.stop_reason.IsEmpty());
	}
}

TEST(OpusTsSplitter, InvalidTimescaleIsNotSplit)
{
	const auto payload = MakeAccessUnit(MakeOpusPacket(CELT_FB_20MS_STEREO, 40));

	const auto result  = OpusTsSplitter::Split(payload.data(), payload.size(), 0);

	EXPECT_TRUE(result.frames.empty());
	EXPECT_FALSE(result.stop_reason.IsEmpty());
}
