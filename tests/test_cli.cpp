// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The command-line tool, driven as a real operating-system process through a
// complete lifecycle: initialise, declare evidence, query, verify, recover,
// prune. The suite runs the built executable, not a linked function, so it
// exercises argument handling and process exit codes as an operator would.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "child_process.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

#ifndef FCAP_TEST_CLI_PATH
#define FCAP_TEST_CLI_PATH ""
#endif

namespace {

namespace fs = std::filesystem;

bool cli_available() {
  const std::string path = FCAP_TEST_CLI_PATH;
  return !path.empty() && fs::exists(fs::path(path));
}

/// Runs the tool once and returns its exit status, appending output to
/// `log`, which is truncated first.
int run_cli(const std::vector<std::string>& arguments, const fs::path& log) {
  fsup::write_bytes(log, std::string());
  ftest::ChildProcess child = ftest::ChildProcess::start_program(fs::path(FCAP_TEST_CLI_PATH), arguments, log);
  return child.wait();
}

std::string cli_output(const fs::path& log) { return fsup::read_bytes(log); }

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

FT_TEST(cli, version_and_help) {
  if (!cli_available()) {
    FT_FAIL("the fcap tool was not built into this configuration");
  }
  fsup::TempDir directory{"cli"};
  const fs::path log = directory.file("out.txt");

  FT_CHECK_EQ(run_cli({"version"}, log), 0);
  FT_CHECK(contains(cli_output(log), "facility-capacity 1.0.0"));

  FT_CHECK_EQ(run_cli({"--help"}, log), 0);
  FT_CHECK(contains(cli_output(log), "usage"));

  // A usage error must be reported as such and must print nothing to stdout
  // that could be mistaken for a result.
  FT_CHECK_EQ(run_cli({}, log), 2);
  FT_CHECK_EQ(run_cli({"not-a-command"}, log), 2);
  FT_CHECK_EQ(run_cli({"store"}, log), 2);
}

FT_TEST(cli, full_lifecycle_through_the_store) {
  if (!cli_available()) {
    FT_FAIL("the fcap tool was not built into this configuration");
  }
  fsup::TempDir directory{"cli-lifecycle"};
  const fs::path root = directory.store_root();
  const fs::path log = directory.file("out.txt");

  FT_CHECK_EQ(run_cli({"store", "init", "--path", root.string(), "--facility", "facility-a", "--site",
                       "site-1", "--epoch", "7", "--incarnation", "3", "--controller", "controller-1"},
                      log),
              0);
  const std::string init_output = cli_output(log);
  FT_CHECK(contains(init_output, "facility-a"));
  FT_CHECK(fs::exists(root / "FORMAT"));
  FT_CHECK(fs::exists(root / "CURRENT"));

  // Initialising twice is refused.
  FT_CHECK_EQ(run_cli({"store", "init", "--path", root.string(), "--facility", "facility-a", "--site",
                       "site-1", "--epoch", "7", "--incarnation", "3", "--controller", "controller-1"},
                      log),
              1);
  FT_CHECK(contains(cli_output(log), "error: already_exists"));

  FT_CHECK_EQ(run_cli({"store", "info", "--path", root.string()}, log), 0);
  FT_CHECK(contains(cli_output(log), "facility-a"));

  FT_CHECK_EQ(run_cli({"evidence", "declare", "--path", root.string(), "--source", "power-capacity",
                       "--dimension", "power", "--generation", "1", "--installed", "1000000", "--usable",
                       "900000", "--unavailable", "40000", "--protected", "0", "--reserved", "0"},
                      log),
              0);
  const std::string declare_output = cli_output(log);
  FT_CHECK(contains(declare_output, "generation"));
  FT_CHECK(contains(declare_output, "power"));

  FT_CHECK_EQ(run_cli({"store", "verify", "--path", root.string(), "--deep"}, log), 0);
  FT_CHECK(contains(cli_output(log), "ok=1"));

  FT_CHECK_EQ(run_cli({"store", "generations", "--path", root.string()}, log), 0);
  FT_CHECK(contains(cli_output(log), "gen-1.fcs"));

  FT_CHECK_EQ(run_cli({"query", "--path", root.string(), "--dimension", "power"}, log), 0);
  FT_CHECK(contains(cli_output(log), "usable"));

  FT_CHECK_EQ(run_cli({"snapshot", "show", "--path", root.string()}, log), 0);
  FT_CHECK(contains(cli_output(log), "completeness="));

  FT_CHECK_EQ(run_cli({"snapshot", "explain", "--path", root.string(), "--dimension", "power"}, log), 0);

  FT_CHECK_EQ(run_cli({"store", "recover", "--path", root.string()}, log), 0);
  const std::string recover_output = cli_output(log);
  FT_CHECK(contains(recover_output, "recovered"));
  FT_CHECK(contains(recover_output, "recovered_not_revalidated"));

  FT_CHECK_EQ(run_cli({"revalidate", "--path", root.string()}, log), 0);
  FT_CHECK(contains(cli_output(log), "status="));

  FT_CHECK_EQ(run_cli({"store", "prune", "--path", root.string(), "--keep", "1"}, log), 0);

  // A dimension the facility does not offer is answered as unavailable, and
  // the tool still succeeds: an unavailable answer is an answer.
  FT_CHECK_EQ(run_cli({"query", "--path", root.string(), "--dimension", "cooling"}, log), 0);
  FT_CHECK(contains(cli_output(log), "unavailable"));
}

FT_TEST(cli, failures_are_stable_and_machine_readable) {
  if (!cli_available()) {
    FT_FAIL("the fcap tool was not built into this configuration");
  }
  fsup::TempDir directory{"cli-failures"};
  const fs::path log = directory.file("out.txt");

  // A store that does not exist.
  FT_CHECK_EQ(run_cli({"store", "info", "--path", directory.file("absent").string()}, log), 1);
  const std::string missing = cli_output(log);
  FT_CHECK(contains(missing, "error: not_found"));

  // An identifier that is not acceptable.
  FT_CHECK_EQ(run_cli({"store", "init", "--path", directory.store_root().string(), "--facility",
                       "../escape", "--site", "site-1", "--epoch", "7", "--incarnation", "3",
                       "--controller", "controller-1"},
                      log),
              1);
  FT_CHECK(contains(cli_output(log), "error: invalid_argument"));

  // A quantity beyond the accepted bound is refused before it reaches the
  // library.
  FT_CHECK_EQ(run_cli({"store", "init", "--path", directory.store_root("second").string(), "--facility",
                       "facility-a", "--site", "site-1", "--epoch", "7", "--incarnation", "3",
                       "--controller", "controller-1"},
                      log),
              0);
  FT_CHECK_EQ(run_cli({"evidence", "declare", "--path", directory.store_root("second").string(), "--source",
                       "power-capacity", "--dimension", "power", "--generation", "1", "--installed",
                       "99999999999999999999", "--usable", "1"},
                      log),
              2);

  // An unknown option is a usage error, never silently ignored.
  FT_CHECK_EQ(run_cli({"store", "info", "--path", directory.store_root("second").string(), "--typo", "1"}, log),
              2);
}
