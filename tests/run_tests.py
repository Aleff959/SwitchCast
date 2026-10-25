#!/usr/bin/env python3
"""
Unified Test Runner for SwitchCast v2.4.0 (Comp. 31) E2E Test Suite.
Executes all test modules, calculates Tier coverage and Feature metrics,
and produces a structured summary report.
Exits with 0 if all tests pass, or 1 if any failure/error occurs.
"""

import sys
import os
import time
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TESTS_DIR.parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


class StructuredTestResult(unittest.TestResult):
    """Custom TestResult tracking Tier and Feature classifications."""

    def __init__(self, stream=None, descriptions=None, verbosity=1):
        super().__init__(stream, descriptions, verbosity)
        self.test_records = []
        self.tier_counts = {"Tier 1": 0, "Tier 2": 0, "Tier 3": 0, "Tier 4": 0, "Other": 0}
        self.tier_passed = {"Tier 1": 0, "Tier 2": 0, "Tier 3": 0, "Tier 4": 0, "Other": 0}
        self.feature_counts = {f"F{i}": 0 for i in range(1, 9)}
        self.start_times = {}

    def startTest(self, test):
        super().startTest(test)
        self.start_times[test.id()] = time.perf_counter()

    def _classify_test(self, test):
        name = test._testMethodName
        doc = test._testMethodDoc or ""

        # Determine Tier
        if "tier1" in name.lower() or "Tier 1" in doc:
            tier = "Tier 1"
        elif "tier2" in name.lower() or "Tier 2" in doc:
            tier = "Tier 2"
        elif "pairwise" in name.lower() or "tier3" in name.lower() or "P" in name:
            tier = "Tier 3"
        elif "scenario" in name.lower() or "tier4" in name.lower() or "S" in name:
            tier = "Tier 4"
        else:
            tier = "Other"

        # Determine Features
        features = []
        for i in range(1, 9):
            pattern = f"F{i}"
            if pattern in name or pattern in doc:
                features.append(pattern)

        return tier, features

    def addSuccess(self, test):
        super().addSuccess(test)
        elapsed = time.perf_counter() - self.start_times.get(test.id(), time.perf_counter())
        tier, features = self._classify_test(test)
        self.tier_counts[tier] += 1
        self.tier_passed[tier] += 1
        for f in features:
            self.feature_counts[f] += 1
        self.test_records.append((test.id(), "PASS", elapsed, None))

    def addFailure(self, test, err):
        super().addFailure(test, err)
        elapsed = time.perf_counter() - self.start_times.get(test.id(), time.perf_counter())
        tier, features = self._classify_test(test)
        self.tier_counts[tier] += 1
        for f in features:
            self.feature_counts[f] += 1
        self.test_records.append((test.id(), "FAIL", elapsed, err))

    def addError(self, test, err):
        super().addError(test, err)
        elapsed = time.perf_counter() - self.start_times.get(test.id(), time.perf_counter())
        tier, features = self._classify_test(test)
        self.tier_counts[tier] += 1
        for f in features:
            self.feature_counts[f] += 1
        self.test_records.append((test.id(), "ERROR", elapsed, err))


