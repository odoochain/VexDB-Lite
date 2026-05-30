//  Boost config.hpp configuration header file  ------------------------------//

//  (C) Copyright John Maddock 2002.
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/config for most recent version.

//  Boost config.hpp policy and rationale documentation has been moved to
//  http://www.boost.org/libs/config
//
//  CAUTION: This file is intended to be completely stable -
//           DO NOT MODIFY THIS FILE!
//

#ifndef BOOST_CONFIG_HPP
#define BOOST_CONFIG_HPP

// ── vendored-boost trim 补丁 (2026-05-26) ──────────────────────────────────
// 本仓 trim 时删掉了 boost/config/ 子目录(只留这份 config.hpp),下面的 #include
// <boost/config/...> 会 fallback 到系统 boost。但系统 boost 常太老(Ubuntu22=1.74、
// CentOS7=1.53),缺 BOOST_NO_CXX20_HDR_CONCEPTS(boost 1.78+ 才有),导致 vendored(1.91)
// 的 concurrent_static_asserts/lockfree 在 c++17 下误走 std::forward_iterator concept
// 分支、且缺 BOOST_NULLPTR,编译失败。这里在任何 config 子文件 include 前按 C++ 标准
// 显式补定义(boost 子文件都用 #ifndef 保护,不冲突;c++20 下不定义 NO_CXX20_HDR_CONCEPTS,
// 自动走 concept 分支)。preprocessor 等其余基础库仍靠完整 boost 兜底(稳定,跨版本兼容)。
#if __cplusplus < 202002L
#  ifndef BOOST_NO_CXX20_HDR_CONCEPTS
#    define BOOST_NO_CXX20_HDR_CONCEPTS
#  endif
#endif
#ifndef BOOST_NULLPTR
#  define BOOST_NULLPTR nullptr
#endif

// if we don't have a user config, then use the default location:
#if !defined(BOOST_USER_CONFIG) && !defined(BOOST_NO_USER_CONFIG)
#  define BOOST_USER_CONFIG <boost/config/user.hpp>
#if 0
// For dependency trackers:
#  include <boost/config/user.hpp>
#endif
#endif
// include it first:
#ifdef BOOST_USER_CONFIG
#  include BOOST_USER_CONFIG
#endif

// if we don't have a compiler config set, try and find one:
#if !defined(BOOST_COMPILER_CONFIG) && !defined(BOOST_NO_COMPILER_CONFIG) && !defined(BOOST_NO_CONFIG)
#  include <boost/config/detail/select_compiler_config.hpp>
#endif
// if we have a compiler config, include it now:
#ifdef BOOST_COMPILER_CONFIG
#  include BOOST_COMPILER_CONFIG
#endif

// if we don't have a std library config set, try and find one:
#if !defined(BOOST_STDLIB_CONFIG) && !defined(BOOST_NO_STDLIB_CONFIG) && !defined(BOOST_NO_CONFIG) && defined(__cplusplus)
#  include <boost/config/detail/select_stdlib_config.hpp>
#endif
// if we have a std library config, include it now:
#ifdef BOOST_STDLIB_CONFIG
#  include BOOST_STDLIB_CONFIG
#endif

// if we don't have a platform config set, try and find one:
#if !defined(BOOST_PLATFORM_CONFIG) && !defined(BOOST_NO_PLATFORM_CONFIG) && !defined(BOOST_NO_CONFIG)
#  include <boost/config/detail/select_platform_config.hpp>
#endif
// if we have a platform config, include it now:
#ifdef BOOST_PLATFORM_CONFIG
#  include BOOST_PLATFORM_CONFIG
#endif

// get config suffix code:
#include <boost/config/detail/suffix.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#endif  // BOOST_CONFIG_HPP
