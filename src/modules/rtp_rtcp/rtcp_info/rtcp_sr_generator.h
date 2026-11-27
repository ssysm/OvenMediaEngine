#pragma once

#include <array>
#include <utility>

#include "base/common_types.h"
#include "../rtp_packet.h"
#include "../rtcp_packet.h"

class RtcpSRGenerator
{
public:
    RtcpSRGenerator(uint32_t ssrc, uint32_t codec_rate);

	void AddRTPPacketInfo(const std::shared_ptr<RtpPacket> &rtp_packet);
	bool IsAvailableRtcpSRPacket() const;
	std::shared_ptr<RtcpPacket> PopRtcpSRPacket();

	// RFC 3550 §6.4.1 RTT lookup from incoming Receiver Report (lsr, dlsr) against sent SRs
	bool ComputeRttSeconds(uint32_t lsr, uint32_t dlsr, std::chrono::steady_clock::time_point now, double *out_rtt_seconds) const;
	
private:
	uint32_t GetElapsedTimeMSFromCreated();
	uint32_t GetElapsedTimeMSFromRtcpSRGenerated();

    uint32_t	_ssrc = 0;
	uint32_t	_last_timestamp = 0;
	uint64_t	_last_ntptime = 0;
    uint32_t    _rtcp_generated_count = 0;
	// Totals since the first packet, wrapping at 32 bits like the SR fields
    uint32_t    _packet_count = 0;
    uint32_t    _octec_count = 0;
	// Whether a packet was sent since the last SR, so an idle track sends no SR
	bool		_has_unreported_packets = false;

	std::chrono::steady_clock::time_point _created_time;
    std::chrono::steady_clock::time_point _last_generated_time;
	uint32_t	_codec_rate = 1;

	static constexpr size_t kSentSrHistorySize = 8;
	std::array<std::pair<uint32_t, std::chrono::steady_clock::time_point>, kSentSrHistorySize> _sent_sr_history{};
	size_t _sent_sr_history_count = 0;
	size_t _sent_sr_history_next  = 0;

	std::shared_ptr<RtcpPacket>	_rtcp_packet = nullptr;
};