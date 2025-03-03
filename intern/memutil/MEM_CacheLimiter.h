/* SPDX-FileCopyrightText: 2006-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_memutil
 */

#ifndef __MEM_CACHELIMITER_H__
#define __MEM_CACHELIMITER_H__

/**
 * \section MEM_CacheLimiter
 * This class defines a generic memory cache management system
 * to limit memory usage to a fixed global maximum.
 *
 * \note Please use the C-API in MEM_CacheLimiterC-Api.h for code written in C.
 *
 * Usage example:
 *
 * \code{.cpp}
 * class BigFatImage {
 * public:
 *       ~BigFatImage() { tell_everyone_we_are_gone(this); }
 * };
 *
 * void doit()
 * {
 *     MEM_Cache<BigFatImage> BigFatImages;
 *
 *     MEM_Cache_Handle<BigFatImage>* h = BigFatImages.insert(new BigFatImage);
 *
 *     BigFatImages.enforce_limits();
 *     h->ref();
 *
 *     // work with image...
 *
 *     h->unref();
 *
 *     // leave image in cache.
 * \endcode
 */

#include "MEM_Allocator.h"
#include <vector>

template<class T> class MEM_CacheLimiter;

#ifndef __MEM_CACHELIMITERC_API_H__
extern "C" {
void MEM_CacheLimiter_set_maximum(size_t m);
size_t MEM_CacheLimiter_get_maximum();
void MEM_CacheLimiter_set_disabled(bool disabled);
bool MEM_CacheLimiter_is_disabled(void);
};
#endif

template<class T> class MEM_CacheLimiterHandle {
 public:
  explicit MEM_CacheLimiterHandle(T *data_, MEM_CacheLimiter<T> *parent_)
      : data(data_), parent(parent_)
  {
  }

  void ref()
  {
    refcount++;
  }

  void unref()
  {
    refcount--;
  }

  T *get()
  {
    return data;
  }

  const T *get() const
  {
    return data;
  }

  int get_refcount() const
  {
    return refcount;
  }

  bool can_destroy() const
  {
    return !data || !refcount;
  }

  bool destroy_if_possible()
  {
    if (can_destroy()) {
      delete data;
      data = NULL;
      unmanage();
      return true;
    }
    return false;
  }

  void unmanage()
  {
    parent->unmanage(this);
  }

  void touch()
  {
    parent->touch(this);
  }

 private:
  friend class MEM_CacheLimiter<T>;

  T *data;
  int refcount = 0;
  int pos;
  MEM_CacheLimiter<T> *parent;
};

