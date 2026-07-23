#include "az_dashboard/history.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>

namespace {

TEST(CostHistoryStoreTest, AppendsAndListsSnapshotsInOrder) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-history-test" / "cost-history.json";
  std::filesystem::remove_all(path.parent_path());
  const auto store = azdash::CostHistoryStore(path);

  store.append({.timestamp = "2026-07-01T10:00:00Z",
                .subscription = "sub-1",
                .total = 10.0,
                .services = {{.service = "VM", .cost = 10.0}}});
  store.append({.timestamp = "2026-07-02T10:00:00Z", .subscription = "sub-1", .total = 12.5});

  const auto snapshots = store.list();

  ASSERT_EQ(snapshots.size(), 2);
  EXPECT_EQ(snapshots[0].timestamp, "2026-07-01T10:00:00Z");
  EXPECT_EQ(snapshots[0].subscription, "sub-1");
  EXPECT_DOUBLE_EQ(snapshots[0].total, 10.0);
  ASSERT_EQ(snapshots[0].services.size(), 1);
  EXPECT_EQ(snapshots[0].services[0].service, "VM");
  EXPECT_DOUBLE_EQ(snapshots[0].services[0].cost, 10.0);
  EXPECT_EQ(snapshots[1].timestamp, "2026-07-02T10:00:00Z");
  EXPECT_DOUBLE_EQ(snapshots[1].total, 12.5);
}

TEST(CostHistoryStoreTest, ListReturnsEmptyWhenFileMissing) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-history-test-missing" / "cost-history.json";
  std::filesystem::remove_all(path.parent_path());
  const auto store = azdash::CostHistoryStore(path);

  EXPECT_TRUE(store.list().empty());
}

TEST(CostHistoryStoreTest, ListThrowsOnCorruptStore) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-history-test-corrupt" / "cost-history.json";
  std::filesystem::remove_all(path.parent_path());
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream file(path);
    file << "not json";
  }
  const auto store = azdash::CostHistoryStore(path);

  EXPECT_THROW((void)store.list(), std::runtime_error);
}

} // namespace
