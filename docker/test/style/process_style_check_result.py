#!/usr/bin/env python3

import os
import logging
import argparse
import csv


def process_result(result_folder):
    status = "success"
    description = ""
    test_results = []
    checks = (
        "duplicate includes",
        "shellcheck",
        "style",
        "black",
        "mypy",
        "typos",
        "whitespaces",
        "workflows",
        "submodules",
        "docs spelling",
    )

    for name in checks:
        out_file = name.replace(" ", "_") + "_output.txt"
        full_path = os.path.join(result_folder, out_file)
        if not os.path.exists(full_path):
            logging.info("No %s check log on path %s", name, full_path)
            return "exception", f"No {name} check log", []
        elif os.stat(full_path).st_size != 0:
            description += f"Check {name} failed. "
            test_results.append((f"Check {name}", "FAIL"))
            status = "failure"
        else:
            test_results.append((f"Check {name}", "OK"))

    if not description:
        description += "Style check success"

    return status, description, test_results


def write_results(results_file, status_file, results, status):
    with open(results_file, "w", encoding="utf-8") as f:
        out = csv.writer(f, delimiter="\t")
        out.writerows(results)
    with open(status_file, "w", encoding="utf-8") as f:
        out = csv.writer(f, delimiter="\t")
        out.writerow(status)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    parser = argparse.ArgumentParser(
        description="ClickHouse script for parsing results of style check"
    )
    default_dir = "/test_output"
    parser.add_argument("--in-results-dir", default=default_dir)
    parser.add_argument("--out-results-file", default=f"{default_dir}/test_results.tsv")
    parser.add_argument("--out-status-file", default=f"{default_dir}/check_status.tsv")
    args = parser.parse_args()

    state, description, test_results = process_result(args.in_results_dir)
    logging.info("Result parsed")
    status = (state, description)
    write_results(args.out_results_file, args.out_status_file, test_results, status)
    logging.info("Result written")
