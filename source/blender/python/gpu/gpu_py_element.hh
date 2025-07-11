/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bpygpu
 */

#pragma once

#include <Python.h>

namespace blender::gpu {
class IndexBuf;
}

extern PyTypeObject BPyGPUIndexBuf_Type;

#define BPyGPUIndexBuf_Check(v) (Py_TYPE(v) == &BPyGPUIndexBuf_Type)

struct BPyGPUIndexBuf {
  PyObject_VAR_HEAD
  blender::gpu::IndexBuf *elem;
};

[[nodiscard]] PyObject *BPyGPUIndexBuf_CreatePyObject(blender::gpu::IndexBuf *elem);
