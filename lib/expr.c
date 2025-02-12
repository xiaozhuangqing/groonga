/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2016 Brazil

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
#include "grn.h"
#include "grn_db.h"
#include "grn_ctx_impl.h"
#include <string.h>
#include "grn_ii.h"
#include "grn_geo.h"
#include "grn_expr.h"
#include "grn_expr_code.h"
#include "grn_scanner.h"
#include "grn_util.h"
#include "grn_report.h"
#include "grn_mrb.h"
#include "mrb/mrb_expr.h"

#ifdef GRN_WITH_ONIGMO
# define GRN_SUPPORT_REGEXP
#endif

#ifdef GRN_SUPPORT_REGEXP
# include "grn_normalizer.h"
# include <oniguruma.h>
#endif

grn_obj *
grn_expr_alloc(grn_ctx *ctx, grn_obj *expr, grn_id domain, grn_obj_flags flags)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  if (e) {
    if (e->values_curr >= e->values_size) {
      // todo : expand values.
      ERR(GRN_NO_MEMORY_AVAILABLE, "no more e->values");
      return NULL;
    }
    res = &e->values[e->values_curr++];
    if (e->values_curr > e->values_tail) { e->values_tail = e->values_curr; }
    grn_obj_reinit(ctx, res, domain, flags);
  }
  return res;
}

grn_hash *
grn_expr_get_vars(grn_ctx *ctx, grn_obj *expr, unsigned int *nvars)
{
  grn_hash *vars = NULL;
  if (expr->header.type == GRN_PROC || expr->header.type == GRN_EXPR) {
    grn_id id = DB_OBJ(expr)->id;
    grn_expr *e = (grn_expr *)expr;
    int added = 0;
    grn_hash **vp;
    if (grn_hash_add(ctx, ctx->impl->expr_vars, &id, sizeof(grn_id), (void **)&vp, &added)) {
      if (!*vp) {
        *vp = grn_hash_create(ctx, NULL, GRN_TABLE_MAX_KEY_SIZE, sizeof(grn_obj),
                              GRN_OBJ_KEY_VAR_SIZE|GRN_OBJ_TEMPORARY|GRN_HASH_TINY);
        if (*vp) {
          uint32_t i;
          grn_obj *value;
          grn_expr_var *v;
          for (v = e->vars, i = e->nvars; i; v++, i--) {
            grn_hash_add(ctx, *vp, v->name, v->name_size, (void **)&value, &added);
            GRN_OBJ_INIT(value, v->value.header.type, 0, v->value.header.domain);
            GRN_TEXT_PUT(ctx, value, GRN_TEXT_VALUE(&v->value), GRN_TEXT_LEN(&v->value));
          }
        }
      }
      vars = *vp;
    }
  }
  *nvars = vars ? GRN_HASH_SIZE(vars) : 0;
  return vars;
}

grn_rc
grn_expr_clear_vars(grn_ctx *ctx, grn_obj *expr)
{
  if (expr->header.type == GRN_PROC || expr->header.type == GRN_EXPR) {
    grn_hash **vp;
    grn_id eid, id = DB_OBJ(expr)->id;
    if ((eid = grn_hash_get(ctx, ctx->impl->expr_vars, &id, sizeof(grn_id), (void **)&vp))) {
      if (*vp) {
        grn_obj *value;
        GRN_HASH_EACH(ctx, *vp, i, NULL, NULL, (void **)&value, {
          GRN_OBJ_FIN(ctx, value);
        });
        grn_hash_close(ctx, *vp);
      }
      grn_hash_delete_by_id(ctx, ctx->impl->expr_vars, eid, NULL);
    }
  }
  return ctx->rc;
}

grn_obj *
grn_proc_get_info(grn_ctx *ctx, grn_user_data *user_data,
                  grn_expr_var **vars, unsigned int *nvars, grn_obj **caller)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  if (caller) { *caller = pctx->caller; }
  if (pctx->proc) {
    if (vars) {
      *vars = pctx->proc->vars;
   // *vars = grn_expr_get_vars(ctx, (grn_obj *)pctx->proc, nvars);
    }
    if (nvars) { *nvars = pctx->proc->nvars; }
  } else {
    if (vars) { *vars = NULL; }
    if (nvars) { *nvars = 0; }
  }
  return (grn_obj *)pctx->proc;
}

grn_obj *
grn_proc_get_vars(grn_ctx *ctx, grn_user_data *user_data)
{
  uint32_t n;
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  if (pctx->proc) {
    return (grn_obj *)grn_expr_get_vars(ctx, (grn_obj *)pctx->proc, &n);
  } else {
    return NULL;
  }
}

grn_obj *
grn_proc_get_var(grn_ctx *ctx, grn_user_data *user_data, const char *name, unsigned int name_size)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->proc ? grn_expr_get_var(ctx, (grn_obj *)pctx->proc, name, name_size) : NULL;
}

grn_obj *
grn_proc_get_var_by_offset(grn_ctx *ctx, grn_user_data *user_data, unsigned int offset)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->proc ? grn_expr_get_var_by_offset(ctx, (grn_obj *)pctx->proc, offset) : NULL;
}

grn_obj *
grn_proc_get_or_add_var(grn_ctx *ctx, grn_user_data *user_data,
                        const char *name, unsigned int name_size)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->proc ? grn_expr_get_or_add_var(ctx, (grn_obj *)pctx->proc, name, name_size) : NULL;
}

grn_obj *
grn_proc_alloc(grn_ctx *ctx, grn_user_data *user_data, grn_id domain, grn_obj_flags flags)
{
  grn_proc_ctx *pctx = (grn_proc_ctx *)user_data;
  return pctx->caller ? grn_expr_alloc(ctx, (grn_obj *)pctx->caller, domain, flags) : NULL;
}

grn_proc_type
grn_proc_get_type(grn_ctx *ctx, grn_obj *proc)
{
  grn_proc *proc_ = (grn_proc *)proc;
  return proc_ ? proc_->type : GRN_PROC_INVALID;
}

grn_rc
grn_proc_set_selector(grn_ctx *ctx, grn_obj *proc, grn_selector_func selector)
{
  grn_proc *proc_ = (grn_proc *)proc;
  if (!grn_obj_is_function_proc(ctx, proc)) {
    return GRN_INVALID_ARGUMENT;
  }
  proc_->selector = selector;
  return GRN_SUCCESS;
}

/* grn_expr */

grn_obj *
grn_ctx_pop(grn_ctx *ctx)
{
  if (ctx && ctx->impl && ctx->impl->stack_curr) {
    return ctx->impl->stack[--ctx->impl->stack_curr];
  }
  return NULL;
}

grn_rc
grn_ctx_push(grn_ctx *ctx, grn_obj *obj)
{
  if (ctx && ctx->impl && ctx->impl->stack_curr < GRN_STACK_SIZE) {
    ctx->impl->stack[ctx->impl->stack_curr++] = obj;
    return GRN_SUCCESS;
  }
  return GRN_STACK_OVER_FLOW;
}

grn_obj *
grn_expr_alloc_const(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;
  uint32_t id = e->nconsts % GRN_EXPR_CONST_BLK_SIZE;
  uint32_t blk_id = e->nconsts / GRN_EXPR_CONST_BLK_SIZE;

  if (id == 0) {
    uint32_t nblks = blk_id + 1;
    grn_obj **blks = (grn_obj **)GRN_REALLOC(e->const_blks,
                                             sizeof(grn_obj *) * nblks);
    if (!blks) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "realloc failed");
      return NULL;
    }
    e->const_blks = blks;
    blks[blk_id] = GRN_MALLOCN(grn_obj, GRN_EXPR_CONST_BLK_SIZE);
    if (!blks[blk_id]) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "malloc failed");
      return NULL;
    }
  }
  e->nconsts++;
  return &e->const_blks[blk_id][id];
}

void
grn_obj_pack(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  grn_text_benc(ctx, buf, obj->header.type);
  if (GRN_DB_OBJP(obj)) {
    grn_text_benc(ctx, buf, DB_OBJ(obj)->id);
  } else {
    // todo : support vector, query, accessor, snip..
    uint32_t vs = GRN_BULK_VSIZE(obj);
    grn_text_benc(ctx, buf, obj->header.domain);
    grn_text_benc(ctx, buf, vs);
    if (vs) { GRN_TEXT_PUT(ctx, buf, GRN_BULK_HEAD(obj), vs); }
  }
}

const uint8_t *
grn_obj_unpack(grn_ctx *ctx, const uint8_t *p, const uint8_t *pe, uint8_t type, uint8_t flags, grn_obj *obj)
{
  grn_id domain;
  uint32_t vs;
  GRN_B_DEC(domain, p);
  GRN_OBJ_INIT(obj, type, flags, domain);
  GRN_B_DEC(vs, p);
  if (pe < p + vs) {
    ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
    return p;
  }
  grn_bulk_write(ctx, obj, (const char *)p, vs);
  return p + vs;
}

typedef enum {
  GRN_EXPR_PACK_TYPE_NULL     = 0,
  GRN_EXPR_PACK_TYPE_VARIABLE = 1,
  GRN_EXPR_PACK_TYPE_OTHERS   = 2
} grn_expr_pack_type;

void
grn_expr_pack(grn_ctx *ctx, grn_obj *buf, grn_obj *expr)
{
  grn_expr_code *c;
  grn_expr_var *v;
  grn_expr *e = (grn_expr *)expr;
  uint32_t i, j;
  grn_text_benc(ctx, buf, e->nvars);
  for (i = e->nvars, v = e->vars; i; i--, v++) {
    grn_text_benc(ctx, buf, v->name_size);
    if (v->name_size) { GRN_TEXT_PUT(ctx, buf, v->name, v->name_size); }
    grn_obj_pack(ctx, buf, &v->value);
  }
  i = e->codes_curr;
  grn_text_benc(ctx, buf, i);
  for (c = e->codes; i; i--, c++) {
    grn_text_benc(ctx, buf, c->op);
    grn_text_benc(ctx, buf, c->nargs);
    if (!c->value) {
      grn_text_benc(ctx, buf, GRN_EXPR_PACK_TYPE_NULL);
    } else {
      for (j = 0, v = e->vars; j < e->nvars; j++, v++) {
        if (&v->value == c->value) {
          grn_text_benc(ctx, buf, GRN_EXPR_PACK_TYPE_VARIABLE);
          grn_text_benc(ctx, buf, j);
          break;
        }
      }
      if (j == e->nvars) {
        grn_text_benc(ctx, buf, GRN_EXPR_PACK_TYPE_OTHERS);
        grn_obj_pack(ctx, buf, c->value);
      }
    }
  }
}

const uint8_t *
grn_expr_unpack(grn_ctx *ctx, const uint8_t *p, const uint8_t *pe, grn_obj *expr)
{
  grn_obj *v;
  grn_expr_pack_type type;
  uint32_t i, n, ns;
  grn_expr_code *code;
  grn_expr *e = (grn_expr *)expr;
  GRN_B_DEC(n, p);
  for (i = 0; i < n; i++) {
    uint32_t object_type;
    GRN_B_DEC(ns, p);
    v = grn_expr_add_var(ctx, expr, ns ? (const char *)p : NULL, ns);
    p += ns;
    GRN_B_DEC(object_type, p);
    if (GRN_TYPE <= object_type && object_type <= GRN_COLUMN_INDEX) { /* error */ }
    p = grn_obj_unpack(ctx, p, pe, object_type, 0, v);
    if (pe < p) {
      ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
      return p;
    }
  }
  GRN_B_DEC(n, p);
  /* confirm e->codes_size >= n */
  e->codes_curr = n;
  for (i = 0, code = e->codes; i < n; i++, code++) {
    GRN_B_DEC(code->op, p);
    GRN_B_DEC(code->nargs, p);
    GRN_B_DEC(type, p);
    switch (type) {
    case GRN_EXPR_PACK_TYPE_NULL :
      code->value = NULL;
      break;
    case GRN_EXPR_PACK_TYPE_VARIABLE :
      {
        uint32_t offset;
        GRN_B_DEC(offset, p);
        code->value = &e->vars[i].value;
      }
      break;
    case GRN_EXPR_PACK_TYPE_OTHERS :
      {
        uint32_t object_type;
        GRN_B_DEC(object_type, p);
        if (GRN_TYPE <= object_type && object_type <= GRN_COLUMN_INDEX) {
          grn_id id;
          GRN_B_DEC(id, p);
          code->value = grn_ctx_at(ctx, id);
        } else {
          if (!(v = grn_expr_alloc_const(ctx, expr))) { return NULL; }
          p = grn_obj_unpack(ctx, p, pe, object_type, GRN_OBJ_EXPRCONST, v);
          code->value = v;
        }
      }
      break;
    }
    if (pe < p) {
      ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
      return p;
    }
  }
  return p;
}

grn_obj *
grn_expr_open(grn_ctx *ctx, grn_obj_spec *spec, const uint8_t *p, const uint8_t *pe)
{
  grn_expr *expr = NULL;
  if ((expr = GRN_MALLOCN(grn_expr, 1))) {
    int size = GRN_STACK_SIZE;
    expr->const_blks = NULL;
    expr->nconsts = 0;
    GRN_TEXT_INIT(&expr->name_buf, 0);
    GRN_TEXT_INIT(&expr->dfi, 0);
    GRN_PTR_INIT(&expr->objs, GRN_OBJ_VECTOR, GRN_ID_NIL);
    expr->vars = NULL;
    expr->nvars = 0;
    GRN_DB_OBJ_SET_TYPE(expr, GRN_EXPR);
    if ((expr->values = GRN_MALLOCN(grn_obj, size))) {
      int i;
      for (i = 0; i < size; i++) {
        GRN_OBJ_INIT(&expr->values[i], GRN_BULK, GRN_OBJ_EXPRVALUE, GRN_ID_NIL);
      }
      expr->values_curr = 0;
      expr->values_tail = 0;
      expr->values_size = size;
      if ((expr->codes = GRN_MALLOCN(grn_expr_code, size))) {
        expr->codes_curr = 0;
        expr->codes_size = size;
        expr->obj.header = spec->header;
        if (grn_expr_unpack(ctx, p, pe, (grn_obj *)expr) == pe) {
          goto exit;
        } else {
          ERR(GRN_INVALID_FORMAT, "benced image is corrupt");
        }
        GRN_FREE(expr->codes);
      }
      GRN_FREE(expr->values);
    }
    GRN_FREE(expr);
    expr = NULL;
  }
exit :
  return (grn_obj *)expr;
}

/* Pass ownership of `obj` to `expr`. */
void
grn_expr_take_obj(grn_ctx *ctx, grn_obj *expr, grn_obj *obj)
{
  grn_expr *e = (grn_expr *)expr;
  GRN_PTR_PUT(ctx, &(e->objs), obj);
}

/* data flow info */
typedef struct {
  grn_expr_code *code;
  grn_id domain;
  unsigned char type;
} grn_expr_dfi;

static grn_expr_dfi *
grn_expr_dfi_pop(grn_expr *expr)
{
  if (GRN_BULK_VSIZE(&expr->dfi) >= sizeof(grn_expr_dfi)) {
    grn_expr_dfi *dfi;
    GRN_BULK_INCR_LEN(&expr->dfi, -sizeof(grn_expr_dfi));
    dfi = (grn_expr_dfi *)GRN_BULK_CURR(&expr->dfi);
    expr->code0 = dfi->code;
    return dfi;
  } else {
    expr->code0 = NULL;
    return NULL;
  }
}

static void
grn_expr_dfi_put(grn_ctx *ctx, grn_expr *expr, uint8_t type, grn_id domain,
                 grn_expr_code *code)
{
  grn_expr_dfi dfi;
  dfi.type = type;
  dfi.domain = domain;
  dfi.code = code;
  if (expr->code0) {
    expr->code0->modify = code ? (code - expr->code0) : 0;
  }
  grn_bulk_write(ctx, &expr->dfi, (char *)&dfi, sizeof(grn_expr_dfi));
  expr->code0 = NULL;
}

grn_obj *
grn_expr_create(grn_ctx *ctx, const char *name, unsigned int name_size)
{
  grn_id id;
  grn_obj *db;
  grn_expr *expr = NULL;
  if (!ctx || !ctx->impl || !(db = ctx->impl->db)) {
    ERR(GRN_INVALID_ARGUMENT, "db not initialized");
    return NULL;
  }
  if (name_size) {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "[expr][create] named expression isn't implemented yet");
    return NULL;
  }
  GRN_API_ENTER;
  if (grn_db_check_name(ctx, name, name_size)) {
    GRN_DB_CHECK_NAME_ERR("[expr][create]", name, name_size);
    GRN_API_RETURN(NULL);
  }
  if (!GRN_DB_P(db)) {
    ERR(GRN_INVALID_ARGUMENT, "named expr is not supported");
    GRN_API_RETURN(NULL);
  }
  id = grn_obj_register(ctx, db, name, name_size);
  if (id && (expr = GRN_MALLOCN(grn_expr, 1))) {
    int size = GRN_STACK_SIZE;
    expr->const_blks = NULL;
    expr->nconsts = 0;
    GRN_TEXT_INIT(&expr->name_buf, 0);
    GRN_TEXT_INIT(&expr->dfi, 0);
    GRN_PTR_INIT(&expr->objs, GRN_OBJ_VECTOR, GRN_ID_NIL);
    expr->code0 = NULL;
    expr->vars = NULL;
    expr->nvars = 0;
    expr->cacheable = 1;
    expr->taintable = 0;
    expr->values_curr = 0;
    expr->values_tail = 0;
    expr->values_size = size;
    expr->codes_curr = 0;
    expr->codes_size = size;
    GRN_DB_OBJ_SET_TYPE(expr, GRN_EXPR);
    expr->obj.header.domain = GRN_ID_NIL;
    expr->obj.range = GRN_ID_NIL;
    if (!grn_db_obj_init(ctx, db, id, DB_OBJ(expr))) {
      if ((expr->values = GRN_MALLOCN(grn_obj, size))) {
        int i;
        for (i = 0; i < size; i++) {
          GRN_OBJ_INIT(&expr->values[i], GRN_BULK, GRN_OBJ_EXPRVALUE, GRN_ID_NIL);
        }
        if ((expr->codes = GRN_MALLOCN(grn_expr_code, size))) {
          goto exit;
        }
        GRN_FREE(expr->values);
      }
    }
    GRN_FREE(expr);
    expr = NULL;
  }
exit :
  GRN_API_RETURN((grn_obj *)expr);
}

#define GRN_PTR_POP(obj,value) do {\
  if (GRN_BULK_VSIZE(obj) >= sizeof(grn_obj *)) {\
    GRN_BULK_INCR_LEN((obj), -(sizeof(grn_obj *)));\
    value = *(grn_obj **)(GRN_BULK_CURR(obj));\
  } else {\
    value = NULL;\
  }\
} while (0)

grn_rc
grn_expr_close(grn_ctx *ctx, grn_obj *expr)
{
  uint32_t i, j;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  /*
  if (e->obj.header.domain) {
    grn_hash_delete(ctx, ctx->impl->qe, &e->obj.header.domain, sizeof(grn_id), NULL);
  }
  */
  grn_expr_clear_vars(ctx, expr);
  if (e->const_blks) {
    uint32_t nblks = e->nconsts + GRN_EXPR_CONST_BLK_SIZE - 1;
    nblks /= GRN_EXPR_CONST_BLK_SIZE;
    for (i = 0; i < nblks; i++) {
      uint32_t end;
      if (i < nblks - 1) {
        end = GRN_EXPR_CONST_BLK_SIZE;
      } else {
        end = ((e->nconsts - 1) % GRN_EXPR_CONST_BLK_SIZE) + 1;
      }
      for (j = 0; j < end; j++) {
        grn_obj *const_obj = &e->const_blks[i][j];
        grn_obj_close(ctx, const_obj);
      }
      GRN_FREE(e->const_blks[i]);
    }
    GRN_FREE(e->const_blks);
  }
  grn_obj_close(ctx, &e->name_buf);
  grn_obj_close(ctx, &e->dfi);
  for (;;) {
    grn_obj *obj;
    GRN_PTR_POP(&e->objs, obj);
    if (obj) {
#ifdef USE_MEMORY_DEBUG
      grn_obj_unlink(ctx, obj);
#else
      if (obj->header.type) {
        if (obj->header.type == GRN_TABLE_HASH_KEY &&
            ((grn_hash *)obj)->value_size == sizeof(grn_obj)) {
          grn_obj *value;
          GRN_HASH_EACH(ctx, (grn_hash *)obj, id, NULL, NULL, (void **)&value, {
            GRN_OBJ_FIN(ctx, value);
          });
        }
        grn_obj_unlink(ctx, obj);
      } else {
        GRN_LOG(ctx, GRN_LOG_WARNING, "GRN_VOID object is tried to be unlinked");
      }
#endif
    } else { break; }
  }
  grn_obj_close(ctx, &e->objs);
  for (i = 0; i < e->nvars; i++) {
    grn_obj_close(ctx, &e->vars[i].value);
  }
  if (e->vars) { GRN_FREE(e->vars); }
  for (i = 0; i < e->values_tail; i++) {
    grn_obj_close(ctx, &e->values[i]);
  }
  GRN_FREE(e->values);
  GRN_FREE(e->codes);
  GRN_FREE(e);
  GRN_API_RETURN(ctx->rc);
}

grn_obj *
grn_expr_add_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t i;
  char *p;
  grn_expr_var *v;
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (DB_OBJ(expr)->id & GRN_OBJ_TMP_OBJECT) {
    res = grn_expr_get_or_add_var(ctx, expr, name, name_size);
  } else {
    if (!e->vars) {
      if (!(e->vars = GRN_MALLOCN(grn_expr_var, GRN_STACK_SIZE))) {
        ERR(GRN_NO_MEMORY_AVAILABLE, "malloc failed");
      }
    }
    if (e->vars && e->nvars < GRN_STACK_SIZE) {
      v = e->vars + e->nvars++;
      if (name_size) {
        GRN_TEXT_PUT(ctx, &e->name_buf, name, name_size);
      } else {
        uint32_t ol = GRN_TEXT_LEN(&e->name_buf);
        GRN_TEXT_PUTC(ctx, &e->name_buf, '$');
        grn_text_itoa(ctx, &e->name_buf, e->nvars);
        name_size = GRN_TEXT_LEN(&e->name_buf) - ol;
      }
      v->name_size = name_size;
      res = &v->value;
      GRN_VOID_INIT(res);
      for (i = e->nvars, p = GRN_TEXT_VALUE(&e->name_buf), v = e->vars; i; i--, v++) {
        v->name = p;
        p += v->name_size;
      }
    }
  }
  GRN_API_RETURN(res);
}

grn_obj *
grn_expr_get_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) { grn_hash_get(ctx, vars, name, name_size, (void **)&res); }
  return res;
}

grn_obj *
grn_expr_get_or_add_var(grn_ctx *ctx, grn_obj *expr, const char *name, unsigned int name_size)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) {
    int added = 0;
    char name_buf[16];
    if (!name_size) {
      char *rest;
      name_buf[0] = '$';
      grn_itoa((int)GRN_HASH_SIZE(vars) + 1, name_buf + 1, name_buf + 16, &rest);
      name_size = rest - name_buf;
      name = name_buf;
    }
    grn_hash_add(ctx, vars, name, name_size, (void **)&res, &added);
    if (added) { GRN_TEXT_INIT(res, 0); }
  }
  return res;
}

grn_obj *
grn_expr_get_var_by_offset(grn_ctx *ctx, grn_obj *expr, unsigned int offset)
{
  uint32_t n;
  grn_obj *res = NULL;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &n);
  if (vars) { res = (grn_obj *)grn_hash_get_value_(ctx, vars, offset + 1, &n); }
  return res;
}

#define EXPRVP(x) ((x)->header.impl_flags & GRN_OBJ_EXPRVALUE)

#define CONSTP(obj) ((obj) && ((obj)->header.impl_flags & GRN_OBJ_EXPRCONST))

#define PUSH_CODE(e,o,v,n,c) do {\
  (c) = &(e)->codes[e->codes_curr++];\
  (c)->value = (v);\
  (c)->nargs = (n);\
  (c)->op = (o);\
  (c)->flags = 0;\
  (c)->modify = 0;\
} while (0)

#define APPEND_UNARY_MINUS_OP(e) do {                           \
  grn_expr_code *code_;                                         \
  grn_id domain;                                                \
  unsigned char type;                                           \
  grn_obj *x;                                                   \
  dfi = grn_expr_dfi_pop(e);                                    \
  code_ = dfi->code;                                            \
  domain = dfi->domain;                                         \
  type = dfi->type;                                             \
  x = code_->value;                                             \
  if (CONSTP(x)) {                                              \
    switch (domain) {                                           \
    case GRN_DB_INT32:                                          \
      {                                                         \
        int value;                                              \
        value = GRN_INT32_VALUE(x);                             \
        if (value == (int)0x80000000) {                         \
          domain = GRN_DB_INT64;                                \
          x->header.domain = domain;                            \
          GRN_INT64_SET(ctx, x, -((long long int)value));       \
        } else {                                                \
          GRN_INT32_SET(ctx, x, -value);                        \
        }                                                       \
      }                                                         \
      break;                                                    \
    case GRN_DB_UINT32:                                         \
      {                                                         \
        unsigned int value;                                     \
        value = GRN_UINT32_VALUE(x);                            \
        if (value > (unsigned int)0x80000000) {                 \
          domain = GRN_DB_INT64;                                \
          x->header.domain = domain;                            \
          GRN_INT64_SET(ctx, x, -((long long int)value));       \
        } else {                                                \
          domain = GRN_DB_INT32;                                \
          x->header.domain = domain;                            \
          GRN_INT32_SET(ctx, x, -((int)value));                 \
        }                                                       \
      }                                                         \
      break;                                                    \
    case GRN_DB_INT64:                                          \
      GRN_INT64_SET(ctx, x, -GRN_INT64_VALUE(x));               \
      break;                                                    \
    case GRN_DB_FLOAT:                                          \
      GRN_FLOAT_SET(ctx, x, -GRN_FLOAT_VALUE(x));               \
      break;                                                    \
    default:                                                    \
      PUSH_CODE(e, op, obj, nargs, code);                       \
      break;                                                    \
    }                                                           \
  } else {                                                      \
    PUSH_CODE(e, op, obj, nargs, code);                         \
  }                                                             \
  grn_expr_dfi_put(ctx, e, type, domain, code_);                \
} while (0)

#define PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code) do { \
  PUSH_CODE(e, op, obj, nargs, code);                           \
  {                                                             \
    int i = nargs;                                              \
    while (i--) {                                               \
      dfi = grn_expr_dfi_pop(e);                                \
    }                                                           \
  }                                                             \
  grn_expr_dfi_put(ctx, e, type, domain, code);                 \
} while (0)

static void
grn_expr_append_obj_resolve_const(grn_ctx *ctx,
                                  grn_obj *obj,
                                  grn_id to_domain)
{
  grn_obj dest;

  GRN_OBJ_INIT(&dest, GRN_BULK, 0, to_domain);
  if (!grn_obj_cast(ctx, obj, &dest, GRN_FALSE)) {
    grn_obj_reinit(ctx, obj, to_domain, 0);
    grn_bulk_write(ctx, obj, GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest));
  }
  GRN_OBJ_FIN(ctx, &dest);
}

