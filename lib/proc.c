/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "grn_proc.h"
#include "grn_ii.h"
#include "grn_db.h"
#include "grn_util.h"
#include "grn_output.h"
#include "grn_pat.h"
#include "grn_geo.h"
#include "grn_token_cursor.h"
#include "grn_expr.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef WIN32
# include <io.h>
# include <share.h>
#endif /* WIN32 */

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/**** globals for procs ****/
const char *grn_document_root = NULL;

#define VAR GRN_PROC_GET_VAR_BY_OFFSET

static double grn_between_too_many_index_match_ratio = 0.01;
static double grn_in_values_too_many_index_match_ratio = 0.01;

void
grn_proc_init_from_env(void)
{
  {
    char grn_between_too_many_index_match_ratio_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_BETWEEN_TOO_MANY_INDEX_MATCH_RATIO",
               grn_between_too_many_index_match_ratio_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_between_too_many_index_match_ratio_env[0]) {
      grn_between_too_many_index_match_ratio =
        atof(grn_between_too_many_index_match_ratio_env);
    }
  }

  {
    char grn_in_values_too_many_index_match_ratio_env[GRN_ENV_BUFFER_SIZE];
    grn_getenv("GRN_IN_VALUES_TOO_MANY_INDEX_MATCH_RATIO",
               grn_in_values_too_many_index_match_ratio_env,
               GRN_ENV_BUFFER_SIZE);
    if (grn_in_values_too_many_index_match_ratio_env[0]) {
      grn_in_values_too_many_index_match_ratio =
        atof(grn_in_values_too_many_index_match_ratio_env);
    }
  }
}

/* bulk must be initialized grn_bulk or grn_msg */
static int
grn_bulk_put_from_file(grn_ctx *ctx, grn_obj *bulk, const char *path)
{
  /* FIXME: implement more smartly with grn_bulk */
  int fd, ret = 0;
  struct stat stat;
  grn_open(fd, path, O_RDONLY|O_NOFOLLOW|GRN_OPEN_FLAG_BINARY);
  if (fd == -1) {
    switch (errno) {
    case EACCES :
      ERR(GRN_OPERATION_NOT_PERMITTED, "request is not allowed: <%s>", path);
      break;
    case ENOENT :
      ERR(GRN_NO_SUCH_FILE_OR_DIRECTORY, "no such file: <%s>", path);
      break;
#ifndef WIN32
    case ELOOP :
      ERR(GRN_NO_SUCH_FILE_OR_DIRECTORY,
          "symbolic link is not allowed: <%s>", path);
      break;
#endif /* WIN32 */
    default :
      ERRNO_ERR("failed to open file: <%s>", path);
      break;
    }
    return 0;
  }
  if (fstat(fd, &stat) != -1) {
    char *buf, *bp;
    off_t rest = stat.st_size;
    if ((buf = GRN_MALLOC(rest))) {
      ssize_t ss;
      for (bp = buf; rest; rest -= ss, bp += ss) {
        if ((ss = grn_read(fd, bp, rest)) == -1) { goto exit; }
      }
      GRN_TEXT_PUT(ctx, bulk, buf, stat.st_size);
      ret = 1;
    }
    GRN_FREE(buf);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "cannot stat file: <%s>", path);
  }
exit :
  grn_close(fd);
  return ret;
}

#ifdef stat
#  undef stat
#endif /* stat */

/**** procs ****/

#define DUMP_COLUMNS            "_id, _key, _value, *"

static grn_obj *
proc_load(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_load(ctx, grn_get_ctype(VAR(4)),
           GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)),
           GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)),
           GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)),
           GRN_TEXT_VALUE(VAR(3)), GRN_TEXT_LEN(VAR(3)),
           GRN_TEXT_VALUE(VAR(5)), GRN_TEXT_LEN(VAR(5)));
  if (ctx->impl->loader.stat != GRN_LOADER_END) {
    grn_ctx_set_next_expr(ctx, grn_proc_get_info(ctx, user_data, NULL, NULL, NULL));
  } else {
    if (ctx->impl->loader.rc != GRN_SUCCESS) {
      ctx->rc = ctx->impl->loader.rc;
      strcpy(ctx->errbuf, ctx->impl->loader.errbuf);
    }
    GRN_OUTPUT_INT64(ctx->impl->loader.nrecords);
    if (ctx->impl->loader.table) {
      grn_db_touch(ctx, DB_OBJ(ctx->impl->loader.table)->db);
    }
    /* maybe necessary : grn_ctx_loader_clear(ctx); */
  }
  return NULL;
}

static grn_obj *
proc_status(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_timeval now;
  grn_cache *cache;
  grn_cache_statistics statistics;

  grn_timeval_now(ctx, &now);
  cache = grn_cache_current_get(ctx);
  grn_cache_get_statistics(ctx, cache, &statistics);
  GRN_OUTPUT_MAP_OPEN("RESULT", 10);
  GRN_OUTPUT_CSTR("alloc_count");
  GRN_OUTPUT_INT32(grn_alloc_count());
  GRN_OUTPUT_CSTR("starttime");
  GRN_OUTPUT_INT32(grn_starttime.tv_sec);
  GRN_OUTPUT_CSTR("start_time");
  GRN_OUTPUT_INT32(grn_starttime.tv_sec);
  GRN_OUTPUT_CSTR("uptime");
  GRN_OUTPUT_INT32(now.tv_sec - grn_starttime.tv_sec);
  GRN_OUTPUT_CSTR("version");
  GRN_OUTPUT_CSTR(grn_get_version());
  GRN_OUTPUT_CSTR("n_queries");
  GRN_OUTPUT_INT64(statistics.nfetches);
  GRN_OUTPUT_CSTR("cache_hit_rate");
  if (statistics.nfetches == 0) {
    GRN_OUTPUT_FLOAT(0.0);
  } else {
    double cache_hit_rate;
    cache_hit_rate = (double)statistics.nhits / (double)statistics.nfetches;
    GRN_OUTPUT_FLOAT(cache_hit_rate * 100.0);
  }
  GRN_OUTPUT_CSTR("command_version");
  GRN_OUTPUT_INT32(grn_ctx_get_command_version(ctx));
  GRN_OUTPUT_CSTR("default_command_version");
  GRN_OUTPUT_INT32(grn_get_default_command_version());
  GRN_OUTPUT_CSTR("max_command_version");
  GRN_OUTPUT_INT32(GRN_COMMAND_VERSION_MAX);
  GRN_OUTPUT_MAP_CLOSE();
  return NULL;
}

static grn_obj_flags
grn_parse_column_create_flags(grn_ctx *ctx, const char *nptr, const char *end)
{
  grn_obj_flags flags = 0;
  while (nptr < end) {
    size_t name_size;

    if (*nptr == '|' || *nptr == ' ') {
      nptr += 1;
      continue;
    }

#define CHECK_FLAG(name)                                                \
    name_size = strlen(#name);                                          \
    if ((end - nptr) >= name_size &&                                    \
        memcmp(nptr, #name, name_size) == 0) {                          \
      flags |= GRN_OBJ_ ## name;                                        \
      nptr += name_size;                                                \
      continue;                                                         \
    }

    CHECK_FLAG(COLUMN_SCALAR);
    CHECK_FLAG(COLUMN_VECTOR);
    CHECK_FLAG(COLUMN_INDEX);
    CHECK_FLAG(COMPRESS_ZLIB);
    CHECK_FLAG(COMPRESS_LZ4);
    CHECK_FLAG(WITH_SECTION);
    CHECK_FLAG(WITH_WEIGHT);
    CHECK_FLAG(WITH_POSITION);
    CHECK_FLAG(RING_BUFFER);

#undef CHECK_FLAG

    ERR(GRN_INVALID_ARGUMENT,
        "[column][create][flags] unknown flag: <%.*s>",
        (int)(end - nptr), nptr);
    return 0;
  }
  return flags;
}

static grn_rc
proc_column_create_resolve_source_name(grn_ctx *ctx,
                                       grn_obj *table,
                                       const char *source_name,
                                       int source_name_length,
                                       grn_obj *source_ids)
{
  grn_obj *column;

  column = grn_obj_column(ctx, table, source_name, source_name_length);
  if (!column) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] nonexistent source: <%.*s>",
        source_name_length, source_name);
    return ctx->rc;
  }

  if (column->header.type == GRN_ACCESSOR) {
    if (strncmp(source_name, "_key", source_name_length) == 0) {
      grn_id source_id = grn_obj_id(ctx, table);
      GRN_UINT32_PUT(ctx, source_ids, source_id);
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "[column][create] pseudo column except <_key> is invalid: <%.*s>",
          source_name_length, source_name);
    }
  } else {
    grn_id source_id = grn_obj_id(ctx, column);
    GRN_UINT32_PUT(ctx, source_ids, source_id);
  }
  grn_obj_unlink(ctx, column);

  return ctx->rc;
}

static grn_rc
proc_column_create_resolve_source_names(grn_ctx *ctx,
                                        grn_obj *table,
                                        grn_obj *source_names,
                                        grn_obj *source_ids)
{
  int i, names_length;
  int start, source_name_length;
  const char *names;

  names = GRN_TEXT_VALUE(source_names);
  start = 0;
  source_name_length = 0;
  names_length = GRN_TEXT_LEN(source_names);
  for (i = 0; i < names_length; i++) {
    switch (names[i]) {
    case ' ' :
      if (source_name_length == 0) {
        start++;
      }
      break;
    case ',' :
      {
        grn_rc rc;
        const char *source_name = names + start;
        rc = proc_column_create_resolve_source_name(ctx,
                                                    table,
                                                    source_name,
                                                    source_name_length,
                                                    source_ids);
        if (rc) {
          return rc;
        }
        start = i + 1;
        source_name_length = 0;
      }
      break;
    default :
      source_name_length++;
      break;
    }
  }

  if (source_name_length > 0) {
    grn_rc rc;
    const char *source_name = names + start;
    rc = proc_column_create_resolve_source_name(ctx,
                                                table,
                                                source_name,
                                                source_name_length,
                                                source_ids);
    if (rc) {
      return rc;
    }
  }

  return GRN_SUCCESS;
}

static grn_obj *
proc_column_create(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_bool succeeded = GRN_TRUE;
  grn_obj *column, *table = NULL, *type = NULL;
  const char *rest;
  grn_obj_flags flags = grn_atoi(GRN_TEXT_VALUE(VAR(2)),
                                 GRN_BULK_CURR(VAR(2)), &rest);
  if (GRN_TEXT_VALUE(VAR(2)) == rest) {
    flags = grn_parse_column_create_flags(ctx, GRN_TEXT_VALUE(VAR(2)),
                                          GRN_BULK_CURR(VAR(2)));
    if (ctx->rc) {
      succeeded = GRN_FALSE;
      goto exit;
    }
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] table doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    succeeded = GRN_FALSE;
    goto exit;
  }
  type = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(3)),
                     GRN_TEXT_LEN(VAR(3)));
  if (!type) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][create] type doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(3)), GRN_TEXT_VALUE(VAR(3))) ;
    succeeded = GRN_FALSE;
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(1))) {
    flags |= GRN_OBJ_PERSISTENT;
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[column][create] name is missing");
    succeeded = GRN_FALSE;
    goto exit;
  }
  column = grn_column_create(ctx, table,
                             GRN_TEXT_VALUE(VAR(1)),
                             GRN_TEXT_LEN(VAR(1)),
                             NULL, flags, type);
  if (column) {
    if (GRN_TEXT_LEN(VAR(4))) {
      grn_rc rc;
      grn_obj source_ids;
      GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
      rc = proc_column_create_resolve_source_names(ctx,
                                                   type,
                                                   VAR(4),
                                                   &source_ids);
      if (!rc && GRN_BULK_VSIZE(&source_ids)) {
        grn_obj_set_info(ctx, column, GRN_INFO_SOURCE, &source_ids);
        rc = ctx->rc;
      }
      GRN_OBJ_FIN(ctx, &source_ids);
      if (rc) {
        grn_obj_remove(ctx, column);
        succeeded = GRN_FALSE;
        goto exit;
      }
    }
    grn_obj_unlink(ctx, column);
  } else {
    succeeded = GRN_FALSE;
  }
exit :
  GRN_OUTPUT_BOOL(succeeded);
  if (table) { grn_obj_unlink(ctx, table); }
  if (type) { grn_obj_unlink(ctx, type); }
  return NULL;
}

static grn_obj *
proc_column_remove(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table, *col;
  char *colname,fullname[GRN_TABLE_MAX_KEY_SIZE];
  unsigned int colname_len,fullname_len;

  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)),
                           GRN_TEXT_LEN(VAR(0)));

  colname = GRN_TEXT_VALUE(VAR(1));
  colname_len = GRN_TEXT_LEN(VAR(1));

  if ((fullname_len = grn_obj_name(ctx, table, fullname, GRN_TABLE_MAX_KEY_SIZE))) {
    fullname[fullname_len] = GRN_DB_DELIMITER;
    grn_memcpy((fullname + fullname_len + 1), colname, colname_len);
    fullname_len += colname_len + 1;
    //TODO:check fullname_len < GRN_TABLE_MAX_KEY_SIZE
    col = grn_ctx_get(ctx, fullname, fullname_len);
    if (col) {
      grn_obj_remove(ctx, col);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "[column][remove] column isn't found: <%.*s.%.*s>",
          (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
          colname_len, colname);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[column][remove] table isn't found: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_column_rename(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *table = NULL;
  grn_obj *column = NULL;
  if (GRN_TEXT_LEN(VAR(0)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "[column][rename] table name isn't specified");
    goto exit;
  }
  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] table isn't found: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(1)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] column name isn't specified: <%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    goto exit;
  }
  column = grn_obj_column(ctx, table,
                          GRN_TEXT_VALUE(VAR(1)), GRN_TEXT_LEN(VAR(1)));
  if (!column) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] column isn't found: <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    goto exit;
  }
  if (GRN_TEXT_LEN(VAR(2)) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[column][rename] new column name isn't specified: <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)));
    goto exit;
  }
  rc = grn_column_rename(ctx, column,
                         GRN_TEXT_VALUE(VAR(2)), GRN_TEXT_LEN(VAR(2)));
  if (rc != GRN_SUCCESS && ctx->rc == GRN_SUCCESS) {
    ERR(rc,
        "[column][rename] failed to rename: <%.*s.%.*s> -> <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(1)), GRN_TEXT_VALUE(VAR(1)),
        (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)),
        (int)GRN_TEXT_LEN(VAR(2)), GRN_TEXT_VALUE(VAR(2)));
  }
exit :
  GRN_OUTPUT_BOOL(!rc);
  if (column) { grn_obj_unlink(ctx, column); }
  if (table) { grn_obj_unlink(ctx, table); }
  return NULL;
}

#define GRN_STRLEN(s) ((s) ? strlen(s) : 0)

static void
output_column_name(grn_ctx *ctx, grn_obj *column)
{
  grn_obj bulk;
  int name_len;
  char name[GRN_TABLE_MAX_KEY_SIZE];

  GRN_TEXT_INIT(&bulk, GRN_OBJ_DO_SHALLOW_COPY);
  name_len = grn_column_name(ctx, column, name, GRN_TABLE_MAX_KEY_SIZE);
  GRN_TEXT_SET(ctx, &bulk, name, name_len);

  GRN_OUTPUT_OBJ(&bulk, NULL);
  GRN_OBJ_FIN(ctx, &bulk);
}

void
grn_proc_output_object_name(grn_ctx *ctx, grn_obj *obj)
{
  grn_obj bulk;
  int name_len;
  char name[GRN_TABLE_MAX_KEY_SIZE];

  if (obj) {
    GRN_TEXT_INIT(&bulk, GRN_OBJ_DO_SHALLOW_COPY);
    name_len = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
    GRN_TEXT_SET(ctx, &bulk, name, name_len);
  } else {
    GRN_VOID_INIT(&bulk);
  }

  GRN_OUTPUT_OBJ(&bulk, NULL);
  GRN_OBJ_FIN(ctx, &bulk);
}

void
grn_proc_output_object_id_name(grn_ctx *ctx, grn_id id)
{
  grn_obj *obj = NULL;

  if (id != GRN_ID_NIL) {
    obj = grn_ctx_at(ctx, id);
  }

  grn_proc_output_object_name(ctx, obj);
}

static int
output_column_info(grn_ctx *ctx, grn_obj *column)
{
  grn_obj o;
  grn_id id;
  const char *type;
  const char *path;

  switch (column->header.type) {
  case GRN_COLUMN_FIX_SIZE:
    type = "fix";
    break;
  case GRN_COLUMN_VAR_SIZE:
    type = "var";
    break;
  case GRN_COLUMN_INDEX:
    type = "index";
    break;
  default:
    GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid header type %d\n", column->header.type);
    return 0;
  }
  id = grn_obj_id(ctx, column);
  path = grn_obj_path(ctx, column);
  GRN_TEXT_INIT(&o, 0);
  GRN_OUTPUT_ARRAY_OPEN("COLUMN", 8);
  GRN_OUTPUT_INT64(id);
  output_column_name(ctx, column);
  GRN_OUTPUT_CSTR(path);
  GRN_OUTPUT_CSTR(type);
  grn_dump_column_create_flags(ctx, column->header.flags, &o);
  GRN_OUTPUT_OBJ(&o, NULL);
  grn_proc_output_object_id_name(ctx, column->header.domain);
  grn_proc_output_object_id_name(ctx, grn_obj_get_range(ctx, column));
  {
    grn_db_obj *obj = (grn_db_obj *)column;
    grn_id *s = obj->source;
    int i = 0, n = obj->source_size / sizeof(grn_id);
    GRN_OUTPUT_ARRAY_OPEN("SOURCES", n);
    for (i = 0; i < n; i++, s++) {
      grn_proc_output_object_id_name(ctx, *s);
    }
    GRN_OUTPUT_ARRAY_CLOSE();

  }
  //  output_obj_source(ctx, (grn_db_obj *)column);
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OBJ_FIN(ctx, &o);
  return 1;
}

static grn_obj *
proc_column_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table;
  if ((table = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)),
                           GRN_TEXT_LEN(VAR(0))))) {
    grn_hash *cols;
    grn_obj *col;
    int column_list_size = -1;
#ifdef GRN_WITH_MESSAGE_PACK
    column_list_size = 1; /* [header, (key), (COLUMNS)] */
    if ((col = grn_obj_column(ctx, table,
                              GRN_COLUMN_NAME_KEY,
                              GRN_COLUMN_NAME_KEY_LEN))) {
      column_list_size++;
      grn_obj_unlink(ctx, col);
    }
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      column_list_size += grn_table_columns(ctx, table, NULL, 0,
                                            (grn_obj *)cols);
      grn_hash_close(ctx, cols);
    }
#endif
    if ((cols = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY))) {
      GRN_OUTPUT_ARRAY_OPEN("COLUMN_LIST", column_list_size);
      GRN_OUTPUT_ARRAY_OPEN("HEADER", 8);
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("id");
      GRN_OUTPUT_CSTR("UInt32");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("name");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("path");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("type");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("flags");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("domain");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("range");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_OPEN("PROPERTY", 2);
      GRN_OUTPUT_CSTR("source");
      GRN_OUTPUT_CSTR("ShortText");
      GRN_OUTPUT_ARRAY_CLOSE();
      GRN_OUTPUT_ARRAY_CLOSE();
      if ((col = grn_obj_column(ctx, table,
                                GRN_COLUMN_NAME_KEY,
                                GRN_COLUMN_NAME_KEY_LEN))) {
        int name_len;
        char name_buf[GRN_TABLE_MAX_KEY_SIZE];
        grn_id id;
        grn_obj buf;
        GRN_TEXT_INIT(&buf, 0);
        GRN_OUTPUT_ARRAY_OPEN("COLUMN", 8);
        id = grn_obj_id(ctx, table);
        GRN_OUTPUT_INT64(id);
        GRN_OUTPUT_CSTR(GRN_COLUMN_NAME_KEY);
        GRN_OUTPUT_CSTR("");
        GRN_OUTPUT_CSTR("");
        grn_dump_column_create_flags(ctx, 0, &buf);
        GRN_OUTPUT_OBJ(&buf, NULL);
        name_len = grn_obj_name(ctx, table, name_buf, GRN_TABLE_MAX_KEY_SIZE);
        GRN_OUTPUT_STR(name_buf, name_len);
        grn_proc_output_object_id_name(ctx, table->header.domain);
        GRN_OUTPUT_ARRAY_OPEN("SOURCES", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
        GRN_OUTPUT_ARRAY_CLOSE();
        GRN_OBJ_FIN(ctx, &buf);
        grn_obj_unlink(ctx, col);
      }
      if (grn_table_columns(ctx, table, NULL, 0, (grn_obj *)cols) >= 0) {
        grn_id *key;
        GRN_HASH_EACH(ctx, cols, id, &key, NULL, NULL, {
          if ((col = grn_ctx_at(ctx, *key))) {
            output_column_info(ctx, col);
            grn_obj_unlink(ctx, col);
          }
        });
      }
      GRN_OUTPUT_ARRAY_CLOSE();
      grn_hash_close(ctx, cols);
    }
    grn_obj_unlink(ctx, table);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "table '%.*s' does not exist.",
        (int)GRN_TEXT_LEN(VAR(0)),
        GRN_TEXT_VALUE(VAR(0)));
  }
  return NULL;
}

