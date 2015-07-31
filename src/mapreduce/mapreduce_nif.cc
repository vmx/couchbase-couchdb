/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/**
 * @copyright 2012 Couchbase, Inc.
 *
 * @author Filipe Manana  <filipe@couchbase.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations under
 * the License.
 **/

#ifdef WIN32
#define NOMINMAX
#endif

#include <platform/platform.h>
#include <algorithm>
#include <iostream>
#include <cstring>
#include <sstream>
#include <map>

#if defined(WIN32) || defined(_WIN32)
#define doSleep(Ms) Sleep(Ms)
#else
#define doSleep(Ms)                             \
    do {                                        \
        struct timespec ts;                     \
        ts.tv_sec = Ms / 1000;                  \
        ts.tv_nsec = (Ms % 1000) * 1000000;     \
        nanosleep(&ts, NULL);                   \
    } while(0)
#endif

#include "erl_nif_compat.h"
#include "mapreduce.h"

// NOTE: keep this file clean (without knowledge) of any V8 APIs

static ERL_NIF_TERM ATOM_OK;
static ERL_NIF_TERM ATOM_ERROR;

// maxTaskDuration is in seconds
static volatile int                                maxTaskDuration = 5;
static int                                         maxKvSize = 1 * 1024 * 1024;
static ErlNifResourceType                          *MAP_REDUCE_CTX_RES;
static ErlNifTid                                   terminatorThreadId;
static ErlNifMutex                                 *terminatorMutex;
static volatile int                                shutdownTerminator = 0;
static std::map< unsigned int, map_reduce_ctx_t* > contexts;


