/*
  (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"


#define VECTORSIZE(count) (count * (sizeof (struct iovec)))

static void
iov_free (struct iovec *vector,
	  int32_t count)
{
  int i;

  for (i=0; i<count; i++)
    free (vector[i].iov_base);

  free (vector);
}

static int32_t
iov_length (struct iovec *vector,
	    int32_t count)
{
  int32_t i;
  size_t size = 0;

  for (i=0; i<count; i++)
    size += vector[i].iov_len;

  return size;
}

struct wb_conf;
struct wb_page;
struct wb_file;

struct wb_conf {
  size_t aggregate_size;
};

struct wb_page {
  struct wb_page *next;
  struct wb_page *prev;
  struct wb_file *file;
  off_t offset;
  struct iovec *vector;
  int32_t count;
};

struct wb_file {
  off_t offset;
  size_t size;
  int32_t refcount;
  int32_t op_ret;
  int32_t op_errno;
  struct wb_page pages;
  dict_t *file_ctx;
};

typedef struct wb_conf wb_conf_t;
typedef struct wb_page wb_page_t;
typedef struct wb_file wb_file_t;

static struct iovec *
vectordup (struct iovec *vector,
	   int32_t count)
{
  int32_t bytecount = (count * sizeof (struct iovec));
  int32_t i;
  struct iovec *newvec = malloc (bytecount);

  for (i=0;i<count;i++) {
    newvec[i].iov_len = vector[i].iov_len;
    newvec[i].iov_base = malloc (newvec[i].iov_len);
    memcpy (newvec[i].iov_base,
	    vector[i].iov_base,
	    newvec[i].iov_len);
  }

  return newvec;
}

static wb_file_t *
wb_file_ref (wb_file_t *file)
{
  file->refcount++;
  return file;
}

static void
wb_file_unref (wb_file_t *file)
{
  file->refcount--;

  if (!file->refcount) {
    wb_page_t *page = file->pages.next;

    while (page != &file->pages) {
      wb_page_t *next = page->next;

      page->prev->next = page->next;
      page->next->prev = page->prev;

      if (page->vector)
	free (page->vector);
      free (page);

      page = next;
    }
    free (file);
  }
}

static int32_t
wb_sync_cbk (call_frame_t *frame,
	     call_frame_t *prev_frame,
	     xlator_t *this,
	     int32_t op_ret,
	     int32_t op_errno)
{
  wb_file_t *file = frame->local;

  if (op_ret == -1) {
    file->op_ret = op_ret;
    file->op_errno = op_errno;
  }

  frame->local = NULL;

  wb_file_unref (file);

  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t
wb_sync (call_frame_t *frame,
	 wb_file_t *file)
{
  size_t total_count = 0;
  size_t copied = 0;
  wb_page_t *page = file->pages.next;
  struct iovec *vector;
  call_frame_t *wb_frame;
  off_t offset;

  while (page != &file->pages) {
    total_count += page->count;
    page = page->next;
  }

  if (!total_count)
    return 0;

  vector = malloc (VECTORSIZE (total_count));

  page = file->pages.next;
  offset = file->pages.next->offset;

  while (page != &file->pages) {
    wb_page_t *next = page->next;
    size_t bytecount = VECTORSIZE (page->count);

    memcpy (((char *)vector)+copied,
	    page->vector,
	    bytecount);
    copied += bytecount;

    page->prev->next = page->next;
    page->next->prev = page->prev;

    free (page->vector);
    free (page);

    page = next;
  }

  wb_frame = copy_frame (frame);
  wb_frame->local = wb_file_ref (file);

  STACK_WIND (wb_frame,
	      wb_sync_cbk,
	      wb_frame->this->first_child,
	      wb_frame->this->first_child->fops->writev,
	      file->file_ctx,
	      vector,
	      total_count,
	      offset);

  file->offset = 0;
  file->size = 0;

  iov_free (vector, total_count);
  return 0;
}

static int32_t
wb_open_cbk (call_frame_t *frame,
	     call_frame_t *prev_frame,
	     xlator_t *this,
	     int32_t op_ret,
	     int32_t op_errno,
	     dict_t *file_ctx,
	     struct stat *buf)
{
  if (op_ret != -1) {
    wb_file_t *file = calloc (1, sizeof (*file));

    file->pages.next = &file->pages;
    file->pages.prev = &file->pages;
    file->file_ctx = file_ctx;

    dict_set (file_ctx,
	      this->name,
	      int_to_data ((long) ((void *) file)));
    wb_file_ref (file);
  }
  STACK_UNWIND (frame, op_ret, op_errno, file_ctx, buf);
  return 0;
}

static int32_t
wb_open (call_frame_t *frame,
	 xlator_t *this,
	 const char *path,
	 int32_t flags,
	 mode_t mode)
{
  STACK_WIND (frame,
	      wb_open_cbk,
	      this->first_child,
	      this->first_child->fops->open,
	      path,
	      flags,
	      mode);
  return 0;
}

static int32_t
wb_create (call_frame_t *frame,
	   xlator_t *this,
	   const char *path,
	   mode_t mode)
{
  STACK_WIND (frame,
	      wb_open_cbk,
	      this->first_child,
	      this->first_child->fops->create,
	      path,
	      mode);
  return 0;
}

static int32_t 
wb_writev (call_frame_t *frame,
	   xlator_t *this,
	   dict_t *file_ctx,
	   struct iovec *vector,
	   int32_t count,
	   off_t offset)
{
  wb_file_t *file;
  wb_conf_t *conf = this->private;
  call_frame_t *wb_frame;

  file = (void *) ((long) data_to_int (dict_get (file_ctx,
						 this->name)));

  if (offset != file->offset)
    /* detect lseek() */
    wb_sync (frame, file);

  if (file->op_ret == -1) {
    /* delayed error delivery */
    STACK_UNWIND (frame, -1, file->op_errno);
    file->op_ret = 0;
    return 0;
  }

  wb_frame = copy_frame (frame);
  STACK_UNWIND (frame, iov_length (vector, count), 0); /* liar! liar! :O */
  file->offset = (offset + iov_length (vector, count));

  {
    wb_page_t *page = calloc (1, sizeof (*page));

    page->vector = vectordup (vector, count);
    page->count = count;
    page->offset = offset;

    page->next = &file->pages;
    page->prev = file->pages.prev;
    page->next->prev = page;
    page->prev->next = page;

    file->size += iov_length (vector, count);
  }

  if (file->size >= conf->aggregate_size)
    wb_sync (wb_frame, file);

  STACK_DESTROY (wb_frame->root);
  return 0;
}


