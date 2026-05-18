#include "az_dashboard/report.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace azdash {
namespace {

auto money(double value) -> std::string {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << value;
  return out.str();
}

auto escape_pdf_text(const std::string& value) -> std::string {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto c : value) {
    if (c == '(' || c == ')' || c == '\\') {
      escaped += '\\';
    }
    escaped += c;
  }
  return escaped;
}

class PdfWriter {
public:
  /**
   * @brief Adds a text line to the PDF page.
   * @param line Text content.
   */
  void line(const std::string& line) {
    if (y_ < 48) {
      return;
    }
    content_ << "BT /F1 10 Tf 48 " << y_ << " Td (" << escape_pdf_text(line) << ") Tj ET\n";
    y_ -= 14;
  }

  /**
   * @brief Writes the PDF to disk.
   * @param path Destination file.
   */
  void save(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path().empty() ? "." : path.parent_path());

    std::ostringstream pdf;
    std::vector<std::streamoff> offsets;
    auto object = [&](int id, const std::string& body) {
      offsets.push_back(static_cast<std::streamoff>(pdf.tellp()));
      pdf << id << " 0 obj\n" << body << "\nendobj\n";
    };

    pdf << "%PDF-1.4\n";
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    object(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>");
    object(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const auto stream = content_.str();
    object(5, "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream");

    const auto xref = static_cast<std::streamoff>(pdf.tellp());
    pdf << "xref\n0 6\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
      pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    }
    pdf << "trailer << /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

    std::ofstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("failed to open report path: " + path.string());
    }
    file << pdf.str();
  }

private:
  int y_{744};
  std::ostringstream content_;
};

auto report_header(const AccountInfo& account, const std::string& title) -> PdfWriter {
  PdfWriter pdf;
  pdf.line("azdash - " + title);
  pdf.line("Subscription: " + account.subscription_name + " (" + account.subscription_id + ")");
  pdf.line("Tenant: " + account.tenant_id);
  pdf.line("");
  return pdf;
}

} // namespace

auto resolve_report_path(const std::string& requested, const std::string& default_name) -> std::filesystem::path {
  if (requested.empty() || requested == "DEFAULT") {
    return std::filesystem::current_path() / default_name;
  }
  auto path = std::filesystem::path(requested);
  if (path.has_extension()) {
    return path;
  }
  return path / default_name;
}

void write_cost_pdf(const std::filesystem::path& path,
                    const AccountInfo& account,
                    const std::vector<CostComparisonRow>& rows) {
  auto pdf = report_header(account, "Cost comparison");
  pdf.line("Service | Previous | Current | Delta | Delta %");
  for (const auto& row : rows) {
    pdf.line(row.service + " | " + money(row.previous) + " | " + money(row.current) + " | " + money(row.delta) +
             " | " + money(row.delta_percent) + "%");
  }
  pdf.save(path);
}

void write_trend_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<MonthCost>& rows) {
  auto pdf = report_header(account, "Six-month trend");
  pdf.line("Month | Total");
  for (const auto& row : rows) {
    pdf.line(row.month + " | " + money(row.total));
  }
  pdf.save(path);
}

void write_waste_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<WasteFinding>& rows) {
  auto pdf = report_header(account, "Waste findings");
  pdf.line("Check | Type | Name | Savings | Recommendation");
  for (const auto& row : rows) {
    pdf.line(row.check + " | " + row.resource_type + " | " + row.name + " | " +
             money(row.estimated_monthly_savings) + " | " + row.recommendation);
  }
  pdf.save(path);
}

} // namespace azdash
