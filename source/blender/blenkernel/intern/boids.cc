/* SPDX-FileCopyrightText: 2009 by Janne Karhu. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include <cmath>
#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_object_force_types.h"
#include "DNA_scene_types.h"

#include "BLI_kdtree.h"
#include "BLI_listbase.h"
#include "BLI_math_base_safe.h"
#include "BLI_math_rotation.h"
#include "BLI_math_vector.h"
#include "BLI_rand.h"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_utildefines.h"

#include "BKE_boids.h"
#include "BKE_collision.h"
#include "BKE_effect.h"
#include "BKE_particle.h"
#include "BLI_kdopbvh.hh"

#include "BLT_translation.hh"

#include "BKE_modifier.hh"

#include "RNA_enum_types.hh"

static float len_squared_v3v3_with_normal_bias(const float co_search[3],
                                               const float co_test[3],
                                               const void *user_data)
{
  const float *normal = static_cast<const float *>(user_data);
  float d[3], dist;

  sub_v3_v3v3(d, co_test, co_search);

  dist = len_squared_v3(d);

  /* Avoid head-on collisions. */
  if (dot_v3v3(d, normal) < 0.0f) {
    dist *= 10.0f;
  }
  return dist;
}

struct BoidValues {
  float max_speed, max_acc;
  float max_ave, min_speed;
  float personal_space, jump_speed;
};

static bool apply_boid_rule(
    BoidBrainData *bbd, BoidRule *rule, BoidValues *val, ParticleData *pa, float fuzziness);

static bool rule_none(BoidRule * /*rule*/,
                      BoidBrainData * /*data*/,
                      BoidValues * /*val*/,
                      ParticleData * /*pa*/)
{
  return false;
}

static bool rule_goal_avoid(BoidRule *rule, BoidBrainData *bbd, BoidValues *val, ParticleData *pa)
{
  BoidRuleGoalAvoid *gabr = (BoidRuleGoalAvoid *)rule;
  BoidSettings *boids = bbd->part->boids;
  BoidParticle *bpa = pa->boid;
  EffectedPoint epoint;
  ListBase *effectors = bbd->sim->psys->effectors;
  EffectorCache *eff = nullptr;
  EffectorCache temp_eff;
  EffectorData efd, cur_efd;
  float mul = (rule->type == eBoidRuleType_Avoid ? 1.0 : -1.0);
  float priority = 0.0f, len = 0.0f;
  bool ret = false;

  int p = 0;
  efd.index = cur_efd.index = &p;

  pd_point_from_particle(bbd->sim, pa, &pa->state, &epoint);

  /* first find out goal/predator with highest priority */
  if (effectors) {
    LISTBASE_FOREACH (EffectorCache *, cur, effectors) {
      Object *eob = cur->ob;
      PartDeflect *pd = cur->pd;

      if (gabr->ob && (rule->type != eBoidRuleType_Goal || gabr->ob != bpa->ground)) {
        if (gabr->ob == eob) {
          /* TODO: effectors with multiple points */
          if (get_effector_data(cur, &efd, &epoint, 0)) {
            if (cur->pd && cur->pd->forcefield == PFIELD_BOID) {
              priority = mul * pd->f_strength *
                         effector_falloff(cur, &efd, &epoint, bbd->part->effector_weights);
            }
            else {
              priority = 1.0;
            }

            eff = cur;
          }
          break;
        }
      }
      else if (rule->type == eBoidRuleType_Goal && eob == bpa->ground) {
        /* skip current object */
      }
      else if (pd->forcefield == PFIELD_BOID && mul * pd->f_strength > 0.0f &&
               get_effector_data(cur, &cur_efd, &epoint, 0))
      {
        float temp = mul * pd->f_strength *
                     effector_falloff(cur, &cur_efd, &epoint, bbd->part->effector_weights);

        if (temp == 0.0f) {
          /* do nothing */
        }
        else if (temp > priority) {
          priority = temp;
          eff = cur;
          efd = cur_efd;
          len = efd.distance;
        }
        /* choose closest object with same priority */
        else if (temp == priority && efd.distance < len) {
          eff = cur;
          efd = cur_efd;
          len = efd.distance;
        }
      }
    }
  }

  /* if the object doesn't have effector data we have to fake it */
  if (eff == nullptr && gabr->ob) {
    memset(&temp_eff, 0, sizeof(EffectorCache));
    temp_eff.ob = gabr->ob;
    temp_eff.depsgraph = bbd->sim->depsgraph;
    temp_eff.scene = bbd->sim->scene;
    eff = &temp_eff;
    get_effector_data(eff, &efd, &epoint, 0);
    priority = 1.0f;
  }

  /* Then use that effector. */

  /* With avoid, factor is "fear factor". */
  if (priority > (rule->type == eBoidRuleType_Avoid ? gabr->fear_factor : 0.0f)) {
    Object *eob = eff->ob;
    PartDeflect *pd = eff->pd;
    float surface = (pd && pd->shape == PFIELD_SHAPE_SURFACE) ? 1.0f : 0.0f;

    if (gabr->options & BRULE_GOAL_AVOID_PREDICT) {
      /* estimate future location of target */
      get_effector_data(eff, &efd, &epoint, 1);

      mul_v3_fl(efd.vel, efd.distance / (val->max_speed * bbd->timestep));
      add_v3_v3(efd.loc, efd.vel);
      sub_v3_v3v3(efd.vec_to_point, pa->prev_state.co, efd.loc);
      efd.distance = len_v3(efd.vec_to_point);
    }

    if (rule->type == eBoidRuleType_Goal && boids->options & BOID_ALLOW_CLIMB && surface != 0.0f) {
      if (!bbd->goal_ob || bbd->goal_priority < priority) {
        bbd->goal_ob = eob;
        copy_v3_v3(bbd->goal_co, efd.loc);
        copy_v3_v3(bbd->goal_nor, efd.nor);
      }
    }
    else if ((rule->type == eBoidRuleType_Avoid) && (bpa->data.mode == eBoidMode_Climbing) &&
             (priority > 2.0f * gabr->fear_factor))
    {
      /* detach from surface and try to fly away from danger */
      negate_v3_v3(efd.vec_to_point, bpa->gravity);
    }

    copy_v3_v3(bbd->wanted_co, efd.vec_to_point);
    mul_v3_fl(bbd->wanted_co, mul);

    bbd->wanted_speed = val->max_speed * priority;

    /* with goals factor is approach velocity factor */
    if (rule->type == eBoidRuleType_Goal && boids->landing_smoothness > 0.0f) {
      float len2 = 2.0f * len_v3(pa->prev_state.vel);

      surface *= pa->size * boids->height;

      if (len2 > 0.0f && efd.distance - surface < len2) {
        len2 = (efd.distance - surface) / len2;
        bbd->wanted_speed *= powf(len2, boids->landing_smoothness);
      }
    }

    ret = true;
  }

  return ret;
}

