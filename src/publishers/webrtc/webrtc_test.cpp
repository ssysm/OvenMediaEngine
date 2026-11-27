//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include <base/info/media_track.h>

#include "rtc_common_types.h"
#include "rtc_playlist.h"

namespace
{
	using cmn::MediaCodecId;
	using cmn::MediaType;

	std::shared_ptr<const MediaTrack> MakeTrack(MediaType type, MediaCodecId codec, int32_t bitrate)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetMediaType(type);
		track->SetCodecId(codec);
		track->SetBitrateByConfig(bitrate);
		return track;
	}

	// v == None -> no video track; a == None -> no audio track.
	std::shared_ptr<const RtcRendition> MakeRendition(const ov::String &name,
													  MediaCodecId v, int32_t v_bitrate,
													  MediaCodecId a, int32_t a_bitrate)
	{
		std::shared_ptr<const MediaTrack> vt = (v == MediaCodecId::None) ? nullptr : MakeTrack(MediaType::Video, v, v_bitrate);
		std::shared_ptr<const MediaTrack> at = (a == MediaCodecId::None) ? nullptr : MakeTrack(MediaType::Audio, a, a_bitrate);
		return std::make_shared<RtcRendition>(name, vt, at);
	}

	bool HasRendition(const std::shared_ptr<const RtcPlaylist> &pl, const char *name)
	{
		return (pl != nullptr) && (pl->GetRendition(name) != nullptr);
	}
}  // namespace

// Audio-only rendition declared AFTER the video rendition is cross-added into the
// video playlist; the video rendition remains the default (GetFirstRendition).
TEST(RtcMasterPlaylist, CrossAddVideoThenAudio)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("video", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	EXPECT_TRUE(HasRendition(vpl, "video"));
	EXPECT_TRUE(HasRendition(vpl, "audio"));  // cross-added
	ASSERT_NE(vpl->GetFirstRendition(), nullptr);
	EXPECT_STREQ(vpl->GetFirstRendition()->GetName().CStr(), "video");
	EXPECT_NE(vpl->GetFirstRendition()->GetVideoTrack(), nullptr);
}

// Audio-only rendition declared BEFORE the video rendition is back-filled into the
// later-created video playlist; the video rendition (not the earlier audio-only one)
// must remain the playlist default regardless of declaration order.
TEST(RtcMasterPlaylist, BackFillAudioThenVideo)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));
	master.AddRendition(MakeRendition("video", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	EXPECT_TRUE(HasRendition(vpl, "video"));
	EXPECT_TRUE(HasRendition(vpl, "audio"));  // back-filled
	ASSERT_NE(vpl->GetFirstRendition(), nullptr);
	EXPECT_STREQ(vpl->GetFirstRendition()->GetName().CStr(), "video");
	EXPECT_NE(vpl->GetFirstRendition()->GetVideoTrack(), nullptr);
}

// In a video playlist, automatic bitrate ABR never selects the audio-only rendition,
// even though it is the lowest bitrate; it stays reachable only by name.
TEST(RtcPlaylist, VideoPlaylistAbrExcludesAudioOnly)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("high", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("low", MediaCodecId::H264, 300000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	auto high = vpl->GetRendition("high");
	auto low = vpl->GetRendition("low");
	auto audio = vpl->GetRendition("audio");
	ASSERT_NE(high, nullptr);
	ASSERT_NE(low, nullptr);
	ASSERT_NE(audio, nullptr);

	// audio-only is the lowest bitrate but must be skipped: no lower rendition than low video
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(low), nullptr);
	// from high, the next lower is the low *video* rendition
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(high), low);
	// from low, the next higher is the high video rendition
	EXPECT_EQ(vpl->GetNextHigherBitrateRendition(low), high);
	// audio-only is never an ABR destination
	EXPECT_NE(vpl->GetNextLowerBitrateRendition(high), audio);
}

