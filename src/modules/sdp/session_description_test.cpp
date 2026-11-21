//==============================================================================
//
//  OvenMediaEngine - Unit Tests
//
//  tests/modules/sdp/test_session_description.cpp
//  Covers: SessionDescription (SDP parsing & building)
//
//==============================================================================
#include <gtest/gtest.h>

#include <modules/sdp/sdp_regex_pattern.h>
#include <modules/sdp/session_description.h>

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

// A minimal valid SDP string for WebRTC (Offer)
static const char *kMinimalSdp =
	"v=0\r\n"
	"o=OvenMediaEngine 1882243660 2 IN IP4 127.0.0.1\r\n"
	"s=-\r\n"
	"t=0 0\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"a=group:BUNDLE video\r\n"
	"a=msid-semantic:WMS *\r\n"
	"a=fingerprint:sha-256 D7:81:CF:01:46:FB:2D:93:8E:04:AF:47:76:0A:88:08:FF:73:37:C6:A7:45:0B:31:FE:12:49:DE:A7:E4:1F:3A\r\n"
	"a=ice-options:trickle\r\n"
	"a=ice-pwd:c32d4070c67e9782bea90a9ab46ea838\r\n"
	"a=ice-ufrag:0dfa46c9\r\n"
	"m=video 9 UDP/TLS/RTP/SAVPF 97\r\n"
	"a=rtpmap:97 VP8/50000\r\n"
	"a=mid:video\r\n"
	"a=rtcp-mux\r\n"
	"a=setup:actpass\r\n"
	"a=sendonly\r\n"
	"a=ssrc:2064629418 cname:{b2266c86-259f-4853-8662-ea94cf0835a3}\r\n";

class SdpTest : public ::testing::Test
{
public:
	static void SetUpTestSuite()
	{
		// SDPRegexPattern must be compiled before any SDP parsing can occur,
		// just as main.cpp does at startup.
		ASSERT_TRUE(SDPRegexPattern::GetInstance()->Compile());
	}

protected:
	std::shared_ptr<SessionDescription> MakeOffer()
	{
		auto sdp = std::make_shared<SessionDescription>(SessionDescription::SdpType::Offer);
		auto result = sdp->FromString(kMinimalSdp);
		return result ? sdp : nullptr;
	}
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST_F(SdpTest, ParseMinimalSdpSucceeds)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
}

TEST_F(SdpTest, ParseVersion)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_EQ(sdp->GetVersion(), 0u);
}

TEST_F(SdpTest, ParseOriginFields)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_STREQ(sdp->GetUserName().CStr(), "OvenMediaEngine");
	EXPECT_EQ(sdp->GetSessionId(), 1882243660u);
	EXPECT_STREQ(sdp->GetAddress().CStr(), "127.0.0.1");
}

TEST_F(SdpTest, ParseIceUfrag)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_STREQ(sdp->GetIceUfrag().CStr(), "0dfa46c9");
}

TEST_F(SdpTest, ParseIcePwd)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_STREQ(sdp->GetIcePwd().CStr(), "c32d4070c67e9782bea90a9ab46ea838");
}

TEST_F(SdpTest, ParseFingerprint)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	auto algo = sdp->GetFingerprintAlgorithm();
	auto fingerprint = sdp->GetFingerprintValue();
	EXPECT_STREQ(algo.CStr(), "sha-256");
	EXPECT_FALSE(fingerprint.IsEmpty());
}

TEST_F(SdpTest, ParseMediaDescriptionCount)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_GE(sdp->GetMediaList().size(), 1u);
}

TEST_F(SdpTest, ParseMediaDescriptionVideoPayload)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	auto media = sdp->GetFirstMedia();
	ASSERT_NE(media, nullptr);
	// Should have payload type 97 -> VP8
	auto payload = media->GetPayload(97);
	ASSERT_NE(payload, nullptr);
	EXPECT_STREQ(payload->GetCodecStr().CStr(), "VP8");
}

TEST_F(SdpTest, SdpTypeIsOffer)
{
	auto sdp = MakeOffer();
	ASSERT_NE(sdp, nullptr);
	EXPECT_EQ(sdp->GetType(), SessionDescription::SdpType::Offer);
}

// ---------------------------------------------------------------------------
// Invalid SDP
// ---------------------------------------------------------------------------

