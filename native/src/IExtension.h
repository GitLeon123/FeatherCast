#pragma once

#include <stdint.h>

#ifdef _WIN32
#define FEATHERCAST_EXTENSION_EXPORT extern "C" __declspec(dllexport)
#else
#define FEATHERCAST_EXTENSION_EXPORT extern "C"
#endif

#define FEATHERCAST_EXTENSION_API_VERSION_1 1u
#define FEATHERCAST_EXTENSION_API_VERSION_2 2u
#define FEATHERCAST_EXTENSION_API_VERSION FEATHERCAST_EXTENSION_API_VERSION_2

typedef uint32_t (*FeatherCastExtensionApiVersionFn)();

/* Handles one request and writes a NUL-terminated UTF-8 JSON response.
 * Returns the response size including the terminating NUL (0 on failure).
 * The host calls this exactly once per request with a buffer of the maximum
 * response size (1 MiB); a larger response is reported as an error and the
 * request is not retried. The plugin's stdin and stdout are connected to NUL,
 * so console output from a plugin never reaches the host protocol. */
typedef uint32_t (*FeatherCastExtensionHandleJsonFn)(const char* requestUtf8,
                                                  char* responseUtf8,
                                                  uint32_t responseCapacity);

