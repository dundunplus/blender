/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "common_view_clipping_lib.glsl"
#include "common_view_lib.glsl"

void main()
{
  /* Disabled per bone wire width due to drawing issues on Mac. See #124691. */
  vec4 bone_color, state_color;
  mat4 model_mat = extract_matrix_packed_data(inst_obmat, state_color, bone_color);

  vec3 world_pos = (model_mat * vec4(pos, 1.0)).xyz;
  gl_Position = point_world_to_ndc(world_pos);

  finalColor.rgb = mix(state_color.rgb, bone_color.rgb, 0.5);
  finalColor.a = 1.0;

  edgeStart = edgePos = ((gl_Position.xy / gl_Position.w) * 0.5 + 0.5) * sizeViewport.xy;

  view_clipping_distances(world_pos);
}
