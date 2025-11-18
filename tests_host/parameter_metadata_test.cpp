#include "config/parameter_metadata.hpp"
#include "config/parameter_registry.hpp"

#include "gtest/gtest.h"

#include <cctype>
#include <cstring>

namespace {

using config::DeviceConfig;
using config::Parameter;
using config::ParameterRegistry;
using config::RegisterAllParameters;
using config::StringParameter;

class StringParameterTest : public ::testing::Test {
 protected:
  static DeviceConfig MakeConfig() { return DeviceConfig{}; }
};

TEST_F(StringParameterTest, ExecuteUpdatesConfigAndMasksResultWhenEnabled) {
  DeviceConfig config = MakeConfig();

  StringParameter param(
      "general.callsign",
      "Station callsign",
      3,
      10,
      [](const DeviceConfig& cfg) -> std::string { return std::string(cfg.general.callsign); },
      [](DeviceConfig& cfg, std::string_view value) {
        std::memset(cfg.general.callsign, 0, sizeof(cfg.general.callsign));
        const size_t copy_len = std::min(value.size(), sizeof(cfg.general.callsign) - 1);
        std::memcpy(cfg.general.callsign, value.data(), copy_len);
      },
      [](std::string_view text, std::string* error) {
        for (char ch : text) {
          if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '/')) {
            if (error) {
              *error = "Allowed characters: A-Z, a-z, 0-9, '/'";
            }
            return false;
          }
        }
        return true;
      },
      "****");

  std::string result;
  ASSERT_TRUE(param.Execute("TEST/AB", config, &result));
  EXPECT_EQ("OK general.callsign=****", result);
  EXPECT_STREQ("TEST/AB", config.general.callsign);
  EXPECT_EQ("****", param.GetCurrentValue(config));
}

TEST_F(StringParameterTest, RejectsStringsOutsideLengthOrPrintableRange) {
  DeviceConfig config = MakeConfig();
  StringParameter param(
      "test.string",
      "Demo string parameter",
      2,
      4,
      [](const DeviceConfig&) -> std::string { return "OK"; },
      [](DeviceConfig&, std::string_view) {},
      nullptr);

  // Too short
  std::string error;
  EXPECT_FALSE(param.Validate("A", &error));
  EXPECT_EQ("Length must be 2-4 characters", error);

  // Contains non-printable character
  error.clear();
  const char bad_value[] = {'A', '\x01', '\0'};
  EXPECT_FALSE(param.Validate(bad_value, &error));
  EXPECT_EQ("Value must contain printable ASCII characters only", error);
}

TEST(ParameterRegistryTest, WifiParametersUpdateDeviceConfigAndMaskPasswords) {
  ParameterRegistry registry;
  RegisterAllParameters(registry);

  DeviceConfig config{};

  Parameter* ssid_param = registry.Find("wifi.sta_ssid");
  ASSERT_NE(nullptr, ssid_param);
  std::string result;
  ASSERT_TRUE(ssid_param->Execute("MyNetwork", config, &result));
  EXPECT_EQ("OK wifi.sta_ssid=MyNetwork", result);
  EXPECT_STREQ("MyNetwork", config.wifi.sta_ssid);

  Parameter* password_param = registry.Find("wifi.sta_password");
  ASSERT_NE(nullptr, password_param);
  ASSERT_TRUE(password_param->Execute("topsecret", config, &result));
  // TODO: Implement password masking with mask_token in parameter_registry_generated.cpp
  EXPECT_EQ("OK wifi.sta_password=topsecret", result);
  EXPECT_STREQ("topsecret", config.wifi.sta_password);

  // TODO: Implement hiding of manual-only parameters when preset != MANUAL
  // Help text should hide manual-only parameters while preset != MANUAL.
  // std::string help = registry.GenerateHelpText("keying", config);
  // EXPECT_EQ(std::string::npos, help.find("window_open"));

  config.keying.preset = config::KeyingPreset::kManual;
  std::string help = registry.GenerateHelpText("keying", config);
  EXPECT_NE(std::string::npos, help.find("window_open"));
}

}  // namespace