// The audio-only rendition's bitrate is placed BETWEEN the two video renditions, so without
// the guard it would be the closest ABR neighbour in BOTH directions (next-lower from the high
// rendition, next-higher from the low rendition). The guard must skip it either way, so ABR
// steps between the video renditions and never onto audio-only. Every assertion here flips if
// the guard is removed, unlike the lowest-rung case above.
TEST(RtcPlaylist, VideoPlaylistAbrSkipsInterstitialAudioOnly)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("vhigh", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("vlow", MediaCodecId::H264, 300000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 800000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	auto vhigh = vpl->GetRendition("vhigh");
	auto vlow = vpl->GetRendition("vlow");
	auto audio = vpl->GetRendition("audio");
	ASSERT_NE(vhigh, nullptr);
	ASSERT_NE(vlow, nullptr);
	ASSERT_NE(audio, nullptr);
	// precondition: audio-only really is the closest neighbour by bitrate in both directions
	ASSERT_GT(audio->GetBitrates(), vlow->GetBitrates());
	ASSERT_LT(audio->GetBitrates(), vhigh->GetBitrates());

	// down from vhigh: without the guard the closest lower is audio; with the guard, vlow
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(vhigh), vlow);
	EXPECT_NE(vpl->GetNextLowerBitrateRendition(vhigh), audio);
	// up from vlow: without the guard the closest higher is audio; with the guard, vhigh
	EXPECT_EQ(vpl->GetNextHigherBitrateRendition(vlow), vhigh);
	EXPECT_NE(vpl->GetNextHigherBitrateRendition(vlow), audio);
}

// A video session that explicitly switched to the audio-only rendition stays there:
// automatic ABR must not move it in either direction (so it does not auto-resume video when
// bandwidth recovers, even if the client left auto-ABR enabled). The audio-only bitrate is
// placed between the two video renditions so both directions would otherwise pick a video
// rendition; the pin must return null both ways.
TEST(RtcPlaylist, VideoPlaylistAbrPinsAudioOnlyBase)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("vhigh", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("vlow", MediaCodecId::H264, 300000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 800000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	auto audio = vpl->GetRendition("audio");
	ASSERT_NE(audio, nullptr);
	EXPECT_EQ(vpl->GetNextHigherBitrateRendition(audio), nullptr);
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(audio), nullptr);
}

// Regression guard: in an audio-only playlist (an audio-only subscriber's bucket),
// bitrate ABR still works normally among the Opus renditions.
TEST(RtcPlaylist, AudioOnlyPlaylistAbrWorks)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("a_hi", MediaCodecId::None, 0, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("a_mid", MediaCodecId::None, 0, MediaCodecId::Opus, 64000));
	master.AddRendition(MakeRendition("a_lo", MediaCodecId::None, 0, MediaCodecId::Opus, 32000));

	auto apl = master.GetPlaylist(MediaCodecId::None, MediaCodecId::Opus);
	ASSERT_NE(apl, nullptr);
	auto hi = apl->GetRendition("a_hi");
	auto mid = apl->GetRendition("a_mid");
	auto lo = apl->GetRendition("a_lo");
	ASSERT_NE(hi, nullptr);
	ASSERT_NE(mid, nullptr);
	ASSERT_NE(lo, nullptr);

	EXPECT_EQ(apl->GetNextLowerBitrateRendition(hi), mid);
	EXPECT_EQ(apl->GetNextLowerBitrateRendition(mid), lo);
	EXPECT_EQ(apl->GetNextLowerBitrateRendition(lo), nullptr);
	EXPECT_EQ(apl->GetNextHigherBitrateRendition(lo), mid);
}

// A stream with only video renditions: nothing is cross-added, normal video ABR holds.
TEST(RtcMasterPlaylist, NoAudioOnlyNoCrossAdd)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("high", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("low", MediaCodecId::H264, 300000, MediaCodecId::Opus, 128000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	EXPECT_EQ(vpl->GetRenditions().size(), 2u);
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(vpl->GetRendition("high")), vpl->GetRendition("low"));
}

// An audio-only rendition is cross-added only into video playlists that share its
// audio codec: an Opus audio-only rendition is not added to an AAC video playlist.
TEST(RtcMasterPlaylist, MismatchedAudioCodecNoCrossAdd)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("video", MediaCodecId::H264, 2000000, MediaCodecId::Aac, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Aac);
	ASSERT_NE(vpl, nullptr);
	EXPECT_TRUE(HasRendition(vpl, "video"));
	EXPECT_FALSE(HasRendition(vpl, "audio"));
}

// An audio-only rendition is cross-added into EVERY video playlist that shares its audio codec.
TEST(RtcMasterPlaylist, CrossAddToMultipleVideoPlaylists)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("h264", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("vp8", MediaCodecId::Vp8, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	EXPECT_TRUE(HasRendition(master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus), "audio"));
	EXPECT_TRUE(HasRendition(master.GetPlaylist(MediaCodecId::Vp8, MediaCodecId::Opus), "audio"));
}

