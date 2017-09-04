/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2012      by Olivier Goffart <ogoffart@woboq.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "csync_private.h"
#include "csync_rename.h"

#include <string>
#include <vector>
#include <algorithm>

static std::string _parentDir(const std::string &path) {
    int len = path.length();
    while(len > 0 && path[len-1]!='/') len--;
    while(len > 0 && path[len-1]=='/') len--;
    return path.substr(0, len);
}

void csync_rename_record(CSYNC* ctx, const char* from, const char* to)
{
    ctx->renames.folder_renamed_to[from] = to;
    ctx->renames.folder_renamed_from[to] = from;
}

char* csync_rename_adjust_path(CSYNC* ctx, const char* path)
{
    for (std::string p = _parentDir(path); !p.empty(); p = _parentDir(p)) {
        std::map< std::string, std::string >::iterator it = ctx->renames.folder_renamed_to.find(p);
        if (it != ctx->renames.folder_renamed_to.end()) {
            std::string rep = it->second + (path + p.length());
            return c_strdup(rep.c_str());
        }
    }
    return c_strdup(path);
}

char* csync_rename_adjust_path_source(CSYNC* ctx, const char* path)
{
    for (std::string p = _parentDir(path); !p.empty(); p = _parentDir(p)) {
        std::map< std::string, std::string >::iterator it = ctx->renames.folder_renamed_from.find(p);
        if (it != ctx->renames.folder_renamed_from.end()) {
            std::string rep = it->second + (path + p.length());
            return c_strdup(rep.c_str());
        }
    }
    return c_strdup(path);
}

bool csync_rename_count(CSYNC *ctx) {
    return ctx->renames.folder_renamed_from.size();
}
