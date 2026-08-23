#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "AccessLog.h"
#include "ExactJson.h"
#include "HttpPolicy.h"
#include "HttpServer.h"
#include "RequestId.h"

namespace ocrservice {
namespace {

using http::middleware::AccessLogEntry;
using http::middleware::RequestBodyMode;
using http::protocol::ErrorCode;
using http::server::HttpMethod;
using http::server::HttpRequest;
using http::server::HttpResponse;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string trim(std::string value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

struct ParsedResponse final {
    int status = 0;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
    bool complete = false;
    std::string raw;
};

ParsedResponse parseResponse(std::string raw) {
    ParsedResponse response;
    response.raw = raw;
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return response;
    }
    std::istringstream headers(raw.substr(0U, headerEnd));
    std::string statusLine;
    std::getline(headers, statusLine);
    std::istringstream status(statusLine);
    std::string version;
    status >> version >> response.status;
    std::string line;
    while (std::getline(headers, line)) {
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            response.headers[lowercase(line.substr(0U, colon))] = trim(line.substr(colon + 1U));
        }
    }
    std::string wireBody = raw.substr(headerEnd + 4U);
    const auto length = response.headers.find("content-length");
    if (length != response.headers.end()) {
        const auto expected = static_cast<std::size_t>(std::stoull(length->second));
        response.complete = wireBody.size() >= expected;
        response.body = wireBody.substr(0U, std::min(wireBody.size(), expected));
        return response;
    }
    const auto transfer = response.headers.find("transfer-encoding");
    if (transfer == response.headers.end() || lowercase(transfer->second) != "chunked") {
        response.body = std::move(wireBody);
        response.complete = true;
        return response;
    }
    std::size_t position = 0U;
    while (position < wireBody.size()) {
        const auto lineEnd = wireBody.find("\r\n", position);
        if (lineEnd == std::string::npos) {
            return response;
        }
        std::size_t chunkSize = 0U;
        std::istringstream sizeInput(wireBody.substr(position, lineEnd - position));
        sizeInput >> std::hex >> chunkSize;
        if (!sizeInput || chunkSize > wireBody.size() - lineEnd - 2U) {
            return response;
        }
        position = lineEnd + 2U;
        if (chunkSize == 0U) {
            response.complete = wireBody.size() >= position + 2U &&
                                wireBody.compare(position, 2U, "\r\n") == 0;
            return response;
        }
        if (wireBody.size() < position + chunkSize + 2U) {
            return response;
        }
        response.body.append(wireBody, position, chunkSize);
        position += chunkSize;
        if (wireBody.compare(position, 2U, "\r\n") != 0) {
            return response;
        }
        position += 2U;
    }
    return response;
}

std::string exchange(const std::uint16_t port, const std::string& request) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::error_code writeError;
    asio::write(socket, asio::buffer(request), writeError);
    std::string raw;
    std::array<char, 16384> buffer{};
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error == asio::error::eof || error == asio::error::connection_reset) {
            break;
        }
        if (error) {
            break;
        }
    }
    return raw;
}

std::string exchangeHalfClosed(const std::uint16_t port, const std::string& request) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port});
    asio::write(socket, asio::buffer(request));
    socket.shutdown(asio::ip::tcp::socket::shutdown_send);

    std::string raw;
    std::array<char, 16384> buffer{};
    for (;;) {
        asio::error_code error;
        const auto count = socket.read_some(asio::buffer(buffer), error);
        raw.append(buffer.data(), count);
        if (error) {
            break;
        }
    }
    return raw;
}

struct WriteFailureGate final {
    std::mutex mutex;
    std::condition_variable ready;
    bool mayWrite = false;
};

void sendAndResetAfterHeaders(
    const std::uint16_t port,
    const std::string& request,
    const std::shared_ptr<WriteFailureGate>& gate) {
    const auto releaseProducer = [&] {
        {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->mayWrite = true;
        }
        gate->ready.notify_one();
    };
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    try {
        socket.connect({asio::ip::make_address("127.0.0.1"), port});
        asio::write(socket, asio::buffer(request));
        asio::streambuf responseHead;
        asio::read_until(socket, responseHead, "\r\n\r\n");
        socket.set_option(asio::socket_base::linger(true, 0));
        socket.close();
    } catch (...) {
        releaseProducer();
        throw;
    }
    releaseProducer();
}