static bool rule_avoid_collision(BoidRule *rule,
                                 BoidBrainData *bbd,
                                 BoidValues *val,
                                 ParticleData *pa)
{
  const int raycast_flag = BVH_RAYCAST_DEFAULT & ~BVH_RAYCAST_WATERTIGHT;
  BoidRuleAvoidCollision *acbr = (BoidRuleAvoidCollision *)rule;
  KDTreeNearest_3d *ptn = nullptr;
  BoidParticle *bpa = pa->boid;
  float vec[3] = {0.0f, 0.0f, 0.0f}, loc[3] = {0.0f, 0.0f, 0.0f};
  float co1[3], vel1[3], co2[3], vel2[3];
  float len, t, inp, t_min = 2.0f;
  int n, neighbors = 0, nearest = 0;
  bool ret = false;

  /* Check deflector objects first. */
  if (acbr->options & BRULE_ACOLL_WITH_DEFLECTORS && bbd->sim->colliders) {
    ParticleCollision col;
    BVHTreeRayHit hit;
    float radius = val->personal_space * pa->size, ray_dir[3];

    memset(&col, 0, sizeof(ParticleCollision));

    copy_v3_v3(col.co1, pa->prev_state.co);
    add_v3_v3v3(col.co2, pa->prev_state.co, pa->prev_state.vel);
    sub_v3_v3v3(ray_dir, col.co2, col.co1);
    mul_v3_fl(ray_dir, acbr->look_ahead);
    col.f = 0.0f;
    hit.index = -1;
    hit.dist = col.original_ray_length = normalize_v3(ray_dir);

    /* find out closest deflector object */
    LISTBASE_FOREACH (ColliderCache *, coll, bbd->sim->colliders) {
      /* don't check with current ground object */
      if (coll->ob == bpa->ground) {
        continue;
      }

      col.current = coll->ob;
      col.md = coll->collmd;

      if (col.md && col.md->bvhtree) {
        BLI_bvhtree_ray_cast_ex(col.md->bvhtree,
                                col.co1,
                                ray_dir,
                                radius,
                                &hit,
                                BKE_psys_collision_neartest_cb,
                                &col,
                                raycast_flag);
      }
    }
    /* then avoid that object */
    if (hit.index >= 0) {
      t = hit.dist / col.original_ray_length;

      /* avoid head-on collision */
      if (dot_v3v3(col.pce.nor, pa->prev_state.ave) < -0.99f) {
        /* don't know why, but uneven range [0.0, 1.0] */
        /* works much better than even [-1.0, 1.0] */
        bbd->wanted_co[0] = BLI_rng_get_float(bbd->rng);
        bbd->wanted_co[1] = BLI_rng_get_float(bbd->rng);
        bbd->wanted_co[2] = BLI_rng_get_float(bbd->rng);
      }
      else {
        copy_v3_v3(bbd->wanted_co, col.pce.nor);
      }

      mul_v3_fl(bbd->wanted_co, (1.0f - t) * val->personal_space * pa->size);

      bbd->wanted_speed = sqrtf(t) * len_v3(pa->prev_state.vel);
      bbd->wanted_speed = std::max(bbd->wanted_speed, val->min_speed);

      return true;
    }
  }

  /* Check boids in their own system. */
  if (acbr->options & BRULE_ACOLL_WITH_BOIDS) {
    neighbors = BLI_kdtree_3d_range_search_with_len_squared_cb(bbd->sim->psys->tree,
                                                               pa->prev_state.co,
                                                               &ptn,
                                                               acbr->look_ahead *
                                                                   len_v3(pa->prev_state.vel),
                                                               len_squared_v3v3_with_normal_bias,
                                                               pa->prev_state.ave);
    if (neighbors > 1) {
      for (n = 1; n < neighbors; n++) {
        copy_v3_v3(co1, pa->prev_state.co);
        copy_v3_v3(vel1, pa->prev_state.vel);
        copy_v3_v3(co2, (bbd->sim->psys->particles + ptn[n].index)->prev_state.co);
        copy_v3_v3(vel2, (bbd->sim->psys->particles + ptn[n].index)->prev_state.vel);

        sub_v3_v3v3(loc, co1, co2);

        sub_v3_v3v3(vec, vel1, vel2);

        inp = dot_v3v3(vec, vec);

        /* velocities not parallel */
        if (inp != 0.0f) {
          t = -dot_v3v3(loc, vec) / inp;
          /* cpa is not too far in the future so investigate further */
          if (t > 0.0f && t < t_min) {
            madd_v3_v3fl(co1, vel1, t);
            madd_v3_v3fl(co2, vel2, t);

            sub_v3_v3v3(vec, co2, co1);

            len = normalize_v3(vec);

            /* distance of cpa is close enough */
            if (len < 2.0f * val->personal_space * pa->size) {
              t_min = t;

              mul_v3_fl(vec, len_v3(vel1));
              mul_v3_fl(vec, (2.0f - t) / 2.0f);
              sub_v3_v3v3(bbd->wanted_co, vel1, vec);
              bbd->wanted_speed = len_v3(bbd->wanted_co);
              ret = true;
            }
          }
        }
      }
    }
  }
  MEM_SAFE_FREE(ptn);

  /* check boids in other systems */
  LISTBASE_FOREACH (ParticleTarget *, pt, &bbd->sim->psys->targets) {
    ParticleSystem *epsys = psys_get_target_system(bbd->sim->ob, pt);

    if (epsys) {
      BLI_assert(epsys->tree != nullptr);
      neighbors = BLI_kdtree_3d_range_search_with_len_squared_cb(epsys->tree,
                                                                 pa->prev_state.co,
                                                                 &ptn,
                                                                 acbr->look_ahead *
                                                                     len_v3(pa->prev_state.vel),
                                                                 len_squared_v3v3_with_normal_bias,
                                                                 pa->prev_state.ave);

      if (neighbors > 0) {
        for (n = 0; n < neighbors; n++) {
          copy_v3_v3(co1, pa->prev_state.co);
          copy_v3_v3(vel1, pa->prev_state.vel);
          copy_v3_v3(co2, (epsys->particles + ptn[n].index)->prev_state.co);
          copy_v3_v3(vel2, (epsys->particles + ptn[n].index)->prev_state.vel);

          sub_v3_v3v3(loc, co1, co2);

          sub_v3_v3v3(vec, vel1, vel2);

          inp = dot_v3v3(vec, vec);

          /* velocities not parallel */
          if (inp != 0.0f) {
            t = -dot_v3v3(loc, vec) / inp;
            /* cpa is not too far in the future so investigate further */
            if (t > 0.0f && t < t_min) {
              madd_v3_v3fl(co1, vel1, t);
              madd_v3_v3fl(co2, vel2, t);

              sub_v3_v3v3(vec, co2, co1);

              len = normalize_v3(vec);

              /* distance of cpa is close enough */
              if (len < 2.0f * val->personal_space * pa->size) {
                t_min = t;

                mul_v3_fl(vec, len_v3(vel1));
                mul_v3_fl(vec, (2.0f - t) / 2.0f);
                sub_v3_v3v3(bbd->wanted_co, vel1, vec);
                bbd->wanted_speed = len_v3(bbd->wanted_co);
                ret = true;
              }
            }
          }
        }
      }

      MEM_SAFE_FREE(ptn);
    }
  }

  if (ptn && nearest == 0) {
    MEM_freeN(ptn);
  }

  return ret;
}
static bool rule_separate(BoidRule * /*rule*/,
                          BoidBrainData *bbd,
                          BoidValues *val,
                          ParticleData *pa)
{
  KDTreeNearest_3d *ptn = nullptr;
  float len = 2.0f * val->personal_space * pa->size + 1.0f;
  float vec[3] = {0.0f, 0.0f, 0.0f};
  int neighbors = BLI_kdtree_3d_range_search(
      bbd->sim->psys->tree, pa->prev_state.co, &ptn, 2.0f * val->personal_space * pa->size);
  bool ret = false;

  if (neighbors > 1 && ptn[1].dist != 0.0f) {
    sub_v3_v3v3(vec, pa->prev_state.co, bbd->sim->psys->particles[ptn[1].index].state.co);
    mul_v3_fl(vec, (2.0f * val->personal_space * pa->size - ptn[1].dist) / ptn[1].dist);
    add_v3_v3(bbd->wanted_co, vec);
    bbd->wanted_speed = val->max_speed;
    len = ptn[1].dist;
    ret = true;
  }
  MEM_SAFE_FREE(ptn);

  /* check other boid systems */
  LISTBASE_FOREACH (ParticleTarget *, pt, &bbd->sim->psys->targets) {
    ParticleSystem *epsys = psys_get_target_system(bbd->sim->ob, pt);

    if (epsys) {
      neighbors = BLI_kdtree_3d_range_search(
          epsys->tree, pa->prev_state.co, &ptn, 2.0f * val->personal_space * pa->size);

      if (neighbors > 0 && ptn[0].dist < len) {
        sub_v3_v3v3(vec, pa->prev_state.co, ptn[0].co);
        mul_v3_fl(vec, (2.0f * val->personal_space * pa->size - ptn[0].dist) / ptn[1].dist);
        add_v3_v3(bbd->wanted_co, vec);
        bbd->wanted_speed = val->max_speed;
        len = ptn[0].dist;
        ret = true;
      }

      MEM_SAFE_FREE(ptn);
    }
  }
  return ret;
}
static bool rule_flock(BoidRule * /*rule*/,
                       BoidBrainData *bbd,
                       BoidValues * /*val*/,
                       ParticleData *pa)
{
  KDTreeNearest_3d ptn[11];
  float vec[3] = {0.0f, 0.0f, 0.0f}, loc[3] = {0.0f, 0.0f, 0.0f};
  int neighbors = BLI_kdtree_3d_find_nearest_n_with_len_squared_cb(
      bbd->sim->psys->tree,
      pa->state.co,
      ptn,
      ARRAY_SIZE(ptn),
      len_squared_v3v3_with_normal_bias,
      pa->prev_state.ave);
  int n;
  bool ret = false;

  if (neighbors > 1) {
    for (n = 1; n < neighbors; n++) {
      add_v3_v3(loc, bbd->sim->psys->particles[ptn[n].index].prev_state.co);
      add_v3_v3(vec, bbd->sim->psys->particles[ptn[n].index].prev_state.vel);
    }

    mul_v3_fl(loc, 1.0f / (float(neighbors) - 1.0f));
    mul_v3_fl(vec, 1.0f / (float(neighbors) - 1.0f));

    sub_v3_v3(loc, pa->prev_state.co);
    sub_v3_v3(vec, pa->prev_state.vel);

    add_v3_v3(bbd->wanted_co, vec);
    add_v3_v3(bbd->wanted_co, loc);
    bbd->wanted_speed = len_v3(bbd->wanted_co);

    ret = true;
  }
  return ret;
}
static bool rule_follow_leader(BoidRule *rule,
                               BoidBrainData *bbd,
                               BoidValues *val,
                               ParticleData *pa)
{
  BoidRuleFollowLeader *flbr = (BoidRuleFollowLeader *)rule;
  float vec[3] = {0.0f, 0.0f, 0.0f}, loc[3] = {0.0f, 0.0f, 0.0f};
  float mul, len;
  const int n = (flbr->queue_size <= 1) ? bbd->sim->psys->totpart : flbr->queue_size;
  BLI_assert(ARRAY_HAS_ITEM(pa, bbd->sim->psys->particles, bbd->sim->psys->totpart));
  const int p = int(pa - bbd->sim->psys->particles);
  int i;
  bool ret = false;

  if (flbr->ob) {
    float vec2[3], t;

    /* first check we're not blocking the leader */
    sub_v3_v3v3(vec, flbr->loc, flbr->oloc);
    mul_v3_fl(vec, 1.0f / bbd->timestep);

    sub_v3_v3v3(loc, pa->prev_state.co, flbr->oloc);

    mul = dot_v3v3(vec, vec);

    /* leader is not moving */
    if (mul < 0.01f) {
      len = len_v3(loc);
      /* too close to leader */
      if (len < 2.0f * val->personal_space * pa->size) {
        copy_v3_v3(bbd->wanted_co, loc);
        bbd->wanted_speed = val->max_speed;
        return true;
      }
    }
    else {
      t = dot_v3v3(loc, vec) / mul;

      /* possible blocking of leader in near future */
      if (t > 0.0f && t < 3.0f) {
        copy_v3_v3(vec2, vec);
        mul_v3_fl(vec2, t);

        sub_v3_v3v3(vec2, loc, vec2);

        len = len_v3(vec2);

        if (len < 2.0f * val->personal_space * pa->size) {
          copy_v3_v3(bbd->wanted_co, vec2);
          bbd->wanted_speed = val->max_speed * (3.0f - t) / 3.0f;
          return true;
        }
      }
    }

    /* not blocking so try to follow leader */
    if (p && flbr->options & BRULE_LEADER_IN_LINE) {
      copy_v3_v3(vec, bbd->sim->psys->particles[p - 1].prev_state.vel);
      copy_v3_v3(loc, bbd->sim->psys->particles[p - 1].prev_state.co);
    }
    else {
      copy_v3_v3(loc, flbr->oloc);
      sub_v3_v3v3(vec, flbr->loc, flbr->oloc);
      mul_v3_fl(vec, 1.0f / bbd->timestep);
    }

    /* fac is seconds behind leader */
    madd_v3_v3fl(loc, vec, -flbr->distance);

    sub_v3_v3v3(bbd->wanted_co, loc, pa->prev_state.co);
    bbd->wanted_speed = len_v3(bbd->wanted_co);

    ret = true;
  }
  else if (p % n) {
    float vec2[3], t, t_min = 3.0f;

    /* first check we're not blocking any leaders */
    for (i = 0; i < bbd->sim->psys->totpart; i += n) {
      copy_v3_v3(vec, bbd->sim->psys->particles[i].prev_state.vel);

      sub_v3_v3v3(loc, pa->prev_state.co, bbd->sim->psys->particles[i].prev_state.co);

      mul = dot_v3v3(vec, vec);

      /* leader is not moving */
      if (mul < 0.01f) {
        len = len_v3(loc);
        /* too close to leader */
        if (len < 2.0f * val->personal_space * pa->size) {
          copy_v3_v3(bbd->wanted_co, loc);
          bbd->wanted_speed = val->max_speed;
          return true;
        }
      }
      else {
        t = dot_v3v3(loc, vec) / mul;

        /* possible blocking of leader in near future */
        if (t > 0.0f && t < t_min) {
          copy_v3_v3(vec2, vec);
          mul_v3_fl(vec2, t);

          sub_v3_v3v3(vec2, loc, vec2);

          len = len_v3(vec2);

          if (len < 2.0f * val->personal_space * pa->size) {
            t_min = t;
            copy_v3_v3(bbd->wanted_co, loc);
            bbd->wanted_speed = val->max_speed * (3.0f - t) / 3.0f;
            ret = true;
          }
        }
      }
    }

    if (ret) {
      return true;
    }

    /* not blocking so try to follow leader */
    if (flbr->options & BRULE_LEADER_IN_LINE) {
      copy_v3_v3(vec, bbd->sim->psys->particles[p - 1].prev_state.vel);
      copy_v3_v3(loc, bbd->sim->psys->particles[p - 1].prev_state.co);
    }
    else {
      copy_v3_v3(vec, bbd->sim->psys->particles[p - p % n].prev_state.vel);
      copy_v3_v3(loc, bbd->sim->psys->particles[p - p % n].prev_state.co);
    }

    /* fac is seconds behind leader */
    madd_v3_v3fl(loc, vec, -flbr->distance);

    sub_v3_v3v3(bbd->wanted_co, loc, pa->prev_state.co);
    bbd->wanted_speed = len_v3(bbd->wanted_co);

    ret = true;
  }

  return ret;
}
static bool rule_average_speed(BoidRule *rule,
                               BoidBrainData *bbd,
                               BoidValues *val,
                               ParticleData *pa)
{
  BoidParticle *bpa = pa->boid;
  BoidRuleAverageSpeed *asbr = (BoidRuleAverageSpeed *)rule;
  float vec[3] = {0.0f, 0.0f, 0.0f};

  if (asbr->wander > 0.0f) {
    /* abuse pa->r_ave for wandering */
    bpa->wander[0] += asbr->wander * (-1.0f + 2.0f * BLI_rng_get_float(bbd->rng));
    bpa->wander[1] += asbr->wander * (-1.0f + 2.0f * BLI_rng_get_float(bbd->rng));
    bpa->wander[2] += asbr->wander * (-1.0f + 2.0f * BLI_rng_get_float(bbd->rng));

    normalize_v3(bpa->wander);

    copy_v3_v3(vec, bpa->wander);

    mul_qt_v3(pa->prev_state.rot, vec);

    copy_v3_v3(bbd->wanted_co, pa->prev_state.ave);

    mul_v3_fl(bbd->wanted_co, 1.1f);

    add_v3_v3(bbd->wanted_co, vec);

    /* leveling */
    if (asbr->level > 0.0f && psys_uses_gravity(bbd->sim)) {
      project_v3_v3v3(vec, bbd->wanted_co, bbd->sim->scene->physics_settings.gravity);
      mul_v3_fl(vec, asbr->level);
      sub_v3_v3(bbd->wanted_co, vec);
    }
  }
  else {
    copy_v3_v3(bbd->wanted_co, pa->prev_state.ave);

    /* may happen at birth */
    if (dot_v2v2(bbd->wanted_co, bbd->wanted_co) == 0.0f) {
      bbd->wanted_co[0] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
      bbd->wanted_co[1] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
      bbd->wanted_co[2] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
    }

    /* leveling */
    if (asbr->level > 0.0f && psys_uses_gravity(bbd->sim)) {
      project_v3_v3v3(vec, bbd->wanted_co, bbd->sim->scene->physics_settings.gravity);
      mul_v3_fl(vec, asbr->level);
      sub_v3_v3(bbd->wanted_co, vec);
    }
  }
  bbd->wanted_speed = asbr->speed * val->max_speed;

  return true;
}
static bool rule_fight(BoidRule *rule, BoidBrainData *bbd, BoidValues *val, ParticleData *pa)
{
  BoidRuleFight *fbr = (BoidRuleFight *)rule;
  KDTreeNearest_3d *ptn = nullptr;
  ParticleData *epars;
  ParticleData *enemy_pa = nullptr;
  BoidParticle *bpa;
  /* friends & enemies */
  float closest_enemy[3] = {0.0f, 0.0f, 0.0f};
  float closest_dist = fbr->distance + 1.0f;
  float f_strength = 0.0f, e_strength = 0.0f;
  float health = 0.0f;
  int n;
  bool ret = false;

  /* calculate its own group strength */
  int neighbors = BLI_kdtree_3d_range_search(
      bbd->sim->psys->tree, pa->prev_state.co, &ptn, fbr->distance);
  for (n = 0; n < neighbors; n++) {
    bpa = bbd->sim->psys->particles[ptn[n].index].boid;
    health += bpa->data.health;
  }

  f_strength += bbd->part->boids->strength * health;

  MEM_SAFE_FREE(ptn);

  /* add other friendlies and calculate enemy strength and find closest enemy */
  LISTBASE_FOREACH (ParticleTarget *, pt, &bbd->sim->psys->targets) {
    ParticleSystem *epsys = psys_get_target_system(bbd->sim->ob, pt);
    if (epsys && epsys->part->boids) {
      epars = epsys->particles;

      neighbors = BLI_kdtree_3d_range_search(epsys->tree, pa->prev_state.co, &ptn, fbr->distance);

      health = 0.0f;

      for (n = 0; n < neighbors; n++) {
        bpa = epars[ptn[n].index].boid;
        health += bpa->data.health;

        if (n == 0 && pt->mode == PTARGET_MODE_ENEMY && ptn[n].dist < closest_dist) {
          copy_v3_v3(closest_enemy, ptn[n].co);
          closest_dist = ptn[n].dist;
          enemy_pa = epars + ptn[n].index;
        }
      }
      if (pt->mode == PTARGET_MODE_ENEMY) {
        e_strength += epsys->part->boids->strength * health;
      }
      else if (pt->mode == PTARGET_MODE_FRIEND) {
        f_strength += epsys->part->boids->strength * health;
      }

      MEM_SAFE_FREE(ptn);
    }
  }
  /* decide action if enemy presence found */
  if (e_strength > 0.0f) {
    sub_v3_v3v3(bbd->wanted_co, closest_enemy, pa->prev_state.co);

    /* attack if in range */
    if (closest_dist <= bbd->part->boids->range + pa->size + enemy_pa->size) {
      float damage = BLI_rng_get_float(bbd->rng);
      float enemy_dir[3];

      normalize_v3_v3(enemy_dir, bbd->wanted_co);

      /* fight mode */
      bbd->wanted_speed = 0.0f;

      /* must face enemy to fight */
      if (dot_v3v3(pa->prev_state.ave, enemy_dir) > 0.5f) {
        bpa = enemy_pa->boid;
        bpa->data.health -= bbd->part->boids->strength * bbd->timestep *
                            ((1.0f - bbd->part->boids->accuracy) * damage +
                             bbd->part->boids->accuracy);
      }
    }
    else {
      /* approach mode */
      bbd->wanted_speed = val->max_speed;
    }

    /* check if boid doesn't want to fight */
    bpa = pa->boid;
    if (bpa->data.health / bbd->part->boids->health * bbd->part->boids->aggression <
        e_strength / f_strength)
    {
      /* decide to flee */
      if (closest_dist < fbr->flee_distance * fbr->distance) {
        negate_v3(bbd->wanted_co);
        bbd->wanted_speed = val->max_speed;
      }
      else { /* wait for better odds */
        bbd->wanted_speed = 0.0f;
      }
    }

    ret = true;
  }

  return ret;
}

