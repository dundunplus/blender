/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <string>

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

namespace blender::dna {

struct ParsedMember {
  /** Single-identifier type name, e.g. `int`, `float`, `Material`. */
  std::string type_name;
  /** Canonical full member name, e.g. `*var`, `arr[4]`, `(*func)()`. */
  std::string member_name;
  /** Required alignment in bytes. */
  int alignment = 0;
};

struct ParsedStruct {
  std::string type_name;
  Vector<ParsedMember> members;
};

/** Extract structs and their members from a DNA header. */
[[nodiscard]] bool parse_dna_header(StringRefNull filepath, Vector<ParsedStruct> &r_structs);

/** Convert C++ types to plain C types understood by DNA. */
void substitute_cpp_types(Vector<ParsedStruct> &structs);

}  // namespace blender::dna
