#include "az_dashboard/render.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <utility>

namespace azdash {
namespace {

class NumberFormatter {
public:
  [[nodiscard]] static auto money(double value) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
  }

  [[nodiscard]] static auto percent(double value) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value << "%";
    return out.str();
  }
};

void write_document(ftxui::Element document, std::ostream& out) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(document));
  ftxui::Render(screen, document);
  out << screen.ToString() << '\n';
}

[[nodiscard]] auto command_row(std::string command, std::string detail) -> ftxui::Element {
  return ftxui::hbox({
             ftxui::text("  "),
             ftxui::text(std::move(command)) | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
             ftxui::text("  "),
             ftxui::text(std::move(detail)) | ftxui::color(ftxui::Color::GrayLight),
         });
}

[[nodiscard]] auto panel_title(std::string title) -> ftxui::Element {
  return ftxui::text(std::move(title)) | ftxui::bold | ftxui::color(ftxui::Color::Green);
}

[[nodiscard]] auto progress_bar(double value, double max_value) -> std::string {
  constexpr auto width = 18;
  const auto ratio = max_value <= 0.0 ? 0.0 : std::clamp(value / max_value, 0.0, 1.0);
  const auto filled = static_cast<int>(ratio * width + 0.5);
  std::string bar{"["};
  bar.append(static_cast<std::size_t>(filled), '#');
  bar.append(static_cast<std::size_t>(width - filled), '-');
  bar += ']';
  return bar;
}

[[nodiscard]] auto max_abs_delta(const std::vector<CostComparisonRow>& rows) -> double {
  auto max_value = 0.0;
  for (const auto& row : rows) {
    max_value = std::max(max_value, std::abs(row.delta));
  }
  return max_value;
}

[[nodiscard]] auto max_total(const std::vector<MonthCost>& rows) -> double {
  auto max_value = 0.0;
  for (const auto& row : rows) {
    max_value = std::max(max_value, row.total);
  }
  return max_value;
}

[[nodiscard]] auto max_savings(const std::vector<WasteFinding>& rows) -> double {
  auto max_value = 0.0;
  for (const auto& row : rows) {
    max_value = std::max(max_value, row.estimated_monthly_savings);
  }
  return max_value;
}

class TerminalTableWriter {
public:
  void write(std::vector<std::vector<std::string>> rows, std::ostream& out, ftxui::Element footer = ftxui::text("")) const {
    auto table = ftxui::Table(padded(std::move(rows)));
    table.SelectAll().Border(ftxui::LIGHT);
    table.SelectAll().Decorate(ftxui::color(ftxui::Color::GrayLight));
    table.SelectRow(0).Decorate(ftxui::bold | ftxui::color(ftxui::Color::Cyan));
    table.SelectColumn(0).Decorate(ftxui::bold | ftxui::color(ftxui::Color::White));
    auto document = ftxui::vbox({
        ftxui::text("azdash") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
        ftxui::separator(),
        table.Render(),
        footer,
        ftxui::separator(),
        ftxui::hbox({
            ftxui::text("complete ") | ftxui::color(ftxui::Color::Green),
            ftxui::gauge(1.0F) | ftxui::color(ftxui::Color::Green),
        }),
    });
    write_document(document, out);
  }

private:
  [[nodiscard]] static auto padded(std::vector<std::vector<std::string>> rows) -> std::vector<std::vector<std::string>> {
    for (auto& row : rows) {
      for (auto& cell : row) {
        cell = " " + cell + " ";
      }
    }
    return rows;
  }
};

class JsonWriter {
public:
  void write(const nlohmann::json& payload, std::ostream& out) const {
    out << payload.dump(2) << '\n';
  }
};

class CsvCellEscaper {
public:
  void write(std::ostream& out, const std::string& value) const {
    const auto escaped_value = needs_formula_neutralization(value) ? "'" + value : value;
    const auto must_quote = escaped_value.find_first_of(",\"\n\r") != std::string::npos;
    if (!must_quote) {
      out << escaped_value;
      return;
    }
    out << '"';
    for (const auto c : escaped_value) {
      if (c == '"') {
        out << "\"\"";
      } else {
        out << c;
      }
    }
    out << '"';
  }

private:
  [[nodiscard]] static auto needs_formula_neutralization(const std::string& value) -> bool {
    if (value.empty()) {
      return false;
    }
    const auto first = value.front();
    return first == '=' || first == '+' || first == '-' || first == '@' || first == '\t' || first == '\r';
  }
};

class CsvRowWriter {
public:
  explicit CsvRowWriter(std::ostream& out) : out_(out) {}

  void escaped_cell(const std::string& value) {
    separator();
    escaper_.write(out_, value);
  }

  void raw_cell(std::string_view value) {
    separator();
    out_ << value;
  }

  void end() {
    out_ << '\n';
  }

private:
  void separator() {
    if (first_) {
      first_ = false;
      return;
    }
    out_ << ',';
  }

  std::ostream& out_;
  CsvCellEscaper escaper_;
  bool first_{true};
};

class CsvDocumentWriter {
public:
  explicit CsvDocumentWriter(std::ostream& out) : out_(out) {}

  void header(std::string_view value) {
    out_ << value << '\n';
  }

  template <typename WriteRow>
  void row(WriteRow write_row) {
    CsvRowWriter writer(out_);
    write_row(writer);
    writer.end();
  }

private:
  std::ostream& out_;
};

[[nodiscard]] auto markdown_cell(const std::string& value) -> std::string {
  std::string escaped;
  escaped.reserve(value.size());
  for (const auto c : value) {
    if (c == '|') {
      escaped += "\\|";
    } else if (c == '\n' || c == '\r') {
      if (!escaped.empty() && escaped.back() != ' ') {
        escaped += ' ';
      }
    } else {
      escaped += c;
    }
  }
  return escaped;
}