using boid_rule_cb = bool (*)(BoidRule *rule,
                              BoidBrainData *data,
                              BoidValues *val,
                              ParticleData *pa);

static boid_rule_cb boid_rules[] = {
    rule_none,
    rule_goal_avoid,
    rule_goal_avoid,
    rule_avoid_collision,
    rule_separate,
    rule_flock,
    rule_follow_leader,
    rule_average_speed,
    rule_fight,
#if 0
    rule_help,
    rule_protect,
    rule_hide,
    rule_follow_path,
    rule_follow_wall,
#endif
};

static void set_boid_values(BoidValues *val, BoidSettings *boids, ParticleData *pa)
{
  BoidParticle *bpa = pa->boid;

  if (ELEM(bpa->data.mode, eBoidMode_OnLand, eBoidMode_Climbing)) {
    val->max_speed = boids->land_max_speed * bpa->data.health / boids->health;
    val->max_acc = boids->land_max_acc * val->max_speed;
    val->max_ave = boids->land_max_ave * float(M_PI) * bpa->data.health / boids->health;
    val->min_speed = 0.0f; /* no minimum speed on land */
    val->personal_space = boids->land_personal_space;
    val->jump_speed = boids->land_jump_speed * bpa->data.health / boids->health;
  }
  else {
    val->max_speed = boids->air_max_speed * bpa->data.health / boids->health;
    val->max_acc = boids->air_max_acc * val->max_speed;
    val->max_ave = boids->air_max_ave * float(M_PI) * bpa->data.health / boids->health;
    val->min_speed = boids->air_min_speed * boids->air_max_speed;
    val->personal_space = boids->air_personal_space;
    val->jump_speed = 0.0f; /* no jumping in air */
  }
}