static grn_obj *
proc_missing(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  uint32_t plen;
  grn_obj *outbuf = ctx->impl->output.buf;
  static int grn_document_root_len = -1;
  if (!grn_document_root) { return NULL; }
  if (grn_document_root_len < 0) {
    size_t l;
    if ((l = strlen(grn_document_root)) > PATH_MAX) {
      return NULL;
    }
    grn_document_root_len = (int)l;
    if (l > 0 && grn_document_root[l - 1] == '/') { grn_document_root_len--; }
  }
  if ((plen = GRN_TEXT_LEN(VAR(0))) + grn_document_root_len < PATH_MAX) {
    char path[PATH_MAX];
    grn_memcpy(path, grn_document_root, grn_document_root_len);
    path[grn_document_root_len] = '/';
    grn_str_url_path_normalize(ctx,
                               GRN_TEXT_VALUE(VAR(0)),
                               GRN_TEXT_LEN(VAR(0)),
                               path + grn_document_root_len + 1,
                               PATH_MAX - grn_document_root_len - 1);
    grn_bulk_put_from_file(ctx, outbuf, path);
  } else {
    uint32_t abbrlen = 32;
    ERR(GRN_INVALID_ARGUMENT,
        "too long path name: <%s/%.*s...> %u(%u)",
        grn_document_root,
        abbrlen < plen ? abbrlen : plen, GRN_TEXT_VALUE(VAR(0)),
        plen + grn_document_root_len, PATH_MAX);
  }
  return NULL;
}

static grn_obj *
proc_quit(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  ctx->stat = GRN_CTX_QUITTING;
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_shutdown(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  const char *mode;
  size_t mode_size;

  mode = grn_plugin_proc_get_var_string(ctx, user_data, "mode", -1, &mode_size);
#define MODE_EQUAL(name)                                                \
  (mode_size == strlen(name) && memcmp(mode, name, mode_size) == 0)
  if (mode_size == 0 || MODE_EQUAL("graceful")) {
    /* Do nothing. This is the default. */
  } else if (MODE_EQUAL("immediate")) {
    grn_request_canceler_cancel_all();
    if (ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
      ctx->rc = GRN_SUCCESS;
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT,
        "[shutdown] mode must be <graceful> or <immediate>: <%.*s>",
        (int)mode_size, mode);
  }
#undef MODE_EQUAL

  if (ctx->rc == GRN_SUCCESS) {
    grn_gctx.stat = GRN_CTX_QUIT;
    ctx->stat = GRN_CTX_QUITTING;
  }

  GRN_OUTPUT_BOOL(!ctx->rc);

  return NULL;
}

static grn_obj *
proc_defrag(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  int olen, threshold;
  olen = GRN_TEXT_LEN(VAR(0));

  if (olen) {
    obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), olen);
  } else {
    obj = ctx->impl->db;
  }

  threshold = GRN_TEXT_LEN(VAR(1))
    ? grn_atoi(GRN_TEXT_VALUE(VAR(1)), GRN_BULK_CURR(VAR(1)), NULL)
    : 0;

  if (obj) {
    grn_obj_defrag(ctx, obj, threshold);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "defrag object not found");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_log_level(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *level_name = VAR(0);
  if (GRN_TEXT_LEN(level_name) > 0) {
    grn_log_level max_level;
    GRN_TEXT_PUTC(ctx, level_name, '\0');
    if (grn_log_level_parse(GRN_TEXT_VALUE(level_name), &max_level)) {
      grn_logger_set_max_level(ctx, max_level);
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "invalid log level: <%s>", GRN_TEXT_VALUE(level_name));
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "log level is missing");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_log_put(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *level_name = VAR(0);
  grn_obj *message = VAR(1);
  if (GRN_TEXT_LEN(level_name) > 0) {
    grn_log_level level;
    GRN_TEXT_PUTC(ctx, level_name, '\0');
    if (grn_log_level_parse(GRN_TEXT_VALUE(level_name), &level)) {
      GRN_LOG(ctx, level, "%.*s",
              (int)GRN_TEXT_LEN(message),
              GRN_TEXT_VALUE(message));
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "invalid log level: <%s>", GRN_TEXT_VALUE(level_name));
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "log level is missing");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_log_reopen(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_log_reopen(ctx);
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_rc
proc_delete_validate_selector(grn_ctx *ctx, grn_obj *table, grn_obj *table_name,
                              grn_obj *key, grn_obj *id, grn_obj *filter)
{
  grn_rc rc = GRN_SUCCESS;

  if (!table) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] table doesn't exist: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return rc;
  }

  if (GRN_TEXT_LEN(key) == 0 &&
      GRN_TEXT_LEN(id) == 0 &&
      GRN_TEXT_LEN(filter) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] either key, id or filter must be specified: "
        "table: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "record selector must be one of key, id and filter: "
        "table: <%.*s>, key: <%.*s>, id: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both key and id: table: <%.*s>, key: <%.*s>, id: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id));
    return rc;
  }

  if (GRN_TEXT_LEN(key) && GRN_TEXT_LEN(id) == 0 && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both key and filter: "
        "table: <%.*s>, key: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(key), GRN_TEXT_VALUE(key),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  if (GRN_TEXT_LEN(key) == 0 && GRN_TEXT_LEN(id) && GRN_TEXT_LEN(filter)) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc,
        "[table][record][delete] "
        "can't use both id and filter: "
        "table: <%.*s>, id: <%.*s>, filter: <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
        (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter));
    return rc;
  }

  return rc;
}

static grn_obj *
proc_delete(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_INVALID_ARGUMENT;
  grn_obj *table_name = VAR(0);
  grn_obj *key = VAR(1);
  grn_obj *id = VAR(2);
  grn_obj *filter = VAR(3);
  grn_obj *table = NULL;

  if (GRN_TEXT_LEN(table_name) == 0) {
    rc = GRN_INVALID_ARGUMENT;
    ERR(rc, "[table][record][delete] table name isn't specified");
    goto exit;
  }

  table = grn_ctx_get(ctx,
                      GRN_TEXT_VALUE(table_name),
                      GRN_TEXT_LEN(table_name));
  rc = proc_delete_validate_selector(ctx, table, table_name, key, id, filter);
  if (rc != GRN_SUCCESS) { goto exit; }

  if (GRN_TEXT_LEN(key)) {
    grn_obj casted_key;
    if (key->header.domain != table->header.domain) {
      GRN_OBJ_INIT(&casted_key, GRN_BULK, 0, table->header.domain);
      grn_obj_cast(ctx, key, &casted_key, GRN_FALSE);
      key = &casted_key;
    }
    if (ctx->rc) {
      rc = ctx->rc;
    } else {
      rc = grn_table_delete(ctx, table, GRN_BULK_HEAD(key), GRN_BULK_VSIZE(key));
      if (key == &casted_key) {
        GRN_OBJ_FIN(ctx, &casted_key);
      }
    }
  } else if (GRN_TEXT_LEN(id)) {
    const char *end;
    grn_id parsed_id = grn_atoui(GRN_TEXT_VALUE(id), GRN_BULK_CURR(id), &end);
    if (end == GRN_BULK_CURR(id)) {
      rc = grn_table_delete_by_id(ctx, table, parsed_id);
    } else {
      rc = GRN_INVALID_ARGUMENT;
      ERR(rc,
          "[table][record][delete] id should be number: "
          "table: <%.*s>, id: <%.*s>, detail: <%.*s|%c|%.*s>",
          (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
          (int)GRN_TEXT_LEN(id), GRN_TEXT_VALUE(id),
          (int)(end - GRN_TEXT_VALUE(id)), GRN_TEXT_VALUE(id),
          end[0],
          (int)(GRN_TEXT_VALUE(id) - end - 1), end + 1);
    }
  } else if (GRN_TEXT_LEN(filter)) {
    grn_obj *cond, *v;

    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, cond, v);
    grn_expr_parse(ctx, cond,
                   GRN_TEXT_VALUE(filter),
                   GRN_TEXT_LEN(filter),
                   NULL, GRN_OP_MATCH, GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc) {
      char original_error_message[GRN_CTX_MSGSIZE];
      grn_strcpy(original_error_message, GRN_CTX_MSGSIZE, ctx->errbuf);
      rc = ctx->rc;
      ERR(rc,
          "[table][record][delete] failed to parse filter: "
          "table: <%.*s>, filter: <%.*s>, detail: <%s>",
          (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
          (int)GRN_TEXT_LEN(filter), GRN_TEXT_VALUE(filter),
          original_error_message);
    } else {
      grn_obj *records;

      records = grn_table_select(ctx, table, cond, NULL, GRN_OP_OR);
      if (records) {
        void *key = NULL;
        GRN_TABLE_EACH(ctx, records, GRN_ID_NIL, GRN_ID_NIL,
                       result_id, &key, NULL, NULL, {
          grn_id id = *(grn_id *)key;
          grn_table_delete_by_id(ctx, table, id);
          if (ctx->rc == GRN_OPERATION_NOT_PERMITTED) {
            ERRCLR(ctx);
          }
        });
        grn_obj_unlink(ctx, records);
      }
    }
    grn_obj_unlink(ctx, cond);
  }

exit :
  if (table) {
    grn_obj_unlink(ctx, table);
  }
  GRN_OUTPUT_BOOL(!rc);
  return NULL;
}

static const size_t DUMP_FLUSH_THRESHOLD_SIZE = 256 * 1024;

static void
dump_value(grn_ctx *ctx, grn_obj *outbuf, const char *value, int value_len)
{
  grn_obj escaped_value;
  GRN_TEXT_INIT(&escaped_value, 0);
  grn_text_esc(ctx, &escaped_value, value, value_len);
  /* is no character escaped? */
  /* TODO false positive with spaces inside values */
  if (GRN_TEXT_LEN(&escaped_value) == value_len + 2) {
    GRN_TEXT_PUT(ctx, outbuf, value, value_len);
  } else {
    GRN_TEXT_PUT(ctx, outbuf,
                 GRN_TEXT_VALUE(&escaped_value), GRN_TEXT_LEN(&escaped_value));
  }
  grn_obj_close(ctx, &escaped_value);
}

static void
dump_configs(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *config_cursor;

  config_cursor = grn_config_cursor_open(ctx);
  if (!config_cursor)
    return;

  while (grn_config_cursor_next(ctx, config_cursor)) {
    const char *key;
    uint32_t key_size;
    const char *value;
    uint32_t value_size;

    key_size = grn_config_cursor_get_key(ctx, config_cursor, &key);
    value_size = grn_config_cursor_get_value(ctx, config_cursor, &value);

    GRN_TEXT_PUTS(ctx, outbuf, "config_set ");
    dump_value(ctx, outbuf, key, key_size);
    GRN_TEXT_PUTS(ctx, outbuf, " ");
    dump_value(ctx, outbuf, value, value_size);
    GRN_TEXT_PUTC(ctx, outbuf, '\n');
  }
  grn_obj_close(ctx, config_cursor);
}

static void
dump_plugins(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj plugin_names;
  unsigned int i, n;

  GRN_TEXT_INIT(&plugin_names, GRN_OBJ_VECTOR);

  grn_plugin_get_names(ctx, &plugin_names);

  n = grn_vector_size(ctx, &plugin_names);
  if (n == 0) {
    GRN_OBJ_FIN(ctx, &plugin_names);
    return;
  }

  if (GRN_TEXT_LEN(outbuf) > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, '\n');
    grn_ctx_output_flush(ctx, 0);
  }
  for (i = 0; i < n; i++) {
    const char *name;
    unsigned int name_size;

    name_size = grn_vector_get_element(ctx, &plugin_names, i, &name, NULL, NULL);
    grn_text_printf(ctx, outbuf, "plugin_register %.*s\n",
                    (int)name_size, name);
  }

  GRN_OBJ_FIN(ctx, &plugin_names);
}

static void
dump_obj_name(grn_ctx *ctx, grn_obj *outbuf, grn_obj *obj)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_obj_name(ctx, obj, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_value(ctx, outbuf, name, name_len);
}

static void
dump_column_name(grn_ctx *ctx, grn_obj *outbuf, grn_obj *column)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;
  name_len = grn_column_name(ctx, column, name, GRN_TABLE_MAX_KEY_SIZE);
  dump_value(ctx, outbuf, name, name_len);
}

static void
dump_index_column_sources(grn_ctx *ctx, grn_obj *outbuf, grn_obj *column)
{
  grn_obj sources;
  grn_id *source_ids;
  int i, n;

  GRN_OBJ_INIT(&sources, GRN_BULK, 0, GRN_ID_NIL);
  grn_obj_get_info(ctx, column, GRN_INFO_SOURCE, &sources);

  n = GRN_BULK_VSIZE(&sources) / sizeof(grn_id);
  source_ids = (grn_id *)GRN_BULK_HEAD(&sources);
  if (n > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, ' ');
  }
  for (i = 0; i < n; i++) {
    grn_obj *source;
    if ((source = grn_ctx_at(ctx, *source_ids))) {
      if (i) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
      switch (source->header.type) {
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_HASH_KEY:
        GRN_TEXT_PUT(ctx, outbuf, GRN_COLUMN_NAME_KEY, GRN_COLUMN_NAME_KEY_LEN);
        break;
      default:
        dump_column_name(ctx, outbuf, source);
        break;
      }
    }
    source_ids++;
  }
  grn_obj_close(ctx, &sources);
}

static void
dump_column(grn_ctx *ctx, grn_obj *outbuf , grn_obj *table, grn_obj *column)
{
  grn_obj *type;
  grn_obj_flags default_flags = GRN_OBJ_PERSISTENT;

  type = grn_ctx_at(ctx, ((grn_db_obj *)column)->range);
  if (!type) {
    // ERR(GRN_RANGE_ERROR, "couldn't get column's type object");
    return;
  }

  GRN_TEXT_PUTS(ctx, outbuf, "column_create ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  dump_column_name(ctx, outbuf, column);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  if (type->header.type == GRN_TYPE) {
    default_flags |= type->header.flags;
  }
  grn_dump_column_create_flags(ctx,
                               column->header.flags & ~default_flags,
                               outbuf);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  dump_obj_name(ctx, outbuf, type);
  if (column->header.flags & GRN_OBJ_COLUMN_INDEX) {
    dump_index_column_sources(ctx, outbuf, column);
  }
  GRN_TEXT_PUTC(ctx, outbuf, '\n');

  grn_obj_unlink(ctx, type);
}

static void
dump_columns(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table,
             grn_obj *pending_reference_columns)
{
  grn_hash *columns;
  columns = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                            GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
  if (!columns) {
    ERR(GRN_NO_MEMORY_AVAILABLE, "couldn't create a hash to hold columns");
    return;
  }

  if (grn_table_columns(ctx, table, NULL, 0, (grn_obj *)columns) >= 0) {
    grn_id *key;

    GRN_HASH_EACH(ctx, columns, id, &key, NULL, NULL, {
      grn_obj *column;
      if ((column = grn_ctx_at(ctx, *key))) {
        if (GRN_OBJ_INDEX_COLUMNP(column)) {
          /* do nothing */
        } else if (grn_obj_is_reference_column(ctx, column)) {
          GRN_PTR_PUT(ctx, pending_reference_columns, column);
        } else {
          dump_column(ctx, outbuf, table, column);
          grn_obj_unlink(ctx, column);
        }
      }
    });
  }
  grn_hash_close(ctx, columns);
}

static void
dump_record_column_vector(grn_ctx *ctx, grn_obj *outbuf, grn_id id,
                          grn_obj *column, grn_id range_id, grn_obj *buf)
{
  grn_obj *range;

  range = grn_ctx_at(ctx, range_id);
  if (GRN_OBJ_TABLEP(range) ||
      (range->header.flags & GRN_OBJ_KEY_VAR_SIZE) == 0) {
    GRN_OBJ_INIT(buf, GRN_UVECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, outbuf, buf, NULL);
  } else {
    grn_obj_format *format_argument = NULL;
    grn_obj_format format;
    if (column->header.flags & GRN_OBJ_WITH_WEIGHT) {
      format.flags = GRN_OBJ_FORMAT_WITH_WEIGHT;
      format_argument = &format;
    }
    GRN_OBJ_INIT(buf, GRN_VECTOR, 0, range_id);
    grn_obj_get_value(ctx, column, id, buf);
    grn_text_otoj(ctx, outbuf, buf, format_argument);
  }
  grn_obj_unlink(ctx, range);
  grn_obj_unlink(ctx, buf);
}