// SessionDescription::FromString returns true even for empty/invalid input
// because ParsingSessionLine always returns true. Verify that an empty SDP
// at least produces an empty media list.
TEST(SdpParse, EmptyStringProducesEmptySdp)
{
	SDPRegexPattern::GetInstance()->Compile();
	auto sdp = std::make_shared<SessionDescription>(SessionDescription::SdpType::Offer);
	sdp->FromString("");
	EXPECT_TRUE(sdp->GetMediaList().empty());
}

// ---------------------------------------------------------------------------
// Multiple audio m= sections (WebRtcMultiAudioTrack)
// ---------------------------------------------------------------------------

// A browser-style answer to an offer with two audio sections: the second one is rejected (port 0).
static const char *kMultiAudioAnswerSdp =
	"v=0\r\n"
	"o=- 4611731400430051336 2 IN IP4 127.0.0.1\r\n"
	"s=-\r\n"
	"t=0 0\r\n"
	"a=group:BUNDLE v0 a0 a1\r\n"
	"a=fingerprint:sha-256 D7:81:CF:01:46:FB:2D:93:8E:04:AF:47:76:0A:88:08:FF:73:37:C6:A7:45:0B:31:FE:12:49:DE:A7:E4:1F:3A\r\n"
	"a=ice-pwd:c32d4070c67e9782bea90a9ab46ea838\r\n"
	"a=ice-ufrag:0dfa46c9\r\n"
	"m=video 9 UDP/TLS/RTP/SAVPF 98\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"a=rtpmap:98 H264/90000\r\n"
	"a=mid:v0\r\n"
	"a=rtcp-mux\r\n"
	"a=setup:active\r\n"
	"a=recvonly\r\n"
	"m=audio 9 UDP/TLS/RTP/SAVPF 110\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"a=rtpmap:110 opus/48000/2\r\n"
	"a=mid:a0\r\n"
	"a=rtcp-mux\r\n"
	"a=setup:active\r\n"
	"a=recvonly\r\n"
	"m=audio 0 UDP/TLS/RTP/SAVPF 110\r\n"
	"c=IN IP4 0.0.0.0\r\n"
	"a=rtpmap:110 opus/48000/2\r\n"
	"a=mid:a1\r\n"
	"a=rtcp-mux\r\n"
	"a=setup:active\r\n"
	"a=inactive\r\n";

TEST_F(SdpTest, ParseMultipleAudioSectionsKeepsOrderAndPorts)
{
	auto sdp = std::make_shared<SessionDescription>(SessionDescription::SdpType::Answer);
	ASSERT_TRUE(sdp->FromString(kMultiAudioAnswerSdp));

	auto media_list = sdp->GetMediaList();
	ASSERT_EQ(media_list.size(), 3u);

	EXPECT_EQ(media_list[1]->GetMediaType(), MediaDescription::MediaType::Audio);
	EXPECT_STREQ(media_list[1]->GetMid().value_or("").CStr(), "a0");
	EXPECT_EQ(media_list[1]->GetPort(), 9);
	ASSERT_NE(media_list[1]->GetFirstPayload(), nullptr);
	EXPECT_EQ(media_list[1]->GetFirstPayload()->GetId(), 110);

	EXPECT_EQ(media_list[2]->GetMediaType(), MediaDescription::MediaType::Audio);
	EXPECT_STREQ(media_list[2]->GetMid().value_or("").CStr(), "a1");
	EXPECT_EQ(media_list[2]->GetPort(), 0);
	EXPECT_FALSE(media_list[2]->IsBundleOnly());
}

// RFC 8843: a port 0 m= section with a=bundle-only is bundled, not rejected.
TEST_F(SdpTest, ParseBundleOnly)
{
	ov::String text = kMultiAudioAnswerSdp;
	text = text.Replace("a=mid:a1\r\n", "a=mid:a1\r\na=bundle-only\r\n");

	auto sdp = std::make_shared<SessionDescription>(SessionDescription::SdpType::Answer);
	ASSERT_TRUE(sdp->FromString(text));

	auto media_list = sdp->GetMediaList();
	ASSERT_EQ(media_list.size(), 3u);
	EXPECT_FALSE(media_list[1]->IsBundleOnly());
	EXPECT_EQ(media_list[2]->GetPort(), 0);
	EXPECT_TRUE(media_list[2]->IsBundleOnly());
}
