#include "az_dashboard/ui.hpp"

#include "az_dashboard/analytics.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/captured_mouse.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace azdash {
namespace {

using namespace ftxui;

[[nodiscard]] auto format_currency_amount(double amount, const std::string& currency) -> std::string {
  std::ostringstream ss;
  ss << std::fixed << std::setprecision(2) << amount << " " << currency;
  return ss.str();
}

} // namespace

auto render_tui_trend_element(const std::vector<MonthCost>& trends,
                              const std::string& currency) -> ftxui::Element {
  if (trends.empty()) {
    return text("No trend data available.") | dim;
  }

  double max_cost = 1.0;
  for (const auto& t : trends) {
    if (t.total > max_cost) {
      max_cost = t.total;
    }
  }

  Elements rows;
  rows.push_back(
      hbox({
          text("Month") | bold | size(WIDTH, EQUAL, 12),
          text("Cost") | bold | size(WIDTH, EQUAL, 18),
          text("Visual Proportion") | bold | flex,
      }) |
      color(Color::Cyan));
  rows.push_back(separator());

  for (std::size_t i = 0; i < trends.size(); ++i) {
    const auto& t = trends[i];
    float ratio = static_cast<float>(std::clamp(t.total / max_cost, 0.0, 1.0));
    const auto curr = t.currency.empty() ? currency : t.currency;

    rows.push_back(hbox({
        text(t.month) | size(WIDTH, EQUAL, 12),
        text(format_currency_amount(t.total, curr)) | size(WIDTH, EQUAL, 18),
        gauge(ratio) | color(i + 1 == trends.size() ? Color::Yellow : Color::Green) | flex,
    }));
  }

  const double current_month_total = trends.back().total;
  const double projection = compute_projection(current_month_total);

  rows.push_back(separator());
  rows.push_back(
      hbox({
          text("Projected Month End: ") | bold | color(Color::Cyan),
          text(format_currency_amount(projection, currency)) | bold | color(Color::Yellow),
      }));

  return vbox(std::move(rows));
}

auto render_tui_waste_element(const std::vector<WasteFinding>& findings,
                              int selected_index) -> ftxui::Element {
  if (findings.empty()) {
    return vbox({
        text("No waste detected! Your Azure cloud resources are well optimized.") | color(Color::Green) | bold,
    });
  }

  Elements list_items;
  list_items.push_back(
      hbox({
          text("Check") | bold | size(WIDTH, EQUAL, 14),
          text("Resource Name") | bold | size(WIDTH, EQUAL, 28),
          text("Type") | bold | size(WIDTH, EQUAL, 35),
          text("Est. Savings") | bold | flex,
      }) |
      color(Color::Cyan));
  list_items.push_back(separator());

  for (int i = 0; i < static_cast<int>(findings.size()); ++i) {
    const auto& item = findings[i];
    const bool is_selected = (i == selected_index);

    auto row = hbox({
        text(item.check) | size(WIDTH, EQUAL, 14),
        text(item.name.empty() ? "<unnamed>" : item.name) | size(WIDTH, EQUAL, 28),
        text(item.resource_type) | size(WIDTH, EQUAL, 35),
        text(item.estimated_monthly_savings > 0.0
                 ? format_currency_amount(item.estimated_monthly_savings, item.currency)
                 : "N/A") |
            flex,
    });

    if (is_selected) {
      row = row | inverted | color(Color::Yellow);
    }
    list_items.push_back(row);
  }

  // Selected item detail box
  Element detail_box = text("Use Up/Down arrows to select a finding for details.") | dim;
  if (selected_index >= 0 && selected_index < static_cast<int>(findings.size())) {
    const auto& sel = findings[selected_index];
    detail_box = vbox({
        text("Finding Details") | bold | color(Color::Yellow),
        separator(),
        text("Resource ID: " + sel.resource_id) | dim,
        text("Recommendation: " + sel.recommendation) | color(Color::White),
    });
  }

  return vbox({
      vbox(std::move(list_items)) | flex,
      separator(),
      detail_box,
  });
}

auto render_tui_cost_element(const std::vector<ServiceCost>& current_costs,
                             int selected_index) -> ftxui::Element {
  if (current_costs.empty()) {
    return text("No cost data available for the current billing cycle.") | dim;
  }

  double total_bill = 0.0;
  for (const auto& c : current_costs) {
    total_bill += c.cost;
  }

  Elements rows;
  rows.push_back(
      hbox({
          text("Service / Group") | bold | size(WIDTH, EQUAL, 35),
          text("Cost") | bold | size(WIDTH, EQUAL, 18),
          text("Share of Total") | bold | flex,
      }) |
      color(Color::Cyan));
  rows.push_back(separator());

  for (int i = 0; i < static_cast<int>(current_costs.size()); ++i) {
    const auto& c = current_costs[i];
    const bool is_selected = (i == selected_index);
    float share = total_bill > 0.0 ? static_cast<float>(c.cost / total_bill) : 0.0f;

    auto row = hbox({
        text(c.service) | size(WIDTH, EQUAL, 35),
        text(format_currency_amount(c.cost, c.currency)) | size(WIDTH, EQUAL, 18),
        gauge(share) | color(Color::Blue) | flex,
    });

    if (is_selected) {
      row = row | inverted | color(Color::Yellow);
    }
    rows.push_back(row);
  }

  rows.push_back(separator());
  const std::string curr = current_costs.empty() ? "USD" : current_costs.front().currency;
  rows.push_back(
      hbox({
          text("Current Month Total: ") | bold | color(Color::Cyan),
          text(format_currency_amount(total_bill, curr)) | bold | color(Color::Green),
      }));

  return vbox(std::move(rows));
}

