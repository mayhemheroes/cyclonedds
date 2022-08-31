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
 #include "dds/ddsi/ddsi_iid.h"
 #include "dds/ddsi/q_thread.h"
 #include "dds/ddsi/ddsi_config_impl.h"
 #include "dds/ddsi/ddsi_domaingv.h"
 #include "dds/ddsi/ddsi_entity.h"
 #include "dds/ddsi/q_radmin.h"
 #include "dds/ddsi/ddsi_plist.h"
 #include "dds/ddsi/q_transmit.h"
 #include "dds/ddsi/q_receive.h"
 #include "dds/ddsi/q_xmsg.h"
 #include "dds/ddsi/q_addrset.h"
 #include "dds/ddsi/ddsi_tkmap.h"
 #include "dds/ddsi/ddsi_sertype.h"
 #include "dds/ddsi/ddsi_serdata.h"
 #include "dds/ddsi/ddsi_builtin_topic_if.h"
 #include "dds/ddsi/ddsi_security_omg.h"
 #include "dds/ddsi/ddsi_rhc.h"
 #include "dds/ddsi/ddsi_vnet.h"
 #include "dds/ddsi/ddsi_tran.h"
 #include "dds/ddsi/ddsi_entity_index.h"
 #include "dds/ddsi/q_bswap.h"
 #include "dds__whc.h"
 #include "dds__types.h"

 #include "dds/security/core/dds_security_utils.h"


int LLVMFuzzerTestOneInput(
    const char *data,
    size_t size)
{

  DDS_Security_parse_xml_date(data);
  return EXIT_SUCCESS;
}
