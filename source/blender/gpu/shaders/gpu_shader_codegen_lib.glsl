/* SPDX-FileCopyrightText: 2020-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "gpu_glsl_cpp_stubs.hh"

float3 calc_barycentric_distances(float3 pos0, float3 pos1, float3 pos2)
{
  float3 edge21 = pos2 - pos1;
  float3 edge10 = pos1 - pos0;
  float3 edge02 = pos0 - pos2;
  float3 d21 = normalize(edge21);
  float3 d10 = normalize(edge10);
  float3 d02 = normalize(edge02);

  float3 dists;
  float d = dot(d21, edge02);
  dists.x = sqrt(dot(edge02, edge02) - d * d);
  d = dot(d02, edge10);
  dists.y = sqrt(dot(edge10, edge10) - d * d);
  d = dot(d10, edge21);
  dists.z = sqrt(dot(edge21, edge21) - d * d);
  return dists;
}

float2 calc_barycentric_co(int vertid)
{
  float2 bary;
  bary.x = float((vertid % 3) == 0);
  bary.y = float((vertid % 3) == 1);
  return bary;
}

#ifdef HAIR_SHADER

/* Hairs uv and col attributes are passed by bufferTextures. */
#  define DEFINE_ATTR(type, attr) uniform samplerBuffer attr
#  define GET_ATTR(type, attr) hair_get_customdata_##type(attr)

#  define barycentric_get() hair_get_barycentric()
#  define barycentric_resolve(bary) hair_resolve_barycentric(bary)

float3 orco_get(float3 local_pos,
                float4x4 modelmatinv,
                float4 orco_madd[2],
                const samplerBuffer orco_samp)
{
  /* TODO: fix ORCO with modifiers. */
  float3 orco = (modelmatinv * float4(local_pos, 1.0f)).xyz;
  return orco_madd[0].xyz + orco * orco_madd[1].xyz;
}

float hair_len_get(int id, const samplerBuffer len)
{
  return texelFetch(len, id).x;
}

float4 tangent_get(const samplerBuffer attr, float3x3 normalmat)
{
  /* Unsupported */
  return float4(0.0f);
}

#else /* MESH_SHADER */

#  define DEFINE_ATTR(type, attr) in type attr
#  define GET_ATTR(type, attr) attr

/* Calculated in geom shader later with calc_barycentric_co. */
#  define barycentric_get() float2(0)
#  define barycentric_resolve(bary) bary

float3 orco_get(float3 local_pos, float4x4 modelmatinv, float4 orco_madd[2], float4 orco)
{
  /* If the object does not have any deformation, the orco layer calculation is done on the fly
   * using the orco_madd factors.
   * We know when there is no orco layer when orco.w is 1.0 because it uses the generic vertex
   * attribute (which is [0,0,0,1]). */
  if (orco.w == 0.0f) {
    return orco.xyz * 0.5f + 0.5f;
  }
  else {
    return orco_madd[0].xyz + local_pos * orco_madd[1].xyz;
  }
}

float hair_len_get(int id, const float len)
{
  return len;
}

float4 tangent_get(float4 attr, float3x3 normalmat)
{
  float4 tangent;
  tangent.xyz = normalmat * attr.xyz;
  tangent.w = attr.w;
  float len_sqr = dot(tangent.xyz, tangent.xyz);
  /* Normalize only if vector is not null. */
  if (len_sqr > 0.0f) {
    tangent.xyz *= inversesqrt(len_sqr);
  }
  return tangent;
}

#endif

/* Assumes GPU_VEC4 is color data, special case that needs luminance coefficients from OCIO. */
#define float_from_vec4(v, luminance_coefficients) dot(v.rgb, luminance_coefficients)
#define float_from_vec3(v) ((v.r + v.g + v.b) * (1.0f / 3.0f))
#define float_from_vec2(v) v.r

#define vec2_from_vec4(v) float2(((v.r + v.g + v.b) * (1.0f / 3.0f)), v.a)
#define vec2_from_vec3(v) float2(((v.r + v.g + v.b) * (1.0f / 3.0f)), 1.0f)
#define vec2_from_float(v) float2(v)

