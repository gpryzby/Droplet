/*
 * Copyright (C) 2010 SCALITY SA. All rights reserved.
 * http://www.scality.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SCALITY SA ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SCALITY SA OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of SCALITY SA.
 *
 * https://github.com/scality/Droplet
 */
#include "dropletp.h"
#include <droplet/cdmi/reqbuilder.h>

//#define DPRINTF(fmt,...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DPRINTF(fmt,...)

static dpl_status_t
add_array_to_json_array(dpl_dict_t *dict,
                        json_object *array)

{
  int bucket;
  dpl_var_t *var;
  int ret;
  json_object *tmp;
  json_object *tmp2;

  tmp = json_object_new_object();
  if (NULL == tmp)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  for (bucket = 0;bucket < dict->n_buckets;bucket++)
    {
      for (var = dict->buckets[bucket];var;var = var->prev)
        {
          switch (var->type)
            {
            case DPL_VAR_STRING:

              tmp2 = json_object_new_string(var->value);
              if (NULL == tmp)
                {
                  ret = DPL_ENOMEM;
                  goto end;
                }
              
              json_object_object_add(tmp, var->key, tmp2);
              //XXX check return value

              break ;

            case DPL_VAR_ARRAY:
              //XXX do nothing
              break ;
            }
        }
    }

  json_object_array_add(array, tmp);
  //XXX check return value

  ret = DPL_SUCCESS;

 end:

  return ret;
}

static dpl_status_t
add_metadata_to_json_body(dpl_dict_t *metadata,
                          json_object *body_obj)

{
  int bucket;
  dpl_var_t *var;
  int ret;
  json_object *md_obj = NULL;
  json_object *tmp;

  if (0 == dpl_dict_count(metadata))
    {
      ret = DPL_SUCCESS;
      goto end;
    }

  md_obj = json_object_new_object();
  if (NULL == md_obj)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  for (bucket = 0;bucket < metadata->n_buckets;bucket++)
    {
      for (var = metadata->buckets[bucket];var;var = var->prev)
        {
          switch (var->type)
            {
            case DPL_VAR_STRING:

              tmp = json_object_new_string(var->value);
              if (NULL == tmp)
                {
                  ret = DPL_ENOMEM;
                  goto end;
                }
              
              break ;

            case DPL_VAR_ARRAY:

              tmp = json_object_new_array();
              if (NULL == tmp)
                {
                  ret = DPL_ENOMEM;
                  goto end;
                }

              add_array_to_json_array(var->array, tmp);
              //XXX check return value
              
              break ;
            }

          json_object_object_add(md_obj, var->key, tmp);
          //XXX check return value
        }
    }

  json_object_object_add(body_obj, "metadata", md_obj);
  //XXX check return value
  md_obj = NULL;

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != md_obj)
    json_object_put(md_obj);

  return ret;
}

static dpl_status_t
add_metadata_to_headers(dpl_dict_t *metadata,
                        dpl_dict_t *headers,
                        dpl_ftype_t object_type)

{
  int bucket;
  dpl_var_t *var;
  char header[1024];
  int ret;

  for (bucket = 0;bucket < metadata->n_buckets;bucket++)
    {
      for (var = metadata->buckets[bucket];var;var = var->prev)
        {
          switch (object_type)
            {
            case DPL_FTYPE_DIR:
              snprintf(header, sizeof (header), "X-Container-Meta-%s", var->key);
              break ;
            default:
              snprintf(header, sizeof (header), "X-Object-Meta-%s", var->key);
              break ;
            }

          ret = dpl_dict_add(headers, header, var->value, 0);
          if (DPL_SUCCESS != ret)
            {
              return DPL_FAILURE;
            }
        }
    }

  return DPL_SUCCESS;
}

static dpl_status_t
add_copy_directive_to_json_body(const dpl_req_t *req,
                                json_object *body_obj)

{
  dpl_ctx_t *ctx = (dpl_ctx_t *) req->ctx;
  int ret;
  json_object *tmp = NULL;
  const char *field = NULL;
  char *buf;
  char *src_resource;

  if (DPL_COPY_DIRECTIVE_UNDEF == req->copy_directive)
    {
      ret = DPL_SUCCESS;
      goto end;
    }

  if (NULL == req->src_resource)
    {
      ret = DPL_EINVAL;
      goto end;
    }    

  switch (req->copy_directive)
    {
    case DPL_COPY_DIRECTIVE_COPY:
      field = "copy";
      break ;
    case DPL_COPY_DIRECTIVE_METADATA_REPLACE:
      ret = DPL_EINVAL;
      goto end;
    case DPL_COPY_DIRECTIVE_HARDLINK:
      field = "link";
      goto end;
    case DPL_COPY_DIRECTIVE_SYMLINK:
      field = "reference";
      break ;
    case DPL_COPY_DIRECTIVE_MOVE:
      field = "move";
      break ;
    default:
      ret = DPL_ENOTSUPP;
      goto end;
    }

  if (ctx->base_path_in_refs)
    {
      int base_path_len = 0;
      int delim_len = 0;
      int src_resource_len = 0;

      if (NULL != ctx->base_path)
        base_path_len = strlen(ctx->base_path);
      
      delim_len = strlen(ctx->delim);

      src_resource_len = strlen(req->src_resource);

      buf = alloca(base_path_len + delim_len + src_resource_len + 1);
      if (NULL == buf)
        {
          ret = DPL_ENOMEM;
          goto end;
        }
      
      buf[0] = 0;

      if (NULL != ctx->base_path)
        strcat(buf, ctx->base_path);

      strcat(buf, ctx->delim);

      strcat(buf, req->src_resource);

      src_resource = buf;
    }
  else
    {
      src_resource = req->src_resource;
    }

  tmp = json_object_new_string(src_resource);
  if (NULL == tmp)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  assert(NULL != field);
  json_object_object_add(body_obj, field, tmp);
  //XXX check return value
  tmp = NULL;

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != tmp)
    json_object_put(tmp);

  return ret;
}

