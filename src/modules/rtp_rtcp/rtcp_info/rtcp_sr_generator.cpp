#include "rtcp_sr_generator.h"
#include "sender_report.h"

RtcpSRGenerator::RtcpSRGenerator(uint32_t ssrc, uint32_t codec_rate)
{
    _ssrc = ssrc;
	_codec_rate = codec_rate;
    _created_time = std::chrono::steady_clock::now();
    _last_generated_time = std::chrono::steady_clock::now();
}

void RtcpSRGenerator::AddRTPPacketInfo(const std::shared_ptr<RtpPacket> &rtp_packet)
{
    _packet_count ++;
    _octec_count += rtp_packet->PayloadSize();
	_last_timestamp = rtp_packet->Timestamp();
	_last_ntptime = rtp_packet->NTPTimestamp();
	_has_unreported_packets = true;
}

bool RtcpSRGenerator::IsAvailableRtcpSRPacket() const
{
	return _has_unreported_packets;
}

std::shared_ptr<RtcpPacket> RtcpSRGenerator::PopRtcpSRPacket()
{
	if (_has_unreported_packets == false)
	{
		return nullptr; 
	}
	
	auto report = std::make_shared<SenderReport>();
	
	uint32_t msw = 0;
	uint32_t lsw = 0;

	msw = _last_ntptime >> 32;
	lsw = _last_ntptime & 0xFFFFFFFF;

	// logc("DEBUG", "[SR] Rate(%d) Timestamp(%u) Clock(%lf) ipart(%lf) fraction(%lf) msw(%u) lsw(%u) reverse(%u)",  _codec_rate, rtp_packet.Timestamp(), clock, ipart, fraction, msw, lsw, reverse);
	
	report->SetSenderSsrc(_ssrc);
	report->SetMsw(msw);
	report->SetLsw(lsw);
	
	report->SetTimestamp(_last_timestamp);
	report->SetPacketCount(_packet_count);
	report->SetOctetCount(_octec_count);

	auto rtcp_packet = std::make_shared<RtcpPacket>();
	rtcp_packet->Build(report);

	// The counts keep running, only the new packet mark is cleared
	_has_unreported_packets = false;
	auto now				= std::chrono::steady_clock::now();
	_last_generated_time	= now;
	_rtcp_generated_count++;

	uint32_t compact_ntp				   = ((msw & 0xFFFFu) << 16) | ((lsw >> 16) & 0xFFFFu);
	_sent_sr_history[_sent_sr_history_next] = {compact_ntp, now};
	_sent_sr_history_next				   = (_sent_sr_history_next + 1) % kSentSrHistorySize;
	if (_sent_sr_history_count < kSentSrHistorySize)
	{
		_sent_sr_history_count++;
	}

    return rtcp_packet;
}

bool RtcpSRGenerator::ComputeRttSeconds(uint32_t lsr, uint32_t dlsr, std::chrono::steady_clock::time_point now, double *out_rtt_seconds) const
{
	if (lsr == 0 || out_rtt_seconds == nullptr || _sent_sr_history_count == 0)
	{
		return false;
	}

	// Search newest to oldest for matching compact NTP LSR
	for (size_t offset = 1; offset <= _sent_sr_history_count; ++offset)
	{
		size_t idx = (_sent_sr_history_next + kSentSrHistorySize - offset) % kSentSrHistorySize;
		if (_sent_sr_history[idx].first == lsr)
		{
			double elapsed_sec = std::chrono::duration<double>(now - _sent_sr_history[idx].second).count();
			double dlsr_sec	   = static_cast<double>(dlsr) / 65536.0;
			double rtt		   = elapsed_sec - dlsr_sec;
			if (rtt < 0.0)
			{
				return false;
			}
			*out_rtt_seconds = rtt;
			return true;
		}
	}

	return false;
}

uint32_t RtcpSRGenerator::GetElapsedTimeMSFromCreated()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _created_time).count();
}

uint32_t RtcpSRGenerator::GetElapsedTimeMSFromRtcpSRGenerated()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _last_generated_time).count();
}