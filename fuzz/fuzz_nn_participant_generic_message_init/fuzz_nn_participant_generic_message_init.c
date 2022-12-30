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

 #include "dds/ddsrt/heap.h"
 #include "dds/ddsrt/string.h"
 #include "dds/ddsi/ddsi_security_msg.h"


int LLVMFuzzerTestOneInput(const char *data, size_t size)
{
  nn_participant_generic_message_t msg_in;
  nn_participant_generic_message_t fuzz_msg;

  if(size <= sizeof(nn_participant_generic_message_t))
  {
    return 0;
  }
  memset(fuzz_msg, data, sizeof(nn_participant_generic_message_t))


  /* Create the message (normally with various arguments). */
  nn_participant_generic_message_init(
              &msg_in,
              &fuzz_msg.message_identity.source_guid,
               fuzz_msg.message_identity.sequence_number,
              &fuzz_msg.destination_participant_guid,
              &fuzz_msg.destination_endpoint_guid,
              &fuzz_msg.source_endpoint_guid,
               fuzz_msg.message_class_id,
              &fuzz_msg.message_data,
              &fuzz_msg.related_message_identity);

  /* Cleanup. */
  nn_participant_generic_message_deinit(&msg_in);
  //ddsrt_free(data);
  return 0;
}