grn_obj *
grn_expr_append_obj(grn_ctx *ctx, grn_obj *expr, grn_obj *obj, grn_operator op, int nargs)
{
  uint8_t type = GRN_VOID;
  grn_id domain = GRN_ID_NIL;
  grn_expr_dfi *dfi;
  grn_expr_code *code;
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (e->codes_curr >= e->codes_size) {
    grn_expr_dfi *dfis = (grn_expr_dfi *)GRN_BULK_HEAD(&e->dfi);
    size_t i, n_dfis = GRN_BULK_VSIZE(&e->dfi) / sizeof(grn_expr_dfi);
    uint32_t new_codes_size = e->codes_size * 2;
    size_t n_bytes = sizeof(grn_expr_code) * new_codes_size;
    grn_expr_code *new_codes = (grn_expr_code *)GRN_MALLOC(n_bytes);
    if (!new_codes) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "stack is full");
      goto exit;
    }
    grn_memcpy(new_codes, e->codes, sizeof(grn_expr_code) * e->codes_size);
    if (e->code0 >= e->codes && e->code0 < e->codes + e->codes_size) {
      e->code0 = new_codes + (e->code0 - e->codes);
    }
    for (i = 0; i < n_dfis; i++) {
      if (dfis[i].code >= e->codes && dfis[i].code < e->codes + e->codes_size) {
        dfis[i].code = new_codes + (dfis[i].code - e->codes);
      }
    }
    GRN_FREE(e->codes);
    e->codes = new_codes;
    e->codes_size = new_codes_size;
  }
  {
    switch (op) {
    case GRN_OP_PUSH :
      if (obj) {
        PUSH_CODE(e, op, obj, nargs, code);
        grn_expr_dfi_put(ctx, e, obj->header.type, GRN_OBJ_GET_DOMAIN(obj),
                         code);
      } else {
        ERR(GRN_INVALID_ARGUMENT, "obj not assigned for GRN_OP_PUSH");
        goto exit;
      }
      break;
    case GRN_OP_NOP :
      /* nop */
      break;
    case GRN_OP_POP :
      if (obj) {
        ERR(GRN_INVALID_ARGUMENT, "obj assigned for GRN_OP_POP");
        goto exit;
      } else {
        PUSH_CODE(e, op, obj, nargs, code);
        dfi = grn_expr_dfi_pop(e);
      }
      break;
    case GRN_OP_CALL :
      {
        grn_obj *proc = NULL;
        if (e->codes_curr - nargs > 0) {
          int i;
          grn_expr_code *code;
          code = &(e->codes[e->codes_curr - 1]);
          for (i = 0; i < nargs; i++) {
            int rest_n_codes = 1;
            while (rest_n_codes > 0) {
              rest_n_codes += code->nargs;
              if (code->value) {
                rest_n_codes--;
              }
              rest_n_codes--;
              code--;
            }
          }
          proc = code->value;
        }
        if (!proc) {
          ERR(GRN_INVALID_ARGUMENT, "invalid function call expression");
          goto exit;
        }
        if (!(grn_obj_is_function_proc(ctx, proc) ||
              grn_obj_is_scorer_proc(ctx, proc))) {
          grn_obj buffer;

          GRN_TEXT_INIT(&buffer, 0);
          switch (proc->header.type) {
          case GRN_TABLE_HASH_KEY:
          case GRN_TABLE_PAT_KEY:
          case GRN_TABLE_NO_KEY:
          case GRN_COLUMN_FIX_SIZE:
          case GRN_COLUMN_VAR_SIZE:
          case GRN_COLUMN_INDEX:
            grn_inspect_name(ctx, &buffer, proc);
            break;
          default:
            grn_inspect(ctx, &buffer, proc);
            break;
          }
          ERR(GRN_INVALID_ARGUMENT, "invalid function: <%.*s>",
              (int)GRN_TEXT_LEN(&buffer), GRN_TEXT_VALUE(&buffer));
          GRN_OBJ_FIN(ctx, &buffer);
          goto exit;
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      {
        int i = nargs;
        while (i--) { dfi = grn_expr_dfi_pop(e); }
      }
      if (!obj) { dfi = grn_expr_dfi_pop(e); }
      // todo : increment e->values_tail.
      /* cannot identify type of return value */
      grn_expr_dfi_put(ctx, e, type, domain, code);
      e->cacheable = 0;
      break;
    case GRN_OP_INTERN :
      if (obj && CONSTP(obj)) {
        grn_obj *value;
        value = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj));
        if (!value) { value = grn_ctx_get(ctx, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj)); }
        if (value) {
          obj = value;
          op = GRN_OP_PUSH;
          type = obj->header.type;
          domain = GRN_OBJ_GET_DOMAIN(obj);
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_EQUAL :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs) {
        grn_id xd, yd = GRN_ID_NIL;
        grn_obj *x, *y = NULL;
        int i = nargs - 1;
        if (obj) {
          xd = GRN_OBJ_GET_DOMAIN(obj);
          x = obj;
        } else {
          dfi = grn_expr_dfi_pop(e);
          x = dfi->code->value;
          xd = dfi->domain;
        }
        while (i--) {
          dfi = grn_expr_dfi_pop(e);
          y = dfi->code->value;
          yd = dfi->domain;
        }
        if (CONSTP(x)) {
          if (CONSTP(y)) {
            /* todo */
          } else {
            if (xd != yd) {
              grn_expr_append_obj_resolve_const(ctx, x, yd);
            }
          }
        } else {
          if (CONSTP(y)) {
            if (xd != yd) {
              grn_expr_append_obj_resolve_const(ctx, y, xd);
            }
          }
        }
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_TABLE_CREATE :
    case GRN_OP_EXPR_GET_VAR :
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_DISTANCE1 :
    case GRN_OP_GEO_DISTANCE2 :
    case GRN_OP_GEO_DISTANCE3 :
    case GRN_OP_GEO_DISTANCE4 :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_OBJ_SEARCH :
    case GRN_OP_TABLE_SELECT :
    case GRN_OP_TABLE_SORT :
    case GRN_OP_TABLE_GROUP :
    case GRN_OP_JSON_PUT :
    case GRN_OP_GET_REF :
    case GRN_OP_ADJUST :
    case GRN_OP_TERM_EXTRACT :
    case GRN_OP_REGEXP :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs) {
        int i = nargs - 1;
        if (!obj) { dfi = grn_expr_dfi_pop(e); }
        while (i--) { dfi = grn_expr_dfi_pop(e); }
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_AND_NOT :
      PUSH_CODE(e, op, obj, nargs, code);
      if (nargs != 2) {
        GRN_LOG(ctx, GRN_LOG_WARNING, "nargs(%d) != 2 in relative op", nargs);
      }
      if (obj) {
        GRN_LOG(ctx, GRN_LOG_WARNING, "obj assigned to relative op");
      }
      {
        int i = nargs;
        while (i--) {
          dfi = grn_expr_dfi_pop(e);
          if (dfi) {
            dfi->code->flags |= GRN_EXPR_CODE_RELATIONAL_EXPRESSION;
          } else {
            ERR(GRN_SYNTAX_ERROR, "stack under flow in relative op");
          }
        }
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_NOT :
      if (nargs == 1) {
        PUSH_CODE(e, op, obj, nargs, code);
      }
      break;
    case GRN_OP_PLUS :
      if (nargs > 1) {
        PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      }
      break;
    case GRN_OP_MINUS :
      if (nargs == 1) {
        APPEND_UNARY_MINUS_OP(e);
      } else {
        PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      }
      break;
    case GRN_OP_BITWISE_NOT :
      dfi = grn_expr_dfi_pop(e);
      if (dfi) {
        type = dfi->type;
        domain = dfi->domain;
        switch (domain) {
        case GRN_DB_UINT8 :
          domain = GRN_DB_INT16;
          break;
        case GRN_DB_UINT16 :
          domain = GRN_DB_INT32;
          break;
        case GRN_DB_UINT32 :
        case GRN_DB_UINT64 :
          domain = GRN_DB_INT64;
          break;
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_STAR :
    case GRN_OP_SLASH :
    case GRN_OP_MOD :
    case GRN_OP_SHIFTL :
    case GRN_OP_SHIFTR :
    case GRN_OP_SHIFTRR :
    case GRN_OP_BITWISE_OR :
    case GRN_OP_BITWISE_XOR :
    case GRN_OP_BITWISE_AND :
      PUSH_N_ARGS_ARITHMETIC_OP(e, op, obj, nargs, code);
      break;
    case GRN_OP_INCR :
    case GRN_OP_DECR :
    case GRN_OP_INCR_POST :
    case GRN_OP_DECR_POST :
      {
        dfi = grn_expr_dfi_pop(e);
        if (dfi) {
          type = dfi->type;
          domain = dfi->domain;
          if (dfi->code) {
            if (dfi->code->op == GRN_OP_GET_VALUE) {
              dfi->code->op = GRN_OP_GET_REF;
            }
            if (dfi->code->value && grn_obj_is_persistent(ctx, dfi->code->value)) {
              e->cacheable = 0;
              e->taintable = 1;
            }
          }
        }
        PUSH_CODE(e, op, obj, nargs, code);
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_GET_VALUE :
      {
        grn_id vdomain = GRN_ID_NIL;
        if (obj) {
          if (nargs == 1) {
            grn_obj *v = grn_expr_get_var_by_offset(ctx, expr, 0);
            if (v) { vdomain = GRN_OBJ_GET_DOMAIN(v); }
          } else {
            dfi = grn_expr_dfi_pop(e);
            vdomain = dfi->domain;
          }
          if (vdomain && CONSTP(obj) && obj->header.type == GRN_BULK) {
            grn_obj *table = grn_ctx_at(ctx, vdomain);
            grn_obj *col = grn_obj_column(ctx, table, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
            if (col) {
              obj = col;
              type = col->header.type;
              domain = grn_obj_get_range(ctx, col);
              grn_expr_take_obj(ctx, (grn_obj *)e, col);
            }
          } else {
            domain = grn_obj_get_range(ctx, obj);
          }
          PUSH_CODE(e, op, obj, nargs, code);
        } else {
          grn_expr_dfi *dfi0;
          dfi0 = grn_expr_dfi_pop(e);
          if (nargs == 1) {
            grn_obj *v = grn_expr_get_var_by_offset(ctx, expr, 0);
            if (v) { vdomain = GRN_OBJ_GET_DOMAIN(v); }
          } else {
            dfi = grn_expr_dfi_pop(e);
            vdomain = dfi->domain;
          }
          if (dfi0->code->op == GRN_OP_PUSH) {
            dfi0->code->op = op;
            dfi0->code->nargs = nargs;
            obj = dfi0->code->value;
            if (vdomain && obj && CONSTP(obj) && obj->header.type == GRN_BULK) {
              grn_obj *table = grn_ctx_at(ctx, vdomain);
              grn_obj *col = grn_obj_column(ctx, table, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
              if (col) {
                dfi0->code->value = col;
                type = col->header.type;
                domain = grn_obj_get_range(ctx, col);
                grn_obj_unlink(ctx, col);
              }
            } else {
              domain = grn_obj_get_range(ctx, obj);
            }
            code = dfi0->code;
          } else {
            PUSH_CODE(e, op, obj, nargs, code);
          }
        }
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_ASSIGN :
    case GRN_OP_STAR_ASSIGN :
    case GRN_OP_SLASH_ASSIGN :
    case GRN_OP_MOD_ASSIGN :
    case GRN_OP_PLUS_ASSIGN :
    case GRN_OP_MINUS_ASSIGN :
    case GRN_OP_SHIFTL_ASSIGN :
    case GRN_OP_SHIFTR_ASSIGN :
    case GRN_OP_SHIFTRR_ASSIGN :
    case GRN_OP_AND_ASSIGN :
    case GRN_OP_OR_ASSIGN :
    case GRN_OP_XOR_ASSIGN :
      {
        if (obj) {
          type = obj->header.type;
          domain = GRN_OBJ_GET_DOMAIN(obj);
        } else {
          dfi = grn_expr_dfi_pop(e);
          if (dfi) {
            type = dfi->type;
            domain = dfi->domain;
          }
        }
        dfi = grn_expr_dfi_pop(e);
        if (dfi && (dfi->code)) {
          if (dfi->code->op == GRN_OP_GET_VALUE) {
            dfi->code->op = GRN_OP_GET_REF;
          }
          if (dfi->code->value && grn_obj_is_persistent(ctx, dfi->code->value)) {
            e->cacheable = 0;
            e->taintable = 1;
          }
        }
        PUSH_CODE(e, op, obj, nargs, code);
      }
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    case GRN_OP_JUMP :
      dfi = grn_expr_dfi_pop(e);
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    case GRN_OP_CJUMP :
      dfi = grn_expr_dfi_pop(e);
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    case GRN_OP_COMMA :
      PUSH_CODE(e, op, obj, nargs, code);
      break;
    case GRN_OP_GET_MEMBER :
      dfi = grn_expr_dfi_pop(e);
      dfi = grn_expr_dfi_pop(e);
      if (dfi) {
        type = dfi->type;
        domain = dfi->domain;
        if (dfi->code) {
          if (dfi->code->op == GRN_OP_GET_VALUE) {
            dfi->code->op = GRN_OP_GET_REF;
          }
        }
      }
      PUSH_CODE(e, op, obj, nargs, code);
      grn_expr_dfi_put(ctx, e, type, domain, code);
      break;
    default :
      break;
    }
  }
exit :
  if (!ctx->rc) { res = obj; }
  GRN_API_RETURN(res);
}
#undef PUSH_N_ARGS_ARITHMETIC_OP
#undef APPEND_UNARY_MINUS_OP

grn_obj *
grn_expr_append_const(grn_ctx *ctx, grn_obj *expr, grn_obj *obj,
                      grn_operator op, int nargs)
{
  grn_obj *res = NULL;
  GRN_API_ENTER;
  if (!obj) {
    ERR(GRN_SYNTAX_ERROR, "constant is null");
    goto exit;
  }
  if (GRN_DB_OBJP(obj) || GRN_ACCESSORP(obj)) {
    res = obj;
  } else {
    if ((res = grn_expr_alloc_const(ctx, expr))) {
      switch (obj->header.type) {
      case GRN_VOID :
      case GRN_BULK :
      case GRN_UVECTOR :
        GRN_OBJ_INIT(res, obj->header.type, 0, obj->header.domain);
        grn_bulk_write(ctx, res, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
        break;
      default :
        res = NULL;
        ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "unsupported type");
        goto exit;
      }
      res->header.impl_flags |= GRN_OBJ_EXPRCONST;
    }
  }
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
exit :
  GRN_API_RETURN(res);
}

static grn_obj *
grn_expr_add_str(grn_ctx *ctx, grn_obj *expr, const char *str, unsigned int str_size)
{
  grn_obj *res = NULL;
  if ((res = grn_expr_alloc_const(ctx, expr))) {
    GRN_TEXT_INIT(res, 0);
    grn_bulk_write(ctx, res, str, str_size);
    res->header.impl_flags |= GRN_OBJ_EXPRCONST;
  }
  return res;
}

grn_obj *
grn_expr_append_const_str(grn_ctx *ctx, grn_obj *expr, const char *str, unsigned int str_size,
                          grn_operator op, int nargs)
{
  grn_obj *res;
  GRN_API_ENTER;
  res = grn_expr_add_str(ctx, expr, str, str_size);
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
  GRN_API_RETURN(res);
}

grn_obj *
grn_expr_append_const_int(grn_ctx *ctx, grn_obj *expr, int i,
                          grn_operator op, int nargs)
{
  grn_obj *res = NULL;
  GRN_API_ENTER;
  if ((res = grn_expr_alloc_const(ctx, expr))) {
    GRN_INT32_INIT(res, 0);
    GRN_INT32_SET(ctx, res, i);
    res->header.impl_flags |= GRN_OBJ_EXPRCONST;
  }
  grn_expr_append_obj(ctx, expr, res, op, nargs); /* constant */
  GRN_API_RETURN(res);
}

grn_rc
grn_expr_append_op(grn_ctx *ctx, grn_obj *expr, grn_operator op, int nargs)
{
  grn_expr_append_obj(ctx, expr, NULL, op, nargs);
  return ctx->rc;
}

grn_rc
grn_expr_compile(grn_ctx *ctx, grn_obj *expr)
{
  grn_obj_spec_save(ctx, DB_OBJ(expr));
  return ctx->rc;
}

grn_obj *
grn_expr_rewrite(grn_ctx *ctx, grn_obj *expr)
{
  grn_obj *rewritten = NULL;

  GRN_API_ENTER;

#ifdef GRN_WITH_MRUBY
  if (ctx->impl->mrb.state) {
    rewritten = grn_mrb_expr_rewrite(ctx, expr);
  }
#endif

  GRN_API_RETURN(rewritten);
}

#define WITH_SPSAVE(block) do {\
  ctx->impl->stack_curr = sp - ctx->impl->stack;\
  e->values_curr = vp - e->values;\
  block\
  vp = e->values + e->values_curr;\
  sp = ctx->impl->stack + ctx->impl->stack_curr;\
  s0 = sp[-1];\
  s1 = sp[-2];\
} while (0)

#define GEO_RESOLUTION   3600000
#define GEO_RADIOUS      6357303
#define GEO_BES_C1       6334834
#define GEO_BES_C2       6377397
#define GEO_BES_C3       0.006674
#define GEO_GRS_C1       6335439
#define GEO_GRS_C2       6378137
#define GEO_GRS_C3       0.006694
#define GEO_INT2RAD(x)   ((M_PI * x) / (GEO_RESOLUTION * 180))

#define VAR_SET_VALUE(ctx,var,value) do {\
  if (GRN_DB_OBJP(value)) {\
    (var)->header.type = GRN_PTR;\
    (var)->header.domain = DB_OBJ(value)->id;\
    GRN_PTR_SET(ctx, (var), (value));\
  } else {\
    (var)->header.type = (value)->header.type;\
    (var)->header.domain = (value)->header.domain;\
    GRN_TEXT_SET(ctx, (var), GRN_TEXT_VALUE(value), GRN_TEXT_LEN(value));\
  }\
} while (0)

grn_rc
grn_proc_call(grn_ctx *ctx, grn_obj *proc, int nargs, grn_obj *caller)
{
  grn_proc_ctx pctx;
  grn_obj *obj = NULL, **args;
  grn_proc *p = (grn_proc *)proc;
  if (nargs > ctx->impl->stack_curr) { return GRN_INVALID_ARGUMENT; }
  GRN_API_ENTER;
  if (grn_obj_is_selector_only_proc(ctx, proc)) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size;
    name_size = grn_obj_name(ctx, proc, name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "selector only proc can't be called: <%.*s>",
        name_size, name);
    GRN_API_RETURN(ctx->rc);
  }
  args = ctx->impl->stack + ctx->impl->stack_curr - nargs;
  pctx.proc = p;
  pctx.caller = caller;
  pctx.user_data.ptr = NULL;
  if (p->funcs[PROC_INIT]) {
    obj = p->funcs[PROC_INIT](ctx, nargs, args, &pctx.user_data);
  }
  pctx.phase = PROC_NEXT;
  if (p->funcs[PROC_NEXT]) {
    obj = p->funcs[PROC_NEXT](ctx, nargs, args, &pctx.user_data);
  }
  pctx.phase = PROC_FIN;
  if (p->funcs[PROC_FIN]) {
    obj = p->funcs[PROC_FIN](ctx, nargs, args, &pctx.user_data);
  }
  ctx->impl->stack_curr -= nargs;
  grn_ctx_push(ctx, obj);
  GRN_API_RETURN(ctx->rc);
}

#define PUSH1(v) do {\
  if (EXPRVP(v)) {\
    vp++;\
    if (vp - e->values > e->values_tail) { e->values_tail = vp - e->values; }\
  }\
  s1 = s0;\
  *sp++ = s0 = v;\
} while (0)

#define POP1(v) do {\
  if (EXPRVP(s0)) { vp--; }\
  v = s0;\
  s0 = s1;\
  sp--;\
  if (sp < s_) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
  s1 = sp[-2];\
} while (0)

#define ALLOC1(value) do {\
  s1 = s0;\
  *sp++ = s0 = value = vp++;\
  if (vp - e->values > e->values_tail) { e->values_tail = vp - e->values; }\
} while (0)

#define POP1ALLOC1(arg,value) do {\
  arg = s0;\
  if (EXPRVP(s0)) {\
    value = s0;\
  } else {\
    if (sp < s_ + 1) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
    sp[-1] = s0 = value = vp++;\
    if (vp - e->values > e->values_tail) { e->values_tail = vp - e->values; }\
    s0->header.impl_flags |= GRN_OBJ_EXPRVALUE;\
  }\
} while (0)

#define POP2ALLOC1(arg1,arg2,value) do {\
  if (EXPRVP(s0)) { vp--; }\
  if (EXPRVP(s1)) { vp--; }\
  arg2 = s0;\
  arg1 = s1;\
  sp--;\
  if (sp < s_ + 1) { ERR(GRN_INVALID_ARGUMENT, "stack underflow"); goto exit; }\
  s1 = sp[-2];\
  sp[-1] = s0 = value = vp++;\
  if (vp - e->values > e->values_tail) { e->values_tail = vp - e->values; }\
  s0->header.impl_flags |= GRN_OBJ_EXPRVALUE;\
} while (0)