static void
dump_records(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table)
{
  grn_obj **columns;
  grn_id old_id = 0, id;
  grn_table_cursor *cursor;
  int i, ncolumns, n_use_columns;
  grn_obj columnbuf, delete_commands, use_columns, column_name;
  grn_bool have_index_column = GRN_FALSE;
  grn_bool have_data_column = GRN_FALSE;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
  case GRN_TABLE_NO_KEY:
    break;
  default:
    return;
  }

  if (grn_table_size(ctx, table) == 0) {
    return;
  }

  GRN_PTR_INIT(&columnbuf, GRN_OBJ_VECTOR, GRN_ID_NIL);
  grn_obj_columns(ctx, table, DUMP_COLUMNS, strlen(DUMP_COLUMNS), &columnbuf);
  columns = (grn_obj **)GRN_BULK_HEAD(&columnbuf);
  ncolumns = GRN_BULK_VSIZE(&columnbuf)/sizeof(grn_obj *);

  GRN_PTR_INIT(&use_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  GRN_TEXT_INIT(&column_name, 0);
  for (i = 0; i < ncolumns; i++) {
    if (GRN_OBJ_INDEX_COLUMNP(columns[i])) {
      have_index_column = GRN_TRUE;
      continue;
    }

    if (columns[i]->header.type != GRN_ACCESSOR) {
      have_data_column = GRN_TRUE;
    }

    GRN_BULK_REWIND(&column_name);
    grn_column_name_(ctx, columns[i], &column_name);
    if (table->header.type != GRN_TABLE_NO_KEY &&
        GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_ID_LEN &&
        memcmp(GRN_TEXT_VALUE(&column_name),
               GRN_COLUMN_NAME_ID,
               GRN_COLUMN_NAME_ID_LEN) == 0) {
      continue;
    }

    if (table->header.type == GRN_TABLE_NO_KEY &&
        GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_KEY_LEN &&
        memcmp(GRN_TEXT_VALUE(&column_name),
               GRN_COLUMN_NAME_KEY,
               GRN_COLUMN_NAME_KEY_LEN) == 0) {
      continue;
    }

    GRN_PTR_PUT(ctx, &use_columns, columns[i]);
  }

  if (have_index_column && !have_data_column) {
    goto exit;
  }

  if (GRN_TEXT_LEN(outbuf) > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, '\n');
  }

  GRN_TEXT_PUTS(ctx, outbuf, "load --table ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTS(ctx, outbuf, "\n[\n");

  n_use_columns = GRN_BULK_VSIZE(&use_columns) / sizeof(grn_obj *);
  GRN_TEXT_PUTC(ctx, outbuf, '[');
  for (i = 0; i < n_use_columns; i++) {
    grn_obj *column;
    column = *((grn_obj **)GRN_BULK_HEAD(&use_columns) + i);
    if (i) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
    GRN_BULK_REWIND(&column_name);
    grn_column_name_(ctx, column, &column_name);
    grn_text_otoj(ctx, outbuf, &column_name, NULL);
  }
  GRN_TEXT_PUTS(ctx, outbuf, "],\n");

  GRN_TEXT_INIT(&delete_commands, 0);
  cursor = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_BY_KEY);
  for (i = 0; (id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL;
       ++i, old_id = id) {
    int is_value_column;
    int j;
    grn_obj buf;
    if (i) { GRN_TEXT_PUTS(ctx, outbuf, ",\n"); }
    if (table->header.type == GRN_TABLE_NO_KEY && old_id + 1 < id) {
      grn_id current_id;
      for (current_id = old_id + 1; current_id < id; current_id++) {
        GRN_TEXT_PUTS(ctx, outbuf, "[],\n");
        GRN_TEXT_PUTS(ctx, &delete_commands, "delete --table ");
        dump_obj_name(ctx, &delete_commands, table);
        GRN_TEXT_PUTS(ctx, &delete_commands, " --id ");
        grn_text_lltoa(ctx, &delete_commands, current_id);
        GRN_TEXT_PUTC(ctx, &delete_commands, '\n');
      }
    }
    GRN_TEXT_PUTC(ctx, outbuf, '[');
    for (j = 0; j < n_use_columns; j++) {
      grn_id range;
      grn_obj *column;
      column = *((grn_obj **)GRN_BULK_HEAD(&use_columns) + j);
      GRN_BULK_REWIND(&column_name);
      grn_column_name_(ctx, column, &column_name);
      if (GRN_TEXT_LEN(&column_name) == GRN_COLUMN_NAME_VALUE_LEN &&
          !memcmp(GRN_TEXT_VALUE(&column_name),
                  GRN_COLUMN_NAME_VALUE,
                  GRN_COLUMN_NAME_VALUE_LEN)) {
        is_value_column = 1;
      } else {
        is_value_column = 0;
      }
      range = grn_obj_get_range(ctx, column);

      if (j) { GRN_TEXT_PUTC(ctx, outbuf, ','); }
      switch (column->header.type) {
      case GRN_COLUMN_VAR_SIZE:
      case GRN_COLUMN_FIX_SIZE:
        switch (column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) {
        case GRN_OBJ_COLUMN_VECTOR:
          dump_record_column_vector(ctx, outbuf, id, column, range, &buf);
          break;
        case GRN_OBJ_COLUMN_SCALAR:
          {
            GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
            grn_obj_get_value(ctx, column, id, &buf);
            grn_text_otoj(ctx, outbuf, &buf, NULL);
            grn_obj_unlink(ctx, &buf);
          }
          break;
        default:
          ERR(GRN_OPERATION_NOT_SUPPORTED,
              "unsupported column type: %#x",
              column->header.type);
          break;
        }
        break;
      case GRN_COLUMN_INDEX:
        break;
      case GRN_ACCESSOR:
        {
          GRN_OBJ_INIT(&buf, GRN_BULK, 0, range);
          grn_obj_get_value(ctx, column, id, &buf);
          /* XXX maybe, grn_obj_get_range() should not unconditionally return
             GRN_DB_INT32 when column is GRN_ACCESSOR and
             GRN_ACCESSOR_GET_VALUE */
          if (is_value_column) {
            buf.header.domain = ((grn_db_obj *)table)->range;
          }
          grn_text_otoj(ctx, outbuf, &buf, NULL);
          grn_obj_unlink(ctx, &buf);
        }
        break;
      default:
        ERR(GRN_OPERATION_NOT_SUPPORTED,
            "unsupported header type %#x",
            column->header.type);
        break;
      }
    }
    GRN_TEXT_PUTC(ctx, outbuf, ']');
    if (GRN_TEXT_LEN(outbuf) >= DUMP_FLUSH_THRESHOLD_SIZE) {
      grn_ctx_output_flush(ctx, 0);
    }
  }
  grn_table_cursor_close(ctx, cursor);
  GRN_TEXT_PUTS(ctx, outbuf, "\n]\n");
  GRN_TEXT_PUT(ctx, outbuf, GRN_TEXT_VALUE(&delete_commands),
                            GRN_TEXT_LEN(&delete_commands));
  grn_obj_unlink(ctx, &delete_commands);

exit :
  grn_obj_unlink(ctx, &column_name);
  grn_obj_unlink(ctx, &use_columns);

  for (i = 0; i < ncolumns; i++) {
    grn_obj_unlink(ctx, columns[i]);
  }
  grn_obj_unlink(ctx, &columnbuf);
}

static void
dump_table(grn_ctx *ctx, grn_obj *outbuf, grn_obj *table,
           grn_obj *pending_reference_columns)
{
  grn_obj *domain = NULL, *range = NULL;
  grn_obj_flags default_flags = GRN_OBJ_PERSISTENT;
  grn_obj *default_tokenizer;
  grn_obj *normalizer;

  switch (table->header.type) {
  case GRN_TABLE_HASH_KEY:
  case GRN_TABLE_PAT_KEY:
  case GRN_TABLE_DAT_KEY:
    domain = grn_ctx_at(ctx, table->header.domain);
    break;
  default:
    break;
  }

  if (GRN_TEXT_LEN(outbuf) > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, '\n');
    grn_ctx_output_flush(ctx, 0);
  }

  GRN_TEXT_PUTS(ctx, outbuf, "table_create ");
  dump_obj_name(ctx, outbuf, table);
  GRN_TEXT_PUTC(ctx, outbuf, ' ');
  grn_dump_table_create_flags(ctx,
                              table->header.flags & ~default_flags,
                              outbuf);
  if (domain) {
    GRN_TEXT_PUTC(ctx, outbuf, ' ');
    dump_obj_name(ctx, outbuf, domain);
  }
  if (((grn_db_obj *)table)->range != GRN_ID_NIL) {
    range = grn_ctx_at(ctx, ((grn_db_obj *)table)->range);
    if (!range) {
      // ERR(GRN_RANGE_ERROR, "couldn't get table's value_type object");
      return;
    }
    if (table->header.type != GRN_TABLE_NO_KEY) {
      GRN_TEXT_PUTC(ctx, outbuf, ' ');
    } else {
      GRN_TEXT_PUTS(ctx, outbuf, " --value_type ");
    }
    dump_obj_name(ctx, outbuf, range);
    grn_obj_unlink(ctx, range);
  }
  default_tokenizer = grn_obj_get_info(ctx, table, GRN_INFO_DEFAULT_TOKENIZER,
                                       NULL);
  if (default_tokenizer) {
    GRN_TEXT_PUTS(ctx, outbuf, " --default_tokenizer ");
    dump_obj_name(ctx, outbuf, default_tokenizer);
  }
  normalizer = grn_obj_get_info(ctx, table, GRN_INFO_NORMALIZER, NULL);
  if (normalizer) {
    GRN_TEXT_PUTS(ctx, outbuf, " --normalizer ");
    dump_obj_name(ctx, outbuf, normalizer);
  }
  if (table->header.type != GRN_TABLE_NO_KEY) {
    grn_obj token_filters;
    int n_token_filters;

    GRN_PTR_INIT(&token_filters, GRN_OBJ_VECTOR, GRN_ID_NIL);
    grn_obj_get_info(ctx, table, GRN_INFO_TOKEN_FILTERS, &token_filters);
    n_token_filters = GRN_BULK_VSIZE(&token_filters) / sizeof(grn_obj *);
    if (n_token_filters > 0) {
      int i;
      GRN_TEXT_PUTS(ctx, outbuf, " --token_filters ");
      for (i = 0; i < n_token_filters; i++) {
        grn_obj *token_filter = GRN_PTR_VALUE_AT(&token_filters, i);
        if (i > 0) {
          GRN_TEXT_PUTC(ctx, outbuf, ',');
        }
        dump_obj_name(ctx, outbuf, token_filter);
      }
    }
    GRN_OBJ_FIN(ctx, &token_filters);
  }

  GRN_TEXT_PUTC(ctx, outbuf, '\n');

  if (domain) {
    grn_obj_unlink(ctx, domain);
  }

  dump_columns(ctx, outbuf, table, pending_reference_columns);
}

static void
dump_pending_columns(grn_ctx *ctx, grn_obj *outbuf, grn_obj *pending_columns)
{
  size_t i, n_columns;

  n_columns = GRN_BULK_VSIZE(pending_columns) / sizeof(grn_obj *);
  if (n_columns == 0) {
    return;
  }

  if (GRN_TEXT_LEN(outbuf) > 0) {
    GRN_TEXT_PUTC(ctx, outbuf, '\n');
    grn_ctx_output_flush(ctx, 0);
  }

  for (i = 0; i < n_columns; i++) {
    grn_obj *table, *column;

    column = GRN_PTR_VALUE_AT(pending_columns, i);
    table = grn_ctx_at(ctx, column->header.domain);
    dump_column(ctx, outbuf, table, column);
    grn_obj_unlink(ctx, column);
    grn_obj_unlink(ctx, table);
  }
}

static void
dump_schema(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *db = ctx->impl->db;
  grn_table_cursor *cur;
  grn_id id;
  grn_obj pending_reference_columns;

  cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                              GRN_CURSOR_BY_ID);
  if (!cur) {
    return;
  }

  GRN_PTR_INIT(&pending_reference_columns, GRN_OBJ_VECTOR, GRN_ID_NIL);
  while ((id = grn_table_cursor_next(ctx, cur)) != GRN_ID_NIL) {
    grn_obj *object;

    if ((object = grn_ctx_at(ctx, id))) {
      switch (object->header.type) {
      case GRN_TABLE_HASH_KEY:
      case GRN_TABLE_PAT_KEY:
      case GRN_TABLE_DAT_KEY:
      case GRN_TABLE_NO_KEY:
        dump_table(ctx, outbuf, object, &pending_reference_columns);
        break;
      default:
        break;
      }
      grn_obj_unlink(ctx, object);
    } else {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      ERRCLR(ctx);
    }
  }
  grn_table_cursor_close(ctx, cur);

  dump_pending_columns(ctx, outbuf, &pending_reference_columns);
  grn_obj_close(ctx, &pending_reference_columns);
}

static void
dump_selected_tables_records(grn_ctx *ctx, grn_obj *outbuf, grn_obj *tables)
{
  const char *p, *e;

  p = GRN_TEXT_VALUE(tables);
  e = p + GRN_TEXT_LEN(tables);
  while (p < e) {
    int len;
    grn_obj *table;
    const char *token, *token_e;

    if ((len = grn_isspace(p, ctx->encoding))) {
      p += len;
      continue;
    }

    token = p;
    if (!(('a' <= *p && *p <= 'z') ||
          ('A' <= *p && *p <= 'Z') ||
          (*p == '_'))) {
      while (p < e && !grn_isspace(p, ctx->encoding)) {
        p++;
      }
      GRN_LOG(ctx, GRN_LOG_WARNING, "invalid table name is ignored: <%.*s>\n",
              (int)(p - token), token);
      continue;
    }
    while (p < e &&
           (('a' <= *p && *p <= 'z') ||
            ('A' <= *p && *p <= 'Z') ||
            ('0' <= *p && *p <= '9') ||
            (*p == '_'))) {
      p++;
    }
    token_e = p;
    while (p < e && (len = grn_isspace(p, ctx->encoding))) {
      p += len;
      continue;
    }
    if (p < e && *p == ',') {
      p++;
    }

    if ((table = grn_ctx_get(ctx, token, token_e - token))) {
      dump_records(ctx, outbuf, table);
      grn_obj_unlink(ctx, table);
    } else {
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "nonexistent table name is ignored: <%.*s>\n",
              (int)(token_e - token), token);
    }
  }
}

static void
dump_all_records(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *db = ctx->impl->db;
  grn_table_cursor *cur;
  if ((cur = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                   GRN_CURSOR_BY_ID))) {
    grn_id id;

    while ((id = grn_table_cursor_next(ctx, cur)) != GRN_ID_NIL) {
      grn_obj *table;

      if ((table = grn_ctx_at(ctx, id))) {
        dump_records(ctx, outbuf, table);
        grn_obj_unlink(ctx, table);
      } else {
        /* XXX: this clause is executed when MeCab tokenizer is enabled in
           database but the groonga isn't supported MeCab.
           We should return error mesage about it and error exit status
           but it's too difficult for this architecture. :< */
        ERRCLR(ctx);
      }
    }
    grn_table_cursor_close(ctx, cur);
  }
}

static void
dump_indexes(grn_ctx *ctx, grn_obj *outbuf)
{
  grn_obj *db = ctx->impl->db;
  grn_table_cursor *cursor;
  grn_id id;
  grn_bool is_first_index_column = GRN_TRUE;

  cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_BY_ID);
  if (!cursor) {
    return;
  }

  while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    grn_obj *object;

    object = grn_ctx_at(ctx, id);
    if (!object) {
      /* XXX: this clause is executed when MeCab tokenizer is enabled in
         database but the groonga isn't supported MeCab.
         We should return error mesage about it and error exit status
         but it's too difficult for this architecture. :< */
      ERRCLR(ctx);
      continue;
    }

    if (object->header.type == GRN_COLUMN_INDEX) {
      grn_obj *table;
      grn_obj *column = object;

      if (is_first_index_column && GRN_TEXT_LEN(outbuf) > 0) {
        GRN_TEXT_PUTC(ctx, outbuf, '\n');
      }
      is_first_index_column = GRN_FALSE;

      table = grn_ctx_at(ctx, column->header.domain);
      dump_column(ctx, outbuf, table, column);
      grn_obj_unlink(ctx, table);
    }
    grn_obj_unlink(ctx, object);
  }
  grn_table_cursor_close(ctx, cursor);
}

grn_bool
grn_proc_option_value_bool(grn_ctx *ctx,
                           grn_obj *option,
                           grn_bool default_value)
{
  const char *value;
  size_t value_length;

  value = GRN_TEXT_VALUE(option);
  value_length = GRN_TEXT_LEN(option);

  if (value_length == 0) {
    return default_value;
  }

  if (value_length == strlen("yes") &&
      strncmp(value, "yes", value_length) == 0) {
    return GRN_TRUE;
  } else if (value_length == strlen("no") &&
             strncmp(value, "no", value_length) == 0) {
    return GRN_FALSE;
  } else {
    return default_value;
  }
}

int32_t
grn_proc_option_value_int32(grn_ctx *ctx,
                            grn_obj *option,
                            int32_t default_value)
{
  const char *value;
  size_t value_length;
  int32_t int32_value;
  const char *rest;

  value = GRN_TEXT_VALUE(option);
  value_length = GRN_TEXT_LEN(option);

  if (value_length == 0) {
    return default_value;
  }

  int32_value = grn_atoi(value, value + value_length, &rest);
  if (rest == value + value_length) {
    return int32_value;
  } else {
    return default_value;
  }
}

const char *
grn_proc_option_value_string(grn_ctx *ctx,
                             grn_obj *option,
                             size_t *size)
{
  const char *value;
  size_t value_length;

  value = GRN_TEXT_VALUE(option);
  value_length = GRN_TEXT_LEN(option);

  if (size) {
    *size = value_length;
  }

  if (value_length == 0) {
    return NULL;
  } else {
    return value;
  }
}

static grn_obj *
proc_dump(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *outbuf = ctx->impl->output.buf;
  grn_obj *tables = VAR(0);
  grn_obj *dump_plugins_raw = VAR(1);
  grn_obj *dump_schema_raw = VAR(2);
  grn_obj *dump_records_raw = VAR(3);
  grn_obj *dump_indexes_raw = VAR(4);
  grn_obj *dump_configs_raw = VAR(5);
  grn_bool is_dump_plugins;
  grn_bool is_dump_schema;
  grn_bool is_dump_records;
  grn_bool is_dump_indexes;
  grn_bool is_dump_configs;

  grn_ctx_set_output_type(ctx, GRN_CONTENT_GROONGA_COMMAND_LIST);

  is_dump_plugins = grn_proc_option_value_bool(ctx, dump_plugins_raw, GRN_TRUE);
  is_dump_schema = grn_proc_option_value_bool(ctx, dump_schema_raw, GRN_TRUE);
  is_dump_records = grn_proc_option_value_bool(ctx, dump_records_raw, GRN_TRUE);
  is_dump_indexes = grn_proc_option_value_bool(ctx, dump_indexes_raw, GRN_TRUE);
  is_dump_configs = grn_proc_option_value_bool(ctx, dump_configs_raw, GRN_TRUE);

  if (is_dump_configs) {
    dump_configs(ctx, outbuf);
  }
  if (is_dump_plugins) {
    dump_plugins(ctx, outbuf);
  }
  if (is_dump_schema) {
    dump_schema(ctx, outbuf);
  }
  if (is_dump_records) {
    /* To update index columns correctly, we first create the whole schema, then
       load non-derivative records, while skipping records of index columns. That
       way, groonga will silently do the job of updating index columns for us. */
    if (GRN_TEXT_LEN(tables) > 0) {
      dump_selected_tables_records(ctx, outbuf, tables);
    } else {
      dump_all_records(ctx, outbuf);
    }
  }
  if (is_dump_indexes) {
    dump_indexes(ctx, outbuf);
  }

  /* remove the last newline because another one will be added by the caller.
     maybe, the caller of proc functions currently doesn't consider the
     possibility of multiple-line output from proc functions. */
  if (GRN_BULK_VSIZE(outbuf) > 0) {
    grn_bulk_truncate(ctx, outbuf, GRN_BULK_VSIZE(outbuf) - 1);
  }
  return NULL;
}

static grn_obj *
proc_cache_limit(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_cache *cache;
  unsigned int current_max_n_entries;

  cache = grn_cache_current_get(ctx);
  current_max_n_entries = grn_cache_get_max_n_entries(ctx, cache);
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *rest;
    uint32_t max = grn_atoui(GRN_TEXT_VALUE(VAR(0)),
                             GRN_BULK_CURR(VAR(0)), &rest);
    if (GRN_BULK_CURR(VAR(0)) == rest) {
      grn_cache_set_max_n_entries(ctx, cache, max);
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "max value is invalid unsigned integer format: <%.*s>",
          (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    }
  }
  if (ctx->rc == GRN_SUCCESS) {
    GRN_OUTPUT_INT64(current_max_n_entries);
  }
  return NULL;
}

static grn_obj *
proc_register(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *name;
    GRN_TEXT_PUTC(ctx, VAR(0), '\0');
    name = GRN_TEXT_VALUE(VAR(0));
    grn_plugin_register(ctx, name);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "path is required");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

void grn_ii_buffer_check(grn_ctx *ctx, grn_ii *ii, uint32_t seg);

static grn_obj *
proc_check(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj = grn_ctx_get(ctx, GRN_TEXT_VALUE(VAR(0)), GRN_TEXT_LEN(VAR(0)));
  if (!obj) {
    ERR(GRN_INVALID_ARGUMENT,
        "no such object: <%.*s>", (int)GRN_TEXT_LEN(VAR(0)), GRN_TEXT_VALUE(VAR(0)));
    GRN_OUTPUT_BOOL(!ctx->rc);
  } else {
    switch (obj->header.type) {
    case GRN_DB :
      GRN_OUTPUT_BOOL(!ctx->rc);
      break;
    case GRN_TABLE_PAT_KEY :
      grn_pat_check(ctx, (grn_pat *)obj);
      break;
    case GRN_TABLE_HASH_KEY :
      grn_hash_check(ctx, (grn_hash *)obj);
      break;
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
    case GRN_COLUMN_FIX_SIZE :
      GRN_OUTPUT_BOOL(!ctx->rc);
      break;
    case GRN_COLUMN_VAR_SIZE :
      grn_ja_check(ctx, (grn_ja *)obj);
      break;
    case GRN_COLUMN_INDEX :
      {
        grn_ii *ii = (grn_ii *)obj;
        struct grn_ii_header *h = ii->header;
        char buf[8];
        GRN_OUTPUT_ARRAY_OPEN("RESULT", 8);
        {
          uint32_t i, j, g =0, a = 0, b = 0;
          uint32_t max = 0;
          for (i = h->bgqtail; i != h->bgqhead; i = ((i + 1) & (GRN_II_BGQSIZE - 1))) {
            j = h->bgqbody[i];
            g++;
            if (j > max) { max = j; }
          }
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            j = h->binfo[i];
            if (j < 0x20000) {
              if (j > max) { max = j; }
              b++;
            }
          }
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            j = h->ainfo[i];
            if (j < 0x20000) {
              if (j > max) { max = j; }
              a++;
            }
          }
          GRN_OUTPUT_MAP_OPEN("SUMMARY", 12);
          GRN_OUTPUT_CSTR("flags");
          grn_itoh(h->flags, buf, 8);
          GRN_OUTPUT_STR(buf, 8);
          GRN_OUTPUT_CSTR("max sid");
          GRN_OUTPUT_INT64(h->smax);
          GRN_OUTPUT_CSTR("number of garbage segments");
          GRN_OUTPUT_INT64(g);
          GRN_OUTPUT_CSTR("number of array segments");
          GRN_OUTPUT_INT64(a);
          GRN_OUTPUT_CSTR("max id of array segment");
          GRN_OUTPUT_INT64(h->amax);
          GRN_OUTPUT_CSTR("number of buffer segments");
          GRN_OUTPUT_INT64(b);
          GRN_OUTPUT_CSTR("max id of buffer segment");
          GRN_OUTPUT_INT64(h->bmax);
          GRN_OUTPUT_CSTR("max id of physical segment in use");
          GRN_OUTPUT_INT64(max);
          GRN_OUTPUT_CSTR("number of unmanaged segments");
          GRN_OUTPUT_INT64(h->pnext - a - b - g);
          GRN_OUTPUT_CSTR("total chunk size");
          GRN_OUTPUT_INT64(h->total_chunk_size);
          for (max = 0, i = 0; i < (GRN_II_MAX_CHUNK >> 3); i++) {
            if ((j = h->chunks[i])) {
              int k;
              for (k = 0; k < 8; k++) {
                if ((j & (1 << k))) { max = (i << 3) + j; }
              }
            }
          }
          GRN_OUTPUT_CSTR("max id of chunk segments in use");
          GRN_OUTPUT_INT64(max);
          GRN_OUTPUT_CSTR("number of garbage chunk");
          GRN_OUTPUT_ARRAY_OPEN("NGARBAGES", GRN_II_N_CHUNK_VARIATION);
          for (i = 0; i <= GRN_II_N_CHUNK_VARIATION; i++) {
            GRN_OUTPUT_INT64(h->ngarbages[i]);
          }
          GRN_OUTPUT_ARRAY_CLOSE();
          GRN_OUTPUT_MAP_CLOSE();
          for (i = 0; i < GRN_II_MAX_LSEG; i++) {
            if (h->binfo[i] < 0x20000) { grn_ii_buffer_check(ctx, ii, i); }
          }
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      }
      break;
    }
  }
  return NULL;
}