static Object *boid_find_ground(BoidBrainData *bbd,
                                ParticleData *pa,
                                float ground_co[3],
                                float ground_nor[3])
{
  const int raycast_flag = BVH_RAYCAST_DEFAULT & ~BVH_RAYCAST_WATERTIGHT;
  BoidParticle *bpa = pa->boid;

  if (bpa->data.mode == eBoidMode_Climbing) {
    SurfaceModifierData *surmd = nullptr;
    float x[3], v[3];

    surmd = (SurfaceModifierData *)BKE_modifiers_findby_type(bpa->ground, eModifierType_Surface);

    /* take surface velocity into account */
    closest_point_on_surface(surmd, pa->state.co, x, nullptr, v);
    add_v3_v3(x, v);

    /* get actual position on surface */
    closest_point_on_surface(surmd, x, ground_co, ground_nor, nullptr);

    return bpa->ground;
  }

  const float zvec[3] = {0.0f, 0.0f, 2000.0f};
  ParticleCollision col;
  BVHTreeRayHit hit;
  float radius = 0.0f, t, ray_dir[3];

  if (!bbd->sim->colliders) {
    return nullptr;
  }

  memset(&col, 0, sizeof(ParticleCollision));

  /* first try to find below boid */
  copy_v3_v3(col.co1, pa->state.co);
  sub_v3_v3v3(col.co2, pa->state.co, zvec);
  sub_v3_v3v3(ray_dir, col.co2, col.co1);
  col.f = 0.0f;
  hit.index = -1;
  hit.dist = col.original_ray_length = normalize_v3(ray_dir);
  col.pce.inside = 0;

  LISTBASE_FOREACH (ColliderCache *, coll, bbd->sim->colliders) {
    col.current = coll->ob;
    col.md = coll->collmd;
    col.fac1 = col.fac2 = 0.0f;

    if (col.md && col.md->bvhtree) {
      BLI_bvhtree_ray_cast_ex(col.md->bvhtree,
                              col.co1,
                              ray_dir,
                              radius,
                              &hit,
                              BKE_psys_collision_neartest_cb,
                              &col,
                              raycast_flag);
    }
  }
  /* then use that object */
  if (hit.index >= 0) {
    t = hit.dist / col.original_ray_length;
    interp_v3_v3v3(ground_co, col.co1, col.co2, t);
    normalize_v3_v3(ground_nor, col.pce.nor);
    return col.hit;
  }

  /* couldn't find below, so find upmost deflector object */
  add_v3_v3v3(col.co1, pa->state.co, zvec);
  sub_v3_v3v3(col.co2, pa->state.co, zvec);
  sub_v3_v3(col.co2, zvec);
  sub_v3_v3v3(ray_dir, col.co2, col.co1);
  col.f = 0.0f;
  hit.index = -1;
  hit.dist = col.original_ray_length = normalize_v3(ray_dir);

  LISTBASE_FOREACH (ColliderCache *, coll, bbd->sim->colliders) {
    col.current = coll->ob;
    col.md = coll->collmd;

    if (col.md && col.md->bvhtree) {
      BLI_bvhtree_ray_cast_ex(col.md->bvhtree,
                              col.co1,
                              ray_dir,
                              radius,
                              &hit,
                              BKE_psys_collision_neartest_cb,
                              &col,
                              raycast_flag);
    }
  }
  /* then use that object */
  if (hit.index >= 0) {
    t = hit.dist / col.original_ray_length;
    interp_v3_v3v3(ground_co, col.co1, col.co2, t);
    normalize_v3_v3(ground_nor, col.pce.nor);
    return col.hit;
  }

  /* default to z=0 */
  copy_v3_v3(ground_co, pa->state.co);
  ground_co[2] = 0;
  ground_nor[0] = ground_nor[1] = 0.0f;
  ground_nor[2] = 1.0f;
  return nullptr;
}
static bool boid_rule_applies(ParticleData *pa, BoidSettings * /*boids*/, BoidRule *rule)
{
  BoidParticle *bpa = pa->boid;

  if (rule == nullptr) {
    return false;
  }

  if (ELEM(bpa->data.mode, eBoidMode_OnLand, eBoidMode_Climbing) && rule->flag & BOIDRULE_ON_LAND)
  {
    return true;
  }

  if (bpa->data.mode == eBoidMode_InAir && rule->flag & BOIDRULE_IN_AIR) {
    return true;
  }

  return false;
}
void boids_precalc_rules(ParticleSettings *part, float cfra)
{
  LISTBASE_FOREACH (BoidState *, state, &part->boids->states) {
    LISTBASE_FOREACH (BoidRule *, rule, &state->rules) {
      if (rule->type == eBoidRuleType_FollowLeader) {
        BoidRuleFollowLeader *flbr = (BoidRuleFollowLeader *)rule;

        if (flbr->ob && flbr->cfra != cfra) {
          /* save object locations for velocity calculations */
          copy_v3_v3(flbr->oloc, flbr->loc);
          copy_v3_v3(flbr->loc, flbr->ob->object_to_world().location());
          flbr->cfra = cfra;
        }
      }
    }
  }
}
static void boid_climb(BoidSettings *boids,
                       ParticleData *pa,
                       float *surface_co,
                       float *surface_nor)
{
  BoidParticle *bpa = pa->boid;
  float nor[3], vel[3];
  copy_v3_v3(nor, surface_nor);

  /* gather apparent gravity */
  madd_v3_v3fl(bpa->gravity, surface_nor, -1.0f);
  normalize_v3(bpa->gravity);

  /* raise boid it's size from surface */
  mul_v3_fl(nor, pa->size * boids->height);
  add_v3_v3v3(pa->state.co, surface_co, nor);

  /* remove normal component from velocity */
  project_v3_v3v3(vel, pa->state.vel, surface_nor);
  sub_v3_v3v3(pa->state.vel, pa->state.vel, vel);
}
static float boid_goal_signed_dist(float *boid_co, float *goal_co, float *goal_nor)
{
  float vec[3];

  sub_v3_v3v3(vec, boid_co, goal_co);

  return dot_v3v3(vec, goal_nor);
}
/* wanted_co is relative to boid location */
static bool apply_boid_rule(
    BoidBrainData *bbd, BoidRule *rule, BoidValues *val, ParticleData *pa, float fuzziness)
{
  if (rule == nullptr) {
    return false;
  }

  if (!boid_rule_applies(pa, bbd->part->boids, rule)) {
    return false;
  }

  if (!boid_rules[rule->type](rule, bbd, val, pa)) {
    return false;
  }

  if (fuzziness < 0.0f || compare_len_v3v3(bbd->wanted_co,
                                           pa->prev_state.vel,
                                           fuzziness * len_v3(pa->prev_state.vel)) == 0)
  {
    return true;
  }
  return false;
}
static BoidState *get_boid_state(BoidSettings *boids, ParticleData *pa)
{
  BoidState *state = static_cast<BoidState *>(boids->states.first);
  BoidParticle *bpa = pa->boid;

  for (; state; state = state->next) {
    if (state->id == bpa->data.state_id) {
      return state;
    }
  }

  /* for some reason particle isn't at a valid state */
  state = static_cast<BoidState *>(boids->states.first);
  if (state) {
    bpa->data.state_id = state->id;
  }

  return state;
}