#define INTEGER_ARITHMETIC_OPERATION_PLUS(x, y) ((x) + (y))
#define FLOAT_ARITHMETIC_OPERATION_PLUS(x, y) ((double)(x) + (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_MINUS(x, y) ((x) - (y))
#define FLOAT_ARITHMETIC_OPERATION_MINUS(x, y) ((double)(x) - (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_STAR(x, y) ((x) * (y))
#define FLOAT_ARITHMETIC_OPERATION_STAR(x, y) ((double)(x) * (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_SLASH(x, y) ((x) / (y))
#define FLOAT_ARITHMETIC_OPERATION_SLASH(x, y) ((double)(x) / (double)(y))
#define INTEGER_ARITHMETIC_OPERATION_MOD(x, y) ((x) % (y))
#define FLOAT_ARITHMETIC_OPERATION_MOD(x, y) (fmod((x), (y)))
#define INTEGER_ARITHMETIC_OPERATION_SHIFTL(x, y) ((x) << (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTL(x, y)                         \
  ((long long int)(x) << (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_SHIFTR(x, y) ((x) >> (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTR(x, y)                         \
  ((long long int)(x) >> (long long int)(y))
#define INTEGER8_ARITHMETIC_OPERATION_SHIFTRR(x, y)             \
  ((uint8_t)(x) >> (y))
#define INTEGER16_ARITHMETIC_OPERATION_SHIFTRR(x, y)            \
  ((uint16_t)(x) >> (y))
#define INTEGER32_ARITHMETIC_OPERATION_SHIFTRR(x, y)            \
  ((unsigned int)(x) >> (y))
#define INTEGER64_ARITHMETIC_OPERATION_SHIFTRR(x, y)            \
  ((long long unsigned int)(x) >> (y))
#define FLOAT_ARITHMETIC_OPERATION_SHIFTRR(x, y)                \
  ((long long unsigned int)(x) >> (long long unsigned int)(y))

#define INTEGER_ARITHMETIC_OPERATION_BITWISE_OR(x, y) ((x) | (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_OR(x, y)                \
  ((long long int)(x) | (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR(x, y) ((x) ^ (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR(x, y)                \
  ((long long int)(x) ^ (long long int)(y))
#define INTEGER_ARITHMETIC_OPERATION_BITWISE_AND(x, y) ((x) & (y))
#define FLOAT_ARITHMETIC_OPERATION_BITWISE_AND(x, y)                \
  ((long long int)(x) & (long long int)(y))

#define INTEGER_UNARY_ARITHMETIC_OPERATION_MINUS(x) (-(x))
#define FLOAT_UNARY_ARITHMETIC_OPERATION_MINUS(x) (-(x))
#define INTEGER_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT(x) (~(x))
#define FLOAT_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT(x) \
  (~((long long int)(x)))

#define TEXT_ARITHMETIC_OPERATION(operator) do {                        \
  long long int x_;                                                     \
  long long int y_;                                                     \
                                                                        \
  res->header.domain = GRN_DB_INT64;                                    \
                                                                        \
  GRN_INT64_SET(ctx, res, 0);                                           \
  grn_obj_cast(ctx, x, res, GRN_FALSE);                                 \
  x_ = GRN_INT64_VALUE(res);                                            \
                                                                        \
  GRN_INT64_SET(ctx, res, 0);                                           \
  grn_obj_cast(ctx, y, res, GRN_FALSE);                                 \
  y_ = GRN_INT64_VALUE(res);                                            \
                                                                        \
  GRN_INT64_SET(ctx, res, x_ operator y_);                              \
} while (0)

#define TEXT_UNARY_ARITHMETIC_OPERATION(unary_operator) do { \
  long long int x_;                                          \
                                                             \
  res->header.domain = GRN_DB_INT64;                         \
                                                             \
  GRN_INT64_SET(ctx, res, 0);                                \
  grn_obj_cast(ctx, x, res, GRN_FALSE);                      \
  x_ = GRN_INT64_VALUE(res);                                 \
                                                             \
  GRN_INT64_SET(ctx, res, unary_operator x_);                \
} while (0)

#define ARITHMETIC_OPERATION_NO_CHECK(y) do {} while (0)
#define ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y) do {        \
  if ((long long int)y == 0) {                                  \
    ERR(GRN_INVALID_ARGUMENT, "divisor should not be 0");       \
    goto exit;                                                  \
  }                                                             \
} while (0)


#define NUMERIC_ARITHMETIC_OPERATION_DISPATCH(set, get, x_, y, res,     \
                                              integer_operation,        \
                                              float_operation,          \
                                              right_expression_check,   \
                                              invalid_type_error) do {  \
  switch (y->header.domain) {                                           \
  case GRN_DB_INT8 :                                                    \
    {                                                                   \
      int8_t y_;                                                        \
      y_ = GRN_INT8_VALUE(y);                                           \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT8 :                                                   \
    {                                                                   \
      uint8_t y_;                                                       \
      y_ = GRN_UINT8_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT16 :                                                   \
    {                                                                   \
      int16_t y_;                                                       \
      y_ = GRN_INT16_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT16 :                                                  \
    {                                                                   \
      uint16_t y_;                                                      \
      y_ = GRN_UINT16_VALUE(y);                                         \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_INT32_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int y_;                                                  \
      y_ = GRN_UINT32_VALUE(y);                                         \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_TIME_VALUE(y);                                           \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_INT64_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int y_;                                        \
      y_ = GRN_UINT64_VALUE(y);                                         \
      right_expression_check(y_);                                       \
      set(ctx, res, integer_operation(x_, y_));                         \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double y_;                                                        \
      y_ = GRN_FLOAT_VALUE(y);                                          \
      right_expression_check(y_);                                       \
      res->header.domain = GRN_DB_FLOAT;                                \
      GRN_FLOAT_SET(ctx, res, float_operation(x_, y_));                 \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    set(ctx, res, 0);                                                   \
    if (grn_obj_cast(ctx, y, res, GRN_FALSE)) {                         \
      ERR(GRN_INVALID_ARGUMENT,                                         \
          "not a numerical format: <%.*s>",                             \
          (int)GRN_TEXT_LEN(y), GRN_TEXT_VALUE(y));                          \
      goto exit;                                                        \
    }                                                                   \
    set(ctx, res, integer_operation(x_, get(res)));                     \
    break;                                                              \
  default :                                                             \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
} while (0)


#define ARITHMETIC_OPERATION_DISPATCH(x, y, res,                        \
                                      integer8_operation,               \
                                      integer16_operation,              \
                                      integer32_operation,              \
                                      integer64_operation,              \
                                      float_operation,                  \
                                      left_expression_check,            \
                                      right_expression_check,           \
                                      text_operation,                   \
                                      invalid_type_error) do {          \
  switch (x->header.domain) {                                           \
  case GRN_DB_INT8 :                                                    \
    {                                                                   \
      int8_t x_;                                                        \
      x_ = GRN_INT8_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_INT8_SET,               \
                                            GRN_INT8_VALUE,             \
                                            x_, y, res,                 \
                                            integer8_operation,         \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT8 :                                                   \
    {                                                                   \
      uint8_t x_;                                                       \
      x_ = GRN_UINT8_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT8_SET,              \
                                            GRN_UINT8_VALUE,            \
                                            x_, y, res,                 \
                                            integer8_operation,         \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT16 :                                                   \
    {                                                                   \
      int16_t x_;                                                       \
      x_ = GRN_INT16_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_INT16_SET,              \
                                            GRN_INT16_VALUE,            \
                                            x_, y, res,                 \
                                            integer16_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT16 :                                                  \
    {                                                                   \
      uint16_t x_;                                                      \
      x_ = GRN_UINT16_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT16_SET,             \
                                            GRN_UINT16_VALUE,           \
                                            x_, y, res,                 \
                                            integer16_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT32_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_INT32_SET,              \
                                            GRN_INT32_VALUE,            \
                                            x_, y, res,                 \
                                            integer32_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int x_;                                                  \
      x_ = GRN_UINT32_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT32_SET,             \
                                            GRN_UINT32_VALUE,           \
                                            x_, y, res,                 \
                                            integer32_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_INT64_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT64_SET,             \
                                            GRN_UINT64_VALUE,           \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_TIME_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_TIME_SET,               \
                                            GRN_TIME_VALUE,             \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int x_;                                        \
      x_ = GRN_UINT64_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_UINT64_SET,             \
                                            GRN_UINT64_VALUE,           \
                                            x_, y, res,                 \
                                            integer64_operation,        \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double x_;                                                        \
      x_ = GRN_FLOAT_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      NUMERIC_ARITHMETIC_OPERATION_DISPATCH(GRN_FLOAT_SET,              \
                                            GRN_FLOAT_VALUE,            \
                                            x_, y, res,                 \
                                            float_operation,            \
                                            float_operation,            \
                                            right_expression_check,     \
                                            invalid_type_error);        \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    text_operation;                                                     \
    break;                                                              \
  default:                                                              \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
  code++;                                                               \
} while (0)

#define ARITHMETIC_BINARY_OPERATION_DISPATCH(operator,                  \
                                             integer8_operation,        \
                                             integer16_operation,       \
                                             integer32_operation,       \
                                             integer64_operation,       \
                                             float_operation,           \
                                             left_expression_check,     \
                                             right_expression_check,    \
                                             text_operation,            \
                                             invalid_type_error) do {   \
  grn_obj *x, *y;                                                       \
                                                                        \
  POP2ALLOC1(x, y, res);                                                \
  if (x->header.type == GRN_VECTOR || y->header.type == GRN_VECTOR) {   \
    grn_obj inspected_x;                                                \
    grn_obj inspected_y;                                                \
    GRN_TEXT_INIT(&inspected_x, 0);                                     \
    GRN_TEXT_INIT(&inspected_y, 0);                                     \
    grn_inspect(ctx, &inspected_x, x);                                  \
    grn_inspect(ctx, &inspected_y, y);                                  \
    ERR(GRN_INVALID_ARGUMENT,                                           \
        "<%s> doesn't support vector: <%.*s> %s <%.*s>",                \
        operator,                                                       \
        (int)GRN_TEXT_LEN(&inspected_x), GRN_TEXT_VALUE(&inspected_x),  \
        operator,                                                       \
        (int)GRN_TEXT_LEN(&inspected_y), GRN_TEXT_VALUE(&inspected_y)); \
    GRN_OBJ_FIN(ctx, &inspected_x);                                     \
    GRN_OBJ_FIN(ctx, &inspected_y);                                     \
    goto exit;                                                          \
  }                                                                     \
  if (y != res) {                                                       \
    res->header.domain = x->header.domain;                              \
  }                                                                     \
  ARITHMETIC_OPERATION_DISPATCH(x, y, res,                              \
                                integer8_operation,                     \
                                integer16_operation,                    \
                                integer32_operation,                    \
                                integer64_operation,                    \
                                float_operation,                        \
                                left_expression_check,                  \
                                right_expression_check,                 \
                                text_operation,                         \
                                invalid_type_error);                    \
  if (y == res) {                                                       \
    res->header.domain = x->header.domain;                              \
  }                                                                     \
} while (0)

#define SIGNED_INTEGER_DIVISION_OPERATION_SLASH(x, y)                   \
  ((y == -1) ? -(x) : (x) / (y))
#define UNSIGNED_INTEGER_DIVISION_OPERATION_SLASH(x, y) ((x) / (y))
#define FLOAT_DIVISION_OPERATION_SLASH(x, y) ((double)(x) / (double)(y))
#define SIGNED_INTEGER_DIVISION_OPERATION_MOD(x, y) ((y == -1) ? 0 : (x) % (y))
#define UNSIGNED_INTEGER_DIVISION_OPERATION_MOD(x, y) ((x) % (y))
#define FLOAT_DIVISION_OPERATION_MOD(x, y) (fmod((x), (y)))

#define DIVISION_OPERATION_DISPATCH_RIGHT(set, get, x_, y, res,         \
                                          signed_integer_operation,     \
                                          unsigned_integer_operation,   \
                                          float_operation) do {         \
  switch (y->header.domain) {                                           \
  case GRN_DB_INT8 :                                                    \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_INT8_VALUE(y);                                           \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, signed_integer_operation(x_, y_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT8 :                                                   \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_UINT8_VALUE(y);                                          \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, unsigned_integer_operation(x_, y_));                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT16 :                                                   \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_INT16_VALUE(y);                                          \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, signed_integer_operation(x_, y_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT16 :                                                  \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_UINT16_VALUE(y);                                         \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, unsigned_integer_operation(x_, y_));                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int y_;                                                           \
      y_ = GRN_INT32_VALUE(y);                                          \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, signed_integer_operation(x_, y_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int y_;                                                  \
      y_ = GRN_UINT32_VALUE(y);                                         \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, unsigned_integer_operation(x_, y_));                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_TIME_VALUE(y);                                           \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, signed_integer_operation(x_, y_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int y_;                                                 \
      y_ = GRN_INT64_VALUE(y);                                          \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, signed_integer_operation(x_, y_));                  \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int y_;                                        \
      y_ = GRN_UINT64_VALUE(y);                                         \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      set(ctx, res, unsigned_integer_operation(x_, y_));                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double y_;                                                        \
      y_ = GRN_FLOAT_VALUE(y);                                          \
      ARITHMETIC_OPERATION_ZERO_DIVISION_CHECK(y_);                     \
      res->header.domain = GRN_DB_FLOAT;                                \
      GRN_FLOAT_SET(ctx, res, float_operation(x_, y_));                 \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    set(ctx, res, 0);                                                   \
    if (grn_obj_cast(ctx, y, res, GRN_FALSE)) {                         \
      ERR(GRN_INVALID_ARGUMENT,                                         \
          "not a numerical format: <%.*s>",                             \
          (int)GRN_TEXT_LEN(y), GRN_TEXT_VALUE(y));                     \
      goto exit;                                                        \
    }                                                                   \
    /* The following "+ 0" is needed to suppress warnings that say */   \
    /* comparison is always false due to limited range of data type */  \
    set(ctx, res, signed_integer_operation(x_, (get(res) + 0)));        \
    break;                                                              \
  default :                                                             \
    break;                                                              \
  }                                                                     \
} while (0)

#define DIVISION_OPERATION_DISPATCH_LEFT(x, y, res,                     \
                                         signed_integer_operation,      \
                                         unsigned_integer_operation,    \
                                         float_operation,               \
                                         invalid_type_error) do {       \
  switch (x->header.domain) {                                           \
  case GRN_DB_INT8 :                                                    \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT8_VALUE(x);                                           \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_INT8_SET,                   \
                                        GRN_INT8_VALUE,                 \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT8 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_UINT8_VALUE(x);                                          \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_UINT8_SET,                  \
                                        (int)GRN_UINT8_VALUE,           \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT16 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT16_VALUE(x);                                          \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_INT16_SET,                  \
                                        GRN_INT16_VALUE,                \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT16 :                                                  \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_UINT16_VALUE(x);                                         \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_UINT16_SET,                 \
                                        (int)GRN_UINT16_VALUE,          \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT32_VALUE(x);                                          \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_INT32_SET,                  \
                                        GRN_INT32_VALUE,                \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      unsigned int x_;                                                  \
      x_ = GRN_UINT32_VALUE(x);                                         \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_UINT32_SET,                 \
                                        GRN_UINT32_VALUE,               \
                                        x_, y, res,                     \
                                        unsigned_integer_operation,     \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_INT64_VALUE(x);                                          \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_INT64_SET,                  \
                                        GRN_INT64_VALUE,                \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_TIME_VALUE(x);                                           \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_TIME_SET,                   \
                                        GRN_TIME_VALUE,                 \
                                        x_, y, res,                     \
                                        signed_integer_operation,       \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int x_;                                        \
      x_ = GRN_UINT64_VALUE(x);                                         \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_UINT64_SET,                 \
                                        GRN_UINT64_VALUE,               \
                                        x_, y, res,                     \
                                        unsigned_integer_operation,     \
                                        unsigned_integer_operation,     \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double x_;                                                        \
      x_ = GRN_FLOAT_VALUE(x);                                          \
      DIVISION_OPERATION_DISPATCH_RIGHT(GRN_FLOAT_SET,                  \
                                        GRN_FLOAT_VALUE,                \
                                        x_, y, res,                     \
                                        float_operation,                \
                                        float_operation,                \
                                        float_operation);               \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    invalid_type_error;                                                 \
    break;                                                              \
  default:                                                              \
    break;                                                              \
  }                                                                     \
  code++;                                                               \
} while (0)

#define DIVISION_OPERATION_DISPATCH(signed_integer_operation,           \
                                    unsigned_integer_operation,         \
                                    float_operation,                    \
                                    invalid_type_error) do {            \
  grn_obj *x, *y;                                                       \
                                                                        \
  POP2ALLOC1(x, y, res);                                                \
  if (y != res) {                                                       \
    res->header.domain = x->header.domain;                              \
  }                                                                     \
  DIVISION_OPERATION_DISPATCH_LEFT(x, y, res,                           \
                                   signed_integer_operation,            \
                                   unsigned_integer_operation,          \
                                   float_operation,                     \
                                   invalid_type_error);                 \
  if (y == res) {                                                       \
    res->header.domain = x->header.domain;                              \
  }                                                                     \
} while (0)

#define ARITHMETIC_UNARY_OPERATION_DISPATCH(integer_operation,          \
                                            float_operation,            \
                                            left_expression_check,      \
                                            right_expression_check,     \
                                            text_operation,             \
                                            invalid_type_error) do {    \
  grn_obj *x;                                                           \
  POP1ALLOC1(x, res);                                                   \
  res->header.domain = x->header.domain;                                \
  switch (x->header.domain) {                                           \
  case GRN_DB_INT8 :                                                    \
    {                                                                   \
      int8_t x_;                                                        \
      x_ = GRN_INT8_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      GRN_INT8_SET(ctx, res, integer_operation(x_));                    \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT8 :                                                   \
    {                                                                   \
      int16_t x_;                                                       \
      x_ = GRN_UINT8_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT16_SET(ctx, res, integer_operation(x_));                   \
      res->header.domain = GRN_DB_INT16;                                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT16 :                                                   \
    {                                                                   \
      int16_t x_;                                                       \
      x_ = GRN_INT16_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT16_SET(ctx, res, integer_operation(x_));                   \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT16 :                                                  \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_UINT16_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      GRN_INT32_SET(ctx, res, integer_operation(x_));                   \
      res->header.domain = GRN_DB_INT32;                                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT32 :                                                   \
    {                                                                   \
      int x_;                                                           \
      x_ = GRN_INT32_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT32_SET(ctx, res, integer_operation(x_));                   \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_UINT32_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      GRN_INT64_SET(ctx, res, integer_operation(x_));                   \
      res->header.domain = GRN_DB_INT64;                                \
    }                                                                   \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_INT64_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_INT64_SET(ctx, res, integer_operation(x_));                   \
    }                                                                   \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    {                                                                   \
      long long int x_;                                                 \
      x_ = GRN_TIME_VALUE(x);                                           \
      left_expression_check(x_);                                        \
      GRN_TIME_SET(ctx, res, integer_operation(x_));                    \
    }                                                                   \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    {                                                                   \
      long long unsigned int x_;                                        \
      x_ = GRN_UINT64_VALUE(x);                                         \
      left_expression_check(x_);                                        \
      if (x_ > (long long unsigned int)INT64_MAX) {                     \
        ERR(GRN_INVALID_ARGUMENT,                                       \
            "too large UInt64 value to inverse sign: "                  \
            "<%" GRN_FMT_LLU ">",                                       \
            x_);                                                        \
        goto exit;                                                      \
      } else {                                                          \
        long long int signed_x_;                                        \
        signed_x_ = x_;                                                 \
        GRN_INT64_SET(ctx, res, integer_operation(signed_x_));          \
        res->header.domain = GRN_DB_INT64;                              \
      }                                                                 \
    }                                                                   \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    {                                                                   \
      double x_;                                                        \
      x_ = GRN_FLOAT_VALUE(x);                                          \
      left_expression_check(x_);                                        \
      GRN_FLOAT_SET(ctx, res, float_operation(x_));                     \
    }                                                                   \
    break;                                                              \
  case GRN_DB_SHORT_TEXT :                                              \
  case GRN_DB_TEXT :                                                    \
  case GRN_DB_LONG_TEXT :                                               \
    text_operation;                                                     \
    break;                                                              \
  default:                                                              \
    invalid_type_error;                                                 \
    break;                                                              \
  }                                                                     \
  code++;                                                               \
} while (0)

#define EXEC_OPERATE(operate_sentence, assign_sentence)   \
  operate_sentence                                        \
  assign_sentence

#define EXEC_OPERATE_POST(operate_sentence, assign_sentence)    \
  assign_sentence                                               \
  operate_sentence

#define UNARY_OPERATE_AND_ASSIGN_DISPATCH(exec_operate, delta,          \
                                          set_flags) do {               \
  grn_obj *var, *col, value;                                            \
  grn_id rid;                                                           \
                                                                        \
  POP1ALLOC1(var, res);                                                 \
  if (var->header.type != GRN_PTR) {                                    \
    ERR(GRN_INVALID_ARGUMENT, "invalid variable type: 0x%0x",           \
        var->header.type);                                              \
    goto exit;                                                          \
  }                                                                     \
  if (GRN_BULK_VSIZE(var) != (sizeof(grn_obj *) + sizeof(grn_id))) {    \
    ERR(GRN_INVALID_ARGUMENT,                                           \
        "invalid variable size: "                                       \
        "expected: %" GRN_FMT_SIZE                                      \
        "actual: %" GRN_FMT_SIZE,                                       \
        (sizeof(grn_obj *) + sizeof(grn_id)), GRN_BULK_VSIZE(var));     \
    goto exit;                                                          \
  }                                                                     \
  col = GRN_PTR_VALUE(var);                                             \
  rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));            \
  res->header.type = GRN_VOID;                                          \
  res->header.domain = DB_OBJ(col)->range;                              \
  switch (DB_OBJ(col)->range) {                                         \
  case GRN_DB_INT32 :                                                   \
    GRN_INT32_INIT(&value, 0);                                          \
    GRN_INT32_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_UINT32 :                                                  \
    GRN_UINT32_INIT(&value, 0);                                         \
    GRN_UINT32_SET(ctx, &value, delta);                                 \
    break;                                                              \
  case GRN_DB_INT64 :                                                   \
    GRN_INT64_INIT(&value, 0);                                          \
    GRN_INT64_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_UINT64 :                                                  \
    GRN_UINT64_INIT(&value, 0);                                         \
    GRN_UINT64_SET(ctx, &value, delta);                                 \
    break;                                                              \
  case GRN_DB_FLOAT :                                                   \
    GRN_FLOAT_INIT(&value, 0);                                          \
    GRN_FLOAT_SET(ctx, &value, delta);                                  \
    break;                                                              \
  case GRN_DB_TIME :                                                    \
    GRN_TIME_INIT(&value, 0);                                           \
    GRN_TIME_SET(ctx, &value, GRN_TIME_PACK(delta, 0));                 \
    break;                                                              \
  default:                                                              \
    ERR(GRN_INVALID_ARGUMENT,                                           \
        "invalid increment target type: %d "                            \
        "(FIXME: type name is needed)", DB_OBJ(col)->range);            \
    goto exit;                                                          \
    break;                                                              \
  }                                                                     \
  exec_operate(grn_obj_set_value(ctx, col, rid, &value, set_flags);,    \
               grn_obj_get_value(ctx, col, rid, res););                 \
  code++;                                                               \
} while (0)

#define ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(integer8_operation,    \
                                                 integer16_operation,   \
                                                 integer32_operation,   \
                                                 integer64_operation,   \
                                                 float_operation,       \
                                                 left_expression_check, \
                                                 right_expression_check,\
                                                 text_operation) do {   \
  grn_obj *value, *var, *res;                                           \
  if (code->value) {                                                    \
    value = code->value;                                                \
    POP1ALLOC1(var, res);                                               \
  } else {                                                              \
    POP2ALLOC1(var, value, res);                                        \
  }                                                                     \
  if (var->header.type == GRN_PTR &&                                    \
      GRN_BULK_VSIZE(var) == (sizeof(grn_obj *) + sizeof(grn_id))) {    \
    grn_obj *col = GRN_PTR_VALUE(var);                                  \
    grn_id rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));   \
    grn_obj variable_value, casted_value;                               \
    grn_id domain;                                                      \
                                                                        \
    value = GRN_OBJ_RESOLVE(ctx, value);                                \
                                                                        \
    domain = grn_obj_get_range(ctx, col);                               \
    GRN_OBJ_INIT(&variable_value, GRN_BULK, 0, domain);                 \
    grn_obj_get_value(ctx, col, rid, &variable_value);                  \
                                                                        \
    GRN_OBJ_INIT(&casted_value, GRN_BULK, 0, domain);                   \
    if (grn_obj_cast(ctx, value, &casted_value, GRN_FALSE)) {           \
      ERR(GRN_INVALID_ARGUMENT, "invalid value: string");               \
      GRN_OBJ_FIN(ctx, &variable_value);                                \
      GRN_OBJ_FIN(ctx, &casted_value);                                  \
      POP1(res);                                                        \
      goto exit;                                                        \
    }                                                                   \
    grn_obj_reinit(ctx, res, domain, 0);                                \
    ARITHMETIC_OPERATION_DISPATCH((&variable_value), (&casted_value),   \
                                  res,                                  \
                                  integer8_operation,                   \
                                  integer16_operation,                  \
                                  integer32_operation,                  \
                                  integer64_operation,                  \
                                  float_operation,                      \
                                  left_expression_check,                \
                                  right_expression_check,               \
                                  text_operation,);                     \
    grn_obj_set_value(ctx, col, rid, res, GRN_OBJ_SET);                 \
    GRN_OBJ_FIN(ctx, (&variable_value));                                \
    GRN_OBJ_FIN(ctx, (&casted_value));                                  \
  } else {                                                              \
    ERR(GRN_INVALID_ARGUMENT, "left hand expression isn't column.");    \
    POP1(res);                                                          \
  }                                                                     \
} while (0)

inline static void
grn_expr_exec_get_member_vector(grn_ctx *ctx,
                                grn_obj *expr,
                                grn_obj *column_and_record_id,
                                grn_obj *index,
                                grn_obj *result)
{
  grn_obj *column;
  grn_id record_id;
  grn_obj values;
  int i;

  column = GRN_PTR_VALUE(column_and_record_id);
  record_id = *((grn_id *)(&(GRN_PTR_VALUE_AT(column_and_record_id, 1))));
  GRN_TEXT_INIT(&values, 0);
  grn_obj_get_value(ctx, column, record_id, &values);

  i = GRN_UINT32_VALUE(index);
  if (values.header.type == GRN_UVECTOR) {
    int n_elements;
    grn_obj_reinit(ctx, result, DB_OBJ(column)->range, 0);
    n_elements = GRN_BULK_VSIZE(&values) / sizeof(grn_id);
    if (n_elements > i) {
      grn_id value;
      value = GRN_RECORD_VALUE_AT(&values, i);
      GRN_RECORD_SET(ctx, result, value);
    }
  } else {
    if (values.u.v.n_sections > i) {
      const char *content;
      unsigned int content_length;
      grn_id domain;

      content_length = grn_vector_get_element(ctx, &values, i,
                                              &content, NULL, &domain);
      grn_obj_reinit(ctx, result, domain, 0);
      grn_bulk_write(ctx, result, content, content_length);
    }
  }

  GRN_OBJ_FIN(ctx, &values);
}

inline static void
grn_expr_exec_get_member_table(grn_ctx *ctx,
                               grn_obj *expr,
                               grn_obj *table,
                               grn_obj *key,
                               grn_obj *result)
{
  grn_id id;

  if (table->header.domain == key->header.domain) {
    id = grn_table_get(ctx, table, GRN_BULK_HEAD(key), GRN_BULK_VSIZE(key));
  } else {
    grn_obj casted_key;
    GRN_OBJ_INIT(&casted_key, GRN_BULK, 0, table->header.domain);
    if (grn_obj_cast(ctx, key, &casted_key, GRN_FALSE) == GRN_SUCCESS) {
      id = grn_table_get(ctx, table,
                         GRN_BULK_HEAD(&casted_key),
                         GRN_BULK_VSIZE(&casted_key));
    } else {
      id = GRN_ID_NIL;
    }
    GRN_OBJ_FIN(ctx, &casted_key);
  }

  grn_obj_reinit(ctx, result, DB_OBJ(table)->id, 0);
  GRN_RECORD_SET(ctx, result, id);
}

static inline grn_bool
grn_expr_exec_is_simple_expr(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;

  if (expr->header.type != GRN_EXPR) {
    return GRN_FALSE;
  }

  if (e->codes_curr != 1) {
    return GRN_FALSE;
  }

  switch (e->codes[0].op) {
  case GRN_OP_PUSH :
    return GRN_TRUE;
  default :
    return GRN_FALSE;
  }
}

static inline grn_obj *
grn_expr_exec_simple(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;

  return e->codes[0].value;
}

grn_obj *
grn_expr_exec(grn_ctx *ctx, grn_obj *expr, int nargs)
{
  grn_obj *val = NULL;
  uint32_t stack_curr = ctx->impl->stack_curr;
  GRN_API_ENTER;
  if (grn_expr_exec_is_simple_expr(ctx, expr)) {
    val = grn_expr_exec_simple(ctx, expr);
    GRN_API_RETURN(val);
  }
  if (expr->header.type == GRN_PROC) {
    grn_proc *proc = (grn_proc *)expr;
    if (proc->type == GRN_PROC_COMMAND) {
      grn_command_input *input;
      input = grn_command_input_open(ctx, expr);
      grn_command_run(ctx, expr, input);
      grn_command_input_close(ctx, input);
      GRN_API_RETURN(NULL);
    } else {
      grn_proc_call(ctx, expr, nargs, expr);
    }
  } else {
    grn_expr *e = (grn_expr *)expr;
    register grn_obj **s_ = ctx->impl->stack;
    register grn_obj *s0 = NULL;
    register grn_obj *s1 = NULL;
    register grn_obj **sp;
    register grn_obj *vp = e->values;
    grn_obj *res = NULL, *v0 = grn_expr_get_var_by_offset(ctx, expr, 0);
    grn_expr_code *code = e->codes, *ce = &e->codes[e->codes_curr];
    sp = s_ + stack_curr;
    while (code < ce) {
      switch (code->op) {
      case GRN_OP_NOP :
        code++;
        break;
      case GRN_OP_PUSH :
        PUSH1(code->value);
        code++;
        break;
      case GRN_OP_POP :
        {
          grn_obj *obj;
          POP1(obj);
          code++;
        }
        break;
      case GRN_OP_GET_REF :
        {
          grn_obj *col, *rec;
          if (code->nargs == 1) {
            rec = v0;
            if (code->value) {
              col = code->value;
              ALLOC1(res);
            } else {
              POP1ALLOC1(col, res);
            }
          } else {
            if (code->value) {
              col = code->value;
              POP1ALLOC1(rec, res);
            } else {
              POP2ALLOC1(rec, col, res);
            }
          }
          if (col->header.type == GRN_BULK) {
            grn_obj *table = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(rec));
            col = grn_obj_column(ctx, table, GRN_BULK_HEAD(col), GRN_BULK_VSIZE(col));
            if (col) { grn_expr_take_obj(ctx, (grn_obj *)e, col); }
          }
          if (col) {
            res->header.type = GRN_PTR;
            res->header.domain = GRN_ID_NIL;
            GRN_PTR_SET(ctx, res, col);
            GRN_UINT32_PUT(ctx, res, GRN_RECORD_VALUE(rec));
          } else {
            ERR(GRN_INVALID_ARGUMENT, "col resolve failed");
            goto exit;
          }
          code++;
        }
        break;
      case GRN_OP_CALL :
        {
          grn_obj *proc;
          if (code->value) {
            if (sp < s_ + code->nargs) {
              ERR(GRN_INVALID_ARGUMENT, "stack error");
              goto exit;
            }
            proc = code->value;
            WITH_SPSAVE({
              grn_proc_call(ctx, proc, code->nargs, expr);
            });
          } else {
            int offset = code->nargs + 1;
            if (sp < s_ + offset) {
              ERR(GRN_INVALID_ARGUMENT, "stack error");
              goto exit;
            }
            proc = sp[-offset];
            WITH_SPSAVE({
              grn_proc_call(ctx, proc, code->nargs, expr);
            });
            if (ctx->rc) {
              goto exit;
            }
            POP1(res);
            {
              grn_obj *proc_;
              POP1(proc_);
              if (proc != proc_) {
                GRN_LOG(ctx, GRN_LOG_WARNING, "stack may be corrupt");
              }
            }
            PUSH1(res);
          }
        }
        code++;
        break;
      case GRN_OP_INTERN :
        {
          grn_obj *obj;
          POP1(obj);
          obj = GRN_OBJ_RESOLVE(ctx, obj);
          res = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj));
          if (!res) { res = grn_ctx_get(ctx, GRN_TEXT_VALUE(obj), GRN_TEXT_LEN(obj)); }
          if (!res) {
            ERR(GRN_INVALID_ARGUMENT, "intern failed");
            goto exit;
          }
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_TABLE_CREATE :
        {
          grn_obj *value_type, *key_type, *flags, *name;
          POP1(value_type);
          value_type = GRN_OBJ_RESOLVE(ctx, value_type);
          POP1(key_type);
          key_type = GRN_OBJ_RESOLVE(ctx, key_type);
          POP1(flags);
          flags = GRN_OBJ_RESOLVE(ctx, flags);
          POP1(name);
          name = GRN_OBJ_RESOLVE(ctx, name);
          res = grn_table_create(ctx, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name),
                                 NULL, GRN_UINT32_VALUE(flags),
                                 key_type, value_type);
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_EXPR_GET_VAR :
        {
          grn_obj *name, *expr;
          POP1(name);
          name = GRN_OBJ_RESOLVE(ctx, name);
          POP1(expr);
          expr = GRN_OBJ_RESOLVE(ctx, expr);
          switch (name->header.domain) {
          case GRN_DB_INT32 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_INT32_VALUE(name));
            break;
          case GRN_DB_UINT32 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_UINT32_VALUE(name));
            break;
          case GRN_DB_INT64 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_INT64_VALUE(name));
            break;
          case GRN_DB_UINT64 :
            res = grn_expr_get_var_by_offset(ctx, expr, (unsigned int) GRN_UINT64_VALUE(name));
            break;
          case GRN_DB_SHORT_TEXT :
          case GRN_DB_TEXT :
          case GRN_DB_LONG_TEXT :
            res = grn_expr_get_var(ctx, expr, GRN_TEXT_VALUE(name), GRN_TEXT_LEN(name));
            break;
          default :
            ERR(GRN_INVALID_ARGUMENT, "invalid type");
            goto exit;
          }
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_ASSIGN :
        {
          grn_obj *value, *var;
          if (code->value) {
            value = code->value;
          } else {
            POP1(value);
          }
          value = GRN_OBJ_RESOLVE(ctx, value);
          POP1(var);
          // var = GRN_OBJ_RESOLVE(ctx, var);
          if (var->header.type == GRN_PTR &&
              GRN_BULK_VSIZE(var) == (sizeof(grn_obj *) + sizeof(grn_id))) {
            grn_obj *col = GRN_PTR_VALUE(var);
            grn_id rid = *(grn_id *)(GRN_BULK_HEAD(var) + sizeof(grn_obj *));
            grn_obj_set_value(ctx, col, rid, value, GRN_OBJ_SET);
          } else {
            VAR_SET_VALUE(ctx, var, value);
          }
          PUSH1(value);
        }
        code++;
        break;
      case GRN_OP_STAR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          FLOAT_ARITHMETIC_OPERATION_STAR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable *= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SLASH_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          INTEGER_ARITHMETIC_OPERATION_SLASH,
          FLOAT_ARITHMETIC_OPERATION_SLASH,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable /= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_MOD_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_MOD,
          INTEGER_ARITHMETIC_OPERATION_MOD,
          INTEGER_ARITHMETIC_OPERATION_MOD,
          INTEGER_ARITHMETIC_OPERATION_MOD,
          FLOAT_ARITHMETIC_OPERATION_MOD,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable %%= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_PLUS_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          FLOAT_ARITHMETIC_OPERATION_PLUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable += \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_MINUS_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          INTEGER_ARITHMETIC_OPERATION_MINUS,
          FLOAT_ARITHMETIC_OPERATION_MINUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable -= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTL_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          FLOAT_ARITHMETIC_OPERATION_SHIFTL,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable <<= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable >>= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_SHIFTRR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER8_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER16_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER32_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER64_ARITHMETIC_OPERATION_SHIFTRR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTRR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "variable >>>= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_AND_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_AND,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable &= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_OR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_OR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable |= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_XOR_ASSIGN :
        ARITHMETIC_OPERATION_AND_ASSIGN_DISPATCH(
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT, "variable ^= \"string\" isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_JUMP :
        code += code->nargs + 1;
        break;
      case GRN_OP_CJUMP :
        {
          grn_obj *v;
          POP1(v);
          if (!grn_obj_is_true(ctx, v)) {
            code += code->nargs;
          }
        }
        code++;
        break;
      case GRN_OP_GET_VALUE :
        {
          grn_obj *col, *rec;
          do {
            if (code->nargs == 1) {
              rec = v0;
              if (code->value) {
                col = code->value;
                ALLOC1(res);
              } else {
                POP1ALLOC1(col, res);
              }
            } else {
              if (code->value) {
                col = code->value;
                POP1ALLOC1(rec, res);
              } else {
                POP2ALLOC1(rec, col, res);
              }
            }
            if (col->header.type == GRN_BULK) {
              grn_obj *table = grn_ctx_at(ctx, GRN_OBJ_GET_DOMAIN(rec));
              col = grn_obj_column(ctx, table, GRN_BULK_HEAD(col), GRN_BULK_VSIZE(col));
              if (col) { grn_expr_take_obj(ctx, (grn_obj *)expr, col); }
            }
            if (!col) {
              ERR(GRN_INVALID_ARGUMENT, "col resolve failed");
              goto exit;
            }
            grn_obj_reinit_for(ctx, res, col);
            grn_obj_get_value(ctx, col, GRN_RECORD_VALUE(rec), res);
            code++;
          } while (code < ce && code->op == GRN_OP_GET_VALUE);
        }
        break;
      case GRN_OP_OBJ_SEARCH :
        {
          grn_obj *op, *query, *index;
          // todo : grn_search_optarg optarg;
          POP1(op);
          op = GRN_OBJ_RESOLVE(ctx, op);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(query);
          query = GRN_OBJ_RESOLVE(ctx, query);
          POP1(index);
          index = GRN_OBJ_RESOLVE(ctx, index);
          grn_obj_search(ctx, index, query, res,
                         (grn_operator)GRN_UINT32_VALUE(op), NULL);
        }
        code++;
        break;
      case GRN_OP_TABLE_SELECT :
        {
          grn_obj *op, *res, *expr, *table;
          POP1(op);
          op = GRN_OBJ_RESOLVE(ctx, op);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(expr);
          expr = GRN_OBJ_RESOLVE(ctx, expr);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          WITH_SPSAVE({
            grn_table_select(ctx, table, expr, res, (grn_operator)GRN_UINT32_VALUE(op));
          });
          PUSH1(res);
        }
        code++;
        break;
      case GRN_OP_TABLE_SORT :
        {
          grn_obj *keys_, *res, *limit, *table;
          POP1(keys_);
          keys_ = GRN_OBJ_RESOLVE(ctx, keys_);
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(limit);
          limit = GRN_OBJ_RESOLVE(ctx, limit);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          {
            grn_table_sort_key *keys;
            const char *p = GRN_BULK_HEAD(keys_), *tokbuf[256];
            int n = grn_str_tok(p, GRN_BULK_VSIZE(keys_), ' ', tokbuf, 256, NULL);
            if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
              int i, n_keys = 0;
              for (i = 0; i < n; i++) {
                uint32_t len = (uint32_t) (tokbuf[i] - p);
                grn_obj *col = grn_obj_column(ctx, table, p, len);
                if (col) {
                  keys[n_keys].key = col;
                  keys[n_keys].flags = GRN_TABLE_SORT_ASC;
                  keys[n_keys].offset = 0;
                  n_keys++;
                } else {
                  if (p[0] == ':' && p[1] == 'd' && len == 2 && n_keys) {
                    keys[n_keys - 1].flags |= GRN_TABLE_SORT_DESC;
                  }
                }
                p = tokbuf[i] + 1;
              }
              WITH_SPSAVE({
                grn_table_sort(ctx, table, 0, GRN_INT32_VALUE(limit), res, keys, n_keys);
              });
              for (i = 0; i < n_keys; i++) {
                grn_obj_unlink(ctx, keys[i].key);
              }
              GRN_FREE(keys);
            }
          }
        }
        code++;
        break;
      case GRN_OP_TABLE_GROUP :
        {
          grn_obj *res, *keys_, *table;
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(keys_);
          keys_ = GRN_OBJ_RESOLVE(ctx, keys_);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          {
            grn_table_sort_key *keys;
            grn_table_group_result results;
            const char *p = GRN_BULK_HEAD(keys_), *tokbuf[256];
            int n = grn_str_tok(p, GRN_BULK_VSIZE(keys_), ' ', tokbuf, 256, NULL);
            if ((keys = GRN_MALLOCN(grn_table_sort_key, n))) {
              int i, n_keys = 0;
              for (i = 0; i < n; i++) {
                uint32_t len = (uint32_t) (tokbuf[i] - p);
                grn_obj *col = grn_obj_column(ctx, table, p, len);
                if (col) {
                  keys[n_keys].key = col;
                  keys[n_keys].flags = GRN_TABLE_SORT_ASC;
                  keys[n_keys].offset = 0;
                  n_keys++;
                } else if (n_keys) {
                  if (p[0] == ':' && p[1] == 'd' && len == 2) {
                    keys[n_keys - 1].flags |= GRN_TABLE_SORT_DESC;
                  } else {
                    keys[n_keys - 1].offset = grn_atoi(p, p + len, NULL);
                  }
                }
                p = tokbuf[i] + 1;
              }
              /* todo : support multi-results */
              results.table = res;
              results.key_begin = 0;
              results.key_end = 0;
              results.limit = 0;
              results.flags = 0;
              results.op = GRN_OP_OR;
              WITH_SPSAVE({
                grn_table_group(ctx, table, keys, n_keys, &results, 1);
              });
              for (i = 0; i < n_keys; i++) {
                grn_obj_unlink(ctx, keys[i].key);
              }
              GRN_FREE(keys);
            }
          }
        }
        code++;
        break;
      case GRN_OP_JSON_PUT :
        {
          grn_obj_format format;
          grn_obj *str, *table, *res;
          POP1(res);
          res = GRN_OBJ_RESOLVE(ctx, res);
          POP1(str);
          str = GRN_OBJ_RESOLVE(ctx, str);
          POP1(table);
          table = GRN_OBJ_RESOLVE(ctx, table);
          GRN_OBJ_FORMAT_INIT(&format, grn_table_size(ctx, table), 0, -1, 0);
          format.flags = 0;
          grn_obj_columns(ctx, table,
                          GRN_TEXT_VALUE(str), GRN_TEXT_LEN(str), &format.columns);
          grn_text_otoj(ctx, res, table, &format);
          GRN_OBJ_FORMAT_FIN(ctx, &format);
        }
        code++;
        break;
      case GRN_OP_AND :
        {
          grn_obj *x, *y;
          grn_obj *result = NULL;
          POP2ALLOC1(x, y, res);
          if (grn_obj_is_true(ctx, x)) {
            if (grn_obj_is_true(ctx, y)) {
              result = y;
            }
          }
          if (result) {
            if (res != result) {
              grn_obj_reinit(ctx, res, result->header.domain, 0);
              grn_obj_cast(ctx, result, res, GRN_FALSE);
            }
          } else {
            grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
            GRN_BOOL_SET(ctx, res, GRN_FALSE);
          }
        }
        code++;
        break;
      case GRN_OP_OR :
        {
          grn_obj *x, *y;
          grn_obj *result;
          POP2ALLOC1(x, y, res);
          if (grn_obj_is_true(ctx, x)) {
            result = x;
          } else {
            if (grn_obj_is_true(ctx, y)) {
              result = y;
            } else {
              result = NULL;
            }
          }
          if (result) {
            if (res != result) {
              grn_obj_reinit(ctx, res, result->header.domain, 0);
              grn_obj_cast(ctx, result, res, GRN_FALSE);
            }
          } else {
            grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
            GRN_BOOL_SET(ctx, res, GRN_FALSE);
          }
        }
        code++;
        break;
      case GRN_OP_AND_NOT :
        {
          grn_obj *x, *y;
          grn_bool is_true;
          POP2ALLOC1(x, y, res);
          if (!grn_obj_is_true(ctx, x) || grn_obj_is_true(ctx, y)) {
            is_true = GRN_FALSE;
          } else {
            is_true = GRN_TRUE;
          }
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, is_true);
        }
        code++;
        break;
      case GRN_OP_ADJUST :
        {
          /* todo */
        }
        code++;
        break;
      case GRN_OP_MATCH :
        {
          grn_obj *x, *y;
          grn_bool matched;
          POP1(y);
          POP1(x);
          WITH_SPSAVE({
            matched = grn_operator_exec_match(ctx, x, y);
          });
          ALLOC1(res);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, matched);
        }
        code++;
        break;
      case GRN_OP_EQUAL :
        {
          grn_bool is_equal;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          is_equal = grn_operator_exec_equal(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, is_equal);
        }
        code++;
        break;
      case GRN_OP_NOT_EQUAL :
        {
          grn_bool is_not_equal;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          is_not_equal = grn_operator_exec_not_equal(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, is_not_equal);
        }
        code++;
        break;
      case GRN_OP_PREFIX :
        {
          grn_obj *x, *y;
          grn_bool matched;
          POP1(y);
          POP1(x);
          WITH_SPSAVE({
            matched = grn_operator_exec_prefix(ctx, x, y);
          });
          ALLOC1(res);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, matched);
        }
        code++;
        break;
      case GRN_OP_SUFFIX :
        {
          grn_obj *x, *y;
          grn_bool matched = GRN_FALSE;
          POP2ALLOC1(x, y, res);
          if (GRN_TEXT_LEN(x) >= GRN_TEXT_LEN(y) &&
              !memcmp(GRN_TEXT_VALUE(x) + GRN_TEXT_LEN(x) - GRN_TEXT_LEN(y),
                      GRN_TEXT_VALUE(y), GRN_TEXT_LEN(y))) {
            matched = GRN_TRUE;
          }
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, matched);
        }
        code++;
        break;
      case GRN_OP_LESS :
        {
          grn_bool r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          r = grn_operator_exec_less(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, r);
        }
        code++;
        break;
      case GRN_OP_GREATER :
        {
          grn_bool r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          r = grn_operator_exec_greater(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, r);
        }
        code++;
        break;
      case GRN_OP_LESS_EQUAL :
        {
          grn_bool r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          r = grn_operator_exec_less_equal(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, r);
        }
        code++;
        break;
      case GRN_OP_GREATER_EQUAL :
        {
          grn_bool r;
          grn_obj *x, *y;
          POP2ALLOC1(x, y, res);
          r = grn_operator_exec_greater_equal(ctx, x, y);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, r);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE1 :
        {
          grn_obj *value;
          double lng1, lat1, lng2, lat2, x, y, d;
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          x = (lng2 - lng1) * cos((lat1 + lat2) * 0.5);
          y = (lat2 - lat1);
          d = sqrt((x * x) + (y * y)) * GEO_RADIOUS;
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE2 :
        {
          grn_obj *value;
          double lng1, lat1, lng2, lat2, x, y, d;
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          x = sin(fabs(lng2 - lng1) * 0.5);
          y = sin(fabs(lat2 - lat1) * 0.5);
          d = asin(sqrt((y * y) + cos(lat1) * cos(lat2) * x * x)) * 2 * GEO_RADIOUS;
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE3 :
        {
          grn_obj *value;
          double lng1, lat1, lng2, lat2, p, q, m, n, x, y, d;
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          p = (lat1 + lat2) * 0.5;
          q = (1 - GEO_BES_C3 * sin(p) * sin(p));
          m = GEO_BES_C1 / sqrt(q * q * q);
          n = GEO_BES_C2 / sqrt(q);
          x = n * cos(p) * fabs(lng1 - lng2);
          y = m * fabs(lat1 - lat2);
          d = sqrt((x * x) + (y * y));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_DISTANCE4 :
        {
          grn_obj *value;
          double lng1, lat1, lng2, lat2, p, q, m, n, x, y, d;
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          p = (lat1 + lat2) * 0.5;
          q = (1 - GEO_GRS_C3 * sin(p) * sin(p));
          m = GEO_GRS_C1 / sqrt(q * q * q);
          n = GEO_GRS_C2 / sqrt(q);
          x = n * cos(p) * fabs(lng1 - lng2);
          y = m * fabs(lat1 - lat2);
          d = sqrt((x * x) + (y * y));
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_FLOAT;
          GRN_FLOAT_SET(ctx, res, d);
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP5 :
        {
          int r;
          grn_obj *value;
          double lng0, lat0, lng1, lat1, x, y, d;
          POP1(value);
          lng0 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat0 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          x = (lng1 - lng0) * cos((lat0 + lat1) * 0.5);
          y = (lat1 - lat0);
          d = sqrt((x * x) + (y * y)) * GEO_RADIOUS;
          switch (value->header.domain) {
          case GRN_DB_INT32 :
            r = d <= GRN_INT32_VALUE(value);
            break;
          case GRN_DB_FLOAT :
            r = d <= GRN_FLOAT_VALUE(value);
            break;
          default :
            r = 0;
            break;
          }
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP6 :
        {
          int r;
          grn_obj *value;
          double lng0, lat0, lng1, lat1, lng2, lat2, x, y, d;
          POP1(value);
          lng0 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat0 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lat1 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1(value);
          lng2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          POP1ALLOC1(value, res);
          lat2 = GEO_INT2RAD(GRN_INT32_VALUE(value));
          x = (lng1 - lng0) * cos((lat0 + lat1) * 0.5);
          y = (lat1 - lat0);
          d = (x * x) + (y * y);
          x = (lng2 - lng1) * cos((lat1 + lat2) * 0.5);
          y = (lat2 - lat1);
          r = d <= (x * x) + (y * y);
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_GEO_WITHINP8 :
        {
          int r;
          grn_obj *value;
          int64_t ln0, la0, ln1, la1, ln2, la2, ln3, la3;
          POP1(value);
          ln0 = GRN_INT32_VALUE(value);
          POP1(value);
          la0 = GRN_INT32_VALUE(value);
          POP1(value);
          ln1 = GRN_INT32_VALUE(value);
          POP1(value);
          la1 = GRN_INT32_VALUE(value);
          POP1(value);
          ln2 = GRN_INT32_VALUE(value);
          POP1(value);
          la2 = GRN_INT32_VALUE(value);
          POP1(value);
          ln3 = GRN_INT32_VALUE(value);
          POP1ALLOC1(value, res);
          la3 = GRN_INT32_VALUE(value);
          r = ((ln2 <= ln0) && (ln0 <= ln3) && (la2 <= la0) && (la0 <= la3));
          GRN_INT32_SET(ctx, res, r);
          res->header.type = GRN_BULK;
          res->header.domain = GRN_DB_INT32;
        }
        code++;
        break;
      case GRN_OP_PLUS :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "+",
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          INTEGER_ARITHMETIC_OPERATION_PLUS,
          FLOAT_ARITHMETIC_OPERATION_PLUS,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            if (x == res) {
              grn_obj_cast(ctx, y, res, GRN_FALSE);
            } else if (y == res) {
              grn_obj buffer;
              GRN_TEXT_INIT(&buffer, 0);
              grn_obj_cast(ctx, x, &buffer, GRN_FALSE);
              grn_obj_cast(ctx, y, &buffer, GRN_FALSE);
              GRN_BULK_REWIND(res);
              grn_obj_cast(ctx, &buffer, res, GRN_FALSE);
              GRN_OBJ_FIN(ctx, &buffer);
            } else {
              GRN_BULK_REWIND(res);
              grn_obj_cast(ctx, x, res, GRN_FALSE);
              grn_obj_cast(ctx, y, res, GRN_FALSE);
            }
          }
          ,);
        break;
      case GRN_OP_MINUS :
        if (code->nargs == 1) {
          ARITHMETIC_UNARY_OPERATION_DISPATCH(
            INTEGER_UNARY_ARITHMETIC_OPERATION_MINUS,
            FLOAT_UNARY_ARITHMETIC_OPERATION_MINUS,
            ARITHMETIC_OPERATION_NO_CHECK,
            ARITHMETIC_OPERATION_NO_CHECK,
            {
              long long int x_;

              res->header.type = GRN_BULK;
              res->header.domain = GRN_DB_INT64;

              GRN_INT64_SET(ctx, res, 0);
              grn_obj_cast(ctx, x, res, GRN_FALSE);
              x_ = GRN_INT64_VALUE(res);

              GRN_INT64_SET(ctx, res, -x_);
            }
            ,);
        } else {
          ARITHMETIC_BINARY_OPERATION_DISPATCH(
            "-",
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            INTEGER_ARITHMETIC_OPERATION_MINUS,
            FLOAT_ARITHMETIC_OPERATION_MINUS,
            ARITHMETIC_OPERATION_NO_CHECK,
            ARITHMETIC_OPERATION_NO_CHECK,
            {
              ERR(GRN_INVALID_ARGUMENT,
                  "\"string\" - \"string\" "
                  "isn't supported");
              goto exit;
            }
            ,);
        }
        break;
      case GRN_OP_STAR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "*",
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          INTEGER_ARITHMETIC_OPERATION_STAR,
          FLOAT_ARITHMETIC_OPERATION_STAR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" * \"string\" "
                "isn't supported");
            goto exit;
          }
          ,);
        break;
      case GRN_OP_SLASH :
        DIVISION_OPERATION_DISPATCH(
          SIGNED_INTEGER_DIVISION_OPERATION_SLASH,
          UNSIGNED_INTEGER_DIVISION_OPERATION_SLASH,
          FLOAT_DIVISION_OPERATION_SLASH,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" / \"string\" "
                "isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_MOD :
        DIVISION_OPERATION_DISPATCH(
          SIGNED_INTEGER_DIVISION_OPERATION_MOD,
          UNSIGNED_INTEGER_DIVISION_OPERATION_MOD,
          FLOAT_DIVISION_OPERATION_MOD,
          {
            ERR(GRN_INVALID_ARGUMENT,
                "\"string\" %% \"string\" "
                "isn't supported");
            goto exit;
          });
        break;
      case GRN_OP_BITWISE_NOT :
        ARITHMETIC_UNARY_OPERATION_DISPATCH(
          INTEGER_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT,
          FLOAT_UNARY_ARITHMETIC_OPERATION_BITWISE_NOT,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_UNARY_ARITHMETIC_OPERATION(~),);
        break;
      case GRN_OP_BITWISE_OR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "|",
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_OR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_OR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(|),);
        break;
      case GRN_OP_BITWISE_XOR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "^",
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_XOR,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_XOR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(^),);
        break;
      case GRN_OP_BITWISE_AND :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "&",
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          INTEGER_ARITHMETIC_OPERATION_BITWISE_AND,
          FLOAT_ARITHMETIC_OPERATION_BITWISE_AND,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(&),);
        break;
      case GRN_OP_SHIFTL :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          "<<",
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          INTEGER_ARITHMETIC_OPERATION_SHIFTL,
          FLOAT_ARITHMETIC_OPERATION_SHIFTL,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(<<),);
        break;
      case GRN_OP_SHIFTR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          ">>",
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          INTEGER_ARITHMETIC_OPERATION_SHIFTR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          TEXT_ARITHMETIC_OPERATION(>>),);
        break;
      case GRN_OP_SHIFTRR :
        ARITHMETIC_BINARY_OPERATION_DISPATCH(
          ">>>",
          INTEGER8_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER16_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER32_ARITHMETIC_OPERATION_SHIFTRR,
          INTEGER64_ARITHMETIC_OPERATION_SHIFTRR,
          FLOAT_ARITHMETIC_OPERATION_SHIFTRR,
          ARITHMETIC_OPERATION_NO_CHECK,
          ARITHMETIC_OPERATION_NO_CHECK,
          {
            long long unsigned int x_;
            long long unsigned int y_;

            res->header.type = GRN_BULK;
            res->header.domain = GRN_DB_INT64;

            GRN_INT64_SET(ctx, res, 0);
            grn_obj_cast(ctx, x, res, GRN_FALSE);
            x_ = GRN_INT64_VALUE(res);

            GRN_INT64_SET(ctx, res, 0);
            grn_obj_cast(ctx, y, res, GRN_FALSE);
            y_ = GRN_INT64_VALUE(res);

            GRN_INT64_SET(ctx, res, x_ >> y_);
          }
          ,);
        break;
      case GRN_OP_INCR :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE, 1, GRN_OBJ_INCR);
        break;
      case GRN_OP_DECR :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE, 1, GRN_OBJ_DECR);
        break;
      case GRN_OP_INCR_POST :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE_POST, 1, GRN_OBJ_INCR);
        break;
      case GRN_OP_DECR_POST :
        UNARY_OPERATE_AND_ASSIGN_DISPATCH(EXEC_OPERATE_POST, 1, GRN_OBJ_DECR);
        break;
      case GRN_OP_NOT :
        {
          grn_obj *value;
          grn_bool value_boolean;
          POP1ALLOC1(value, res);
          GRN_OBJ_IS_TRUE(ctx, value, value_boolean);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, !value_boolean);
        }
        code++;
        break;
      case GRN_OP_GET_MEMBER :
        {
          grn_obj *receiver, *index_or_key;
          POP2ALLOC1(receiver, index_or_key, res);
          if (receiver->header.type == GRN_PTR) {
            grn_obj *index = index_or_key;
            grn_expr_exec_get_member_vector(ctx, expr, receiver, index, res);
          } else {
            grn_obj *key = index_or_key;
            grn_expr_exec_get_member_table(ctx, expr, receiver, key, res);
          }
          code++;
        }
        break;
      case GRN_OP_REGEXP :
        {
          grn_obj *target, *pattern;
          grn_bool matched;
          POP1(pattern);
          POP1(target);
          WITH_SPSAVE({
            matched = grn_operator_exec_regexp(ctx, target, pattern);
          });
          ALLOC1(res);
          grn_obj_reinit(ctx, res, GRN_DB_BOOL, 0);
          GRN_BOOL_SET(ctx, res, matched);
        }
        code++;
        break;
      default :
        ERR(GRN_FUNCTION_NOT_IMPLEMENTED, "not implemented operator assigned");
        goto exit;
        break;
      }
    }
    ctx->impl->stack_curr = sp - s_;
  }
  if (ctx->impl->stack_curr + nargs > stack_curr) {
    val = grn_ctx_pop(ctx);
  }