def run_all_tests():
    """Discovers and runs all test modules in tests/ directory."""
    print("=" * 80)
    print(" SWITCHCAST v2.4.0 (COMP. 31) - OPAQUE-BOX E2E TEST RUNNER")
    print("=" * 80)
    print(f"Target Working Directory: {REPO_ROOT}")
    print(f"Test Harness Directory:   {TESTS_DIR}")
    print("-" * 80)

    loader = unittest.TestLoader()
    suite = unittest.TestSuite()

    test_modules = [
        "test_versioning_and_manifest",
        "test_webremote_api",
        "test_ota_pipeline",
        "test_media_and_wallpaper",
        "test_scenarios_and_combinations"
    ]

    for mod_name in test_modules:
        try:
            mod_suite = loader.loadTestsFromName(mod_name)
            suite.addTests(mod_suite)
            print(f" [+] Loaded test module: {mod_name} ({mod_suite.countTestCases()} test cases)")
        except Exception as e:
            print(f" [!] Error loading module {mod_name}: {e}")
            return 1

    print("-" * 80)
    print(f" Total test cases scheduled: {suite.countTestCases()}")
    print(" Executing test suites...\n")

    start_wall = time.perf_counter()
    result = StructuredTestResult()
    suite.run(result)
    total_time = time.perf_counter() - start_wall

    # Print test results summary
    print("=" * 80)
    print(" TEST EXECUTION SUMMARY")
    print("=" * 80)
    print(f"{'Module / Suite':<40} | {'Tests':<6} | {'Passed':<6} | {'Failed':<6} | {'Errors':<6}")
    print("-" * 80)

    for mod_name in test_modules:
        mod_records = [r for r in result.test_records if mod_name in r[0]]
        total_m = len(mod_records)
        passed_m = sum(1 for r in mod_records if r[1] == "PASS")
        failed_m = sum(1 for r in mod_records if r[1] == "FAIL")
        errors_m = sum(1 for r in mod_records if r[1] == "ERROR")
        print(f"{mod_name:<40} | {total_m:<6} | {passed_m:<6} | {failed_m:<6} | {errors_m:<6}")

    print("-" * 80)
    total_tests = result.testsRun
    total_passed = len(result.test_records) - len(result.failures) - len(result.errors)
    total_failed = len(result.failures)
    total_errors = len(result.errors)
    print(f"{'TOTAL':<40} | {total_tests:<6} | {total_passed:<6} | {total_failed:<6} | {total_errors:<6}")
    print("=" * 80)

    # Print Tier Breakdown
    print(" TIER COVERAGE BREAKDOWN")
    print("-" * 80)
    print(f"{'Tier':<25} | {'Required':<10} | {'Executed':<10} | {'Passed':<10} | {'Status'}")
    print("-" * 80)

    tier_specs = [
        ("Tier 1 (Happy Path)", ">= 40", result.tier_counts["Tier 1"], result.tier_passed["Tier 1"], result.tier_counts["Tier 1"] >= 40 and result.tier_passed["Tier 1"] == result.tier_counts["Tier 1"]),
        ("Tier 2 (Boundaries)", ">= 40", result.tier_counts["Tier 2"], result.tier_passed["Tier 2"], result.tier_counts["Tier 2"] >= 40 and result.tier_passed["Tier 2"] == result.tier_counts["Tier 2"]),
        ("Tier 3 (Pairwise)", ">= 8", result.tier_counts["Tier 3"], result.tier_passed["Tier 3"], result.tier_counts["Tier 3"] >= 8 and result.tier_passed["Tier 3"] == result.tier_counts["Tier 3"]),
        ("Tier 4 (Scenarios)", ">= 5", result.tier_counts["Tier 4"], result.tier_passed["Tier 4"], result.tier_counts["Tier 4"] >= 5 and result.tier_passed["Tier 4"] == result.tier_counts["Tier 4"]),
    ]

    all_tiers_ok = True
    for name, req, count, passed, status in tier_specs:
        status_str = "PASS" if status else "FAIL"
        if not status:
            all_tiers_ok = False
        print(f"{name:<25} | {req:<10} | {count:<10} | {passed:<10} | {status_str}")

    print("-" * 80)
    print(f"Total Tests Executed: {total_tests} (Threshold >= 93)")
    print(f"Total Time:           {total_time:.3f} seconds")

    if result.failures:
        print("\n" + "!" * 80)
        print(" FAILURES DETAILS:")
        print("!" * 80)
        for test, trace in result.failures:
            print(f"\n[FAIL] {test.id()}:\n{trace}")

    if result.errors:
        print("\n" + "!" * 80)
        print(" ERRORS DETAILS:")
        print("!" * 80)
        for test, trace in result.errors:
            print(f"\n[ERROR] {test.id()}:\n{trace}")

    if result.wasSuccessful() and total_tests >= 93 and all_tiers_ok:
        print("\n" + "=" * 80)
        print(" >>> ALL TESTS PASSED! E2E SUITE VERIFICATION SUCCESSFUL <<<")
        print("=" * 80)
        return 0
    else:
        print("\n" + "=" * 80)
        print(" >>> TEST RUN FAILED OR THRESHOLDS NOT MET <<<")
        print("=" * 80)
        return 1


if __name__ == "__main__":
    sys.exit(run_all_tests())