#if 0 /* TODO */
static int boid_condition_is_true(BoidCondition *cond)
{
  return 0;
}
#endif

void boid_brain(BoidBrainData *bbd, int p, ParticleData *pa)
{
  BoidRule *rule;
  BoidSettings *boids = bbd->part->boids;
  BoidValues val;
  BoidState *state = get_boid_state(boids, pa);
  BoidParticle *bpa = pa->boid;
  ParticleSystem *psys = bbd->sim->psys;
  int rand;

  if (bpa->data.health <= 0.0f) {
    pa->alive = PARS_DYING;
    pa->dietime = bbd->cfra;
    return;
  }

/* Planned for near future. */
#if 0
  BoidCondition *cond = state->conditions.first;
  for (; cond; cond = cond->next) {
    if (boid_condition_is_true(cond)) {
      pa->boid->state_id = cond->state_id;
      state = get_boid_state(boids, pa);
      break; /* only first true condition is used */
    }
  }
#endif

  zero_v3(bbd->wanted_co);
  bbd->wanted_speed = 0.0f;

  /* create random seed for every particle & frame */
  rand = int(psys_frand(psys, psys->seed + p) * 1000);
  rand = int(psys_frand(psys, int(bbd->cfra) + rand) * 1000);

  set_boid_values(&val, bbd->part->boids, pa);

  /* go through rules */
  switch (state->ruleset_type) {
    case eBoidRulesetType_Fuzzy: {
      LISTBASE_FOREACH (BoidRule *, rule, &state->rules) {
        if (apply_boid_rule(bbd, rule, &val, pa, state->rule_fuzziness)) {
          break; /* only first nonzero rule that comes through fuzzy rule is applied */
        }
      }
      break;
    }
    case eBoidRulesetType_Random: {
      /* use random rule for each particle (always same for same particle though) */
      const int n = BLI_listbase_count(&state->rules);
      if (n) {
        rule = static_cast<BoidRule *>(BLI_findlink(&state->rules, rand % n));
        apply_boid_rule(bbd, rule, &val, pa, -1.0);
      }
      break;
    }
    case eBoidRulesetType_Average: {
      float wanted_co[3] = {0.0f, 0.0f, 0.0f}, wanted_speed = 0.0f;
      int n = 0;
      LISTBASE_FOREACH (BoidRule *, rule, &state->rules) {
        if (apply_boid_rule(bbd, rule, &val, pa, -1.0f)) {
          add_v3_v3(wanted_co, bbd->wanted_co);
          wanted_speed += bbd->wanted_speed;
          n++;
          zero_v3(bbd->wanted_co);
          bbd->wanted_speed = 0.0f;
        }
      }

      if (n > 1) {
        mul_v3_fl(wanted_co, 1.0f / float(n));
        wanted_speed /= float(n);
      }

      copy_v3_v3(bbd->wanted_co, wanted_co);
      bbd->wanted_speed = wanted_speed;
      break;
    }
  }

  /* decide on jumping & liftoff */
  if (bpa->data.mode == eBoidMode_OnLand) {
    /* Fuzziness makes boids capable of misjudgment. */
    float mul = 1.0f + state->rule_fuzziness;

    if (boids->options & BOID_ALLOW_FLIGHT && bbd->wanted_co[2] > 0.0f) {
      float cvel[3], dir[3];

      copy_v3_v3(dir, pa->prev_state.ave);
      normalize_v2(dir);

      copy_v3_v3(cvel, bbd->wanted_co);
      normalize_v2(cvel);

      if (dot_v2v2(cvel, dir) > 0.95f / mul) {
        bpa->data.mode = eBoidMode_Liftoff;
      }
    }
    else if (val.jump_speed > 0.0f) {
      float jump_v[3];
      int jump = 0;

      /* jump to get to a location */
      if (bbd->wanted_co[2] > 0.0f) {
        float cvel[3], dir[3];
        float z_v, ground_v, cur_v;
        float len;

        copy_v3_v3(dir, pa->prev_state.ave);
        normalize_v2(dir);

        copy_v3_v3(cvel, bbd->wanted_co);
        normalize_v2(cvel);

        len = len_v2(pa->prev_state.vel);

        /* first of all, are we going in a suitable direction? */
        /* or at a suitably slow speed */
        if (dot_v2v2(cvel, dir) > 0.95f / mul || len <= state->rule_fuzziness) {
          /* try to reach goal at highest point of the parabolic path */
          cur_v = len_v2(pa->prev_state.vel);
          z_v = safe_sqrtf(-2.0f * bbd->sim->scene->physics_settings.gravity[2] *
                           bbd->wanted_co[2]);
          ground_v = len_v2(bbd->wanted_co) *
                     safe_sqrtf(-0.5f * bbd->sim->scene->physics_settings.gravity[2] /
                                bbd->wanted_co[2]);

          len = safe_sqrtf((ground_v - cur_v) * (ground_v - cur_v) + z_v * z_v);

          if (len < val.jump_speed * mul || bbd->part->boids->options & BOID_ALLOW_FLIGHT) {
            jump = 1;

            len = std::min(len, val.jump_speed);

            copy_v3_v3(jump_v, dir);
            jump_v[2] = z_v;
            mul_v3_fl(jump_v, ground_v);

            normalize_v3(jump_v);
            mul_v3_fl(jump_v, len);
            add_v2_v2v2(jump_v, jump_v, pa->prev_state.vel);
          }
        }
      }

      /* jump to go faster */
      if (jump == 0 && val.jump_speed > val.max_speed && bbd->wanted_speed > val.max_speed) {
        /* pass */
      }

      if (jump) {
        copy_v3_v3(pa->prev_state.vel, jump_v);
        bpa->data.mode = eBoidMode_Falling;
      }
    }
  }
}
void boid_body(BoidBrainData *bbd, ParticleData *pa)
{
  BoidSettings *boids = bbd->part->boids;
  BoidParticle *bpa = pa->boid;
  BoidValues val;
  EffectedPoint epoint;
  float acc[3] = {0.0f, 0.0f, 0.0f}, tan_acc[3], nor_acc[3];
  float dvec[3], bvec[3];
  float new_dir[3], new_speed;
  float old_dir[3], old_speed;
  float wanted_dir[3];
  float q[4], mat[3][3]; /* rotation */
  float ground_co[3] = {0.0f, 0.0f, 0.0f}, ground_nor[3] = {0.0f, 0.0f, 1.0f};
  float force[3] = {0.0f, 0.0f, 0.0f};
  float pa_mass = bbd->part->mass, dtime = bbd->dfra * bbd->timestep;

  set_boid_values(&val, boids, pa);

  /* make sure there's something in new velocity, location & rotation */
  copy_particle_key(&pa->state, &pa->prev_state, 0);

  if (bbd->part->flag & PART_SIZEMASS) {
    pa_mass *= pa->size;
  }

  /* if boids can't fly they fall to the ground */
  if ((boids->options & BOID_ALLOW_FLIGHT) == 0 &&
      ELEM(bpa->data.mode, eBoidMode_OnLand, eBoidMode_Climbing) == 0 &&
      psys_uses_gravity(bbd->sim))
  {
    bpa->data.mode = eBoidMode_Falling;
  }

  if (bpa->data.mode == eBoidMode_Falling) {
    /* Falling boids are only effected by gravity. */
    acc[2] = bbd->sim->scene->physics_settings.gravity[2];
  }
  else {
    /* figure out acceleration */
    float landing_level = 2.0f;
    float level = landing_level + 1.0f;
    float new_vel[3];

    if (bpa->data.mode == eBoidMode_Liftoff) {
      bpa->data.mode = eBoidMode_InAir;
      bpa->ground = boid_find_ground(bbd, pa, ground_co, ground_nor);
    }
    else if (bpa->data.mode == eBoidMode_InAir && boids->options & BOID_ALLOW_LAND) {
      /* auto-leveling & landing if close to ground */

      bpa->ground = boid_find_ground(bbd, pa, ground_co, ground_nor);

      /* level = how many particle sizes above ground */
      level = (pa->prev_state.co[2] - ground_co[2]) / (2.0f * pa->size) - 0.5f;

      landing_level = -boids->landing_smoothness * pa->prev_state.vel[2] * pa_mass;

      if (pa->prev_state.vel[2] < 0.0f) {
        if (level < 1.0f) {
          bbd->wanted_co[0] = bbd->wanted_co[1] = bbd->wanted_co[2] = 0.0f;
          bbd->wanted_speed = 0.0f;
          bpa->data.mode = eBoidMode_Falling;
        }
        else if (level < landing_level) {
          bbd->wanted_speed *= (level - 1.0f) / landing_level;
          bbd->wanted_co[2] *= (level - 1.0f) / landing_level;
        }
      }
    }

    copy_v3_v3(old_dir, pa->prev_state.ave);
    new_speed = normalize_v3_v3(wanted_dir, bbd->wanted_co);

    /* first check if we have valid direction we want to go towards */
    if (new_speed == 0.0f) {
      copy_v3_v3(new_dir, old_dir);
    }
    else {
      float old_dir2[2], wanted_dir2[2], nor[3], angle;
      copy_v2_v2(old_dir2, old_dir);
      normalize_v2(old_dir2);
      copy_v2_v2(wanted_dir2, wanted_dir);
      normalize_v2(wanted_dir2);

      /* choose random direction to turn if wanted velocity */
      /* is directly behind regardless of z-coordinate */
      if (dot_v2v2(old_dir2, wanted_dir2) < -0.99f) {
        wanted_dir[0] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
        wanted_dir[1] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
        wanted_dir[2] = 2.0f * (0.5f - BLI_rng_get_float(bbd->rng));
        normalize_v3(wanted_dir);
      }

      /* constrain direction with maximum angular velocity */
      angle = safe_acosf(dot_v3v3(old_dir, wanted_dir));
      angle = min_ff(angle, val.max_ave);

      cross_v3_v3v3(nor, old_dir, wanted_dir);
      axis_angle_to_quat(q, nor, angle);
      copy_v3_v3(new_dir, old_dir);
      mul_qt_v3(q, new_dir);
      normalize_v3(new_dir);

      /* save direction in case resulting velocity too small */
      axis_angle_to_quat(q, nor, angle * dtime);
      copy_v3_v3(pa->state.ave, old_dir);
      mul_qt_v3(q, pa->state.ave);
      normalize_v3(pa->state.ave);
    }

    /* constrain speed with maximum acceleration */
    old_speed = len_v3(pa->prev_state.vel);

    if (bbd->wanted_speed < old_speed) {
      new_speed = std::max(bbd->wanted_speed, old_speed - val.max_acc);
    }
    else {
      new_speed = std::min(bbd->wanted_speed, old_speed + val.max_acc);
    }

    /* combine direction and speed */
    copy_v3_v3(new_vel, new_dir);
    mul_v3_fl(new_vel, new_speed);

    /* maintain minimum flying velocity if not landing */
    if (level >= landing_level) {
      float len2 = dot_v2v2(new_vel, new_vel);
      float root;

      len2 = std::max(len2, val.min_speed * val.min_speed);
      root = safe_sqrtf(new_speed * new_speed - len2);

      new_vel[2] = new_vel[2] < 0.0f ? -root : root;

      normalize_v2(new_vel);
      mul_v2_fl(new_vel, safe_sqrtf(len2));
    }

    /* finally constrain speed to max speed */
    new_speed = normalize_v3(new_vel);
    mul_v3_fl(new_vel, std::min(new_speed, val.max_speed));

    /* get acceleration from difference of velocities */
    sub_v3_v3v3(acc, new_vel, pa->prev_state.vel);

    /* break acceleration to components */
    project_v3_v3v3(tan_acc, acc, pa->prev_state.ave);
    sub_v3_v3v3(nor_acc, acc, tan_acc);
  }

  /* account for effectors */
  pd_point_from_particle(bbd->sim, pa, &pa->state, &epoint);
  BKE_effectors_apply(bbd->sim->psys->effectors,
                      bbd->sim->colliders,
                      bbd->part->effector_weights,
                      &epoint,
                      force,
                      nullptr,
                      nullptr);

  if (ELEM(bpa->data.mode, eBoidMode_OnLand, eBoidMode_Climbing)) {
    float length = normalize_v3(force);

    length = std::max(0.0f, length - boids->land_stick_force);

    mul_v3_fl(force, length);
  }

  add_v3_v3(acc, force);

  /* store smoothed acceleration for nice banking etc. */
  madd_v3_v3fl(bpa->data.acc, acc, dtime);
  mul_v3_fl(bpa->data.acc, 1.0f / (1.0f + dtime));

  /* integrate new location & velocity */

  /* by regarding the acceleration as a force at this stage we
   * can get better control although it's a bit unphysical */
  mul_v3_fl(acc, 1.0f / pa_mass);

  copy_v3_v3(dvec, acc);
  mul_v3_fl(dvec, dtime * dtime * 0.5f);

  copy_v3_v3(bvec, pa->prev_state.vel);
  mul_v3_fl(bvec, dtime);
  add_v3_v3(dvec, bvec);
  add_v3_v3(pa->state.co, dvec);

  madd_v3_v3fl(pa->state.vel, acc, dtime);

  // if (bpa->data.mode != eBoidMode_InAir)
  bpa->ground = boid_find_ground(bbd, pa, ground_co, ground_nor);

  /* change modes, constrain movement & keep track of down vector */
  switch (bpa->data.mode) {
    case eBoidMode_InAir: {
      float grav[3];

      grav[0] = 0.0f;
      grav[1] = 0.0f;
      grav[2] = bbd->sim->scene->physics_settings.gravity[2] < 0.0f ? -1.0f : 0.0f;

      /* don't take forward acceleration into account (better banking) */
      if (dot_v3v3(bpa->data.acc, pa->state.vel) > 0.0f) {
        project_v3_v3v3(dvec, bpa->data.acc, pa->state.vel);
        sub_v3_v3v3(dvec, bpa->data.acc, dvec);
      }
      else {
        copy_v3_v3(dvec, bpa->data.acc);
      }

      /* gather apparent gravity */
      madd_v3_v3v3fl(bpa->gravity, grav, dvec, -boids->banking);
      normalize_v3(bpa->gravity);

      /* stick boid on goal when close enough */
      if (bbd->goal_ob && boid_goal_signed_dist(pa->state.co, bbd->goal_co, bbd->goal_nor) <=
                              pa->size * boids->height)
      {
        bpa->data.mode = eBoidMode_Climbing;
        bpa->ground = bbd->goal_ob;
        boid_find_ground(bbd, pa, ground_co, ground_nor);
        boid_climb(boids, pa, ground_co, ground_nor);
      }
      else if (pa->state.co[2] <= ground_co[2] + pa->size * boids->height) {
        /* land boid when below ground */
        if (boids->options & BOID_ALLOW_LAND) {
          pa->state.co[2] = ground_co[2] + pa->size * boids->height;
          pa->state.vel[2] = 0.0f;
          bpa->data.mode = eBoidMode_OnLand;
        }
        /* fly above ground */
        else if (bpa->ground) {
          pa->state.co[2] = ground_co[2] + pa->size * boids->height;
          pa->state.vel[2] = 0.0f;
        }
      }
      break;
    }
    case eBoidMode_Falling: {
      float grav[3];

      grav[0] = 0.0f;
      grav[1] = 0.0f;
      grav[2] = bbd->sim->scene->physics_settings.gravity[2] < 0.0f ? -1.0f : 0.0f;

      /* gather apparent gravity */
      madd_v3_v3fl(bpa->gravity, grav, dtime);
      normalize_v3(bpa->gravity);

      if (boids->options & BOID_ALLOW_LAND) {
        /* stick boid on goal when close enough */
        if (bbd->goal_ob && boid_goal_signed_dist(pa->state.co, bbd->goal_co, bbd->goal_nor) <=
                                pa->size * boids->height)
        {
          bpa->data.mode = eBoidMode_Climbing;
          bpa->ground = bbd->goal_ob;
          boid_find_ground(bbd, pa, ground_co, ground_nor);
          boid_climb(boids, pa, ground_co, ground_nor);
        }
        /* land boid when really near ground */
        else if (pa->state.co[2] <= ground_co[2] + 1.01f * pa->size * boids->height) {
          pa->state.co[2] = ground_co[2] + pa->size * boids->height;
          pa->state.vel[2] = 0.0f;
          bpa->data.mode = eBoidMode_OnLand;
        }
        /* if we're falling, can fly and want to go upwards lets fly */
        else if (boids->options & BOID_ALLOW_FLIGHT && bbd->wanted_co[2] > 0.0f) {
          bpa->data.mode = eBoidMode_InAir;
        }
      }
      else {
        bpa->data.mode = eBoidMode_InAir;
      }
      break;
    }
    case eBoidMode_Climbing: {
      boid_climb(boids, pa, ground_co, ground_nor);
#if 0
      float nor[3];
      copy_v3_v3(nor, ground_nor);

      /* Gather apparent gravity to r_ve. */
      madd_v3_v3fl(pa->r_ve, ground_nor, -1.0);
      normalize_v3(pa->r_ve);

      /* Raise boid it's size from surface. */
      mul_v3_fl(nor, pa->size * boids->height);
      add_v3_v3v3(pa->state.co, ground_co, nor);

      /* Remove normal component from velocity. */
      project_v3_v3v3(v, pa->state.vel, ground_nor);
      sub_v3_v3v3(pa->state.vel, pa->state.vel, v);
#endif
      break;
    }
    case eBoidMode_OnLand: {
      /* stick boid on goal when close enough */
      if (bbd->goal_ob && boid_goal_signed_dist(pa->state.co, bbd->goal_co, bbd->goal_nor) <=
                              pa->size * boids->height)
      {
        bpa->data.mode = eBoidMode_Climbing;
        bpa->ground = bbd->goal_ob;
        boid_find_ground(bbd, pa, ground_co, ground_nor);
        boid_climb(boids, pa, ground_co, ground_nor);
      }
      /* ground is too far away so boid falls */
      else if (pa->state.co[2] - ground_co[2] > 1.1f * pa->size * boids->height) {
        bpa->data.mode = eBoidMode_Falling;
      }
      else {
        /* constrain to surface */
        pa->state.co[2] = ground_co[2] + pa->size * boids->height;
        pa->state.vel[2] = 0.0f;
      }

      if (boids->banking > 0.0f) {
        float grav[3];
        /* Don't take gravity's strength in to account, */
        /* otherwise amount of banking is hard to control. */
        negate_v3_v3(grav, ground_nor);

        project_v3_v3v3(dvec, bpa->data.acc, pa->state.vel);
        sub_v3_v3v3(dvec, bpa->data.acc, dvec);

        /* gather apparent gravity */
        madd_v3_v3v3fl(bpa->gravity, grav, dvec, -boids->banking);
        normalize_v3(bpa->gravity);
      }
      else {
        /* gather negative surface normal */
        madd_v3_v3fl(bpa->gravity, ground_nor, -1.0f);
        normalize_v3(bpa->gravity);
      }
      break;
    }
  }

  /* save direction to state.ave unless the boid is falling */
  /* (boids can't effect their direction when falling) */
  if (bpa->data.mode != eBoidMode_Falling && len_v3(pa->state.vel) > 0.1f * pa->size) {
    copy_v3_v3(pa->state.ave, pa->state.vel);
    pa->state.ave[2] *= bbd->part->boids->pitch;
    normalize_v3(pa->state.ave);
  }

  /* apply damping */
  if (ELEM(bpa->data.mode, eBoidMode_OnLand, eBoidMode_Climbing)) {
    mul_v3_fl(pa->state.vel, 1.0f - 0.2f * bbd->part->dampfac);
  }

  /* calculate rotation matrix based on forward & down vectors */
  if (bpa->data.mode == eBoidMode_InAir) {
    copy_v3_v3(mat[0], pa->state.ave);

    project_v3_v3v3(dvec, bpa->gravity, pa->state.ave);
    sub_v3_v3v3(mat[2], bpa->gravity, dvec);
    normalize_v3(mat[2]);
  }
  else {
    project_v3_v3v3(dvec, pa->state.ave, bpa->gravity);
    sub_v3_v3v3(mat[0], pa->state.ave, dvec);
    normalize_v3(mat[0]);

    copy_v3_v3(mat[2], bpa->gravity);
  }
  negate_v3(mat[2]);
  cross_v3_v3v3(mat[1], mat[2], mat[0]);

  /* apply rotation */
  mat3_to_quat_legacy(q, mat);
  copy_qt_qt(pa->state.rot, q);
}