static grn_obj *
proc_truncate(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  const char *target_name;
  int target_name_len;

  target_name_len = GRN_TEXT_LEN(VAR(0));
  if (target_name_len > 0) {
    target_name = GRN_TEXT_VALUE(VAR(0));
  } else {
    target_name_len = GRN_TEXT_LEN(VAR(1));
    if (target_name_len == 0) {
      ERR(GRN_INVALID_ARGUMENT, "[truncate] table name is missing");
      goto exit;
    }
    target_name = GRN_TEXT_VALUE(VAR(1));
  }

  {
    grn_obj *target = grn_ctx_get(ctx, target_name, target_name_len);
    if (!target) {
      ERR(GRN_INVALID_ARGUMENT,
          "[truncate] no such target: <%.*s>", target_name_len, target_name);
      goto exit;
    }

    switch (target->header.type) {
    case GRN_TABLE_HASH_KEY :
    case GRN_TABLE_PAT_KEY :
    case GRN_TABLE_DAT_KEY :
    case GRN_TABLE_NO_KEY :
      grn_table_truncate(ctx, target);
      break;
    case GRN_COLUMN_FIX_SIZE :
    case GRN_COLUMN_VAR_SIZE :
    case GRN_COLUMN_INDEX :
      grn_column_truncate(ctx, target);
      break;
    default:
      {
        grn_obj buffer;
        GRN_TEXT_INIT(&buffer, 0);
        grn_inspect(ctx, &buffer, target);
        ERR(GRN_INVALID_ARGUMENT,
            "[truncate] not a table nor column object: <%.*s>",
            (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
        GRN_OBJ_FIN(ctx, &buffer);
      }
      break;
    }
  }

exit :
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static int
parse_normalize_flags(grn_ctx *ctx, grn_obj *flag_names)
{
  int flags = 0;
  const char *names, *names_end;
  int length;

  names = GRN_TEXT_VALUE(flag_names);
  length = GRN_TEXT_LEN(flag_names);
  names_end = names + length;
  while (names < names_end) {
    if (*names == '|' || *names == ' ') {
      names += 1;
      continue;
    }

#define CHECK_FLAG(name)\
    if (((names_end - names) >= (sizeof(#name) - 1)) &&\
        (!memcmp(names, #name, sizeof(#name) - 1))) {\
      flags |= GRN_STRING_ ## name;\
      names += sizeof(#name) - 1;\
      continue;\
    }

    CHECK_FLAG(REMOVE_BLANK);
    CHECK_FLAG(WITH_TYPES);
    CHECK_FLAG(WITH_CHECKS);
    CHECK_FLAG(REMOVE_TOKENIZED_DELIMITER);

#define GRN_STRING_NONE 0
    CHECK_FLAG(NONE);
#undef GRN_STRING_NONE

    ERR(GRN_INVALID_ARGUMENT, "[normalize] invalid flag: <%.*s>",
        (int)(names_end - names), names);
    return 0;
#undef CHECK_FLAG
  }

  return flags;
}

static grn_bool
is_normalizer(grn_ctx *ctx, grn_obj *object)
{
  if (object->header.type != GRN_PROC) {
    return GRN_FALSE;
  }

  if (grn_proc_get_type(ctx, object) != GRN_PROC_NORMALIZER) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static grn_bool
is_tokenizer(grn_ctx *ctx, grn_obj *object)
{
  if (object->header.type != GRN_PROC) {
    return GRN_FALSE;
  }

  if (grn_proc_get_type(ctx, object) != GRN_PROC_TOKENIZER) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static const char *
char_type_name(grn_char_type type)
{
  const char *name = "unknown";

  switch (type) {
  case GRN_CHAR_NULL :
    name = "null";
    break;
  case GRN_CHAR_ALPHA :
    name = "alpha";
    break;
  case GRN_CHAR_DIGIT :
    name = "digit";
    break;
  case GRN_CHAR_SYMBOL :
    name = "symbol";
    break;
  case GRN_CHAR_HIRAGANA :
    name = "hiragana";
    break;
  case GRN_CHAR_KATAKANA :
    name = "katakana";
    break;
  case GRN_CHAR_KANJI :
    name = "kanji";
    break;
  case GRN_CHAR_OTHERS :
    name = "others";
    break;
  }

  return name;
}

static grn_obj *
proc_normalize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *normalizer_name;
  grn_obj *string;
  grn_obj *flag_names;

  normalizer_name = VAR(0);
  string = VAR(1);
  flag_names = VAR(2);
  if (GRN_TEXT_LEN(normalizer_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "normalizer name is missing");
    GRN_OUTPUT_CSTR("");
    return NULL;
  }

  {
    grn_obj *normalizer;
    grn_obj *grn_string;
    int flags;
    unsigned int normalized_length_in_bytes;
    unsigned int normalized_n_characters;

    flags = parse_normalize_flags(ctx, flag_names);
    normalizer = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(normalizer_name),
                             GRN_TEXT_LEN(normalizer_name));
    if (!normalizer) {
      ERR(GRN_INVALID_ARGUMENT,
          "[normalize] nonexistent normalizer: <%.*s>",
          (int)GRN_TEXT_LEN(normalizer_name),
          GRN_TEXT_VALUE(normalizer_name));
      GRN_OUTPUT_CSTR("");
      return NULL;
    }

    if (!is_normalizer(ctx, normalizer)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, normalizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[normalize] not normalizer: %.*s",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      GRN_OUTPUT_CSTR("");
      grn_obj_unlink(ctx, normalizer);
      return NULL;
    }

    grn_string = grn_string_open(ctx,
                                 GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                                 normalizer, flags);
    grn_obj_unlink(ctx, normalizer);

    GRN_OUTPUT_MAP_OPEN("RESULT", 3);
    {
      const char *normalized;

      grn_string_get_normalized(ctx, grn_string,
                                &normalized,
                                &normalized_length_in_bytes,
                                &normalized_n_characters);
      GRN_OUTPUT_CSTR("normalized");
      GRN_OUTPUT_STR(normalized, normalized_length_in_bytes);
    }
    {
      const unsigned char *types;

      types = grn_string_get_types(ctx, grn_string);
      GRN_OUTPUT_CSTR("types");
      if (types) {
        unsigned int i;
        GRN_OUTPUT_ARRAY_OPEN("types", normalized_n_characters);
        for (i = 0; i < normalized_n_characters; i++) {
          GRN_OUTPUT_CSTR(char_type_name(types[i]));
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      } else {
        GRN_OUTPUT_ARRAY_OPEN("types", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
      }
    }
    {
      const short *checks;

      checks = grn_string_get_checks(ctx, grn_string);
      GRN_OUTPUT_CSTR("checks");
      if (checks) {
        unsigned int i;
        GRN_OUTPUT_ARRAY_OPEN("checks", normalized_length_in_bytes);
        for (i = 0; i < normalized_length_in_bytes; i++) {
          GRN_OUTPUT_INT32(checks[i]);
        }
        GRN_OUTPUT_ARRAY_CLOSE();
      } else {
        GRN_OUTPUT_ARRAY_OPEN("checks", 0);
        GRN_OUTPUT_ARRAY_CLOSE();
      }
    }
    GRN_OUTPUT_MAP_CLOSE();

    grn_obj_unlink(ctx, grn_string);
  }

  return NULL;
}

static unsigned int
parse_tokenize_flags(grn_ctx *ctx, grn_obj *flag_names)
{
  unsigned int flags = 0;
  const char *names, *names_end;
  int length;

  names = GRN_TEXT_VALUE(flag_names);
  length = GRN_TEXT_LEN(flag_names);
  names_end = names + length;
  while (names < names_end) {
    if (*names == '|' || *names == ' ') {
      names += 1;
      continue;
    }

#define CHECK_FLAG(name)\
    if (((names_end - names) >= (sizeof(#name) - 1)) &&\
        (!memcmp(names, #name, sizeof(#name) - 1))) {\
      flags |= GRN_TOKEN_CURSOR_ ## name;\
      names += sizeof(#name) - 1;\
      continue;\
    }

    CHECK_FLAG(ENABLE_TOKENIZED_DELIMITER);

#define GRN_TOKEN_CURSOR_NONE 0
    CHECK_FLAG(NONE);
#undef GRN_TOKEN_CURSOR_NONE

    ERR(GRN_INVALID_ARGUMENT, "[tokenize] invalid flag: <%.*s>",
        (int)(names_end - names), names);
    return 0;
#undef CHECK_FLAG
  }

  return flags;
}

typedef struct {
  grn_id id;
  int32_t position;
  grn_bool force_prefix;
} tokenize_token;

static void
output_tokens(grn_ctx *ctx, grn_obj *tokens, grn_obj *lexicon)
{
  int i, n_tokens;

  n_tokens = GRN_BULK_VSIZE(tokens) / sizeof(tokenize_token);
  GRN_OUTPUT_ARRAY_OPEN("TOKENS", n_tokens);
  for (i = 0; i < n_tokens; i++) {
    tokenize_token *token;
    char value[GRN_TABLE_MAX_KEY_SIZE];
    unsigned int value_size;

    token = ((tokenize_token *)(GRN_BULK_HEAD(tokens))) + i;

    GRN_OUTPUT_MAP_OPEN("TOKEN", 3);

    GRN_OUTPUT_CSTR("value");
    value_size = grn_table_get_key(ctx, lexicon, token->id,
                                   value, GRN_TABLE_MAX_KEY_SIZE);
    GRN_OUTPUT_STR(value, value_size);

    GRN_OUTPUT_CSTR("position");
    GRN_OUTPUT_INT32(token->position);

    GRN_OUTPUT_CSTR("force_prefix");
    GRN_OUTPUT_BOOL(token->force_prefix);

    GRN_OUTPUT_MAP_CLOSE();
  }
  GRN_OUTPUT_ARRAY_CLOSE();
}

static grn_obj *
create_lexicon_for_tokenize(grn_ctx *ctx,
                            grn_obj *tokenizer_name,
                            grn_obj *normalizer_name,
                            grn_obj *token_filter_names)
{
  grn_obj *lexicon;
  grn_obj *tokenizer;
  grn_obj *normalizer = NULL;

  tokenizer = grn_ctx_get(ctx,
                          GRN_TEXT_VALUE(tokenizer_name),
                          GRN_TEXT_LEN(tokenizer_name));
  if (!tokenizer) {
    ERR(GRN_INVALID_ARGUMENT,
        "[tokenize] nonexistent tokenizer: <%.*s>",
        (int)GRN_TEXT_LEN(tokenizer_name),
        GRN_TEXT_VALUE(tokenizer_name));
    return NULL;
  }

  if (!is_tokenizer(ctx, tokenizer)) {
    grn_obj inspected;
    GRN_TEXT_INIT(&inspected, 0);
    grn_inspect(ctx, &inspected, tokenizer);
    ERR(GRN_INVALID_ARGUMENT,
        "[tokenize] not tokenizer: %.*s",
        (int)GRN_TEXT_LEN(&inspected),
        GRN_TEXT_VALUE(&inspected));
    GRN_OBJ_FIN(ctx, &inspected);
    grn_obj_unlink(ctx, tokenizer);
    return NULL;
  }

  if (GRN_TEXT_LEN(normalizer_name) > 0) {
    normalizer = grn_ctx_get(ctx,
                             GRN_TEXT_VALUE(normalizer_name),
                             GRN_TEXT_LEN(normalizer_name));
    if (!normalizer) {
      grn_obj_unlink(ctx, tokenizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[tokenize] nonexistent normalizer: <%.*s>",
          (int)GRN_TEXT_LEN(normalizer_name),
          GRN_TEXT_VALUE(normalizer_name));
      return NULL;
    }

    if (!is_normalizer(ctx, normalizer)) {
      grn_obj inspected;
      grn_obj_unlink(ctx, tokenizer);
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, normalizer);
      ERR(GRN_INVALID_ARGUMENT,
          "[tokenize] not normalizer: %.*s",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      grn_obj_unlink(ctx, normalizer);
      return NULL;
    }
  }

  lexicon = grn_table_create(ctx, NULL, 0,
                             NULL,
                             GRN_OBJ_TABLE_HASH_KEY,
                             grn_ctx_at(ctx, GRN_DB_SHORT_TEXT),
                             NULL);
  grn_obj_set_info(ctx, lexicon,
                   GRN_INFO_DEFAULT_TOKENIZER, tokenizer);
  grn_obj_unlink(ctx, tokenizer);
  if (normalizer) {
    grn_obj_set_info(ctx, lexicon,
                     GRN_INFO_NORMALIZER, normalizer);
    grn_obj_unlink(ctx, normalizer);
  }
  grn_proc_table_set_token_filters(ctx, lexicon, token_filter_names);

  return lexicon;
}

static void
tokenize(grn_ctx *ctx, grn_obj *lexicon, grn_obj *string, grn_tokenize_mode mode,
         unsigned int flags, grn_obj *tokens)
{
  grn_token_cursor *token_cursor;

  token_cursor =
    grn_token_cursor_open(ctx, lexicon,
                          GRN_TEXT_VALUE(string), GRN_TEXT_LEN(string),
                          mode, flags);
  if (!token_cursor) {
    return;
  }

  while (token_cursor->status == GRN_TOKEN_CURSOR_DOING) {
    grn_id token_id = grn_token_cursor_next(ctx, token_cursor);
    tokenize_token *current_token;
    if (token_id == GRN_ID_NIL) {
      continue;
    }
    grn_bulk_space(ctx, tokens, sizeof(tokenize_token));
    current_token = ((tokenize_token *)(GRN_BULK_CURR(tokens))) - 1;
    current_token->id = token_id;
    current_token->position = token_cursor->pos;
    current_token->force_prefix = token_cursor->force_prefix;
  }
  grn_token_cursor_close(ctx, token_cursor);
}

static grn_obj *
proc_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *tokenizer_name;
  grn_obj *string;
  grn_obj *normalizer_name;
  grn_obj *flag_names;
  grn_obj *mode_name;
  grn_obj *token_filter_names;

  tokenizer_name = VAR(0);
  string = VAR(1);
  normalizer_name = VAR(2);
  flag_names = VAR(3);
  mode_name = VAR(4);
  token_filter_names = VAR(5);

  if (GRN_TEXT_LEN(tokenizer_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[tokenize] tokenizer name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = create_lexicon_for_tokenize(ctx,
                                          tokenizer_name,
                                          normalizer_name,
                                          token_filter_names);
    if (!lexicon) {
      return NULL;
    }

#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
      if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("ADD")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon);
      } else if (MODE_NAME_EQUAL("GET")) {
        tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
        GRN_BULK_REWIND(&tokens);
        tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
        output_tokens(ctx, &tokens, lexicon);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "[tokenize] invalid mode: <%.*s>",
            (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
      }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

    grn_obj_unlink(ctx, lexicon);
  }

  return NULL;
}

static grn_obj *
proc_table_tokenize(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *table_name;
  grn_obj *string;
  grn_obj *flag_names;
  grn_obj *mode_name;

  table_name = VAR(0);
  string = VAR(1);
  flag_names = VAR(2);
  mode_name = VAR(3);

  if (GRN_TEXT_LEN(table_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] table name is missing");
    return NULL;
  }

  if (GRN_TEXT_LEN(string) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] string is missing");
    return NULL;
  }

  {
    unsigned int flags;
    grn_obj *lexicon;

    flags = parse_tokenize_flags(ctx, flag_names);
    if (ctx->rc != GRN_SUCCESS) {
      return NULL;
    }

    lexicon = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));

    if (!lexicon) {
      return NULL;
    }

#define MODE_NAME_EQUAL(name)\
    (GRN_TEXT_LEN(mode_name) == strlen(name) &&\
     memcmp(GRN_TEXT_VALUE(mode_name), name, strlen(name)) == 0)

    {
      grn_obj tokens;
      GRN_VALUE_FIX_SIZE_INIT(&tokens, GRN_OBJ_VECTOR, GRN_ID_NIL);
    if (GRN_TEXT_LEN(mode_name) == 0 || MODE_NAME_EQUAL("GET")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_GET, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon);
    } else if (MODE_NAME_EQUAL("ADD")) {
      tokenize(ctx, lexicon, string, GRN_TOKEN_ADD, flags, &tokens);
      output_tokens(ctx, &tokens, lexicon);
    } else {
      ERR(GRN_INVALID_ARGUMENT, "[table_tokenize] invalid mode: <%.*s>",
          (int)GRN_TEXT_LEN(mode_name), GRN_TEXT_VALUE(mode_name));
    }
      GRN_OBJ_FIN(ctx, &tokens);
    }
#undef MODE_NAME_EQUAL

    grn_obj_unlink(ctx, lexicon);
  }

  return NULL;
}

static void
list_proc(grn_ctx *ctx, grn_proc_type target_proc_type,
          const char *name, const char *plural_name)
{
  grn_obj *db;
  grn_table_cursor *cursor;
  grn_obj target_procs;

  db = grn_ctx_db(ctx);
  cursor = grn_table_cursor_open(ctx, db, NULL, 0, NULL, 0, 0, -1,
                                 GRN_CURSOR_BY_ID);
  if (!cursor) {
    return;
  }

  GRN_PTR_INIT(&target_procs, GRN_OBJ_VECTOR, GRN_ID_NIL);
  {
    grn_id id;

    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_obj *obj;
      grn_proc_type proc_type;

      obj = grn_ctx_at(ctx, id);
      if (!obj) {
        continue;
      }

      if (obj->header.type != GRN_PROC) {
        grn_obj_unlink(ctx, obj);
        continue;
      }

      proc_type = grn_proc_get_type(ctx, obj);
      if (proc_type != target_proc_type) {
        grn_obj_unlink(ctx, obj);
        continue;
      }

      GRN_PTR_PUT(ctx, &target_procs, obj);
    }
    grn_table_cursor_close(ctx, cursor);

    {
      int i, n_procs;

      n_procs = GRN_BULK_VSIZE(&target_procs) / sizeof(grn_obj *);
      GRN_OUTPUT_ARRAY_OPEN(plural_name, n_procs);
      for (i = 0; i < n_procs; i++) {
        grn_obj *proc;
        char name[GRN_TABLE_MAX_KEY_SIZE];
        int name_size;

        proc = GRN_PTR_VALUE_AT(&target_procs, i);
        name_size = grn_obj_name(ctx, proc, name, GRN_TABLE_MAX_KEY_SIZE);
        GRN_OUTPUT_MAP_OPEN(name, 1);
        GRN_OUTPUT_CSTR("name");
        GRN_OUTPUT_STR(name, name_size);
        GRN_OUTPUT_MAP_CLOSE();

        grn_obj_unlink(ctx, proc);
      }
      GRN_OUTPUT_ARRAY_CLOSE();
    }

    grn_obj_unlink(ctx, &target_procs);
  }
}

static grn_obj *
proc_tokenizer_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  list_proc(ctx, GRN_PROC_TOKENIZER, "tokenizer", "tokenizers");
  return NULL;
}

static grn_obj *
proc_normalizer_list(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  list_proc(ctx, GRN_PROC_NORMALIZER, "normalizer", "normalizers");
  return NULL;
}

static grn_obj *
func_rand(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  int val;
  grn_obj *obj;
  if (nargs > 0) {
    int max = GRN_INT32_VALUE(args[0]);
    val = (int) (1.0 * max * rand() / (RAND_MAX + 1.0));
  } else {
    val = rand();
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_INT32, 0))) {
    GRN_INT32_SET(ctx, obj, val);
  }
  return obj;
}

static grn_obj *
func_now(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  if ((obj = GRN_PROC_ALLOC(GRN_DB_TIME, 0))) {
    GRN_TIME_NOW(ctx, obj);
  }
  return obj;
}

static inline grn_bool
is_comparable_number_type(grn_id type)
{
  return GRN_DB_INT8 <= type && type <= GRN_DB_TIME;
}

static inline grn_id
larger_number_type(grn_id type1, grn_id type2)
{
  if (type1 == type2) {
    return type1;
  }

  switch (type1) {
  case GRN_DB_FLOAT :
    return type1;
  case GRN_DB_TIME :
    if (type2 == GRN_DB_FLOAT) {
      return type2;
    } else {
      return type1;
    }
  default :
    if (type2 > type1) {
      return type2;
    } else {
      return type1;
    }
  }
}

static inline grn_id
smaller_number_type(grn_id type1, grn_id type2)
{
  if (type1 == type2) {
    return type1;
  }

  switch (type1) {
  case GRN_DB_FLOAT :
    return type1;
  case GRN_DB_TIME :
    if (type2 == GRN_DB_FLOAT) {
      return type2;
    } else {
      return type1;
    }
  default :
    {
      grn_id smaller_number_type;
      if (type2 > type1) {
        smaller_number_type = type2;
      } else {
        smaller_number_type = type1;
      }
      switch (smaller_number_type) {
      case GRN_DB_UINT8 :
        return GRN_DB_INT8;
      case GRN_DB_UINT16 :
        return GRN_DB_INT16;
      case GRN_DB_UINT32 :
        return GRN_DB_INT32;
      case GRN_DB_UINT64 :
        return GRN_DB_INT64;
      default :
        return smaller_number_type;
      }
    }
  }
}

static inline grn_bool
is_negative_value(grn_obj *number)
{
  switch (number->header.domain) {
  case GRN_DB_INT8 :
    return GRN_INT8_VALUE(number) < 0;
  case GRN_DB_INT16 :
    return GRN_INT16_VALUE(number) < 0;
  case GRN_DB_INT32 :
    return GRN_INT32_VALUE(number) < 0;
  case GRN_DB_INT64 :
    return GRN_INT64_VALUE(number) < 0;
  case GRN_DB_TIME :
    return GRN_TIME_VALUE(number) < 0;
  case GRN_DB_FLOAT :
    return GRN_FLOAT_VALUE(number) < 0;
  default :
    return GRN_FALSE;
  }
}

static inline grn_bool
number_safe_cast(grn_ctx *ctx, grn_obj *src, grn_obj *dest, grn_id type)
{
  grn_obj_reinit(ctx, dest, type, 0);
  if (src->header.domain == type) {
    GRN_TEXT_SET(ctx, dest, GRN_TEXT_VALUE(src), GRN_TEXT_LEN(src));
    return GRN_TRUE;
  }

  switch (type) {
  case GRN_DB_UINT8 :
    if (is_negative_value(src)) {
      GRN_UINT8_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT16 :
    if (is_negative_value(src)) {
      GRN_UINT16_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT32 :
    if (is_negative_value(src)) {
      GRN_UINT32_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  case GRN_DB_UINT64 :
    if (is_negative_value(src)) {
      GRN_UINT64_SET(ctx, dest, 0);
      return GRN_TRUE;
    }
  default :
    return grn_obj_cast(ctx, src, dest, GRN_FALSE) == GRN_SUCCESS;
  }
}

static inline int
compare_number(grn_ctx *ctx, grn_obj *number1, grn_obj *number2, grn_id type)
{
#define COMPARE_AND_RETURN(type, value1, value2)\
  {\
    type computed_value1 = value1;\
    type computed_value2 = value2;\
    if (computed_value1 > computed_value2) {\
      return 1;\
    } else if (computed_value1 < computed_value2) {\
      return -1;\
    } else {\
      return 0;\
    }\
  }

  switch (type) {
  case GRN_DB_INT8 :
    COMPARE_AND_RETURN(int8_t,
                       GRN_INT8_VALUE(number1),
                       GRN_INT8_VALUE(number2));
  case GRN_DB_UINT8 :
    COMPARE_AND_RETURN(uint8_t,
                       GRN_UINT8_VALUE(number1),
                       GRN_UINT8_VALUE(number2));
  case GRN_DB_INT16 :
    COMPARE_AND_RETURN(int16_t,
                       GRN_INT16_VALUE(number1),
                       GRN_INT16_VALUE(number2));
  case GRN_DB_UINT16 :
    COMPARE_AND_RETURN(uint16_t,
                       GRN_UINT16_VALUE(number1),
                       GRN_UINT16_VALUE(number2));
  case GRN_DB_INT32 :
    COMPARE_AND_RETURN(int32_t,
                       GRN_INT32_VALUE(number1),
                       GRN_INT32_VALUE(number2));
  case GRN_DB_UINT32 :
    COMPARE_AND_RETURN(uint32_t,
                       GRN_UINT32_VALUE(number1),
                       GRN_UINT32_VALUE(number2));
  case GRN_DB_INT64 :
    COMPARE_AND_RETURN(int64_t,
                       GRN_INT64_VALUE(number1),
                       GRN_INT64_VALUE(number2));
  case GRN_DB_UINT64 :
    COMPARE_AND_RETURN(uint64_t,
                       GRN_UINT64_VALUE(number1),
                       GRN_UINT64_VALUE(number2));
  case GRN_DB_FLOAT :
    COMPARE_AND_RETURN(double,
                       GRN_FLOAT_VALUE(number1),
                       GRN_FLOAT_VALUE(number2));
  case GRN_DB_TIME :
    COMPARE_AND_RETURN(int64_t,
                       GRN_TIME_VALUE(number1),
                       GRN_TIME_VALUE(number2));
  default :
    return 0;
  }

#undef COMPARE_AND_RETURN
}

static grn_obj *
func_max(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *max;
  grn_id cast_type = GRN_DB_INT8;
  grn_obj casted_max, casted_number;
  int i;

  max = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  if (!max) {
    return max;
  }

  GRN_VOID_INIT(&casted_max);
  GRN_VOID_INIT(&casted_number);
  for (i = 0; i < nargs; i++) {
    grn_obj *number = args[i];
    grn_id domain = number->header.domain;
    if (!is_comparable_number_type(domain)) {
      continue;
    }
    cast_type = larger_number_type(cast_type, domain);
    if (!number_safe_cast(ctx, number, &casted_number, cast_type)) {
      continue;
    }
    if (max->header.domain == GRN_DB_VOID) {
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
      continue;
    }

    if (max->header.domain != cast_type) {
      if (!number_safe_cast(ctx, max, &casted_max, cast_type)) {
        continue;
      }
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_max),
                   GRN_TEXT_LEN(&casted_max));
    }
    if (compare_number(ctx, &casted_number, max, cast_type) > 0) {
      grn_obj_reinit(ctx, max, cast_type, 0);
      GRN_TEXT_SET(ctx, max,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
    }
  }
  GRN_OBJ_FIN(ctx, &casted_max);
  GRN_OBJ_FIN(ctx, &casted_number);

  return max;
}

static grn_obj *
func_min(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *min;
  grn_id cast_type = GRN_DB_INT8;
  grn_obj casted_min, casted_number;
  int i;

  min = GRN_PROC_ALLOC(GRN_DB_VOID, 0);
  if (!min) {
    return min;
  }

  GRN_VOID_INIT(&casted_min);
  GRN_VOID_INIT(&casted_number);
  for (i = 0; i < nargs; i++) {
    grn_obj *number = args[i];
    grn_id domain = number->header.domain;
    if (!is_comparable_number_type(domain)) {
      continue;
    }
    cast_type = smaller_number_type(cast_type, domain);
    if (!number_safe_cast(ctx, number, &casted_number, cast_type)) {
      continue;
    }
    if (min->header.domain == GRN_DB_VOID) {
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
      continue;
    }

    if (min->header.domain != cast_type) {
      if (!number_safe_cast(ctx, min, &casted_min, cast_type)) {
        continue;
      }
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_min),
                   GRN_TEXT_LEN(&casted_min));
    }
    if (compare_number(ctx, &casted_number, min, cast_type) < 0) {
      grn_obj_reinit(ctx, min, cast_type, 0);
      GRN_TEXT_SET(ctx, min,
                   GRN_TEXT_VALUE(&casted_number),
                   GRN_TEXT_LEN(&casted_number));
    }
  }
  GRN_OBJ_FIN(ctx, &casted_min);
  GRN_OBJ_FIN(ctx, &casted_number);

  return min;
}

static grn_obj *
func_geo_in_circle(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  unsigned char r = GRN_FALSE;
  grn_geo_approximate_type type = GRN_GEO_APPROXIMATE_RECTANGLE;
  switch (nargs) {
  case 4 :
    if (grn_geo_resolve_approximate_type(ctx, args[3], &type) != GRN_SUCCESS) {
      break;
    }
    /* fallthru */
  case 3 :
    r = grn_geo_in_circle(ctx, args[0], args[1], args[2], type);
    break;
  default :
    break;
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, r);
  }
  return obj;
}

static grn_obj *
func_geo_in_rectangle(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  unsigned char r = GRN_FALSE;
  if (nargs == 3) {
    r = grn_geo_in_rectangle(ctx, args[0], args[1], args[2]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_UINT32, 0))) {
    GRN_UINT32_SET(ctx, obj, r);
  }
  return obj;
}

static grn_obj *
func_geo_distance(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0.0;
  grn_geo_approximate_type type = GRN_GEO_APPROXIMATE_RECTANGLE;
  switch (nargs) {
  case 3 :
    if (grn_geo_resolve_approximate_type(ctx, args[2], &type) != GRN_SUCCESS) {
      break;
    }
    /* fallthru */
  case 2 :
    d = grn_geo_distance(ctx, args[0], args[1], type);
    break;
  default:
    break;
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

/* deprecated. */
static grn_obj *
func_geo_distance2(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0;
  if (nargs == 2) {
    d = grn_geo_distance_sphere(ctx, args[0], args[1]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

/* deprecated. */
static grn_obj *
func_geo_distance3(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *obj;
  double d = 0;
  if (nargs == 2) {
    d = grn_geo_distance_ellipsoid(ctx, args[0], args[1]);
  }
  if ((obj = GRN_PROC_ALLOC(GRN_DB_FLOAT, 0))) {
    GRN_FLOAT_SET(ctx, obj, d);
  }
  return obj;
}

static grn_obj *
func_all_records(grn_ctx *ctx, int nargs, grn_obj **args,
                 grn_user_data *user_data)
{
  grn_obj *true_value;
  if ((true_value = GRN_PROC_ALLOC(GRN_DB_BOOL, 0))) {
    GRN_BOOL_SET(ctx, true_value, GRN_TRUE);
  }
  return true_value;
}

static grn_rc
selector_all_records(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                     int nargs, grn_obj **args,
                     grn_obj *res, grn_operator op)
{
  grn_posting posting;

  memset(&posting, 0, sizeof(grn_posting));
  GRN_TABLE_EACH(ctx, table, 0, 0, id, NULL, NULL, NULL, {
    posting.rid = id;
    grn_ii_posting_add(ctx, &posting, (grn_hash *)res, GRN_OP_OR);
  });

  return ctx->rc;
}

typedef struct {
  grn_obj *found;
  grn_obj *table;
  grn_obj *records;
} selector_to_function_data;

static grn_bool
selector_to_function_data_init(grn_ctx *ctx,
                               selector_to_function_data *data,
                               grn_user_data *user_data)
{
  grn_obj *condition = NULL;
  grn_obj *variable;

  data->table = NULL;
  data->records = NULL;

  data->found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!data->found) {
    return GRN_FALSE;
  }
  GRN_BOOL_SET(ctx, data->found, GRN_FALSE);

  grn_proc_get_info(ctx, user_data, NULL, NULL, &condition);
  if (!condition) {
    return GRN_FALSE;
  }

  variable = grn_expr_get_var_by_offset(ctx, condition, 0);
  if (!variable) {
    return GRN_FALSE;
  }

  data->table = grn_ctx_at(ctx, variable->header.domain);
  if (!data->table) {
    return GRN_FALSE;
  }

  data->records = grn_table_create(ctx, NULL, 0, NULL,
                                   GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                   data->table, NULL);
  if (!data->records) {
    return GRN_FALSE;
  }

  {
    grn_rset_posinfo pi;
    unsigned int key_size;
    memset(&pi, 0, sizeof(grn_rset_posinfo));
    pi.rid = GRN_RECORD_VALUE(variable);
    key_size = ((grn_hash *)(data->records))->key_size;
    if (grn_table_add(ctx, data->records, &pi, key_size, NULL) == GRN_ID_NIL) {
      return GRN_FALSE;
    }
  }

  return GRN_TRUE;
}

static void
selector_to_function_data_selected(grn_ctx *ctx,
                                   selector_to_function_data *data)
{
  GRN_BOOL_SET(ctx, data->found, grn_table_size(ctx, data->records) > 0);
}

static void
selector_to_function_data_fin(grn_ctx *ctx,
                              selector_to_function_data *data)
{
  if (data->records) {
    grn_obj_unlink(ctx, data->records);
  }
}

static grn_rc
run_query(grn_ctx *ctx, grn_obj *table,
          int nargs, grn_obj **args,
          grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *match_columns_string;
  grn_obj *query;
  grn_obj *query_expander_name = NULL;
  grn_obj *match_columns = NULL;
  grn_obj *condition = NULL;
  grn_obj *dummy_variable;

  /* TODO: support flags by parameters */
  if (!(2 <= nargs && nargs <= 3)) {
    ERR(GRN_INVALID_ARGUMENT,
        "wrong number of arguments (%d for 2..3)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  match_columns_string = args[0];
  query = args[1];
  if (nargs > 2) {
    query_expander_name = args[2];
  }

  if (match_columns_string->header.domain == GRN_DB_TEXT &&
      GRN_TEXT_LEN(match_columns_string) > 0) {
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, match_columns, dummy_variable);
    if (!match_columns) {
      rc = ctx->rc;
      goto exit;
    }

    grn_expr_parse(ctx, match_columns,
                   GRN_TEXT_VALUE(match_columns_string),
                   GRN_TEXT_LEN(match_columns_string),
                   NULL, GRN_OP_MATCH, GRN_OP_AND,
                   GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      rc = ctx->rc;
      goto exit;
    }
  }

  if (query->header.domain == GRN_DB_TEXT && GRN_TEXT_LEN(query) > 0) {
    const char *query_string;
    unsigned int query_string_len;
    grn_obj expanded_query;
    grn_expr_flags flags =
      GRN_EXPR_SYNTAX_QUERY|GRN_EXPR_ALLOW_PRAGMA|GRN_EXPR_ALLOW_COLUMN;

    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, condition, dummy_variable);
    if (!condition) {
      rc = ctx->rc;
      goto exit;
    }

    query_string = GRN_TEXT_VALUE(query);
    query_string_len = GRN_TEXT_LEN(query);

    GRN_TEXT_INIT(&expanded_query, 0);
    if (query_expander_name &&
        query_expander_name->header.domain == GRN_DB_TEXT &&
        GRN_TEXT_LEN(query_expander_name) > 0) {
      rc = grn_proc_syntax_expand_query(ctx,
                                        query_string, query_string_len,
                                        flags,
                                        GRN_TEXT_VALUE(query_expander_name),
                                        GRN_TEXT_LEN(query_expander_name),
                                        &expanded_query);
      if (rc != GRN_SUCCESS) {
        GRN_OBJ_FIN(ctx, &expanded_query);
        goto exit;
      }
      query_string = GRN_TEXT_VALUE(&expanded_query);
      query_string_len = GRN_TEXT_LEN(&expanded_query);
    }
    grn_expr_parse(ctx, condition,
                   query_string,
                   query_string_len,
                   match_columns, GRN_OP_MATCH, GRN_OP_AND, flags);
    rc = ctx->rc;
    GRN_OBJ_FIN(ctx, &expanded_query);
    if (rc != GRN_SUCCESS) {
      goto exit;
    }
    grn_table_select(ctx, table, condition, res, op);
    rc = ctx->rc;
  }

exit :
  if (match_columns) {
    grn_obj_unlink(ctx, match_columns);
  }
  if (condition) {
    grn_obj_unlink(ctx, condition);
  }

  return rc;
}

static grn_obj *
func_query(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  selector_to_function_data data;

  if (selector_to_function_data_init(ctx, &data, user_data)) {
    grn_rc rc;
    rc = run_query(ctx, data.table, nargs, args, data.records, GRN_OP_AND);
    if (rc == GRN_SUCCESS) {
      selector_to_function_data_selected(ctx, &data);
    }
  }
  selector_to_function_data_fin(ctx, &data);

  return data.found;
}

static grn_rc
selector_query(grn_ctx *ctx, grn_obj *table, grn_obj *index,
               int nargs, grn_obj **args,
               grn_obj *res, grn_operator op)
{
  return run_query(ctx, table, nargs - 1, args + 1, res, op);
}

static grn_rc
run_sub_filter(grn_ctx *ctx, grn_obj *table,
               int nargs, grn_obj **args,
               grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *scope;
  grn_obj *sub_filter_string;
  grn_obj *scope_domain = NULL;
  grn_obj *sub_filter = NULL;
  grn_obj *dummy_variable = NULL;

  if (nargs != 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): wrong number of arguments (%d for 2)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  scope = args[0];
  sub_filter_string = args[1];

  switch (scope->header.type) {
  case GRN_ACCESSOR :
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_INDEX :
    break;
  default :
    /* TODO: put inspected the 1st argument to message */
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 1st argument must be column or accessor");
    rc = ctx->rc;
    goto exit;
    break;
  }

  scope_domain = grn_ctx_at(ctx, grn_obj_get_range(ctx, scope));

  if (sub_filter_string->header.domain != GRN_DB_TEXT) {
    /* TODO: put inspected the 2nd argument to message */
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 2nd argument must be String");
    rc = ctx->rc;
    goto exit;
  }
  if (GRN_TEXT_LEN(sub_filter_string) == 0) {
    ERR(GRN_INVALID_ARGUMENT,
        "sub_filter(): the 2nd argument must not be empty String");
    rc = ctx->rc;
    goto exit;
  }

  GRN_EXPR_CREATE_FOR_QUERY(ctx, scope_domain, sub_filter, dummy_variable);
  if (!sub_filter) {
    rc = ctx->rc;
    goto exit;
  }

  grn_expr_parse(ctx, sub_filter,
                 GRN_TEXT_VALUE(sub_filter_string),
                 GRN_TEXT_LEN(sub_filter_string),
                 NULL, GRN_OP_MATCH, GRN_OP_AND,
                 GRN_EXPR_SYNTAX_SCRIPT);
  if (ctx->rc != GRN_SUCCESS) {
    rc = ctx->rc;
    goto exit;
  }

  {
    grn_obj *base_res = NULL;
    grn_obj *resolve_res = NULL;

    base_res = grn_table_create(ctx, NULL, 0, NULL,
                                GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                scope_domain, NULL);
    grn_table_select(ctx, scope_domain, sub_filter, base_res, GRN_OP_OR);
    if (scope->header.type == GRN_ACCESSOR) {
      rc = grn_accessor_resolve(ctx, scope, -1, base_res, &resolve_res, NULL);
    } else {
      grn_accessor accessor;
      accessor.header.type = GRN_ACCESSOR;
      accessor.obj = scope;
      accessor.action = GRN_ACCESSOR_GET_COLUMN_VALUE;
      accessor.next = NULL;
      rc = grn_accessor_resolve(ctx, (grn_obj *)&accessor, -1, base_res,
                                &resolve_res, NULL);
    }
    if (resolve_res) {
      rc = grn_table_setoperation(ctx, res, resolve_res, res, op);
      grn_obj_unlink(ctx, resolve_res);
    }
    grn_obj_unlink(ctx, base_res);
  }

exit :
  if (scope_domain) {
    grn_obj_unlink(ctx, scope_domain);
  }
  if (sub_filter) {
    grn_obj_unlink(ctx, sub_filter);
  }

  return rc;
}

static grn_rc
selector_sub_filter(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                    int nargs, grn_obj **args,
                    grn_obj *res, grn_operator op)
{
  return run_sub_filter(ctx, table, nargs - 1, args + 1, res, op);
}

static grn_obj *
func_html_untag(grn_ctx *ctx, int nargs, grn_obj **args,
                grn_user_data *user_data)
{
  grn_obj *html_arg;
  int html_arg_domain;
  grn_obj html;
  grn_obj *text;
  const char *html_raw;
  int i, length;
  grn_bool in_tag = GRN_FALSE;

  if (nargs != 1) {
    ERR(GRN_INVALID_ARGUMENT, "HTML is missing");
    return NULL;
  }

  html_arg = args[0];
  html_arg_domain = html_arg->header.domain;
  switch (html_arg_domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    GRN_VALUE_VAR_SIZE_INIT(&html, GRN_OBJ_DO_SHALLOW_COPY, html_arg_domain);
    GRN_TEXT_SET(ctx, &html, GRN_TEXT_VALUE(html_arg), GRN_TEXT_LEN(html_arg));
    break;
  default :
    GRN_TEXT_INIT(&html, 0);
    if (grn_obj_cast(ctx, html_arg, &html, GRN_FALSE)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, html_arg);
      ERR(GRN_INVALID_ARGUMENT, "failed to cast to text: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      GRN_OBJ_FIN(ctx, &html);
      return NULL;
    }
    break;
  }

  text = GRN_PROC_ALLOC(html.header.domain, 0);
  if (!text) {
    GRN_OBJ_FIN(ctx, &html);
    return NULL;
  }

  html_raw = GRN_TEXT_VALUE(&html);
  length = GRN_TEXT_LEN(&html);
  for (i = 0; i < length; i++) {
    switch (html_raw[i]) {
    case '<' :
      in_tag = GRN_TRUE;
      break;
    case '>' :
      if (in_tag) {
        in_tag = GRN_FALSE;
      } else {
        GRN_TEXT_PUTC(ctx, text, html_raw[i]);
      }
      break;
    default :
      if (!in_tag) {
        GRN_TEXT_PUTC(ctx, text, html_raw[i]);
      }
      break;
    }
  }

  GRN_OBJ_FIN(ctx, &html);

  return text;
}

static grn_bool
grn_text_equal_cstr(grn_ctx *ctx, grn_obj *text, const char *cstr)
{
  int cstr_len;

  cstr_len = strlen(cstr);
  return (GRN_TEXT_LEN(text) == cstr_len &&
          strncmp(GRN_TEXT_VALUE(text), cstr, cstr_len) == 0);
}

typedef enum {
  BETWEEN_BORDER_INVALID,
  BETWEEN_BORDER_INCLUDE,
  BETWEEN_BORDER_EXCLUDE
} between_border_type;

typedef struct {
  grn_obj *value;
  grn_obj *min;
  grn_obj casted_min;
  between_border_type min_border_type;
  grn_obj *max;
  grn_obj casted_max;
  between_border_type max_border_type;
} between_data;

static void
between_data_init(grn_ctx *ctx, between_data *data)
{
  GRN_VOID_INIT(&(data->casted_min));
  GRN_VOID_INIT(&(data->casted_max));
}

static void
between_data_fin(grn_ctx *ctx, between_data *data)
{
  GRN_OBJ_FIN(ctx, &(data->casted_min));
  GRN_OBJ_FIN(ctx, &(data->casted_max));
}

static between_border_type
between_parse_border(grn_ctx *ctx, grn_obj *border,
                     const char *argument_description)
{
  grn_obj inspected;

  /* TODO: support other text types */
  if (border->header.domain == GRN_DB_TEXT) {
    if (grn_text_equal_cstr(ctx, border, "include")) {
      return BETWEEN_BORDER_INCLUDE;
    } else if (grn_text_equal_cstr(ctx, border, "exclude")) {
      return BETWEEN_BORDER_EXCLUDE;
    }
  }

  GRN_TEXT_INIT(&inspected, 0);
  grn_inspect(ctx, &inspected, border);
  ERR(GRN_INVALID_ARGUMENT,
      "between(): %s must be \"include\" or \"exclude\": <%.*s>",
      argument_description,
      (int)GRN_TEXT_LEN(&inspected),
      GRN_TEXT_VALUE(&inspected));
  grn_obj_unlink(ctx, &inspected);

  return BETWEEN_BORDER_INVALID;
}

static grn_rc
between_cast(grn_ctx *ctx, grn_obj *source, grn_obj *destination, grn_id domain,
             const char *target_argument_name)
{
  grn_rc rc;

  GRN_OBJ_INIT(destination, GRN_BULK, 0, domain);
  rc = grn_obj_cast(ctx, source, destination, GRN_FALSE);
  if (rc != GRN_SUCCESS) {
    grn_obj inspected_source;
    grn_obj *domain_object;
    char domain_name[GRN_TABLE_MAX_KEY_SIZE];
    int domain_name_length;

    GRN_TEXT_INIT(&inspected_source, 0);
    grn_inspect(ctx, &inspected_source, source);

    domain_object = grn_ctx_at(ctx, domain);
    domain_name_length =
      grn_obj_name(ctx, domain_object, domain_name, GRN_TABLE_MAX_KEY_SIZE);

    ERR(rc, "between(): failed to cast %s: <%.*s> -> <%.*s>",
        target_argument_name,
        (int)GRN_TEXT_LEN(&inspected_source),
        GRN_TEXT_VALUE(&inspected_source),
        domain_name_length,
        domain_name);

    grn_obj_unlink(ctx, &inspected_source);
    grn_obj_unlink(ctx, domain_object);
  }

  return rc;
}

static grn_rc
between_parse_args(grn_ctx *ctx, int nargs, grn_obj **args, between_data *data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *min_border;
  grn_obj *max_border;

  if (nargs != 5) {
    ERR(GRN_INVALID_ARGUMENT,
        "between(): wrong number of arguments (%d for 5)", nargs);
    rc = ctx->rc;
    goto exit;
  }

  data->value = args[0];
  data->min   = args[1];
  min_border  = args[2];
  data->max   = args[3];
  max_border  = args[4];

  data->min_border_type =
    between_parse_border(ctx, min_border, "the 3rd argument (min_border)");
  if (data->min_border_type == BETWEEN_BORDER_INVALID) {
    rc = ctx->rc;
    goto exit;
  }

  data->max_border_type =
    between_parse_border(ctx, max_border, "the 5th argument (max_border)");
  if (data->max_border_type == BETWEEN_BORDER_INVALID) {
    rc = ctx->rc;
    goto exit;
  }

  {
    grn_id value_type;
    if (data->value->header.type == GRN_BULK) {
      value_type = data->value->header.domain;
    } else {
      value_type = grn_obj_get_range(ctx, data->value);
    }
    if (value_type != data->min->header.domain) {
      rc = between_cast(ctx, data->min, &data->casted_min, value_type, "min");
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      data->min = &(data->casted_min);
    }

    if (value_type != data->max->header.domain) {
      rc = between_cast(ctx, data->max, &data->casted_max, value_type, "max");
      if (rc != GRN_SUCCESS) {
        goto exit;
      }
      data->max = &(data->casted_max);
    }
  }

exit :
  return rc;
}

static grn_bool
between_create_expr(grn_ctx *ctx, grn_obj *table, between_data *data,
                    grn_obj **expr, grn_obj **variable)
{
  GRN_EXPR_CREATE_FOR_QUERY(ctx, table, *expr, *variable);
  if (!*expr) {
    return GRN_FALSE;
  }

  if (data->value->header.type == GRN_BULK) {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_PUSH, 1);
  } else {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_GET_VALUE, 1);
  }
  grn_expr_append_obj(ctx, *expr, data->min, GRN_OP_PUSH, 1);
  if (data->min_border_type == BETWEEN_BORDER_INCLUDE) {
    grn_expr_append_op(ctx, *expr, GRN_OP_GREATER_EQUAL, 2);
  } else {
    grn_expr_append_op(ctx, *expr, GRN_OP_GREATER, 2);
  }

  if (data->value->header.type == GRN_BULK) {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_PUSH, 1);
  } else {
    grn_expr_append_obj(ctx, *expr, data->value, GRN_OP_GET_VALUE, 1);
  }
  grn_expr_append_obj(ctx, *expr, data->max, GRN_OP_PUSH, 1);
  if (data->max_border_type == BETWEEN_BORDER_INCLUDE) {
    grn_expr_append_op(ctx, *expr, GRN_OP_LESS_EQUAL, 2);
  } else {
    grn_expr_append_op(ctx, *expr, GRN_OP_LESS, 2);
  }

  grn_expr_append_op(ctx, *expr, GRN_OP_AND, 2);

  return GRN_TRUE;
}

static grn_obj *
func_between(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *found;
  between_data data;
  grn_obj *condition = NULL;
  grn_obj *variable;
  grn_obj *table = NULL;
  grn_obj *between_expr;
  grn_obj *between_variable;
  grn_obj *result;

  found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!found) {
    return NULL;
  }
  GRN_BOOL_SET(ctx, found, GRN_FALSE);

  grn_proc_get_info(ctx, user_data, NULL, NULL, &condition);
  if (!condition) {
    return found;
  }

  variable = grn_expr_get_var_by_offset(ctx, condition, 0);
  if (!variable) {
    return found;
  }

  between_data_init(ctx, &data);
  rc = between_parse_args(ctx, nargs, args, &data);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  table = grn_ctx_at(ctx, variable->header.domain);
  if (!table) {
    goto exit;
  }
  if (!between_create_expr(ctx, table, &data, &between_expr, &between_variable)) {
    goto exit;
  }

  GRN_RECORD_SET(ctx, between_variable, GRN_RECORD_VALUE(variable));
  result = grn_expr_exec(ctx, between_expr, 0);
  if (grn_obj_is_true(ctx, result)) {
    GRN_BOOL_SET(ctx, found, GRN_TRUE);
  }

  grn_obj_unlink(ctx, between_expr);
  grn_obj_unlink(ctx, table);

exit :
  between_data_fin(ctx, &data);
  if (table) {
    grn_obj_unlink(ctx, table);
  }

  return found;
}

static grn_bool
selector_between_sequential_search_should_use(grn_ctx *ctx,
                                              grn_obj *table,
                                              grn_obj *index,
                                              grn_obj *index_table,
                                              between_data *data,
                                              grn_obj *res,
                                              grn_operator op,
                                              double too_many_index_match_ratio)
{
  int n_index_keys;

  if (too_many_index_match_ratio < 0.0) {
    return GRN_FALSE;
  }

  if (op != GRN_OP_AND) {
    return GRN_FALSE;
  }

  if (index->header.flags & GRN_OBJ_WITH_WEIGHT) {
    return GRN_FALSE;
  }

  n_index_keys = grn_table_size(ctx, index_table);
  if (n_index_keys == 0) {
    return GRN_FALSE;
  }

  switch (index_table->header.domain) {
  /* TODO: */
  /* case GRN_DB_INT8 : */
  /* case GRN_DB_UINT8 : */
  /* case GRN_DB_INT16 : */
  /* case GRN_DB_UINT16 : */
  /* case GRN_DB_INT32 : */
  /* case GRN_DB_UINT32 : */
  /* case GRN_DB_INT64 : */
  /* case GRN_DB_UINT64 : */
  /* case GRN_DB_FLOAT : */
  case GRN_DB_TIME :
    break;
  default :
    return GRN_FALSE;
  }

  {
    grn_table_cursor *cursor;
    long long int all_min;
    long long int all_max;
    cursor = grn_table_cursor_open(ctx, index_table,
                                   NULL, -1,
                                   NULL, -1,
                                   0, 1,
                                   GRN_CURSOR_BY_KEY | GRN_CURSOR_ASCENDING);
    if (!cursor) {
      return GRN_FALSE;
    }
    if (grn_table_cursor_next(ctx, cursor) == GRN_ID_NIL) {
      grn_table_cursor_close(ctx, cursor);
      return GRN_FALSE;
    }
    {
      long long int *key;
      grn_table_cursor_get_key(ctx, cursor, (void **)&key);
      all_min = *key;
    }
    grn_table_cursor_close(ctx, cursor);

    cursor = grn_table_cursor_open(ctx, index_table,
                                   NULL, 0, NULL, 0,
                                   0, 1,
                                   GRN_CURSOR_BY_KEY | GRN_CURSOR_DESCENDING);
    if (!cursor) {
      return GRN_FALSE;
    }
    if (grn_table_cursor_next(ctx, cursor) == GRN_ID_NIL) {
      grn_table_cursor_close(ctx, cursor);
      return GRN_FALSE;
    }
    {
      long long int *key;
      grn_table_cursor_get_key(ctx, cursor, (void **)&key);
      all_max = *key;
    }
    grn_table_cursor_close(ctx, cursor);

    /*
     * We assume the following:
     *   * homogeneous index key distribution.
     *   * each index key matches only 1 record.
     * TODO: Improve me.
     */
    {
      int n_existing_records;
      int n_indexed_records;
      long long int all_difference;
      long long int argument_difference;

      n_existing_records = grn_table_size(ctx, res);

      all_difference = all_max - all_min;
      if (all_difference <= 0) {
        return GRN_FALSE;
      }
      argument_difference =
        GRN_TIME_VALUE(data->max) - GRN_TIME_VALUE(data->min);
      if (argument_difference <= 0) {
        return GRN_FALSE;
      }
      n_indexed_records =
        n_index_keys * ((double)argument_difference / (double)all_difference);

      /*
       * Same as:
       * ((n_existing_record / n_indexed_records) > too_many_index_match_ratio)
       */
      if (n_existing_records > (n_indexed_records * too_many_index_match_ratio)) {
        return GRN_FALSE;
      }
    }
  }

  return GRN_TRUE;
}

static grn_bool
selector_between_sequential_search(grn_ctx *ctx,
                                   grn_obj *table,
                                   grn_obj *index, grn_obj *index_table,
                                   between_data *data,
                                   grn_obj *res, grn_operator op)
{
  if (!selector_between_sequential_search_should_use(
        ctx, table, index, index_table, data, res, op,
        grn_between_too_many_index_match_ratio)) {
    return GRN_FALSE;
  }

  {
    int offset = 0;
    int limit = -1;
    int flags = 0;
    grn_table_cursor *cursor;
    grn_obj *expr;
    grn_obj *variable;
    grn_id id;

    if (!between_create_expr(ctx, table, data, &expr, &variable)) {
      return GRN_FALSE;
    }

    cursor = grn_table_cursor_open(ctx, res,
                                   NULL, 0,
                                   NULL, 0,
                                   offset, limit, flags);
    if (!cursor) {
      grn_obj_unlink(ctx, expr);
      return GRN_FALSE;
    }

    while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
      grn_id record_id;
      grn_obj *result;
      {
        grn_id *key;
        grn_table_cursor_get_key(ctx, cursor, (void **)&key);
        record_id = *key;
      }
      GRN_RECORD_SET(ctx, variable, record_id);
      result = grn_expr_exec(ctx, expr, 0);
      if (ctx->rc) {
        break;
      }
      if (grn_obj_is_true(ctx, result)) {
        grn_posting posting;
        posting.rid = record_id;
        posting.sid = 1;
        posting.pos = 0;
        posting.weight = 0;
        grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
      }
    }
    grn_obj_unlink(ctx, expr);
    grn_table_cursor_close(ctx, cursor);

    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  }

  return GRN_TRUE;
}

static grn_rc
selector_between(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                 int nargs, grn_obj **args,
                 grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  int offset = 0;
  int limit = -1;
  int flags = GRN_CURSOR_ASCENDING | GRN_CURSOR_BY_KEY;
  between_data data;
  grn_obj *index_table = NULL;
  grn_table_cursor *cursor;
  grn_id id;

  if (!index) {
    return GRN_INVALID_ARGUMENT;
  }

  between_data_init(ctx, &data);
  rc = between_parse_args(ctx, nargs - 1, args + 1, &data);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  if (data.min_border_type == BETWEEN_BORDER_EXCLUDE) {
    flags |= GRN_CURSOR_GT;
  }
  if (data.max_border_type == BETWEEN_BORDER_EXCLUDE) {
    flags |= GRN_CURSOR_LT;
  }

  index_table = grn_ctx_at(ctx, index->header.domain);
  if (selector_between_sequential_search(ctx, table, index, index_table,
                                         &data, res, op)) {
    goto exit;
  }

  cursor = grn_table_cursor_open(ctx, index_table,
                                 GRN_BULK_HEAD(data.min),
                                 GRN_BULK_VSIZE(data.min),
                                 GRN_BULK_HEAD(data.max),
                                 GRN_BULK_VSIZE(data.max),
                                 offset, limit, flags);
  if (!cursor) {
    rc = ctx->rc;
    goto exit;
  }

  while ((id = grn_table_cursor_next(ctx, cursor))) {
    grn_ii_at(ctx, (grn_ii *)index, id, (grn_hash *)res, op);
  }
  grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  grn_table_cursor_close(ctx, cursor);

exit :
  between_data_fin(ctx, &data);
  if (index_table) {
    grn_obj_unlink(ctx, index_table);
  }

  return rc;
}

static grn_obj *
func_in_values(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *found;
  grn_obj *target_value;
  int i;

  found = GRN_PROC_ALLOC(GRN_DB_BOOL, 0);
  if (!found) {
    return NULL;
  }
  GRN_BOOL_SET(ctx, found, GRN_FALSE);

  if (nargs < 1) {
    ERR(GRN_INVALID_ARGUMENT,
        "in_values(): wrong number of arguments (%d for 1..)", nargs);
    return found;
  }

  target_value = args[0];
  for (i = 1; i < nargs; i++) {
    grn_obj *value = args[i];
    grn_bool result;

    result = grn_operator_exec_equal(ctx, target_value, value);
    if (ctx->rc) {
      break;
    }

    if (result) {
      GRN_BOOL_SET(ctx, found, GRN_TRUE);
      break;
    }
  }

  return found;
}

static grn_bool
is_reference_type_column(grn_ctx *ctx, grn_obj *column)
{
  grn_bool is_reference_type;
  grn_obj *range;

  range = grn_ctx_at(ctx, grn_obj_get_range(ctx, column));
  switch (range->header.type) {
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    is_reference_type = GRN_TRUE;
    break;
  default :
    is_reference_type = GRN_FALSE;
    break;
  }
  grn_obj_unlink(ctx, range);

  return is_reference_type;
}

static grn_obj *
selector_in_values_find_source(grn_ctx *ctx, grn_obj *index, grn_obj *res)
{
  grn_id source_id = GRN_ID_NIL;
  grn_obj source_ids;
  unsigned int n_source_ids;

  GRN_UINT32_INIT(&source_ids, GRN_OBJ_VECTOR);
  grn_obj_get_info(ctx, index, GRN_INFO_SOURCE, &source_ids);
  n_source_ids = GRN_BULK_VSIZE(&source_ids) / sizeof(grn_id);
  if (n_source_ids == 1) {
    source_id = GRN_UINT32_VALUE_AT(&source_ids, 0);
  }
  GRN_OBJ_FIN(ctx, &source_ids);

  if (source_id == GRN_ID_NIL) {
    return NULL;
  } else {
    return grn_ctx_at(ctx, source_id);
  }
}

static grn_bool
selector_in_values_sequential_search(grn_ctx *ctx,
                                     grn_obj *table,
                                     grn_obj *index,
                                     int n_values,
                                     grn_obj **values,
                                     grn_obj *res,
                                     grn_operator op)
{
  grn_obj *source;
  int n_existing_records;

  if (grn_in_values_too_many_index_match_ratio < 0.0) {
    return GRN_FALSE;
  }

  if (op != GRN_OP_AND) {
    return GRN_FALSE;
  }

  if (index->header.flags & GRN_OBJ_WITH_WEIGHT) {
    return GRN_FALSE;
  }

  n_existing_records = grn_table_size(ctx, res);
  if (n_existing_records == 0) {
    return GRN_TRUE;
  }

  source = selector_in_values_find_source(ctx, index, res);
  if (!source) {
    return GRN_FALSE;
  }

  if (!is_reference_type_column(ctx, source)) {
    grn_obj_unlink(ctx, source);
    return GRN_FALSE;
  }

  {
    grn_obj value_ids;
    int i, n_value_ids;
    int n_indexed_records = 0;

    {
      grn_id range_id;
      grn_obj *range;

      range_id = grn_obj_get_range(ctx, source);
      range = grn_ctx_at(ctx, range_id);
      if (!range) {
        grn_obj_unlink(ctx, source);
        return GRN_FALSE;
      }

      GRN_RECORD_INIT(&value_ids, GRN_OBJ_VECTOR, range_id);
      for (i = 0; i < n_values; i++) {
        grn_obj *value = values[i];
        grn_id value_id;

        value_id = grn_table_get(ctx, range,
                                 GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));
        if (value_id == GRN_ID_NIL) {
          continue;
        }
        GRN_RECORD_PUT(ctx, &value_ids, value_id);
      }
      grn_obj_unlink(ctx, range);
    }

    n_value_ids = GRN_BULK_VSIZE(&value_ids) / sizeof(grn_id);
    for (i = 0; i < n_value_ids; i++) {
      grn_id value_id = GRN_RECORD_VALUE_AT(&value_ids, i);
      n_indexed_records += grn_ii_estimate_size(ctx, (grn_ii *)index, value_id);
    }

    /*
     * Same as:
     * ((n_existing_record / n_indexed_records) >
     *  grn_in_values_too_many_index_match_ratio)
    */
    if (n_existing_records >
        (n_indexed_records * grn_in_values_too_many_index_match_ratio)) {
      grn_obj_unlink(ctx, &value_ids);
      grn_obj_unlink(ctx, source);
      return GRN_FALSE;
    }

    {
      grn_obj *accessor;
      char local_source_name[GRN_TABLE_MAX_KEY_SIZE];
      int local_source_name_length;

      local_source_name_length = grn_column_name(ctx, source,
                                                 local_source_name,
                                                 GRN_TABLE_MAX_KEY_SIZE);
      grn_obj_unlink(ctx, source);
      accessor = grn_obj_column(ctx, res,
                                local_source_name,
                                local_source_name_length);
      {
        grn_table_cursor *cursor;
        grn_id record_id;
        grn_obj record_value;
        GRN_RECORD_INIT(&record_value, 0, grn_obj_id(ctx, res));
        cursor = grn_table_cursor_open(ctx, res,
                                       NULL, 0, NULL, 0,
                                       0, -1, GRN_CURSOR_ASCENDING);
        while ((record_id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
          GRN_BULK_REWIND(&record_value);
          grn_obj_get_value(ctx, accessor, record_id, &record_value);
          for (i = 0; i < n_value_ids; i++) {
            grn_id value_id = GRN_RECORD_VALUE_AT(&value_ids, i);
            if (value_id == GRN_RECORD_VALUE(&record_value)) {
              grn_posting posting;
              posting.rid = record_id;
              posting.sid = 1;
              posting.pos = 0;
              posting.weight = 0;
              grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
            }
          }
        }
        grn_table_cursor_close(ctx, cursor);
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
        GRN_OBJ_FIN(ctx, &record_value);
      }
      grn_obj_unlink(ctx, accessor);
    }
    grn_obj_unlink(ctx, &value_ids);
  }
  grn_obj_unlink(ctx, source);

  return GRN_TRUE;
}

static grn_rc
selector_in_values(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                   int nargs, grn_obj **args,
                   grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  int i, n_values;
  grn_obj **values;

  if (!index) {
    return GRN_INVALID_ARGUMENT;
  }

  if (nargs < 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "in_values(): wrong number of arguments (%d for 1..)", nargs);
    return ctx->rc;
  }

  n_values = nargs - 2;
  values = args + 2;

  if (n_values == 0) {
    return rc;
  }

  if (selector_in_values_sequential_search(ctx, table, index,
                                           n_values, values,
                                           res, op)) {
    return ctx->rc;
  }

  ctx->flags |= GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
  for (i = 0; i < n_values; i++) {
    grn_obj *value = values[i];
    grn_search_optarg search_options;
    memset(&search_options, 0, sizeof(grn_search_optarg));
    search_options.mode = GRN_OP_EXACT;
    search_options.similarity_threshold = 0;
    search_options.max_interval = 0;
    search_options.weight_vector = NULL;
    search_options.vector_size = 0;
    search_options.proc = NULL;
    search_options.max_size = 0;
    search_options.scorer = NULL;
    if (i == n_values - 1) {
      ctx->flags &= ~GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
    }
    rc = grn_obj_search(ctx, index, value, res, op, &search_options);
    if (rc != GRN_SUCCESS) {
      break;
    }
  }

  return rc;
}

static grn_obj *
proc_range_filter(grn_ctx *ctx, int nargs, grn_obj **args,
                  grn_user_data *user_data)
{
  grn_obj *table_name = VAR(0);
  grn_obj *column_name = VAR(1);
  grn_obj *min = VAR(2);
  grn_obj *min_border = VAR(3);
  grn_obj *max = VAR(4);
  grn_obj *max_border = VAR(5);
  grn_obj *offset = VAR(6);
  grn_obj *limit = VAR(7);
  grn_obj *filter = VAR(8);
  grn_obj *output_columns = VAR(9);
  grn_obj *table;
  grn_obj *res = NULL;
  grn_obj *filter_expr = NULL;
  grn_obj *filter_variable = NULL;
  int real_offset;
  int real_limit;

  table = grn_ctx_get(ctx, GRN_TEXT_VALUE(table_name), GRN_TEXT_LEN(table_name));
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[range_filter] nonexistent table <%.*s>",
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name));
    return NULL;
  }

  if (GRN_TEXT_LEN(filter) > 0) {
    GRN_EXPR_CREATE_FOR_QUERY(ctx, table, filter_expr, filter_variable);
    if (!filter_expr) {
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] failed to create expression");
      goto exit;
    }

    grn_expr_parse(ctx, filter_expr,
                   GRN_TEXT_VALUE(filter), GRN_TEXT_LEN(filter),
                   NULL, GRN_OP_MATCH, GRN_OP_AND, GRN_EXPR_SYNTAX_SCRIPT);
    if (ctx->rc != GRN_SUCCESS) {
      goto exit;
    }
  }

  res = grn_table_create(ctx, NULL, 0, NULL,
                         GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                         table, NULL);
  if (!res) {
    ERR(GRN_INVALID_ARGUMENT,
        "[range_filter] failed to result table");
    goto exit;
  }

  {
    grn_obj int32_value;

    GRN_INT32_INIT(&int32_value, 0);

    if (GRN_TEXT_LEN(offset) > 0) {
      if (grn_obj_cast(ctx, offset, &int32_value, GRN_FALSE) != GRN_SUCCESS) {
        ERR(GRN_INVALID_ARGUMENT,
            "[range_filter] invalid offset format: <%.*s>",
            (int)GRN_TEXT_LEN(offset), GRN_TEXT_VALUE(offset));
        GRN_OBJ_FIN(ctx, &int32_value);
        goto exit;
      }
      real_offset = GRN_INT32_VALUE(&int32_value);
    } else {
      real_offset = 0;
    }

    GRN_BULK_REWIND(&int32_value);

    if (GRN_TEXT_LEN(limit) > 0) {
      if (grn_obj_cast(ctx, limit, &int32_value, GRN_FALSE) != GRN_SUCCESS) {
        ERR(GRN_INVALID_ARGUMENT,
            "[range_filter] invalid limit format: <%.*s>",
            (int)GRN_TEXT_LEN(limit), GRN_TEXT_VALUE(limit));
        GRN_OBJ_FIN(ctx, &int32_value);
        goto exit;
      }
      real_limit = GRN_INT32_VALUE(&int32_value);
    } else {
      real_limit = GRN_SELECT_DEFAULT_LIMIT;
    }

    GRN_OBJ_FIN(ctx, &int32_value);
  }
  {
    grn_rc rc;
    int original_offset = real_offset;
    int original_limit = real_limit;
    rc = grn_normalize_offset_and_limit(ctx, grn_table_size(ctx, table),
                                        &real_offset, &real_limit);
    switch (rc) {
    case GRN_TOO_SMALL_OFFSET :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too small offset: <%d>", original_offset);
      goto exit;
    case GRN_TOO_LARGE_OFFSET :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too large offset: <%d>", original_offset);
      goto exit;
    case GRN_TOO_SMALL_LIMIT :
      ERR(GRN_INVALID_ARGUMENT,
          "[range_filter] too small limit: <%d>", original_limit);
      goto exit;
    default :
      break;
    }
  }

  if (real_limit != 0) {
    grn_table_sort_key *sort_keys;
    unsigned int n_sort_keys;
    sort_keys = grn_table_sort_key_from_str(ctx,
                                            GRN_TEXT_VALUE(column_name),
                                            GRN_TEXT_LEN(column_name),
                                            table,
                                            &n_sort_keys);
    if (n_sort_keys == 1) {
      grn_table_sort_key *sort_key;
      grn_obj *index;
      int n_indexes;
      grn_operator op = GRN_OP_OR;

      sort_key = &(sort_keys[0]);
      n_indexes = grn_column_index(ctx, sort_key->key, GRN_OP_LESS,
                                   &index, 1, NULL);
      if (n_indexes > 0) {
        grn_obj *lexicon;
        grn_table_cursor *table_cursor;
        int table_cursor_flags = 0;
        between_border_type min_border_type;
        between_border_type max_border_type;
        grn_obj real_min;
        grn_obj real_max;
        int n_records = 0;
        grn_obj *index_cursor;
        int index_cursor_flags = 0;
        grn_posting *posting;

        lexicon = grn_ctx_at(ctx, index->header.domain);
        if (sort_key->flags & GRN_TABLE_SORT_DESC) {
          table_cursor_flags |= GRN_CURSOR_DESCENDING;
        } else {
          table_cursor_flags |= GRN_CURSOR_ASCENDING;
        }
        if (GRN_TEXT_LEN(min_border) > 0) {
          min_border_type = between_parse_border(ctx, min_border, "min_border");
        } else {
          min_border_type = BETWEEN_BORDER_INCLUDE;
        }
        if (GRN_TEXT_LEN(max_border) > 0) {
          max_border_type = between_parse_border(ctx, max_border, "max_border");
        } else {
          max_border_type = BETWEEN_BORDER_INCLUDE;
        }
        if (min_border_type == BETWEEN_BORDER_EXCLUDE) {
          table_cursor_flags |= GRN_CURSOR_GT;
        }
        if (max_border_type == BETWEEN_BORDER_EXCLUDE) {
          table_cursor_flags |= GRN_CURSOR_LT;
        }
        GRN_OBJ_INIT(&real_min, GRN_BULK, 0, lexicon->header.domain);
        GRN_OBJ_INIT(&real_max, GRN_BULK, 0, lexicon->header.domain);
        if (GRN_TEXT_LEN(min) > 0) {
          grn_obj_cast(ctx, min, &real_min, GRN_FALSE);
        }
        if (GRN_TEXT_LEN(max) > 0) {
          grn_obj_cast(ctx, max, &real_max, GRN_FALSE);
        }
        table_cursor = grn_table_cursor_open(ctx, lexicon,
                                             GRN_BULK_HEAD(&real_min),
                                             GRN_BULK_VSIZE(&real_min),
                                             GRN_BULK_HEAD(&real_max),
                                             GRN_BULK_VSIZE(&real_max),
                                             0, -1, table_cursor_flags);
        index_cursor = grn_index_cursor_open(ctx, table_cursor,
                                             index, GRN_ID_NIL, GRN_ID_NIL,
                                             index_cursor_flags);
        while ((posting = grn_index_cursor_next(ctx, index_cursor, NULL))) {
          grn_bool result_boolean = GRN_FALSE;

          if (filter_expr) {
            grn_obj *result;
            GRN_RECORD_SET(ctx, filter_variable, posting->rid);
            result = grn_expr_exec(ctx, filter_expr, 0);
            if (ctx->rc) {
              break;
            }
            result_boolean = grn_obj_is_true(ctx, result);
          } else {
            result_boolean = GRN_TRUE;
          }

          if (result_boolean) {
            if (n_records >= real_offset) {
              grn_ii_posting_add(ctx, posting, (grn_hash *)res, op);
            }
            n_records++;
            if (n_records == real_limit) {
              break;
            }
          }
        }
        grn_obj_unlink(ctx, index_cursor);
        grn_table_cursor_close(ctx, table_cursor);

        GRN_OBJ_FIN(ctx, &real_min);
        GRN_OBJ_FIN(ctx, &real_max);
      }
      grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
    }
    grn_table_sort_key_close(ctx, sort_keys, n_sort_keys);
  }

  if (ctx->rc == GRN_SUCCESS) {
    const char *raw_output_columns;
    int raw_output_columns_len;

    raw_output_columns = GRN_TEXT_VALUE(output_columns);
    raw_output_columns_len = GRN_TEXT_LEN(output_columns);
    if (raw_output_columns_len == 0) {
      raw_output_columns = GRN_SELECT_DEFAULT_OUTPUT_COLUMNS;
      raw_output_columns_len = strlen(raw_output_columns);
    }
    grn_proc_select_output_columns(ctx, res, -1, real_offset, real_limit,
                                   raw_output_columns,
                                   raw_output_columns_len,
                                   filter_expr);
  }

exit :
  if (filter_expr) {
    grn_obj_unlink(ctx, filter_expr);
  }
  if (res) {
    grn_obj_unlink(ctx, res);
  }

  return NULL;
}

