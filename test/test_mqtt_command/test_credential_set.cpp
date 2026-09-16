#include <gtest/gtest.h>

#include <helpers/MqttCredentialSet.h>

using namespace mqtt_credential_set;

TEST(MqttCredentialSet, IdentifiesCredentialKeys) {
  EXPECT_TRUE(isCredentialKey("username"));
  EXPECT_TRUE(isCredentialKey("password"));
  EXPECT_TRUE(isCredentialKey("1.username"));
  EXPECT_TRUE(isCredentialKey("6.password"));
  EXPECT_FALSE(isCredentialKey("uri"));
  EXPECT_FALSE(isCredentialKey("1.uri"));
  EXPECT_FALSE(isCredentialKey("wifi.pass"));
  EXPECT_FALSE(isCredentialKey("2.neighbor.interval"));
  EXPECT_FALSE(isCredentialKey("client.version"));
  EXPECT_FALSE(isCredentialKey(""));
}

TEST(MqttCredentialSet, LiteralQuotesNormalizeToBlank) {
  char quoted_empty[] = "\"\"";
  EXPECT_STREQ(normalizeCredentialValue(quoted_empty), "");

  char quoted_text[] = "\"x\"";
  EXPECT_STREQ(normalizeCredentialValue(quoted_text), "\"x\"");

  char plain[] = "secret";
  EXPECT_STREQ(normalizeCredentialValue(plain), "secret");

  char already_blank[] = "";
  EXPECT_STREQ(normalizeCredentialValue(already_blank), "");

  char lone_quote[] = "\"";
  EXPECT_STREQ(normalizeCredentialValue(lone_quote), "\"");
}

TEST(MqttCredentialSet, NullValueIsBlank) {
  EXPECT_STREQ(normalizeCredentialValue(nullptr), "");
}
