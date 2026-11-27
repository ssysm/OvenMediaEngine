#include "session.h"

#include "application.h"
#include "base/info/stream.h"
#include "publisher_private.h"

namespace pub
{
	Session::Session(const std::shared_ptr<Application> &application, const std::shared_ptr<Stream> &stream)
		: info::Session(*std::static_pointer_cast<info::Stream>(stream))
	{
		_application = application;
		_stream		 = stream;
		_state		 = SessionState::Ready;
	}

	Session::Session(const info::Session &info, const std::shared_ptr<Application> &application, const std::shared_ptr<Stream> &stream)
		: info::Session(*std::static_pointer_cast<info::Stream>(stream), info)
	{
		_application = application;
		_stream		 = stream;
		_state		 = SessionState::Ready;
	}

	Session::~Session()
	{
	}

	const std::shared_ptr<Application> &Session::GetApplication()
	{
		return _application;
	}

	std::shared_ptr<const Application> Session::GetApplication() const
	{
		return _application;
	}

	const std::shared_ptr<Stream> &Session::GetStream()
	{
		return _stream;
	}

	std::shared_ptr<const Stream> Session::GetStream() const
	{
		return _stream;
	}

	std::shared_ptr<ov::Url> Session::GetRequestedUrl() const
	{
		return _requested_url;
	}

	void Session::SetRequestedUrl(const std::shared_ptr<ov::Url> &requested_url)
	{
		_requested_url = requested_url;
	}

	std::shared_ptr<ov::Url> Session::GetFinalUrl() const
	{
		return _final_url;
	}

	void Session::SetFinalUrl(const std::shared_ptr<ov::Url> &final_url)
	{
		_final_url = final_url;
	}

	bool Session::Start()
	{
		_state = SessionState::Started;
		return true;
	}

	bool Session::Stop()
	{
		_state = SessionState::Stopped;

		return true;
	}

	Session::SessionState Session::GetState()
	{
		return _state;
	}

	void Session::SetState(SessionState state)
	{
		_state = state;
	}

	void Session::Terminate(ov::String reason)
	{
		_state		  = SessionState::Error;
		_error_reason = reason;

		auto stream	  = GetStream();
		if (stream != nullptr)
		{
			stream->RemoveSession(GetId());
		}
	}

	void Session::RecordSentBytes(uint64_t bytes)
	{
		if (bytes == 0)
		{
			return;
		}

		_session_sent_bytes.fetch_add(bytes, std::memory_order_relaxed);

		auto now = std::chrono::steady_clock::now();
		ov::LockGuard lock(_throughput_mutex);
		_throughput_window_bytes += bytes;
		double elapsed_sec = std::chrono::duration<double>(now - _throughput_window_start).count();
		if (elapsed_sec >= 1.0)
		{
			_last_transmit_bps		 = (static_cast<double>(_throughput_window_bytes) * 8.0) / elapsed_sec;
			_throughput_window_bytes = 0;
			_throughput_window_start = now;
		}
	}

	uint64_t Session::GetSessionSentBytes() const
	{
		return _session_sent_bytes.load(std::memory_order_relaxed);
	}

	double Session::GetSessionTransmitBps() const
	{
		auto now = std::chrono::steady_clock::now();
		ov::LockGuard lock(_throughput_mutex);
		double elapsed_sec = std::chrono::duration<double>(now - _throughput_window_start).count();
		if (elapsed_sec >= 1.0)
		{
			if (_throughput_window_bytes > 0)
			{
				return (static_cast<double>(_throughput_window_bytes) * 8.0) / elapsed_sec;
			}
			// Idle for > 2s -> 0 bps
			if (elapsed_sec >= 2.0)
			{
				return 0.0;
			}
		}
		else if (_last_transmit_bps == 0.0 && elapsed_sec > 0.05 && _throughput_window_bytes > 0)
		{
			return (static_cast<double>(_throughput_window_bytes) * 8.0) / elapsed_sec;
		}
		return _last_transmit_bps;
	}
}  // namespace pub