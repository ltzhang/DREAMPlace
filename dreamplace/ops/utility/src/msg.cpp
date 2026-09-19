/*************************************************************************
    > File Name: Msg.cpp
    > Author: Yibo Lin
    > Mail: yibolin@utexas.edu
    > Created Time: Fri 31 Jul 2015 03:20:14 PM CDT
 ************************************************************************/

#include "utility/src/msg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

DREAMPLACE_BEGIN_NAMESPACE

int dreamplacePrint(MessageType m, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int ret = dreamplaceVPrintStream(m, stdout, format, args);
  va_end(args);

  return ret;
}

int dreamplacePrintStream(MessageType m, FILE* stream, const char* format,
                          ...) {
  va_list args;
  va_start(args, format);
  int ret = dreamplaceVPrintStream(m, stream, format, args);
  va_end(args);

  return ret;
}

int dreamplaceVPrintStream(MessageType m, FILE* stream, const char* format,
                           va_list args) {
  // print prefix
  char prefix[16];
  dreamplaceSPrintPrefix(m, prefix);
  fprintf(stream, "%s", prefix);

  // print message
  int ret = vfprintf(stream, format, args);

  return ret;
}

int dreamplaceSPrintN(MessageType m, char* buf, std::size_t capacity,
                      const char* format, ...) {
  va_list args;
  va_start(args, format);
  int ret = dreamplaceVSPrintN(m, buf, capacity, format, args);
  va_end(args);

  return ret;
}

int dreamplaceVSPrintN(MessageType m, char* buf, std::size_t capacity,
                       const char* format, va_list args) {
  if (buf == NULL || capacity == 0) {
    return -1;
  }
  buf[0] = '\0';

  // print prefix
  char prefix[16];
  if (dreamplaceSPrintPrefixN(m, prefix, sizeof(prefix)) < 0) {
    return -1;
  }
  int written = snprintf(buf, capacity, "%s", prefix);
  if (written < 0) {
    return -1;
  }
  std::size_t used = (std::size_t)written;
  if (used >= capacity) {
    fprintf(stderr,
            "[ERROR  ] message buffer of %zu bytes truncated while writing the "
            "prefix\n",
            capacity);
    return -1;
  }

  // print message
  int ret = vsnprintf(buf + used, capacity - used, format, args);
  if (ret < 0) {
    return -1;
  }
  if ((std::size_t)ret >= capacity - used) {
    fprintf(stderr,
            "[ERROR  ] message buffer of %zu bytes truncated: %d bytes were "
            "needed\n",
            capacity, (int)used + ret);
  }

  return ret;
}

int dreamplaceSPrintPrefixN(MessageType m, char* prefix,
                            std::size_t capacity) {
  if (prefix == NULL || capacity == 0) {
    return -1;
  }
  const char* text = NULL;
  switch (m) {
    case kNONE:
      text = "";
      break;
    case kINFO:
      text = "[INFO   ] ";
      break;
    case kWARN:
      text = "[WARNING] ";
      break;
    case kERROR:
      text = "[ERROR  ] ";
      break;
    case kDEBUG:
      text = "[DEBUG  ] ";
      break;
    case kASSERT:
      text = "[ASSERT ] ";
      break;
    default:
      prefix[0] = '\0';
      fprintf(stderr, "[ERROR  ] unknown message type %d\n", (int)m);
      return -1;
  }
  int written = snprintf(prefix, capacity, "%s", text);
  if (written < 0 || (std::size_t)written >= capacity) {
    prefix[0] = '\0';
    return -1;
  }
  return written;
}

std::string dreamplaceAssertText(const char* expr, const char* fileName,
                                 unsigned lineNum, const char* funcName,
                                 const char* detail) {
  char buf[1024];
  if (detail != NULL) {
    snprintf(buf, sizeof(buf), "%s:%u: %s: Assertion `%s' failed: %s", fileName,
             lineNum, funcName, expr, detail);
  } else {
    snprintf(buf, sizeof(buf), "%s:%u: %s: Assertion `%s' failed", fileName,
             lineNum, funcName, expr);
  }
  return std::string(buf);
}

void dreamplacePrintAssertMsg(const char* expr, const char* fileName,
                              unsigned lineNum, const char* funcName,
                              const char* format, ...) {
  // construct message
  char buf[1024];
  va_list args;
  va_start(args, format);
  int ret = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (ret < 0) {
    buf[0] = '\0';
  } else if ((std::size_t)ret >= sizeof(buf)) {
    fprintf(stderr,
            "[ERROR  ] assertion detail truncated to %zu bytes (%d needed)\n",
            sizeof(buf), ret);
  }

  // print message
  dreamplacePrintStream(kASSERT, stderr,
                        "%s:%u: %s: Assertion `%s' failed: %s\n", fileName,
                        lineNum, funcName, expr, buf);
}

void dreamplacePrintAssertMsg(const char* expr, const char* fileName,
                              unsigned lineNum, const char* funcName) {
  // print message
  dreamplacePrintStream(kASSERT, stderr, "%s:%u: %s: Assertion `%s' failed\n",
                        fileName, lineNum, funcName, expr);
}

DREAMPLACE_END_NAMESPACE