exit :
  if (ctx->impl->stack_curr + nargs > stack_curr) {
    /*
      GRN_LOG(ctx, GRN_LOG_WARNING, "nargs=%d stack balance=%d",
      nargs, stack_curr - ctx->impl->stack_curr);
    */
    ctx->impl->stack_curr = stack_curr - nargs;
  }
  GRN_API_RETURN(val);
}

grn_obj *
grn_expr_get_value(grn_ctx *ctx, grn_obj *expr, int offset)
{
  grn_obj *res = NULL;
  grn_expr *e = (grn_expr *)expr;
  GRN_API_ENTER;
  if (0 <= offset && offset < e->values_size) {
    res = &e->values[offset];
  }
  GRN_API_RETURN(res);
}

#define DEFAULT_WEIGHT 5
#define DEFAULT_DECAYSTEP 2
#define DEFAULT_MAX_INTERVAL 10
#define DEFAULT_SIMILARITY_THRESHOLD 0
#define DEFAULT_TERM_EXTRACT_POLICY 0
#define DEFAULT_WEIGHT_VECTOR_SIZE 4096

#define GRN_SCAN_INFO_MAX_N_ARGS 128

struct _grn_scan_info {
  uint32_t start;
  uint32_t end;
  int32_t nargs;
  int flags;
  grn_operator op;
  grn_operator logical_op;
  grn_obj wv;
  grn_obj index;
  grn_obj *query;
  grn_obj *args[GRN_SCAN_INFO_MAX_N_ARGS];
  int max_interval;
  int similarity_threshold;
  grn_obj scorers;
  grn_obj scorer_args_exprs;
  grn_obj scorer_args_expr_offsets;
  struct {
    grn_bool specified;
    int start;
  } position;
};

#define SI_FREE(si) do {\
  GRN_OBJ_FIN(ctx, &(si)->wv);\
  GRN_OBJ_FIN(ctx, &(si)->index);\
  GRN_OBJ_FIN(ctx, &(si)->scorers);\
  GRN_OBJ_FIN(ctx, &(si)->scorer_args_exprs);\
  GRN_OBJ_FIN(ctx, &(si)->scorer_args_expr_offsets);\
  GRN_FREE(si);\
} while (0)

#define SI_ALLOC(si, i, st) do {\
  if (!((si) = GRN_MALLOCN(scan_info, 1))) {\
    int j;\
    for (j = 0; j < i; j++) { SI_FREE(sis[j]); }\
    GRN_FREE(sis);\
    return NULL;\
  }\
  GRN_INT32_INIT(&(si)->wv, GRN_OBJ_VECTOR);\
  GRN_PTR_INIT(&(si)->index, GRN_OBJ_VECTOR, GRN_ID_NIL);\
  (si)->logical_op = GRN_OP_OR;\
  (si)->flags = SCAN_PUSH;\
  (si)->nargs = 0;\
  (si)->max_interval = DEFAULT_MAX_INTERVAL;\
  (si)->similarity_threshold = DEFAULT_SIMILARITY_THRESHOLD;\
  (si)->start = (st);\
  GRN_PTR_INIT(&(si)->scorers, GRN_OBJ_VECTOR, GRN_ID_NIL);\
  GRN_PTR_INIT(&(si)->scorer_args_exprs, GRN_OBJ_VECTOR, GRN_ID_NIL);\
  GRN_UINT32_INIT(&(si)->scorer_args_expr_offsets, GRN_OBJ_VECTOR);\
  (si)->position.specified = GRN_FALSE;\
  (si)->position.start = 0;\
} while (0)

static scan_info **
put_logical_op(grn_ctx *ctx, scan_info **sis, int *ip, grn_operator op, int start)
{
  int nparens = 1, ndifops = 0, i = *ip, j = i, r = 0;
  while (j--) {
    scan_info *s_ = sis[j];
    if (s_->flags & SCAN_POP) {
      ndifops++;
      nparens++;
    } else {
      if (s_->flags & SCAN_PUSH) {
        if (!(--nparens)) {
          if (!r) {
            if (ndifops) {
              if (j && op != GRN_OP_AND_NOT) {
                nparens = 1;
                ndifops = 0;
                r = j;
              } else {
                SI_ALLOC(s_, i, start);
                s_->flags = SCAN_POP;
                s_->logical_op = op;
                sis[i++] = s_;
                *ip = i;
                break;
              }
            } else {
              s_->flags &= ~SCAN_PUSH;
              s_->logical_op = op;
              break;
            }
          } else {
            if (ndifops) {
              SI_ALLOC(s_, i, start);
              s_->flags = SCAN_POP;
              s_->logical_op = op;
              sis[i++] = s_;
              *ip = i;
            } else {
              s_->flags &= ~SCAN_PUSH;
              s_->logical_op = op;
              grn_memcpy(&sis[i], &sis[j], sizeof(scan_info *) * (r - j));
              grn_memmove(&sis[j], &sis[r], sizeof(scan_info *) * (i - r));
              grn_memcpy(&sis[i + j - r], &sis[i], sizeof(scan_info *) * (r - j));
            }
            break;
          }
        }
      } else {
        if ((op == GRN_OP_AND_NOT) || (op != s_->logical_op)) {
          ndifops++;
        }
      }
    }
  }
  if (j < 0) {
    ERR(GRN_INVALID_ARGUMENT, "unmatched nesting level");
    for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
    GRN_FREE(sis);
    return NULL;
  }
  return sis;
}

/* TODO: Remove me if nobody doesn't want to reuse the implementation again. */
#if 0
static const char *opstrs[] = {
  "PUSH",
  "POP",
  "NOP",
  "CALL",
  "INTERN",
  "GET_REF",
  "GET_VALUE",
  "AND",
  "AND_NOT",
  "OR",
  "ASSIGN",
  "STAR_ASSIGN",
  "SLASH_ASSIGN",
  "MOD_ASSIGN",
  "PLUS_ASSIGN",
  "MINUS_ASSIGN",
  "SHIFTL_ASSIGN",
  "SHIFTR_ASSIGN",
  "SHIFTRR_ASSIGN",
  "AND_ASSIGN",
  "XOR_ASSIGN",
  "OR_ASSIGN",
  "JUMP",
  "CJUMP",
  "COMMA",
  "BITWISE_OR",
  "BITWISE_XOR",
  "BITWISE_AND",
  "BITWISE_NOT",
  "EQUAL",
  "NOT_EQUAL",
  "LESS",
  "GREATER",
  "LESS_EQUAL",
  "GREATER_EQUAL",
  "IN",
  "MATCH",
  "NEAR",
  "NEAR2",
  "SIMILAR",
  "TERM_EXTRACT",
  "SHIFTL",
  "SHIFTR",
  "SHIFTRR",
  "PLUS",
  "MINUS",
  "STAR",
  "SLASH",
  "MOD",
  "DELETE",
  "INCR",
  "DECR",
  "INCR_POST",
  "DECR_POST",
  "NOT",
  "ADJUST",
  "EXACT",
  "LCP",
  "PARTIAL",
  "UNSPLIT",
  "PREFIX",
  "SUFFIX",
  "GEO_DISTANCE1",
  "GEO_DISTANCE2",
  "GEO_DISTANCE3",
  "GEO_DISTANCE4",
  "GEO_WITHINP5",
  "GEO_WITHINP6",
  "GEO_WITHINP8",
  "OBJ_SEARCH",
  "EXPR_GET_VAR",
  "TABLE_CREATE",
  "TABLE_SELECT",
  "TABLE_SORT",
  "TABLE_GROUP",
  "JSON_PUT"
};

static void
put_value(grn_ctx *ctx, grn_obj *buf, grn_obj *obj)
{
  int len;
  char namebuf[GRN_TABLE_MAX_KEY_SIZE];
  if ((len = grn_column_name(ctx, obj, namebuf, GRN_TABLE_MAX_KEY_SIZE))) {
    GRN_TEXT_PUT(ctx, buf, namebuf, len);
  } else {
    grn_text_otoj(ctx, buf, obj, NULL);
  }
}

static grn_rc
grn_expr_inspect_internal(grn_ctx *ctx, grn_obj *buf, grn_obj *expr)
{
  uint32_t i, j;
  grn_expr_var *var;
  grn_expr_code *code;
  grn_expr *e = (grn_expr *)expr;
  grn_hash *vars = grn_expr_get_vars(ctx, expr, &i);
  GRN_TEXT_PUTS(ctx, buf, "noname");
  GRN_TEXT_PUTC(ctx, buf, '(');
  {
    int i = 0;
    grn_obj *value;
    const char *name;
    uint32_t name_len;
    GRN_HASH_EACH(ctx, vars, id, &name, &name_len, &value, {
      if (i++) { GRN_TEXT_PUTC(ctx, buf, ','); }
      GRN_TEXT_PUT(ctx, buf, name, name_len);
      GRN_TEXT_PUTC(ctx, buf, ':');
      put_value(ctx, buf, value);
    });
  }
  GRN_TEXT_PUTC(ctx, buf, ')');
  GRN_TEXT_PUTC(ctx, buf, '{');
  for (j = 0, code = e->codes; j < e->codes_curr; j++, code++) {
    if (j) { GRN_TEXT_PUTC(ctx, buf, ','); }
    grn_text_itoa(ctx, buf, code->modify);
    if (code->op == GRN_OP_PUSH) {
      for (i = 0, var = e->vars; i < e->nvars; i++, var++) {
        if (&var->value == code->value) {
          GRN_TEXT_PUTC(ctx, buf, '?');
          if (var->name_size) {
            GRN_TEXT_PUT(ctx, buf, var->name, var->name_size);
          } else {
            grn_text_itoa(ctx, buf, (int)i);
          }
          break;
        }
      }
      if (i == e->nvars) {
        put_value(ctx, buf, code->value);
      }
    } else {
      if (code->value) {
        put_value(ctx, buf, code->value);
        GRN_TEXT_PUTC(ctx, buf, ' ');
      }
      GRN_TEXT_PUTS(ctx, buf, opstrs[code->op]);
    }
  }
  GRN_TEXT_PUTC(ctx, buf, '}');
  return GRN_SUCCESS;
}

#define EXPRLOG(name,expr) do {\
  grn_obj strbuf;\
  GRN_TEXT_INIT(&strbuf, 0);\
  grn_expr_inspect_internal(ctx, &strbuf, (expr));\
  GRN_TEXT_PUTC(ctx, &strbuf, '\0');\
  GRN_LOG(ctx, GRN_LOG_NOTICE, "%s=(%s)", (name), GRN_TEXT_VALUE(&strbuf));\
  GRN_OBJ_FIN(ctx, &strbuf);\
} while (0)
#endif


static void
scan_info_put_index(grn_ctx *ctx, scan_info *si,
                    grn_obj *index, uint32_t sid, int32_t weight,
                    grn_obj *scorer,
                    grn_obj *scorer_args_expr,
                    uint32_t scorer_args_expr_offset)
{
  GRN_PTR_PUT(ctx, &si->index, index);
  GRN_UINT32_PUT(ctx, &si->wv, sid);
  GRN_INT32_PUT(ctx, &si->wv, weight);
  GRN_PTR_PUT(ctx, &si->scorers, scorer);
  GRN_PTR_PUT(ctx, &si->scorer_args_exprs, scorer_args_expr);
  GRN_UINT32_PUT(ctx, &si->scorer_args_expr_offsets, scorer_args_expr_offset);
  {
    int i, ni = (GRN_BULK_VSIZE(&si->index) / sizeof(grn_obj *)) - 1;
    grn_obj **pi = &GRN_PTR_VALUE_AT(&si->index, ni);
    for (i = 0; i < ni; i++, pi--) {
      if (index == pi[-1]) {
        if (i) {
          int32_t *pw = &GRN_INT32_VALUE_AT(&si->wv, (ni - i) * 2);
          grn_memmove(pw + 2, pw, sizeof(int32_t) * 2 * i);
          pw[0] = (int32_t) sid;
          pw[1] = weight;
          grn_memmove(pi + 1, pi, sizeof(grn_obj *) * i);
          pi[0] = index;
        }
        return;
      }
    }
  }
}

static int32_t
get_weight(grn_ctx *ctx, grn_expr_code *ec, uint32_t *offset)
{
  if (ec->modify == 2 && ec[2].op == GRN_OP_STAR &&
      ec[1].value && ec[1].value->header.type == GRN_BULK) {
    if (offset) {
      *offset = 2;
    }
    if (ec[1].value->header.domain == GRN_DB_INT32 ||
        ec[1].value->header.domain == GRN_DB_UINT32) {
      return GRN_INT32_VALUE(ec[1].value);
    } else {
      int32_t weight = 1;
      grn_obj weight_buffer;
      GRN_INT32_INIT(&weight_buffer, 0);
      if (!grn_obj_cast(ctx, ec[1].value, &weight_buffer, GRN_FALSE)) {
        weight = GRN_INT32_VALUE(&weight_buffer);
      }
      grn_obj_unlink(ctx, &weight_buffer);
      return weight;
    }
  } else {
    if (offset) {
      *offset = 0;
    }
    return 1;
  }
}

scan_info *
grn_scan_info_open(grn_ctx *ctx, int start)
{
  scan_info *si = GRN_MALLOCN(scan_info, 1);

  if (!si) {
    return NULL;
  }

  GRN_INT32_INIT(&si->wv, GRN_OBJ_VECTOR);
  GRN_PTR_INIT(&si->index, GRN_OBJ_VECTOR, GRN_ID_NIL);
  si->logical_op = GRN_OP_OR;
  si->flags = SCAN_PUSH;
  si->nargs = 0;
  si->max_interval = DEFAULT_MAX_INTERVAL;
  si->similarity_threshold = DEFAULT_SIMILARITY_THRESHOLD;
  si->start = start;
  GRN_PTR_INIT(&si->scorers, GRN_OBJ_VECTOR, GRN_ID_NIL);
  GRN_PTR_INIT(&si->scorer_args_exprs, GRN_OBJ_VECTOR, GRN_ID_NIL);
  GRN_UINT32_INIT(&si->scorer_args_expr_offsets, GRN_OBJ_VECTOR);
  si->position.specified = GRN_FALSE;
  si->position.start = 0;

  return si;
}

void
grn_scan_info_close(grn_ctx *ctx, scan_info *si)
{
  SI_FREE(si);
}

void
grn_scan_info_put_index(grn_ctx *ctx, scan_info *si,
                        grn_obj *index, uint32_t sid, int32_t weight,
                        grn_obj *scorer,
                        grn_obj *scorer_args_expr,
                        uint32_t scorer_args_expr_offset)
{
  scan_info_put_index(ctx, si, index, sid, weight,
                      scorer,
                      scorer_args_expr,
                      scorer_args_expr_offset);
}

scan_info **
grn_scan_info_put_logical_op(grn_ctx *ctx, scan_info **sis, int *ip,
                             grn_operator op, int start)
{
  return put_logical_op(ctx, sis, ip, op, start);
}

int32_t
grn_expr_code_get_weight(grn_ctx *ctx, grn_expr_code *ec, uint32_t *offset)
{
  return get_weight(ctx, ec, offset);
}

int
grn_scan_info_get_flags(scan_info *si)
{
  return si->flags;
}

void
grn_scan_info_set_flags(scan_info *si, int flags)
{
  si->flags = flags;
}

grn_operator
grn_scan_info_get_logical_op(scan_info *si)
{
  return si->logical_op;
}

void
grn_scan_info_set_logical_op(scan_info *si, grn_operator logical_op)
{
  si->logical_op = logical_op;
}

grn_operator
grn_scan_info_get_op(scan_info *si)
{
  return si->op;
}

void
grn_scan_info_set_op(scan_info *si, grn_operator op)
{
  si->op = op;
}

void
grn_scan_info_set_end(scan_info *si, uint32_t end)
{
  si->end = end;
}

void
grn_scan_info_set_query(scan_info *si, grn_obj *query)
{
  si->query = query;
}

int
grn_scan_info_get_max_interval(scan_info *si)
{
  return si->max_interval;
}

void
grn_scan_info_set_max_interval(scan_info *si, int max_interval)
{
  si->max_interval = max_interval;
}

int
grn_scan_info_get_similarity_threshold(scan_info *si)
{
  return si->similarity_threshold;
}

void
grn_scan_info_set_similarity_threshold(scan_info *si, int similarity_threshold)
{
  si->similarity_threshold = similarity_threshold;
}

grn_bool
grn_scan_info_push_arg(scan_info *si, grn_obj *arg)
{
  if (si->nargs >= GRN_SCAN_INFO_MAX_N_ARGS) {
    return GRN_FALSE;
  }

  si->args[si->nargs++] = arg;
  return GRN_TRUE;
}

grn_obj *
grn_scan_info_get_arg(grn_ctx *ctx, scan_info *si, int i)
{
  if (i >= si->nargs) {
    return NULL;
  }
  return si->args[i];
}

int
grn_scan_info_get_start_position(scan_info *si)
{
  return si->position.start;
}

void
grn_scan_info_set_start_position(scan_info *si, int start)
{
  si->position.specified = GRN_TRUE;
  si->position.start = start;
}

void
grn_scan_info_reset_position(scan_info *si)
{
  si->position.specified = GRN_FALSE;
}

static uint32_t
scan_info_build_match_expr_codes_find_index(grn_ctx *ctx, scan_info *si,
                                            grn_expr *expr, uint32_t i,
                                            grn_obj **index,
                                            int *sid)
{
  grn_expr_code *ec;
  uint32_t offset = 1;
  grn_index_datum index_datum;
  unsigned int n_index_data = 0;

  ec = &(expr->codes[i]);
  switch (ec->value->header.type) {
  case GRN_ACCESSOR :
    n_index_data = grn_column_find_index_data(ctx, ec->value, si->op,
                                              &index_datum, 1);
    if (n_index_data > 0) {
      grn_accessor *a = (grn_accessor *)(ec->value);
      *sid = index_datum.section;
      if (a->next && a->obj != index_datum.index) {
        *index = ec->value;
      } else {
        *index = index_datum.index;
      }
    }
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    n_index_data = grn_column_find_index_data(ctx, ec->value, si->op,
                                              &index_datum, 1);
    if (n_index_data > 0) {
      *index = index_datum.index;
      *sid = index_datum.section;
    }
    break;
  case GRN_COLUMN_INDEX :
    {
      uint32_t n_rest_codes;

      *index = ec->value;

      n_rest_codes = expr->codes_curr - i;
      if (n_rest_codes >= 2 &&
          ec[1].value &&
          (ec[1].value->header.domain == GRN_DB_INT32 ||
           ec[1].value->header.domain == GRN_DB_UINT32) &&
          ec[2].op == GRN_OP_GET_MEMBER) {
        if (ec[1].value->header.domain == GRN_DB_INT32) {
          *sid = GRN_INT32_VALUE(ec[1].value) + 1;
        } else {
          *sid = GRN_UINT32_VALUE(ec[1].value) + 1;
        }
        offset += 2;
      }
    }
    break;
  default :
    break;
  }

  return offset;
}