static grn_obj *
proc_request_cancel(grn_ctx *ctx, int nargs, grn_obj **args,
                    grn_user_data *user_data)
{
  grn_obj *id = VAR(0);
  grn_bool canceled;

  if (GRN_TEXT_LEN(id) == 0) {
    ERR(GRN_INVALID_ARGUMENT, "[request_cancel] ID is missing");
    return NULL;
  }

  canceled = grn_request_canceler_cancel(GRN_TEXT_VALUE(id), GRN_TEXT_LEN(id));

  GRN_OUTPUT_MAP_OPEN("result", 2);
  GRN_OUTPUT_CSTR("id");
  GRN_OUTPUT_STR(GRN_TEXT_VALUE(id), GRN_TEXT_LEN(id));
  GRN_OUTPUT_CSTR("canceled");
  GRN_OUTPUT_BOOL(canceled);
  GRN_OUTPUT_MAP_CLOSE();

  return NULL;
}

static grn_obj *
proc_plugin_register(grn_ctx *ctx, int nargs, grn_obj **args,
                     grn_user_data *user_data)
{
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *name;
    GRN_TEXT_PUTC(ctx, VAR(0), '\0');
    name = GRN_TEXT_VALUE(VAR(0));
    grn_plugin_register(ctx, name);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[plugin_register] name is missing");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_plugin_unregister(grn_ctx *ctx, int nargs, grn_obj **args,
                       grn_user_data *user_data)
{
  if (GRN_TEXT_LEN(VAR(0))) {
    const char *name;
    GRN_TEXT_PUTC(ctx, VAR(0), '\0');
    name = GRN_TEXT_VALUE(VAR(0));
    grn_plugin_unregister(ctx, name);
  } else {
    ERR(GRN_INVALID_ARGUMENT, "[plugin_unregister] name is missing");
  }
  GRN_OUTPUT_BOOL(!ctx->rc);
  return NULL;
}

