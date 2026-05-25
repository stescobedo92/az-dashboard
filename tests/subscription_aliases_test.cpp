#include "az_dashboard/subscription_aliases.hpp"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

TEST(SubscriptionAliasStoreTest, PersistsAndResolvesAliases) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-alias-test" / "aliases.json";
  std::filesystem::remove_all(path.parent_path());
  const auto store = azdash::SubscriptionAliasStore(path);

  store.set("prod", "2fedd50d-dec4-4920-b394-3eca4ae86032");

  ASSERT_TRUE(store.resolve("prod").has_value());
  EXPECT_EQ(*store.resolve("prod"), "2fedd50d-dec4-4920-b394-3eca4ae86032");
  EXPECT_FALSE(store.resolve("missing").has_value());
  ASSERT_EQ(store.list().size(), 1);
  EXPECT_EQ(store.list()[0].alias, "prod");
}

TEST(SubscriptionAliasStoreTest, RejectsUnsafeAliasNames) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-alias-test-invalid" / "aliases.json";
  std::filesystem::remove_all(path.parent_path());
  const auto store = azdash::SubscriptionAliasStore(path);

  EXPECT_THROW(store.set("../prod", "sub-id"), std::invalid_argument);
  EXPECT_THROW(store.set("", "sub-id"), std::invalid_argument);
}

TEST(SubscriptionAliasStoreTest, RemovesExistingAliases) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-alias-test-remove" / "aliases.json";
  std::filesystem::remove_all(path.parent_path());
  const auto store = azdash::SubscriptionAliasStore(path);

  store.set("prod", "sub-id");

  EXPECT_TRUE(store.remove("prod"));
  EXPECT_FALSE(store.remove("prod"));
  EXPECT_FALSE(store.resolve("prod").has_value());
}

} // namespace