BoidRule *boid_new_rule(int type)
{
  BoidRule *rule = nullptr;
  if (type <= 0) {
    return nullptr;
  }

  switch (type) {
    case eBoidRuleType_Goal:
    case eBoidRuleType_Avoid: {
      BoidRuleGoalAvoid *rule_goal = MEM_callocN<BoidRuleGoalAvoid>("BoidRuleGoalAvoid");
      rule = reinterpret_cast<BoidRule *>(rule_goal);
      break;
    }
    case eBoidRuleType_AvoidCollision: {
      BoidRuleAvoidCollision *rule_avoid = MEM_callocN<BoidRuleAvoidCollision>(
          "BoidRuleAvoidCollision");
      rule_avoid->look_ahead = 2.0f;
      rule = reinterpret_cast<BoidRule *>(rule_avoid);
      break;
    }
    case eBoidRuleType_FollowLeader: {
      BoidRuleFollowLeader *rule_follow = MEM_callocN<BoidRuleFollowLeader>(
          "BoidRuleFollowLeader");
      rule_follow->distance = 1.0f;
      rule = reinterpret_cast<BoidRule *>(rule_follow);
      break;
    }
    case eBoidRuleType_AverageSpeed: {
      BoidRuleAverageSpeed *rule_avgspeed = MEM_callocN<BoidRuleAverageSpeed>(
          "BoidRuleAverageSpeed");
      rule_avgspeed->speed = 0.5f;
      rule = reinterpret_cast<BoidRule *>(rule_avgspeed);
      break;
    }
    case eBoidRuleType_Fight: {
      BoidRuleFight *rule_fight = MEM_callocN<BoidRuleFight>("BoidRuleFight");
      rule_fight->distance = 100.0f;
      rule_fight->flee_distance = 100.0f;
      rule = reinterpret_cast<BoidRule *>(rule_fight);
      break;
    }
    default:
      rule = MEM_callocN<BoidRule>("BoidRule");
      break;
  }

  rule->type = type;
  rule->flag |= BOIDRULE_IN_AIR | BOIDRULE_ON_LAND;
  STRNCPY_UTF8(rule->name, DATA_(rna_enum_boidrule_type_items[type - 1].name));

  return rule;
}
void boid_default_settings(BoidSettings *boids)
{
  boids->air_max_speed = 10.0f;
  boids->air_max_acc = 0.5f;
  boids->air_max_ave = 0.5f;
  boids->air_personal_space = 1.0f;

  boids->land_max_speed = 5.0f;
  boids->land_max_acc = 0.5f;
  boids->land_max_ave = 0.5f;
  boids->land_personal_space = 1.0f;

  boids->options = BOID_ALLOW_FLIGHT;

  boids->landing_smoothness = 3.0f;
  boids->banking = 1.0f;
  boids->pitch = 1.0f;
  boids->height = 1.0f;

  boids->health = 1.0f;
  boids->accuracy = 1.0f;
  boids->aggression = 2.0f;
  boids->range = 1.0f;
  boids->strength = 0.1f;
}