static dpl_status_t
add_data_to_json_body(dpl_chunk_t *chunk,
                      json_object *body_obj)

{
  int ret;
  json_object *value_obj = NULL;
  json_object *valuetransferencoding_obj = NULL;
  char *base64_str;
  int base64_len;

  //encode body to base64
  base64_str = alloca(DPL_BASE64_LENGTH(chunk->len) + 1);
  base64_len = dpl_base64_encode((const u_char *) chunk->buf, chunk->len, (u_char *) base64_str);
  base64_str[base64_len] = 0;

  value_obj = json_object_new_string(base64_str);
  if (NULL == value_obj)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  json_object_object_add(body_obj, "value", value_obj);
  //XXX check return value
  value_obj = NULL;

  /**/

  valuetransferencoding_obj = json_object_new_string("base64");
  if (NULL == valuetransferencoding_obj)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  json_object_object_add(body_obj, "valuetransferencoding", valuetransferencoding_obj);
  //XXX check return valuetransferencoding
  valuetransferencoding_obj = NULL;

  ret = DPL_SUCCESS;
  
 end:

  if (NULL != valuetransferencoding_obj)
    json_object_put(valuetransferencoding_obj);

  if (NULL != value_obj)
    json_object_put(value_obj);

  return ret;
}

static dpl_status_t
add_authorization_to_headers(const dpl_req_t *req,
                             dpl_dict_t *headers)
{
  int ret, ret2;
  char basic_str[1024];
  int basic_len;
  char base64_str[1024];
  int base64_len;
  char auth_str[1024];

  snprintf(basic_str, sizeof (basic_str), "%s:%s", req->ctx->access_key, req->ctx->secret_key);
  basic_len = strlen(basic_str);

  base64_len = dpl_base64_encode((const u_char *) basic_str, basic_len, (u_char *) base64_str);

  snprintf(auth_str, sizeof (auth_str), "Basic %.*s", base64_len, base64_str);

  ret2 = dpl_dict_add(headers, "Authorization", auth_str, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret = DPL_SUCCESS;

 end:

  return ret;
}

/**
 * build headers from request
 *
 * @param req
 * @param headersp
 *
 * @return
 */
dpl_status_t
dpl_cdmi_req_build(const dpl_req_t *req,
                   dpl_dict_t **headersp,
                   char **body_strp,
                   int *body_lenp)
{
  dpl_dict_t *headers = NULL;
  int ret, ret2;
  char *method = dpl_method_str(req->method);
  json_object *body_obj = NULL;
  char *body_str = NULL;
  int body_len = 0;
  char buf[256];

  DPL_TRACE(req->ctx, DPL_TRACE_REQ, "req_build method=%s bucket=%s resource=%s subresource=%s", method, req->bucket, req->resource, req->subresource);

  headers = dpl_dict_new(13);
  if (NULL == headers)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  /*
   * per method headers
   */
  if (DPL_METHOD_GET == req->method)
    {
      //XXX ranges, conditions

      switch (req->object_type)
        {
        case DPL_FTYPE_UNDEF:
          //do nothing
          break ;
        case DPL_FTYPE_ANY:
          ret2 = dpl_dict_add(headers, "Accept", DPL_CDMI_CONTENT_TYPE_ANY, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        case DPL_FTYPE_REG:
          ret2 = dpl_dict_add(headers, "Accept", DPL_CDMI_CONTENT_TYPE_OBJECT, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        case DPL_FTYPE_DIR:
          ret2 = dpl_dict_add(headers, "Accept", DPL_CDMI_CONTENT_TYPE_CONTAINER, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        case DPL_FTYPE_CAP:
          ret2 = dpl_dict_add(headers, "Accept", DPL_CDMI_CONTENT_TYPE_CAPABILITY, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        }
    }
  else if (DPL_METHOD_PUT == req->method ||
           DPL_METHOD_POST == req->method)
    {
      body_obj = json_object_new_object();
      if (NULL == body_obj)
        {
          ret = DPL_ENOMEM;
          goto end;
        }

      if (NULL != req->cache_control)
        {
          ret2 = dpl_dict_add(headers, "Cache-Control", req->cache_control, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
        }

      if (NULL != req->content_disposition)
        {
          ret2 = dpl_dict_add(headers, "Content-Disposition", req->content_disposition, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
        }

      if (NULL != req->content_encoding)
        {
          ret2 = dpl_dict_add(headers, "Content-Encoding", req->content_encoding, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
        }

      if (!(req->behavior_flags & DPL_BEHAVIOR_HTTP_COMPAT))
        {
          ret2 = add_metadata_to_json_body(req->metadata, body_obj);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }

          ret2 = add_copy_directive_to_json_body(req, body_obj);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          
          if (NULL != req->chunk)
            {
              ret2 = add_data_to_json_body(req->chunk, body_obj);
              if (DPL_SUCCESS != ret2)
                {
                  ret = ret2;
                  goto end;
                }
            }
          
          pthread_mutex_lock(&req->ctx->lock);
          body_str = (char *) json_object_to_json_string(body_obj);
          pthread_mutex_unlock(&req->ctx->lock);
          if (NULL == body_str)
            {
              ret = DPL_ENOMEM;
              goto end;
            }

          body_len = strlen(body_str);
          
          snprintf(buf, sizeof (buf), "%u", body_len);
          ret2 = dpl_dict_add(headers, "Content-Length", buf, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
        }
      else
        {
          ret2 = add_metadata_to_headers(req->metadata, headers, req->object_type);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }

          snprintf(buf, sizeof (buf), "%u", req->chunk->len);
          ret2 = dpl_dict_add(headers, "Content-Length", buf, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
        }

      if (req->behavior_flags & DPL_BEHAVIOR_EXPECT)
        {
          ret2 = dpl_dict_add(headers, "Expect", "100-continue", 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
        }

      switch (req->object_type)
        {
        case DPL_FTYPE_UNDEF:
          //do nothing
          break ;
        case DPL_FTYPE_ANY:
          //error ?
          break ;
        case DPL_FTYPE_REG:
          ret2 = dpl_dict_add(headers, "Content-Type", DPL_CDMI_CONTENT_TYPE_OBJECT, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        case DPL_FTYPE_DIR:
          ret2 = dpl_dict_add(headers, "Content-Type", DPL_CDMI_CONTENT_TYPE_CONTAINER, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        case DPL_FTYPE_CAP:
          ret2 = dpl_dict_add(headers, "Content-Type", DPL_CDMI_CONTENT_TYPE_CAPABILITY, 0);
          if (DPL_SUCCESS != ret2)
            {
              ret = ret2;
              goto end;
            }
          break ;
        }
    }
  else if (DPL_METHOD_DELETE == req->method)
    {
    }
  else
    {
      ret = DPL_EINVAL;
      goto end;
    }

  /*
   * common headers
   */
  if (!(req->behavior_flags & DPL_BEHAVIOR_HTTP_COMPAT))
    {
      ret2 = dpl_dict_add(headers, "X-CDMI-Specification-Version", "1.0.1", 0);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  if (req->behavior_flags & DPL_BEHAVIOR_VIRTUAL_HOSTING)
    {
      char host[1024];

      snprintf(host, sizeof (host), "%s.%s", req->bucket, req->ctx->host);

      ret2 = dpl_dict_add(headers, "Host", host, 0);
      if (DPL_SUCCESS != ret2)
        {
          ret = DPL_ENOMEM;
          goto end;
        }
    }
  else
    {
      ret2 = dpl_dict_add(headers, "Host", req->ctx->host, 0);
      if (DPL_SUCCESS != ret2)
        {
          ret = DPL_ENOMEM;
          goto end;
        }
    }

  if (req->behavior_flags & DPL_BEHAVIOR_KEEP_ALIVE)
    {
      ret2 = dpl_dict_add(headers, "Connection", "keep-alive", 0);
      if (DPL_SUCCESS != ret2)
        {
          ret = DPL_ENOMEM;
          goto end;
        }
    }

  ret2 = add_authorization_to_headers(req, headers);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (NULL != body_strp)
    {
      if (NULL == body_str)
        {
          *body_strp = NULL;
        }
      else
        {
          *body_strp = strdup(body_str);
          if (NULL == body_strp)
            {
              ret = DPL_ENOMEM;
              goto end;
            }
        }
    }

  if (NULL != body_lenp)
    *body_lenp = body_len;

  if (NULL != headersp)
    {
      *headersp = headers;
      headers = NULL; //consume it
    }

  ret = DPL_SUCCESS;

 end:

  if (NULL != body_obj)
    json_object_put(body_obj);

  if (NULL != headers)
    dpl_dict_free(headers);

  return ret;
}

dpl_status_t
dpl_cdmi_req_gen_url(const dpl_req_t *req,
                   dpl_dict_t *headers,
                   char *buf,
                   int len,
                   unsigned int *lenp)
{
  //XXX no spec in CDMI

  return DPL_ENOTSUPP;
}