static uint32_t
scan_info_build_match_expr_codes(grn_ctx *ctx, scan_info *si,
                                 grn_expr *expr, uint32_t i)
{
  grn_expr_code *ec;
  grn_obj *index = NULL;
  int sid = 0;
  uint32_t offset = 0;

  ec = &(expr->codes[i]);
  if (!ec->value) {
    return i + 1;
  }

  switch (ec->value->header.type) {
  case GRN_ACCESSOR :
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
  case GRN_COLUMN_INDEX :
    offset = scan_info_build_match_expr_codes_find_index(ctx, si, expr, i,
                                                         &index, &sid);
    i += offset - 1;
    if (index) {
      if (ec->value->header.type == GRN_ACCESSOR) {
        si->flags |= SCAN_ACCESSOR;
      }
      scan_info_put_index(ctx, si, index, sid,
                          get_weight(ctx, &(expr->codes[i]), &offset),
                          NULL, NULL, 0);
      i += offset;
    }
    break;
  case GRN_PROC :
    if (!grn_obj_is_scorer_proc(ctx, ec->value)) {
      grn_obj inspected;
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, ec->value);
      ERR(GRN_INVALID_ARGUMENT,
          "procedure must be scorer: <%.*s>",
          (int)GRN_TEXT_LEN(&inspected),
          GRN_TEXT_VALUE(&inspected));
      GRN_OBJ_FIN(ctx, &inspected);
      return expr->codes_curr;
    }
    i++;
    offset = scan_info_build_match_expr_codes_find_index(ctx, si, expr, i,
                                                         &index, &sid);
    i += offset;
    if (index) {
      uint32_t scorer_args_expr_offset = 0;
      if (expr->codes[i].op != GRN_OP_CALL) {
        scorer_args_expr_offset = i;
      }
      while (i < expr->codes_curr && expr->codes[i].op != GRN_OP_CALL) {
        i++;
      }
      scan_info_put_index(ctx, si, index, sid,
                          get_weight(ctx, &(expr->codes[i]), &offset),
                          ec->value,
                          (grn_obj *)expr,
                          scorer_args_expr_offset);
      i += offset;
    }
    break;
  default :
    {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_obj_name(ctx, ec->value, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "invalid match target: <%.*s>",
          name_size, name);
      return expr->codes_curr;
    }
    break;
  }

  return i + 1;
}

static void
scan_info_build_match_expr(grn_ctx *ctx, scan_info *si, grn_expr *expr)
{
  uint32_t i;
  i = 0;
  while (i < expr->codes_curr) {
    i = scan_info_build_match_expr_codes(ctx, si, expr, i);
  }
}

static grn_bool
is_index_searchable_regexp(grn_ctx *ctx, grn_obj *regexp)
{
  const char *regexp_raw;
  const char *regexp_raw_end;
  grn_bool escaping = GRN_FALSE;

  if (!(regexp->header.domain == GRN_DB_SHORT_TEXT ||
        regexp->header.domain == GRN_DB_TEXT ||
        regexp->header.domain == GRN_DB_LONG_TEXT)) {
    return GRN_FALSE;
  }

  regexp_raw = GRN_TEXT_VALUE(regexp);
  regexp_raw_end = regexp_raw + GRN_TEXT_LEN(regexp);

  while (regexp_raw < regexp_raw_end) {
    unsigned int char_len;

    char_len = grn_charlen(ctx, regexp_raw, regexp_raw_end);
    if (char_len == 0) {
      return GRN_FALSE;
    }

    if (char_len == 1) {
      if (escaping) {
        escaping = GRN_FALSE;
        switch (regexp_raw[0]) {
        case 'Z' :
        case 'b' :
        case 'B' :
        case 'd' :
        case 'D' :
        case 'h' :
        case 'H' :
        case 'p' :
        case 's' :
        case 'S' :
        case 'w' :
        case 'W' :
        case 'X' :
        case 'k' :
        case 'g' :
        case '1' :
        case '2' :
        case '3' :
        case '4' :
        case '5' :
        case '6' :
        case '7' :
        case '8' :
        case '9' :
          return GRN_FALSE;
        default :
          break;
        }
      } else {
        switch (regexp_raw[0]) {
        case '.' :
        case '[' :
        case ']' :
        case '|' :
        case '?' :
        case '+' :
        case '*' :
        case '{' :
        case '}' :
        case '^' :
        case '$' :
        case '(' :
        case ')' :
          escaping = GRN_FALSE;
          return GRN_FALSE;
        case '\\' :
          escaping = GRN_TRUE;
          break;
        default :
          escaping = GRN_FALSE;
          break;
        }
      }
    } else {
      escaping = GRN_FALSE;
    }

    regexp_raw += char_len;
  }

  return GRN_TRUE;
}

static void
scan_info_build_match(grn_ctx *ctx, scan_info *si)
{
  grn_obj **p, **pe;

  if (si->op == GRN_OP_REGEXP) {
    p = si->args;
    pe = si->args + si->nargs;
    for (; p < pe; p++) {
      if ((*p)->header.type == GRN_BULK &&
          !is_index_searchable_regexp(ctx, *p)) {
        return;
      }
    }
  }

  p = si->args;
  pe = si->args + si->nargs;
  for (; p < pe; p++) {
    if ((*p)->header.type == GRN_EXPR) {
      scan_info_build_match_expr(ctx, si, (grn_expr *)(*p));
    } else if ((*p)->header.type == GRN_COLUMN_INDEX) {
      scan_info_put_index(ctx, si, *p, 0, 1, NULL, NULL, 0);
    } else if (grn_obj_is_proc(ctx, *p)) {
      break;
    } else if (GRN_DB_OBJP(*p)) {
      grn_index_datum index_datum;
      unsigned int n_index_data;
      n_index_data = grn_column_find_index_data(ctx, *p, si->op,
                                                &index_datum, 1);
      if (n_index_data > 0) {
        scan_info_put_index(ctx, si,
                            index_datum.index, index_datum.section, 1,
                            NULL, NULL, 0);
      }
    } else if (GRN_ACCESSORP(*p)) {
      grn_index_datum index_datum;
      unsigned int n_index_data;
      si->flags |= SCAN_ACCESSOR;
      n_index_data = grn_column_find_index_data(ctx, *p, si->op,
                                                &index_datum, 1);
      if (n_index_data > 0) {
        grn_obj *index;
        if (((grn_accessor *)(*p))->next) {
          index = *p;
        } else {
          index = index_datum.index;
        }
        scan_info_put_index(ctx, si,
                            index, index_datum.section, 1,
                            NULL, NULL, 0);
      }
    } else {
      switch (si->op) {
      case GRN_OP_NEAR :
      case GRN_OP_NEAR2 :
        if (si->nargs == 3 &&
            *p == si->args[2] &&
            (*p)->header.domain == GRN_DB_INT32) {
          si->max_interval = GRN_INT32_VALUE(*p);
        } else {
          si->query = *p;
        }
        break;
      case GRN_OP_SIMILAR :
        if (si->nargs == 3 &&
            *p == si->args[2] &&
            (*p)->header.domain == GRN_DB_INT32) {
          si->similarity_threshold = GRN_INT32_VALUE(*p);
        } else {
          si->query = *p;
        }
        break;
      default :
        si->query = *p;
        break;
      }
    }
  }
}

scan_info **
grn_scan_info_build(grn_ctx *ctx, grn_obj *expr, int *n,
                    grn_operator op, grn_bool record_exist)
{
  grn_obj *var;
  scan_stat stat;
  int i, m = 0, o = 0;
  scan_info **sis, *si = NULL;
  grn_expr_code *c, *ce;
  grn_expr *e = (grn_expr *)expr;
#ifdef GRN_WITH_MRUBY
  if (ctx->impl->mrb.state) {
    return grn_mrb_scan_info_build(ctx, expr, n, op, record_exist);
  }
#endif
  if (!(var = grn_expr_get_var_by_offset(ctx, expr, 0))) { return NULL; }
  for (stat = SCAN_START, c = e->codes, ce = &e->codes[e->codes_curr]; c < ce; c++) {
    switch (c->op) {
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_TERM_EXTRACT :
    case GRN_OP_REGEXP :
      if (stat < SCAN_COL1 || SCAN_CONST < stat) { return NULL; }
      stat = SCAN_START;
      m++;
      break;
    case GRN_OP_BITWISE_OR :
    case GRN_OP_BITWISE_XOR :
    case GRN_OP_BITWISE_AND :
    case GRN_OP_BITWISE_NOT :
    case GRN_OP_SHIFTL :
    case GRN_OP_SHIFTR :
    case GRN_OP_SHIFTRR :
    case GRN_OP_PLUS :
    case GRN_OP_MINUS :
    case GRN_OP_STAR :
    case GRN_OP_MOD :
      if (stat < SCAN_COL1 || SCAN_CONST < stat) { return NULL; }
      stat = SCAN_START;
      if (m != o + 1) { return NULL; }
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_AND_NOT :
    case GRN_OP_ADJUST :
      if (stat != SCAN_START) { return NULL; }
      o++;
      if (o >= m) { return NULL; }
      break;
    case GRN_OP_PUSH :
      stat = (c->value == var) ? SCAN_VAR : SCAN_CONST;
      break;
    case GRN_OP_GET_VALUE :
      switch (stat) {
      case SCAN_START :
      case SCAN_CONST :
      case SCAN_VAR :
        stat = SCAN_COL1;
        break;
      case SCAN_COL1 :
        stat = SCAN_COL2;
        break;
      case SCAN_COL2 :
        break;
      default :
        return NULL;
        break;
      }
      break;
    case GRN_OP_CALL :
      if ((c->flags & GRN_EXPR_CODE_RELATIONAL_EXPRESSION) || c + 1 == ce) {
        stat = SCAN_START;
        m++;
      } else {
        stat = SCAN_COL2;
      }
      break;
    case GRN_OP_GET_REF :
      switch (stat) {
      case SCAN_START :
        stat = SCAN_COL1;
        break;
      default :
        return NULL;
        break;
      }
      break;
    case GRN_OP_GET_MEMBER :
      switch (stat) {
      case SCAN_CONST :
        {
          grn_expr_code *prev_c = c - 1;
          if (prev_c->value->header.domain < GRN_DB_INT8 ||
              prev_c->value->header.domain > GRN_DB_UINT64) {
            return NULL;
          }
        }
        stat = SCAN_COL1;
        break;
      default :
        return NULL;
        break;
      }
      break;
    default :
      return NULL;
      break;
    }
  }
  if (stat || m != o + 1) { return NULL; }
  if (!(sis = GRN_MALLOCN(scan_info *, m + m + o))) { return NULL; }
  for (i = 0, stat = SCAN_START, c = e->codes, ce = &e->codes[e->codes_curr]; c < ce; c++) {
    switch (c->op) {
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_PREFIX :
    case GRN_OP_SUFFIX :
    case GRN_OP_EQUAL :
    case GRN_OP_NOT_EQUAL :
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
    case GRN_OP_GEO_WITHINP5 :
    case GRN_OP_GEO_WITHINP6 :
    case GRN_OP_GEO_WITHINP8 :
    case GRN_OP_TERM_EXTRACT :
    case GRN_OP_REGEXP :
      stat = SCAN_START;
      si->op = c->op;
      si->end = c - e->codes;
      sis[i++] = si;
      scan_info_build_match(ctx, si);
      if (ctx->rc != GRN_SUCCESS) {
        int j;
        for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
        GRN_FREE(sis);
        return NULL;
      }
      si = NULL;
      break;
    case GRN_OP_AND :
    case GRN_OP_OR :
    case GRN_OP_AND_NOT :
    case GRN_OP_ADJUST :
      if (!put_logical_op(ctx, sis, &i, c->op, c - e->codes)) { return NULL; }
      stat = SCAN_START;
      break;
    case GRN_OP_PUSH :
      if (!si) { SI_ALLOC(si, i, c - e->codes); }
      if (c->value == var) {
        stat = SCAN_VAR;
      } else {
        if (si->nargs < GRN_SCAN_INFO_MAX_N_ARGS) {
          si->args[si->nargs++] = c->value;
        }
        if (stat == SCAN_START) { si->flags |= SCAN_PRE_CONST; }
        stat = SCAN_CONST;
      }
      break;
    case GRN_OP_GET_VALUE :
      switch (stat) {
      case SCAN_START :
        if (!si) { SI_ALLOC(si, i, c - e->codes); }
        // fallthru
      case SCAN_CONST :
      case SCAN_VAR :
        stat = SCAN_COL1;
        if (si->nargs < GRN_SCAN_INFO_MAX_N_ARGS) {
          si->args[si->nargs++] = c->value;
        }
        break;
      case SCAN_COL1 :
        {
          int j;
          grn_obj inspected;
          GRN_TEXT_INIT(&inspected, 0);
          GRN_TEXT_PUTS(ctx, &inspected, "<");
          grn_inspect_name(ctx, &inspected, c->value);
          GRN_TEXT_PUTS(ctx, &inspected, ">: <");
          grn_inspect(ctx, &inspected, expr);
          GRN_TEXT_PUTS(ctx, &inspected, ">");
          ERR(GRN_INVALID_ARGUMENT,
              "invalid expression: can't use column as a value: %.*s",
              (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
          GRN_OBJ_FIN(ctx, &inspected);
          SI_FREE(si);
          for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
          GRN_FREE(sis);
          return NULL;
        }
        stat = SCAN_COL2;
        break;
      case SCAN_COL2 :
        break;
      default :
        break;
      }
      break;
    case GRN_OP_CALL :
      if (!si) { SI_ALLOC(si, i, c - e->codes); }
      if ((c->flags & GRN_EXPR_CODE_RELATIONAL_EXPRESSION) || c + 1 == ce) {
        stat = SCAN_START;
        si->op = c->op;
        si->end = c - e->codes;
        sis[i++] = si;
        /* better index resolving framework for functions should be implemented */
        {
          grn_obj **p = si->args, **pe = si->args + si->nargs;
          for (; p < pe; p++) {
            if (GRN_DB_OBJP(*p)) {
              grn_index_datum index_datum;
              unsigned int n_index_data;
              n_index_data = grn_column_find_index_data(ctx, *p, c->op,
                                                        &index_datum, 1);
              if (n_index_data > 0) {
                scan_info_put_index(ctx, si,
                                    index_datum.index, index_datum.section, 1,
                                    NULL, NULL, 0);
              }
            } else if (GRN_ACCESSORP(*p)) {
              grn_index_datum index_datum;
              unsigned int n_index_data;
              si->flags |= SCAN_ACCESSOR;
              n_index_data = grn_column_find_index_data(ctx, *p, c->op,
                                                        &index_datum, 1);
              if (n_index_data > 0) {
                scan_info_put_index(ctx, si,
                                    index_datum.index, index_datum.section, 1,
                                    NULL, NULL, 0);
              }
            } else {
              si->query = *p;
            }
          }
        }
        si = NULL;
      } else {
        stat = SCAN_COL2;
      }
      break;
    case GRN_OP_GET_REF :
      switch (stat) {
      case SCAN_START :
        if (!si) { SI_ALLOC(si, i, c - e->codes); }
        stat = SCAN_COL1;
        if (si->nargs < GRN_SCAN_INFO_MAX_N_ARGS) {
          si->args[si->nargs++] = c->value;
        }
        break;
      default :
        break;
      }
      break;
    case GRN_OP_GET_MEMBER :
      {
        grn_obj *start_position;
        grn_obj buffer;
        start_position = si->args[--si->nargs];
        GRN_INT32_INIT(&buffer, 0);
        grn_obj_cast(ctx, start_position, &buffer, GRN_FALSE);
        grn_scan_info_set_start_position(si, GRN_INT32_VALUE(&buffer));
        GRN_OBJ_FIN(ctx, &buffer);
      }
      stat = SCAN_COL1;
      break;
    default :
      break;
    }
  }
  if (op == GRN_OP_OR && !record_exist) {
    // for debug
    if (!(sis[0]->flags & SCAN_PUSH) || (sis[0]->logical_op != op)) {
      int j;
      ERR(GRN_INVALID_ARGUMENT, "invalid expr");
      for (j = 0; j < i; j++) { SI_FREE(sis[j]); }
      GRN_FREE(sis);
      return NULL;
    } else {
      sis[0]->flags &= ~SCAN_PUSH;
      sis[0]->logical_op = op;
    }
  } else {
    if (!put_logical_op(ctx, sis, &i, op, c - e->codes)) { return NULL; }
  }
  *n = i;
  return sis;
}

void
grn_inspect_scan_info_list(grn_ctx *ctx, grn_obj *buffer, scan_info **sis, int n)
{
  int i;

  for (i = 0; i < n; i++) {
    scan_info *si = sis[i];

    grn_text_printf(ctx, buffer, "[%d]\n", i);
    grn_text_printf(ctx, buffer,
                    "  op:         <%s>\n",
                    grn_operator_to_string(si->op));
    grn_text_printf(ctx, buffer,
                    "  logical_op: <%s>\n",
                    grn_operator_to_string(si->logical_op));

    if (si->op == GRN_OP_CALL) {
      int i;
      for (i = 0; i < si->nargs; i++) {
        grn_text_printf(ctx, buffer, "  args[%d]:    <", i);
        grn_inspect(ctx, buffer, si->args[i]);
        GRN_TEXT_PUTS(ctx, buffer, ">\n");
      }
    } else {
      GRN_TEXT_PUTS(ctx, buffer, "  query:      <");
      grn_inspect(ctx, buffer, si->query);
      GRN_TEXT_PUTS(ctx, buffer, ">\n");
    }

    grn_text_printf(ctx, buffer,
                    "  expr:       <%d..%d>\n", si->start, si->end);
  }
}

void
grn_p_scan_info_list(grn_ctx *ctx, scan_info **sis, int n)
{
  grn_obj inspected;
  GRN_TEXT_INIT(&inspected, 0);
  grn_inspect_scan_info_list(ctx, &inspected, sis, n);
  printf("%.*s\n",
         (int)GRN_TEXT_LEN(&inspected),
         GRN_TEXT_VALUE(&inspected));
  GRN_OBJ_FIN(ctx, &inspected);
}

inline static int32_t
exec_result_to_score(grn_ctx *ctx, grn_obj *result, grn_obj *score_buffer)
{
  if (!result) {
    return 0;
  }

  switch (result->header.type) {
  case GRN_VOID :
    return 0;
  case GRN_BULK :
    if (grn_obj_cast(ctx, result, score_buffer, GRN_FALSE) != GRN_SUCCESS) {
      return 1;
    }
    return GRN_INT32_VALUE(score_buffer);
  case GRN_UVECTOR :
  case GRN_PVECTOR :
  case GRN_VECTOR :
    return 1;
  default :
    return 1; /* TODO: 1 is reasonable? */
  }
}

typedef union {
  struct {
    grn_obj *expr;
    grn_obj *variable;
  } common;
  struct {
    grn_obj *expr;
    grn_obj *variable;
    grn_obj score_buffer;
  } general;
  struct {
    grn_obj *expr;
    grn_obj *variable;
    int32_t score;
  } constant;
  struct {
    grn_obj *expr;
    grn_obj *variable;
#ifdef GRN_SUPPORT_REGEXP
    OnigRegex regex;
    grn_obj value_buffer;
    grn_obj *normalizer;
#endif /* GRN_SUPPORT_REGEXP */
  } simple_regexp;
  struct {
    grn_obj *expr;
    grn_obj *variable;
    grn_obj value_buffer;
    grn_obj constant_buffer;
    int32_t score;
    grn_bool (*exec)(grn_ctx *ctx, grn_obj *x, grn_obj *y);
  } simple_condition;
} grn_table_select_sequential_data;

typedef void (*grn_table_select_sequential_init_func)(grn_ctx *ctx,
                                                      grn_table_select_sequential_data *data);
typedef int32_t (*grn_table_select_sequential_exec_func)(grn_ctx *ctx,
                                                         grn_id id,
                                                         grn_table_select_sequential_data *data);
typedef void (*grn_table_select_sequential_fin_func)(grn_ctx *ctx,
                                                     grn_table_select_sequential_data *data);

static void
grn_table_select_sequential_init_general(grn_ctx *ctx,
                                         grn_table_select_sequential_data *data)
{
  GRN_INT32_INIT(&(data->general.score_buffer), 0);
}

static int32_t
grn_table_select_sequential_exec_general(grn_ctx *ctx,
                                         grn_id id,
                                         grn_table_select_sequential_data *data)
{
  grn_obj *result;
  GRN_RECORD_SET(ctx, data->general.variable, id);
  result = grn_expr_exec(ctx, data->general.expr, 0);
  if (ctx->rc) {
    return -1;
  }
  return exec_result_to_score(ctx, result, &(data->general.score_buffer));
}

static void
grn_table_select_sequential_fin_general(grn_ctx *ctx,
                                        grn_table_select_sequential_data *data)
{
  GRN_OBJ_FIN(ctx, &(data->general.score_buffer));
}

static grn_bool
grn_table_select_sequential_is_constant(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;
  grn_expr_code *target;

  if (e->codes_curr != 1) {
    return GRN_FALSE;
  }

  target = &(e->codes[0]);

  if (target->op != GRN_OP_PUSH) {
    return GRN_FALSE;
  }
  if (!target->value) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static void
grn_table_select_sequential_init_constant(grn_ctx *ctx,
                                          grn_table_select_sequential_data *data)
{
  grn_obj *result;
  grn_obj score_buffer;

  GRN_INT32_INIT(&score_buffer, 0);
  result = grn_expr_exec(ctx, data->constant.expr, 0);
  if (ctx->rc) {
    data->constant.score = -1;
  } else {
    data->constant.score = exec_result_to_score(ctx, result, &score_buffer);
  }
  GRN_OBJ_FIN(ctx, &score_buffer);
}

static int32_t
grn_table_select_sequential_exec_constant(grn_ctx *ctx,
                                          grn_id id,
                                          grn_table_select_sequential_data *data)
{
  return data->constant.score;
}

static void
grn_table_select_sequential_fin_constant(grn_ctx *ctx,
                                         grn_table_select_sequential_data *data)
{
}

#ifdef GRN_SUPPORT_REGEXP
static grn_bool
grn_table_select_sequential_is_simple_regexp(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;
  grn_expr_code *target;
  grn_expr_code *pattern;
  grn_expr_code *operator;

  if (e->codes_curr != 3) {
    return GRN_FALSE;
  }

  target = &(e->codes[0]);
  pattern = &(e->codes[1]);
  operator = &(e->codes[2]);

  if (operator->op != GRN_OP_REGEXP) {
    return GRN_FALSE;
  }
  if (operator->nargs != 2) {
    return GRN_FALSE;
  }

  if (target->op != GRN_OP_GET_VALUE) {
    return GRN_FALSE;
  }
  if (target->nargs != 1) {
    return GRN_FALSE;
  }
  if (!target->value) {
    return GRN_FALSE;
  }
  if (target->value->header.type != GRN_COLUMN_VAR_SIZE) {
    return GRN_FALSE;
  }
  if ((target->value->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) !=
      GRN_OBJ_COLUMN_SCALAR) {
    return GRN_FALSE;
  }
  switch (grn_obj_get_range(ctx, target->value)) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    break;
  default :
    return GRN_FALSE;
  }

  if (pattern->op != GRN_OP_PUSH) {
    return GRN_FALSE;
  }
  if (pattern->nargs != 1) {
    return GRN_FALSE;
  }
  if (!pattern->value) {
    return GRN_FALSE;
  }
  if (pattern->value->header.type != GRN_BULK) {
    return GRN_FALSE;
  }
  switch (pattern->value->header.domain) {
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    break;
  default :
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static void
grn_table_select_sequential_init_simple_regexp(grn_ctx *ctx,
                                               grn_table_select_sequential_data *data)
{
  grn_expr *e = (grn_expr *)(data->simple_regexp.expr);
  OnigEncoding onig_encoding;
  int onig_result;
  OnigErrorInfo onig_error_info;
  grn_obj *pattern;

  if (ctx->encoding == GRN_ENC_NONE) {
    data->simple_regexp.regex = NULL;
    return;
  }

  switch (ctx->encoding) {
  case GRN_ENC_EUC_JP :
    onig_encoding = ONIG_ENCODING_EUC_JP;
    break;
  case GRN_ENC_UTF8 :
    onig_encoding = ONIG_ENCODING_UTF8;
    break;
  case GRN_ENC_SJIS :
    onig_encoding = ONIG_ENCODING_CP932;
    break;
  case GRN_ENC_LATIN1 :
    onig_encoding = ONIG_ENCODING_ISO_8859_1;
    break;
  case GRN_ENC_KOI8R :
    onig_encoding = ONIG_ENCODING_KOI8_R;
    break;
  default :
    data->simple_regexp.regex = NULL;
    return;
  }

  pattern = e->codes[1].value;
  onig_result = onig_new(&(data->simple_regexp.regex),
                         GRN_TEXT_VALUE(pattern),
                         GRN_TEXT_VALUE(pattern) + GRN_TEXT_LEN(pattern),
                         ONIG_OPTION_ASCII_RANGE |
                         ONIG_OPTION_MULTILINE,
                         onig_encoding,
                         ONIG_SYNTAX_RUBY,
                         &onig_error_info);
  if (onig_result != ONIG_NORMAL) {
    char message[ONIG_MAX_ERROR_MESSAGE_LEN];
    onig_error_code_to_str(message, onig_result, onig_error_info);
    ERR(GRN_INVALID_ARGUMENT,
        "[table][select][sequential][regexp] "
        "failed to create regular expression object: <%.*s>: %s",
        (int)GRN_TEXT_LEN(pattern), GRN_TEXT_VALUE(pattern),
        message);
    return;
  }

  GRN_VOID_INIT(&(data->simple_regexp.value_buffer));

  data->simple_regexp.normalizer =
    grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
}

static int32_t
grn_table_select_sequential_exec_simple_regexp(grn_ctx *ctx,
                                               grn_id id,
                                               grn_table_select_sequential_data *data)
{
  grn_expr *e = (grn_expr *)(data->simple_regexp.expr);
  grn_obj *value_buffer = &(data->simple_regexp.value_buffer);

  if (ctx->rc) {
    return -1;
  }

  grn_obj_reinit_for(ctx, value_buffer, e->codes[0].value);
  grn_obj_get_value(ctx, e->codes[0].value, id, value_buffer);
  {
    grn_obj *norm_target;
    const char *norm_target_raw;
    unsigned int norm_target_raw_length_in_bytes;

    norm_target = grn_string_open(ctx,
                                  GRN_TEXT_VALUE(value_buffer),
                                  GRN_TEXT_LEN(value_buffer),
                                  data->simple_regexp.normalizer,
                                  0);
    grn_string_get_normalized(ctx, norm_target,
                              &norm_target_raw,
                              &norm_target_raw_length_in_bytes,
                              NULL);

    {
      OnigPosition position;
      position = onig_search(data->simple_regexp.regex,
                             norm_target_raw,
                             norm_target_raw + norm_target_raw_length_in_bytes,
                             norm_target_raw,
                             norm_target_raw + norm_target_raw_length_in_bytes,
                             NULL,
                             ONIG_OPTION_NONE);
      grn_obj_close(ctx, norm_target);
      if (position == ONIG_MISMATCH) {
        return -1;
      } else {
        return 1;
      }
    }
  }
}

static void
grn_table_select_sequential_fin_simple_regexp(grn_ctx *ctx,
                                              grn_table_select_sequential_data *data)
{
  if (!data->simple_regexp.regex) {
    return;
  }

  onig_free(data->simple_regexp.regex);
  GRN_OBJ_FIN(ctx, &(data->simple_regexp.value_buffer));
}
#endif /* GRN_SUPPORT_REGEXP */

static grn_bool
grn_table_select_sequential_is_simple_condition(grn_ctx *ctx, grn_obj *expr)
{
  grn_expr *e = (grn_expr *)expr;
  grn_expr_code *target;
  grn_expr_code *constant;
  grn_expr_code *operator;

  if (e->codes_curr != 3) {
    return GRN_FALSE;
  }

  target = &(e->codes[0]);
  constant = &(e->codes[1]);
  operator = &(e->codes[2]);

  switch (operator->op) {
  case GRN_OP_EQUAL :
  case GRN_OP_NOT_EQUAL :
  case GRN_OP_LESS :
  case GRN_OP_GREATER :
  case GRN_OP_LESS_EQUAL :
  case GRN_OP_GREATER_EQUAL :
    break;
  default :
    return GRN_FALSE;
  }
  if (operator->nargs != 2) {
    return GRN_FALSE;
  }

  if (target->op != GRN_OP_GET_VALUE) {
    return GRN_FALSE;
  }
  if (target->nargs != 1) {
    return GRN_FALSE;
  }
  if (!target->value) {
    return GRN_FALSE;
  }
  if ((target->value->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) !=
      GRN_OBJ_COLUMN_SCALAR) {
    return GRN_FALSE;
  }

  if (constant->op != GRN_OP_PUSH) {
    return GRN_FALSE;
  }
  if (constant->nargs != 1) {
    return GRN_FALSE;
  }
  if (!constant->value) {
    return GRN_FALSE;
  }
  if (constant->value->header.type != GRN_BULK) {
    return GRN_FALSE;
  }

  return GRN_TRUE;
}

static void
grn_table_select_sequential_init_simple_condition(
  grn_ctx *ctx,
  grn_table_select_sequential_data *data)
{
  grn_expr *e = (grn_expr *)(data->simple_condition.expr);
  grn_obj *target;
  grn_obj *constant;
  grn_operator op;
  grn_obj *value_buffer;
  grn_obj *constant_buffer;
  grn_rc rc;

  target = e->codes[0].value;
  constant = e->codes[1].value;
  op = e->codes[2].op;

  data->simple_condition.score = 0;

  value_buffer = &(data->simple_condition.value_buffer);
  GRN_VOID_INIT(value_buffer);
  grn_obj_reinit_for(ctx, value_buffer, target);

  switch (op) {
  case GRN_OP_EQUAL :
    data->simple_condition.exec = grn_operator_exec_equal;
    break;
  case GRN_OP_NOT_EQUAL :
    data->simple_condition.exec = grn_operator_exec_not_equal;
    break;
  case GRN_OP_LESS :
    data->simple_condition.exec = grn_operator_exec_less;
    break;
  case GRN_OP_GREATER :
    data->simple_condition.exec = grn_operator_exec_greater;
    break;
  case GRN_OP_LESS_EQUAL :
    data->simple_condition.exec = grn_operator_exec_less_equal;
    break;
  case GRN_OP_GREATER_EQUAL :
    data->simple_condition.exec = grn_operator_exec_greater_equal;
    break;
  default :
    break;
  }

  constant_buffer = &(data->simple_condition.constant_buffer);
  GRN_VOID_INIT(constant_buffer);
  grn_obj_reinit_for(ctx, constant_buffer, target);
  rc = grn_obj_cast(ctx, constant, constant_buffer, GRN_FALSE);
  if (rc != GRN_SUCCESS) {
    grn_obj *type;

    type = grn_ctx_at(ctx, constant_buffer->header.domain);
    if (grn_obj_is_table(ctx, type)) {
      if (op == GRN_OP_NOT_EQUAL) {
        data->simple_condition.score = 1;
      } else {
        data->simple_condition.score = -1;
      }
    } else {
      int type_name_size;
      char type_name[GRN_TABLE_MAX_KEY_SIZE];
      grn_obj inspected;

      type_name_size = grn_obj_name(ctx, type, type_name,
                                    GRN_TABLE_MAX_KEY_SIZE);
      GRN_TEXT_INIT(&inspected, 0);
      grn_inspect(ctx, &inspected, constant);
      ERR(rc,
          "[table][select][sequential][condition] "
          "failed to cast to <%.*s>: <%.*s>",
          type_name_size, type_name,
          (int)GRN_TEXT_LEN(&inspected), GRN_TEXT_VALUE(&inspected));
    }
    return;
  }
}

static int32_t
grn_table_select_sequential_exec_simple_condition(
  grn_ctx *ctx,
  grn_id id,
  grn_table_select_sequential_data *data)
{
  grn_expr *e = (grn_expr *)(data->simple_condition.expr);
  grn_obj *target;
  grn_obj *value_buffer = &(data->simple_condition.value_buffer);
  grn_obj *constant_buffer = &(data->simple_condition.constant_buffer);

  if (ctx->rc) {
    return -1;
  }

  if (data->simple_condition.score != 0) {
    return data->simple_condition.score;
  }

  target = e->codes[0].value;
  GRN_BULK_REWIND(value_buffer);
  grn_obj_get_value(ctx, target, id, value_buffer);

  if (data->simple_condition.exec(ctx, value_buffer, constant_buffer)) {
    return 1;
  } else {
    return -1;
  }
}

static void
grn_table_select_sequential_fin_simple_condition(
  grn_ctx *ctx,
  grn_table_select_sequential_data *data)
{
  GRN_OBJ_FIN(ctx, &(data->simple_condition.value_buffer));
  GRN_OBJ_FIN(ctx, &(data->simple_condition.constant_buffer));
}

static void
grn_table_select_sequential(grn_ctx *ctx, grn_obj *table, grn_obj *expr,
                            grn_obj *v, grn_obj *res, grn_operator op)
{
  int32_t score;
  grn_id id, *idp;
  grn_table_cursor *tc;
  grn_hash_cursor *hc;
  grn_hash *s = (grn_hash *)res;
  grn_table_select_sequential_data data;
  grn_table_select_sequential_init_func init;
  grn_table_select_sequential_exec_func exec;
  grn_table_select_sequential_fin_func fin;

  data.common.expr = expr;
  data.common.variable = v;
  init = grn_table_select_sequential_init_general;
  exec = grn_table_select_sequential_exec_general;
  fin = grn_table_select_sequential_fin_general;
  if (grn_table_select_sequential_is_constant(ctx, expr)) {
    init = grn_table_select_sequential_init_constant;
    exec = grn_table_select_sequential_exec_constant;
    fin = grn_table_select_sequential_fin_constant;
#ifdef GRN_SUPPORT_REGEXP
  } else if (grn_table_select_sequential_is_simple_regexp(ctx, expr)) {
    init = grn_table_select_sequential_init_simple_regexp;
    exec = grn_table_select_sequential_exec_simple_regexp;
    fin = grn_table_select_sequential_fin_simple_regexp;
#endif /* GRN_SUPPORT_REGEXP */
  } else if (grn_table_select_sequential_is_simple_condition(ctx, expr)) {
    init = grn_table_select_sequential_init_simple_condition;
    exec = grn_table_select_sequential_exec_simple_condition;
    fin = grn_table_select_sequential_fin_simple_condition;
  }

  init(ctx, &data);
  switch (op) {
  case GRN_OP_OR :
    if ((tc = grn_table_cursor_open(ctx, table, NULL, 0, NULL, 0, 0, -1, 0))) {
      while ((id = grn_table_cursor_next(ctx, tc))) {
        score = exec(ctx, id, &data);
        if (ctx->rc) {
          break;
        }
        if (score > 0) {
          grn_rset_recinfo *ri;
          if (grn_hash_add(ctx, s, &id, s->key_size, (void **)&ri, NULL)) {
            grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)&id, 1);
          }
        }
      }
      grn_table_cursor_close(ctx, tc);
    }
    break;
  case GRN_OP_AND :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        score = exec(ctx, *idp, &data);
        if (ctx->rc) {
          break;
        }
        if (score > 0) {
          grn_rset_recinfo *ri;
          grn_hash_cursor_get_value(ctx, hc, (void **) &ri);
          grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)idp, 1);
        } else {
          grn_hash_cursor_delete(ctx, hc, NULL);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  case GRN_OP_AND_NOT :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        GRN_RECORD_SET(ctx, v, *idp);
        score = exec(ctx, *idp, &data);
        if (ctx->rc) {
          break;
        }
        if (score > 0) {
          grn_hash_cursor_delete(ctx, hc, NULL);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  case GRN_OP_ADJUST :
    if ((hc = grn_hash_cursor_open(ctx, s, NULL, 0, NULL, 0, 0, -1, 0))) {
      while (grn_hash_cursor_next(ctx, hc)) {
        grn_hash_cursor_get_key(ctx, hc, (void **) &idp);
        score = exec(ctx, *idp, &data);
        if (ctx->rc) {
          break;
        }
        if (score > 0) {
          grn_rset_recinfo *ri;
          grn_hash_cursor_get_value(ctx, hc, (void **) &ri);
          grn_table_add_subrec(res, ri, score, (grn_rset_posinfo *)idp, 1);
        }
      }
      grn_hash_cursor_close(ctx, hc);
    }
    break;
  default :
    break;
  }
  fin(ctx, &data);
}

static inline void
grn_table_select_index_report(grn_ctx *ctx, const char *tag, grn_obj *index)
{
  grn_report_index(ctx, "[table][select]", tag, index);
}

static inline grn_bool
grn_table_select_index_equal(grn_ctx *ctx,
                             grn_obj *table,
                             grn_obj *index,
                             scan_info *si,
                             grn_obj *res)
{
  grn_bool processed = GRN_FALSE;

  if (GRN_BULK_VSIZE(si->query) == 0) {
    /* We can't use index for empty value. */
    return GRN_FALSE;
  }

  if (si->flags & SCAN_ACCESSOR) {
    if (index->header.type == GRN_ACCESSOR && !((grn_accessor *)index)->next) {
      grn_obj dest;
      grn_accessor *a = (grn_accessor *)index;
      grn_posting posting;
      posting.sid = 1;
      posting.pos = 0;
      posting.weight = 0;
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        grn_table_select_index_report(ctx, "[equal][accessor][id]", table);
        GRN_UINT32_INIT(&dest, 0);
        if (!grn_obj_cast(ctx, si->query, &dest, GRN_FALSE)) {
          posting.rid = GRN_UINT32_VALUE(&dest);
          if (posting.rid) {
            if (posting.rid == grn_table_at(ctx, table, posting.rid)) {
              grn_ii_posting_add(ctx, &posting, (grn_hash *)res,
                                 si->logical_op);
            }
          }
          processed = GRN_TRUE;
        }
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
        GRN_OBJ_FIN(ctx, &dest);
        break;
      case GRN_ACCESSOR_GET_KEY :
        grn_table_select_index_report(ctx, "[equal][accessor][key]", table);
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, table->header.domain);
        if (!grn_obj_cast(ctx, si->query, &dest, GRN_FALSE)) {
          if ((posting.rid = grn_table_get(ctx, table,
                                           GRN_BULK_HEAD(&dest),
                                           GRN_BULK_VSIZE(&dest)))) {
            grn_ii_posting_add(ctx, &posting, (grn_hash *)res,
                               si->logical_op);
          }
          processed = GRN_TRUE;
        }
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
        GRN_OBJ_FIN(ctx, &dest);
        break;
      }
    }
  } else {
    grn_obj *domain = grn_ctx_at(ctx, index->header.domain);
    if (domain) {
      grn_id tid;
      if (GRN_OBJ_GET_DOMAIN(si->query) == DB_OBJ(domain)->id) {
        tid = GRN_RECORD_VALUE(si->query);
      } else {
        tid = grn_table_get(ctx, domain,
                            GRN_BULK_HEAD(si->query),
                            GRN_BULK_VSIZE(si->query));
      }
      if (tid != GRN_ID_NIL) {
        uint32_t sid;
        int32_t weight;
        grn_ii *ii = (grn_ii *)index;
        grn_ii_cursor *ii_cursor;

        grn_table_select_index_report(ctx, "[equal]", index);

        sid = GRN_UINT32_VALUE_AT(&(si->wv), 0);
        weight = GRN_INT32_VALUE_AT(&(si->wv), 1);
        ii_cursor = grn_ii_cursor_open(ctx, ii, tid,
                                       GRN_ID_NIL, GRN_ID_MAX,
                                       ii->n_elements, 0);
        if (ii_cursor) {
          grn_posting *posting;
          while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
            grn_posting new_posting;

            if (!(sid == 0 || posting->sid == sid)) {
              continue;
            }

            if (si->position.specified) {
              while ((posting = grn_ii_cursor_next_pos(ctx, ii_cursor))) {
                if (posting->pos == si->position.start) {
                  break;
                }
              }
              if (!posting) {
                continue;
              }
            }

            new_posting = *posting;
            new_posting.weight *= weight;
            grn_ii_posting_add(ctx, &new_posting, (grn_hash *)res,
                               si->logical_op);
          }
          grn_ii_cursor_close(ctx, ii_cursor);
        }
      }
      processed = GRN_TRUE;
    }
    if (processed) {
      grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
    }
  }

  return processed;
}

