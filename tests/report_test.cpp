#include "az_dashboard/report.hpp"

#include <fstream>
#include <gtest/gtest.h>

namespace {

TEST(ReportTest, ResolveReportPathUsesDefaultNameForDirectory) {
  const auto path = azdash::resolve_report_path("reports", "azdash-cost.pdf");

  EXPECT_EQ(path.filename().string(), "azdash-cost.pdf");
}

TEST(ReportTest, WritesPdfHeader) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-test-report.pdf";
  const azdash::AccountInfo account{.subscription_id = "sub", .subscription_name = "Test", .tenant_id = "tenant"};

  azdash::write_cost_pdf(path, account, {azdash::CostComparisonRow{.service = "VM", .current = 1.0}});

  std::ifstream file(path, std::ios::binary);
  std::string header(8, '\0');
  file.read(header.data(), static_cast<std::streamsize>(header.size()));

  EXPECT_EQ(header, "%PDF-1.4");
  std::filesystem::remove(path);
}

} // namespace