BoidState *boid_new_state(BoidSettings *boids)
{
  BoidState *state = MEM_callocN<BoidState>("BoidState");

  state->id = boids->last_state_id++;
  if (state->id) {
    SNPRINTF_UTF8(state->name, "State %i", state->id);
  }
  else {
    STRNCPY_UTF8(state->name, "State");
  }

  state->rule_fuzziness = 0.5;
  state->volume = 1.0f;
  state->channels |= ~0;

  return state;
}

BoidState *boid_duplicate_state(BoidSettings *boids, BoidState *state)
{
  BoidState *staten = static_cast<BoidState *>(MEM_dupallocN(state));

  BLI_duplicatelist(&staten->rules, &state->rules);
  BLI_duplicatelist(&staten->conditions, &state->conditions);
  BLI_duplicatelist(&staten->actions, &state->actions);

  staten->id = boids->last_state_id++;

  return staten;
}
void boid_free_settings(BoidSettings *boids)
{
  if (boids) {
    BoidState *state = static_cast<BoidState *>(boids->states.first);

    for (; state; state = state->next) {
      BLI_freelistN(&state->rules);
      BLI_freelistN(&state->conditions);
      BLI_freelistN(&state->actions);
    }

    BLI_freelistN(&boids->states);

    MEM_freeN(boids);
  }
}
BoidSettings *boid_copy_settings(const BoidSettings *boids)
{
  BoidSettings *nboids = nullptr;

  if (boids) {
    BoidState *state;
    BoidState *nstate;

    nboids = static_cast<BoidSettings *>(MEM_dupallocN(boids));

    BLI_duplicatelist(&nboids->states, &boids->states);

    state = static_cast<BoidState *>(boids->states.first);
    nstate = static_cast<BoidState *>(nboids->states.first);
    for (; state; state = state->next, nstate = nstate->next) {
      BLI_duplicatelist(&nstate->rules, &state->rules);
      BLI_duplicatelist(&nstate->conditions, &state->conditions);
      BLI_duplicatelist(&nstate->actions, &state->actions);
    }
  }

  return nboids;
}
BoidState *boid_get_current_state(BoidSettings *boids)
{
  BoidState *state = static_cast<BoidState *>(boids->states.first);

  for (; state; state = state->next) {
    if (state->flag & BOIDSTATE_CURRENT) {
      break;
    }
  }

  return state;
}
