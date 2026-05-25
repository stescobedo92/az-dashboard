# azdash

`azdash` is an Azure-focused C++23 CLI. It audits Azure spending,
Six-month cost trends, and waste signals from Azure Advisor, plus resource
heuristics, then renders the results as an FTXUI table, JSON, CSV, or a PDF
report.

The implementation is intentionally layered:

- `AzureCliClient` gathers data through the official Azure CLI, so the core is
  easy to test without Azure credentials.
- `analytics` contains template-based generic helpers with concepts for
  aggregation and filtering.
- `render` owns FTXUI, JSON, and CSV presentation.
- `report` writes stakeholder-friendly PDF reports without requiring a browser.
- GTest covers parser, analytics, and report generation behavior.

## Features

- Comparative cost analytics for current month versus the matching window in
  the previous month.
- Six-month trend report with optional service filtering.
- Waste detection from `az advisor recommendation list --category Cost`.
- Extra Azure heuristics for unattached managed disks, orphan public IPs, old
  snapshots, and stopped or deallocated virtual machines.
- Selective scans by check name, for example `compute network advisor`.
- Output formats: `table`, `json`, and `csv`.
- PDF reports for cost, trend, and waste workflows.
- CMake, vcpkg manifest mode, Docker, CI build/test, and vcpkg port publishing
  workflow.

## Requirements

- C++23 compiler.
- CMake 3.25 or newer.
- Ninja.
- vcpkg.
- Azure CLI authenticated with `az login`.

Azure cost data is read with `az consumption usage list`; Advisor data is read
with `az advisor recommendation list`. The Azure CLI documentation currently
marks Advisor recommendations as GA and Consumption as preview.

## Build

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

cmake --build build
ctest --test-dir build --output-on-failure
```

## Docker

```bash
docker build -t azdash .
docker run --rm -it -v "$HOME/.azure:/home/azdash/.azure:ro" azdash cost
```

## Usage

```bash
# Cost comparison: current month vs previous matching window
azdash cost

# JSON or CSV output
azdash --output json cost
azdash --output csv waste advisor compute

# Use a specific subscription
azdash --subscription "00000000-0000-0000-0000-000000000000" trend

# Trend for selected services
azdash trend "Virtual Machines" "Storage"

# Waste checks
azdash waste
azdash waste advisor compute network

# PDF reports
azdash report cost --path ./reports
azdash report trend "Virtual Machines" --path ./reports/trend.pdf
azdash report waste compute network --path ./reports/waste.pdf

# Local version and update guidance
azdash version
azdash update
```
4. Add focused GTest coverage for parsing, filtering, and rendering behavior.
5. Document public methods with C++ documentation comments.