static grn_obj *
proc_io_flush(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *target_name;
  grn_obj *recursive;
  grn_obj *target;
  grn_bool is_recursive;

  target_name = VAR(0);
  recursive = VAR(1);

  if (GRN_TEXT_LEN(target_name) > 0) {
    target = grn_ctx_get(ctx,
                         GRN_TEXT_VALUE(target_name),
                         GRN_TEXT_LEN(target_name));
    if (!target) {
      ERR(GRN_INVALID_ARGUMENT, "[io_flush] unknown target: <%.*s>",
          (int)GRN_TEXT_LEN(target_name),
          GRN_TEXT_VALUE(target_name));
      GRN_OUTPUT_BOOL(GRN_FALSE);
      return NULL;
    }
  } else {
    target = grn_ctx_db(ctx);
  }

  is_recursive = grn_proc_option_value_bool(ctx, recursive, GRN_TRUE);
  {
    grn_rc rc;
    if (is_recursive) {
      rc = grn_obj_flush_recursive(ctx, target);
    } else {
      rc = grn_obj_flush(ctx, target);
    }

    GRN_OUTPUT_BOOL(rc == GRN_SUCCESS);
  }

  return NULL;
}

static grn_obj *
proc_thread_limit(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_obj *max_bulk;
  uint32_t current_limit;

  current_limit = grn_thread_get_limit();
  GRN_OUTPUT_INT64(current_limit);

  max_bulk = VAR(0);
  if (GRN_TEXT_LEN(max_bulk) > 0) {
    uint32_t max;
    const char *max_text = GRN_TEXT_VALUE(max_bulk);
    const char *max_text_end;
    const char *max_text_rest;

    max_text_end = max_text + GRN_TEXT_LEN(max_bulk);
    max = grn_atoui(max_text, max_text_end, &max_text_rest);
    if (max_text_rest != max_text_end) {
      ERR(GRN_INVALID_ARGUMENT,
          "[thread_limit] max must be unsigned integer value: <%.*s>",
          (int)GRN_TEXT_LEN(max_bulk),
          max_text);
      return NULL;
    }
    if (max == 0) {
      ERR(GRN_INVALID_ARGUMENT,
          "[thread_limit] max must be 1 or larger: <%.*s>",
          (int)GRN_TEXT_LEN(max_bulk),
          max_text);
      return NULL;
    }
    grn_thread_set_limit(max);
  }

  return NULL;
}

