//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "metrics_controller.h"

#include <monitoring/openmetrics/openmetrics_collectors.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace api::v2
{
	namespace
	{
		constexpr const char *kOpenMetricsContentType =
			"application/openmetrics-text; version=1.0.0; charset=utf-8";

		std::string TrimAscii(const std::string &s)
		{
			size_t start = 0;
			while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
			{
				++start;
			}
			size_t end = s.size();
			while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
			{
				--end;
			}
			return s.substr(start, end - start);
		}

		std::string ToLowerAscii(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
			});
			return s;
		}
	}  // namespace

	MetricsController::MetricsController()
		: _cache(true, 3000)
	{
	}

	MetricsController::MetricsController(const cfg::mgr::api::openmetrics::Cache &cache_config)
		: _cache(cache_config.IsEnabled(), cache_config.GetDurationMs())
	{
	}

	void MetricsController::PrepareHandlers()
	{
		// GET /v2/metrics
		RegisterMetricsRoute("", ScopeLevel::Root);

		// GET /v2/metrics/vhosts/{vhost}
		RegisterMetricsRoute(R"(\/vhosts\/(?<vhost_name>[^\/]+))", ScopeLevel::VHost);

		// GET /v2/metrics/vhosts/{vhost}/apps/{app}
		RegisterMetricsRoute(R"(\/vhosts\/(?<vhost_name>[^\/]+)\/apps\/(?<app_name>[^\/]+))", ScopeLevel::App);

		// GET /v2/metrics/vhosts/{vhost}/apps/{app}/streams/{stream}
		RegisterMetricsRoute(R"(\/vhosts\/(?<vhost_name>[^\/]+)\/apps\/(?<app_name>[^\/]+)\/streams\/(?<stream_name>[^\/]+))", ScopeLevel::Stream);

		// GET /v2/metrics/vhosts/{vhost}/apps/{app}/streams/{stream}/sessions
		RegisterMetricsRoute(R"(\/vhosts\/(?<vhost_name>[^\/]+)\/apps\/(?<app_name>[^\/]+)\/streams\/(?<stream_name>[^\/]+)\/sessions)", ScopeLevel::Sessions);
	}

	void MetricsController::RegisterMetricsRoute(const ov::String &pattern, ScopeLevel level)
	{
		auto full_pattern = ov::String::FormatString("%s%s", _prefix.CStr(), pattern.CStr());

		_interceptor->Register(
			http::Method::Get,
			full_pattern,
			[this, level](const std::shared_ptr<http::svr::HttpExchange> &client) -> http::svr::NextHandler {
				return HandleMetricsRequest(client, level);
			});
	}

	bool MetricsController::AcceptsGzip(const ov::String &accept_encoding)
	{
		if (accept_encoding.IsEmpty())
		{
			return false;
		}

		std::string header(accept_encoding.CStr(), accept_encoding.GetLength());
		size_t pos = 0;
		while (pos <= header.size())
		{
			size_t comma = header.find(',', pos);
			std::string item = (comma == std::string::npos)
								   ? header.substr(pos)
								   : header.substr(pos, comma - pos);

			size_t semi = item.find(';');
			std::string coding = ToLowerAscii(TrimAscii(semi == std::string::npos ? item : item.substr(0, semi)));

			if (coding == "gzip" || coding == "x-gzip")
			{
				bool rejected = false;
				if (semi != std::string::npos)
				{
					std::string params = item.substr(semi + 1);
					size_t ppos = 0;
					while (ppos <= params.size())
					{
						size_t psemi = params.find(';', ppos);
						std::string param = TrimAscii(psemi == std::string::npos ? params.substr(ppos) : params.substr(ppos, psemi - ppos));
						size_t eq = param.find('=');
						if (eq != std::string::npos)
						{
							std::string key = ToLowerAscii(TrimAscii(param.substr(0, eq)));
							if (key == "q")
							{
								std::string val = TrimAscii(param.substr(eq + 1));
								char *endptr = nullptr;
								double q = std::strtod(val.c_str(), &endptr);
								if (endptr != val.c_str() && q <= 0.0)
								{
									rejected = true;
								}
							}
						}
						if (psemi == std::string::npos)
						{
							break;
						}
						ppos = psemi + 1;
					}
				}

				if (!rejected)
				{
					return true;
				}
			}

			if (comma == std::string::npos)
			{
				break;
			}
			pos = comma + 1;
		}

		return false;
	}

	ov::String MetricsController::ExtractRawQuery(const std::shared_ptr<http::svr::HttpRequest> &request)
	{
		if (request == nullptr)
		{
			return "";
		}

		ov::String target = request->GetRequestTarget();
		if (target.IsEmpty())
		{
			target = request->GetUri();
		}

		std::string str(target.CStr(), target.GetLength());
		size_t q = str.find('?');
		if (q == std::string::npos)
		{
			return "";
		}

		size_t hash = str.find('#', q + 1);
		std::string query = (hash == std::string::npos)
								? str.substr(q + 1)
								: str.substr(q + 1, hash - (q + 1));
		return ov::String(query.c_str(), query.size());
	}

	http::svr::NextHandler MetricsController::HandleMetricsRequest(
		const std::shared_ptr<http::svr::HttpExchange> &client,
		ScopeLevel level)
	{
		auto request = client->GetRequest();
		auto response = client->GetResponse();

		try
		{
			bool is_sessions_path = (level == ScopeLevel::Sessions);
			uint32_t collector_mask = mon::openmetrics::kDefaultCollectors;
			ov::String parse_error;
			ov::String raw_query = ExtractRawQuery(request);

			if (!mon::openmetrics::ParseCollectorSelection(raw_query, is_sessions_path, &collector_mask, &parse_error))
			{
				throw http::HttpError(http::StatusCode::BadRequest, "%s", parse_error.CStr());
			}

			OpenMetricsScope scope;
			scope.is_root = (level == ScopeLevel::Root);
			scope.collector_mask = collector_mask;

			const auto &match_result = request->GetMatchResult();
			char cache_key_buf[128];

			switch (level)
			{
				case ScopeLevel::Root:
					std::snprintf(cache_key_buf, sizeof(cache_key_buf), "root|%x", collector_mask);
					break;

				case ScopeLevel::VHost:
					GetVirtualHostMetrics(match_result, &scope.vhost);
					std::snprintf(cache_key_buf, sizeof(cache_key_buf), "vhost:%u|%x", scope.vhost->GetId(), collector_mask);
					break;

				case ScopeLevel::App:
					GetVirtualHostMetrics(match_result, &scope.vhost);
					GetApplicationMetrics(match_result, scope.vhost, &scope.app);
					std::snprintf(cache_key_buf, sizeof(cache_key_buf), "app:%u:%u|%x",
								  scope.vhost->GetId(), scope.app->GetId(), collector_mask);
					break;

				case ScopeLevel::Stream:
				case ScopeLevel::Sessions:
					GetVirtualHostMetrics(match_result, &scope.vhost);
					GetApplicationMetrics(match_result, scope.vhost, &scope.app);
					GetStreamMetrics(match_result, scope.vhost, scope.app, &scope.stream, &scope.output_streams);
					std::snprintf(cache_key_buf, sizeof(cache_key_buf), "%s:%u:%u:%u|%x",
								  is_sessions_path ? "sessions" : "stream",
								  scope.vhost->GetId(), scope.app->GetId(), scope.stream->GetId(),
								  collector_mask);
					break;
			}

			std::string payload = _cache.GetOrRender(cache_key_buf, [&scope]() {
				return OpenMetricsRenderer::Render(scope);
			});

			response->SetStatusCode(http::StatusCode::OK);
			response->SetHeader("Content-Type", kOpenMetricsContentType);

			auto accept_encoding = request->GetHeader("Accept-Encoding");
			std::shared_ptr<ov::Data> compressed;
			if (AcceptsGzip(accept_encoding) && mon::openmetrics::CompressGzip(payload, &compressed) && compressed != nullptr)
			{
				response->SetHeader("Content-Encoding", "gzip");
				response->SetHeader("Vary", "Accept-Encoding");
				response->AppendData(compressed);
			}
			else
			{
				auto data = std::make_shared<ov::Data>(payload.data(), payload.size());
				response->AppendData(data);
			}
		}
		catch (const http::HttpError &error)
		{
			logw("APIController", "HTTP error occurred: %s", error.What());
			ApiResponse(&error).SendToClient(client);
		}
		catch (const cfg::ConfigError &error)
		{
			logw("APIController", "Config error occurred: %s", error.GetDetailedMessage().CStr());
			ApiResponse(&error).SendToClient(client);
		}
		catch (const std::exception &error)
		{
			logw("APIController", "Unknown error occurred: %s", error.what());
			ApiResponse(&error).SendToClient(client);
		}

		return http::svr::NextHandler::DoNotCall;
	}
}  // namespace api::v2
