/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_implicit_sharing_ptr.hh"

namespace blender::nodes {

class GList;
using GListPtr = ImplicitSharingPtr<GList>;

template<typename T> class List;
template<typename T> using ListPtr = ImplicitSharingPtr<List<T>>;

}  // namespace blender::nodes
