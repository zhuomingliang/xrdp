/*
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

   xrdp: A Remote Desktop Protocol server.
   Copyright (C) Jay Sorg 2004-2008

   listen for incoming connection

*/

#include "xrdp.h"

/* 'g_process' is protected by the semaphore 'g_process_sem'.  One thread sets
   g_process and waits for the other to process it */
static tbus g_process_sem = 0;
static struct xrdp_process* g_process = 0;

/*****************************************************************************/
struct xrdp_listen* APP_CC
xrdp_listen_create(void)
{
  struct xrdp_listen* self;

  self = (struct xrdp_listen*)g_malloc(sizeof(struct xrdp_listen), 1);
  g_process_sem = tc_sem_create(0);
  self->pro_done_event = g_create_wait_obj("xrdp_listen_pro_done_event");
  self->process_list = list_create();
  return self;
}

/*****************************************************************************/
void APP_CC
xrdp_listen_delete(struct xrdp_listen* self)
{
  tc_sem_delete(g_process_sem);
  g_destroy_wait_obj(self->pro_done_event);
  list_delete(self->process_list);
  g_free(self);
}

/*****************************************************************************/
static int APP_CC
xrdp_listen_term_processes(struct xrdp_listen* self)
{
  int i;
  struct xrdp_process* pro;

  /* tell all xrdp processes to end */
  for (i = self->process_list->count - 1; i >= 0; i--)
  {
    pro = (struct xrdp_process*)list_get_item(self->process_list, i);
    if (pro != 0)
    {
      pro->term = 1;
    }
  }
  /* make sure they are done */
  for (i = self->process_list->count - 1; i >= 0; i--)
  {
    pro = (struct xrdp_process*)list_get_item(self->process_list, i);
    if (pro != 0)
    {
      while (pro->status > 0)
      {
        g_sleep(10);
      }
    }
  }
  /* free them all */
  for (i = self->process_list->count - 1; i >= 0; i--)
  {
    pro = (struct xrdp_process*)list_get_item(self->process_list, i);
    if (pro != 0)
    {
      xrdp_process_delete(pro);
      list_remove_item(self->process_list, i);
    }
  }
  return 0;
}

/*****************************************************************************/
/* returns error */
static int APP_CC
xrdp_listen_add_pro(struct xrdp_listen* self, struct xrdp_process* process)
{
  list_add_item(self->process_list, (tbus)process);
  return 0;
}

/*****************************************************************************/
static int APP_CC
xrdp_listen_delete_done_pro(struct xrdp_listen* self)
{
  int i;
  struct xrdp_process* pro;

  for (i = self->process_list->count - 1; i >= 0; i--)
  {
    pro = (struct xrdp_process*)list_get_item(self->process_list, i);
    if (pro != 0)
    {
      if (pro->status < 0)
      {
        xrdp_process_delete(pro);
        list_remove_item(self->process_list, i);
      }
    }
  }
  return 0;
}

/*****************************************************************************/
/* i can't get stupid in_val to work, hum using global var for now */
THREAD_RV THREAD_CC
xrdp_process_run(void* in_val)
{
  struct xrdp_process* process;

  DEBUG(("process started"));
  process = g_process;
  g_process = 0;
  tc_sem_inc(g_process_sem);
  xrdp_process_main_loop(process);
  DEBUG(("process done"));
  return 0;
}

/*****************************************************************************/
static int
xrdp_listen_get_port(char* port, int port_bytes)
{
  int fd;
  int error;
  int index;
  char* val;
  struct list* names;
  struct list* values;

  /* default to port 3389 */
  g_strncpy(port, "3389", port_bytes - 1);
  /* see if port is in xrdp.ini file */
  fd = g_file_open(XRDP_CFG_FILE);
  if (fd > 0)
  {
    names = list_create();
    names->auto_free = 1;
    values = list_create();
    values->auto_free = 1;
    if (file_read_section(fd, "globals", names, values) == 0)
    {
      for (index = 0; index < names->count; index++)
      {
        val = (char*)list_get_item(names, index);
        if (val != 0)
        {
          if (g_strcasecmp(val, "port") == 0)
          {
            val = (char*)list_get_item(values, index);
            error = g_atoi(val);
            if ((error > 0) && (error < 65000))
            {
              g_strncpy(port, val, port_bytes - 1);
            }
            break;
          }
        }
      }
    }
    list_delete(names);
    list_delete(values);
    g_file_close(fd);
  }
  return 0;
}

/*****************************************************************************/
/* wait for incoming connections */
int APP_CC
xrdp_listen_main_loop(struct xrdp_listen* self)
{
  int error;
  int robjs_count;
  int cont;
  char port[8];
  tbus robjs[4];

  self->status = 1;
  robjs_count = 0;
  xrdp_listen_get_port(port, sizeof(port));
  self->sck = g_tcp_socket();
  g_tcp_set_non_blocking(self->sck);
  error = g_tcp_bind(self->sck, port);
  if (error != 0)
  {
    g_writeln("bind error in xrdp_listen_main_loop");
    g_tcp_close(self->sck);
    self->status = -1;
    return 1;
  }
  error = g_tcp_listen(self->sck);
  if (error == 0)
  {
    robjs[0] = g_get_term_event();
    robjs[1] = g_get_sync_event();
    robjs[2] = g_create_wait_obj_from_socket(self->sck, 0);
    robjs[3] = self->pro_done_event;
    robjs_count = 4;
    cont = 1;
    while (cont)
    {
      if (g_obj_wait(robjs, robjs_count, 0, 0, -1) != 0)
      {
        g_sleep(100);
      }
      if (g_is_wait_obj_set(robjs[0])) /* term */
      {
        break;
      }
      if (g_is_wait_obj_set(robjs[1])) /* sync */
      {
        g_reset_wait_obj(robjs[1]);
        g_loop();
      }
      if (g_is_wait_obj_set(robjs[2])) /* incomming connection */
      {
        error = g_tcp_accept(self->sck);
        if ((error == -1) && g_tcp_last_error_would_block(self->sck))
        {
          g_sleep(100);
        }
        else if (error == -1)
        {
          break;
        }
        else
        {
          g_process = xrdp_process_create(self, self->pro_done_event);
          if (xrdp_listen_add_pro(self, g_process) == 0)
          {
            /* start thread */
            g_process->sck = error;
            tc_thread_create(xrdp_process_run, 0);
            tc_sem_dec(g_process_sem); /* this will wait */
          }
          else
          {
            xrdp_process_delete(g_process);
          }
        }
      }
      if (g_is_wait_obj_set(robjs[3])) /* pro_done_event */
      {
        g_reset_wait_obj(robjs[3]);
        xrdp_listen_delete_done_pro(self);
      }
    }
  }
  else
  {
    DEBUG(("listen error in xrdp_listen_main_loop"));
  }
  xrdp_listen_term_processes(self);
  g_tcp_close(self->sck);
  self->status = -1;
  return 0;
}