#define vec3_from_vec4(v) v.rgb
#define vec3_from_vec2(v) v.rrr
#define vec3_from_float(v) float3(v)

#define vec4_from_vec3(v) float4(v, 1.0f)
#define vec4_from_vec2(v) v.rrrg
#define vec4_from_float(v) float4(float3(v), 1.0f)

/* TODO: Move to shader_shared. */
#define RAY_TYPE_CAMERA 0
#define RAY_TYPE_SHADOW 1
#define RAY_TYPE_DIFFUSE 2
#define RAY_TYPE_GLOSSY 3

#ifdef GPU_FRAGMENT_SHADER
#  define FrontFacing gl_FrontFacing
#else
#  define FrontFacing true
#endif

/* Can't use enum here because not a header file. But would be great to do. */
enum ClosureType : uchar {
  CLOSURE_NONE_ID = 0u,
  /* Diffuse */
  CLOSURE_BSDF_DIFFUSE_ID = 1u,
  // CLOSURE_BSDF_OREN_NAYAR_ID = 2u,   /* TODO */
  // CLOSURE_BSDF_SHEEN_ID = 4u,        /* TODO */
  // CLOSURE_BSDF_DIFFUSE_TOON_ID = 5u, /* TODO */
  CLOSURE_BSDF_TRANSLUCENT_ID = 6u,

  /* Glossy */
  CLOSURE_BSDF_MICROFACET_GGX_REFLECTION_ID = 7u,
  // CLOSURE_BSDF_ASHIKHMIN_SHIRLEY_ID = 8u, /* TODO */
  // CLOSURE_BSDF_ASHIKHMIN_VELVET_ID = 9u,  /* TODO */
  // CLOSURE_BSDF_GLOSSY_TOON_ID = 10u,      /* TODO */
  // CLOSURE_BSDF_HAIR_REFLECTION_ID = 11u,  /* TODO */

  /* Transmission */
  CLOSURE_BSDF_MICROFACET_GGX_REFRACTION_ID = 12u,

  /* Glass */
  // CLOSURE_BSDF_HAIR_HUANG_ID = 13u, /* TODO */

  /* BSSRDF */
  CLOSURE_BSSRDF_BURLEY_ID = 14u,
};

struct ClosureUndetermined {
  packed_float3 color;
  float weight;
  packed_float3 N;
  ClosureType type;
  /* Additional data different for each closure type. */
  packed_float4 data;
};

ClosureUndetermined closure_new(ClosureType type)
{
  ClosureUndetermined cl;
  cl.type = type;
  return cl;
}

struct ClosureOcclusion {
  packed_float3 N;
};

struct ClosureDiffuse {
  packed_float3 color;
  float weight;
  packed_float3 N;
};

struct ClosureSubsurface {
  packed_float3 color;
  float weight;
  packed_float3 N;
  packed_float3 sss_radius;
};

struct ClosureTranslucent {
  packed_float3 color;
  float weight;
  packed_float3 N;
};

struct ClosureReflection {
  packed_float3 color;
  float weight;
  packed_float3 N;
  float roughness;
};

struct ClosureRefraction {
  packed_float3 color;
  float weight;
  packed_float3 N;
  float roughness;
  float ior;
};

struct ClosureHair {
  packed_float3 color;
  float weight;
  packed_float3 T;
  float offset;
  packed_float2 roughness;
};

struct ClosureVolumeScatter {
  packed_float3 scattering;
  float weight;
  float anisotropy;
};

struct ClosureVolumeAbsorption {
  packed_float3 absorption;
  float weight;
};

struct ClosureEmission {
  packed_float3 emission;
  float weight;
};

struct ClosureTransparency {
  packed_float3 transmittance;
  float weight;
  float holdout;
};

ClosureDiffuse to_closure_diffuse(ClosureUndetermined cl)
{
  ClosureDiffuse closure;
  closure.N = cl.N;
  closure.color = cl.color;
  return closure;
}

