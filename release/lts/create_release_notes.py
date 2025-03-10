#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
This python script is used to generate the release notes and
download URLs which we can copy-paste directly into the CMS of
www.blender.org and stores.
"""
__all__ = (
    "main",
)

import argparse
import sys

import lts_issue
import lts_download


def main() -> int:
    # Parse arguments
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--version",
        required=True,
        help="Version string in the form of {major}.{minor}.{patch} (e.g. 3.3.2)")
    parser.add_argument(
        "--issue",
        help="Task that contains the release notes information (e.g. #77348)")
    parser.add_argument(
        "--format",
        help="Format the result in `text`, `steam`, `markdown` or `html`",
        default="text")
    args = parser.parse_args()

    # Determine issue number
    version = args.version
    issue = args.issue
    if not issue:
        if version.startswith("2.83."):
            issue = "#77348"
        elif version.startswith("2.93."):
            issue = "#88449"
        elif version.startswith("3.3."):
            issue = "#100749"
        elif version.startswith("3.6."):
            issue = "#109399"
        elif version.startswith("4.2."):
            issue = "#124452"
        else:
            raise ValueError("Specify --issue or update script to include issue number for this version")

    # Print
    if args.format == "html":
        lts_download.print_urls(version=version)
        print("")

    lts_issue.print_notes(version=version, format=args.format, issue=issue)


if __name__ == "__main__":
    sys.exit(main())
