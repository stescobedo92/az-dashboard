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
  void write(std::vector<std::vector<std::string>> rows, std::ostream& out) const {
    auto table = ftxui::Table(padded(std::move(rows)));
    table.SelectAll().Border(ftxui::LIGHT);
    table.SelectAll().Decorate(ftxui::color(ftxui::Color::GrayLight));
    table.SelectRow(0).Decorate(ftxui::bold | ftxui::color(ftxui::Color::Cyan));
    table.SelectColumn(0).Decorate(ftxui::bold | ftxui::color(ftxui::Color::White));
    auto document = ftxui::vbox({
        ftxui::text("azdash") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
        ftxui::separator(),
        table.Render(),
        ftxui::separator(),
        ftxui::hbox({
            ftxui::text("complete ") | ftxui::color(ftxui::Color::Green),
            ftxui::gauge(1.0F) | ftxui::color(ftxui::Color::Green),
        }),
    });
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(document));
    ftxui::Render(screen, document);
    out << screen.ToString() << '\n';
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

class CostRowsView {
public:
  explicit CostRowsView(const std::vector<CostComparisonRow>& rows) : rows_(rows) {}

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      payload.push_back({
          {"service", row.service},
          {"previous", row.previous},
          {"current", row.current},
          {"delta", row.delta},
          {"deltaPercent", row.delta_percent},
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

private:
  const std::vector<CostComparisonRow>& rows_;
};

class TrendRowsView {
public:
  explicit TrendRowsView(const std::vector<MonthCost>& rows) : rows_(rows) {}

  [[nodiscard]] auto json() const -> nlohmann::json {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows_) {
      nlohmann::json services = nlohmann::json::array();
      for (const auto& service : row.services) {
        services.push_back({{"service", service.service}, {"cost", service.cost}});
      }
      payload.push_back({{"month", row.month}, {"total", row.total}, {"services", services}});
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

private:
  const std::vector<MonthCost>& rows_;
};

class WasteRowsView {
public:
  explicit WasteRowsView(const std::vector<WasteFinding>& rows) : rows_(rows) {}

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

private:
  const std::vector<WasteFinding>& rows_;
};

class SubscriptionAliasRowsView {
public:
  explicit SubscriptionAliasRowsView(const std::vector<SubscriptionAlias>& rows) : rows_(rows) {}

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

private:
  const std::vector<SubscriptionAlias>& rows_;
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
  case OutputFormat::Table:
    break;
  }
  TerminalTableWriter{}.write(rows.table(), out);
}

} // namespace

void render_costs(const std::vector<CostComparisonRow>& rows, OutputFormat format, std::ostream& out) {
  render_rows(CostRowsView(rows), format, out);
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

void render_version(std::ostream& out) {
  out << "azdash " << AZ_DASHBOARD_VERSION << '\n';
}

} // namespace azdash
