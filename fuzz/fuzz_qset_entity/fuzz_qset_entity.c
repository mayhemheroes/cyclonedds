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

#include "dds/ddsrt/environ.h"
#include "dds/ddsrt/misc.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsi/ddsi_xqos.h"
//#include "test_util.h"
#include "DataRepresentationTypes.h"

#define DDS_DOMAINID 0
#define DDS_CONFIG "${CYCLONEDDS_URI}${CYCLONEDDS_URI:+,}<Discovery><ExternalDomainId>0</ExternalDomainId></Discovery>"

#define MAX_DR 10
#define DESC(n) DataRepresentationTypes_ ## n ## _desc
#define XCDR1 DDS_DATA_REPRESENTATION_XCDR1
#define XCDR2 DDS_DATA_REPRESENTATION_XCDR2

static dds_entity_t d, dp;

static void data_representation_init(void)
{
  char * conf = ddsrt_expand_envvars(DDS_CONFIG, DDS_DOMAINID);
  d = dds_create_domain(DDS_DOMAINID, conf);
  ddsrt_free(conf);
  dp = dds_create_participant(DDS_DOMAINID, NULL, NULL);

}

static void data_representation_fini(void)
{
 dds_delete (d);
}

int LLVMFuzzerTestOneInput(const char *data, size_t size)
{
  data_representation_init();

  char *topicname = "MayhemFuzzing";
  dds_entity_t tp = dds_create_topic(dp, &DESC(TypeFinal), topicname, NULL, NULL);

  dds_entity_t ent = 0;
  ent = dds_create_reader(dp, tp, NULL, NULL);
  ent = dds_create_writer(dp, tp, NULL, NULL);
  //datarep_qos_exp_t exp = { .exp = { { XCDR1, XCDR2 }, 2 } };
  //exp_qos (ent, &exp);
  dds_qos_t *qos = dds_create_qos();
  dds_qset_userdata(qos, data, size);
  dds_set_qos(ent, qos);
  //exp_qos (ent, &exp);

  // change data representation
  //dds_qset_data_representation (qos, 1, (dds_data_representation_id_t[]) { XCDR2 });
  //ret = dds_set_qos (ent, qos);
  //if(ret != DDS_RETCODE_IMMUTABLE_POLICY)
  //{
  //  panic();
  //}
  dds_delete_qos (qos);

  data_representation_fini();
  return 0;
}