template<class T> class MEM_CacheLimiter {
 public:
  using MEM_CacheLimiter_DataSize_Func = size_t (*)(void *);
  using MEM_CacheLimiter_ItemPriority_Func = int (*)(void *, int);
  using MEM_CacheLimiter_ItemDestroyable_Func = bool (*)(void *);

  MEM_CacheLimiter(MEM_CacheLimiter_DataSize_Func data_size_func) : data_size_func(data_size_func)
  {
  }

  ~MEM_CacheLimiter()
  {
    int i;
    for (i = 0; i < queue.size(); i++) {
      delete queue[i];
    }
  }

  MEM_CacheLimiterHandle<T> *insert(T *elem)
  {
    queue.push_back(new MEM_CacheLimiterHandle<T>(elem, this));
    queue.back()->pos = queue.size() - 1;
    return queue.back();
  }

  void unmanage(MEM_CacheLimiterHandle<T> *handle)
  {
    int pos = handle->pos;
    queue[pos] = queue.back();
    queue[pos]->pos = pos;
    queue.pop_back();
    delete handle;
  }

  size_t get_memory_in_use()
  {
    size_t size = 0;
    if (data_size_func) {
      int i;
      for (i = 0; i < queue.size(); i++) {
        size += data_size_func(queue[i]->get()->get_data());
      }
    }
    else {
      size = MEM_get_memory_in_use();
    }
    return size;
  }

  void enforce_limits()
  {
    size_t max = MEM_CacheLimiter_get_maximum();
    bool is_disabled = MEM_CacheLimiter_is_disabled();
    size_t mem_in_use, cur_size;

    if (is_disabled) {
      return;
    }

    if (max == 0) {
      return;
    }

    mem_in_use = get_memory_in_use();

    if (mem_in_use <= max) {
      return;
    }

    while (!queue.empty() && mem_in_use > max) {
      MEM_CacheElementPtr elem = get_least_priority_destroyable_element();

      if (!elem) {
        break;
      }

      if (data_size_func) {
        cur_size = data_size_func(elem->get()->get_data());
      }
      else {
        cur_size = mem_in_use;
      }

      if (elem->destroy_if_possible()) {
        if (data_size_func) {
          mem_in_use -= cur_size;
        }
        else {
          mem_in_use -= cur_size - MEM_get_memory_in_use();
        }
      }
    }
  }

  void touch(MEM_CacheLimiterHandle<T> *handle)
  {
    /* If we're using custom priority callback re-arranging the queue
     * doesn't make much sense because we'll iterate it all to get
     * least priority element anyway.
     */
    if (item_priority_func == nullptr) {
      queue[handle->pos] = queue.back();
      queue[handle->pos]->pos = handle->pos;
      queue.pop_back();
      queue.push_back(handle);
      handle->pos = queue.size() - 1;
    }
  }

  void set_item_priority_func(MEM_CacheLimiter_ItemPriority_Func item_priority_func)
  {
    this->item_priority_func = item_priority_func;
  }

  void set_item_destroyable_func(MEM_CacheLimiter_ItemDestroyable_Func item_destroyable_func)
  {
    this->item_destroyable_func = item_destroyable_func;
  }

 private:
  using MEM_CacheElementPtr = MEM_CacheLimiterHandle<T> *;
  using MEM_CacheQueue = std::vector<MEM_CacheElementPtr, MEM_Allocator<MEM_CacheElementPtr>>;
  using iterator = typename MEM_CacheQueue::iterator;

  /* Check whether element can be destroyed when enforcing cache limits */
  bool can_destroy_element(MEM_CacheElementPtr &elem)
  {
    if (!elem->can_destroy()) {
      /* Element is referenced */
      return false;
    }
    if (item_destroyable_func) {
      if (!item_destroyable_func(elem->get()->get_data())) {
        return false;
      }
    }
    return true;
  }

  MEM_CacheElementPtr get_least_priority_destroyable_element()
  {
    if (queue.empty()) {
      return NULL;
    }

    MEM_CacheElementPtr best_match_elem = NULL;

    if (!item_priority_func) {
      for (iterator it = queue.begin(); it != queue.end(); it++) {
        MEM_CacheElementPtr elem = *it;
        if (!can_destroy_element(elem)) {
          continue;
        }
        best_match_elem = elem;
        break;
      }
    }
    else {
      int best_match_priority = 0;
      int i;

      for (i = 0; i < queue.size(); i++) {
        MEM_CacheElementPtr elem = queue[i];

        if (!can_destroy_element(elem)) {
          continue;
        }

        /* By default 0 means highest priority element. */
        /* Casting a size type to int is questionable,
         * but unlikely to cause problems. */
        int priority = -((int)(queue.size()) - i - 1);
        priority = item_priority_func(elem->get()->get_data(), priority);

        if (priority < best_match_priority || best_match_elem == NULL) {
          best_match_priority = priority;
          best_match_elem = elem;
        }
      }
    }

    return best_match_elem;
  }

  MEM_CacheQueue queue;
  MEM_CacheLimiter_DataSize_Func data_size_func;
  MEM_CacheLimiter_ItemPriority_Func item_priority_func;
  MEM_CacheLimiter_ItemDestroyable_Func item_destroyable_func;
};

#endif  // __MEM_CACHELIMITER_H__