std::string request(
    const std::string& method,
    const std::string& target,
    const std::vector<std::pair<std::string, std::string>>& headers = {},
    const std::string& body = {}) {
    std::string result = method + " " + target + " HTTP/1.1\r\nHost: localhost\r\n";
    bool hasLength = false;
    bool chunked = false;
    for (const auto& [name, value] : headers) {
        result += name + ": " + value + "\r\n";
        hasLength = hasLength || lowercase(name) == "content-length";
        chunked = chunked ||
                  (lowercase(name) == "transfer-encoding" && lowercase(value) == "chunked");
    }
    if (!hasLength && !chunked && !body.empty()) {
        result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    result += "Connection: close\r\n\r\n";
    result += body;
    return result;
}

nlohmann::json jsonBody(const ParsedResponse& response) {
    return nlohmann::json::parse(response.body);
}

void expectEnvelope(const ParsedResponse& response, const int status, const std::string& code) {
    ASSERT_EQ(response.status, status);
    ASSERT_TRUE(response.complete);
    ASSERT_EQ(response.headers.at("content-type"), "application/json");
    const auto body = jsonBody(response);
    EXPECT_EQ(body.size(), 5U);
    EXPECT_EQ(body.at("success"), status >= 200 && status < 300);
    EXPECT_EQ(body.at("code"), code);
    EXPECT_TRUE(body.at("requestId").is_string());
    const auto requestId = domain::Uuid::parse(body.at("requestId").get<std::string>());
    EXPECT_EQ(requestId.version(), 4U);
    if (status >= 400) {
        EXPECT_TRUE(body.at("data").is_null());
    }
}

class SequentialRequestIds final : public http::middleware::IRequestIdGenerator {
public:
    domain::Uuid next() override {
        std::array<std::uint8_t, 16> bytes{};
        const auto value = next_.fetch_add(1U);
        for (std::size_t index = 0U; index < sizeof(value); ++index) {
            bytes[bytes.size() - 1U - index] =
                static_cast<std::uint8_t>(value >> (index * 8U));
        }
        return domain::Uuid::v4(bytes);
    }

private:
    std::atomic<std::uint64_t> next_{1U};
};

class CapturingAccessLog final : public http::middleware::IAccessLogSink {
public:
    void write(const AccessLogEntry& entry) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
    }

    std::vector<AccessLogEntry> entries() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<AccessLogEntry> entries_;
};