static inline grn_bool
grn_table_select_index_not_equal(grn_ctx *ctx,
                             grn_obj *table,
                             grn_obj *index,
                             scan_info *si,
                             grn_obj *res)
{
  grn_bool processed = GRN_FALSE;

  if (GRN_BULK_VSIZE(si->query) == 0) {
    /* We can't use index for empty value. */
    return GRN_FALSE;
  }

  if (si->logical_op != GRN_OP_AND) {
    /* We can't use index for OR and AND_NOT. */
    return GRN_FALSE;
  }

  if (si->flags & SCAN_ACCESSOR) {
    if (index->header.type == GRN_ACCESSOR && !((grn_accessor *)index)->next) {
      grn_obj dest;
      grn_accessor *a = (grn_accessor *)index;
      grn_id id;
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        grn_table_select_index_report(ctx, "[not-equal][accessor][id]", table);
        GRN_UINT32_INIT(&dest, 0);
        if (!grn_obj_cast(ctx, si->query, &dest, GRN_FALSE)) {
          id = GRN_UINT32_VALUE(&dest);
          if (id != GRN_ID_NIL) {
            if (id == grn_table_at(ctx, table, id)) {
              grn_hash_delete(ctx, (grn_hash *)res, &id, sizeof(grn_id), NULL);
            }
          }
          processed = GRN_TRUE;
        }
        GRN_OBJ_FIN(ctx, &dest);
        break;
      case GRN_ACCESSOR_GET_KEY :
        grn_table_select_index_report(ctx, "[not-equal][accessor][key]", table);
        GRN_OBJ_INIT(&dest, GRN_BULK, 0, table->header.domain);
        if (!grn_obj_cast(ctx, si->query, &dest, GRN_FALSE)) {
          id = grn_table_get(ctx, table,
                             GRN_BULK_HEAD(&dest),
                             GRN_BULK_VSIZE(&dest));
          if (id != GRN_ID_NIL) {
            grn_hash_delete(ctx, (grn_hash *)res, &id, sizeof(grn_id), NULL);
          }
          processed = GRN_TRUE;
        }
        GRN_OBJ_FIN(ctx, &dest);
        break;
      }
    }
  } else {
    grn_obj *domain = grn_ctx_at(ctx, index->header.domain);
    if (domain) {
      grn_id tid;
      if (GRN_OBJ_GET_DOMAIN(si->query) == DB_OBJ(domain)->id) {
        tid = GRN_RECORD_VALUE(si->query);
      } else {
        tid = grn_table_get(ctx, domain,
                            GRN_BULK_HEAD(si->query),
                            GRN_BULK_VSIZE(si->query));
      }
      if (tid == GRN_ID_NIL) {
        processed = GRN_TRUE;
      } else {
        uint32_t sid;
        int32_t weight;
        grn_ii *ii = (grn_ii *)index;
        grn_ii_cursor *ii_cursor;

        grn_table_select_index_report(ctx, "[not-equal]", index);

        sid = GRN_UINT32_VALUE_AT(&(si->wv), 0);
        weight = GRN_INT32_VALUE_AT(&(si->wv), 1);
        ii_cursor = grn_ii_cursor_open(ctx, ii, tid,
                                       GRN_ID_NIL, GRN_ID_MAX,
                                       ii->n_elements, 0);
        if (ii_cursor) {
          grn_posting *posting;
          while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
            if (!(sid == 0 || posting->sid == sid)) {
              continue;
            }

            if (si->position.specified) {
              while ((posting = grn_ii_cursor_next_pos(ctx, ii_cursor))) {
                if (posting->pos == si->position.start) {
                  break;
                }
              }
              if (!posting) {
                continue;
              }
            }

            grn_hash_delete(ctx, (grn_hash *)res,
                            &(posting->rid), sizeof(grn_id),
                            NULL);
          }
          grn_ii_cursor_close(ctx, ii_cursor);
          processed = GRN_TRUE;
        }
      }
    }
  }

  return processed;
}

static inline grn_bool
grn_table_select_index_range_column(grn_ctx *ctx, grn_obj *table,
                                    grn_obj *index,
                                    scan_info *si, grn_operator logical_op,
                                    grn_obj *res)
{
  grn_bool processed = GRN_FALSE;
  grn_obj *index_table;
  grn_obj range;

  index_table = grn_ctx_at(ctx, index->header.domain);
  if (!index_table) {
    return GRN_FALSE;
  }

  GRN_OBJ_INIT(&range, GRN_BULK, 0, index_table->header.domain);
  if (grn_obj_cast(ctx, si->query, &range, GRN_FALSE) == GRN_SUCCESS) {
    grn_table_cursor *cursor;
    const void *min = NULL, *max = NULL;
    unsigned int min_size = 0, max_size = 0;
    int offset = 0;
    int limit = -1;
    int flags = GRN_CURSOR_ASCENDING;

    grn_table_select_index_report(ctx, "[range]", index_table);

    switch (si->op) {
    case GRN_OP_LESS :
      flags |= GRN_CURSOR_LT;
      max = GRN_BULK_HEAD(&range);
      max_size = GRN_BULK_VSIZE(&range);
      break;
    case GRN_OP_GREATER :
      flags |= GRN_CURSOR_GT;
      min = GRN_BULK_HEAD(&range);
      min_size = GRN_BULK_VSIZE(&range);
      break;
    case GRN_OP_LESS_EQUAL :
      flags |= GRN_CURSOR_LE;
      max = GRN_BULK_HEAD(&range);
      max_size = GRN_BULK_VSIZE(&range);
      break;
    case GRN_OP_GREATER_EQUAL :
      flags |= GRN_CURSOR_GE;
      min = GRN_BULK_HEAD(&range);
      min_size = GRN_BULK_VSIZE(&range);
      break;
    default :
      break;
    }
    cursor = grn_table_cursor_open(ctx, index_table,
                                   min, min_size, max, max_size,
                                   offset, limit, flags);
    if (cursor) {
      grn_id tid;
      uint32_t sid;
      int32_t weight;
      grn_ii *ii = (grn_ii *)index;

      sid = GRN_UINT32_VALUE_AT(&(si->wv), 0);
      weight = GRN_INT32_VALUE_AT(&(si->wv), 1);
      while ((tid = grn_table_cursor_next(ctx, cursor)) != GRN_ID_NIL) {
        grn_ii_cursor *ii_cursor;

        ii_cursor = grn_ii_cursor_open(ctx, ii, tid,
                                       GRN_ID_NIL, GRN_ID_MAX,
                                       ii->n_elements, 0);
        if (ii_cursor) {
          grn_posting *posting;
          while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
            grn_posting new_posting;

            if (!(sid == 0 || posting->sid == sid)) {
              continue;
            }

            if (si->position.specified) {
              while ((posting = grn_ii_cursor_next_pos(ctx, ii_cursor))) {
                if (posting->pos == si->position.start) {
                  break;
                }
              }
              if (!posting) {
                continue;
              }
            }

            new_posting = *posting;
            new_posting.weight *= weight;
            grn_ii_posting_add(ctx, &new_posting, (grn_hash *)res, logical_op);
          }
        }
        processed = GRN_TRUE;
        grn_ii_cursor_close(ctx, ii_cursor);
      }
      grn_table_cursor_close(ctx, cursor);
    }

    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, logical_op);
  }
  GRN_OBJ_FIN(ctx, &range);

  grn_obj_unlink(ctx, index_table);

  return processed;
}

static inline grn_bool
grn_table_select_index_range_accessor(grn_ctx *ctx, grn_obj *table,
                                      grn_obj *accessor_stack,
                                      scan_info *si, grn_obj *res)
{
  int n_accessors;
  grn_obj *current_res = NULL;

  n_accessors = GRN_BULK_VSIZE(accessor_stack) / sizeof(grn_obj *);

  {
    grn_accessor *last_accessor;
    grn_obj *target;
    grn_obj *index;
    grn_obj *range;

    last_accessor = (grn_accessor *)GRN_PTR_VALUE_AT(accessor_stack,
                                                     n_accessors - 1);
    target = last_accessor->obj;
    if (grn_column_index(ctx, target, si->op, &index, 1, NULL) == 0) {
      return GRN_FALSE;
    }

    range = grn_ctx_at(ctx, DB_OBJ(index)->range);
    current_res = grn_table_create(ctx, NULL, 0, NULL,
                                   GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                   range,
                                   NULL);
    grn_obj_unlink(ctx, range);
    if (!current_res) {
      return GRN_FALSE;
    }
    if (!grn_table_select_index_range_column(ctx, table, index, si, GRN_OP_OR,
                                             current_res)) {
      grn_obj_unlink(ctx, current_res);
      return GRN_FALSE;
    }
  }

  grn_table_select_index_report(ctx, "[range][accessor]",
                                GRN_PTR_VALUE_AT(accessor_stack, 0));

  {
    int i;
    grn_obj weight_vector;
    grn_search_optarg optarg;

    GRN_INT32_INIT(&weight_vector, GRN_OBJ_VECTOR);
    memset(&optarg, 0, sizeof(grn_search_optarg));
    if (si->op == GRN_OP_MATCH) {
      optarg.mode = GRN_OP_EXACT;
    } else {
      optarg.mode = si->op;
    }
    for (i = n_accessors - 1; i > 0; i--) {
      grn_rc rc = GRN_SUCCESS;
      grn_accessor *accessor;
      grn_obj *index;
      int section;
      grn_obj *domain;
      grn_obj *target;
      grn_obj *next_res;
      grn_operator next_op;
      grn_id *next_record_id = NULL;

      accessor = (grn_accessor *)GRN_PTR_VALUE_AT(accessor_stack, i - 1);
      target = accessor->obj;
      {
        grn_index_datum index_datum;
        unsigned int n_index_data;
        n_index_data = grn_column_find_index_data(ctx, target, GRN_OP_EQUAL,
                                                  &index_datum, 1);
        if (n_index_data == 0) {
          grn_obj_unlink(ctx, current_res);
          current_res = NULL;
          break;
        }
        index = index_datum.index;
        section = index_datum.section;
      }

      if (grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
#define TAG_BUFFER_SIZE 128
        char tag[TAG_BUFFER_SIZE];
        grn_snprintf(tag, TAG_BUFFER_SIZE, TAG_BUFFER_SIZE,
                     "[range][accessor][%d]", i - 1);
        grn_table_select_index_report(ctx, tag, index);
#undef TAG_BUFFER_SIZE
      }

      if (section > 0) {
        int j;
        int weight_position = section - 1;

        GRN_BULK_REWIND(&weight_vector);
        GRN_INT32_SET_AT(ctx, &weight_vector, weight_position, 1);
        optarg.weight_vector = &(GRN_INT32_VALUE(&weight_vector));
        optarg.vector_size = GRN_BULK_VSIZE(&weight_vector) / sizeof(int32_t);
        for (j = 0; j < weight_position - 1; j++) {
          optarg.weight_vector[j] = 0;
        }
      } else {
        optarg.weight_vector = NULL;
        optarg.vector_size = 1;
      }

      {
        grn_obj *range;
        range = grn_ctx_at(ctx, DB_OBJ(index)->range);
        next_res = grn_table_create(ctx, NULL, 0, NULL,
                                   GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC,
                                   range,
                                   NULL);
        grn_obj_unlink(ctx, range);
        if (!next_res) {
          grn_obj_unlink(ctx, current_res);
          current_res = NULL;
          break;
        }
        next_op = GRN_OP_OR;
      }

      domain = grn_ctx_at(ctx, index->header.domain);
      GRN_HASH_EACH(ctx, (grn_hash *)current_res, id, &next_record_id,
                    NULL, NULL, {
        if (domain->header.type == GRN_TABLE_NO_KEY) {
          rc = grn_ii_sel(ctx, (grn_ii *)index,
                          (const char *)next_record_id, sizeof(grn_id),
                          (grn_hash *)next_res, next_op, &optarg);
        } else {
          char key[GRN_TABLE_MAX_KEY_SIZE];
          int key_len;
          key_len = grn_table_get_key(ctx, domain, *next_record_id,
                                      key, GRN_TABLE_MAX_KEY_SIZE);
          rc = grn_ii_sel(ctx, (grn_ii *)index, key, key_len,
                          (grn_hash *)next_res, next_op, &optarg);
        }
        if (rc != GRN_SUCCESS) {
          break;
        }
      });
      grn_obj_unlink(ctx, domain);
      grn_obj_unlink(ctx, current_res);

      if (rc == GRN_SUCCESS) {
        if (i == 1) {
          grn_table_setoperation(ctx, res, next_res, res, si->logical_op);
          grn_obj_unlink(ctx, next_res);
          current_res = res;
        } else {
          current_res = next_res;
        }
      } else {
        if (res != next_res) {
          grn_obj_unlink(ctx, next_res);
        }
        current_res = NULL;
        break;
      }
    }
    GRN_OBJ_FIN(ctx, &weight_vector);
  }

  return current_res == res;
}

static inline grn_bool
grn_table_select_index_range(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                             scan_info *si, grn_obj *res)
{
  if (si->flags & SCAN_ACCESSOR) {
    grn_bool processed;
    grn_accessor *accessor = (grn_accessor *)index;
    grn_accessor *a;
    grn_obj accessor_stack;

    if (index->header.type != GRN_ACCESSOR) {
      return GRN_FALSE;
    }

    GRN_PTR_INIT(&accessor_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
    for (a = accessor; a; a = a->next) {
      GRN_PTR_PUT(ctx, &accessor_stack, a);
    }
    processed = grn_table_select_index_range_accessor(ctx, table,
                                                      &accessor_stack,
                                                      si, res);
    GRN_OBJ_FIN(ctx, &accessor_stack);
    return processed;
  } else {
    return grn_table_select_index_range_column(ctx, table, index, si,
                                               si->logical_op, res);
  }
}

static inline grn_bool
grn_table_select_index(grn_ctx *ctx, grn_obj *table, scan_info *si,
                       grn_obj *res)
{
  grn_bool processed = GRN_FALSE;
  if (GRN_BULK_VSIZE(&si->index)) {
    grn_obj *index = GRN_PTR_VALUE(&si->index);
    switch (si->op) {
    case GRN_OP_EQUAL :
      processed = grn_table_select_index_equal(ctx, table, index, si, res);
      break;
    case GRN_OP_NOT_EQUAL :
      processed = grn_table_select_index_not_equal(ctx, table, index, si, res);
      break;
    case GRN_OP_SUFFIX :
      {
        grn_obj *domain;
        if (si->flags & SCAN_ACCESSOR) {
          domain = table;
        } else {
          domain = grn_ctx_at(ctx, index->header.domain);
        }
        if (domain->header.type != GRN_TABLE_PAT_KEY) {
          break;
        }
        if (!(domain->header.flags & GRN_OBJ_KEY_WITH_SIS)) {
          break;
        }
      }
      /* fallthru */
    case GRN_OP_PREFIX :
      if (si->flags & SCAN_ACCESSOR) {
        if (index->header.type == GRN_ACCESSOR &&
            !((grn_accessor *)index)->next) {
          grn_obj dest;
          grn_accessor *a = (grn_accessor *)index;
          grn_posting posting;
          posting.sid = 1;
          posting.pos = 0;
          posting.weight = 0;
          switch (a->action) {
          case GRN_ACCESSOR_GET_ID :
            /* todo */
            break;
          case GRN_ACCESSOR_GET_KEY :
            if (si->op == GRN_OP_SUFFIX) {
              grn_table_select_index_report(ctx,
                                            "[suffix][accessor][key]", table);
            } else {
              grn_table_select_index_report(ctx,
                                            "[prefix][accessor][key]", table);
            }
            GRN_OBJ_INIT(&dest, GRN_BULK, 0, table->header.domain);
            if (!grn_obj_cast(ctx, si->query, &dest, GRN_FALSE)) {
              grn_hash *pres;
              if ((pres = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                          GRN_OBJ_TABLE_HASH_KEY))) {
                grn_id *key;
                grn_table_search(ctx, table,
                                 GRN_BULK_HEAD(&dest), GRN_BULK_VSIZE(&dest),
                                 si->op, (grn_obj *)pres, GRN_OP_OR);
                GRN_HASH_EACH(ctx, pres, id, &key, NULL, NULL, {
                  posting.rid = *key;
                  grn_ii_posting_add(ctx, &posting, (grn_hash *)res,
                                     si->logical_op);
                });
                grn_hash_close(ctx, pres);
              }
              processed = GRN_TRUE;
            }
            grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
            GRN_OBJ_FIN(ctx, &dest);
            break;
          }
        }
      } else {
        grn_obj *domain = grn_ctx_at(ctx, index->header.domain);
        if (domain) {
          grn_hash *pres;
          if ((pres = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                      GRN_OBJ_TABLE_HASH_KEY))) {
            grn_id *key;
            if (si->op == GRN_OP_SUFFIX) {
              grn_table_select_index_report(ctx, "[suffix]", index);
            } else {
              grn_table_select_index_report(ctx, "[prefix]", index);
            }
            grn_table_search(ctx, domain,
                             GRN_BULK_HEAD(si->query),
                             GRN_BULK_VSIZE(si->query),
                             si->op, (grn_obj *)pres, GRN_OP_OR);
            grn_obj_unlink(ctx, domain);
            GRN_HASH_EACH(ctx, pres, id, &key, NULL, NULL, {
              grn_ii_at(ctx, (grn_ii *)index, *key, (grn_hash *)res, si->logical_op);
            });
            grn_hash_close(ctx, pres);
          }
          grn_obj_unlink(ctx, domain);
        }
        grn_ii_resolve_sel_and(ctx, (grn_hash *)res, si->logical_op);
        processed = GRN_TRUE;
      }
      break;
    case GRN_OP_MATCH :
    case GRN_OP_NEAR :
    case GRN_OP_NEAR2 :
    case GRN_OP_SIMILAR :
    case GRN_OP_REGEXP :
      {
        grn_obj wv, **ip = &GRN_PTR_VALUE(&si->index);
        int j;
        int n_indexes = GRN_BULK_VSIZE(&si->index)/sizeof(grn_obj *);
        int32_t *wp = &GRN_INT32_VALUE(&si->wv);
        grn_search_optarg optarg;
        GRN_INT32_INIT(&wv, GRN_OBJ_VECTOR);
        if (si->op == GRN_OP_MATCH) {
          optarg.mode = GRN_OP_EXACT;
        } else {
          optarg.mode = si->op;
        }
        optarg.max_interval = 0;
        optarg.similarity_threshold = 0;
        switch (si->op) {
        case GRN_OP_NEAR :
        case GRN_OP_NEAR2 :
          optarg.max_interval = si->max_interval;
          break;
        case GRN_OP_SIMILAR :
          optarg.similarity_threshold = si->similarity_threshold;
          break;
        default :
          break;
        }
        optarg.weight_vector = (int *)GRN_BULK_HEAD(&wv);
        /* optarg.vector_size = GRN_BULK_VSIZE(&si->wv); */
        optarg.vector_size = 1;
        optarg.proc = NULL;
        optarg.max_size = 0;
        ctx->flags |= GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
        for (j = 0; j < n_indexes; j++, ip++, wp += 2) {
          uint32_t sid = (uint32_t) wp[0];
          int32_t weight = wp[1];
          if (sid) {
            int weight_index = sid - 1;
            int current_vector_size;
            current_vector_size = GRN_BULK_VSIZE(&wv)/sizeof(int32_t);
            if (weight_index < current_vector_size) {
              ((int *)GRN_BULK_HEAD(&wv))[weight_index] = weight;
            } else {
              GRN_INT32_SET_AT(ctx, &wv, weight_index, weight);
            }
            optarg.weight_vector = &GRN_INT32_VALUE(&wv);
            optarg.vector_size = GRN_BULK_VSIZE(&wv)/sizeof(int32_t);
          } else {
            optarg.weight_vector = NULL;
            optarg.vector_size = weight;
          }
          optarg.scorer = GRN_PTR_VALUE_AT(&(si->scorers), j);
          optarg.scorer_args_expr =
            GRN_PTR_VALUE_AT(&(si->scorer_args_exprs), j);
          optarg.scorer_args_expr_offset =
            GRN_UINT32_VALUE_AT(&(si->scorer_args_expr_offsets), j);
          if (j < n_indexes - 1) {
            if (sid && ip[0] == ip[1]) { continue; }
          } else {
            ctx->flags &= ~GRN_CTX_TEMPORARY_DISABLE_II_RESOLVE_SEL_AND;
          }
          grn_obj_search(ctx, ip[0], si->query, res, si->logical_op, &optarg);
          if (optarg.weight_vector) {
            int i;
            for (i = 0; i < optarg.vector_size; i++) {
              optarg.weight_vector[i] = 0;
            }
          }
          GRN_BULK_REWIND(&wv);
        }
        GRN_OBJ_FIN(ctx, &wv);
      }
      processed = GRN_TRUE;
      break;
    case GRN_OP_TERM_EXTRACT :
      if (si->flags & SCAN_ACCESSOR) {
        if (index->header.type == GRN_ACCESSOR &&
            !((grn_accessor *)index)->next) {
          grn_accessor *a = (grn_accessor *)index;
          switch (a->action) {
          case GRN_ACCESSOR_GET_KEY :
            grn_table_select_index_report(ctx, "[term-extract][accessor][key]",
                                          table);
            grn_table_search(ctx, table,
                             GRN_TEXT_VALUE(si->query), GRN_TEXT_LEN(si->query),
                             GRN_OP_TERM_EXTRACT, res, si->logical_op);
            processed = GRN_TRUE;
            break;
          }
        }
      }
      break;
    case GRN_OP_CALL :
      if (grn_obj_is_selector_proc(ctx, si->args[0])) {
        grn_rc rc;
        grn_proc *proc = (grn_proc *)(si->args[0]);
        if (grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
          char proc_name[GRN_TABLE_MAX_KEY_SIZE];
          int proc_name_size;
          char tag[GRN_TABLE_MAX_KEY_SIZE];
          proc_name_size = grn_obj_name(ctx, (grn_obj *)proc,
                                        proc_name, GRN_TABLE_MAX_KEY_SIZE);
          proc_name[proc_name_size] = '\0';
          grn_snprintf(tag, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
                       "[selector][%s]", proc_name);
          grn_table_select_index_report(ctx, tag, index);
        }
        rc = proc->selector(ctx, table, index, si->nargs, si->args,
                            res, si->logical_op);
        if (rc) {
          /* TODO: report error */
        } else {
          processed = GRN_TRUE;
        }
      }
      break;
    case GRN_OP_LESS :
    case GRN_OP_GREATER :
    case GRN_OP_LESS_EQUAL :
    case GRN_OP_GREATER_EQUAL :
      processed = grn_table_select_index_range(ctx, table, index, si, res);
      break;
    default :
      /* todo : implement */
      /* todo : handle SCAN_PRE_CONST */
      break;
    }
  } else {
    switch (si->op) {
    case GRN_OP_CALL :
      if (grn_obj_is_selector_proc(ctx, si->args[0])) {
        grn_rc rc;
        grn_proc *proc = (grn_proc *)(si->args[0]);
        if (grn_logger_pass(ctx, GRN_REPORT_INDEX_LOG_LEVEL)) {
          char proc_name[GRN_TABLE_MAX_KEY_SIZE];
          int proc_name_size;
          char tag[GRN_TABLE_MAX_KEY_SIZE];
          proc_name_size = grn_obj_name(ctx, (grn_obj *)proc,
                                        proc_name, GRN_TABLE_MAX_KEY_SIZE);
          proc_name[proc_name_size] = '\0';
          grn_snprintf(tag, GRN_TABLE_MAX_KEY_SIZE, GRN_TABLE_MAX_KEY_SIZE,
                       "[selector][no-index][%s]", proc_name);
          grn_table_select_index_report(ctx, tag, table);
        }
        rc = proc->selector(ctx, table, NULL, si->nargs, si->args,
                            res, si->logical_op);
        if (rc) {
          /* TODO: report error */
        } else {
          processed = GRN_TRUE;
        }
      }
    default :
      break;
    }
  }
  return processed;
}

