/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 *
 * Licensed under the Apache License, Version 2.0 (the ""License"");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an ""AS IS"" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>

#include "mgos.h"
#include "mgos_rpc.h"
#include "mgos_vfs.h"

#if MG_ENABLE_DIRECTORY_LISTING

#if !defined(CS_DEFINE_DIRENT)
#include <dirent.h>
#else
#include "common/cs_dirent.h"
#endif

/* Handler for FS.List */
static void rpc_fs_list_common(const struct mg_str args,
                               struct mg_rpc_request_info *ri, bool ext) {
  struct mbuf fb;
  struct json_out out = JSON_OUT_MBUF(&fb);
  char *path = NULL;
  DIR *dirp;

  mbuf_init(&fb, 50);

  json_scanf(args.p, args.len, ri->args_fmt, &path);

  json_printf(&out, "[");

  if ((dirp = (opendir(path ? path : "/"))) != NULL) {
    struct dirent *dp;
    int i;
    for (i = 0; (dp = readdir(dirp)) != NULL;) {
      /* Do not show current and parent dirs */
      if (strcmp((const char *) dp->d_name, ".") == 0 ||
          strcmp((const char *) dp->d_name, "..") == 0) {
        continue;
      }

      if (i > 0) {
        json_printf(&out, ",");
      }
      if (ext) {
        cs_stat_t st;
        char fname[MG_MAX_PATH + 2];
        snprintf(fname, sizeof(fname), "%s%s%s", (path ? path : "/"),
                 (path ? "/" : ""), dp->d_name);
        if (mg_stat(fname, &st) == 0) {
          json_printf(&out, "{name: %Q, size: %llu}", dp->d_name,
                      (uint64_t) st.st_size);
        } else {
          continue;
        }
      } else {
        json_printf(&out, "%Q", dp->d_name);
      }
      i++;
    }
    closedir(dirp);
  }

  json_printf(&out, "]");

  mg_rpc_send_responsef(ri, "%.*s", fb.len, fb.buf);
  ri = NULL;

  mbuf_free(&fb);
  free(path);
}

static void rpc_fs_list_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                struct mg_rpc_frame_info *fi,
                                struct mg_str args) {
  rpc_fs_list_common(args, ri, false /* ext */);
  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_list_ext_handler(struct mg_rpc_request_info *ri,
                                    void *cb_arg, struct mg_rpc_frame_info *fi,
                                    struct mg_str args) {
  rpc_fs_list_common(args, ri, true /* ext */);
  (void) cb_arg;
  (void) fi;
}
#endif /* MG_ENABLE_DIRECTORY_LISTING */

static void rpc_fs_get_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                               struct mg_rpc_frame_info *fi,
                               struct mg_str args) {
  char *filename = NULL;
  long offset = 0, len = -1;
  long file_size = 0;
  FILE *fp = NULL;
  char *data = NULL;

  json_scanf(args.p, args.len, ri->args_fmt, &filename, &offset, &len);

  /* check arguments */
  if (filename == NULL) {
    mg_rpc_send_errorf(ri, 400, "filename is required");
    ri = NULL;
    goto clean;
  }

  if (offset < 0) {
    mg_rpc_send_errorf(ri, 400, "illegal offset");
    ri = NULL;
    goto clean;
  }

  /* try to open file */
  fp = fopen(filename, "rb");

  if (fp == NULL) {
    mg_rpc_send_errorf(ri, 400, "failed to open file \"%s\"", filename);
    ri = NULL;
    goto clean;
  }

  /* determine file size */
  if (fseek(fp, 0, SEEK_END) != 0) {
    mg_rpc_send_errorf(ri, 500, "fseek");
    ri = NULL;
    goto clean;
  }

  file_size = (long) ftell(fp);
  if (file_size < 0) {
    mg_rpc_send_errorf(ri, 500, "ftell");
    ri = NULL;
    goto clean;
  }

  /* determine the size of the chunk to read */
  if (offset > file_size) {
    offset = file_size;
  }
  if (len < 0 || offset + len > file_size) {
    len = file_size - offset;
  }

  if (len > 0) {
    /* try to allocate the chunk of needed size */
    data = (char *) malloc(len);
    if (data == NULL) {
      mg_rpc_send_errorf(ri, 500, "out of memory");
      ri = NULL;
      goto clean;
    }

    if (offset == 0) {
      LOG(LL_INFO, ("Sending %s", filename));
    }

    /* seek & read the data */
    if (fseek(fp, offset, SEEK_SET)) {
      mg_rpc_send_errorf(ri, 500, "fseek");
      ri = NULL;
      goto clean;
    }

    if ((long) fread(data, 1, len, fp) != len) {
      mg_rpc_send_errorf(ri, 500, "fread");
      ri = NULL;
      goto clean;
    }
  }

  /* send the response */
  mg_rpc_send_responsef(ri, "{data: %V, left: %d}", data, len,
                        (file_size - offset - len));
  ri = NULL;

clean:
  if (filename != NULL) {
    free(filename);
  }

  if (data != NULL) {
    free(data);
  }

  if (fp != NULL) {
    fclose(fp);
  }

  (void) cb_arg;
  (void) fi;
}

