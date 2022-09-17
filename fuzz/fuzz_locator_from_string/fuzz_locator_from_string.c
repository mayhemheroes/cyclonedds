/*
 * Copyright(c) 2021 to 2022 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

 #include <stdio.h>
 #include <stdint.h>
 #include <string.h>
 #include <dds/dds.h>

 #include "dds/ddsrt/heap.h"
 #include "dds/ddsrt/sockets.h"
 #include "dds/ddsi/ddsi_tran.h"
 #include "dds/ddsi/ddsi_domaingv.h"
 #include "dds/ddsi/ddsi_udp.h"
 #include "dds/ddsi/ddsi_tcp.h"
 #include "dds/ddsi/ddsi_config_impl.h"
 #include "dds/ddsi/q_rtps.h"

 static struct ddsi_tran_factory *init(struct ddsi_domaingv *gv, enum ddsi_transport_selector tr)
 {
   memset(gv, 0, sizeof (*gv));
   gv->config.transport_selector = tr;
   ddsi_udp_init(gv);
   ddsi_tcp_init(gv);
   switch(tr)
   {
     case DDSI_TRANS_UDP: return ddsi_factory_find(gv, "udp");
     case DDSI_TRANS_TCP: return ddsi_factory_find(gv, "tcp");
     case DDSI_TRANS_UDP6: return ddsi_factory_find(gv, "udp6");
     case DDSI_TRANS_TCP6: return ddsi_factory_find(gv, "tcp6");
     default: return NULL;
   }
 }

 static void fini(struct ddsi_domaingv *gv)
 {
   while(gv->ddsi_tran_factories)
   {
     struct ddsi_tran_factory *f = gv->ddsi_tran_factories;
     gv->ddsi_tran_factories = f->m_factory;
     ddsi_factory_free(f);
   }
 }


int LLVMFuzzerTestOneInput(const char *data, size_t size)
{
  struct ddsi_domaingv gv;
  enum ddsi_transport_selector tr;
  // strchr crashes without a null terminator
  if(size <= 0 || data[size - 1] != '\0')
  {
    return 0;
  }

  switch(data[0] % 4)
  {
    case 0: tr = DDSI_TRANS_UDP;
    case 1: tr = DDSI_TRANS_TCP;
    case 2: tr = DDSI_TRANS_UDP6;
    case 3: tr = DDSI_TRANS_TCP6;
  }
  struct ddsi_tran_factory * const fact = init(&gv, tr);
  ddsi_locator_t loc;
  assert(fact);
  ddsi_locator_from_string(&gv, &loc, data, fact);
  fini(&gv);
  return 0;
}
