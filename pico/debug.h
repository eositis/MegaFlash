#ifndef _DEBUG_H
#define _DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>

//Debug Log Levels
#define LEVEL_NOLOG 5
#define LEVEL_ERROR 4
#define LEVEL_WARN  3
#define LEVEL_INFO  2
#define LEVEL_DEBUG 1
#define LEVEL_TRACE 0


//Current Debug Log Level
#define LOG_LEVEL LEVEL_DEBUG


#if (!defined NDEBUG) && (LOG_LEVEL<=LEVEL_ERROR)
	#define ERROR_PRINTF(...) printf( __VA_ARGS__)
#else
	#define ERROR_PRINTF(...) do {} while (0)
#endif

#if (!defined NDEBUG) && (LOG_LEVEL<=LEVEL_WARN)
	#define WARN_PRINTF(...) printf( __VA_ARGS__)
#else
	#define WARN_PRINTF(...) do {} while (0)
#endif

#if (!defined NDEBUG) && (LOG_LEVEL<=LEVEL_INFO)
	#define INFO_PRINTF(...) printf( __VA_ARGS__)
#else
	#define INFO_PRINTF(...) do {} while (0)
#endif

#if (!defined NDEBUG) && (LOG_LEVEL<=LEVEL_DEBUG)
	#define DEBUG_PRINTF(...) printf( __VA_ARGS__)
#else
	#define DEBUG_PRINTF(...) do {} while (0)
#endif

#if (!defined NDEBUG) && (LOG_LEVEL<=LEVEL_TRACE)
	#define TRACE_PRINTF(...) printf( __VA_ARGS__)
#else
	#define TRACE_PRINTF(...) do {} while (0)
#endif

void StartTimer();
uint32_t EndTimer();

#ifdef __cplusplus
}
#endif

#endif