template <typename Predicate>
std::optional<std::vector<AccessLogEntry>> waitForAccessLog(
    const std::shared_ptr<CapturingAccessLog>& accessLog,
    Predicate predicate) {
    constexpr std::size_t maxAttempts = 100U;
    for (std::size_t attempt = 0U; attempt < maxAttempts; ++attempt) {
        auto entries = accessLog->entries();
        if (std::any_of(entries.begin(), entries.end(), predicate)) {
            return entries;
        }
        if (attempt + 1U < maxAttempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return std::nullopt;
}

HttpResponse success(const HttpRequest& request, std::string value = "ok") {
    return HttpResponse::json(
        200,
        serialization::json::serializeExact(
            serialization::json::EnvelopeWriter::success(
                request.requestId, nlohmann::json{{"value", std::move(value)}})));
}

class HttpRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        requestIds_ = std::make_shared<SequentialRequestIds>();
        accessLog_ = std::make_shared<CapturingAccessLog>();
        writeFailureGate_ = std::make_shared<WriteFailureGate>();
        oversizedImageProducerCalled_ = std::make_shared<std::atomic<bool>>(false);
        server_ = std::make_unique<http::server::HttpServer>(0U, requestIds_, accessLog_);

        server_->registerRoute(HttpMethod::get, "/ok", {}, [](const HttpRequest& req) {
            return success(req);
        });
        server_->registerRoute(
            HttpMethod::post, "/json", {RequestBodyMode::json, true},
            [](const HttpRequest& req) {
                return success(req, req.bearerToken.value_or("missing"));
            });
        server_->registerRoute(
            HttpMethod::post, "/multipart", {RequestBodyMode::multipart, false},
            [](const HttpRequest& req) { return success(req); });
        server_->registerRoute(
            HttpMethod::get, "/recognitions/{id}", {}, [](const HttpRequest& req) {
                return success(req, req.pathParameters.at("id"));
            });
        server_->registerRoute(
            HttpMethod::get, "/recognitions/export", {}, [](const HttpRequest& req) {
                return success(req, "static");
            });
        server_->registerRoute(
            HttpMethod::get, "/reverse/fixed", {}, [](const HttpRequest& req) {
                return success(req, "reverse-static");
            });
        server_->registerRoute(
            HttpMethod::get, "/reverse/{id}", {}, [](const HttpRequest& req) {
                return success(req, req.pathParameters.at("id"));
            });
        server_->registerRoute(HttpMethod::get, "/throw", {}, [](const HttpRequest&) -> HttpResponse {
            throw std::runtime_error("sensitive controller failure");
        });
        server_->registerRoute(
            HttpMethod::get, "/redirect", {}, [](const HttpRequest& req) {
                auto body = serialization::json::serializeExact(
                    serialization::json::EnvelopeWriter::success(req.requestId));
                return HttpResponse::json(302, std::move(body));
            });
        server_->registerRoute(
            HttpMethod::get, "/large-json", {}, [](const HttpRequest&) {
                return HttpResponse::json(
                    200, std::string(http::protocol::kMaximumJsonBytes + 1U, 'x'));
            });
        server_->registerRoute(
            HttpMethod::get, "/image", {}, [](const HttpRequest&) {
                return HttpResponse::image(
                    http::protocol::ResponseBodyKind::jpeg, 5U,
                    [](http::server::IStreamWriter& writer) { writer.write("hello"); });
            });
        server_->registerRoute(
            HttpMethod::get, "/image-short", {}, [](const HttpRequest&) {
                return HttpResponse::image(
                    http::protocol::ResponseBodyKind::png, 5U,
                    [](http::server::IStreamWriter& writer) { writer.write("hey"); });
            });
        server_->registerRoute(
            HttpMethod::get, "/image-long", {}, [](const HttpRequest&) {
                return HttpResponse::image(
                    http::protocol::ResponseBodyKind::png, 3U,
                    [](http::server::IStreamWriter& writer) { writer.write("toolong"); });
            });
        server_->registerRoute(
            HttpMethod::get, "/image-too-large", {},
            [called = oversizedImageProducerCalled_](const HttpRequest&) {
                return HttpResponse::image(
                    http::protocol::ResponseBodyKind::jpeg,
                    http::protocol::kMaximumImageResponseBytes + 1U,
                    [called](http::server::IStreamWriter&) { called->store(true); });
            });
        server_->registerRoute(HttpMethod::get, "/csv", {}, [](const HttpRequest&) {
            return HttpResponse::csv([](http::server::IStreamWriter& writer) {
                writer.write("");
                writer.write("\xEF\xBB\xBF" "a,b\n");
            });
        });
        server_->registerRoute(HttpMethod::get, "/csv-empty", {}, [](const HttpRequest&) {
            return HttpResponse::csv([](http::server::IStreamWriter& writer) {
                writer.write("");
            });
        });
        server_->registerRoute(HttpMethod::get, "/csv-abort", {}, [](const HttpRequest&) {
            return HttpResponse::csv([](http::server::IStreamWriter& writer) {
                writer.write("first\n");
                throw std::runtime_error("sensitive producer failure");
            });
        });
        server_->registerRoute(HttpMethod::get, "/csv-write-failure", {}, [gate = writeFailureGate_](const HttpRequest&) {
            return HttpResponse::csv([gate](http::server::IStreamWriter& writer) {
                {
                    std::unique_lock<std::mutex> lock(gate->mutex);
                    gate->ready.wait(lock, [&] { return gate->mayWrite; });
                }
                const std::string block(64U * 1024U, 'x');
                for (std::size_t index = 0U; index < 1024U; ++index) {
                    writer.write(block);
                }
            });
        });

        thread_ = std::thread([this] { server_->run(); });
        server_->waitUntilStarted();
        port_ = server_->port();
        ASSERT_NE(port_, 0U);
    }

    void TearDown() override {
        server_->stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    ParsedResponse call(const std::string& raw) const {
        return parseResponse(exchange(port_, raw));
    }

    std::shared_ptr<SequentialRequestIds> requestIds_;
    std::shared_ptr<CapturingAccessLog> accessLog_;
    std::shared_ptr<WriteFailureGate> writeFailureGate_;
    std::shared_ptr<std::atomic<bool>> oversizedImageProducerCalled_;
    std::unique_ptr<http::server::HttpServer> server_;
    std::thread thread_;
    std::uint16_t port_ = 0U;
};

TEST_F(HttpRuntimeTest, RoutesDeterministicallyAndNeverUsesImplicitMethodsOrRedirects) {
    expectEnvelope(call(request("GET", "/missing")), 404, "ROUTE_NOT_FOUND");
    expectEnvelope(call(request("POST", "/ok")), 405, "METHOD_NOT_ALLOWED");
    expectEnvelope(call(request("HEAD", "/ok")), 405, "METHOD_NOT_ALLOWED");
    expectEnvelope(call(request("OPTIONS", "/ok")), 405, "METHOD_NOT_ALLOWED");
    expectEnvelope(call(request("GET", "/ok/")), 404, "ROUTE_NOT_FOUND");

    const auto exact = call(request("GET", "/recognitions/export"));
    expectEnvelope(exact, 200, "OK");
    EXPECT_EQ(jsonBody(exact).at("data").at("value"), "static");

    const auto parameter = call(request("GET", "/recognitions/abc"));
    expectEnvelope(parameter, 200, "OK");
    EXPECT_EQ(jsonBody(parameter).at("data").at("value"), "abc");

    const auto reverse = call(request("GET", "/reverse/fixed"));
    expectEnvelope(reverse, 200, "OK");
    EXPECT_EQ(jsonBody(reverse).at("data").at("value"), "reverse-static");

    const auto duplicateContentType = call(request(
        "GET", "/ok", {{"Content-Type", "text/plain"}, {"Content-Type", "bad/type"}}));
    expectEnvelope(duplicateContentType, 200, "OK");

    EXPECT_THROW(
        server_->registerRoute(
            HttpMethod::get, "/too-late", {},
            [](const HttpRequest& req) { return success(req); }),
        std::logic_error);
}

TEST_F(HttpRuntimeTest, AppliesContentTypeBearerAndResponseGuards) {
    expectEnvelope(call(request("POST", "/json", {}, "{}")), 400, "INVALID_REQUEST");
    expectEnvelope(
        call(request(
            "POST", "/json",
            {{"Content-Type", "application/json"}, {"Authorization", "Bearer  bad"}},
            "{}")),
        400,
        "INVALID_REQUEST");

    const auto missing = call(request(
        "POST", "/json", {{"Content-Type", "application/json"}}, "{}"));
    expectEnvelope(missing, 200, "OK");
    EXPECT_EQ(jsonBody(missing).at("data").at("value"), "missing");

    const auto valid = call(request(
        "POST", "/json",
        {{"Content-Type", "application/json"}, {"Authorization", "bearer good-token"}},
        "{}"));
    expectEnvelope(valid, 200, "OK");
    EXPECT_EQ(jsonBody(valid).at("data").at("value"), "good-token");

    const auto redirect = call(request("GET", "/redirect"));
    expectEnvelope(redirect, 500, "INTERNAL_ERROR");
    EXPECT_EQ(redirect.headers.count("location"), 0U);
    expectEnvelope(call(request("GET", "/large-json")), 500, "INTERNAL_ERROR");
    expectEnvelope(call(request("GET", "/throw")), 500, "INTERNAL_ERROR");
    expectEnvelope(call(request("GET", "/image-too-large")), 500, "INTERNAL_ERROR");
    EXPECT_FALSE(oversizedImageProducerCalled_->load());

    const auto spoofed = call(request(
        "GET", "/ok?secret-query", {{"X-Request-Id", "11111111-1111-4111-8111-111111111111"}}));
    expectEnvelope(spoofed, 200, "OK");
    EXPECT_NE(
        jsonBody(spoofed).at("requestId").get<std::string>(),
        "11111111-1111-4111-8111-111111111111");

    const auto entries = accessLog_->entries();
    EXPECT_TRUE(std::none_of(entries.begin(), entries.end(), [](const AccessLogEntry& entry) {
        const auto visible = entry.method + entry.routeTemplate + entry.code + entry.requestId;
        return visible.find("secret-query") != std::string::npos ||
               visible.find("sensitive controller failure") != std::string::npos ||
               visible.find("11111111-1111-4111-8111-111111111111") != std::string::npos;
    }));
}

TEST_F(HttpRuntimeTest, ReturnsJsonForMalformedRequestsAndRawSizeLimits) {
    expectEnvelope(
        call("GET /ok HTTP/1.1\r\nHost localhost\r\nConnection: close\r\n\r\n"),
        400,
        "INVALID_REQUEST");
    expectEnvelope(
        call("G?T /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"),
        400,
        "INVALID_REQUEST");

    const std::string exactUrl = "/" + std::string(http::protocol::kMaximumRawUrlBytes - 1U, 'a');
    expectEnvelope(call(request("GET", exactUrl)), 404, "ROUTE_NOT_FOUND");
    expectEnvelope(call(request("GET", exactUrl + "a")), 413, "REQUEST_TOO_LARGE");

    const std::string prefix = "GET /ok HTTP/1.1\r\nHost: localhost\r\nX-Fill: ";
    const std::string suffix = "\r\nConnection: close\r\n\r\n";
    ASSERT_LT(prefix.size() + suffix.size(), http::protocol::kMaximumRequestHeadBytes);
    const std::string exactHead = prefix +
        std::string(http::protocol::kMaximumRequestHeadBytes - prefix.size() - suffix.size(), 'h') +
        suffix;
    expectEnvelope(call(exactHead), 200, "OK");
    const auto oversizedHead = prefix +
        std::string(http::protocol::kMaximumRequestHeadBytes - prefix.size() - suffix.size() + 1U, 'h') +
        suffix;
    expectEnvelope(call(oversizedHead), 413, "REQUEST_TOO_LARGE");
}

TEST_F(HttpRuntimeTest, EnforcesContentLengthAndChunkedBodyLimitsDuringReceive) {
    const std::string exactJson(http::protocol::kMaximumJsonBytes, ' ');
    expectEnvelope(
        call(request(
            "POST", "/json", {{"Content-Type", "application/json"}}, exactJson)),
        200,
        "OK");

    const auto expectTooLarge = call(request(
        "POST", "/json",
        {{"Content-Type", "application/json"},
         {"Content-Length", std::to_string(http::protocol::kMaximumJsonBytes + 1U)},
         {"Expect", "100-continue"}}));
    expectEnvelope(expectTooLarge, 413, "REQUEST_TOO_LARGE");
    EXPECT_EQ(expectTooLarge.raw.find("100 Continue"), std::string::npos);

    std::ostringstream chunkSize;
    chunkSize << std::hex << (http::protocol::kMaximumJsonBytes + 1U);
    const std::string chunkedBody = chunkSize.str() + "\r\n" +
        std::string(http::protocol::kMaximumJsonBytes + 1U, 'x') + "\r\n0\r\n\r\n";
    expectEnvelope(
        call(request(
            "POST", "/json",
            {{"Content-Type", "application/json"}, {"Transfer-Encoding", "chunked"}},
            chunkedBody)),
        413,
        "REQUEST_TOO_LARGE");

    const auto invalidMultipartPrefix = call(request(
        "POST", "/multipart",
        {{"Content-Type", "multipart/form-datajunk; boundary=abc"},
         {"Content-Length", std::to_string(http::protocol::kMaximumJsonBytes + 1U)}}));
    expectEnvelope(invalidMultipartPrefix, 413, "REQUEST_TOO_LARGE");

    const auto duplicateMultipart = call(request(
        "POST", "/multipart",
        {{"Content-Type", "multipart/form-data; boundary=abc"},
         {"Content-Type", "application/json"},
         {"Content-Length", std::to_string(http::protocol::kMaximumJsonBytes + 1U)}}));
    expectEnvelope(duplicateMultipart, 413, "REQUEST_TOO_LARGE");

    const auto multipartTooLarge = call(request(
        "POST", "/multipart",
        {{"Content-Type", "multipart/form-data; boundary=abc"},
         {"Content-Length", std::to_string(http::protocol::kMaximumMultipartBytes + 1U)},
         {"Expect", "100-continue"}}));
    expectEnvelope(multipartTooLarge, 413, "REQUEST_TOO_LARGE");
    EXPECT_EQ(multipartTooLarge.raw.find("100 Continue"), std::string::npos);

    const std::string exactMultipart(http::protocol::kMaximumMultipartBytes, 'm');
    expectEnvelope(
        call(request(
            "POST", "/multipart",
            {{"Content-Type", "multipart/form-data; boundary=abc"}},
            exactMultipart)),
        200,
        "OK");
}

TEST_F(HttpRuntimeTest, ReturnsJsonForContentLengthShorterThanDeclaredOnHalfClose) {
    const auto raw = request(
        "POST", "/json",
        {{"Content-Type", "application/json"}, {"Content-Length", "5"}},
        "{}");
    expectEnvelope(parseResponse(exchangeHalfClosed(port_, raw)), 400, "INVALID_REQUEST");
}

TEST_F(HttpRuntimeTest, StreamsExactBodiesAndAbortsLengthOrProducerFailures) {
    const auto image = call(request("GET", "/image"));
    ASSERT_EQ(image.status, 200);
    EXPECT_TRUE(image.complete);
    EXPECT_EQ(image.headers.at("content-type"), "image/jpeg");
    EXPECT_EQ(image.body, "hello");

    const auto shortImage = call(request("GET", "/image-short"));
    EXPECT_EQ(shortImage.status, 200);
    EXPECT_FALSE(shortImage.complete);
    EXPECT_EQ(shortImage.body, "hey");

    const auto longImage = call(request("GET", "/image-long"));
    EXPECT_EQ(longImage.status, 200);
    EXPECT_FALSE(longImage.complete);
    EXPECT_TRUE(longImage.body.empty());

    const auto csv = call(request("GET", "/csv"));
    ASSERT_EQ(csv.status, 200);
    EXPECT_TRUE(csv.complete);
    EXPECT_EQ(csv.headers.at("content-type"), "text/csv; charset=utf-8");
    EXPECT_EQ(csv.body, "\xEF\xBB\xBF" "a,b\n");

    const auto emptyCsv = call(request("GET", "/csv-empty"));
    EXPECT_EQ(emptyCsv.status, 200);
    EXPECT_TRUE(emptyCsv.complete);
    EXPECT_TRUE(emptyCsv.body.empty());

    const auto aborted = call(request("GET", "/csv-abort"));
    EXPECT_EQ(aborted.status, 200);
    EXPECT_FALSE(aborted.complete);
    EXPECT_EQ(aborted.body, "first\n");

    const auto entries = waitForAccessLog(accessLog_, [](const AccessLogEntry& entry) {
        return entry.routeTemplate == "/csv-abort" && entry.code == "INTERNAL_ERROR";
    });
    ASSERT_TRUE(entries.has_value());
    EXPECT_TRUE(std::none_of(entries->begin(), entries->end(), [](const AccessLogEntry& entry) {
        return entry.routeTemplate.find("sensitive") != std::string::npos ||
               entry.code.find("sensitive") != std::string::npos;
    }));
}

TEST_F(HttpRuntimeTest, SocketWriteFailureAbortsAndProducesOnlySanitizedLogFields) {
    sendAndResetAfterHeaders(
        port_, request("GET", "/csv-write-failure"), writeFailureGate_);

    const auto entries = waitForAccessLog(accessLog_, [](const AccessLogEntry& entry) {
        return entry.routeTemplate == "/csv-write-failure" &&
               entry.code == "INTERNAL_ERROR";
    });
    EXPECT_TRUE(entries.has_value());
}

std::string readLine(asio::ip::tcp::socket& socket, asio::streambuf& buffered) {
    asio::read_until(socket, buffered, "\r\n");
    std::istream input(&buffered);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

ParsedResponse readOne(asio::ip::tcp::socket& socket, asio::streambuf& buffered) {
    const std::string statusLine = readLine(socket, buffered);
    std::string head = statusLine + "\r\n";
    std::map<std::string, std::string, std::less<>> headers;
    for (;;) {
        const auto line = readLine(socket, buffered);
        head += line + "\r\n";
        if (line.empty()) {
            break;
        }
        const auto colon = line.find(':');
        headers[lowercase(line.substr(0U, colon))] = trim(line.substr(colon + 1U));
    }
    std::string bodyWire;
    if (headers.count("content-length") != 0U) {
        const auto length = static_cast<std::size_t>(std::stoull(headers.at("content-length")));
        while (buffered.size() < length) {
            asio::read(socket, buffered, asio::transfer_at_least(length - buffered.size()));
        }
        std::istream input(&buffered);
        std::string body(length, '\0');
        input.read(body.data(), static_cast<std::streamsize>(length));
        bodyWire = std::move(body);
    } else {
        for (;;) {
            const auto sizeLine = readLine(socket, buffered);
            bodyWire += sizeLine + "\r\n";
            std::size_t size = 0U;
            std::istringstream parser(sizeLine);
            parser >> std::hex >> size;
            while (buffered.size() < size + 2U) {
                asio::read(socket, buffered, asio::transfer_at_least(size + 2U - buffered.size()));
            }
            std::istream input(&buffered);
            std::string chunk(size + 2U, '\0');
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            bodyWire += chunk;
            if (size == 0U) {
                break;
            }
        }
    }
    return parseResponse(head + bodyWire);
}

TEST_F(HttpRuntimeTest, CompletedStreamAllowsNextBufferedResponseOnSameConnection) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::make_address("127.0.0.1"), port_});
    asio::streambuf buffered;

    const std::string firstRequest =
        "GET /csv HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    asio::write(socket, asio::buffer(firstRequest));
    const auto first = readOne(socket, buffered);
    ASSERT_EQ(first.status, 200);
    ASSERT_TRUE(first.complete);

    const std::string secondRequest =
        "GET /ok HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    asio::write(socket, asio::buffer(secondRequest));
    const auto second = readOne(socket, buffered);
    SCOPED_TRACE(second.raw);
    expectEnvelope(second, 200, "OK");
    EXPECT_EQ(second.headers.count("content-length"), 1U);
}

