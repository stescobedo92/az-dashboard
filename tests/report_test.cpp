#include "az_dashboard/report.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>

namespace {

TEST(ReportTest, ResolveReportPathUsesDefaultNameForDirectory) {
  const auto path = azdash::resolve_report_path("reports", "azdash-cost.pdf");

  EXPECT_EQ(path.filename().string(), "azdash-cost.pdf");
  EXPECT_EQ(path.parent_path().filename().string(), "reports");
  EXPECT_TRUE(path.is_absolute());
}

TEST(ReportTest, ResolveReportPathUsesCurrentDirectoryForDefaultRequest) {
  const auto path = azdash::resolve_report_path("", "azdash-cost.pdf");

  EXPECT_EQ(path, std::filesystem::current_path().lexically_normal() / "azdash-cost.pdf");
}

TEST(ReportTest, ResolveReportPathRejectsUnsafePaths) {
  EXPECT_THROW((void)azdash::resolve_report_path("..", "azdash-cost.pdf"), std::invalid_argument);
  EXPECT_THROW((void)azdash::resolve_report_path("reports/../cost.pdf", "azdash-cost.pdf"), std::invalid_argument);
  EXPECT_THROW((void)azdash::resolve_report_path("/azdash-outside.pdf", "azdash-cost.pdf"), std::invalid_argument);
}

TEST(ReportTest, ResolveReportPathRejectsSymlinkComponents) {
  const auto target = std::filesystem::temp_directory_path();
  const auto link = std::filesystem::current_path() / "azdash-test-report-link";
  std::filesystem::remove(link);

  std::error_code error;
  std::filesystem::create_directory_symlink(target, link, error);
  if (error) {
    GTEST_SKIP() << "directory symlink creation is unavailable: " << error.message();
  }

  EXPECT_THROW((void)azdash::resolve_report_path(link.filename().string(), "azdash-cost.pdf"), std::invalid_argument);
  std::filesystem::remove(link);
}

TEST(ReportTest, ResolveReportPathRejectsUnsafeDefaultName) {
  EXPECT_THROW((void)azdash::resolve_report_path("", ""), std::invalid_argument);
  EXPECT_THROW((void)azdash::resolve_report_path("", "../azdash-cost.pdf"), std::invalid_argument);
}

TEST(ReportTest, WritesPdfHeader) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-test-report.pdf";
  const azdash::AccountInfo account{.subscription_id = "sub", .subscription_name = "Test", .tenant_id = "tenant"};

  azdash::write_cost_pdf(path, account, {azdash::CostComparisonRow{.service = "VM", .current = 1.0}});

  {
    std::ifstream file(path, std::ios::binary);
    std::string header(8, '\0');
    file.read(header.data(), static_cast<std::streamsize>(header.size()));

    EXPECT_EQ(header, "%PDF-1.4");
  }
  std::filesystem::remove(path);
}

TEST(ReportTest, EscapesPdfTextDelimiters) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-test-escaped-report.pdf";
  const azdash::AccountInfo account{
      .subscription_id = "sub\\1", .subscription_name = "Test (Prod)", .tenant_id = "tenant)1"};

  azdash::write_cost_pdf(path, account, {azdash::CostComparisonRow{.service = "VM (east)\\primary", .current = 1.0}});

  {
    std::ifstream file(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("Test \\(Prod\\)"), std::string::npos);
    EXPECT_NE(content.find("sub\\\\1"), std::string::npos);
    EXPECT_NE(content.find("tenant\\)1"), std::string::npos);
    EXPECT_NE(content.find("VM \\(east\\)\\\\primary"), std::string::npos);
  }
  std::filesystem::remove(path);
}

TEST(ReportTest, WritePdfRejectsSymlinkDestination) {
  const auto target = std::filesystem::temp_directory_path() / "azdash-test-report-target.pdf";
  const auto link = std::filesystem::temp_directory_path() / "azdash-test-report-link.pdf";
  std::filesystem::remove(target);
  std::filesystem::remove(link);
  {
    std::ofstream file(target, std::ios::binary);
    file << "%PDF";
  }

  std::error_code error;
  std::filesystem::create_symlink(target, link, error);
  if (error) {
    std::filesystem::remove(target);
    GTEST_SKIP() << "file symlink creation is unavailable: " << error.message();
  }

  const azdash::AccountInfo account{.subscription_id = "sub", .subscription_name = "Test", .tenant_id = "tenant"};
  EXPECT_THROW(azdash::write_cost_pdf(link, account, {}), std::runtime_error);

  std::filesystem::remove(link);
  std::filesystem::remove(target);
}

TEST(ReportTest, MultiPagePdfGeneratesMultiplePagesWithoutDroppingRows) {
  const auto path = std::filesystem::temp_directory_path() / "azdash-test-multipage-report.pdf";
  const azdash::AccountInfo account{.subscription_id = "sub-123", .subscription_name = "Enterprise Prod", .tenant_id = "tenant-456"};

  std::vector<azdash::CostComparisonRow> rows;
  rows.reserve(120);
  for (int i = 0; i < 120; ++i) {
    rows.push_back({
        .service = "Enterprise-Service-" + std::to_string(i),
        .previous = static_cast<double>(i * 10),
        .current = static_cast<double>(i * 12),
        .delta = static_cast<double>(i * 2),
        .delta_percent = 20.0,
    });
  }

  azdash::write_cost_pdf(path, account, rows);

  {
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Verify it is a valid PDF header
    EXPECT_EQ(content.substr(0, 8), "%PDF-1.4");

    // Verify multiple pages are created (120 rows plus headers = 3 pages)
    EXPECT_NE(content.find("/Count 3"), std::string::npos);
    EXPECT_NE(content.find("Page 1 of 3"), std::string::npos);
    EXPECT_NE(content.find("Page 2 of 3"), std::string::npos);
    EXPECT_NE(content.find("Page 3 of 3"), std::string::npos);

    // Verify that first, middle, and LAST row are all preserved and not dropped!
    EXPECT_NE(content.find("Enterprise-Service-0"), std::string::npos);
    EXPECT_NE(content.find("Enterprise-Service-60"), std::string::npos);
    EXPECT_NE(content.find("Enterprise-Service-119"), std::string::npos);
  }

  std::filesystem::remove(path);
}

} // namespace