grn_obj *
grn_table_select(grn_ctx *ctx, grn_obj *table, grn_obj *expr,
                 grn_obj *res, grn_operator op)
{
  grn_obj *v;
  unsigned int res_size;
  grn_bool res_created = GRN_FALSE;
  if (res) {
    if (res->header.type != GRN_TABLE_HASH_KEY ||
        (res->header.domain != DB_OBJ(table)->id)) {
      ERR(GRN_INVALID_ARGUMENT, "hash table required");
      return NULL;
    }
  } else {
    if (!(res = grn_table_create(ctx, NULL, 0, NULL,
                                 GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL))) {
      return NULL;
    }
    res_created = GRN_TRUE;
  }
  if (!(v = grn_expr_get_var_by_offset(ctx, expr, 0))) {
    ERR(GRN_INVALID_ARGUMENT, "at least one variable must be defined");
    return NULL;
  }
  GRN_API_ENTER;
  res_size = GRN_HASH_SIZE((grn_hash *)res);
  if (op == GRN_OP_OR || res_size) {
    int i;
    grn_scanner *scanner;
    scanner = grn_scanner_open(ctx, expr, op, res_size > 0);
    if (scanner) {
      grn_obj res_stack;
      grn_expr *e = (grn_expr *)scanner->expr;
      grn_expr_code *codes = e->codes;
      uint32_t codes_curr = e->codes_curr;
      GRN_PTR_INIT(&res_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
      for (i = 0; i < scanner->n_sis; i++) {
        scan_info *si = scanner->sis[i];
        if (si->flags & SCAN_POP) {
          grn_obj *res_;
          GRN_PTR_POP(&res_stack, res_);
          grn_table_setoperation(ctx, res_, res, res_, si->logical_op);
          grn_obj_close(ctx, res);
          res = res_;
        } else {
          grn_bool processed = GRN_FALSE;
          if (si->flags & SCAN_PUSH) {
            grn_obj *res_ = NULL;
            res_ = grn_table_create(ctx, NULL, 0, NULL,
                                    GRN_TABLE_HASH_KEY|GRN_OBJ_WITH_SUBREC, table, NULL);
            if (!res_) {
              break;
            }
            GRN_PTR_PUT(ctx, &res_stack, res);
            res = res_;
          }
          processed = grn_table_select_index(ctx, table, si, res);
          if (!processed) {
            if (ctx->rc) { break; }
            e->codes = codes + si->start;
            e->codes_curr = si->end - si->start + 1;
            grn_table_select_sequential(ctx, table, expr, v,
                                        res, si->logical_op);
          }
        }
        GRN_QUERY_LOG(ctx, GRN_QUERY_LOG_SIZE,
                      ":", "filter(%d)", grn_table_size(ctx, res));
        if (ctx->rc) {
          if (res_created) {
            grn_obj_close(ctx, res);
          }
          res = NULL;
          break;
        }
      }

      i = 0;
      if (!res_created) { i++; }
      for (; i < GRN_BULK_VSIZE(&res_stack) / sizeof(grn_obj *); i++) {
        grn_obj *stacked_res;
        stacked_res = *((grn_obj **)GRN_BULK_HEAD(&res_stack) + i);
        grn_obj_close(ctx, stacked_res);
      }
      GRN_OBJ_FIN(ctx, &res_stack);
      e->codes = codes;
      e->codes_curr = codes_curr;

      grn_scanner_close(ctx, scanner);
    } else {
      if (!ctx->rc) {
        grn_table_select_sequential(ctx, table, expr, v, res, op);
        if (ctx->rc) {
          if (res_created) {
            grn_obj_close(ctx, res);
          }
          res = NULL;
        }
      }
    }
  }
  GRN_API_RETURN(res);
}

/* grn_expr_parse */

grn_obj *
grn_ptr_value_at(grn_obj *obj, int offset)
{
  int size = GRN_BULK_VSIZE(obj) / sizeof(grn_obj *);
  if (offset < 0) { offset = size + offset; }
  return (0 <= offset && offset < size)
    ? (((grn_obj **)GRN_BULK_HEAD(obj))[offset])
    : NULL;
}

int32_t
grn_int32_value_at(grn_obj *obj, int offset)
{
  int size = GRN_BULK_VSIZE(obj) / sizeof(int32_t);
  if (offset < 0) { offset = size + offset; }
  return (0 <= offset && offset < size)
    ? (((int32_t *)GRN_BULK_HEAD(obj))[offset])
    : 0;
}

/* grn_expr_create_from_str */

#include "grn_snip.h"

typedef struct {
  grn_ctx *ctx;
  grn_obj *e;
  grn_obj *v;
  const char *str;
  const char *cur;
  const char *str_end;
  grn_obj *table;
  grn_obj *default_column;
  grn_obj buf;
  grn_obj token_stack;
  grn_obj column_stack;
  grn_obj op_stack;
  grn_obj mode_stack;
  grn_obj max_interval_stack;
  grn_obj similarity_threshold_stack;
  grn_operator default_op;
  grn_select_optarg opt;
  grn_operator default_mode;
  grn_expr_flags flags;
  grn_expr_flags default_flags;
  int escalation_threshold;
  int escalation_decaystep;
  int weight_offset;
  grn_hash *weight_set;
  snip_cond *snip_conds;
  grn_hash *object_literal;
} efs_info;

typedef struct {
  grn_operator op;
  int weight;
} efs_op;

inline static void
skip_space(grn_ctx *ctx, efs_info *q)
{
  unsigned int len;
  while (q->cur < q->str_end && grn_isspace(q->cur, ctx->encoding)) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, q->cur, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    q->cur += len;
  }
}

static grn_bool
get_op(efs_info *q, efs_op *op, grn_operator *mode, int *option)
{
  grn_bool found = GRN_TRUE;
  const char *start, *end = q->cur;
  switch (*end) {
  case 'S' :
    *mode = GRN_OP_SIMILAR;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_SIMILARITY_THRESHOLD; }
    q->cur = end;
    break;
  case 'N' :
    *mode = GRN_OP_NEAR;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_MAX_INTERVAL; }
    q->cur = end;
    break;
  case 'n' :
    *mode = GRN_OP_NEAR2;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_MAX_INTERVAL; }
    q->cur = end;
    break;
  case 'T' :
    *mode = GRN_OP_TERM_EXTRACT;
    start = ++end;
    *option = grn_atoi(start, q->str_end, (const char **)&end);
    if (start == end) { *option = DEFAULT_TERM_EXTRACT_POLICY; }
    q->cur = end;
    break;
  case 'X' : /* force exact mode */
    op->op = GRN_OP_AND;
    *mode = GRN_OP_EXACT;
    *option = 0;
    start = ++end;
    q->cur = end;
    break;
  default :
    found = GRN_FALSE;
    break;
  }
  return found;
}

#define DISABLE_UNUSED_CODE 1
#ifndef DISABLE_UNUSED_CODE
static const char *
get_weight_vector(grn_ctx *ctx, efs_info *query, const char *source)
{
  const char *p;

  if (!query->opt.weight_vector &&
      !query->weight_set &&
      !(query->opt.weight_vector = GRN_CALLOC(sizeof(int) * DEFAULT_WEIGHT_VECTOR_SIZE))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "get_weight_vector malloc fail");
    return source;
  }
  for (p = source; p < query->str_end; ) {
    unsigned int key;
    int value;

    /* key, key is not zero */
    key = grn_atoui(p, query->str_end, &p);
    if (!key || key > GRN_ID_MAX) { break; }

    /* value */
    if (*p == ':') {
      p++;
      value = grn_atoi(p, query->str_end, &p);
    } else {
      value = 1;
    }

    if (query->weight_set) {
      int *pval;
      if (grn_hash_add(ctx, query->weight_set, &key, sizeof(unsigned int), (void **)&pval, NULL)) {
        *pval = value;
      }
    } else if (key < DEFAULT_WEIGHT_VECTOR_SIZE) {
      query->opt.weight_vector[key - 1] = value;
    } else {
      GRN_FREE(query->opt.weight_vector);
      query->opt.weight_vector = NULL;
      if (!(query->weight_set = grn_hash_create(ctx, NULL, sizeof(unsigned int), sizeof(int),
                                                0))) {
        return source;
      }
      p = source;           /* reparse */
      continue;
    }
    if (*p != ',') { break; }
    p++;
  }
  return p;
}

static void
get_pragma(grn_ctx *ctx, efs_info *q)
{
  const char *start, *end = q->cur;
  while (end < q->str_end && *end == GRN_QUERY_PREFIX) {
    if (++end >= q->str_end) { break; }
    switch (*end) {
    case 'E' :
      start = ++end;
      q->escalation_threshold = grn_atoi(start, q->str_end, (const char **)&end);
      while (end < q->str_end && (('0' <= *end && *end <= '9') || *end == '-')) { end++; }
      if (*end == ',') {
        start = ++end;
        q->escalation_decaystep = grn_atoi(start, q->str_end, (const char **)&end);
      }
      q->cur = end;
      break;
    case 'D' :
      start = ++end;
      while (end < q->str_end && *end != GRN_QUERY_PREFIX && !grn_isspace(end, ctx->encoding)) {
        end++;
      }
      if (end > start) {
        switch (*start) {
        case 'O' :
          q->default_op = GRN_OP_OR;
          break;
        case GRN_QUERY_AND :
          q->default_op = GRN_OP_AND;
          break;
        case GRN_QUERY_AND_NOT :
          q->default_op = GRN_OP_AND_NOT;
          break;
        case GRN_QUERY_ADJ_INC :
          q->default_op = GRN_OP_ADJUST;
          break;
        }
      }
      q->cur = end;
      break;
    case 'W' :
      start = ++end;
      end = (char *)get_weight_vector(ctx, q, start);
      q->cur = end;
      break;
    }
  }
}

static int
section_weight_cb(grn_ctx *ctx, grn_hash *r, const void *rid, int sid, void *arg)
{
  int *w;
  grn_hash *s = (grn_hash *)arg;
  if (s && grn_hash_get(ctx, s, &sid, sizeof(grn_id), (void **)&w)) {
    return *w;
  } else {
    return 0;
  }
}
#endif

#include "grn_ecmascript.h"
#include "grn_ecmascript.c"

static grn_rc
grn_expr_parser_open(grn_ctx *ctx)
{
  if (!ctx->impl->parser) {
    yyParser *pParser = GRN_MALLOCN(yyParser, 1);
    if (pParser) {
      pParser->yyidx = -1;
#if YYSTACKDEPTH<=0
      yyGrowStack(pParser);
#endif
      ctx->impl->parser = pParser;
    }
  }
  return ctx->rc;
}

#define PARSE(token) grn_expr_parser(ctx->impl->parser, (token), 0, q)

static void
accept_query_string(grn_ctx *ctx, efs_info *efsi,
                    const char *str, unsigned int str_size)
{
  grn_obj *column, *token;
  grn_operator mode;

  GRN_PTR_PUT(ctx, &efsi->token_stack,
              grn_expr_add_str(ctx, efsi->e, str, str_size));
  {
    efs_info *q = efsi;
    PARSE(GRN_EXPR_TOKEN_QSTRING);
  }

  GRN_PTR_POP(&efsi->token_stack, token);
  column = grn_ptr_value_at(&efsi->column_stack, -1);
  grn_expr_append_const(efsi->ctx, efsi->e, column, GRN_OP_GET_VALUE, 1);
  grn_expr_append_obj(efsi->ctx, efsi->e, token, GRN_OP_PUSH, 1);

  mode = grn_int32_value_at(&efsi->mode_stack, -1);
  switch (mode) {
  case GRN_OP_NEAR :
  case GRN_OP_NEAR2 :
    {
      int max_interval;
      max_interval = grn_int32_value_at(&efsi->max_interval_stack, -1);
      grn_expr_append_const_int(efsi->ctx, efsi->e, max_interval,
                                GRN_OP_PUSH, 1);
      grn_expr_append_op(efsi->ctx, efsi->e, mode, 3);
    }
    break;
  case GRN_OP_SIMILAR :
    {
      int similarity_threshold;
      similarity_threshold =
        grn_int32_value_at(&efsi->similarity_threshold_stack, -1);
      grn_expr_append_const_int(efsi->ctx, efsi->e, similarity_threshold,
                                GRN_OP_PUSH, 1);
      grn_expr_append_op(efsi->ctx, efsi->e, mode, 3);
    }
    break;
  default :
    grn_expr_append_op(efsi->ctx, efsi->e, mode, 2);
    break;
  }
}

static grn_rc
get_word_(grn_ctx *ctx, efs_info *q)
{
  const char *end;
  unsigned int len;
  GRN_BULK_REWIND(&q->buf);
  for (end = q->cur;; ) {
    /* null check and length check */
    if (!(len = grn_charlen(ctx, end, q->str_end))) {
      q->cur = q->str_end;
      break;
    }
    if (grn_isspace(end, ctx->encoding) ||
        *end == GRN_QUERY_PARENL || *end == GRN_QUERY_PARENR) {
      q->cur = end;
      break;
    }
    if (q->flags & GRN_EXPR_ALLOW_COLUMN && *end == GRN_QUERY_COLUMN) {
      grn_operator mode;
      grn_obj *c = grn_obj_column(ctx, q->table,
                                  GRN_TEXT_VALUE(&q->buf),
                                  GRN_TEXT_LEN(&q->buf));
      if (c && end + 1 < q->str_end) {
        //        efs_op op;
        switch (end[1]) {
        case '!' :
          mode = GRN_OP_NOT_EQUAL;
          q->cur = end + 2;
          break;
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            mode = GRN_OP_ASSIGN;
            q->cur = end + 2;
          } else {
            mode = GRN_OP_EQUAL;
            q->cur = end + 1;
          }
          break;
        case '<' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_LESS_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_LESS;
            q->cur = end + 2;
          }
          break;
        case '>' :
          if (end + 2 < q->str_end && end[2] == '=') {
            mode = GRN_OP_GREATER_EQUAL;
            q->cur = end + 3;
          } else {
            mode = GRN_OP_GREATER;
            q->cur = end + 2;
          }
          break;
        case '@' :
          mode = GRN_OP_MATCH;
          q->cur = end + 2;
          break;
        case '^' :
          mode = GRN_OP_PREFIX;
          q->cur = end + 2;
          break;
        case '$' :
          mode = GRN_OP_SUFFIX;
          q->cur = end + 2;
          break;
        case '~' :
          mode = GRN_OP_REGEXP;
          q->cur = end + 2;
          break;
        default :
          mode = GRN_OP_EQUAL;
          q->cur = end + 1;
          break;
        }
      } else {
        ERR(GRN_INVALID_ARGUMENT, "column lookup failed");
        q->cur = q->str_end;
        return ctx->rc;
      }
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      PARSE(GRN_EXPR_TOKEN_RELATIVE_OP);

      grn_expr_take_obj(ctx, q->e, c);
      GRN_PTR_PUT(ctx, &q->column_stack, c);
      GRN_INT32_PUT(ctx, &q->mode_stack, mode);

      return GRN_SUCCESS;
    } else if (GRN_TEXT_LEN(&q->buf) > 0 && *end == GRN_QUERY_PREFIX) {
      q->cur = end + 1;
      GRN_INT32_PUT(ctx, &q->mode_stack, GRN_OP_PREFIX);
      break;
    } else if (*end == GRN_QUERY_ESCAPE) {
      end += len;
      if (!(len = grn_charlen(ctx, end, q->str_end))) {
        q->cur = q->str_end;
        break;
      }
    }
    GRN_TEXT_PUT(ctx, &q->buf, end, len);
    end += len;
  }
  accept_query_string(ctx, q, GRN_TEXT_VALUE(&q->buf), GRN_TEXT_LEN(&q->buf));

  return GRN_SUCCESS;
}

static grn_rc
parse_query(grn_ctx *ctx, efs_info *q)
{
  int option = 0;
  grn_operator mode;
  efs_op op_, *op = &op_;
  grn_bool first_token = GRN_TRUE;
  grn_bool block_started = GRN_FALSE;

  op->op = q->default_op;
  op->weight = DEFAULT_WEIGHT;
  while (!ctx->rc) {
    skip_space(ctx, q);
    if (q->cur >= q->str_end) { goto exit; }
    switch (*q->cur) {
    case '\0' :
      goto exit;
      break;
    case GRN_QUERY_PARENR :
      PARSE(GRN_EXPR_TOKEN_PARENR);
      q->cur++;
      break;
    case GRN_QUERY_QUOTEL :
      q->cur++;

      {
        const char *start, *s;
        start = s = q->cur;
        GRN_BULK_REWIND(&q->buf);
        while (1) {
          unsigned int len;
          if (s >= q->str_end) {
            q->cur = s;
            break;
          }
          len = grn_charlen(ctx, s, q->str_end);
          if (len == 0) {
            /* invalid string containing malformed multibyte char */
            goto exit;
          } else if (len == 1) {
            if (*s == GRN_QUERY_QUOTER) {
              q->cur = s + 1;
              break;
            } else if (*s == GRN_QUERY_ESCAPE && s + 1 < q->str_end) {
              s++;
              len = grn_charlen(ctx, s, q->str_end);
            }
          }
          GRN_TEXT_PUT(ctx, &q->buf, s, len);
          s += len;

        }
        accept_query_string(ctx, q,
                            GRN_TEXT_VALUE(&q->buf), GRN_TEXT_LEN(&q->buf));
      }

      break;
    case GRN_QUERY_PREFIX :
      q->cur++;
      if (get_op(q, op, &mode, &option)) {
        switch (mode) {
        case GRN_OP_NEAR :
        case GRN_OP_NEAR2 :
          GRN_INT32_PUT(ctx, &q->max_interval_stack, option);
          break;
        case GRN_OP_SIMILAR :
          GRN_INT32_PUT(ctx, &q->similarity_threshold_stack, option);
          break;
        default :
          break;
        }
        GRN_INT32_PUT(ctx, &q->mode_stack, mode);
        PARSE(GRN_EXPR_TOKEN_RELATIVE_OP);
      } else {
        q->cur--;
        get_word_(ctx, q);
      }
      break;
    case GRN_QUERY_AND :
      if (!first_token) {
        op->op = GRN_OP_AND;
        PARSE(GRN_EXPR_TOKEN_LOGICAL_AND);
      }
      q->cur++;
      break;
    case GRN_QUERY_AND_NOT :
      if (first_token && (q->flags & GRN_EXPR_ALLOW_LEADING_NOT)) {
        grn_obj *all_records = grn_ctx_get(ctx, "all_records", 11);
        if (all_records) {
          /* dummy token */
          PARSE(GRN_EXPR_TOKEN_QSTRING);
          grn_expr_append_obj(ctx, q->e, all_records, GRN_OP_PUSH, 1);
          grn_expr_append_op(ctx, q->e, GRN_OP_CALL, 0);
        }
      }
      op->op = GRN_OP_AND_NOT;
      PARSE(GRN_EXPR_TOKEN_LOGICAL_AND_NOT);
      q->cur++;
      break;
    case GRN_QUERY_ADJ_INC :
      if (op->weight < 127) { op->weight++; }
      op->op = GRN_OP_ADJUST;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      q->cur++;
      break;
    case GRN_QUERY_ADJ_DEC :
      if (op->weight > -128) { op->weight--; }
      op->op = GRN_OP_ADJUST;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      q->cur++;
      break;
    case GRN_QUERY_ADJ_NEG :
      op->op = GRN_OP_ADJUST;
      op->weight = -1;
      PARSE(GRN_EXPR_TOKEN_ADJUST);
      q->cur++;
      break;
    case GRN_QUERY_PARENL :
      PARSE(GRN_EXPR_TOKEN_PARENL);
      q->cur++;
      block_started = GRN_TRUE;
      break;
    case 'O' :
      if (q->cur[1] == 'R' && q->cur[2] == ' ') {
        PARSE(GRN_EXPR_TOKEN_LOGICAL_OR);
        q->cur += 2;
        break;
      }
      /* fallthru */
    default :
      get_word_(ctx, q);
      break;
    }
    first_token = block_started;
    block_started = GRN_FALSE;
  }
exit :
  PARSE(0);
  return GRN_SUCCESS;
}

static grn_rc
get_string(grn_ctx *ctx, efs_info *q, char quote)
{
  const char *s;
  unsigned int len;
  grn_rc rc = GRN_END_OF_DATA;
  GRN_BULK_REWIND(&q->buf);
  for (s = q->cur + 1; s < q->str_end; s += len) {
    if (!(len = grn_charlen(ctx, s, q->str_end))) { break; }
    if (len == 1) {
      if (*s == quote) {
        s++;
        rc = GRN_SUCCESS;
        break;
      }
      if (*s == GRN_QUERY_ESCAPE && s + 1 < q->str_end) {
        s++;
        if (!(len = grn_charlen(ctx, s, q->str_end))) { break; }
      }
    }
    GRN_TEXT_PUT(ctx, &q->buf, s, len);
  }
  q->cur = s;
  return rc;
}

static grn_obj *
resolve_top_level_name(grn_ctx *ctx, const char *name, unsigned int name_size)
{
  unsigned int i;
  unsigned int first_delimiter_position = 0;
  unsigned int n_delimiters = 0;
  grn_obj *top_level_object;
  grn_obj *object;

  for (i = 0; i < name_size; i++) {
    if (name[i] != GRN_DB_DELIMITER) {
      continue;
    }

    if (n_delimiters == 0) {
      first_delimiter_position = i;
    }
    n_delimiters++;
  }

  if (n_delimiters < 2) {
    return grn_ctx_get(ctx, name, name_size);
  }

  top_level_object = grn_ctx_get(ctx, name, first_delimiter_position);
  if (!top_level_object) {
    return NULL;
  }
  object = grn_obj_column(ctx, top_level_object,
                          name + first_delimiter_position + 1,
                          name_size - first_delimiter_position - 1);
  grn_obj_unlink(ctx, top_level_object);
  return object;
}

static grn_rc
get_identifier(grn_ctx *ctx, efs_info *q, grn_obj *name_resolve_context)
{
  const char *s;
  unsigned int len;
  grn_rc rc = GRN_SUCCESS;
  for (s = q->cur; s < q->str_end; s += len) {
    if (!(len = grn_charlen(ctx, s, q->str_end))) {
      rc = GRN_END_OF_DATA;
      goto exit;
    }
    if (grn_isspace(s, ctx->encoding)) { goto done; }
    if (len == 1) {
      switch (*s) {
      case '\0' : case '(' : case ')' : case '{' : case '}' :
      case '[' : case ']' : case ',' : case ':' : case '@' :
      case '?' : case '"' : case '*' : case '+' : case '-' :
      case '|' : case '/' : case '%' : case '!' : case '^' :
      case '&' : case '>' : case '<' : case '=' : case '~' :
        /* case '.' : */
        goto done;
        break;
      }
    }
  }
done :
  len = s - q->cur;
  switch (*q->cur) {
  case 'd' :
    if (len == 6 && !memcmp(q->cur, "delete", 6)) {
      PARSE(GRN_EXPR_TOKEN_DELETE);
      goto exit;
    }
    break;
  case 'f' :
    if (len == 5 && !memcmp(q->cur, "false", 5)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_BOOLEAN);
      GRN_BOOL_INIT(&buf, 0);
      GRN_BOOL_SET(ctx, &buf, 0);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  case 'i' :
    if (len == 2 && !memcmp(q->cur, "in", 2)) {
      PARSE(GRN_EXPR_TOKEN_IN);
      goto exit;
    }
    break;
  case 'n' :
    if (len == 4 && !memcmp(q->cur, "null", 4)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_NULL);
      GRN_VOID_INIT(&buf);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  case 't' :
    if (len == 4 && !memcmp(q->cur, "true", 4)) {
      grn_obj buf;
      PARSE(GRN_EXPR_TOKEN_BOOLEAN);
      GRN_BOOL_INIT(&buf, 0);
      GRN_BOOL_SET(ctx, &buf, 1);
      grn_expr_append_const(ctx, q->e, &buf, GRN_OP_PUSH, 1);
      GRN_OBJ_FIN(ctx, &buf);
      goto exit;
    }
    break;
  }
  {
    grn_obj *obj;
    const char *name = q->cur;
    unsigned int name_size = s - q->cur;
    if (name_resolve_context) {
      if ((obj = grn_obj_column(ctx, name_resolve_context, name, name_size))) {
        if (obj->header.type == GRN_ACCESSOR) {
          grn_expr_take_obj(ctx, q->e, obj);
        }
        PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
        grn_expr_append_obj(ctx, q->e, obj, GRN_OP_GET_VALUE, 2);
        goto exit;
      }
    }
    if ((obj = grn_expr_get_var(ctx, q->e, name, name_size))) {
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_PUSH, 1);
      goto exit;
    }
    if ((obj = grn_obj_column(ctx, q->table, name, name_size))) {
      if (obj->header.type == GRN_ACCESSOR) {
        grn_expr_take_obj(ctx, q->e, obj);
      }
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_GET_VALUE, 1);
      goto exit;
    }
    if ((obj = resolve_top_level_name(ctx, name, name_size))) {
      if (obj->header.type == GRN_ACCESSOR) {
        grn_expr_take_obj(ctx, q->e, obj);
      }
      PARSE(GRN_EXPR_TOKEN_IDENTIFIER);
      grn_expr_append_obj(ctx, q->e, obj, GRN_OP_PUSH, 1);
      goto exit;
    }
    if (q->flags & GRN_EXPR_SYNTAX_OUTPUT_COLUMNS) {
      PARSE(GRN_EXPR_TOKEN_NONEXISTENT_COLUMN);
    } else {
      rc = GRN_SYNTAX_ERROR;
    }
  }
exit :
  q->cur = s;
  return rc;
}

static void
set_tos_minor_to_curr(grn_ctx *ctx, efs_info *q)
{
  yyParser *pParser = ctx->impl->parser;
  yyStackEntry *yytos = &pParser->yystack[pParser->yyidx];
  yytos->minor.yy0 = ((grn_expr *)(q->e))->codes_curr;
}

static grn_obj *
parse_script_extract_name_resolve_context(grn_ctx *ctx, efs_info *q)
{
  grn_expr *expr = (grn_expr *)(q->e);
  grn_expr_code *code_start;
  grn_expr_code *code_last;

  if (expr->codes_curr == 0) {
    return NULL;
  }

  code_start = expr->codes;
  code_last = code_start + (expr->codes_curr - 1);
  switch (code_last->op) {
  case GRN_OP_GET_MEMBER :
    {
      unsigned int n_used_codes_for_key;
      grn_expr_code *code_key;
      grn_expr_code *code_receiver;

      code_key = code_last - 1;
      if (code_key < code_start) {
        return NULL;
      }

      n_used_codes_for_key = grn_expr_code_n_used_codes(ctx,
                                                        code_start,
                                                        code_key);
      if (n_used_codes_for_key == 0) {
        return NULL;
      }
      code_receiver = code_key - n_used_codes_for_key;
      if (code_receiver < code_start) {
        return NULL;
      }
      return code_receiver->value;
    }
    break;
  default :
    /* TODO: Support other operators. */
    return NULL;
    break;
  }
}

