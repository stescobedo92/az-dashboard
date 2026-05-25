#include "az_dashboard/report.hpp"

#include <cerrno>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace azdash {
namespace {

class NumberFormatter {
public:
  [[nodiscard]] static auto money(double value) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
  }
};

class PdfTextEscaper {
public:
  [[nodiscard]] auto escape(const std::string& value) const -> std::string {
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
};

class ReportPathPolicy {
public:
  [[nodiscard]] auto resolve(const std::string& requested, const std::string& default_name) const
      -> std::filesystem::path {
    const auto default_path = validate_default_name(default_name);
    const auto cwd = std::filesystem::current_path().lexically_normal();

    if (requested.empty() || requested == "DEFAULT") {
      return (cwd / default_path).lexically_normal();
    }

    const auto requested_path = std::filesystem::path(requested);
    const auto target = requested_path.has_extension() ? requested_path : requested_path / default_path;
    if (target.empty()) {
      throw std::invalid_argument("report path is required");
    }
    if (contains_parent_traversal(target)) {
      throw std::invalid_argument("report path must not contain parent directory traversal");
    }

    const auto absolute_target = (target.is_absolute() ? target : cwd / target).lexically_normal();
    if (!is_within_directory(absolute_target, cwd)) {
      throw std::invalid_argument("report path must stay within the current working directory");
    }
    if (contains_symlink_component(absolute_target, cwd)) {
      throw std::invalid_argument("report path must not include symbolic links");
    }
    return absolute_target;
  }

private:
  [[nodiscard]] static auto validate_default_name(const std::string& default_name) -> std::filesystem::path {
    if (default_name.empty()) {
      throw std::invalid_argument("report default file name is required");
    }

    const std::filesystem::path default_path(default_name);
    if (default_path.is_absolute() || default_path.has_parent_path() || contains_parent_traversal(default_path)) {
      throw std::invalid_argument("report default file name must be a simple file name");
    }
    return default_path;
  }

