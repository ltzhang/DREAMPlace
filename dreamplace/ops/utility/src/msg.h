/**
 * @file   Msg.h
 * @author Yibo Lin
 * @date   Jan 2019
 */

#ifndef DREAMPLACE_UTILITY_MSG_H
#define DREAMPLACE_UTILITY_MSG_H

#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include "utility/src/namespace.h"

DREAMPLACE_BEGIN_NAMESPACE

/// message type for print functions
enum MessageType {
  kNONE = 0,
  kINFO = 1,
  kWARN = 2,
  kERROR = 3,
  kDEBUG = 4,
  kASSERT = 5
};

/// print to screen (stdout)
int dreamplacePrint(MessageType m, const char* format, ...);
/// print to stream
int dreamplacePrintStream(MessageType m, FILE* stream, const char* format, ...);
/// core function to print formatted data from variable argument list
int dreamplaceVPrintStream(MessageType m, FILE* stream, const char* format,
                           va_list args);
/// format into a buffer of a known capacity; never writes past @p capacity and
/// reports truncation on stderr.  Returns the number of characters written,
/// or a negative value on an encoding error.
int dreamplaceSPrintN(MessageType m, char* buf, std::size_t capacity,
                      const char* format, ...);
/// core function to format a buffer of a known capacity
int dreamplaceVSPrintN(MessageType m, char* buf, std::size_t capacity,
                       const char* format, va_list args);
/// format prefix into a buffer of a known capacity
int dreamplaceSPrintPrefixN(MessageType m, char* prefix, std::size_t capacity);

/// Array-reference overloads: the capacity is deduced from the destination, so
/// an ordinary `char buf[N]` call site is bounded with no change.  A bare
/// `char*` destination no longer compiles, which is deliberate — the capacity
/// has to come from somewhere.
template <std::size_t N, typename... Args>
int dreamplaceSPrint(MessageType m, char (&buf)[N], const char* format,
                     Args... args) {
  return dreamplaceSPrintN(m, buf, N, format, args...);
}
template <std::size_t N>
int dreamplaceVSPrint(MessageType m, char (&buf)[N], const char* format,
                      va_list args) {
  return dreamplaceVSPrintN(m, buf, N, format, args);
}
template <std::size_t N>
int dreamplaceSPrintPrefix(MessageType m, char (&prefix)[N]) {
  return dreamplaceSPrintPrefixN(m, prefix, N);
}

/// Thrown by a failed assertion on a host path.  An in-process engine must not
/// terminate its host: pybind11 translates this into a Python exception, which
/// the WiseSyn drivers catch and turn into a declined placement.
class AssertionFailure : public std::runtime_error {
 public:
  explicit AssertionFailure(const std::string& what)
      : std::runtime_error(what) {}
};

/// assertion
void dreamplacePrintAssertMsg(const char* expr, const char* fileName,
                              unsigned lineNum, const char* funcName,
                              const char* format, ...);
void dreamplacePrintAssertMsg(const char* expr, const char* fileName,
                              unsigned lineNum, const char* funcName);
/// Build the "file:line: func: Assertion `expr' failed" text for a throw.
std::string dreamplaceAssertText(const char* expr, const char* fileName,
                                 unsigned lineNum, const char* funcName,
                                 const char* detail);

// Every `dreamplaceAssert*` site in this package is host code (verified by
// inspection of every .cu/.cuh use), so a throw is always valid here.  Device
// invariants use plain `assert`.
#define DREAMPLACE_ASSERT_FAIL_MSG(expr, detail)    \
  throw ::DREAMPLACE_NAMESPACE::AssertionFailure(   \
      ::DREAMPLACE_NAMESPACE::dreamplaceAssertText( \
          expr, __FILE__, __LINE__, __PRETTY_FUNCTION__, detail))
#define DREAMPLACE_ASSERT_FAIL(expr)                \
  throw ::DREAMPLACE_NAMESPACE::AssertionFailure(   \
      ::DREAMPLACE_NAMESPACE::dreamplaceAssertText( \
          expr, __FILE__, __LINE__, __PRETTY_FUNCTION__, nullptr))

#define dreamplaceAssertMsg(condition, args...)                        \
  do {                                                                 \
    if (!(condition)) {                                                \
      ::DREAMPLACE_NAMESPACE::dreamplacePrintAssertMsg(                \
          #condition, __FILE__, __LINE__, __PRETTY_FUNCTION__, args);  \
      DREAMPLACE_ASSERT_FAIL_MSG(#condition, "see the logged message"); \
    }                                                                  \
  } while (false)
#define dreamplaceAssert(condition)                             \
  do {                                                          \
    if (!(condition)) {                                         \
      ::DREAMPLACE_NAMESPACE::dreamplacePrintAssertMsg(         \
          #condition, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
      DREAMPLACE_ASSERT_FAIL(#condition);                       \
    }                                                           \
  } while (false)

/// static assertion
template <bool>
struct dreamplaceStaticAssert;
template <>
struct dreamplaceStaticAssert<true> {
  dreamplaceStaticAssert(const char* = NULL) {}
};

DREAMPLACE_END_NAMESPACE

#endif