static grn_rc
parse_script(grn_ctx *ctx, efs_info *q)
{
  grn_rc rc = GRN_SUCCESS;
  grn_obj *name_resolve_context = NULL;
  for (;;) {
    grn_obj *current_name_resolve_context = name_resolve_context;
    name_resolve_context = NULL;
    skip_space(ctx, q);
    if (q->cur >= q->str_end) { rc = GRN_END_OF_DATA; goto exit; }
    switch (*q->cur) {
    case '\0' :
      rc = GRN_END_OF_DATA;
      goto exit;
      break;
    case '(' :
      PARSE(GRN_EXPR_TOKEN_PARENL);
      q->cur++;
      break;
    case ')' :
      PARSE(GRN_EXPR_TOKEN_PARENR);
      q->cur++;
      break;
    case '{' :
      PARSE(GRN_EXPR_TOKEN_BRACEL);
      q->cur++;
      break;
    case '}' :
      PARSE(GRN_EXPR_TOKEN_BRACER);
      q->cur++;
      break;
    case '[' :
      PARSE(GRN_EXPR_TOKEN_BRACKETL);
      q->cur++;
      break;
    case ']' :
      PARSE(GRN_EXPR_TOKEN_BRACKETR);
      q->cur++;
      break;
    case ',' :
      PARSE(GRN_EXPR_TOKEN_COMMA);
      q->cur++;
      break;
    case '.' :
      PARSE(GRN_EXPR_TOKEN_DOT);
      name_resolve_context = parse_script_extract_name_resolve_context(ctx, q);
      q->cur++;
      break;
    case ':' :
      PARSE(GRN_EXPR_TOKEN_COLON);
      q->cur++;
      set_tos_minor_to_curr(ctx, q);
      grn_expr_append_op(ctx, q->e, GRN_OP_JUMP, 0);
      break;
    case '@' :
      switch (q->cur[1]) {
      case '^' :
        PARSE(GRN_EXPR_TOKEN_PREFIX);
        q->cur += 2;
        break;
      case '$' :
        PARSE(GRN_EXPR_TOKEN_SUFFIX);
        q->cur += 2;
        break;
      case '~' :
        PARSE(GRN_EXPR_TOKEN_REGEXP);
        q->cur += 2;
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MATCH);
        q->cur++;
        break;
      }
      break;
    case '~' :
      PARSE(GRN_EXPR_TOKEN_BITWISE_NOT);
      q->cur++;
      break;
    case '?' :
      PARSE(GRN_EXPR_TOKEN_QUESTION);
      q->cur++;
      set_tos_minor_to_curr(ctx, q);
      grn_expr_append_op(ctx, q->e, GRN_OP_CJUMP, 0);
      break;
    case '"' :
      if ((rc = get_string(ctx, q, '"'))) { goto exit; }
      PARSE(GRN_EXPR_TOKEN_STRING);
      grn_expr_append_const(ctx, q->e, &q->buf, GRN_OP_PUSH, 1);
      break;
    case '\'' :
      if ((rc = get_string(ctx, q, '\''))) { goto exit; }
      PARSE(GRN_EXPR_TOKEN_STRING);
      grn_expr_append_const(ctx, q->e, &q->buf, GRN_OP_PUSH, 1);
      break;
    case '*' :
      switch (q->cur[1]) {
      case 'N' :
        PARSE(GRN_EXPR_TOKEN_NEAR);
        q->cur += 2;
        break;
      case 'S' :
        PARSE(GRN_EXPR_TOKEN_SIMILAR);
        q->cur += 2;
        break;
      case 'T' :
        PARSE(GRN_EXPR_TOKEN_TERM_EXTRACT);
        q->cur += 2;
        break;
      case '>' :
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        q->cur += 2;
        break;
      case '<' :
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        q->cur += 2;
        break;
      case '~' :
        PARSE(GRN_EXPR_TOKEN_ADJUST);
        q->cur += 2;
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_STAR_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'*=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_STAR);
        q->cur++;
        break;
      }
      break;
    case '+' :
      switch (q->cur[1]) {
      case '+' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_INCR);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'++' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_PLUS_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'+=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_PLUS);
        q->cur++;
        break;
      }
      break;
    case '-' :
      switch (q->cur[1]) {
      case '-' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_DECR);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'--' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_MINUS_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'-=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MINUS);
        q->cur++;
        break;
      }
      break;
    case '|' :
      switch (q->cur[1]) {
      case '|' :
        PARSE(GRN_EXPR_TOKEN_LOGICAL_OR);
        q->cur += 2;
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_OR_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'|=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_OR);
        q->cur++;
        break;
      }
      break;
    case '/' :
      switch (q->cur[1]) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_SLASH_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'/=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_SLASH);
        q->cur++;
        break;
      }
      break;
    case '%' :
      switch (q->cur[1]) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_MOD_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'%%=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_MOD);
        q->cur++;
        break;
      }
      break;
    case '!' :
      switch (q->cur[1]) {
      case '=' :
        PARSE(GRN_EXPR_TOKEN_NOT_EQUAL);
        q->cur += 2;
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_NOT);
        q->cur++;
        break;
      }
      break;
    case '^' :
      switch (q->cur[1]) {
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          q->cur += 2;
          PARSE(GRN_EXPR_TOKEN_XOR_ASSIGN);
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'^=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_XOR);
        q->cur++;
        break;
      }
      break;
    case '&' :
      switch (q->cur[1]) {
      case '&' :
        PARSE(GRN_EXPR_TOKEN_LOGICAL_AND);
        q->cur += 2;
        break;
      case '=' :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_AND_ASSIGN);
          q->cur += 2;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'&=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      case '!' :
        PARSE(GRN_EXPR_TOKEN_LOGICAL_AND_NOT);
        q->cur += 2;
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_BITWISE_AND);
        q->cur++;
        break;
      }
      break;
    case '>' :
      switch (q->cur[1]) {
      case '>' :
        switch (q->cur[2]) {
        case '>' :
          switch (q->cur[3]) {
          case '=' :
            if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
              PARSE(GRN_EXPR_TOKEN_SHIFTRR_ASSIGN);
              q->cur += 4;
            } else {
              ERR(GRN_UPDATE_NOT_ALLOWED,
                  "'>>>=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
            }
            break;
          default :
            PARSE(GRN_EXPR_TOKEN_SHIFTRR);
            q->cur += 3;
            break;
          }
          break;
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            PARSE(GRN_EXPR_TOKEN_SHIFTR_ASSIGN);
            q->cur += 3;
          } else {
            ERR(GRN_UPDATE_NOT_ALLOWED,
                "'>>=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
          }
          break;
        default :
          PARSE(GRN_EXPR_TOKEN_SHIFTR);
          q->cur += 2;
          break;
        }
        break;
      case '=' :
        PARSE(GRN_EXPR_TOKEN_GREATER_EQUAL);
        q->cur += 2;
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_GREATER);
        q->cur++;
        break;
      }
      break;
    case '<' :
      switch (q->cur[1]) {
      case '<' :
        switch (q->cur[2]) {
        case '=' :
          if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
            PARSE(GRN_EXPR_TOKEN_SHIFTL_ASSIGN);
            q->cur += 3;
          } else {
            ERR(GRN_UPDATE_NOT_ALLOWED,
                "'<<=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
          }
          break;
        default :
          PARSE(GRN_EXPR_TOKEN_SHIFTL);
          q->cur += 2;
          break;
        }
        break;
      case '=' :
        PARSE(GRN_EXPR_TOKEN_LESS_EQUAL);
        q->cur += 2;
        break;
      default :
        PARSE(GRN_EXPR_TOKEN_LESS);
        q->cur++;
        break;
      }
      break;
    case '=' :
      switch (q->cur[1]) {
      case '=' :
        PARSE(GRN_EXPR_TOKEN_EQUAL);
        q->cur += 2;
        break;
      default :
        if (q->flags & GRN_EXPR_ALLOW_UPDATE) {
          PARSE(GRN_EXPR_TOKEN_ASSIGN);
          q->cur++;
        } else {
          ERR(GRN_UPDATE_NOT_ALLOWED,
              "'=' is not allowed (%.*s)", (int)(q->str_end - q->str), q->str);
        }
        break;
      }
      break;
    case '0' : case '1' : case '2' : case '3' : case '4' :
    case '5' : case '6' : case '7' : case '8' : case '9' :
      {
        const char *rest;
        int64_t int64 = grn_atoll(q->cur, q->str_end, &rest);
        // checks to see grn_atoll was appropriate
        // (NOTE: *q->cur begins with a digit. Thus, grn_atoll parses at leaset
        //        one char.)
        if (q->str_end != rest &&
            (*rest == '.' || *rest == 'e' || *rest == 'E' ||
             (*rest >= '0' && *rest <= '9'))) {
          char *rest_float;
          double d = strtod(q->cur, &rest_float);
          grn_obj floatbuf;
          GRN_FLOAT_INIT(&floatbuf, 0);
          GRN_FLOAT_SET(ctx, &floatbuf, d);
          grn_expr_append_const(ctx, q->e, &floatbuf, GRN_OP_PUSH, 1);
          rest = rest_float;
        } else {
          const char *rest64 = rest;
          grn_atoui(q->cur, q->str_end, &rest);
          // checks to see grn_atoi failed (see above NOTE)
          if ((int64 > UINT32_MAX) ||
              (q->str_end != rest && *rest >= '0' && *rest <= '9')) {
            grn_obj int64buf;
            GRN_INT64_INIT(&int64buf, 0);
            GRN_INT64_SET(ctx, &int64buf, int64);
            grn_expr_append_const(ctx, q->e, &int64buf, GRN_OP_PUSH, 1);
            rest = rest64;
          } else if (int64 > INT32_MAX || int64 < INT32_MIN) {
            grn_obj int64buf;
            GRN_INT64_INIT(&int64buf, 0);
            GRN_INT64_SET(ctx, &int64buf, int64);
            grn_expr_append_const(ctx, q->e, &int64buf, GRN_OP_PUSH, 1);
          } else {
            grn_obj int32buf;
            GRN_INT32_INIT(&int32buf, 0);
            GRN_INT32_SET(ctx, &int32buf, (int32_t)int64);
            grn_expr_append_const(ctx, q->e, &int32buf, GRN_OP_PUSH, 1);
          }
        }
        PARSE(GRN_EXPR_TOKEN_DECIMAL);
        q->cur = rest;
      }
      break;
    default :
      if ((rc = get_identifier(ctx, q, current_name_resolve_context))) {
        goto exit;
      }
      break;
    }
    if (ctx->rc) { rc = ctx->rc; break; }
  }
exit :
  PARSE(0);
  return rc;
}

grn_rc
grn_expr_parse(grn_ctx *ctx, grn_obj *expr,
               const char *str, unsigned int str_size,
               grn_obj *default_column, grn_operator default_mode,
               grn_operator default_op, grn_expr_flags flags)
{
  efs_info efsi;
  if (grn_expr_parser_open(ctx)) { return ctx->rc; }
  GRN_API_ENTER;
  efsi.ctx = ctx;
  efsi.str = str;
  if ((efsi.v = grn_expr_get_var_by_offset(ctx, expr, 0)) &&
      (efsi.table = grn_ctx_at(ctx, efsi.v->header.domain))) {
    GRN_TEXT_INIT(&efsi.buf, 0);
    GRN_INT32_INIT(&efsi.op_stack, GRN_OBJ_VECTOR);
    GRN_INT32_INIT(&efsi.mode_stack, GRN_OBJ_VECTOR);
    GRN_INT32_INIT(&efsi.max_interval_stack, GRN_OBJ_VECTOR);
    GRN_INT32_INIT(&efsi.similarity_threshold_stack, GRN_OBJ_VECTOR);
    GRN_PTR_INIT(&efsi.column_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
    GRN_PTR_INIT(&efsi.token_stack, GRN_OBJ_VECTOR, GRN_ID_NIL);
    efsi.e = expr;
    efsi.str = str;
    efsi.cur = str;
    efsi.str_end = str + str_size;
    efsi.default_column = default_column;
    GRN_PTR_PUT(ctx, &efsi.column_stack, default_column);
    GRN_INT32_PUT(ctx, &efsi.op_stack, default_op);
    GRN_INT32_PUT(ctx, &efsi.mode_stack, default_mode);
    efsi.default_flags = efsi.flags = flags;
    efsi.escalation_threshold = GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD;
    efsi.escalation_decaystep = DEFAULT_DECAYSTEP;
    efsi.weight_offset = 0;
    efsi.opt.weight_vector = NULL;
    efsi.weight_set = NULL;
    efsi.object_literal = NULL;

    if (flags & (GRN_EXPR_SYNTAX_SCRIPT |
                 GRN_EXPR_SYNTAX_OUTPUT_COLUMNS |
                 GRN_EXPR_SYNTAX_ADJUSTER)) {
      efs_info *q = &efsi;
      if (flags & GRN_EXPR_SYNTAX_OUTPUT_COLUMNS) {
        PARSE(GRN_EXPR_TOKEN_START_OUTPUT_COLUMNS);
      } else if (flags & GRN_EXPR_SYNTAX_ADJUSTER) {
        PARSE(GRN_EXPR_TOKEN_START_ADJUSTER);
      }
      parse_script(ctx, &efsi);
    } else {
      parse_query(ctx, &efsi);
    }

    /*
        grn_obj strbuf;
        GRN_TEXT_INIT(&strbuf, 0);
        grn_expr_inspect_internal(ctx, &strbuf, expr);
        GRN_TEXT_PUTC(ctx, &strbuf, '\0');
        GRN_LOG(ctx, GRN_LOG_NOTICE, "query=(%s)", GRN_TEXT_VALUE(&strbuf));
        GRN_OBJ_FIN(ctx, &strbuf);
    */

    /*
    efsi.opt.vector_size = DEFAULT_WEIGHT_VECTOR_SIZE;
    efsi.opt.func = efsi.weight_set ? section_weight_cb : NULL;
    efsi.opt.func_arg = efsi.weight_set;
    efsi.snip_conds = NULL;
    */
    GRN_OBJ_FIN(ctx, &efsi.op_stack);
    GRN_OBJ_FIN(ctx, &efsi.mode_stack);
    GRN_OBJ_FIN(ctx, &efsi.max_interval_stack);
    GRN_OBJ_FIN(ctx, &efsi.similarity_threshold_stack);
    GRN_OBJ_FIN(ctx, &efsi.column_stack);
    GRN_OBJ_FIN(ctx, &efsi.token_stack);
    GRN_OBJ_FIN(ctx, &efsi.buf);
    if (efsi.object_literal) {
      grn_obj *value;
      GRN_HASH_EACH(ctx, efsi.object_literal, i, NULL, NULL, (void **)&value, {
        GRN_OBJ_FIN(ctx, value);
      });
      grn_hash_close(ctx, efsi.object_literal);
    }
  } else {
    ERR(GRN_INVALID_ARGUMENT, "variable is not defined correctly");
  }
  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_expr_parser_close(grn_ctx *ctx)
{
  if (ctx->impl->parser) {
    yyParser *pParser = (yyParser*)ctx->impl->parser;
    while (pParser->yyidx >= 0) yy_pop_parser_stack(pParser);
#if YYSTACKDEPTH<=0
    free(pParser->yystack);
#endif
    GRN_FREE(pParser);
    ctx->impl->parser = NULL;
  }
  return ctx->rc;
}

typedef grn_rc (*grn_expr_syntax_expand_term_func)(grn_ctx *ctx,
                                                   const char *term,
                                                   unsigned int term_len,
                                                   grn_obj *substituted_term,
                                                   grn_user_data *user_data);
typedef struct {
  grn_obj *table;
  grn_obj *column;
} grn_expr_syntax_expand_term_by_column_data;

static grn_rc
grn_expr_syntax_expand_term_by_func(grn_ctx *ctx,
                                    const char *term, unsigned int term_len,
                                    grn_obj *expanded_term,
                                    grn_user_data *user_data)
{
  grn_rc rc;
  grn_obj *expander = user_data->ptr;
  grn_obj grn_term;
  grn_obj *caller;
  grn_obj *rc_object;
  int nargs = 0;

  GRN_TEXT_INIT(&grn_term, GRN_OBJ_DO_SHALLOW_COPY);
  GRN_TEXT_SET(ctx, &grn_term, term, term_len);
  grn_ctx_push(ctx, &grn_term);
  nargs++;
  grn_ctx_push(ctx, expanded_term);
  nargs++;

  caller = grn_expr_create(ctx, NULL, 0);
  rc = grn_proc_call(ctx, expander, nargs, caller);
  GRN_OBJ_FIN(ctx, &grn_term);
  rc_object = grn_ctx_pop(ctx);
  rc = GRN_INT32_VALUE(rc_object);
  grn_obj_unlink(ctx, caller);

  return rc;
}

static grn_rc
grn_expr_syntax_expand_term_by_column(grn_ctx *ctx,
                                      const char *term, unsigned int term_len,
                                      grn_obj *expanded_term,
                                      grn_user_data *user_data)
{
  grn_rc rc = GRN_END_OF_DATA;
  grn_id id;
  grn_expr_syntax_expand_term_by_column_data *data = user_data->ptr;
  grn_obj *table, *column;

  table = data->table;
  column = data->column;
  if ((id = grn_table_get(ctx, table, term, term_len))) {
    if ((column->header.type == GRN_COLUMN_VAR_SIZE) &&
        ((column->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_VECTOR)) {
      unsigned int i, n;
      grn_obj values;
      GRN_TEXT_INIT(&values, GRN_OBJ_VECTOR);
      grn_obj_get_value(ctx, column, id, &values);
      n = grn_vector_size(ctx, &values);
      if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, '('); }
      for (i = 0; i < n; i++) {
        const char *value;
        unsigned int length;
        if (i > 0) {
          GRN_TEXT_PUTS(ctx, expanded_term, " OR ");
        }
        if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, '('); }
        length = grn_vector_get_element(ctx, &values, i, &value, NULL, NULL);
        GRN_TEXT_PUT(ctx, expanded_term, value, length);
        if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, ')'); }
      }
      if (n > 1) { GRN_TEXT_PUTC(ctx, expanded_term, ')'); }
      GRN_OBJ_FIN(ctx, &values);
    } else {
      grn_obj_get_value(ctx, column, id, expanded_term);
    }
    rc = GRN_SUCCESS;
  }
  return rc;
}

static grn_rc
grn_expr_syntax_expand_query_terms(grn_ctx *ctx,
                                   const char *query, unsigned int query_size,
                                   grn_expr_flags flags,
                                   grn_obj *expanded_query,
                                   grn_expr_syntax_expand_term_func expand_term_func,
                                   grn_user_data *user_data)
{
  grn_obj buf;
  unsigned int len;
  const char *start, *cur = query, *query_end = query + (size_t)query_size;
  GRN_TEXT_INIT(&buf, 0);
  for (;;) {
    while (cur < query_end && grn_isspace(cur, ctx->encoding)) {
      if (!(len = grn_charlen(ctx, cur, query_end))) { goto exit; }
      GRN_TEXT_PUT(ctx, expanded_query, cur, len);
      cur += len;
    }
    if (query_end <= cur) { break; }
    switch (*cur) {
    case '\0' :
      goto exit;
      break;
    case GRN_QUERY_AND :
    case GRN_QUERY_ADJ_INC :
    case GRN_QUERY_ADJ_DEC :
    case GRN_QUERY_ADJ_NEG :
    case GRN_QUERY_AND_NOT :
    case GRN_QUERY_PARENL :
    case GRN_QUERY_PARENR :
    case GRN_QUERY_PREFIX :
      GRN_TEXT_PUTC(ctx, expanded_query, *cur);
      cur++;
      break;
    case GRN_QUERY_QUOTEL :
      GRN_BULK_REWIND(&buf);
      for (start = cur++; cur < query_end; cur += len) {
        if (!(len = grn_charlen(ctx, cur, query_end))) {
          goto exit;
        } else if (len == 1) {
          if (*cur == GRN_QUERY_QUOTER) {
            cur++;
            break;
          } else if (cur + 1 < query_end && *cur == GRN_QUERY_ESCAPE) {
            cur++;
            len = grn_charlen(ctx, cur, query_end);
          }
        }
        GRN_TEXT_PUT(ctx, &buf, cur, len);
      }
      if (expand_term_func(ctx, GRN_TEXT_VALUE(&buf), GRN_TEXT_LEN(&buf),
                           expanded_query, user_data)) {
        GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
      }
      break;
    case 'O' :
      if (cur + 2 <= query_end && cur[1] == 'R' &&
          (cur + 2 == query_end || grn_isspace(cur + 2, ctx->encoding))) {
        GRN_TEXT_PUT(ctx, expanded_query, cur, 2);
        cur += 2;
        break;
      }
      /* fallthru */
    default :
      for (start = cur; cur < query_end; cur += len) {
        if (!(len = grn_charlen(ctx, cur, query_end))) {
          goto exit;
        } else if (grn_isspace(cur, ctx->encoding)) {
          break;
        } else if (len == 1) {
          if (*cur == GRN_QUERY_PARENL ||
              *cur == GRN_QUERY_PARENR ||
              *cur == GRN_QUERY_PREFIX) {
            break;
          } else if (flags & GRN_EXPR_ALLOW_COLUMN && *cur == GRN_QUERY_COLUMN) {
            if (cur + 1 < query_end) {
              switch (cur[1]) {
              case '!' :
              case '@' :
              case '^' :
              case '$' :
                cur += 2;
                break;
              case '=' :
                cur += (flags & GRN_EXPR_ALLOW_UPDATE) ? 2 : 1;
                break;
              case '<' :
              case '>' :
                cur += (cur + 2 < query_end && cur[2] == '=') ? 3 : 2;
                break;
              default :
                cur += 1;
                break;
              }
            } else {
              cur += 1;
            }
            GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
            start = cur;
            break;
          }
        }
      }
      if (start < cur) {
        if (expand_term_func(ctx, start, cur - start,
                             expanded_query, user_data)) {
          GRN_TEXT_PUT(ctx, expanded_query, start, cur - start);
        }
      }
      break;
    }
  }
exit :
  GRN_OBJ_FIN(ctx, &buf);
  return GRN_SUCCESS;
}

grn_rc
grn_expr_syntax_expand_query(grn_ctx *ctx,
                             const char *query, int query_size,
                             grn_expr_flags flags,
                             grn_obj *expander,
                             grn_obj *expanded_query)
{
  GRN_API_ENTER;

  if (query_size < 0) {
    query_size = strlen(query);
  }

  switch (expander->header.type) {
  case GRN_PROC :
    if (((grn_proc *)expander)->type == GRN_PROC_FUNCTION) {
      grn_user_data user_data;
      user_data.ptr = expander;
      grn_expr_syntax_expand_query_terms(ctx,
                                         query, query_size,
                                         flags,
                                         expanded_query,
                                         grn_expr_syntax_expand_term_by_func,
                                         &user_data);
    } else {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_obj_name(ctx, expander, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_INVALID_ARGUMENT,
          "[query][expand] "
          "proc query expander must be a function proc: <%.*s>",
          name_size, name);
    }
    break;
  case GRN_COLUMN_FIX_SIZE :
  case GRN_COLUMN_VAR_SIZE :
    {
      grn_obj *expansion_table;
      expansion_table = grn_column_table(ctx, expander);
      if (expansion_table) {
        grn_user_data user_data;
        grn_expr_syntax_expand_term_by_column_data data;
        user_data.ptr = &data;
        data.table = expansion_table;
        data.column = expander;
        grn_expr_syntax_expand_query_terms(ctx,
                                           query, query_size,
                                           flags,
                                           expanded_query,
                                           grn_expr_syntax_expand_term_by_column,
                                           &user_data);
      } else {
        char name[GRN_TABLE_MAX_KEY_SIZE];
        int name_size;
        name_size = grn_obj_name(ctx, expander, name, GRN_TABLE_MAX_KEY_SIZE);
        ERR(GRN_INVALID_ARGUMENT,
            "[query][expand] "
            "failed to get table of query expansion column: <%.*s>",
            name_size, name);
      }
    }
    break;
  default :
    {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      grn_obj type_name;

      name_size = grn_obj_name(ctx, expander, name, GRN_TABLE_MAX_KEY_SIZE);
      GRN_TEXT_INIT(&type_name, 0);
      grn_inspect_type(ctx, &type_name, expander->header.type);
      ERR(GRN_INVALID_ARGUMENT,
          "[query][expand] "
          "query expander must be a data column or function proc: <%.*s>(%.*s)",
          name_size, name,
          (int)GRN_TEXT_LEN(&type_name), GRN_TEXT_VALUE(&type_name));
      GRN_OBJ_FIN(ctx, &type_name);
    }
    break;
  }

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_expr_get_keywords(grn_ctx *ctx, grn_obj *expr, grn_obj *keywords)
{
  int i, n;
  scan_info **sis, *si;
  GRN_API_ENTER;
  if ((sis = grn_scan_info_build(ctx, expr, &n, GRN_OP_OR, GRN_FALSE))) {
    int butp = 0, nparens = 0, npbut = 0;
    grn_obj but_stack;
    GRN_UINT32_INIT(&but_stack, GRN_OBJ_VECTOR);
    for (i = n; i--;) {
      si = sis[i];
      if (si->flags & SCAN_POP) {
        nparens++;
        if (si->logical_op == GRN_OP_AND_NOT) {
          GRN_UINT32_PUT(ctx, &but_stack, npbut);
          npbut = nparens;
          butp = 1 - butp;
        }
      } else {
        if (si->op == GRN_OP_MATCH && si->query) {
          if (butp == (si->logical_op == GRN_OP_AND_NOT)) {
            GRN_PTR_PUT(ctx, keywords, si->query);
          }
        }
        if (si->flags & SCAN_PUSH) {
          if (nparens == npbut) {
            butp = 1 - butp;
            GRN_UINT32_POP(&but_stack, npbut);
          }
          nparens--;
        }
      }
    }
    GRN_OBJ_FIN(ctx, &but_stack);
    for (i = n; i--;) { SI_FREE(sis[i]); }
    GRN_FREE(sis);
  }
  GRN_API_RETURN(GRN_SUCCESS);
}

grn_rc
grn_expr_snip_add_conditions(grn_ctx *ctx, grn_obj *expr, grn_obj *snip,
                             unsigned int n_tags,
                             const char **opentags, unsigned int *opentag_lens,
                             const char **closetags, unsigned int *closetag_lens)
{
  grn_rc rc;
  grn_obj keywords;

  GRN_API_ENTER;

  GRN_PTR_INIT(&keywords, GRN_OBJ_VECTOR, GRN_ID_NIL);
  rc = grn_expr_get_keywords(ctx, expr, &keywords);
  if (rc != GRN_SUCCESS) {
    GRN_OBJ_FIN(ctx, &keywords);
    GRN_API_RETURN(rc);
  }

  if (n_tags) {
    int i;
    for (i = 0;; i = (i + 1) % n_tags) {
      grn_obj *keyword;
      GRN_PTR_POP(&keywords, keyword);
      if (!keyword) { break; }
      grn_snip_add_cond(ctx, snip,
                        GRN_TEXT_VALUE(keyword), GRN_TEXT_LEN(keyword),
                        opentags[i], opentag_lens[i],
                        closetags[i], closetag_lens[i]);
    }
  } else {
    for (;;) {
      grn_obj *keyword;
      GRN_PTR_POP(&keywords, keyword);
      if (!keyword) { break; }
      grn_snip_add_cond(ctx, snip,
                        GRN_TEXT_VALUE(keyword), GRN_TEXT_LEN(keyword),
                        NULL, 0, NULL, 0);
    }
  }
  GRN_OBJ_FIN(ctx, &keywords);

  GRN_API_RETURN(GRN_SUCCESS);
}

grn_obj *
grn_expr_snip(grn_ctx *ctx, grn_obj *expr, int flags,
              unsigned int width, unsigned int max_results,
              unsigned int n_tags,
              const char **opentags, unsigned int *opentag_lens,
              const char **closetags, unsigned int *closetag_lens,
              grn_snip_mapping *mapping)
{
  grn_obj *res = NULL;
  GRN_API_ENTER;
  if ((res = grn_snip_open(ctx, flags, width, max_results,
                           NULL, 0, NULL, 0, mapping))) {
    grn_expr_snip_add_conditions(ctx, expr, res,
                                 n_tags,
                                 opentags, opentag_lens,
                                 closetags, closetag_lens);
  }
  GRN_API_RETURN(res);
}

/*
  So far, grn_column_filter() is nothing but a very rough prototype.
  Although GRN_COLUMN_EACH() can accelerate many range queries,
  the following stuff must be resolved one by one.

  * support accessors as column
  * support tables which have deleted records
  * support various operators
  * support various column types
*/
grn_rc
grn_column_filter(grn_ctx *ctx, grn_obj *column,
                  grn_operator operator,
                  grn_obj *value, grn_obj *result_set,
                  grn_operator set_operation)
{
  uint32_t *vp;
  grn_posting posting;
  uint32_t value_ = grn_atoui(GRN_TEXT_VALUE(value), GRN_BULK_CURR(value), NULL);
  posting.sid = 1;
  posting.pos = 0;
  posting.weight = 0;
  GRN_COLUMN_EACH(ctx, column, id, vp, {
    if (*vp < value_) {
      posting.rid = id;
      grn_ii_posting_add(ctx, &posting, (grn_hash *)result_set, set_operation);
    }
  });
  grn_ii_resolve_sel_and(ctx, (grn_hash *)result_set, set_operation);
  return ctx->rc;
}

grn_rc
grn_expr_syntax_escape(grn_ctx *ctx, const char *string, int string_size,
                       const char *target_characters,
                       char escape_character,
                       grn_obj *escaped_string)
{
  grn_rc rc = GRN_SUCCESS;
  const char *current, *string_end;

  if (!string) {
    return GRN_INVALID_ARGUMENT;
  }

  GRN_API_ENTER;
  if (string_size < 0) {
    string_size = strlen(string);
  }
  string_end = string + string_size;

  current = string;
  while (current < string_end) {
    unsigned int char_size;
    char_size = grn_charlen(ctx, current, string_end);
    switch (char_size) {
    case 0 :
      /* string includes malformed multibyte character. */
      return GRN_INVALID_ARGUMENT;
      break;
    case 1 :
      if (strchr(target_characters, *current)) {
        GRN_TEXT_PUTC(ctx, escaped_string, escape_character);
      }
      GRN_TEXT_PUT(ctx, escaped_string, current, char_size);
      current += char_size;
      break;
    default :
      GRN_TEXT_PUT(ctx, escaped_string, current, char_size);
      current += char_size;
      break;
    }
  }

  GRN_API_RETURN(rc);
}

grn_rc
grn_expr_syntax_escape_query(grn_ctx *ctx, const char *query, int query_size,
                             grn_obj *escaped_query)
{
  const char target_characters[] = {
    GRN_QUERY_AND,
    GRN_QUERY_AND_NOT,
    GRN_QUERY_ADJ_INC,
    GRN_QUERY_ADJ_DEC,
    GRN_QUERY_ADJ_NEG,
    GRN_QUERY_PREFIX,
    GRN_QUERY_PARENL,
    GRN_QUERY_PARENR,
    GRN_QUERY_QUOTEL,
    GRN_QUERY_ESCAPE,
    GRN_QUERY_COLUMN,
    '\0',
  };
  return grn_expr_syntax_escape(ctx, query, query_size,
                                target_characters, GRN_QUERY_ESCAPE,
                                escaped_query);
}

grn_rc
grn_expr_dump_plan(grn_ctx *ctx, grn_obj *expr, grn_obj *buffer)
{
  int n;
  scan_info **sis;

  GRN_API_ENTER;
  sis = grn_scan_info_build(ctx, expr, &n, GRN_OP_OR, GRN_FALSE);
  if (sis) {
    int i;
    grn_inspect_scan_info_list(ctx, buffer, sis, n);
    for (i = 0; i < n; i++) {
      SI_FREE(sis[i]);
    }
    GRN_FREE(sis);
  } else {
    GRN_TEXT_PUTS(ctx, buffer, "sequential search\n");
  }
  GRN_API_RETURN(GRN_SUCCESS);
}

static unsigned int
grn_expr_estimate_size_raw(grn_ctx *ctx, grn_obj *expr, grn_obj *table)
{
  return grn_table_size(ctx, table);
}

unsigned int
grn_expr_estimate_size(grn_ctx *ctx, grn_obj *expr)
{
  grn_obj *table;
  grn_obj *variable;
  unsigned int size;

  variable = grn_expr_get_var_by_offset(ctx, expr, 0);
  if (!variable) {
    ERR(GRN_INVALID_ARGUMENT, "at least one variable must be defined");
    return 0;
  }

  table = grn_ctx_at(ctx, variable->header.domain);
  if (!table) {
    ERR(GRN_INVALID_ARGUMENT,
        "variable refers unknown domain: <%u>", variable->header.domain);
    return 0;
  }

  GRN_API_ENTER;
#ifdef GRN_WITH_MRUBY
  if (ctx->impl->mrb.state) {
    size = grn_mrb_expr_estimate_size(ctx, expr, table);
  } else {
    size = grn_expr_estimate_size_raw(ctx, expr, table);
  }
#else
  size = grn_expr_estimate_size_raw(ctx, expr, table);
#endif
  GRN_API_RETURN(size);
}