static grn_obj *
proc_database_unmap(grn_ctx *ctx, int nargs, grn_obj **args,
                    grn_user_data *user_data)
{
  grn_rc rc;
  uint32_t current_limit;

  current_limit = grn_thread_get_limit();
  if (current_limit != 1) {
    ERR(GRN_OPERATION_NOT_PERMITTED,
        "[database_unmap] the max number of threads must be 1: <%u>",
        current_limit);
    GRN_OUTPUT_BOOL(GRN_FALSE);
    return NULL;
  }

  rc = grn_db_unmap(ctx, grn_ctx_db(ctx));
  GRN_OUTPUT_BOOL(rc == GRN_SUCCESS);

  return NULL;
}

static grn_rc
proc_column_copy_resolve_target(grn_ctx *ctx,
                                const char *label,
                                grn_obj *table_name,
                                grn_obj *column_name,
                                grn_obj **table,
                                grn_obj **column)
{
  if (GRN_TEXT_LEN(table_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][copy] %s table name isn't specified",
        label);
    return ctx->rc;
  }
  *table = grn_ctx_get(ctx,
                       GRN_TEXT_VALUE(table_name),
                       GRN_TEXT_LEN(table_name));
  if (!*table) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][copy] %s table isn't found: <%.*s>",
        label,
        (int)GRN_TEXT_LEN(table_name),
        GRN_TEXT_VALUE(table_name));
    return ctx->rc;
  }

  if (GRN_TEXT_LEN(column_name) == 0) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][copy] %s column name isn't specified: <%.*s>",
        label,
        (int)GRN_TEXT_LEN(table_name),
        GRN_TEXT_VALUE(table_name));
    return ctx->rc;
  }
  *column = grn_obj_column(ctx, *table,
                           GRN_TEXT_VALUE(column_name),
                           GRN_TEXT_LEN(column_name));
  if (!*column) {
    ERR(GRN_INVALID_ARGUMENT,
        "[column][copy] %s column isn't found: <%.*s.%.*s>",
        label,
        (int)GRN_TEXT_LEN(table_name), GRN_TEXT_VALUE(table_name),
        (int)GRN_TEXT_LEN(column_name), GRN_TEXT_VALUE(column_name));
    return ctx->rc;
  }

  return ctx->rc;
}

static void
proc_column_copy_same_table(grn_ctx *ctx, grn_obj *table,
                            grn_obj *from_column, grn_obj *to_column)
{
  grn_table_cursor *cursor;
  grn_id id;
  grn_obj value;

  cursor = grn_table_cursor_open(ctx, table,
                                 NULL, 0,
                                 NULL, 0,
                                 0, -1, 0);
  if (!cursor) {
    return;
  }

  GRN_VOID_INIT(&value);
  while ((id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx, from_column, id, &value);
    grn_obj_set_value(ctx, to_column, id, &value, GRN_OBJ_SET);
  }
  GRN_OBJ_FIN(ctx, &value);
  grn_table_cursor_close(ctx, cursor);
}

