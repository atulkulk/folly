/*
 * Copyright 2016-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <boost/intrusive/list.hpp>

#include <folly/ThreadLocal.h>
#include <folly/detail/Singleton.h>
#include <folly/functional/Invoke.h>

namespace folly {

/// SingletonThreadLocal
///
/// Useful for a per-thread leaky-singleton model in libraries and applications.
///
/// By "leaky" it is meant that the T instances held by the instantiation
/// SingletonThreadLocal<T> will survive until their owning thread exits.
/// Therefore, they can safely be used before main() begins and after main()
/// ends, and they can also safely be used in an application that spawns many
/// temporary threads throughout its life.
///
/// Example:
///
///   struct UsefulButHasExpensiveCtor {
///     UsefulButHasExpensiveCtor(); // this is expensive
///     Result operator()(Arg arg);
///   };
///
///   Result useful(Arg arg) {
///     using Useful = UsefulButHasExpensiveCtor;
///     auto& useful = folly::SingletonThreadLocal<Useful>::get();
///     return useful(arg);
///   }
///
/// As an example use-case, the random generators in <random> are expensive to
/// construct. And their constructors are deterministic, but many cases require
/// that they be randomly seeded. So folly::Random makes good canonical uses of
/// folly::SingletonThreadLocal so that a seed is computed from the secure
/// random device once per thread, and the random generator is constructed with
/// the seed once per thread.
///
/// Keywords to help people find this class in search:
/// Thread Local Singleton ThreadLocalSingleton
template <
    typename T,
    typename Tag = detail::DefaultTag,
    typename Make = detail::DefaultMake<T>>
class SingletonThreadLocal {
 private:
  struct Wrapper;

  using NodeBase = boost::intrusive::list_base_hook<
      boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

  struct Node : NodeBase {
    Wrapper*& cache;
    bool& stale;

    Node(Wrapper*& cache_, bool& stale_) : cache(cache_), stale(stale_) {
      auto& wrapper = getWrapper();
      wrapper.caches.push_front(*this);
      cache = &wrapper;
    }
    ~Node() {
      clear();
    }

    void clear() {
      cache = nullptr;
      stale = true;
    }
  };

  using List =
      boost::intrusive::list<Node, boost::intrusive::constant_time_size<false>>;

  struct Wrapper {
    template <typename S>
    using MakeRet = is_invocable_r<S, Make>;

    // keep as first field, to save 1 instr in the fast path
    union {
      alignas(alignof(T)) unsigned char storage[sizeof(T)];
      T object;
    };
    List caches;

    /* implicit */ operator T&() {
      return object;
    }

    // normal make types
    template <typename S = T, _t<std::enable_if<MakeRet<S>::value, int>> = 0>
    Wrapper() {
      (void)new (storage) S(Make{}());
    }
    // default and special make types for non-move-constructible T, until C++17
    template <typename S = T, _t<std::enable_if<!MakeRet<S>::value, int>> = 0>
    Wrapper() {
      (void)Make{}(storage);
    }
    ~Wrapper() {
      for (auto& node : caches) {
        node.clear();
      }
      caches.clear();
      object.~T();
    }
  };

  SingletonThreadLocal() = delete;

  FOLLY_EXPORT FOLLY_NOINLINE static ThreadLocal<Wrapper>& getWrapperTL() {
    static auto& entry = *detail::createGlobal<ThreadLocal<Wrapper>, Tag>();
    return entry;
  }

  FOLLY_NOINLINE static Wrapper& getWrapper() {
    return *getWrapperTL();
  }

#ifdef FOLLY_TLS
  FOLLY_NOINLINE static T& getSlow(Wrapper*& cache) {
    static thread_local Wrapper** check = &cache;
    CHECK_EQ(check, &cache) << "inline function static thread_local merging";
    static thread_local bool stale;
    static thread_local Node node(cache, stale);
    return !stale && node.cache ? *node.cache : getWrapper();
  }
#endif

 public:
  FOLLY_EXPORT FOLLY_ALWAYS_INLINE static T& get() {
#ifdef FOLLY_TLS
    static thread_local Wrapper* cache;
    return FOLLY_LIKELY(!!cache) ? *cache : getSlow(cache);
#else
    return getWrapper();
#endif
  }
};
} // namespace folly
