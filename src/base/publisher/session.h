//
// Created by getroot on 18. 3. 19.
//

#pragma once

#include <atomic>
#include <chrono>
#include <optional>

#include <base/ovlibrary/ovlibrary.h>
#include <base/ovlibrary/tsa/mutex.h>

#include "base/common_types.h"
#include "base/info/session.h"

namespace ov
{
	class Socket;
}

namespace pub
{
	class Application;
	class Stream;

	class Session : public info::Session
	{
	public:
		Session(const std::shared_ptr<Application> &application, const std::shared_ptr<Stream> &stream);
		Session(const info::Session &info, const std::shared_ptr<Application> &app, const std::shared_ptr<Stream> &stream);
		virtual ~Session();

		const std::shared_ptr<Application> &GetApplication();
		std::shared_ptr<const Application> GetApplication() const;
		const std::shared_ptr<Stream> &GetStream();
		std::shared_ptr<const Stream> GetStream() const;

		std::shared_ptr<ov::Url> GetRequestedUrl() const;
		void SetRequestedUrl(const std::shared_ptr<ov::Url> &requested_url);

		std::shared_ptr<ov::Url> GetFinalUrl() const;
		void SetFinalUrl(const std::shared_ptr<ov::Url> &final_url);

		virtual bool Start();
		virtual bool Stop();

		virtual void SendOutgoingData(const std::any &packet) {};
		virtual void OnMessageReceived(const std::any &message) {};

		enum class SessionState : int8_t
		{
			Ready,
			Started,
			Stopping,
			Stopped,
			Error
		};

		SessionState GetState();
		void SetState(SessionState state);
		virtual void Terminate(ov::String reason);

		// Per-session observability metrics (OpenMetrics `session` collector)
		void RecordSentBytes(uint64_t bytes);
		uint64_t GetSessionSentBytes() const;
		double GetSessionTransmitBps() const;

		// Transport / RTT introspection hooks (overridden by protocol sessions)
		virtual std::shared_ptr<ov::Socket> GetSessionTransportSocket() const
		{
			return nullptr;
		}

		virtual std::optional<double> GetSessionRtcpRttSeconds() const
		{
			return std::nullopt;
		}

		virtual std::optional<double> GetSessionStunRttSeconds() const
		{
			return std::nullopt;
		}

		struct EgressLatencySummary
		{
			uint32_t track_id  = 0;
			uint64_t count	   = 0;
			double sum_seconds = 0.0;
		};

		virtual std::vector<EgressLatencySummary> GetSessionEgressLatencySnapshot() const
		{
			return {};
		}

	protected:
		std::shared_ptr<ov::Url> _requested_url;
		std::shared_ptr<ov::Url> _final_url;

	private:
		std::shared_ptr<Application> _application;
		std::shared_ptr<Stream> _stream;
		SessionState _state;
		ov::String _error_reason;

		// Per-session byte counter and 1-second transmit throughput estimator
		std::atomic<uint64_t> _session_sent_bytes{0};
		mutable ov::Mutex _throughput_mutex;
		uint64_t _throughput_window_bytes OV_GUARDED_BY(_throughput_mutex) = 0;
		std::chrono::steady_clock::time_point _throughput_window_start OV_GUARDED_BY(_throughput_mutex){std::chrono::steady_clock::now()};
		double _last_transmit_bps OV_GUARDED_BY(_throughput_mutex) = 0.0;
	};

}  // namespace pub