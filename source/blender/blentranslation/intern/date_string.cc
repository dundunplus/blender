/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** \file
 * \ingroup blt
 */

#include <algorithm>
#include <array>
#include <iomanip>
#include <string>

#include <fmt/format.h>

#include "BLT_date_string.hh"

#include "BLT_translation.hh"

namespace blender::date_string {

struct LocalePatterns {
  StringRef locale;
  StringRef date;
};

static const LocalePatterns *get_locale_patterns(const StringRef locale_iso)
{
  static constexpr std::array<LocalePatterns, 9> patterns = {{
      {"", "{d:02} {b} {Y}"},         /* default */
      {"en_US", "{d:02} {b} {Y}"},    /* English (US) */
      {"ar_EG", "{d:02} {b} {Y}"},    /* Arabic (Egypt) */
      {"zh_HANS", "{Y}年{m}月{d}日"}, /* Chinese (Simplified) */
      {"zh_HANT", "{Y}年{m}月{d}日"}, /* Chinese (Traditional) */
      {"hu_HU", "{Y}. {b} {d:02}"},   /* Hungarian */
      {"ja_JP", "{Y}年{m}月{d}日"},   /* Japanese */
      {"ko_KR", "{Y}년 {m}월 {d}일"}, /* Korean */
      {"ur", "{d:02} {b} {Y}"},       /* Urdu */
  }};

  for (const LocalePatterns &pattern : patterns) {
    if (pattern.locale == locale_iso) {
      return &pattern;
    }
  }

  /* Fallback default pattern (index 0). */
  return &patterns[0];
}

std::string time(const std::tm *date_time, TimeFormat format)
{
  std::string time_format_str;

  if (format == TimeFormat::H24) {
    time_format_str = "{H:02}:{M:02}";
  }
  else if (format == TimeFormat::H12) {
    time_format_str = "{I}:{M:02} {p}";
  }

  return fmt::format(
      fmt::runtime(time_format_str),
      fmt::arg("H", date_time->tm_hour),
      fmt::arg("M", date_time->tm_min),
      fmt::arg("S", date_time->tm_sec),
      fmt::arg("I", (date_time->tm_hour % 12) == 0 ? 12 : (date_time->tm_hour % 12)),
      fmt::arg("p",
               (date_time->tm_hour < 12) ? CTX_IFACE_(BLT_I18NCONTEXT_TIME, "AM") :
                                           CTX_IFACE_(BLT_I18NCONTEXT_TIME, "PM")));
}

std::string date(const std::tm *date_time, const StringRef locale_iso, DateFormat format)
{
  static constexpr std::array<StringRef, 12> months = {CTX_N_(BLT_I18NCONTEXT_TIME, "Jan"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Feb"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Mar"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Apr"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "May"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Jun"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Jul"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Aug"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Sep"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Oct"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Nov"),
                                                       CTX_N_(BLT_I18NCONTEXT_TIME, "Dec")};

  std::string date_format_str;

  BLI_assert(date_time->tm_mon >= 0 && date_time->tm_mon < 12);
  const int month_index = std::clamp(date_time->tm_mon, 0, 11);

  if (format == DateFormat::Default) {
    const LocalePatterns *pattern = get_locale_patterns(locale_iso);
    date_format_str = pattern->date;
  }
  else if (format == DateFormat::LE_Slash) {
    date_format_str = "{d:02}/{m:02}/{Y}";
  }
  else if (format == DateFormat::LE_Dot) {
    date_format_str = "{d:02}.{m:02}.{Y}";
  }
  else if (format == DateFormat::LE_Dash) {
    date_format_str = "{d:02}-{m:02}-{Y}";
  }
  else if (format == DateFormat::ME_Slash) {
    date_format_str = "{m:02}/{d:02}/{Y}";
  }
  else if (format == DateFormat::BE_Slash) {
    date_format_str = "{Y}/{m:02}/{d:02}";
  }
  else if (format == DateFormat::BE_Dot) {
    date_format_str = "{Y}.{m:02}.{d:02}";
  }
  else if (format == DateFormat::BE_Dash) {
    date_format_str = "{Y}-{m:02}-{d:02}";
  }

  return fmt::format(fmt::runtime(date_format_str),
                     fmt::arg("Y", date_time->tm_year + 1900),
                     fmt::arg("m", date_time->tm_mon + 1),
                     fmt::arg("b", CTX_IFACE_(BLT_I18NCONTEXT_TIME, months[month_index])),
                     fmt::arg("d", date_time->tm_mday));
}

std::string datetime(const std::tm *datetime,
                     const StringRef locale_iso,
                     DateFormat date_format,
                     TimeFormat time_format,
                     const std::tm *now,
                     const StringRef today,
                     const StringRef yesterday)
{
  bool is_today = false;
  bool is_yesterday = false;
  if (now && !today.is_empty() && !yesterday.is_empty()) {
    is_today = (datetime->tm_yday == now->tm_yday && datetime->tm_year == now->tm_year);
    std::tm yesterday_tm = *now;
    yesterday_tm.tm_mday--;
    mktime(&yesterday_tm);
    is_yesterday = (datetime->tm_yday == yesterday_tm.tm_yday &&
                    datetime->tm_year == yesterday_tm.tm_year);
  }

  const std::string time_s = time(datetime, time_format);

  if (is_today) {
    return std::string(today) + " " + time_s;
  }
  else if (is_yesterday) {
    return std::string(yesterday) + " " + time_s;
  }
  else {
    const std::string date_s = date(datetime, locale_iso, date_format);
    return date_s + " " + time_s;
  }
}

}  // namespace blender::date_string
