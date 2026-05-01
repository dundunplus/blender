/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup blt
 *
 * This allows converting std::tm datetime structures to localized date and
 * time strings. The localization is based on the CLDR from the current locale.
 */

#include <ctime>
#include <string>

#include "BLI_string_ref.hh"

namespace blender::date_string {

enum class DateFormat : uint8_t {
  Default = 0, /* Convention based on output language. */
  LE_Slash,    /* dd/mm/yyyy */
  LE_Dot,      /* dd.mm.yyyy */
  LE_Dash,     /* dd-mm-yyyy */
  ME_Slash,    /* mm/dd/yyyy */
  BE_Slash,    /* yyyy/mm/dd */
  BE_Dot,      /* yyyy.mm.dd */
  BE_Dash,     /* yyyy-mm-dd */
};

enum class TimeFormat : uint8_t {
  H24 = 0, /* 23:59 */
  H12,     /* 11:59 PM */
};

std::string date(const std::tm *date_time,
                 const StringRef locale_iso = {},
                 DateFormat format = DateFormat::Default);

std::string time(const std::tm *date_time, TimeFormat format = TimeFormat::H24);

std::string datetime(const std::tm *date_time,
                     const StringRef locale_iso = {},
                     DateFormat date_format = DateFormat::Default,
                     TimeFormat time_format = TimeFormat::H24,
                     const std::tm *now = nullptr,
                     const StringRef today = {},
                     const StringRef yesterday = {});

}  // namespace blender::date_string
