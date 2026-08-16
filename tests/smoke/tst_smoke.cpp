#include <array>
#include <cstdint>
#include <string>

#include <cppconn/driver.h>
#include <crow.h>
#include <crypt.h>
#include <gtest/gtest.h>
#include <mqtt/message.h>
#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>
#include <utf8proc.h>

TEST(Smoke, RuntimeDependenciesLinkAndRespond) {
    const nlohmann::json payload{{"status", "UP"}};
    const crow::response response{payload.dump()};
    EXPECT_EQ(response.code, 200);

    const cv::Mat pixel = cv::Mat::zeros(1, 1, CV_8UC1);
    EXPECT_EQ(pixel.total(), 1U);

    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    const std::string input = "ocrservice";
    ASSERT_NE(SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest.data()), nullptr);

    struct crypt_data crypt_context {};
    ASSERT_NE(crypt_r("smoke", "$6$ocrservice$", &crypt_context), nullptr);

    ASSERT_NE(get_driver_instance(), nullptr);
    EXPECT_FALSE(Ort::GetVersionString().empty());
    EXPECT_NE(utf8proc_version(), nullptr);

    const auto message = mqtt::make_message("plate/smoke", "ok");
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->get_payload_str(), "ok");

    spdlog::set_level(spdlog::level::off);
    spdlog::info("smoke");
}
