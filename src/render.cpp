#include "az_dashboard/render.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace azdash {
namespace {

auto money(double value) -> std::string {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << value;
  return out.str();
}

auto percent(double value) -> std::string {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << value << "%";
  return out.str();
}

void print_table(std::vector<std::vector<std::string>> rows, std::ostream& out) {
  auto table = ftxui::Table(std::move(rows));
  table.SelectAll().Border(ftxui::LIGHT);
  table.SelectRow(0).Decorate(ftxui::bold);
  table.SelectColumn(0).Decorate(ftxui::bold);
  auto document = table.Render();
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(document));
  ftxui::Render(screen, document);
  out << screen.ToString() << '\n';
}

void render_json(const nlohmann::json& payload, std::ostream& out) {
  out << payload.dump(2) << '\n';
}

void csv_escape(std::ostream& out, const std::string& value) {
  const auto must_quote = value.find_first_of(",\"\n") != std::string::npos;
  if (!must_quote) {
    out << value;
    return;
  }
  out << '"';
  for (const auto c : value) {
    if (c == '"') {
      out << "\"\"";
    } else {
      out << c;
    }
  }
  out << '"';
}

} // namespace

void render_costs(const std::vector<CostComparisonRow>& rows, OutputFormat format, std::ostream& out) {
  if (format == OutputFormat::Json) {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows) {
      payload.push_back({
          {"service", row.service},
          {"previous", row.previous},
          {"current", row.current},
          {"delta", row.delta},
          {"deltaPercent", row.delta_percent},
      });
    }
    render_json(payload, out);
    return;
  }

  if (format == OutputFormat::Csv) {
    out << "service,previous,current,delta,delta_percent\n";
    for (const auto& row : rows) {
      csv_escape(out, row.service);
      out << ',' << money(row.previous) << ',' << money(row.current) << ',' << money(row.delta) << ','
          << percent(row.delta_percent) << '\n';
    }
    return;
  }

  std::vector<std::vector<std::string>> table{{"Service", "Previous", "Current", "Delta", "Delta %"}};
  for (const auto& row : rows) {
    table.push_back({row.service, money(row.previous), money(row.current), money(row.delta), percent(row.delta_percent)});
  }
  print_table(std::move(table), out);
}

void render_trends(const std::vector<MonthCost>& rows, OutputFormat format, std::ostream& out) {
  if (format == OutputFormat::Json) {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows) {
      nlohmann::json services = nlohmann::json::array();
      for (const auto& service : row.services) {
        services.push_back({{"service", service.service}, {"cost", service.cost}});
      }
      payload.push_back({{"month", row.month}, {"total", row.total}, {"services", services}});
    }
    render_json(payload, out);
    return;
  }

  if (format == OutputFormat::Csv) {
    out << "month,total\n";
    for (const auto& row : rows) {
      out << row.month << ',' << money(row.total) << '\n';
    }
    return;
  }

  std::vector<std::vector<std::string>> table{{"Month", "Total"}};
  for (const auto& row : rows) {
    table.push_back({row.month, money(row.total)});
  }
  print_table(std::move(table), out);
}

void render_waste(const std::vector<WasteFinding>& rows, OutputFormat format, std::ostream& out) {
  if (format == OutputFormat::Json) {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& row : rows) {
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
    render_json(payload, out);
    return;
  }

  if (format == OutputFormat::Csv) {
    out << "check,resource_type,name,location,estimated_monthly_savings,recommendation,resource_id\n";
    for (const auto& row : rows) {
      csv_escape(out, row.check);
      out << ',';
      csv_escape(out, row.resource_type);
      out << ',';
      csv_escape(out, row.name);
      out << ',';
      csv_escape(out, row.location);
      out << ',' << money(row.estimated_monthly_savings) << ',';
      csv_escape(out, row.recommendation);
      out << ',';
      csv_escape(out, row.resource_id);
      out << '\n';
    }
    return;
  }

  std::vector<std::vector<std::string>> table{{"Check", "Type", "Name", "Location", "Savings", "Recommendation"}};
  for (const auto& row : rows) {
    table.push_back({row.check, row.resource_type, row.name, row.location, money(row.estimated_monthly_savings),
                     row.recommendation});
  }
  print_table(std::move(table), out);
}

void render_version(std::ostream& out) {
  out << "azdash " << AZ_DASHBOARD_VERSION << '\n';
}

} // namespace azdash