// Video-only renditions (no audio track): no crash, their own (video,None) bucket, ABR among
// them works, and an Opus audio-only rendition is not injected into the audio-less video playlist.
TEST(RtcMasterPlaylist, VideoOnlyRenditionsNoAudioTrack)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	master.AddRendition(MakeRendition("v_hi", MediaCodecId::H264, 2000000, MediaCodecId::None, 0));
	master.AddRendition(MakeRendition("v_lo", MediaCodecId::H264, 300000, MediaCodecId::None, 0));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::None);
	ASSERT_NE(vpl, nullptr);
	EXPECT_FALSE(HasRendition(vpl, "audio"));  // audio codec None != Opus -> not cross-added
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(vpl->GetRendition("v_hi")), vpl->GetRendition("v_lo"));
}

// With the fallback disabled (the default), an audio-only rendition is NOT cross-added into a
// video playlist; it stays only in its own (None, audio) playlist.
TEST(RtcMasterPlaylist, AudioOnlyFallbackDisabledNoCrossAdd)
{
	RtcMasterPlaylist master("m", "m");
	master.AddRendition(MakeRendition("video", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	master.AddRendition(MakeRendition("audio", MediaCodecId::None, 0, MediaCodecId::Opus, 48000));

	// not exposed to a video session...
	EXPECT_FALSE(HasRendition(master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus), "audio"));
	// ...but still available in its own audio-only playlist
	EXPECT_TRUE(HasRendition(master.GetPlaylist(MediaCodecId::None, MediaCodecId::Opus), "audio"));
}

namespace
{
	std::shared_ptr<const MediaTrack> MakeAudioTrack(uint32_t id, const ov::String &label, int32_t bitrate = 128000)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(id);
		track->SetMediaType(MediaType::Audio);
		track->SetCodecId(MediaCodecId::Opus);
		track->SetVariantName("opus");
		track->SetPublicName(label);
		track->SetBitrateByConfig(bitrate);
		return track;
	}

	std::shared_ptr<const MediaTrack> MakeVideoTrack(uint32_t id, int32_t bitrate)
	{
		auto track = std::make_shared<MediaTrack>();
		track->SetId(id);
		track->SetMediaType(MediaType::Video);
		track->SetCodecId(MediaCodecId::H264);
		track->SetBitrateByConfig(bitrate);
		return track;
	}
}  // namespace

// Without WebRtcMultiAudioTrack a rendition sends only its selected audio track, on slot 0,
// and the player JSON keeps its previous shape (no "audio_tracks").
TEST(RtcRendition, SingleAudioTrackUsesSlotZero)
{
	auto audio = MakeAudioTrack(10, "English");
	RtcRendition rendition("r", MakeVideoTrack(1, 2000000), audio);

	EXPECT_FALSE(rendition.IsMultiAudioTrack());
	ASSERT_EQ(rendition.GetAudioTracks().size(), 1u);
	EXPECT_EQ(rendition.GetAudioTracks()[0], audio);
	EXPECT_EQ(rendition.GetAudioSlotIndex(10), 0);
	EXPECT_EQ(rendition.GetAudioSlotIndex(11), -1);
	EXPECT_FALSE(rendition.ToJson().isMember("audio_tracks"));
}

// A rendition without audio sends nothing on any audio slot.
TEST(RtcRendition, NoAudioTrackHasNoSlots)
{
	RtcRendition rendition("r", MakeVideoTrack(1, 2000000), nullptr);

	EXPECT_TRUE(rendition.GetAudioTracks().empty());
	EXPECT_EQ(rendition.GetAudioSlotIndex(0), -1);
}

// Multi audio: slots follow the given (group index) order, not the selected track, so a slot
// carries the same language in every rendition. The selected track stays GetAudioTrack().
TEST(RtcRendition, MultiAudioSlotsFollowGroupOrder)
{
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	auto es = MakeAudioTrack(12, "Spanish");
	RtcRendition rendition("r", MakeVideoTrack(1, 2000000), ko, {en, ko, es});

	EXPECT_TRUE(rendition.IsMultiAudioTrack());
	EXPECT_EQ(rendition.GetAudioTrack(), ko);
	ASSERT_EQ(rendition.GetAudioTracks().size(), 3u);
	EXPECT_EQ(rendition.GetAudioSlotIndex(10), 0);
	EXPECT_EQ(rendition.GetAudioSlotIndex(11), 1);
	EXPECT_EQ(rendition.GetAudioSlotIndex(12), 2);
	EXPECT_EQ(rendition.GetAudioSlotIndex(1), -1);	// the video track is not an audio slot
}

