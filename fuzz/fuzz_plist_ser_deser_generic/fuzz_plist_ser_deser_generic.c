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
 #include "dds/ddsrt/endian.h"
 #include "dds/ddsi/ddsi_xqos.h"
 #include "dds/ddsi/ddsi_plist_generic.h"

 struct desc {
   const enum pserop desc[20];
   const void *data;
   size_t exp_sersize;
   const unsigned char *exp_ser;

   /* XbPROP means expectation after deser may be different from input, if exp_data
      is NULL, use "data", else use "exp_data" */
   const void *exp_data;
 };

 typedef unsigned char raw[];
 typedef uint32_t raw32[];
 typedef uint64_t raw64[];
 typedef ddsi_octetseq_t oseq;

int LLVMFuzzerTestOneInput(const char *data, size_t size)
{

  union {
    uint64_t u;
    void *p;
    char buf[256];
  } mem;

  /* Example descs
   * { {XSTOP}, (raw){0}, 0, (raw){0} },
   * { {XO,XSTOP}, &(oseq){0, NULL },       4, (raw){SER32(0)} },
   * { {XO,XSTOP}, &(oseq){1, (raw){3} },   5, (raw){SER32(1), 3} },
   * { {XS,XSTOP}, &(char *[]){""},         5, (raw){SER32(1), 0} },
   * { {XS,XSTOP}, &(char *[]){"meow"},     9, (raw){SER32(5), 'm','e','o','w',0} },
   *
   */

  struct desc fdesc = { .desc = {XSTOP}, .data = (raw){data} };
  // struct desc fdesc;
  // switch(data[0] % 3)
  // {
  //   case 0: fdesc = ((struct desc){{XSTOP}, (raw){data}});
  //   case 1: fdesc = ((struct desc){{XO,XSTOP}, &(oseq){data}});
  //   case 2: fdesc = ((struct desc){{XS,XSTOP}, &(char *[]){data}});
  // }

  void *ser;
  size_t sersize;
  (void) plist_ser_generic(&ser, &sersize, fdesc.data, fdesc.desc);
  (void) plist_deser_generic(&mem, ser, sersize, false, fdesc.desc);

  ddsrt_free(ser);
  plist_fini_generic(&mem, fdesc.desc, false);

  return 0;
}