class MarkdownTableWriter {
public:
  void write(const std::vector<std::vector<std::string>>& rows,
             std::ostream& out,
             const std::vector<std::string>& footer_lines = {}) const {
    if (!rows.empty()) {
      write_row(rows.front(), out);
      write_separator(rows.front().size(), out);
      for (auto row = rows.begin() + 1; row != rows.end(); ++row) {
        write_row(*row, out);
      }
    }
    for (const auto& line : footer_lines) {
      out << '\n' << line << '\n';
    }
  }

private:
  static void write_row(const std::vector<std::string>& cells, std::ostream& out) {
    out << '|';
    for (const auto& cell : cells) {
      out << ' ' << markdown_cell(cell) << " |";
    }
    out << '\n';
  }

  static void write_separator(std::size_t columns, std::ostream& out) {
    out << '|';
    for (std::size_t index = 0; index < columns; ++index) {
      out << " --- |";
    }
    out << '\n';
  }
};

class HtmlTableWriter {
public:
  void write(std::string_view title,
             const std::vector<std::vector<std::string>>& rows,
             std::ostream& out,
             const std::vector<std::string>& footer_lines = {}) const {
    out << "<!DOCTYPE html>\n"
        << "<html lang=\"en\">\n"
        << "<head>\n"
        << "  <meta charset=\"UTF-8\">\n"
        << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        << "  <title>" << escape_html(title) << "</title>\n"
        << "  <style>\n"
        << "    :root {\n"
        << "      --bg: #0f172a;\n"
        << "      --surface: #1e293b;\n"
        << "      --surface-border: #334155;\n"
        << "      --text: #f8fafc;\n"
        << "      --text-muted: #94a3b8;\n"
        << "      --primary: #38bdf8;\n"
        << "      --success: #4ade80;\n"
        << "      --warning: #fbbf24;\n"
        << "      --danger: #f87171;\n"
        << "    }\n"
        << "    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
        << "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: var(--bg); color: var(--text); padding: 2rem; line-height: 1.5; }\n"
        << "    .container { max-width: 1200px; margin: 0 auto; }\n"
        << "    header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 2rem; padding-bottom: 1rem; border-bottom: 1px solid var(--surface-border); }\n"
        << "    h1 { font-size: 1.75rem; font-weight: 700; color: var(--primary); letter-spacing: -0.025em; }\n"
        << "    .badge { display: inline-block; padding: 0.25rem 0.75rem; border-radius: 9999px; font-size: 0.75rem; font-weight: 600; text-transform: uppercase; background: var(--surface-border); color: var(--text-muted); }\n"
        << "    .card { background: var(--surface); border: 1px solid var(--surface-border); border-radius: 0.75rem; overflow: hidden; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.1); margin-bottom: 1.5rem; }\n"
        << "    table { width: 100%; border-collapse: collapse; text-align: left; font-size: 0.875rem; }\n"
        << "    th { background: #162032; padding: 0.75rem 1rem; font-weight: 600; color: var(--primary); border-bottom: 1px solid var(--surface-border); text-transform: uppercase; font-size: 0.75rem; letter-spacing: 0.05em; }\n"
        << "    td { padding: 0.75rem 1rem; border-bottom: 1px solid var(--surface-border); }\n"
        << "    tr:last-child td { border-bottom: none; }\n"
        << "    tr:hover td { background: rgba(255, 255, 255, 0.02); }\n"
        << "    .footer { margin-top: 1rem; padding: 1rem; background: var(--surface); border-radius: 0.5rem; border: 1px solid var(--surface-border); font-size: 0.875rem; color: var(--text-muted); }\n"
        << "  </style>\n"
        << "</head>\n"
        << "<body>\n"
        << "  <div class=\"container\">\n"
        << "    <header>\n"
        << "      <h1>" << escape_html(title) << "</h1>\n"
        << "      <span class=\"badge\">azdash FinOps</span>\n"
        << "    </header>\n"
        << "    <div class=\"card\">\n"
        << "      <table>\n";

    if (!rows.empty()) {
      out << "        <thead>\n          <tr>\n";
      for (const auto& cell : rows.front()) {
        out << "            <th>" << escape_html(cell) << "</th>\n";
      }
      out << "          </tr>\n        </thead>\n        <tbody>\n";
      for (std::size_t i = 1; i < rows.size(); ++i) {
        out << "          <tr>\n";
        for (const auto& cell : rows[i]) {
          out << "            <td>" << escape_html(cell) << "</td>\n";
        }
        out << "          </tr>\n";
      }
      out << "        </tbody>\n";
    }

    out << "      </table>\n"
        << "    </div>\n";

    if (!footer_lines.empty()) {
      out << "    <div class=\"footer\">\n";
      for (const auto& line : footer_lines) {
        out << "      <p>" << escape_html(line) << "</p>\n";
      }
      out << "    </div>\n";
    }

    out << "  </div>\n"
        << "</body>\n"
        << "</html>\n";
  }

  static auto escape_html(std::string_view text) -> std::string {
    std::string escaped;
    escaped.reserve(text.size());
    for (char c : text) {
      switch (c) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += c; break;
      }
    }
    return escaped;
  }
};

class CostRowsView {
public:
  explicit CostRowsView(const std::vector<CostComparisonRow>& rows, double projected_total) : rows_(rows), projected_total_(projected_total) {}
  