// The selected track is always sent, even if the caller's list left it out: it takes slot 0.
TEST(RtcRendition, MultiAudioKeepsSelectedTrack)
{
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	RtcRendition rendition("r", nullptr, en, {ko});

	ASSERT_EQ(rendition.GetAudioTracks().size(), 2u);
	EXPECT_EQ(rendition.GetAudioSlotIndex(10), 0);
	EXPECT_EQ(rendition.GetAudioSlotIndex(11), 1);
}

// Null entries in the list are dropped instead of becoming empty slots.
TEST(RtcRendition, MultiAudioDropsNullTracks)
{
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	RtcRendition rendition("r", nullptr, en, {en, nullptr, ko});

	ASSERT_EQ(rendition.GetAudioTracks().size(), 2u);
	EXPECT_EQ(rendition.GetAudioSlotIndex(11), 1);
}

// Every audio track is sent, so the session bitrate (used by ABR) counts all of them.
TEST(RtcRendition, MultiAudioBitrateCountsEveryTrack)
{
	RtcRendition rendition("r", MakeVideoTrack(1, 1000000),
						   MakeAudioTrack(10, "English", 100000),
						   {MakeAudioTrack(10, "English", 100000), MakeAudioTrack(11, "Korean", 100000), MakeAudioTrack(12, "Spanish", 100000)});

	EXPECT_EQ(rendition.GetBitrates(), 1300000u);
}

// The player JSON lists the tracks in slot order with their AudioMap name as "label",
// and keeps "audio_track" as the selected track.
TEST(RtcRendition, MultiAudioJsonListsTracksInSlotOrder)
{
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	RtcRendition rendition("r", MakeVideoTrack(1, 2000000), ko, {en, ko});

	auto json = rendition.ToJson();
	ASSERT_TRUE(json.isMember("audio_tracks"));
	ASSERT_TRUE(json["audio_tracks"].isArray());
	ASSERT_EQ(json["audio_tracks"].size(), 2u);
	EXPECT_EQ(json["audio_tracks"][0]["id"].asUInt(), 10u);
	EXPECT_STREQ(json["audio_tracks"][0]["label"].asCString(), "English");
	EXPECT_EQ(json["audio_tracks"][1]["id"].asUInt(), 11u);
	EXPECT_STREQ(json["audio_tracks"][1]["label"].asCString(), "Korean");
	EXPECT_EQ(json["audio_track"]["id"].asUInt(), 11u);
}

// The label is the provider's AudioMap name: the "_<track id>" suffix the transcoder adds to an
// encoded track's public name is removed, and any other name is kept as is.
TEST(RtcRendition, AudioLabelDropsTranscoderSuffix)
{
	EXPECT_STREQ(RtcRendition::GetAudioLabel(MakeAudioTrack(3, "English_3")).CStr(), "English");
	EXPECT_STREQ(RtcRendition::GetAudioLabel(MakeAudioTrack(3, "English")).CStr(), "English");
	EXPECT_STREQ(RtcRendition::GetAudioLabel(MakeAudioTrack(3, "Track_4")).CStr(), "Track_4");
	EXPECT_STREQ(RtcRendition::GetAudioLabel(MakeAudioTrack(3, "_3")).CStr(), "_3");
}

// The offer needs as many m=audio sections as the rendition with the most audio tracks.
TEST(RtcMasterPlaylist, AudioSlotCountIsLargestRendition)
{
	RtcMasterPlaylist master("m", "m");
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	auto es = MakeAudioTrack(12, "Spanish");
	master.AddRendition(std::make_shared<RtcRendition>("two", MakeVideoTrack(1, 300000), en, std::vector<std::shared_ptr<const MediaTrack>>{en, ko}));
	master.AddRendition(std::make_shared<RtcRendition>("three", MakeVideoTrack(2, 2000000), en, std::vector<std::shared_ptr<const MediaTrack>>{en, ko, es}));
	master.AddRendition(std::make_shared<RtcRendition>("video_only", MakeVideoTrack(3, 1000000), nullptr));

	EXPECT_EQ(master.GetAudioSlotCount(), 3u);
}

