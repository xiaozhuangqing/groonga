/* -*- c-basic-offset: 2; coding: utf-8 -*- */
/*
  Copyright (C) 2008  Kouhei Sutou <kou@cozmixng.org>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <hash.h>
#include <gcutter.h>
#include "../lib/grn-assertions.h"

void data_read_write(void);
void test_read_write(gconstpointer *data);

#define N_THREADS 10

static grn_ctx *contexts[N_THREADS];
static grn_hash *hashes[N_THREADS];

void
startup(void)
{
  int i;

  for (i = 0; i < N_THREADS; i++) {
    contexts[i] = NULL;
  }
}

void
shutdown(void)
{
  int i;

  for (i = 0; i < N_THREADS; i++) {
    grn_ctx *context;

    context = contexts[i];
    if (context) {
      grn_hash *hash;

      hash = hashes[i];
      if (hash)
        grn_hash_close(context, hash);
      grn_ctx_fin(context);
    }
  }
}


void
data_read_write(void)
{
  gint i;
  const gchar *n_processes, *process_number, *thread_type;

  n_processes = g_getenv(GRN_TEST_ENV_N_PROCESSES);
  if (!n_processes)
    n_processes = "?";

  process_number = g_getenv(GRN_TEST_ENV_PROCESS_NUMBER);
  if (!process_number)
    process_number = "?";

  if (g_getenv(GRN_TEST_ENV_MULTI_THREAD))
    thread_type = "multi thread";
  else
    thread_type = "single thread";

  for (i = 0; i < N_THREADS; i++) {
    cut_add_data(cut_take_printf("%s process(es)[%s] - %s[%d]",
                                 n_processes, process_number, thread_type, i),
                 GINT_TO_POINTER(i), NULL);
  }
}

void
test_read_write(gconstpointer *data)
{
  gint i, key;
  grn_search_flags flags;
  grn_ctx *context;
  grn_hash *hash;
  const gchar *path;
  const gchar *value_string;
  gint process_number = 0;
  const gchar *process_number_string;
  void *value;
  grn_id id = GRN_ID_NIL;
  grn_rc rc;

  i = GPOINTER_TO_INT(data);
  process_number_string = g_getenv(GRN_TEST_ENV_PROCESS_NUMBER);
  if (process_number_string)
    process_number = atoi(process_number_string);

  key = i + process_number * N_THREADS;

  rc = grn_ctx_init(contexts[i], GRN_CTX_USE_QL);
  cut_set_message("context: %d (%d)", i, process_number);
  grn_test_assert(rc);
  context = contexts[i];

  path = g_getenv(GRN_TEST_ENV_HASH_PATH);
  cut_assert_not_null(path);
  hashes[i] = grn_hash_open(context, path);
  cut_assert_not_null(hashes[i], "hash: %d (%d)", i, process_number);
  hash = hashes[i];

  flags = 0;
  cut_set_message("lookup - fail: %d (%d:%d)", key, i, process_number);
  grn_test_assert_nil(grn_hash_lookup(context, hash, &key, sizeof(key),
                                      &value, &flags));

  value_string = cut_take_printf("value: %d (%d:%d)", key, i, process_number);
  flags = GRN_TABLE_ADD;
  rc = grn_io_lock(context, hash->io, -1);
  if (rc != GRN_SUCCESS)
    grn_test_assert(rc);
  id = grn_hash_lookup(context, hash, &key, sizeof(key), &value, &flags);
  grn_io_unlock(hash->io);
  grn_test_assert_not_nil(id);
  cut_assert_equal_uint(GRN_TABLE_ADDED, flags & GRN_TABLE_ADDED);
  strcpy(value, value_string);

  flags = 0;
  value = NULL;
  id = grn_hash_lookup(context, hash, &key, sizeof(key), &value, &flags);
  cut_set_message("lookup - success: %d (%d:%d)", key, i, process_number);
  grn_test_assert_not_nil(id);
  cut_assert_equal_string(value_string, value);

  hashes[i] = NULL;
  grn_test_assert(grn_hash_close(context, hash));

  contexts[i] = NULL;
  grn_test_assert(grn_ctx_fin(context));
}