struct put_data {
  char *p;
  int len;
};

static void rpc_fs_put_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                               struct mg_rpc_frame_info *fi,
                               struct mg_str args) {
  char *filename = NULL;
  int append = 0;
  FILE *fp = NULL;
  struct put_data data = {NULL, 0};
  long offset = -1;

  json_scanf(args.p, args.len, ri->args_fmt, &filename, &offset, &data.p,
             &data.len, &append);

  /* check arguments */
  if (filename == NULL) {
    mg_rpc_send_errorf(ri, 400, "filename is required");
    ri = NULL;
    goto clean;
  }

  /* try to open file */
  if (offset < 0) {
    fp = fopen(filename, append ? "ab" : "wb");
  } else if (offset == 0) {
    fp = fopen(filename, "wb");
  } else {
    fp = fopen(filename, "r+");
  }

  if (fp == NULL) {
    mg_rpc_send_errorf(ri, 400, "failed to open file \"%s\"", filename);
    ri = NULL;
    goto clean;
  }

  if (!append || offset == 0) {
    LOG(LL_INFO, ("Receiving %s", filename));
  }

  if (offset > 0 && fseek(fp, offset, SEEK_SET) != 0) {
    mg_rpc_send_errorf(ri, 400, "failed to seek to %ld", offset);
    ri = NULL;
    goto clean;
  }

  if (fwrite(data.p, 1, data.len, fp) != (size_t) data.len) {
    mg_rpc_send_errorf(ri, 500, "failed to write data");
    ri = NULL;
    goto clean;
  }

  mg_rpc_send_responsef(ri, NULL);
  ri = NULL;

clean:
  if (filename != NULL) {
    free(filename);
  }

  if (data.p != NULL) {
    free(data.p);
  }

  if (fp != NULL) {
    fclose(fp);
  }

  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_remove_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                  struct mg_rpc_frame_info *fi,
                                  struct mg_str args) {
  int ret = 0;
  char *filename = NULL;

  json_scanf(args.p, args.len, ri->args_fmt, &filename);

  /* check arguments */
  if (filename == NULL) {
    mg_rpc_send_errorf(ri, 400, "filename is required");
    ri = NULL;
    goto clean;
  }

  ret = remove(filename);
  LOG(LL_INFO, ("Remove %s -> %d", filename, ret));
  if (ret != 0) {
    mg_rpc_send_errorf(ri, 500, "remove failed");
    ri = NULL;
    goto clean;
  }

  mg_rpc_send_responsef(ri, NULL);
  ri = NULL;

clean:
  if (filename != NULL) {
    free(filename);
  }

  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_rename_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                  struct mg_rpc_frame_info *fi,
                                  struct mg_str args) {
  char *src = NULL, *dst = NULL;
  json_scanf(args.p, args.len, ri->args_fmt, &src, &dst);
  if (src == NULL || dst == NULL) {
    mg_rpc_send_errorf(ri, 400, "src and dst are required");
  } else if (rename(src, dst) != 0) {
    mg_rpc_send_errorf(ri, 500, "rename failed");
  } else {
    mg_rpc_send_responsef(ri, NULL);
  }
  free(src);
  free(dst);
  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_mkfs_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                struct mg_rpc_frame_info *fi,
                                struct mg_str args) {
  bool res = false;
  char *dev_name = NULL, *dev_type = NULL, *dev_opts = NULL;
  char *fs_type = NULL, *fs_opts = NULL;

  json_scanf(args.p, args.len, ri->args_fmt, &dev_name, &dev_type, &dev_opts,
             &fs_type, &fs_opts);

  if ((dev_type == NULL && dev_name == NULL) || fs_type == NULL) {
    mg_rpc_send_errorf(ri, 400, "fs_type is required");
    ri = NULL;
    goto clean;
  }

  if (dev_name != NULL) {
    res = mgos_vfs_mkfs_dev_name(dev_name, fs_type, (fs_opts ? fs_opts : ""));
  } else {
    res = mgos_vfs_mkfs(dev_type, dev_opts, fs_type, (fs_opts ? fs_opts : ""));
  }
  if (!res) {
    mg_rpc_send_errorf(ri, 500, "mkfs failed");
    ri = NULL;
    goto clean;
  }

  mg_rpc_send_responsef(ri, NULL);
  ri = NULL;

clean:
  free(dev_name);
  free(dev_type);
  free(dev_opts);
  free(fs_type);
  free(fs_opts);
  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_mount_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                 struct mg_rpc_frame_info *fi,
                                 struct mg_str args) {
  bool res = false;
  char *path = NULL;
  char *dev_name = NULL, *dev_type = NULL, *dev_opts = NULL;
  char *fs_type = NULL, *fs_opts = NULL;

  json_scanf(args.p, args.len, ri->args_fmt, &path, &dev_name, &dev_type,
             &dev_opts, &fs_type, &fs_opts);

  if (path == NULL || (dev_type == NULL && dev_name == NULL) ||
      fs_type == NULL) {
    mg_rpc_send_errorf(ri, 400, "path and fs_type is required");
    ri = NULL;
    goto clean;
  }

  if (dev_name != NULL) {
    res = mgos_vfs_mount_dev_name(path, dev_name, fs_type,
                                  (fs_opts ? fs_opts : ""));
  } else {
    res = mgos_vfs_mount(path, dev_type, (dev_opts ? dev_opts : ""), fs_type,
                         (fs_opts ? fs_opts : ""));
  }
  if (!res) {
    mg_rpc_send_errorf(ri, 500, "mount failed");
    ri = NULL;
    goto clean;
  }

  mg_rpc_send_responsef(ri, NULL);
  ri = NULL;

clean:
  free(path);
  free(dev_name);
  free(dev_type);
  free(dev_opts);
  free(fs_type);
  free(fs_opts);
  (void) cb_arg;
  (void) fi;
}