static int32_t
wb_read_cbk (call_frame_t *frame,
	     call_frame_t *prev_frame,
	     xlator_t *this,
	     int32_t op_ret,
	     int32_t op_errno,
	     char *buf)
{
  STACK_UNWIND (frame, op_ret, op_errno, buf);
  return 0;
}

static int32_t
wb_read (call_frame_t *frame,
	 xlator_t *this,
	 dict_t *file_ctx,
	 size_t size,
	 off_t offset)
{
  wb_file_t *file;

  file = (void *) ((long) data_to_int (dict_get (file_ctx,
						 this->name)));

  wb_sync (frame, file);

  STACK_WIND (frame,
	      wb_read_cbk,
	      this->first_child,
	      this->first_child->fops->read,
	      file_ctx,
	      size,
	      offset);

  return 0;
}

static int32_t
wb_ffr_cbk (call_frame_t *frame,
	    call_frame_t *prev_frame,
	    xlator_t *this,
	    int32_t op_ret,
	    int32_t op_errno)
{
  wb_file_t *file = frame->local;

  if (file->op_ret == -1) {
    op_ret = file->op_ret;
    op_errno = file->op_errno;

    file->op_ret = 0;
  }

  frame->local = NULL;
  wb_file_unref (file);
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t
wb_flush (call_frame_t *frame,
	  xlator_t *this,
	  dict_t *file_ctx)
{
  wb_file_t *file;

  file = (void *) ((long) data_to_int (dict_get (file_ctx,
						 this->name)));
  wb_sync (frame, file);

  frame->local = wb_file_ref (file);

  STACK_WIND (frame,
	      wb_ffr_cbk,
	      this->first_child,
	      this->first_child->fops->flush,
	      file_ctx);
  return 0;
}

static int32_t
wb_fsync (call_frame_t *frame,
	  xlator_t *this,
	  dict_t *file_ctx,
	  int32_t datasync)
{
  wb_file_t *file;

  file = (void *) ((long) data_to_int (dict_get (file_ctx,
						 this->name)));
  wb_sync (frame, file);

  frame->local = wb_file_ref (file);

  STACK_WIND (frame,
	      wb_ffr_cbk,
	      this->first_child,
	      this->first_child->fops->fsync,
	      file_ctx,
	      datasync);
  return 0;
}

static int32_t
wb_release (call_frame_t *frame,
	    xlator_t *this,
	    dict_t *file_ctx)
{
  wb_file_t *file;

  file = (void *) ((long) data_to_int (dict_get (file_ctx,
						 this->name)));
  wb_sync (frame, file);

  frame->local = wb_file_ref (file);

  dict_del (file_ctx, this->name);
  wb_file_unref (file);


  STACK_WIND (frame,
	      wb_ffr_cbk,
	      this->first_child,
	      this->first_child->fops->release,
	      file_ctx);
  return 0;
}

int32_t 
init (struct xlator *this)
{
  dict_t *options = this->options;
  wb_conf_t *conf;

  if (!this->first_child || this->first_child->next_sibling) {
    gf_log ("write-back",
	    GF_LOG_ERROR,
	    "FATAL: write-back (%s) not configured with exactly one child",
	    this->name);
    return -1;
  }

  conf = calloc (1, sizeof (*conf));

  conf->aggregate_size = 1048576;

  if (dict_get (options, "aggregate-size")) {
    conf->aggregate_size = data_to_int (dict_get (options,
						  "aggregate-size"));
  }
  gf_log ("write-back",
	  GF_LOG_DEBUG,
	  "using aggregate-size = %d", conf->aggregate_size);

  this->private = conf;
  return 0;
}

void
fini (struct xlator *this)
{
  return;
}

struct xlator_fops fops = {
  .writev      = wb_writev,
  .open        = wb_open,
  .create      = wb_create,
  .read        = wb_read,
  .flush       = wb_flush,
  .fsync       = wb_fsync,
  .release     = wb_release
};

struct xlator_mops mops = {
};