ClosureSubsurface to_closure_subsurface(ClosureUndetermined cl)
{
  ClosureSubsurface closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.sss_radius = cl.data.xyz;
  return closure;
}

ClosureTranslucent to_closure_translucent(ClosureUndetermined cl)
{
  ClosureTranslucent closure;
  closure.N = cl.N;
  closure.color = cl.color;
  return closure;
}

ClosureReflection to_closure_reflection(ClosureUndetermined cl)
{
  ClosureReflection closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.roughness = cl.data.x;
  return closure;
}

ClosureRefraction to_closure_refraction(ClosureUndetermined cl)
{
  ClosureRefraction closure;
  closure.N = cl.N;
  closure.color = cl.color;
  closure.roughness = cl.data.x;
  closure.ior = cl.data.y;
  return closure;
}

struct GlobalData {
  /** World position. */
  packed_float3 P;
  /** Surface Normal. Normalized, overridden by bump displacement. */
  packed_float3 N;
  /** Raw interpolated normal (non-normalized) data. */
  packed_float3 Ni;
  /** Geometric Normal. */
  packed_float3 Ng;
  /** Curve Tangent Space. */
  packed_float3 curve_T, curve_B, curve_N;
  /** Barycentric coordinates. */
  packed_float2 barycentric_coords;
  packed_float3 barycentric_dists;
  /** Hair time along hair length. 0 at base 1 at tip. */
  float hair_time;
  /** Hair time along width of the hair. */
  float hair_time_width;
  /** Hair thickness in world space. */
  float hair_thickness;
  /** Index of the strand for per strand effects. */
  int hair_strand_id;
  /** Ray properties (approximation). */
  float ray_depth;
  float ray_length;
  uchar ray_type;
  /** Is hair. */
  bool is_strand;
};

GlobalData g_data;

#ifndef GPU_FRAGMENT_SHADER
/* Stubs. */

#  define dF_impl(a) (float3(0.0f))
#  define dF_branch(a, b, c) (c = float2(0.0f))
#  define dF_branch_incomplete(a, b, c) (c = float2(0.0f))

#elif defined(GPU_FAST_DERIVATIVE) /* TODO(@fclem): User Option? */
/* Fast derivatives */
float3 dF_impl(float3 v)
{
  return float3(0.0f);
}

void dF_branch(float fn, out float2 result)
{
  /* NOTE: this function is currently unused, once it is used we need to check if
   * `g_derivative_filter_width` needs to be applied. */
  result.x = gpu_dfdx(fn) * derivative_scale_get();
  result.y = gpu_dfdy(fn) * derivative_scale_get();
}

#else

/* Offset of coordinates for evaluating bump node. Unit in pixel. */
float g_derivative_filter_width = 0.0f;
/* Precise derivatives */
int g_derivative_flag = 0;

float3 dF_impl(float3 v)
{
  if (g_derivative_flag > 0) {
    return gpu_dfdx(v) * g_derivative_filter_width;
  }
  else if (g_derivative_flag < 0) {
    return gpu_dfdy(v) * g_derivative_filter_width;
  }
  return float3(0.0f);
}

#  define dF_branch(fn, filter_width, result) \
    if (true) { \
      g_derivative_filter_width = filter_width; \
      g_derivative_flag = 1; \
      result.x = (fn); \
      g_derivative_flag = -1; \
      result.y = (fn); \
      g_derivative_flag = 0; \
      result -= float2((fn)); \
    }

/* Used when the non-offset value is already computed elsewhere */
#  define dF_branch_incomplete(fn, filter_width, result) \
    if (true) { \
      g_derivative_filter_width = filter_width; \
      g_derivative_flag = 1; \
      result.x = (fn); \
      g_derivative_flag = -1; \
      result.y = (fn); \
      g_derivative_flag = 0; \
    }
#endif

/* TODO(fclem): Remove. */
#define CODEGEN_LIB
