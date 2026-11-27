//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <chrono>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace mon::openmetrics
{
	bool CompressGzip(const std::string &input, std::shared_ptr<ov::Data> *output);

	class ScrapeCache
	{
	public:
		ScrapeCache(bool enabled, int duration_ms)
			: _enabled(enabled),
			  _duration_ms(duration_ms > 0 ? duration_ms : 0)
		{
		}

		std::string GetOrRender(const std::string &cache_key, const std::function<std::string()> &render_fn)
		{
			if (!_enabled || _duration_ms <= 0)
			{
				return render_fn();
			}

			auto now = std::chrono::steady_clock::now();

			{
				std::shared_lock<std::shared_mutex> lock(_mutex);
				auto it = _entries.find(cache_key);
				if (it != _entries.end())
				{
					auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.created_at).count();
					if (age_ms < _duration_ms)
					{
						return it->second.payload;
					}
				}
			}

			std::string rendered = render_fn();

			{
				std::unique_lock<std::shared_mutex> lock(_mutex);
				auto store_now = std::chrono::steady_clock::now();

				// Double-check if another thread filled a fresh entry while we rendered
				auto it = _entries.find(cache_key);
				if (it != _entries.end())
				{
					auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(store_now - it->second.created_at).count();
					if (age_ms < _duration_ms)
					{
						return it->second.payload;
					}
				}

				_entries[cache_key] = Entry{rendered, store_now};

				if (_entries.size() > 64)
				{
					for (auto eit = _entries.begin(); eit != _entries.end();)
					{
						auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(store_now - eit->second.created_at).count();
						if (age_ms >= _duration_ms)
						{
							eit = _entries.erase(eit);
						}
						else
						{
							++eit;
						}
					}
				}
			}

			return rendered;
		}

		void Clear()
		{
			std::unique_lock<std::shared_mutex> lock(_mutex);
			_entries.clear();
		}

	private:
		struct Entry
		{
			std::string payload;
			std::chrono::steady_clock::time_point created_at;
		};

		bool _enabled;
		int _duration_ms;
		mutable std::shared_mutex _mutex;
		std::unordered_map<std::string, Entry> _entries;
	};
}  // namespace mon::openmetrics