// NIF API functions
static ERL_NIF_TERM startMapContext(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM doMapDoc(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM startReduceContext(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM doReduce(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM doRereduce(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM setTimeout(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);
static ERL_NIF_TERM setMaxKvSize(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]);

// NIF API callbacks
static int onLoad(ErlNifEnv* env, void** priv, ERL_NIF_TERM info);
static void onUnload(ErlNifEnv *env, void *priv_data);

// Utilities
static ERL_NIF_TERM makeError(ErlNifEnv *env, const std::string &msg);
static bool parseFunctions(ErlNifEnv *env, ERL_NIF_TERM functionsArg, function_sources_list_t &result);

// NIF resource functions
static void free_map_reduce_context(ErlNifEnv *env, void *res);

static inline void registerContext(map_reduce_ctx_t *ctx, ErlNifEnv *env, const ERL_NIF_TERM &refTerm);
static inline void unregisterContext(map_reduce_ctx_t *ctx);
static void *terminatorLoop(void *);



ERL_NIF_TERM startMapContext(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    char viewTypeAtom[256];
    view_index_type_t viewType;
    function_sources_list_t mapFunctions;

    if (!enif_get_atom(env, argv[0], viewTypeAtom, sizeof(viewTypeAtom),
                       ERL_NIF_LATIN1)) {
        return enif_make_badarg(env);
    }
    if (!strcmp(viewTypeAtom, "mapreduce_view")) {
        viewType = VIEW_INDEX_TYPE_MAPREDUCE;
    } else if (!strcmp(viewTypeAtom, "spatial_view")) {
        viewType = VIEW_INDEX_TYPE_SPATIAL;
    } else {
        return makeError(env, "unknown view type");
    }

    if (!parseFunctions(env, argv[1], mapFunctions)) {
        return enif_make_badarg(env);
    }

    map_reduce_ctx_t *ctx = static_cast<map_reduce_ctx_t *>(
        enif_alloc_resource(MAP_REDUCE_CTX_RES, sizeof(map_reduce_ctx_t)));

    try {
        initContext(ctx, mapFunctions, viewType);

        ERL_NIF_TERM res = enif_make_resource(env, ctx);
        enif_release_resource(ctx);

        registerContext(ctx, env, argv[2]);

        return enif_make_tuple2(env, ATOM_OK, res);

    } catch(MapReduceError &e) {
        return makeError(env, e.getMsg());
    } catch(std::bad_alloc &) {
        return makeError(env, "memory allocation failure");
    }
}


ERL_NIF_TERM doMapDoc(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    map_reduce_ctx_t *ctx;

    if (!enif_get_resource(env, argv[0], MAP_REDUCE_CTX_RES, reinterpret_cast<void **>(&ctx))) {
        return enif_make_badarg(env);
    }
    ctx->env = env;
    ctx->maxEmitKvSize = maxKvSize;

    ErlNifBinary docBin;

    if (!enif_inspect_iolist_as_binary(env, argv[1], &docBin)) {
        return enif_make_badarg(env);
    }

    ErlNifBinary metaBin;

    if (!enif_inspect_iolist_as_binary(env, argv[2], &metaBin)) {
        return enif_make_badarg(env);
    }

    try {
        // Map results is a list of lists. An inner list is the list of key value
        // pairs emitted by a map function for the document.
        map_results_list_t mapResults = mapDoc(ctx, docBin, metaBin);
        ERL_NIF_TERM outerList = enif_make_list(env, 0);
        ERL_NIF_TERM logList = enif_make_list(env, 0);
        map_results_list_t::reverse_iterator i = mapResults.rbegin();

        for ( ; i != mapResults.rend(); ++i) {
            map_result_t mapResult = *i;

            switch (mapResult.type) {
            case MAP_KVS:
                {
                    ERL_NIF_TERM kvList = enif_make_list(env, 0);
                    kv_pair_list_t::reverse_iterator j = mapResult.result.kvs->rbegin();

                    for ( ; j != mapResult.result.kvs->rend(); ++j) {
                        ERL_NIF_TERM key = enif_make_binary(env, &j->first);
                        ERL_NIF_TERM value = enif_make_binary(env, &j->second);
                        ERL_NIF_TERM kvPair = enif_make_tuple2(env, key, value);
                        kvList = enif_make_list_cell(env, kvPair, kvList);
                    }
                    mapResult.result.kvs->~kv_pair_list_t();
                    enif_free(mapResult.result.kvs);
                    outerList = enif_make_list_cell(env, kvList, outerList);
                }
                break;
            case MAP_ERROR:
                ERL_NIF_TERM reason = enif_make_binary(env, mapResult.result.error);
                ERL_NIF_TERM errorTuple = enif_make_tuple2(env, ATOM_ERROR, reason);

                enif_free(mapResult.result.error);
                outerList = enif_make_list_cell(env, errorTuple, outerList);
                break;
            }
        }
        if (ctx->logResults) {
            log_results_list_t::reverse_iterator k = ctx->logResults->rbegin();
            for ( ; k != ctx->logResults->rend(); ++k) {
                ERL_NIF_TERM logMsg = enif_make_binary(env, &(*k));
                logList = enif_make_list_cell(env, logMsg, logList);
            }
            ctx->logResults->~log_results_list_t();
            enif_free(ctx->logResults);
            ctx->logResults = NULL;
        }
        return enif_make_tuple3(env, ATOM_OK, outerList, logList);

    } catch(MapReduceError &e) {
        return makeError(env, e.getMsg());
    } catch(std::bad_alloc &) {
        return makeError(env, "memory allocation failure");
    }
}


ERL_NIF_TERM startReduceContext(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    function_sources_list_t reduceFunctions;

    if (!parseFunctions(env, argv[0], reduceFunctions)) {
        return enif_make_badarg(env);
    }

    map_reduce_ctx_t *ctx = static_cast<map_reduce_ctx_t *>(
        enif_alloc_resource(MAP_REDUCE_CTX_RES, sizeof(map_reduce_ctx_t)));

    try {
        initContext(ctx, reduceFunctions, VIEW_INDEX_TYPE_MAPREDUCE);

        ERL_NIF_TERM res = enif_make_resource(env, ctx);
        enif_release_resource(ctx);

        registerContext(ctx, env, argv[1]);

        return enif_make_tuple2(env, ATOM_OK, res);

    } catch(MapReduceError &e) {
        return makeError(env, e.getMsg());
    } catch(std::bad_alloc &) {
        return makeError(env, "memory allocation failure");
    }
}


ERL_NIF_TERM doReduce(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    map_reduce_ctx_t *ctx;

    if (!enif_get_resource(env, argv[0], MAP_REDUCE_CTX_RES, reinterpret_cast<void **>(&ctx))) {
        return enif_make_badarg(env);
    }
    ctx->env = env;

    int reduceFunNum = -1;
    json_results_list_t keys;
    json_results_list_t values;
    ERL_NIF_TERM tail;
    ERL_NIF_TERM head;

    if (!enif_get_int(env, argv[1], &reduceFunNum)) {
        if (!enif_is_list(env, argv[1])) {
            return enif_make_badarg(env);
        }
        tail = argv[1];
    } else {
        if (!enif_is_list(env, argv[2])) {
            return enif_make_badarg(env);
        }
        tail = argv[2];
    }

    while (enif_get_list_cell(env, tail, &head, &tail)) {
        const ERL_NIF_TERM* array;
        int arity;

        if (!enif_get_tuple(env, head, &arity, &array)) {
            return enif_make_badarg(env);
        }
        if (arity != 2) {
            return enif_make_badarg(env);
        }

        ErlNifBinary keyBin;
        ErlNifBinary valueBin;

        if (!enif_inspect_iolist_as_binary(env, array[0], &keyBin)) {
            return enif_make_badarg(env);
        }
        if (!enif_inspect_iolist_as_binary(env, array[1], &valueBin)) {
            return enif_make_badarg(env);
        }

        keys.push_back(keyBin);
        values.push_back(valueBin);
    }

    try {
        if (reduceFunNum == -1) {
            json_results_list_t results = runReduce(ctx, keys, values);

            ERL_NIF_TERM list = enif_make_list(env, 0);
            json_results_list_t::reverse_iterator it = results.rbegin();

            for ( ; it != results.rend(); ++it) {
                ErlNifBinary reduction = *it;

                list = enif_make_list_cell(env, enif_make_binary(env, &reduction), list);
            }

            return enif_make_tuple2(env, ATOM_OK, list);
        } else {
            ErlNifBinary reduction = runReduce(ctx, reduceFunNum, keys, values);

            return enif_make_tuple2(env, ATOM_OK, enif_make_binary(env, &reduction));
        }

    } catch(MapReduceError &e) {
        return makeError(env, e.getMsg());
    } catch(std::bad_alloc &) {
        return makeError(env, "memory allocation failure");
    }
}


ERL_NIF_TERM doRereduce(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    map_reduce_ctx_t *ctx;

    if (!enif_get_resource(env, argv[0], MAP_REDUCE_CTX_RES, reinterpret_cast<void **>(&ctx))) {
        return enif_make_badarg(env);
    }
    ctx->env = env;

    int reduceFunNum;

    if (!enif_get_int(env, argv[1], &reduceFunNum)) {
        return enif_make_badarg(env);
    }

    if (!enif_is_list(env, argv[2])) {
        return enif_make_badarg(env);
    }

    json_results_list_t reductions;
    ERL_NIF_TERM tail = argv[2];
    ERL_NIF_TERM head;

    while (enif_get_list_cell(env, tail, &head, &tail)) {
        ErlNifBinary reductionBin;

        if (!enif_inspect_iolist_as_binary(env, head, &reductionBin)) {
            return enif_make_badarg(env);
        }

        reductions.push_back(reductionBin);
    }

    try {
        ErlNifBinary result = runRereduce(ctx, reduceFunNum, reductions);

        return enif_make_tuple2(env, ATOM_OK, enif_make_binary(env, &result));
    } catch(MapReduceError &e) {
        return makeError(env, e.getMsg());
    } catch(std::bad_alloc &) {
        return makeError(env, "memory allocation failure");
    }
}


ERL_NIF_TERM setTimeout(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int timeout;

    if (!enif_get_int(env, argv[0], &timeout)) {
        return enif_make_badarg(env);
    }

    maxTaskDuration = (timeout + 999) / 1000;

    return ATOM_OK;
}


ERL_NIF_TERM setMaxKvSize(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int max;

    if (!enif_get_int(env, argv[0], &max)) {
        return enif_make_badarg(env);
    }

    maxKvSize = max;

    return ATOM_OK;
}


int onLoad(ErlNifEnv *env, void **priv, ERL_NIF_TERM info)
{
    ATOM_OK = enif_make_atom(env, "ok");
    ATOM_ERROR = enif_make_atom(env, "error");

    MAP_REDUCE_CTX_RES = enif_open_resource_type(
        env,
        NULL,
        "map_reduce_context",
        free_map_reduce_context,
        ERL_NIF_RT_CREATE,
        NULL);

    if (MAP_REDUCE_CTX_RES == NULL) {
        return -1;
    }

    terminatorMutex = enif_mutex_create(const_cast<char *>("terminator mutex"));
    if (terminatorMutex == NULL) {
        return -2;
    }

    if (enif_thread_create(const_cast<char *>("terminator thread"),
                           &terminatorThreadId,
                           terminatorLoop,
                           NULL,
                           NULL) != 0) {
        enif_mutex_destroy(terminatorMutex);
        return -4;
    }

    return 0;
}


void onUnload(ErlNifEnv *env, void *priv_data)
{
    void *result = NULL;

    shutdownTerminator = 1;
    enif_thread_join(terminatorThreadId, &result);
    enif_mutex_destroy(terminatorMutex);
}


bool parseFunctions(ErlNifEnv *env, ERL_NIF_TERM functionsArg, function_sources_list_t &result)
{
    if (!enif_is_list(env, functionsArg)) {
        return false;
    }

    ERL_NIF_TERM tail = functionsArg;
    ERL_NIF_TERM head;

    while (enif_get_list_cell(env, tail, &head, &tail)) {
        ErlNifBinary funBin;

        if (!enif_inspect_iolist_as_binary(env, head, &funBin)) {
            return false;
        }

        function_source_t src;

        src.reserve(funBin.size + 2);
        src += '(';
        src.append(reinterpret_cast<char *>(funBin.data), funBin.size);
        src += ')';

        result.push_back(src);
    }

    return true;
}


ERL_NIF_TERM makeError(ErlNifEnv *env, const std::string &msg)
{
    ErlNifBinary reason;

    if (!enif_alloc_binary_compat(env, msg.length(), &reason)) {
        return ATOM_ERROR;
    } else {
        memcpy(reason.data, msg.data(), msg.length());
        return enif_make_tuple2(env, ATOM_ERROR, enif_make_binary(env, &reason));
    }
}


void free_map_reduce_context(ErlNifEnv *env, void *res) {
    map_reduce_ctx_t *ctx = static_cast<map_reduce_ctx_t *>(res);

    unregisterContext(ctx);
    destroyContext(ctx);
}


hrtime_t gethrtime_period(void)
{
    /* this isn't actually completely accurate, but who cares ;-) */
    hrtime_t start = gethrtime();
    hrtime_t end = gethrtime() - start;
    if (end == 0) {
       end = 1;
    }

    return end;
}

#define SEC_TO_NSEC 1000000000ULL
#define NSEC_TO_MSEC (1.0/1000000.0)

void *terminatorLoop(void *args)
{
    std::map< unsigned int, map_reduce_ctx_t* >::iterator it;

    while (!shutdownTerminator) {
        // Convert maxTaskDuration to nanoseconds
        const hrtime_t maxTaskTimeNSec = maxTaskDuration * SEC_TO_NSEC;
        // gethrtime() returns values in nanoseconds
        hrtime_t now, minTimeDiff = maxTaskTimeNSec;

        enif_mutex_lock(terminatorMutex);
        now = gethrtime();

        for (it = contexts.begin(); it != contexts.end(); ++it) {
            map_reduce_ctx_t *ctx = (*it).second;
            if (ctx->taskStartTime > 0) {
                int64_t  timeGap = maxTaskTimeNSec -
                        (now - ctx->taskStartTime);
                if ((int64_t)gethrtime_period() > timeGap) {
                    terminateTask(ctx);
                }
                else {
                    minTimeDiff = std::min((hrtime_t)timeGap, minTimeDiff);
                }
            }
        }

        enif_mutex_unlock(terminatorMutex);
        // Convert minTimeDiff to miliseconds
        hrtime_t minTimeMSec = (hrtime_t)(minTimeDiff * NSEC_TO_MSEC);
        doSleep(minTimeMSec);
    }

    return NULL;
}


void registerContext(map_reduce_ctx_t *ctx, ErlNifEnv *env, const ERL_NIF_TERM &refTerm)
{
    if (!enif_get_uint(env, refTerm, &ctx->key)) {
        throw MapReduceError("invalid context reference");
    }

    enif_mutex_lock(terminatorMutex);
    contexts[ctx->key] = ctx;
    enif_mutex_unlock(terminatorMutex);
}


void unregisterContext(map_reduce_ctx_t *ctx)
{
    enif_mutex_lock(terminatorMutex);
    contexts.erase(ctx->key);
    enif_mutex_unlock(terminatorMutex);
}


static ErlNifFunc nif_functions[] = {
    {"start_map_context", 3, startMapContext},
    {"map_doc", 3, doMapDoc},
    {"start_reduce_context", 2, startReduceContext},
    {"reduce", 2, doReduce},
    {"reduce", 3, doReduce},
    {"rereduce", 3, doRereduce},
    {"set_timeout", 1, setTimeout},
    {"set_max_kv_size_per_doc", 1, setMaxKvSize}
};

// Due to the stupid macros I need to manually do this in order
// to get the correct linkage attributes :P
extern "C" {
#if defined (__SUNPRO_C) && (__SUNPRO_C >= 0x550)
    __global ErlNifEntry* nif_init(void);
#elif defined __GNUC__
    __attribute__((visibility("default"))) ErlNifEntry* nif_init(void);
#endif
}

ERL_NIF_INIT(mapreduce, nif_functions, &onLoad, NULL, NULL, &onUnload)