  [[nodiscard]] static auto contains_parent_traversal(const std::filesystem::path& path) -> bool {
    for (const auto& part : path) {
      if (part == "..") {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] static auto is_within_directory(const std::filesystem::path& child,
                                                const std::filesystem::path& parent) -> bool {
    auto child_it = child.begin();
    auto parent_it = parent.begin();
    for (; parent_it != parent.end(); ++parent_it, ++child_it) {
      if (child_it == child.end() || *child_it != *parent_it) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] static auto contains_symlink_component(const std::filesystem::path& path,
                                                       const std::filesystem::path& root) -> bool {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() ||
        (!relative.empty() && *relative.begin() == std::filesystem::path(".."))) {
      return true;
    }

    auto current = root;
    std::error_code error;
    for (const auto& part : relative) {
      current /= part;
      const auto status = std::filesystem::symlink_status(current, error);
      if (error) {
        error.clear();
        continue;
      }
      if (std::filesystem::is_symlink(status)) {
        return true;
      }
    }
    return false;
  }
};

class PdfPage {
public:
  void line(const std::string& line) {
    if (y_ < 48) {
      return;
    }
    content_ << "BT /F1 10 Tf 48 " << y_ << " Td (" << escaper_.escape(line) << ") Tj ET\n";
    y_ -= 14;
  }

  [[nodiscard]] auto content() const -> std::string {
    return content_.str();
  }

private:
  int y_{744};
  std::ostringstream content_;
  PdfTextEscaper escaper_;
};

class PdfFileWriter {
public:
  void save(const std::filesystem::path& path, const PdfPage& page) const {
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
    object(3,
           "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> "
           "/Contents 5 0 R >>");
    object(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
    const auto stream = page.content();
    object(5, "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "endstream");

    const auto xref = static_cast<std::streamoff>(pdf.tellp());
    pdf << "xref\n0 6\n0000000000 65535 f \n";
    for (const auto offset : offsets) {
      pdf << std::setw(10) << std::setfill('0') << offset << " 00000 n \n";
    }
    pdf << "trailer << /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

    write_file_without_following_symlink(path, pdf.str());
  }

private:
  static void write_file_without_following_symlink(const std::filesystem::path& path, const std::string& content) {
    std::error_code status_error;
    const auto existing_status = std::filesystem::symlink_status(path, status_error);
    if (!status_error && std::filesystem::is_symlink(existing_status)) {
      throw std::runtime_error("report path must not be a symbolic link: " + path.string());
    }

#ifdef _WIN32
    const auto native_path = path.string();
    HANDLE handle = ::CreateFileA(native_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("failed to open report path: " + path.string());
    }

    DWORD attributes = ::GetFileAttributesA(native_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      (void)::CloseHandle(handle);
      throw std::runtime_error("report path must not be a symbolic link: " + path.string());
    }

    auto remaining = content.size();
    auto cursor = content.data();
    while (remaining > 0) {
      const auto chunk = remaining > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())
                             ? std::numeric_limits<DWORD>::max()
                             : static_cast<DWORD>(remaining);
      DWORD written = 0;
      if (::WriteFile(handle, cursor, chunk, &written, nullptr) == 0 || written == 0) {
        (void)::CloseHandle(handle);
        throw std::runtime_error("failed to write report path: " + path.string());
      }
      cursor += written;
      remaining -= written;
    }
    (void)::CloseHandle(handle);
#else
#ifdef O_CLOEXEC
    constexpr auto close_on_exec = O_CLOEXEC;
#else
    constexpr auto close_on_exec = 0;
#endif
#ifdef O_NOFOLLOW
    constexpr auto no_follow = O_NOFOLLOW;
#else
    constexpr auto no_follow = 0;
#endif
    const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | close_on_exec | no_follow, 0600);
    if (fd == -1) {
      const auto reason = errno == ELOOP ? "report path must not be a symbolic link: "
                                         : "failed to open report path: ";
      throw std::runtime_error(std::string(reason) + path.string() + " (" + std::strerror(errno) + ")");
    }

    auto close_fd = [fd] { (void)::close(fd); };
    auto remaining = content.size();
    auto cursor = content.data();
    while (remaining > 0) {
      const auto written = ::write(fd, cursor, remaining);
      if (written == -1) {
        if (errno == EINTR) {
          continue;
        }
        close_fd();
        throw std::runtime_error("failed to write report path: " + path.string() + " (" + std::strerror(errno) + ")");
      }
      if (written == 0) {
        close_fd();
        throw std::runtime_error("failed to write report path: " + path.string());
      }
      cursor += written;
      remaining -= static_cast<std::size_t>(written);
    }
    close_fd();
#endif
  }
};

class PdfReportSection {
public:
  virtual ~PdfReportSection() = default;
  [[nodiscard]] virtual auto title() const -> std::string = 0;
  virtual void write(PdfPage& page) const = 0;
};

class CostReportSection final : public PdfReportSection {
public:
  explicit CostReportSection(const std::vector<CostComparisonRow>& rows) : rows_(rows) {}

  [[nodiscard]] auto title() const -> std::string override {
    return "Cost comparison";
  }

  void write(PdfPage& page) const override {
    page.line("Service | Previous | Current | Delta | Delta %");
    for (const auto& row : rows_) {
      page.line(row.service + " | " + NumberFormatter::money(row.previous) + " | " +
                NumberFormatter::money(row.current) + " | " + NumberFormatter::money(row.delta) + " | " +
                NumberFormatter::money(row.delta_percent) + "%");
    }
  }

private:
  const std::vector<CostComparisonRow>& rows_;
};

class TrendReportSection final : public PdfReportSection {
public:
  explicit TrendReportSection(const std::vector<MonthCost>& rows) : rows_(rows) {}

  [[nodiscard]] auto title() const -> std::string override {
    return "Six-month trend";
  }

  void write(PdfPage& page) const override {
    page.line("Month | Total");
    for (const auto& row : rows_) {
      page.line(row.month + " | " + NumberFormatter::money(row.total));
    }
  }

private:
  const std::vector<MonthCost>& rows_;
};

class WasteReportSection final : public PdfReportSection {
public:
  explicit WasteReportSection(const std::vector<WasteFinding>& rows) : rows_(rows) {}

  [[nodiscard]] auto title() const -> std::string override {
    return "Waste findings";
  }

  void write(PdfPage& page) const override {
    page.line("Check | Type | Name | Savings | Recommendation");
    for (const auto& row : rows_) {
      page.line(row.check + " | " + row.resource_type + " | " + row.name + " | " +
                NumberFormatter::money(row.estimated_monthly_savings) + " | " + row.recommendation);
    }
  }

private:
  const std::vector<WasteFinding>& rows_;
};

void write_report_header(PdfPage& page, const AccountInfo& account, const std::string& title) {
  page.line("azdash - " + title);
  page.line("Subscription: " + account.subscription_name + " (" + account.subscription_id + ")");
  page.line("Tenant: " + account.tenant_id);
  page.line("");
}

void write_pdf_report(const std::filesystem::path& path, const AccountInfo& account, const PdfReportSection& section) {
  PdfPage page;
  write_report_header(page, account, section.title());
  section.write(page);
  PdfFileWriter{}.save(path, page);
}

} // namespace

auto resolve_report_path(const std::string& requested, const std::string& default_name) -> std::filesystem::path {
  return ReportPathPolicy{}.resolve(requested, default_name);
}

void write_cost_pdf(const std::filesystem::path& path,
                    const AccountInfo& account,
                    const std::vector<CostComparisonRow>& rows) {
  write_pdf_report(path, account, CostReportSection(rows));
}

void write_trend_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<MonthCost>& rows) {
  write_pdf_report(path, account, TrendReportSection(rows));
}

void write_waste_pdf(const std::filesystem::path& path,
                     const AccountInfo& account,
                     const std::vector<WasteFinding>& rows) {
  write_pdf_report(path, account, WasteReportSection(rows));
}

} // namespace azdash