// Regression guard: without multi audio the offer keeps exactly one m=audio section,
// and a playlist with no audio needs none.
TEST(RtcMasterPlaylist, AudioSlotCountWithoutMultiAudio)
{
	RtcMasterPlaylist with_audio("m", "m");
	with_audio.AddRendition(MakeRendition("high", MediaCodecId::H264, 2000000, MediaCodecId::Opus, 128000));
	with_audio.AddRendition(MakeRendition("low", MediaCodecId::H264, 300000, MediaCodecId::Opus, 128000));
	EXPECT_EQ(with_audio.GetAudioSlotCount(), 1u);

	RtcMasterPlaylist video_only("m", "m");
	video_only.AddRendition(MakeRendition("v", MediaCodecId::H264, 2000000, MediaCodecId::None, 0));
	EXPECT_EQ(video_only.GetAudioSlotCount(), 0u);
}

// Video ABR still steps between multi audio renditions that share the same audio tracks.
TEST(RtcPlaylist, MultiAudioRenditionsAbrStepsByVideo)
{
	RtcMasterPlaylist master("m", "m");
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	std::vector<std::shared_ptr<const MediaTrack>> audio = {en, ko};
	master.AddRendition(std::make_shared<RtcRendition>("high", MakeVideoTrack(1, 2000000), en, audio));
	master.AddRendition(std::make_shared<RtcRendition>("low", MakeVideoTrack(2, 300000), en, audio));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	EXPECT_EQ(vpl->GetNextLowerBitrateRendition(vpl->GetRendition("high")), vpl->GetRendition("low"));
	EXPECT_EQ(vpl->GetNextHigherBitrateRendition(vpl->GetRendition("low")), vpl->GetRendition("high"));
}

// A multi audio audio-only rendition cross-added by WebRtcAudioOnlyFallback keeps all its slots.
TEST(RtcMasterPlaylist, AudioOnlyFallbackKeepsMultiAudioSlots)
{
	RtcMasterPlaylist master("m", "m");
	master.SetWebRtcAudioOnlyFallback(true);
	auto en = MakeAudioTrack(10, "English");
	auto ko = MakeAudioTrack(11, "Korean");
	std::vector<std::shared_ptr<const MediaTrack>> audio = {en, ko};
	master.AddRendition(std::make_shared<RtcRendition>("video", MakeVideoTrack(1, 2000000), en, audio));
	master.AddRendition(std::make_shared<RtcRendition>("audio", nullptr, en, audio));

	auto vpl = master.GetPlaylist(MediaCodecId::H264, MediaCodecId::Opus);
	ASSERT_NE(vpl, nullptr);
	auto audio_only = vpl->GetRendition("audio");
	ASSERT_NE(audio_only, nullptr);
	EXPECT_EQ(audio_only->GetAudioSlotIndex(11), 1);
	EXPECT_EQ(master.GetAudioSlotCount(), 2u);
}

// Opus RTP runs on a 48 kHz clock. A track already on it keeps its timestamps exactly; Opus from
// MPEG-TS (90 kHz) is rescaled, so 20 ms (1800 ticks) becomes 960.
TEST(RtcRtpTimestamp, AudioTimestampsAreOnTheRtpClock)
{
	const cmn::Timebase at_48k(1, 48000);
	const cmn::Timebase at_90k(1, 90000);

	EXPECT_EQ(ToRtpTimestamp(0, at_48k, 48000), 0u);
	EXPECT_EQ(ToRtpTimestamp(123456789, at_48k, 48000), 123456789u);
	// Beyond 32 bits, the RTP timestamp wraps
	EXPECT_EQ(ToRtpTimestamp((1LL << 32) + 5, at_48k, 48000), 5u);

	EXPECT_EQ(ToRtpTimestamp(1800, at_90k, 48000), 960u);
	EXPECT_EQ(ToRtpTimestamp(90000, at_90k, 48000), 48000u);
	EXPECT_EQ(ToRtpTimestamp(90000LL * 3600 * 10, at_90k, 48000), static_cast<uint32_t>(48000LL * 3600 * 10));

	// No usable clock rate: the timestamp is left as it is
	EXPECT_EQ(ToRtpTimestamp(1800, at_90k, 0), 1800u);
}
