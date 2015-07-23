% -*- Mode: Erlang; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

%% @copyright 2015 Couchbase, Inc.
%%
%% @author Volker Mische  <volker@couchbase.com>
%%
%% Licensed under the Apache License, Version 2.0 (the "License"); you may not
%% use this file except in compliance with the License. You may obtain a copy of
%% the License at
%%
%%  http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
%% WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
%% License for the specific language governing permissions and limitations under
%% the License.

-module(crc32).

-export([crc32/1, crc32/2]).

-on_load(init/0).

% The name of the application we're writing. This is the name
% used for the Erlang .app file.
-define(APPNAME, crc32).

% The name of the shared library we're going to load the NIF
% code from. Defined in rebar.config as so_name.
-define(LIBNAME, crc32_nif).

%% API

crc32(Data) ->
    crc32(0, Data).

crc32(_OldCrc, _Data) ->
    not_loaded(?LINE).


%% Iternal functions

init() ->
    SoName = case code:priv_dir(?APPNAME) of
        {error, bad_name} ->
            case filelib:is_dir(filename:join(["..", priv])) of
                true ->
                    filename:join(["..", priv, ?LIBNAME]);
                _ ->
                    filename:join([priv, ?LIBNAME])
            end;
        Dir ->
            filename:join(Dir, ?LIBNAME)
    end,
    erlang:load_nif(SoName, 0).

not_loaded(Line) ->
    exit({not_loaded, [{module, ?MODULE}, {line, Line}]}).
