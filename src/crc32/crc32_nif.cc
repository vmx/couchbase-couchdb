/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/**
 * @copyright 2015 Couchbase, Inc.
 *
 * @author Volker Mische  <volker@couchbase.com>
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
#include "erl_nif.h"

#include "platform/visibility.h"
#include "platform/crc32c.h"



static ERL_NIF_TERM
crc32(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary bin;
    uint32_t old_crc;

    if(!enif_get_uint(env, argv[0], &old_crc)) {
        return enif_make_badarg(env);
    }
    if(!enif_inspect_iolist_as_binary(env, argv[1], &bin)) {
        return enif_make_badarg(env);
    }

    uint32_t crc32 = crc32c(bin.data, bin.size, old_crc);
    return enif_make_uint(env, crc32);
}


static ErlNifFunc nif_funcs[] = {
    {"crc32", 2, crc32}
};

// Trond: Due to the stupid macros I need to manually do this in order
// to get the correct linkage attributes :P
extern "C" {
    EXPORT_SYMBOL ErlNifEntry* nif_init(void);
}

ERL_NIF_INIT(crc32, nif_funcs, NULL, NULL, NULL, NULL)