  [[nodiscard]] auto footer() const -> ftxui::Element {
    if (projected_total_ > 0.0) {
      std::string label = " Projected EOM Total: ";
      if (!rows_.empty() && rows_.front().currency != "USD" && !rows_.front().currency.empty()) {
        label = " Projected EOM Total (" + rows_.front().currency + "): ";
      }
      return ftxui::hbox({
          ftxui::text(label) | ftxui::bold | ftxui::color(ftxui::Color::Yellow),
          ftxui::text(NumberFormatter::money(projected_total_)) | ftxui::bold | ftxui::color(ftxui::Color::White)
      });
    }
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      payload.push_back({
          {"service", row.service},
          {"previous", row.previous},
          {"current", row.current},
          {"delta", row.delta},
          {"deltaPercent", row.delta_percent},
          {"currency", row.currency},
      });
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("service,previous,current,delta,delta_percent");
    for (const auto& row : rows_) {
      csv.row([&row](CsvRowWriter& writer) {
        writer.escaped_cell(row.service);
        writer.raw_cell(NumberFormatter::money(row.previous));
        writer.raw_cell(NumberFormatter::money(row.current));
        writer.raw_cell(NumberFormatter::money(row.delta));
        writer.raw_cell(NumberFormatter::percent(row.delta_percent));
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    const auto max_delta = max_abs_delta(rows_);
    std::vector<std::vector<std::string>> rows{{"Service", "Previous", "Current", "Delta", "Delta %", "Delta Bar"}};
    for (const auto& row : rows_) {
      rows.push_back({row.service, NumberFormatter::money(row.previous), NumberFormatter::money(row.current),
                      NumberFormatter::money(row.delta), NumberFormatter::percent(row.delta_percent),
                      progress_bar(std::abs(row.delta), max_delta)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{{"Service", "Previous", "Current", "Delta", "Delta %"}};
    for (const auto& row : rows_) {
      rows.push_back({row.service, NumberFormatter::money(row.previous), NumberFormatter::money(row.current),
                      NumberFormatter::money(row.delta), NumberFormatter::percent(row.delta_percent)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    if (projected_total_ > 0.0) {
      return {"**Projected end-of-month total:** " + NumberFormatter::money(projected_total_)};
    }
    return {};
  }

private:
  const std::vector<CostComparisonRow>& rows_;
  double projected_total_{0.0};
};

class TrendRowsView {
public:
  explicit TrendRowsView(const std::vector<MonthCost>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      nlohmann::json services = nlohmann::json::array();
      for (const auto& service : row.services) {
        services.push_back({{"service", service.service}, {"cost", service.cost}, {"currency", service.currency}});
      }
      payload.push_back({{"month", row.month}, {"total", row.total}, {"currency", row.currency}, {"services", services}});
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("month,total");
    for (const auto& row : rows_) {
      csv.row([&row](CsvRowWriter& writer) {
        writer.escaped_cell(row.month);
        writer.raw_cell(NumberFormatter::money(row.total));
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    const auto max_value = max_total(rows_);
    std::vector<std::vector<std::string>> rows{{"Month", "Total", "Spend Bar"}};
    for (const auto& row : rows_) {
      rows.push_back({row.month, NumberFormatter::money(row.total), progress_bar(row.total, max_value)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{{"Month", "Total"}};
    for (const auto& row : rows_) {
      rows.push_back({row.month, NumberFormatter::money(row.total)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    return {};
  }

private:
  const std::vector<MonthCost>& rows_;
};

class WasteRowsView {
public:
  explicit WasteRowsView(const std::vector<WasteFinding>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      payload.push_back({
          {"check", row.check},
          {"resourceId", row.resource_id},
          {"resourceType", row.resource_type},
          {"name", row.name},
          {"location", row.location},
          {"recommendation", row.recommendation},
          {"estimatedMonthlySavings", row.estimated_monthly_savings},
          {"currency", row.currency},
      });
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("check,resource_type,name,location,estimated_monthly_savings,recommendation,resource_id");
    for (const auto& row : rows_) {
      csv.row([&row](CsvRowWriter& writer) {
        writer.escaped_cell(row.check);
        writer.escaped_cell(row.resource_type);
        writer.escaped_cell(row.name);
        writer.escaped_cell(row.location);
        writer.raw_cell(NumberFormatter::money(row.estimated_monthly_savings));
        writer.escaped_cell(row.recommendation);
        writer.escaped_cell(row.resource_id);
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    const auto max_value = max_savings(rows_);
    std::vector<std::vector<std::string>> rows{
        {"Check", "Type", "Name", "Location", "Savings", "Savings Bar", "Recommendation"}};
    for (const auto& row : rows_) {
      rows.push_back({row.check, row.resource_type, row.name, row.location,
                      NumberFormatter::money(row.estimated_monthly_savings),
                      progress_bar(row.estimated_monthly_savings, max_value), row.recommendation});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{{"Check", "Type", "Name", "Location", "Savings", "Recommendation"}};
    for (const auto& row : rows_) {
      rows.push_back({row.check, row.resource_type, row.name, row.location,
                      NumberFormatter::money(row.estimated_monthly_savings), row.recommendation});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    return {};
  }

private:
  const std::vector<WasteFinding>& rows_;
};

class CostHistoryRowsView {
public:
  explicit CostHistoryRowsView(const std::vector<CostSnapshot>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      nlohmann::json services = nlohmann::json::array();
      for (const auto& service : row.services) {
        services.push_back({{"service", service.service}, {"cost", service.cost}, {"currency", service.currency}});
      }
      payload.push_back({{"timestamp", row.timestamp},
                         {"subscription", row.subscription},
                         {"total", row.total},
                         {"currency", row.currency},
                         {"services", services}});
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("timestamp,subscription,total");
    for (const auto& row : rows_) {
      csv.row([&row](CsvRowWriter& writer) {
        writer.escaped_cell(row.timestamp);
        writer.escaped_cell(row.subscription);
        writer.raw_cell(NumberFormatter::money(row.total));
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    auto max_value = 0.0;
    for (const auto& row : rows_) {
      max_value = std::max(max_value, row.total);
    }
    std::vector<std::vector<std::string>> rows{{"Timestamp", "Subscription", "Total", "Spend Bar"}};
    for (const auto& row : rows_) {
      rows.push_back({row.timestamp, row.subscription, NumberFormatter::money(row.total),
                      progress_bar(row.total, max_value)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{{"Timestamp", "Subscription", "Total"}};
    for (const auto& row : rows_) {
      rows.push_back({row.timestamp, row.subscription, NumberFormatter::money(row.total)});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    return {};
  }

private:
  const std::vector<CostSnapshot>& rows_;
};

class SubscriptionAliasRowsView {
public:
  explicit SubscriptionAliasRowsView(const std::vector<SubscriptionAlias>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      payload.push_back({{"alias", row.alias}, {"subscription", row.subscription}});
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("alias,subscription");
    for (const auto& row : rows_) {
      csv.row([&row](CsvRowWriter& writer) {
        writer.escaped_cell(row.alias);
        writer.escaped_cell(row.subscription);
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{{"Alias", "Subscription"}};
    for (const auto& row : rows_) {
      rows.push_back({row.alias, row.subscription});
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    return table();
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    return {};
  }

private:
  const std::vector<SubscriptionAlias>& rows_;
};

class BudgetsView {
public:
  explicit BudgetsView(const std::vector<BudgetInfo>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    return ftxui::text("");
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& b : rows_) {
      double usage_pct = b.amount > 0.0 ? (b.current_spend / b.amount) * 100.0 : 0.0;
      payload.push_back({
          {"name", b.name},
          {"amount", b.amount},
          {"current_spend", b.current_spend},
          {"usage_percent", usage_pct},
          {"currency", b.currency},
          {"time_grain", b.time_grain},
          {"start_date", b.start_date},
          {"end_date", b.end_date},
      });
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("name,amount,current_spend,usage_percent,currency,time_grain,start_date,end_date");
    for (const auto& b : rows_) {
      csv.row([&b](CsvRowWriter& writer) {
        writer.escaped_cell(b.name);
        writer.raw_cell(NumberFormatter::money(b.amount));
        writer.raw_cell(NumberFormatter::money(b.current_spend));
        double usage_pct = b.amount > 0.0 ? (b.current_spend / b.amount) * 100.0 : 0.0;
        writer.raw_cell(NumberFormatter::percent(usage_pct));
        writer.escaped_cell(b.currency);
        writer.escaped_cell(b.time_grain);
        writer.escaped_cell(b.start_date);
        writer.escaped_cell(b.end_date);
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{
        {"Budget", "Amount", "Spend", "Usage %", "Usage Bar", "Status"}};
    for (const auto& b : rows_) {
      double usage_pct = b.amount > 0.0 ? (b.current_spend / b.amount) * 100.0 : 0.0;
      std::string status = (b.current_spend > b.amount && b.amount > 0.0) ? "EXCEEDED" : "OK";
      rows.push_back({
          b.name,
          NumberFormatter::money(b.amount) + " " + b.currency,
          NumberFormatter::money(b.current_spend) + " " + b.currency,
          NumberFormatter::percent(usage_pct),
          progress_bar(b.current_spend, b.amount),
          status,
      });
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{
        {"Budget Name", "Amount", "Current Spend", "Usage %", "Time Grain", "Status"}};
    for (const auto& b : rows_) {
      double usage_pct = b.amount > 0.0 ? (b.current_spend / b.amount) * 100.0 : 0.0;
      std::string status = (b.current_spend > b.amount && b.amount > 0.0) ? "EXCEEDED" : "OK";
      rows.push_back({
          b.name,
          NumberFormatter::money(b.amount) + " " + b.currency,
          NumberFormatter::money(b.current_spend) + " " + b.currency,
          NumberFormatter::percent(usage_pct),
          b.time_grain,
          status,
      });
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    return {};
  }

private:
  const std::vector<BudgetInfo>& rows_;
};

template <typename RowsView>
void render_rows(const RowsView& rows, OutputFormat format, std::ostream& out) {
  switch (format) {
  case OutputFormat::Json:
    JsonWriter{}.write(rows.json(), out);
    return;
  case OutputFormat::Csv: {
    CsvDocumentWriter csv(out);
    rows.csv(csv);
    return;
  }
  case OutputFormat::Markdown:
    MarkdownTableWriter{}.write(rows.markdown_rows(), out, rows.markdown_footer());
    return;
  case OutputFormat::Html:
    HtmlTableWriter{}.write("azdash Report", rows.markdown_rows(), out, rows.markdown_footer());
    return;
  case OutputFormat::Table:
    break;
  }
  TerminalTableWriter{}.write(rows.table(), out, rows.footer());
}

} // namespace

void render_costs(const std::vector<CostComparisonRow>& rows, double projected_total, OutputFormat format, std::ostream& out) {
  render_rows(CostRowsView(rows, projected_total), format, out);
}

void render_trends(const std::vector<MonthCost>& rows, OutputFormat format, std::ostream& out) {
  render_rows(TrendRowsView(rows), format, out);
}

void render_waste(const std::vector<WasteFinding>& rows, OutputFormat format, std::ostream& out) {
  render_rows(WasteRowsView(rows), format, out);
}

void render_subscription_aliases(const std::vector<SubscriptionAlias>& rows, OutputFormat format, std::ostream& out) {
  render_rows(SubscriptionAliasRowsView(rows), format, out);
}

void render_cost_history(const std::vector<CostSnapshot>& rows, OutputFormat format, std::ostream& out) {
  render_rows(CostHistoryRowsView(rows), format, out);
}

void render_budgets(const std::vector<BudgetInfo>& budgets, OutputFormat format, std::ostream& out) {
  render_rows(BudgetsView(budgets), format, out);
}

class CommitmentsView {
public:
  explicit CommitmentsView(const std::vector<CommitmentRecommendation>& rows) : rows_(rows) {}

  [[nodiscard]] auto footer() const -> ftxui::Element {
    double total_savings = 0.0;
    std::string currency = "USD";
    for (const auto& r : rows_) {
      total_savings += r.estimated_monthly_savings;
      if (r.currency != "USD" && !r.currency.empty()) {
        currency = r.currency;
      }
    }
    return ftxui::hbox({
        ftxui::text(" Total Potential Monthly Savings: ") | ftxui::bold | ftxui::color(ftxui::Color::Yellow),
        ftxui::text(NumberFormatter::money(total_savings) + " " + currency) | ftxui::bold | ftxui::color(ftxui::Color::Green),
    });
  }

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& r : rows_) {
      payload.push_back({
          {"id", r.id},
          {"type", r.type},
          {"resource_type", r.resource_type},
          {"sku", r.sku},
          {"region", r.region},
          {"term", r.term},
          {"estimated_monthly_savings", r.estimated_monthly_savings},
          {"estimated_monthly_cost", r.estimated_monthly_cost},
          {"currency", r.currency},
          {"details", r.details},
      });
    }
    return payload;
  }

  void csv(CsvDocumentWriter& csv) const {
    csv.header("type,resource_type,sku,region,term,estimated_monthly_cost,estimated_monthly_savings,currency,details");
    for (const auto& r : rows_) {
      csv.row([&r](CsvRowWriter& writer) {
        writer.escaped_cell(r.type);
        writer.escaped_cell(r.resource_type);
        writer.escaped_cell(r.sku);
        writer.escaped_cell(r.region);
        writer.escaped_cell(r.term);
        writer.raw_cell(NumberFormatter::money(r.estimated_monthly_cost));
        writer.raw_cell(NumberFormatter::money(r.estimated_monthly_savings));
        writer.escaped_cell(r.currency);
        writer.escaped_cell(r.details);
      });
    }
  }

  [[nodiscard]] auto table() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{
        {"Type", "SKU / Scope", "Region", "Term", "Est. Cost", "Monthly Savings"}};
    for (const auto& r : rows_) {
      rows.push_back({
          r.type,
          r.sku.empty() ? r.resource_type : r.sku,
          r.region.empty() ? "Global" : r.region,
          r.term,
          NumberFormatter::money(r.estimated_monthly_cost) + " " + r.currency,
          NumberFormatter::money(r.estimated_monthly_savings) + " " + r.currency,
      });
    }
    return rows;
  }

  [[nodiscard]] auto markdown_rows() const -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> rows{
        {"Type", "Resource / SKU", "Region", "Term", "Est. Cost", "Monthly Savings", "Details"}};
    for (const auto& r : rows_) {
      rows.push_back({
          r.type,
          r.sku.empty() ? r.resource_type : r.sku,
          r.region.empty() ? "Global" : r.region,
          r.term,
          NumberFormatter::money(r.estimated_monthly_cost) + " " + r.currency,
          NumberFormatter::money(r.estimated_monthly_savings) + " " + r.currency,
          r.details,
      });
    }
    return rows;
  }

  [[nodiscard]] auto markdown_footer() const -> std::vector<std::string> {
    double total_savings = 0.0;
    std::string currency = "USD";
    for (const auto& r : rows_) {
      total_savings += r.estimated_monthly_savings;
      if (r.currency != "USD" && !r.currency.empty()) {
        currency = r.currency;
      }
    }
    return {"**Total potential monthly savings:** " + NumberFormatter::money(total_savings) + " " + currency};
  }

private:
  const std::vector<CommitmentRecommendation>& rows_;
};

void render_commitments(const std::vector<CommitmentRecommendation>& recommendations, OutputFormat format, std::ostream& out) {
  render_rows(CommitmentsView(recommendations), format, out);
}

void render_compliance(const TagComplianceSummary& summary, OutputFormat format, std::ostream& out) {
  switch (format) {
  case OutputFormat::Json: {
    nlohmann::json root;
    root["totalResources"] = summary.total_resources;
    root["compliantResources"] = summary.compliant_resources;
    root["nonCompliantResources"] = summary.non_compliant_resources;
    root["compliancePercentage"] = summary.compliance_percentage;
    root["totalSpend"] = summary.total_spend;
    root["allocatedSpend"] = summary.allocated_spend;
    root["unallocatedSpend"] = summary.unallocated_spend;
    root["currency"] = summary.currency;

    nlohmann::json missing_counts = nlohmann::json::object();
    for (const auto& [tag, count] : summary.missing_tag_counts) {
      missing_counts[tag] = count;
    }
    root["missingTagCounts"] = missing_counts;

    nlohmann::json missing_costs = nlohmann::json::object();
    for (const auto& [tag, cost] : summary.missing_tag_costs) {
      missing_costs[tag] = cost;
    }
    root["missingTagCosts"] = missing_costs;

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : summary.non_compliant_items) {
      nlohmann::json it;
      it["resourceName"] = item.resource_name;
      it["resourceGroup"] = item.resource_group;
      it["resourceType"] = item.resource_type;
      it["cost"] = item.cost;
      it["currency"] = item.currency;
      it["missingTags"] = item.missing_tags;
      it["tags"] = item.tags;
      items.push_back(it);
    }
    root["nonCompliantItems"] = items;

    JsonWriter{}.write(root, out);
    return;
  }
  case OutputFormat::Csv: {
    CsvDocumentWriter csv(out);
    csv.header("resource_name,resource_group,resource_type,cost,currency,missing_tags");
    for (const auto& item : summary.non_compliant_items) {
      csv.row([&item](CsvRowWriter& writer) {
        writer.escaped_cell(item.resource_name);
        writer.escaped_cell(item.resource_group);
        writer.escaped_cell(item.resource_type);
        writer.raw_cell(NumberFormatter::money(item.cost));
        writer.escaped_cell(item.currency);
        std::string missing_str;
        for (std::size_t i = 0; i < item.missing_tags.size(); ++i) {
          if (i > 0) missing_str += ";";
          missing_str += item.missing_tags[i];
        }
        writer.escaped_cell(missing_str);
      });
    }
    return;
  }
  case OutputFormat::Markdown: {
    out << "# Azure Tag Compliance & Cost Allocation Report\n\n";
    out << "- **Overall Compliance:** " << NumberFormatter::percent(summary.compliance_percentage) << "\n";
    out << "- **Compliant Resources:** " << summary.compliant_resources << " / " << summary.total_resources << "\n";
    out << "- **Total Tracked Spend:** " << NumberFormatter::money(summary.total_spend) << " " << summary.currency << "\n";
    out << "- **Allocated Spend:** " << NumberFormatter::money(summary.allocated_spend) << " " << summary.currency
        << " (" << NumberFormatter::percent(summary.total_spend > 0.0 ? (100.0 * summary.allocated_spend / summary.total_spend) : 100.0) << ")\n";
    out << "- **Unallocated Spend:** " << NumberFormatter::money(summary.unallocated_spend) << " " << summary.currency
        << " (" << NumberFormatter::percent(summary.total_spend > 0.0 ? (100.0 * summary.unallocated_spend / summary.total_spend) : 0.0) << ")\n\n";

    out << "### Missing Tags Breakdown\n\n";
    std::vector<std::vector<std::string>> tag_rows{{"Tag", "Missing Count", "Unallocated Spend"}};
    for (const auto& [tag, count] : summary.missing_tag_counts) {
      double cost = 0.0;
      if (auto it = summary.missing_tag_costs.find(tag); it != summary.missing_tag_costs.end()) {
        cost = it->second;
      }
      tag_rows.push_back({tag, std::to_string(count), NumberFormatter::money(cost) + " " + summary.currency});
    }
    if (tag_rows.size() == 1) {
      tag_rows.push_back({"(None)", "0", "0.00 " + summary.currency});
    }
    MarkdownTableWriter{}.write(tag_rows, out);

    out << "\n### Non-Compliant Resources\n\n";
    std::vector<std::vector<std::string>> item_rows{{"Resource", "Resource Group", "Type", "Cost", "Missing Tags"}};
    for (const auto& item : summary.non_compliant_items) {
      std::string missing_str;
      for (std::size_t i = 0; i < item.missing_tags.size(); ++i) {
        if (i > 0) missing_str += ", ";
        missing_str += item.missing_tags[i];
      }
      item_rows.push_back({item.resource_name, item.resource_group, item.resource_type,
                           NumberFormatter::money(item.cost) + " " + item.currency, missing_str});
    }
    if (item_rows.size() == 1) {
      item_rows.push_back({"(None)", "-", "-", "0.00 " + summary.currency, "All resources compliant!"});
    }
    MarkdownTableWriter{}.write(item_rows, out);
    return;
  }
  case OutputFormat::Html: {
    out << "<!DOCTYPE html>\n"
        << "<html lang=\"en\">\n"
        << "<head>\n"
        << "  <meta charset=\"UTF-8\">\n"
        << "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        << "  <title>Azure Tag Compliance &amp; Cost Allocation Report</title>\n"
        << "  <style>\n"
        << "    :root {\n"
        << "      --bg: #0f172a;\n"
        << "      --surface: #1e293b;\n"
        << "      --surface-border: #334155;\n"
        << "      --text: #f8fafc;\n"
        << "      --text-muted: #94a3b8;\n"
        << "      --primary: #38bdf8;\n"
        << "      --success: #4ade80;\n"
        << "      --warning: #fbbf24;\n"
        << "      --danger: #f87171;\n"
        << "    }\n"
        << "    * { box-sizing: border-box; margin: 0; padding: 0; }\n"
        << "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background-color: var(--bg); color: var(--text); padding: 2rem; line-height: 1.5; }\n"
        << "    .container { max-width: 1200px; margin: 0 auto; }\n"
        << "    header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 2rem; padding-bottom: 1rem; border-bottom: 1px solid var(--surface-border); }\n"
        << "    h1 { font-size: 1.75rem; font-weight: 700; color: var(--primary); }\n"
        << "    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 1.25rem; margin-bottom: 2rem; }\n"
        << "    .kpi-card { background: var(--surface); border: 1px solid var(--surface-border); border-radius: 0.75rem; padding: 1.25rem; }\n"
        << "    .kpi-title { font-size: 0.8rem; text-transform: uppercase; color: var(--text-muted); font-weight: 600; margin-bottom: 0.5rem; }\n"
        << "    .kpi-value { font-size: 1.75rem; font-weight: 700; }\n"
        << "    .kpi-sub { font-size: 0.8rem; color: var(--text-muted); margin-top: 0.25rem; }\n"
        << "    .card { background: var(--surface); border: 1px solid var(--surface-border); border-radius: 0.75rem; overflow: hidden; margin-bottom: 2rem; }\n"
        << "    .card-header { padding: 1rem 1.25rem; font-weight: 600; font-size: 1rem; border-bottom: 1px solid var(--surface-border); background: #162032; color: var(--primary); }\n"
        << "    table { width: 100%; border-collapse: collapse; text-align: left; font-size: 0.875rem; }\n"
        << "    th { background: #162032; padding: 0.75rem 1rem; font-weight: 600; color: var(--primary); border-bottom: 1px solid var(--surface-border); text-transform: uppercase; font-size: 0.75rem; }\n"
        << "    td { padding: 0.75rem 1rem; border-bottom: 1px solid var(--surface-border); }\n"
        << "    tr:last-child td { border-bottom: none; }\n"
        << "    .tag-badge { display: inline-block; padding: 0.15rem 0.5rem; border-radius: 4px; font-size: 0.75rem; background: rgba(248, 113, 113, 0.15); color: var(--danger); border: 1px solid rgba(248, 113, 113, 0.3); margin-right: 0.25rem; }\n"
        << "  </style>\n"
        << "</head>\n"
        << "<body>\n"
        << "  <div class=\"container\">\n"
        << "    <header>\n"
        << "      <h1>Azure Tag Compliance &amp; Cost Allocation Report</h1>\n"
        << "      <span style=\"color: var(--text-muted); font-size: 0.875rem;\">azdash FinOps Governance</span>\n"
        << "    </header>\n"
        << "    <div class=\"grid\">\n"
        << "      <div class=\"kpi-card\">\n"
        << "        <div class=\"kpi-title\">Tag Compliance</div>\n"
        << "        <div class=\"kpi-value\" style=\"color: "
        << (summary.compliance_percentage >= 90.0 ? "var(--success)" : (summary.compliance_percentage >= 75.0 ? "var(--warning)" : "var(--danger)"))
        << "\">" << NumberFormatter::percent(summary.compliance_percentage) << "</div>\n"
        << "        <div class=\"kpi-sub\">" << summary.compliant_resources << " of " << summary.total_resources << " resources compliant</div>\n"
        << "      </div>\n"
        << "      <div class=\"kpi-card\">\n"
        << "        <div class=\"kpi-title\">Unallocated Spend</div>\n"
        << "        <div class=\"kpi-value\" style=\"color: var(--danger);\">"
        << NumberFormatter::money(summary.unallocated_spend) << " " << HtmlTableWriter::escape_html(summary.currency) << "</div>\n"
        << "        <div class=\"kpi-sub\">" << summary.non_compliant_resources << " non-compliant resources</div>\n"
        << "      </div>\n"
        << "      <div class=\"kpi-card\">\n"
        << "        <div class=\"kpi-title\">Allocated Spend</div>\n"
        << "        <div class=\"kpi-value\" style=\"color: var(--success);\">"
        << NumberFormatter::money(summary.allocated_spend) << " " << HtmlTableWriter::escape_html(summary.currency) << "</div>\n"
        << "        <div class=\"kpi-sub\">Total: " << NumberFormatter::money(summary.total_spend) << " " << HtmlTableWriter::escape_html(summary.currency) << "</div>\n"
        << "      </div>\n"
        << "    </div>\n";

    out << "    <div class=\"card\">\n"
        << "      <div class=\"card-header\">Missing Tags Breakdown</div>\n"
        << "      <table>\n"
        << "        <thead><tr><th>Tag</th><th>Missing Count</th><th>Unallocated Spend</th></tr></thead>\n"
        << "        <tbody>\n";
    if (summary.missing_tag_counts.empty()) {
      out << "          <tr><td colspan=\"3\" style=\"text-align: center; color: var(--success);\">No missing tags!</td></tr>\n";
    } else {
      for (const auto& [tag, count] : summary.missing_tag_counts) {
        double cost = 0.0;
        if (auto it = summary.missing_tag_costs.find(tag); it != summary.missing_tag_costs.end()) {
          cost = it->second;
        }
        out << "          <tr><td>" << HtmlTableWriter::escape_html(tag) << "</td><td>" << count << "</td><td>"
            << NumberFormatter::money(cost) << " " << HtmlTableWriter::escape_html(summary.currency) << "</td></tr>\n";
      }
    }
    out << "        </tbody>\n      </table>\n    </div>\n";

    out << "    <div class=\"card\">\n"
        << "      <div class=\"card-header\">Non-Compliant Resources (" << summary.non_compliant_items.size() << ")</div>\n"
        << "      <table>\n"
        << "        <thead><tr><th>Resource</th><th>Resource Group</th><th>Type</th><th>Cost</th><th>Missing Tags</th></tr></thead>\n"
        << "        <tbody>\n";
    if (summary.non_compliant_items.empty()) {
      out << "          <tr><td colspan=\"5\" style=\"text-align: center; color: var(--success);\">All resources compliant!</td></tr>\n";
    } else {
      for (const auto& item : summary.non_compliant_items) {
        out << "          <tr><td>" << HtmlTableWriter::escape_html(item.resource_name) << "</td><td>"
            << HtmlTableWriter::escape_html(item.resource_group) << "</td><td>"
            << HtmlTableWriter::escape_html(item.resource_type) << "</td><td>"
            << NumberFormatter::money(item.cost) << " " << HtmlTableWriter::escape_html(item.currency) << "</td><td>";
        for (const auto& mt : item.missing_tags) {
          out << "<span class=\"tag-badge\">" << HtmlTableWriter::escape_html(mt) << "</span>";
        }
        out << "</td></tr>\n";
      }
    }
    out << "        </tbody>\n      </table>\n    </div>\n";

    out << "  </div>\n</body>\n</html>\n";
    return;
  }
  case OutputFormat::Table:
  default:
    break;
  }

  std::vector<std::vector<std::string>> tag_rows{{"Required Tag", "Missing Count", "Spend Impact"}};
  for (const auto& [tag, count] : summary.missing_tag_counts) {
    double cost = 0.0;
    if (auto it = summary.missing_tag_costs.find(tag); it != summary.missing_tag_costs.end()) {
      cost = it->second;
    }
    tag_rows.push_back({tag, std::to_string(count), NumberFormatter::money(cost) + " " + summary.currency});
  }

  std::vector<std::vector<std::string>> item_rows{{"Resource", "Resource Group", "Type", "Cost", "Missing Tags"}};
  for (const auto& item : summary.non_compliant_items) {
    std::string missing_str;
    for (std::size_t i = 0; i < item.missing_tags.size(); ++i) {
      if (i > 0) missing_str += ", ";
      missing_str += item.missing_tags[i];
    }
    item_rows.push_back({item.resource_name, item.resource_group, item.resource_type,
                         NumberFormatter::money(item.cost) + " " + item.currency, missing_str});
  }
  if (item_rows.size() == 1) {
    item_rows.push_back({"(None)", "-", "-", "0.00 " + summary.currency, "All resources compliant!"});
  }

  auto footer = ftxui::vbox({
      ftxui::hbox({
          ftxui::text(" Compliance: ") | ftxui::bold | ftxui::color(ftxui::Color::Yellow),
          ftxui::text(NumberFormatter::percent(summary.compliance_percentage)) | ftxui::bold |
              ftxui::color(summary.compliance_percentage >= 90.0 ? ftxui::Color::Green : (summary.compliance_percentage >= 75.0 ? ftxui::Color::Yellow : ftxui::Color::Red)),
          ftxui::text(" (" + std::to_string(summary.compliant_resources) + "/" + std::to_string(summary.total_resources) + " resources)  |  "),
          ftxui::text("Unallocated Spend: ") | ftxui::bold | ftxui::color(ftxui::Color::Red),
          ftxui::text(NumberFormatter::money(summary.unallocated_spend) + " " + summary.currency) | ftxui::bold | ftxui::color(ftxui::Color::White),
      }),
  });

  if (!summary.missing_tag_counts.empty()) {
    TerminalTableWriter{}.write(tag_rows, out, ftxui::text(""));
  }
  TerminalTableWriter{}.write(item_rows, out, footer);
}

void render_help_screen(std::ostream& out) {
  auto document = ftxui::vbox({
                      panel_title("azdash command center"),
                      ftxui::separator(),
                      ftxui::text("Usage:") | ftxui::bold | ftxui::color(ftxui::Color::White),
                      command_row("azdash link-account", "confirm the active az login"),
                      command_row("azdash cost", "current vs previous spend"),
                      command_row("azdash trend [services...]", "six-month spend trend"),
                      command_row("azdash waste [checks...]", "advisor and waste signals"),
                      command_row("azdash report cost|trend|waste --path <path>", "write PDF reports"),
                      command_row("azdash alias-sub set <alias> <subscription>", "save a subscription alias"),
                      command_row("azdash alias-sub list", "show configured aliases"),
                      command_row("azdash history", "locally recorded cost snapshots"),
                      command_row("azdash version", "show release banner"),
                      command_row("azdash update", "show upgrade commands"),
                      ftxui::separator(),
                      ftxui::text("Global flags:") | ftxui::bold | ftxui::color(ftxui::Color::White),
                      command_row("--subscription <id-name-or-alias>", "Azure subscription selector"),
                      command_row("-o, --output <table|json|csv|markdown>", "table is styled; json/csv/markdown stay script-safe"),
                      command_row("--path <file-or-directory>", "PDF output destination"),
                      ftxui::separator(),
                      ftxui::text("Waste checks: advisor compute network storage appservice database") |
                          ftxui::color(ftxui::Color::Yellow),
                      ftxui::text("              containers keyvault") |
                          ftxui::color(ftxui::Color::Yellow),
                  }) |
                  ftxui::border | ftxui::color(ftxui::Color::Green);
  write_document(document, out);
}

void render_version(std::ostream& out) {
  constexpr auto banner =
      "    _    _________     _    ____  _   _ \n"
      "   / \\  |__  /  _ \\   / \\  / ___|| | | |\n"
      "  / _ \\   / /| | | | / _ \\ \\___ \\| |_| |\n"
      " / ___ \\ / /_| |_| |/ ___ \\ ___) |  _  |\n"
      "/_/   \\_/____|____//_/   \\_\\____/|_| |_|\n";

  auto document = ftxui::vbox({
      ftxui::text(""),
      ftxui::paragraph(banner) | ftxui::bold | ftxui::color(ftxui::Color::Green),
      ftxui::separator() | ftxui::color(ftxui::Color::Green),
      ftxui::hbox({ftxui::text(" Version: "), ftxui::text(AZ_DASHBOARD_VERSION) | ftxui::bold}),
      ftxui::hbox({ftxui::text(" Release: "), ftxui::text("az-dashboard " AZ_DASHBOARD_VERSION)}),
      ftxui::hbox({ftxui::text(" Built:   "), ftxui::text(__DATE__ " " __TIME__)}),
      ftxui::hbox({ftxui::text(" C++:     "), ftxui::text(std::to_string(__cplusplus))}),
      ftxui::hbox({ftxui::text(" Backend: "), ftxui::text("Azure CLI")}),
      ftxui::hbox({ftxui::text(" Output:  "), ftxui::text("FTXUI tables, JSON, CSV, PDF reports")}),
      ftxui::hbox({ftxui::text(" Config:  "), ftxui::text("subscription aliases enabled")}),
  }) | ftxui::border | ftxui::color(ftxui::Color::Green);

  write_document(document, out);
}

void render_update_guidance(std::ostream& out) {
  auto document = ftxui::vbox({
                      panel_title("azdash update"),
                      ftxui::separator(),
                      ftxui::text("Recommended upgrade paths") | ftxui::bold | ftxui::color(ftxui::Color::White),
                      command_row("vcpkg update && vcpkg upgrade az-dashboard", "refresh a vcpkg install"),
                      command_row("docker pull <registry>/azdash:latest", "refresh container deployments"),
                      command_row("git pull && cmake --build build", "refresh local source builds"),
                      ftxui::separator(),
                      ftxui::hbox({
                          ftxui::text("status ") | ftxui::color(ftxui::Color::Green),
                          ftxui::gauge(1.0F) | ftxui::color(ftxui::Color::Green),
                      }),
                  }) |
                  ftxui::border | ftxui::color(ftxui::Color::Cyan);
  write_document(document, out);
}

void render_success(const std::string& title, const std::string& detail, std::ostream& out) {
  auto document = ftxui::vbox({
                      ftxui::hbox({
                          ftxui::text("OK ") | ftxui::bold | ftxui::color(ftxui::Color::Green),
                          ftxui::text(title) | ftxui::bold | ftxui::color(ftxui::Color::White),
                      }),
                      ftxui::separator(),
                      ftxui::paragraph(detail) | ftxui::color(ftxui::Color::GrayLight),
                      ftxui::hbox({
                          ftxui::text("complete ") | ftxui::color(ftxui::Color::Green),
                          ftxui::gauge(1.0F) | ftxui::color(ftxui::Color::Green),
                      }),
                  }) |
                  ftxui::border | ftxui::color(ftxui::Color::Green);
  write_document(document, out);
}

void render_error(const std::string& message, std::ostream& out) {
  auto document = ftxui::vbox({
                      ftxui::hbox({
                          ftxui::text("ERROR ") | ftxui::bold | ftxui::color(ftxui::Color::Red),
                          ftxui::text("azdash") | ftxui::bold | ftxui::color(ftxui::Color::White),
                      }),
                      ftxui::separator(),
                      ftxui::paragraph(message) | ftxui::color(ftxui::Color::Yellow),
                      ftxui::separator(),
                      ftxui::text("Run azdash --help for available commands.") |
                          ftxui::color(ftxui::Color::GrayLight),
                  }) |
                  ftxui::border | ftxui::color(ftxui::Color::Red);
  write_document(document, out);
}

} // namespace azdash