TEST(HttpRegistrarTest, RejectsAmbiguousAndRepeatedParameterTemplates) {
    http::server::HttpServer server(0U, std::make_shared<SequentialRequestIds>());
    server.registerRoute(HttpMethod::get, "/items/{id}", {}, [](const HttpRequest& req) {
        return success(req);
    });
    EXPECT_THROW(
        server.registerRoute(
            HttpMethod::get, "/items/{other}", {},
            [](const HttpRequest& req) { return success(req); }),
        std::invalid_argument);
    EXPECT_THROW(
        server.registerRoute(
            HttpMethod::post, "/items/{id}/{id}", {},
            [](const HttpRequest& req) { return success(req); }),
        std::invalid_argument);
    server.registerRoute(
        HttpMethod::get, "/overlap/{id}/details", {},
        [](const HttpRequest& req) { return success(req); });
    EXPECT_THROW(
        server.registerRoute(
            HttpMethod::get, "/overlap/fixed/{tail}", {},
            [](const HttpRequest& req) { return success(req); }),
        std::invalid_argument);
}

TEST(HttpServerLifecycleTest, OccupiedPortPropagatesStartupExceptionWithoutHanging) {
    asio::io_context context;
    asio::ip::tcp::acceptor occupied(
        context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0U));
    http::server::HttpServer server(
        occupied.local_endpoint().port(), std::make_shared<SequentialRequestIds>());
    auto running = std::async(std::launch::async, [&server] { server.run(); });

    EXPECT_THROW(server.waitUntilStarted(), std::exception);
    EXPECT_THROW(running.get(), std::exception);
}

}  // namespace
}  // namespace ocrservice