auto run_tui(const CliOptions& options, const CliRuntime& runtime) -> int {
  auto screen = ScreenInteractive::Fullscreen();

  std::atomic<bool> loaded{false};
  std::string status = "Loading Azure cloud data...";

  AccountInfo account_info;
  std::vector<ServiceCost> current_costs;
  std::vector<MonthCost> trends;
  std::vector<WasteFinding> waste;
  std::string active_currency = "USD";

  int tab_index = 0;
  const std::vector<std::string> tab_entries = {
      "1. Cost Drilldown",
      "2. 6-Month Trends",
      "3. Waste Findings",
      "4. Account & Aliases",
  };
  auto tab_menu = Menu(&tab_entries, &tab_index);

  int cost_selected = 0;
  int waste_selected = 0;

  // Background data fetcher
  auto fetch_data = [&]() {
    try {
      account_info = runtime.account_provider.account(options);
      current_costs = runtime.cost_provider.current_month_costs(options);
      trends = runtime.trend_provider.six_month_trends(options);
      waste = runtime.waste_provider.waste_findings(options);

      if (!current_costs.empty()) {
        active_currency = current_costs.front().currency;
      }
      status = "Ready";
      loaded = true;
    } catch (const std::exception& e) {
      status = std::string("Error: ") + e.what();
    }
    screen.PostEvent(Event::Custom);
  };

  std::jthread loader(fetch_data);

  auto render_account_tab = [&]() {
    const auto aliases = runtime.alias_store.list();
    Elements alias_elements;
    alias_elements.push_back(
        hbox({
            text("Alias") | bold | size(WIDTH, EQUAL, 20),
            text("Subscription Target") | bold | flex,
        }) |
        color(Color::Cyan));
    alias_elements.push_back(separator());

    if (aliases.empty()) {
      alias_elements.push_back(text("No subscription aliases configured. Use 'azdash alias-sub set <name> <sub>'.") | dim);
    } else {
      for (const auto& a : aliases) {
        alias_elements.push_back(hbox({
            text(a.alias) | size(WIDTH, EQUAL, 20),
            text(a.subscription) | flex,
        }));
      }
    }

    return vbox({
        text("Azure Identity & Subscriptions") | bold | color(Color::Cyan),
        separator(),
        hbox({text("Subscription: ") | bold, text(account_info.subscription_name + " (" + account_info.subscription_id + ")")}),
        hbox({text("Tenant:       ") | bold, text(account_info.tenant_id)}),
        hbox({text("User:         ") | bold, text(account_info.user_name)}),
        separator(),
        text("Configured Aliases:") | bold,
        vbox(std::move(alias_elements)) | flex,
    });
  };

  auto container = Container::Vertical({
      tab_menu,
  });

  auto renderer = Renderer(container, [&] {
    Element content;
    if (!loaded) {
      content = vbox({
          filler(),
          text(status) | bold | color(Color::Yellow) | center,
          filler(),
      }) | flex;
    } else {
      switch (tab_index) {
        case 0:
          content = render_tui_cost_element(current_costs, cost_selected);
          break;
        case 1:
          content = render_tui_trend_element(trends, active_currency);
          break;
        case 2:
          content = render_tui_waste_element(waste, waste_selected);
          break;
        case 3:
        default:
          content = render_account_tab();
          break;
      }
    }

    return vbox({
        hbox({
            text(" azdash FinOps Dashboard ") | bold | color(Color::White) | bgcolor(Color::Blue),
            separator(),
            text(account_info.subscription_name.empty() ? "Connecting..." : account_info.subscription_name) | color(Color::Cyan),
            filler(),
            text("Status: " + status) | color(loaded ? Color::Green : Color::Yellow),
        }),
        separator(),
        tab_menu->Render() | color(Color::White),
        separator(),
        content | flex,
        separator(),
        hbox({
            text("Tab / 1-4: Switch Tabs | Up/Down: Navigate | r: Reload | q: Quit") | dim,
        }),
    }) | border;
  });

  auto event_handler = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (event == Event::Character('1')) {
      tab_index = 0;
      return true;
    }
    if (event == Event::Character('2')) {
      tab_index = 1;
      return true;
    }
    if (event == Event::Character('3')) {
      tab_index = 2;
      return true;
    }
    if (event == Event::Character('4')) {
      tab_index = 3;
      return true;
    }
    if (event == Event::ArrowUp) {
      if (tab_index == 0 && cost_selected > 0) {
        --cost_selected;
        return true;
      }
      if (tab_index == 2 && waste_selected > 0) {
        --waste_selected;
        return true;
      }
    }
    if (event == Event::ArrowDown) {
      if (tab_index == 0 && cost_selected + 1 < static_cast<int>(current_costs.size())) {
        ++cost_selected;
        return true;
      }
      if (tab_index == 2 && waste_selected + 1 < static_cast<int>(waste.size())) {
        ++waste_selected;
        return true;
      }
    }
    if (event == Event::Character('r')) {
      loaded = false;
      status = "Reloading Azure cloud data...";
      std::jthread reloader(fetch_data);
      reloader.detach();
      return true;
    }
    return false;
  });

  screen.Loop(event_handler);
  return 0;
}

} // namespace azdash