static void rpc_fs_umount_handler(struct mg_rpc_request_info *ri, void *cb_arg,
                                  struct mg_rpc_frame_info *fi,
                                  struct mg_str args) {
  char *path = NULL;

  json_scanf(args.p, args.len, ri->args_fmt, &path);

  if (path == NULL) {
    mg_rpc_send_errorf(ri, 400, "path is required");
    ri = NULL;
    goto clean;
  }

  if (!mgos_vfs_umount(path)) {
    mg_rpc_send_errorf(ri, 500, "umount failed");
    ri = NULL;
    goto clean;
  }

  mg_rpc_send_responsef(ri, NULL);
  ri = NULL;

clean:
  free(path);
  (void) cb_arg;
  (void) fi;
}

bool mgos_rpc_service_fs_init(void) {
  struct mg_rpc *c = mgos_rpc_get_global();
#if MG_ENABLE_DIRECTORY_LISTING
  mg_rpc_add_handler(c, "FS.List", "{path: %Q}", rpc_fs_list_handler, NULL);
  mg_rpc_add_handler(c, "FS.ListExt", "{path: %Q}", rpc_fs_list_ext_handler,
                     NULL);
#endif
  mg_rpc_add_handler(c, "FS.Get", "{filename: %Q, offset: %ld, len: %ld}",
                     rpc_fs_get_handler, NULL);
  mg_rpc_add_handler(c, "FS.Put",
                     "{filename: %Q, offset: %ld, data: %V, append: %B}",
                     rpc_fs_put_handler, NULL);
  mg_rpc_add_handler(c, "FS.Remove", "{filename: %Q}", rpc_fs_remove_handler,
                     NULL);
  mg_rpc_add_handler(c, "FS.Rename", "{src: %Q, dst: %Q}",
                     rpc_fs_rename_handler, NULL);
  mg_rpc_add_handler(
      c, "FS.Mkfs",
      /* dev_name OR type+opts */
      "{dev_name: %Q, dev_type: %Q, dev_opts: %Q, fs_type: %Q, fs_opts: %Q}",
      rpc_fs_mkfs_handler, NULL);
  mg_rpc_add_handler(c, "FS.Mount",
                     /* dev_name OR type+opts */
                     "{path: %Q, dev_name: %Q, dev_type: %Q, dev_opts: %Q, "
                     "fs_type: %Q, fs_opts: %Q}",
                     rpc_fs_mount_handler, NULL);
  mg_rpc_add_handler(c, "FS.Umount", "{path: %Q}", rpc_fs_umount_handler, NULL);
  return true;
}