static void
proc_column_copy_same_key_type(grn_ctx *ctx,
                               grn_obj *from_table,
                               grn_obj *from_column,
                               grn_obj *to_table,
                               grn_obj *to_column)
{
  grn_table_cursor *cursor;
  grn_id from_id;
  grn_obj value;

  cursor = grn_table_cursor_open(ctx, from_table,
                                 NULL, 0,
                                 NULL, 0,
                                 0, -1, 0);
  if (!cursor) {
    return;
  }

  GRN_VOID_INIT(&value);
  while ((from_id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    void *key;
    int key_size;
    grn_id to_id;

    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
    to_id = grn_table_add(ctx, to_table, key, key_size, NULL);
    if (to_id == GRN_ID_NIL) {
      continue;
    }

    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx, from_column, from_id, &value);
    grn_obj_set_value(ctx, to_column, to_id, &value, GRN_OBJ_SET);
  }
  GRN_OBJ_FIN(ctx, &value);
  grn_table_cursor_close(ctx, cursor);
}

static void
proc_column_copy_different(grn_ctx *ctx,
                           grn_obj *from_table,
                           grn_obj *from_column,
                           grn_obj *to_table,
                           grn_obj *to_column,
                           grn_obj *from_table_name,
                           grn_obj *from_column_name,
                           grn_obj *to_table_name,
                           grn_obj *to_column_name)
{
  grn_table_cursor *cursor;
  grn_id from_id;
  grn_obj from_key_buffer;
  grn_obj to_key_buffer;
  grn_obj value;

  cursor = grn_table_cursor_open(ctx, from_table,
                                 NULL, 0,
                                 NULL, 0,
                                 0, -1, 0);
  if (!cursor) {
    return;
  }

  if (from_table->header.domain == GRN_DB_SHORT_TEXT) {
    GRN_SHORT_TEXT_INIT(&from_key_buffer, 0);
  } else {
    GRN_VALUE_FIX_SIZE_INIT(&from_key_buffer, 0, from_table->header.domain);
  }
  if (to_table->header.domain == GRN_DB_SHORT_TEXT) {
    GRN_SHORT_TEXT_INIT(&to_key_buffer, 0);
  } else {
    GRN_VALUE_FIX_SIZE_INIT(&to_key_buffer, 0, to_table->header.domain);
  }
  GRN_VOID_INIT(&value);
  while ((from_id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
    void *key;
    int key_size;
    grn_rc cast_rc;
    grn_id to_id;

    GRN_BULK_REWIND(&from_key_buffer);
    GRN_BULK_REWIND(&to_key_buffer);

    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
    grn_bulk_write(ctx, &from_key_buffer, key, key_size);
    cast_rc = grn_obj_cast(ctx, &from_key_buffer, &to_key_buffer, GRN_FALSE);
    if (cast_rc != GRN_SUCCESS) {
      grn_obj *to_key_type;
      grn_obj inspected_key;
      grn_obj inspected_to_key_type;

      to_key_type = grn_ctx_at(ctx, to_table->header.domain);
      GRN_TEXT_INIT(&inspected_key, 0);
      GRN_TEXT_INIT(&inspected_to_key_type, 0);
      grn_inspect(ctx, &inspected_key, &from_key_buffer);
      grn_inspect(ctx, &inspected_to_key_type, to_key_type);
      ERR(cast_rc,
          "[column][copy] failed to cast key: <%.*s> -> %.*s: "
          "<%.*s.%.*s> -> <%.*s.%.*s>",
          (int)GRN_TEXT_LEN(&inspected_key),
          GRN_TEXT_VALUE(&inspected_key),
          (int)GRN_TEXT_LEN(&inspected_to_key_type),
          GRN_TEXT_VALUE(&inspected_to_key_type),
          (int)GRN_TEXT_LEN(from_table_name),
          GRN_TEXT_VALUE(from_table_name),
          (int)GRN_TEXT_LEN(from_column_name),
          GRN_TEXT_VALUE(from_column_name),
          (int)GRN_TEXT_LEN(to_table_name),
          GRN_TEXT_VALUE(to_table_name),
          (int)GRN_TEXT_LEN(to_column_name),
          GRN_TEXT_VALUE(to_column_name));
      GRN_OBJ_FIN(ctx, &inspected_key);
      GRN_OBJ_FIN(ctx, &inspected_to_key_type);
      break;
    }
    to_id = grn_table_add(ctx, to_table,
                          GRN_BULK_HEAD(&to_key_buffer),
                          GRN_BULK_VSIZE(&to_key_buffer),
                          NULL);
    if (to_id == GRN_ID_NIL) {
      continue;
    }

    GRN_BULK_REWIND(&value);
    grn_obj_get_value(ctx, from_column, from_id, &value);
    grn_obj_set_value(ctx, to_column, to_id, &value, GRN_OBJ_SET);
  }
  GRN_OBJ_FIN(ctx, &from_key_buffer);
  GRN_OBJ_FIN(ctx, &to_key_buffer);
  GRN_OBJ_FIN(ctx, &value);

  grn_table_cursor_close(ctx, cursor);
}

static grn_obj *
proc_column_copy(grn_ctx *ctx, int nargs, grn_obj **args,
                 grn_user_data *user_data)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *from_table = NULL;
  grn_obj *from_column = NULL;
  grn_obj *to_table = NULL;
  grn_obj *to_column = NULL;
  grn_obj *from_table_name = VAR(0);
  grn_obj *from_column_name = VAR(1);
  grn_obj *to_table_name = VAR(2);
  grn_obj *to_column_name = VAR(3);

  rc = proc_column_copy_resolve_target(ctx, "from",
                                       from_table_name, from_column_name,
                                       &from_table, &from_column);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }
  rc = proc_column_copy_resolve_target(ctx, "to",
                                       to_table_name, to_column_name,
                                       &to_table, &to_column);
  if (rc != GRN_SUCCESS) {
    goto exit;
  }

  if ((from_table->header.type == GRN_TABLE_NO_KEY ||
       to_table->header.type == GRN_TABLE_NO_KEY) &&
      from_table != to_table) {
    rc = GRN_OPERATION_NOT_SUPPORTED;
    ERR(rc,
        "[column][copy] copy from/to TABLE_NO_KEY isn't supported: "
        "<%.*s.%.*s> -> <%.*s.%.*s>",
        (int)GRN_TEXT_LEN(from_table_name), GRN_TEXT_VALUE(from_table_name),
        (int)GRN_TEXT_LEN(from_column_name), GRN_TEXT_VALUE(from_column_name),
        (int)GRN_TEXT_LEN(to_table_name), GRN_TEXT_VALUE(to_table_name),
        (int)GRN_TEXT_LEN(to_column_name), GRN_TEXT_VALUE(to_column_name));
    goto exit;
  }

  if (from_table == to_table) {
    proc_column_copy_same_table(ctx, from_table, from_column, to_column);
  } else if (from_table->header.domain == to_table->header.domain) {
    proc_column_copy_same_key_type(ctx,
                                   from_table, from_column,
                                   to_table, to_column);
  } else {
    proc_column_copy_different(ctx,
                               from_table,
                               from_column,
                               to_table,
                               to_column,
                               from_table_name,
                               from_column_name,
                               to_table_name,
                               to_column_name);
  }

exit :
  GRN_OUTPUT_BOOL(!rc);

  if (to_column) {
    grn_obj_unlink(ctx, to_column);
  }
  if (to_table) {
    grn_obj_unlink(ctx, to_table);
  }
  if (from_column) {
    grn_obj_unlink(ctx, from_column);
  }
  if (from_table) {
    grn_obj_unlink(ctx, from_table);
  }

  return NULL;
}

static grn_obj *
proc_reindex(grn_ctx *ctx, int nargs, grn_obj **args,
             grn_user_data *user_data)
{
  grn_obj *target_name;
  grn_obj *target;

  target_name = VAR(0);
  if (GRN_TEXT_LEN(target_name) == 0) {
    target = grn_ctx_db(ctx);
  } else {
    target = grn_ctx_get(ctx,
                         GRN_TEXT_VALUE(target_name),
                         GRN_TEXT_LEN(target_name));
    if (!target) {
      ERR(GRN_INVALID_ARGUMENT,
          "[reindex] nonexistent target: <%.*s>",
          (int)GRN_TEXT_LEN(target_name),
          GRN_TEXT_VALUE(target_name));
      GRN_OUTPUT_BOOL(GRN_FALSE);
      return NULL;
    }
  }

  grn_obj_reindex(ctx, target);

  GRN_OUTPUT_BOOL(ctx->rc == GRN_SUCCESS);

  return NULL;
}

static grn_rc
selector_prefix_rk_search(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                          int nargs, grn_obj **args,
                          grn_obj *res, grn_operator op)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *column;
  grn_obj *query;

  if ((nargs - 1) != 2) {
    ERR(GRN_INVALID_ARGUMENT,
        "prefix_rk_serach(): wrong number of arguments (%d for 2)", nargs - 1);
    rc = ctx->rc;
    goto exit;
  }

  column = args[1];
  query = args[2];

  if (!grn_obj_is_key_accessor(ctx, column)) {
    grn_obj inspected_column;
    GRN_TEXT_INIT(&inspected_column, 0);
    grn_inspect(ctx, &inspected_column, column);
    ERR(GRN_INVALID_ARGUMENT,
        "prefix_rk_serach(): column must be _key: %.*s",
        (int)GRN_TEXT_LEN(&inspected_column),
        GRN_TEXT_VALUE(&inspected_column));
    rc = ctx->rc;
    GRN_OBJ_FIN(ctx, &inspected_column);
    goto exit;
  }

  if (table->header.type != GRN_TABLE_PAT_KEY) {
    grn_obj inspected_table;
    GRN_TEXT_INIT(&inspected_table, 0);
    grn_inspect(ctx, &inspected_table, table);
    ERR(GRN_INVALID_ARGUMENT,
        "prefix_rk_serach(): table of _key must TABLE_PAT_KEY: %.*s",
        (int)GRN_TEXT_LEN(&inspected_table),
        GRN_TEXT_VALUE(&inspected_table));
    rc = ctx->rc;
    GRN_OBJ_FIN(ctx, &inspected_table);
    goto exit;
  }

  {
    grn_table_cursor *cursor;
    const void *max = NULL;
    unsigned int max_size = 0;
    int offset = 0;
    int limit = -1;

    cursor = grn_table_cursor_open(ctx, table,
                                   GRN_TEXT_VALUE(query),
                                   GRN_TEXT_LEN(query),
                                   max, max_size,
                                   offset, limit,
                                   GRN_CURSOR_PREFIX | GRN_CURSOR_RK);
    if (!cursor) {
      rc = ctx->rc;
      goto exit;
    }
    {
      grn_id record_id;
      while ((record_id = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
        grn_posting posting;
        posting.rid = record_id;
        posting.sid = 1;
        posting.pos = 0;
        posting.weight = 0;
        grn_ii_posting_add(ctx, &posting, (grn_hash *)res, op);
      }
    }
    grn_table_cursor_close(ctx, cursor);
    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  }

exit :
  return rc;
}

#define DEF_VAR(v,name_str) do {\
  (v).name = (name_str);\
  (v).name_size = GRN_STRLEN(name_str);\
  GRN_TEXT_INIT(&(v).value, 0);\
} while (0)

#define DEF_COMMAND(name, func, nvars, vars)\
  (grn_proc_create(ctx, (name), (sizeof(name) - 1),\
                   GRN_PROC_COMMAND, (func), NULL, NULL, (nvars), (vars)))

void
grn_db_init_builtin_query(grn_ctx *ctx)
{
  grn_expr_var vars[10];

  grn_proc_init_define_selector(ctx);
  grn_proc_init_select(ctx);

  DEF_VAR(vars[0], "values");
  DEF_VAR(vars[1], "table");
  DEF_VAR(vars[2], "columns");
  DEF_VAR(vars[3], "ifexists");
  DEF_VAR(vars[4], "input_type");
  DEF_VAR(vars[5], "each");
  DEF_COMMAND("load", proc_load, 6, vars);

  DEF_COMMAND("status", proc_status, 0, vars);

  grn_proc_init_table_list(ctx);

  DEF_VAR(vars[0], "table");
  DEF_COMMAND("column_list", proc_column_list, 1, vars);

  grn_proc_init_table_create(ctx);

  grn_proc_init_table_remove(ctx);

  grn_proc_init_table_rename(ctx);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_VAR(vars[2], "flags");
  DEF_VAR(vars[3], "type");
  DEF_VAR(vars[4], "source");
  DEF_COMMAND("column_create", proc_column_create, 5, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_COMMAND("column_remove", proc_column_remove, 2, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "name");
  DEF_VAR(vars[2], "new_name");
  DEF_COMMAND("column_rename", proc_column_rename, 3, vars);

  DEF_VAR(vars[0], "path");
  DEF_COMMAND(GRN_EXPR_MISSING_NAME, proc_missing, 1, vars);

  DEF_COMMAND("quit", proc_quit, 0, vars);

  DEF_VAR(vars[0], "mode");
  DEF_COMMAND("shutdown", proc_shutdown, 1, vars);

  grn_proc_init_clearlock(ctx);
  grn_proc_init_lock_clear(ctx);

  DEF_VAR(vars[0], "target_name");
  DEF_VAR(vars[1], "threshold");
  DEF_COMMAND("defrag", proc_defrag, 2, vars);

  DEF_VAR(vars[0], "level");
  DEF_COMMAND("log_level", proc_log_level, 1, vars);

  DEF_VAR(vars[0], "level");
  DEF_VAR(vars[1], "message");
  DEF_COMMAND("log_put", proc_log_put, 2, vars);

  DEF_COMMAND("log_reopen", proc_log_reopen, 0, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "key");
  DEF_VAR(vars[2], "id");
  DEF_VAR(vars[3], "filter");
  DEF_COMMAND("delete", proc_delete, 4, vars);

  DEF_VAR(vars[0], "max");
  DEF_COMMAND("cache_limit", proc_cache_limit, 1, vars);

  DEF_VAR(vars[0], "tables");
  DEF_VAR(vars[1], "dump_plugins");
  DEF_VAR(vars[2], "dump_schema");
  DEF_VAR(vars[3], "dump_records");
  DEF_VAR(vars[4], "dump_indexes");
  DEF_VAR(vars[5], "dump_configs");
  DEF_COMMAND("dump", proc_dump, 6, vars);

  /* Deprecated. Use "plugin_register" instead. */
  DEF_VAR(vars[0], "path");
  DEF_COMMAND("register", proc_register, 1, vars);

  DEF_VAR(vars[0], "obj");
  DEF_COMMAND("check", proc_check, 1, vars);

  DEF_VAR(vars[0], "target_name");
  DEF_VAR(vars[1], "table");
  DEF_COMMAND("truncate", proc_truncate, 2, vars);

  DEF_VAR(vars[0], "normalizer");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "flags");
  DEF_COMMAND("normalize", proc_normalize, 3, vars);

  DEF_VAR(vars[0], "tokenizer");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "normalizer");
  DEF_VAR(vars[3], "flags");
  DEF_VAR(vars[4], "mode");
  DEF_VAR(vars[5], "token_filters");
  DEF_COMMAND("tokenize", proc_tokenize, 6, vars);

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "string");
  DEF_VAR(vars[2], "flags");
  DEF_VAR(vars[3], "mode");
  DEF_COMMAND("table_tokenize", proc_table_tokenize, 4, vars);

  DEF_COMMAND("tokenizer_list", proc_tokenizer_list, 0, vars);

  DEF_COMMAND("normalizer_list", proc_normalizer_list, 0, vars);

  DEF_VAR(vars[0], "seed");
  grn_proc_create(ctx, "rand", -1, GRN_PROC_FUNCTION, func_rand,
                  NULL, NULL, 0, vars);

  grn_proc_create(ctx, "now", -1, GRN_PROC_FUNCTION, func_now,
                  NULL, NULL, 0, vars);

  grn_proc_create(ctx, "max", -1, GRN_PROC_FUNCTION, func_max,
                  NULL, NULL, 0, vars);
  grn_proc_create(ctx, "min", -1, GRN_PROC_FUNCTION, func_min,
                  NULL, NULL, 0, vars);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "geo_in_circle", -1, GRN_PROC_FUNCTION,
                                    func_geo_in_circle, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, grn_selector_geo_in_circle);

    selector_proc = grn_proc_create(ctx, "geo_in_rectangle", -1,
                                    GRN_PROC_FUNCTION,
                                    func_geo_in_rectangle, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, grn_selector_geo_in_rectangle);
  }

  grn_proc_create(ctx, "geo_distance", -1, GRN_PROC_FUNCTION,
                  func_geo_distance, NULL, NULL, 0, NULL);

  /* deprecated. */
  grn_proc_create(ctx, "geo_distance2", -1, GRN_PROC_FUNCTION,
                  func_geo_distance2, NULL, NULL, 0, NULL);

  /* deprecated. */
  grn_proc_create(ctx, "geo_distance3", -1, GRN_PROC_FUNCTION,
                  func_geo_distance3, NULL, NULL, 0, NULL);

  grn_proc_init_edit_distance(ctx);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "all_records", -1, GRN_PROC_FUNCTION,
                                    func_all_records, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_all_records);
  }

  /* experimental */
  grn_proc_init_snippet_html(ctx);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "query", -1, GRN_PROC_FUNCTION,
                                    func_query, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_query);
  }

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "sub_filter", -1, GRN_PROC_FUNCTION,
                                    NULL, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_sub_filter);
  }

  grn_proc_create(ctx, "html_untag", -1, GRN_PROC_FUNCTION,
                  func_html_untag, NULL, NULL, 0, NULL);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "between", -1, GRN_PROC_FUNCTION,
                                    func_between, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_between);
  }

  grn_proc_init_highlight_html(ctx);
  grn_proc_init_highlight_full(ctx);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "in_values", -1, GRN_PROC_FUNCTION,
                                    func_in_values, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_in_values);
  }

  DEF_VAR(vars[0], "table");
  DEF_VAR(vars[1], "column");
  DEF_VAR(vars[2], "min");
  DEF_VAR(vars[3], "min_border");
  DEF_VAR(vars[4], "max");
  DEF_VAR(vars[5], "max_border");
  DEF_VAR(vars[6], "offset");
  DEF_VAR(vars[7], "limit");
  DEF_VAR(vars[8], "filter");
  DEF_VAR(vars[9], "output_columns");
  DEF_COMMAND("range_filter", proc_range_filter, 10, vars);

  DEF_VAR(vars[0], "id");
  DEF_COMMAND("request_cancel", proc_request_cancel, 1, vars);

  DEF_VAR(vars[0], "name");
  DEF_COMMAND("plugin_register", proc_plugin_register, 1, vars);

  DEF_VAR(vars[0], "name");
  DEF_COMMAND("plugin_unregister", proc_plugin_unregister, 1, vars);

  DEF_VAR(vars[0], "target_name");
  DEF_VAR(vars[1], "recursive");
  DEF_COMMAND("io_flush", proc_io_flush, 2, vars);

  grn_proc_init_object_exist(ctx);

  DEF_VAR(vars[0], "max");
  DEF_COMMAND("thread_limit", proc_thread_limit, 1, vars);

  DEF_COMMAND("database_unmap", proc_database_unmap, 0, vars);

  DEF_VAR(vars[0], "from_table");
  DEF_VAR(vars[1], "from_name");
  DEF_VAR(vars[2], "to_table");
  DEF_VAR(vars[3], "to_name");
  DEF_COMMAND("column_copy", proc_column_copy, 4, vars);

  grn_proc_init_schema(ctx);

  DEF_VAR(vars[0], "target_name");
  DEF_COMMAND("reindex", proc_reindex, 1, vars);

  {
    grn_obj *selector_proc;

    selector_proc = grn_proc_create(ctx, "prefix_rk_search", -1,
                                    GRN_PROC_FUNCTION,
                                    NULL, NULL, NULL, 0, NULL);
    grn_proc_set_selector(ctx, selector_proc, selector_prefix_rk_search);
  }

  grn_proc_init_config_get(ctx);
  grn_proc_init_config_set(ctx);
  grn_proc_init_config_delete(ctx);

  grn_proc_init_lock_acquire(ctx);
  grn_proc_init_lock_release(ctx);

  grn_proc_init_object_inspect(ctx);

  grn_proc_init_fuzzy_search(ctx);

  grn_proc_init_object_remove(ctx);

  grn_proc_init_snippet(ctx);
  grn_proc_init_highlight(ctx);
}
