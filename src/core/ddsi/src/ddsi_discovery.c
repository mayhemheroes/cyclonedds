/*
 * Copyright(c) 2006 to 2022 ZettaScale Technology and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <ctype.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "dds/version.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/log.h"
#include "dds/ddsrt/md5.h"
#include "dds/ddsrt/sync.h"
#include "dds/ddsrt/avl.h"
#include "dds/ddsrt/string.h"
#include "dds/ddsi/ddsi_log.h"
#include "dds/ddsi/ddsi_unused.h"
#include "dds/ddsi/ddsi_domaingv.h"
#include "dds/ddsi/ddsi_feature_check.h"
#include "ddsi__protocol.h"
#include "ddsi__misc.h"
#include "ddsi__xevent.h"
#include "ddsi__discovery.h"
#include "ddsi__serdata_plist.h"
#include "ddsi__radmin.h"
#include "ddsi__entity_index.h"
#include "ddsi__entity.h"
#include "ddsi__participant.h"
#include "ddsi__xmsg.h"
#include "ddsi__transmit.h"
#include "ddsi__lease.h"
#include "ddsi__security_omg.h"
#include "ddsi__pmd.h"
#include "ddsi__endpoint.h"
#include "ddsi__plist.h"
#include "ddsi__proxy_endpoint.h"
#include "ddsi__proxy_participant.h"
#include "ddsi__topic.h"
#include "ddsi__tran.h"
#include "ddsi__typelib.h"
#include "ddsi__vendor.h"
#include "ddsi__xqos.h"
#include "ddsi__addrset.h"
#ifdef DDS_HAS_TYPE_DISCOVERY
#include "ddsi__typelookup.h"
#endif

#ifdef DDS_HAS_SECURITY
#include "ddsi__security_exchange.h"
#endif

typedef enum ddsi_sedp_kind {
  SEDP_KIND_READER,
  SEDP_KIND_WRITER,
  SEDP_KIND_TOPIC
} ddsi_sedp_kind_t;

static void allowmulticast_aware_add_to_addrset (const struct ddsi_domaingv *gv, uint32_t allow_multicast, struct ddsi_addrset *as, const ddsi_xlocator_t *loc)
{
#ifdef DDS_HAS_SSM
  if (ddsi_is_ssm_mcaddr (gv, &loc->c))
  {
    if (!(allow_multicast & DDSI_AMC_SSM))
      return;
  }
  else if (ddsi_is_mcaddr (gv, &loc->c))
  {
    if (!(allow_multicast & DDSI_AMC_ASM))
      return;
  }
#else
  if (ddsi_is_mcaddr (gv, &loc->c) && !(allow_multicast & DDSI_AMC_ASM))
    return;
#endif
  ddsi_add_xlocator_to_addrset (gv, as, loc);
}

typedef struct interface_set {
  bool xs[MAX_XMIT_CONNS];
} interface_set_t;

static void interface_set_init (interface_set_t *intfs)
{
  for (size_t i = 0; i < sizeof (intfs->xs) / sizeof (intfs->xs[0]); i++)
    intfs->xs[i] = false;
}

static void addrset_from_locatorlists_add_one (struct ddsi_domaingv const * const gv, const ddsi_locator_t *loc, struct ddsi_addrset *as, interface_set_t *intfs, bool *direct)
{
  size_t interf_idx;
  switch (ddsi_is_nearby_address (gv, loc, (size_t) gv->n_interfaces, gv->interfaces, &interf_idx))
  {
    case DNAR_SELF:
    case DNAR_LOCAL:
      // if it matches an interface, use that one and record that this is a
      // directly connected interface: those will then all be possibilities
      // for transmitting multicasts (assuming capable, allowed, &c.)
      assert (interf_idx < MAX_XMIT_CONNS);
      ddsi_add_xlocator_to_addrset (gv, as, &(const ddsi_xlocator_t) {
        .conn = gv->xmit_conns[interf_idx],
        .c = *loc });
      intfs->xs[interf_idx] = true;
      *direct = true;
      break;
    case DNAR_DISTANT:
      // If DONT_ROUTE is set and there is no matching interface, then presumably
      // one would not be able to reach this address.
      if (!gv->config.dontRoute)
      {
        // Pick the first selected interface that isn't link-local or loopback
        // (maybe it matters, maybe not, but it doesn't make sense to assign
        // a transmit socket for a local interface to a distant host).  If none
        // exists, skip the address.
        for (int i = 0; i < gv->n_interfaces; i++)
        {
          // do not use link-local or loopback interfaces transmit conn for distant nodes
          if (gv->interfaces[i].link_local || gv->interfaces[i].loopback)
            continue;
          ddsi_add_xlocator_to_addrset (gv, as, &(const ddsi_xlocator_t) {
            .conn = gv->xmit_conns[i],
            .c = *loc });
          break;
        }
      }
      break;
    case DNAR_UNREACHABLE:
      break;
  }
}

/** @brief Constructs a new address set from uni- and multicast locators received in SPDP or SEDP
 *
 * The construction process uses heuristics for determining which interfaces appear to be applicable for and uses
 * this information to set (1) the transmit sockets and (2) choose the interfaces with which to associate multicast
 * addresses.
 *
 * Loopback addresses are accepted if it can be determined that they originate on the same machine:
 * - if all enabled interfaces are loopback interfaces, the peer must be on the same host (this ought to be cached)
 * - if all advertised addresses are loopback addresses
 * - if there is a non-unicast address that matches one of the (enabled) addresses of the host
 *
 * Unicast addresses are matched against interface addresses to determine whether the address is likely to be
 * reachable without any routing. If so, the address is assigned to the interface and the interface is marked as
 * "enabled" for the purposes of multicast handling. If not, it is associated with the first enabled non-loopback
 * interface on the assumption that unicast routing works fine (but the interface is not "enabled" for multicast
 * handling).
 *
 * Multicast addresses are added only for interfaces that are "enabled" based on unicast processing. If none are
 * and the source locator matches an interface, it will enable that interface.
 *
 * @param[in] gv domain state, needed for interfaces, transports, tracing
 * @param[in] uc list of advertised unicast locators
 * @param[in] mc list of advertised multicast locators
 * @param[in] srcloc source address for discovery packet, or "invalid"
 * @param[in,out] inherited_intfs set of applicable interfaces, may be NULL
 *
 * @return new addrset, possibly empty */
static struct ddsi_addrset *addrset_from_locatorlists (const struct ddsi_domaingv *gv, const ddsi_locators_t *uc, const ddsi_locators_t *mc, const ddsi_locator_t *srcloc, const interface_set_t *inherited_intfs)
{
  struct ddsi_addrset *as = ddsi_new_addrset ();
  interface_set_t intfs;
  interface_set_init (&intfs);

  // if all interfaces are loopback, or all locators in uc are loopback, we're cool with loopback addresses
  bool allow_loopback;
  {
    bool a = true;
    for (int i = 0; i < gv->n_interfaces && a; i++)
      if (!gv->interfaces[i].loopback)
        a = false;
    bool b = true;
    // FIXME: what about the cases where SEDP gives just a loopback address, but the proxypp is known to be on a remote node?
    for (struct ddsi_locators_one *l = uc->first; l != NULL && b; l = l->next)
      b = ddsi_is_loopbackaddr (gv, &l->loc);
    allow_loopback = (a || b);
  }

  // if any non-loopback address is identical to one of our own addresses (actual or advertised),
  // assume it is the same machine, in which case loopback addresses may be picked up
  for (struct ddsi_locators_one *l = uc->first; l != NULL && !allow_loopback; l = l->next)
  {
    if (ddsi_is_loopbackaddr (gv, &l->loc))
      continue;
    allow_loopback = (ddsi_is_nearby_address (gv, &l->loc, (size_t) gv->n_interfaces, gv->interfaces, NULL) == DNAR_SELF);
  }
  //GVTRACE(" allow_loopback=%d\n", allow_loopback);

  bool direct = false;
  for (struct ddsi_locators_one *l = uc->first; l != NULL; l = l->next)
  {
#if 0
    {
      char buf[DDSI_LOCSTRLEN];
      ddsi_locator_to_string_no_port (buf, sizeof (buf), &l->loc);
      GVTRACE("%s: ignore %d loopback %d\n", buf, l->loc.tran->m_ignore, ddsi_is_loopbackaddr (gv, &l->loc));
    }
#endif
    // skip unrecognized ones, as well as loopback ones if not on the same host
    if (!allow_loopback && ddsi_is_loopbackaddr (gv, &l->loc))
      continue;

    ddsi_locator_t loc = l->loc;

    // if the advertised locator matches our own external locator, than presumably
    // it is the same machine and should be addressed using the actual interface
    // address
    bool extloc_of_self = false;
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      if (loc.kind == gv->interfaces[i].loc.kind && memcmp (loc.address, gv->interfaces[i].extloc.address, sizeof (loc.address)) == 0)
      {
        memcpy (loc.address, gv->interfaces[i].loc.address, sizeof (loc.address));
        extloc_of_self = true;
        break;
      }
    }

    if (!extloc_of_self && loc.kind == DDSI_LOCATOR_KIND_UDPv4 && gv->extmask.kind != DDSI_LOCATOR_KIND_INVALID)
    {
      /* If the examined locator is in the same subnet as our own
         external IP address, this locator will be translated into one
         in the same subnet as our own local ip and selected. */
      assert (gv->n_interfaces == 1); // gv->extmask: the hack is only supported if limited to a single interface
      struct in_addr tmp4 = *((struct in_addr *) (loc.address + 12));
      const struct in_addr ownip = *((struct in_addr *) (gv->interfaces[0].loc.address + 12));
      const struct in_addr extip = *((struct in_addr *) (gv->interfaces[0].extloc.address + 12));
      const struct in_addr extmask = *((struct in_addr *) (gv->extmask.address + 12));

      if ((tmp4.s_addr & extmask.s_addr) == (extip.s_addr & extmask.s_addr))
      {
        /* translate network part of the IP address from the external
           one to the internal one */
        tmp4.s_addr = (tmp4.s_addr & ~extmask.s_addr) | (ownip.s_addr & extmask.s_addr);
        memcpy (loc.address + 12, &tmp4, 4);
      }
    }

    addrset_from_locatorlists_add_one (gv, &loc, as, &intfs, &direct);
  }

  if (ddsi_addrset_empty (as) && !ddsi_is_unspec_locator (srcloc))
  {
    //GVTRACE("add srcloc\n");
    // FIXME: conn_read should provide interface information in source address
    //GVTRACE (" add-srcloc");
    addrset_from_locatorlists_add_one (gv, srcloc, as, &intfs, &direct);
  }

  if (ddsi_addrset_empty (as) && inherited_intfs)
  {
    // implies no interfaces enabled in "intfs" yet -- just use whatever
    // we inherited for the purposes of selecting multicast addresses
    assert (!direct);
    for (int i = 0; i < gv->n_interfaces; i++)
      assert (!intfs.xs[i]);
    //GVTRACE (" using-inherited-intfs");
    intfs = *inherited_intfs;
  }
  else if (!direct && gv->config.multicast_ttl > 1)
  {
    //GVTRACE("assuming multicast routing works\n");
    // if not directly connected but multicast TTL allows routing,
    // assume any non-local interface will do
    //GVTRACE (" enabling-non-loopback/link-local");
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      assert (!intfs.xs[i]);
      intfs.xs[i] = !(gv->interfaces[i].link_local || gv->interfaces[i].loopback);
    }
  }

#if 0
  GVTRACE("enabled interfaces for multicast:");
  for (int i = 0; i < gv->n_interfaces; i++)
  {
    if (intfs[i])
      GVTRACE(" %s(%d)", gv->interfaces[i].name, gv->interfaces[i].mc_capable);
  }
  GVTRACE("\n");
#endif

  for (struct ddsi_locators_one *l = mc->first; l != NULL; l = l->next)
  {
    for (int i = 0; i < gv->n_interfaces; i++)
    {
      if (intfs.xs[i] && gv->interfaces[i].mc_capable)
      {
        const ddsi_xlocator_t loc = {
          .conn = gv->xmit_conns[i],
          .c = l->loc
        };
        if (ddsi_factory_supports (loc.conn->m_factory, loc.c.kind))
          allowmulticast_aware_add_to_addrset (gv, gv->config.allowMulticast, as, &loc);
      }
    }
  }
  return as;
}


/******************************************************************************
 ***
 *** SPDP
 ***
 *****************************************************************************/

static void maybe_add_pp_as_meta_to_as_disc (struct ddsi_domaingv *gv, const struct ddsi_addrset *as_meta)
{
  if (ddsi_addrset_empty_mc (as_meta) || !(gv->config.allowMulticast & DDSI_AMC_SPDP))
  {
    ddsi_xlocator_t loc;
    if (ddsi_addrset_any_uc (as_meta, &loc))
    {
      ddsi_add_xlocator_to_addrset (gv, gv->as_disc, &loc);
    }
  }
}

struct locators_builder {
  ddsi_locators_t *dst;
  struct ddsi_locators_one *storage;
  size_t storage_n;
};

static struct locators_builder locators_builder_init (ddsi_locators_t *dst, struct ddsi_locators_one *storage, size_t storage_n)
{
  dst->n = 0;
  dst->first = NULL;
  dst->last = NULL;
  return (struct locators_builder) {
    .dst = dst,
    .storage = storage,
    .storage_n = storage_n
  };
}

static bool locators_add_one (struct locators_builder *b, const ddsi_locator_t *loc, uint32_t port_override)
{
  if (b->dst->n >= b->storage_n)
    return false;
  if (b->dst->n == 0)
    b->dst->first = b->storage;
  else
    b->dst->last->next = &b->storage[b->dst->n];
  b->dst->last = &b->storage[b->dst->n++];
  b->dst->last->loc = *loc;
  if (port_override != DDSI_LOCATOR_PORT_INVALID)
    b->dst->last->loc.port = port_override;
  b->dst->last->next = NULL;
  return true;
}

static bool include_multicast_locator_in_discovery (const struct ddsi_participant *pp)
{
#ifdef DDS_HAS_SSM
  /* Note that if the default multicast address is an SSM address,
     we will simply advertise it. The recipients better understand
     it means the writers will publish to address and the readers
     favour SSM. */
  if (ddsi_is_ssm_mcaddr (pp->e.gv, &pp->e.gv->loc_default_mc))
    return (pp->e.gv->config.allowMulticast & DDSI_AMC_SSM) != 0;
  else
    return (pp->e.gv->config.allowMulticast & DDSI_AMC_ASM) != 0;
#else
  return (pp->e.gv->config.allowMulticast & DDSI_AMC_ASM) != 0;
#endif
}

void ddsi_get_participant_builtin_topic_data (const struct ddsi_participant *pp, ddsi_plist_t *dst, struct ddsi_participant_builtin_topic_data_locators *locs)
{
  size_t size;
  char node[64];
  uint64_t qosdiff;

  ddsi_plist_init_empty (dst);
  dst->present |= PP_PARTICIPANT_GUID | PP_BUILTIN_ENDPOINT_SET |
    PP_PROTOCOL_VERSION | PP_VENDORID | PP_DOMAIN_ID;
  dst->participant_guid = pp->e.guid;
  dst->builtin_endpoint_set = pp->bes;
  dst->protocol_version.major = DDSI_RTPS_MAJOR;
  dst->protocol_version.minor = DDSI_RTPS_MINOR;
  dst->vendorid = DDSI_VENDORID_ECLIPSE;
  dst->domain_id = pp->e.gv->config.extDomainId.value;
  /* Be sure not to send a DOMAIN_TAG when it is the default (an empty)
     string: it is an "incompatible-if-unrecognized" parameter, and so
     implementations that don't understand the parameter will refuse to
     discover us, and so sending the default would break backwards
     compatibility. */
  if (strcmp (pp->e.gv->config.domainTag, "") != 0)
  {
    dst->present |= PP_DOMAIN_TAG;
    dst->aliased |= PP_DOMAIN_TAG;
    dst->domain_tag = pp->e.gv->config.domainTag;
  }

  // Construct unicast locator parameters
  {
    struct locators_builder def_uni = locators_builder_init (&dst->default_unicast_locators, locs->def_uni, MAX_XMIT_CONNS);
    struct locators_builder meta_uni = locators_builder_init (&dst->metatraffic_unicast_locators, locs->meta_uni, MAX_XMIT_CONNS);
    for (int i = 0; i < pp->e.gv->n_interfaces; i++)
    {
      if (!pp->e.gv->xmit_conns[i]->m_factory->m_enable_spdp)
      {
        // skip any interfaces where the address kind doesn't match the selected transport
        // as a reasonablish way of not advertising iceoryx locators here
        continue;
      }
#ifndef NDEBUG
      int32_t kind;
#endif
      uint32_t data_port, meta_port;
      if (pp->e.gv->config.many_sockets_mode != DDSI_MSM_MANY_UNICAST)
      {
#ifndef NDEBUG
        kind = pp->e.gv->loc_default_uc.kind;
#endif
        assert (kind == pp->e.gv->loc_meta_uc.kind);
        data_port = pp->e.gv->loc_default_uc.port;
        meta_port = pp->e.gv->loc_meta_uc.port;
      }
      else
      {
#ifndef NDEBUG
        kind = pp->m_locator.kind;
#endif
        data_port = meta_port = pp->m_locator.port;
      }
      assert (kind == pp->e.gv->interfaces[i].extloc.kind);
      locators_add_one (&def_uni, &pp->e.gv->interfaces[i].extloc, data_port);
      locators_add_one (&meta_uni, &pp->e.gv->interfaces[i].extloc, meta_port);
    }
    if (pp->e.gv->config.publish_uc_locators)
    {
      dst->present |= PP_DEFAULT_UNICAST_LOCATOR | PP_METATRAFFIC_UNICAST_LOCATOR;
      dst->aliased |= PP_DEFAULT_UNICAST_LOCATOR | PP_METATRAFFIC_UNICAST_LOCATOR;
    }
  }

  if (include_multicast_locator_in_discovery (pp))
  {
    dst->present |= PP_DEFAULT_MULTICAST_LOCATOR | PP_METATRAFFIC_MULTICAST_LOCATOR;
    dst->aliased |= PP_DEFAULT_MULTICAST_LOCATOR | PP_METATRAFFIC_MULTICAST_LOCATOR;
    struct locators_builder def_mc = locators_builder_init (&dst->default_multicast_locators, &locs->def_multi, 1);
    struct locators_builder meta_mc = locators_builder_init (&dst->metatraffic_multicast_locators, &locs->meta_multi, 1);
    locators_add_one (&def_mc, &pp->e.gv->loc_default_mc, DDSI_LOCATOR_PORT_INVALID);
    locators_add_one (&meta_mc, &pp->e.gv->loc_meta_mc, DDSI_LOCATOR_PORT_INVALID);
  }

  /* Add Adlink specific version information */
  {
    dst->present |= PP_ADLINK_PARTICIPANT_VERSION_INFO;
    memset (&dst->adlink_participant_version_info, 0, sizeof (dst->adlink_participant_version_info));
    dst->adlink_participant_version_info.version = 0;
    dst->adlink_participant_version_info.flags =
      DDSI_ADLINK_FL_DDSI2_PARTICIPANT_FLAG |
      DDSI_ADLINK_FL_PTBES_FIXED_0 |
      DDSI_ADLINK_FL_SUPPORTS_STATUSINFOX;
    if (pp->e.gv->config.besmode == DDSI_BESMODE_MINIMAL)
      dst->adlink_participant_version_info.flags |= DDSI_ADLINK_FL_MINIMAL_BES_MODE;
    ddsrt_mutex_lock (&pp->e.gv->privileged_pp_lock);
    if (pp->is_ddsi2_pp)
      dst->adlink_participant_version_info.flags |= DDSI_ADLINK_FL_PARTICIPANT_IS_DDSI2;
    ddsrt_mutex_unlock (&pp->e.gv->privileged_pp_lock);

#if DDSRT_HAVE_GETHOSTNAME
    if (ddsrt_gethostname(node, sizeof(node)-1) < 0)
#endif
      (void) ddsrt_strlcpy (node, "unknown", sizeof (node));
    size = strlen(node) + strlen(DDS_VERSION) + strlen(DDS_HOST_NAME) + strlen(DDS_TARGET_NAME) + 4; /* + ///'\0' */
    dst->adlink_participant_version_info.internals = ddsrt_malloc(size);
    (void) snprintf(dst->adlink_participant_version_info.internals, size, "%s/%s/%s/%s", node, DDS_VERSION, DDS_HOST_NAME, DDS_TARGET_NAME);
    ETRACE (pp, "ddsi_spdp_write("PGUIDFMT") - internals: %s\n", PGUID (pp->e.guid), dst->adlink_participant_version_info.internals);
  }

  /* Add Cyclone specific information */
  {
    const uint32_t bufsz = ddsi_receive_buffer_size (pp->e.gv->m_factory);
    if (bufsz > 0)
    {
      dst->present |= PP_CYCLONE_RECEIVE_BUFFER_SIZE;
      dst->cyclone_receive_buffer_size = bufsz;
    }
  }
  if (pp->e.gv->config.redundant_networking)
  {
    dst->present |= PP_CYCLONE_REDUNDANT_NETWORKING;
    dst->cyclone_redundant_networking = true;
  }

#ifdef DDS_HAS_SECURITY
  /* Add Security specific information. */
  if (ddsi_omg_get_participant_security_info (pp, &(dst->participant_security_info))) {
    dst->present |= PP_PARTICIPANT_SECURITY_INFO;
    dst->aliased |= PP_PARTICIPANT_SECURITY_INFO;
  }
#endif

  /* Participant QoS's insofar as they are set, different from the default, and mapped to the SPDP data, rather than to the Adlink-specific CMParticipant endpoint. */
  qosdiff = ddsi_xqos_delta (&pp->plist->qos, &ddsi_default_qos_participant, DDSI_QP_USER_DATA | DDSI_QP_ENTITY_NAME | DDSI_QP_PROPERTY_LIST | DDSI_QP_LIVELINESS);
  if (pp->e.gv->config.explicitly_publish_qos_set_to_default)
    qosdiff |= ~(DDSI_QP_UNRECOGNIZED_INCOMPATIBLE_MASK | DDSI_QP_LIVELINESS);

  assert (dst->qos.present == 0);
  ddsi_plist_mergein_missing (dst, pp->plist, 0, qosdiff);
#ifdef DDS_HAS_SECURITY
  if (ddsi_omg_participant_is_secure(pp))
    ddsi_plist_mergein_missing (dst, pp->plist, PP_IDENTITY_TOKEN | PP_PERMISSIONS_TOKEN, 0);
#endif
}

static int write_and_fini_plist (struct ddsi_writer *wr, ddsi_plist_t *ps, bool alive)
{
  struct ddsi_serdata *serdata = ddsi_serdata_from_sample (wr->type, alive ? SDK_DATA : SDK_KEY, ps);
  ddsi_plist_fini (ps);
  serdata->statusinfo = alive ? 0 : (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER);
  serdata->timestamp = ddsrt_time_wallclock ();
  return ddsi_write_sample_nogc_notk (ddsi_lookup_thread_state (), NULL, wr, serdata);
}

int ddsi_spdp_write (struct ddsi_participant *pp)
{
  struct ddsi_writer *wr;
  ddsi_plist_t ps;
  struct ddsi_participant_builtin_topic_data_locators locs;

  if (pp->e.onlylocal) {
      /* This topic is only locally available. */
      return 0;
  }

  ETRACE (pp, "ddsi_spdp_write("PGUIDFMT")\n", PGUID (pp->e.guid));

  if ((wr = ddsi_get_builtin_writer (pp, DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER)) == NULL)
  {
    ETRACE (pp, "ddsi_spdp_write("PGUIDFMT") - builtin participant writer not found\n", PGUID (pp->e.guid));
    return 0;
  }

  ddsi_get_participant_builtin_topic_data (pp, &ps, &locs);
  return write_and_fini_plist (wr, &ps, true);
}

static int ddsi_spdp_dispose_unregister_with_wr (struct ddsi_participant *pp, unsigned entityid)
{
  ddsi_plist_t ps;
  struct ddsi_writer *wr;

  if ((wr = ddsi_get_builtin_writer (pp, entityid)) == NULL)
  {
    ETRACE (pp, "ddsi_spdp_dispose_unregister("PGUIDFMT") - builtin participant %s writer not found\n",
            PGUID (pp->e.guid),
            entityid == DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER ? "secure" : "");
    return 0;
  }

  ddsi_plist_init_empty (&ps);
  ps.present |= PP_PARTICIPANT_GUID;
  ps.participant_guid = pp->e.guid;
  return write_and_fini_plist (wr, &ps, false);
}

int ddsi_spdp_dispose_unregister (struct ddsi_participant *pp)
{
  /*
   * When disposing a participant, it should be announced on both the
   * non-secure and secure writers.
   * The receiver will decide from which writer it accepts the dispose.
   */
  int ret = ddsi_spdp_dispose_unregister_with_wr(pp, DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER);
  if ((ret > 0) && ddsi_omg_participant_is_secure(pp))
  {
    ret = ddsi_spdp_dispose_unregister_with_wr(pp, DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER);
  }
  return ret;
}

static unsigned pseudo_random_delay (const ddsi_guid_t *x, const ddsi_guid_t *y, ddsrt_mtime_t tnow)
{
  /* You know, an ordinary random generator would be even better, but
     the C library doesn't have a reentrant one and I don't feel like
     integrating, say, the Mersenne Twister right now. */
  static const uint64_t cs[] = {
    UINT64_C (15385148050874689571),
    UINT64_C (17503036526311582379),
    UINT64_C (11075621958654396447),
    UINT64_C ( 9748227842331024047),
    UINT64_C (14689485562394710107),
    UINT64_C (17256284993973210745),
    UINT64_C ( 9288286355086959209),
    UINT64_C (17718429552426935775),
    UINT64_C (10054290541876311021),
    UINT64_C (13417933704571658407)
  };
  uint32_t a = x->prefix.u[0], b = x->prefix.u[1], c = x->prefix.u[2], d = x->entityid.u;
  uint32_t e = y->prefix.u[0], f = y->prefix.u[1], g = y->prefix.u[2], h = y->entityid.u;
  uint32_t i = (uint32_t) ((uint64_t) tnow.v >> 32), j = (uint32_t) tnow.v;
  uint64_t m = 0;
  m += (a + cs[0]) * (b + cs[1]);
  m += (c + cs[2]) * (d + cs[3]);
  m += (e + cs[4]) * (f + cs[5]);
  m += (g + cs[6]) * (h + cs[7]);
  m += (i + cs[8]) * (j + cs[9]);
  return (unsigned) (m >> 32);
}

static void respond_to_spdp (const struct ddsi_domaingv *gv, const ddsi_guid_t *dest_proxypp_guid)
{
  struct ddsi_entity_enum_participant est;
  struct ddsi_participant *pp;
  ddsrt_mtime_t tnow = ddsrt_time_monotonic ();
  ddsi_entidx_enum_participant_init (&est, gv->entity_index);
  while ((pp = ddsi_entidx_enum_participant_next (&est)) != NULL)
  {
    /* delay_base has 32 bits, so delay_norm is approximately 1s max;
       delay_max <= 1s by gv.config checks */
    unsigned delay_base = pseudo_random_delay (&pp->e.guid, dest_proxypp_guid, tnow);
    unsigned delay_norm = delay_base >> 2;
    int64_t delay_max_ms = gv->config.spdp_response_delay_max / 1000000;
    int64_t delay = (int64_t) delay_norm * delay_max_ms / 1000;
    ddsrt_mtime_t tsched = ddsrt_mtime_add_duration (tnow, delay);
    GVTRACE (" %"PRId64, delay);
    if (!pp->e.gv->config.unicast_response_to_spdp_messages)
      /* pp can't reach gc_delete_participant => can safely reschedule */
      (void) ddsi_resched_xevent_if_earlier (pp->spdp_xevent, tsched);
    else
      ddsi_qxev_spdp (gv->xevents, tsched, &pp->e.guid, dest_proxypp_guid);
  }
  ddsi_entidx_enum_participant_fini (&est);
}

static int handle_spdp_dead (const struct ddsi_receiver_state *rst, ddsi_entityid_t pwr_entityid, ddsrt_wctime_t timestamp, const ddsi_plist_t *datap, unsigned statusinfo)
{
  struct ddsi_domaingv * const gv = rst->gv;
  ddsi_guid_t guid;

  GVLOGDISC ("SPDP ST%x", statusinfo);

  if (datap->present & PP_PARTICIPANT_GUID)
  {
    guid = datap->participant_guid;
    GVLOGDISC (" %"PRIx32":%"PRIx32":%"PRIx32":%"PRIx32, PGUID (guid));
    assert (guid.entityid.u == DDSI_ENTITYID_PARTICIPANT);
    if (ddsi_is_proxy_participant_deletion_allowed(gv, &guid, pwr_entityid))
    {
      if (ddsi_delete_proxy_participant_by_guid (gv, &guid, timestamp, 0) < 0)
      {
        GVLOGDISC (" unknown");
      }
      else
      {
        GVLOGDISC (" delete");
      }
    }
    else
    {
      GVLOGDISC (" not allowed");
    }
  }
  else
  {
    GVWARNING ("data (SPDP, vendor %u.%u): no/invalid payload\n", rst->vendor.id[0], rst->vendor.id[1]);
  }
  return 1;
}

static struct ddsi_proxy_participant *find_ddsi2_proxy_participant (const struct ddsi_entity_index *entidx, const ddsi_guid_t *ppguid)
{
  struct ddsi_entity_enum_proxy_participant it;
  struct ddsi_proxy_participant *pp;
  ddsi_entidx_enum_proxy_participant_init (&it, entidx);
  while ((pp = ddsi_entidx_enum_proxy_participant_next (&it)) != NULL)
  {
    if (ddsi_vendor_is_eclipse_or_opensplice (pp->vendor) && pp->e.guid.prefix.u[0] == ppguid->prefix.u[0] && pp->is_ddsi2_pp)
      break;
  }
  ddsi_entidx_enum_proxy_participant_fini (&it);
  return pp;
}

static void make_participants_dependent_on_ddsi2 (struct ddsi_domaingv *gv, const ddsi_guid_t *ddsi2guid, ddsrt_wctime_t timestamp)
{
  struct ddsi_entity_enum_proxy_participant it;
  struct ddsi_proxy_participant *pp, *d2pp;
  if ((d2pp = ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, ddsi2guid)) == NULL)
    return;
  ddsi_entidx_enum_proxy_participant_init (&it, gv->entity_index);
  while ((pp = ddsi_entidx_enum_proxy_participant_next (&it)) != NULL)
  {
    if (ddsi_vendor_is_eclipse_or_opensplice (pp->vendor) && pp->e.guid.prefix.u[0] == ddsi2guid->prefix.u[0] && !pp->is_ddsi2_pp)
    {
      GVTRACE ("proxy participant "PGUIDFMT" depends on ddsi2 "PGUIDFMT, PGUID (pp->e.guid), PGUID (*ddsi2guid));
      ddsrt_mutex_lock (&pp->e.lock);
      pp->privileged_pp_guid = *ddsi2guid;
      ddsrt_mutex_unlock (&pp->e.lock);
      ddsi_proxy_participant_reassign_lease (pp, d2pp->lease);
      GVTRACE ("\n");

      if (ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, ddsi2guid) == NULL)
      {
        /* If DDSI2 has been deleted here (i.e., very soon after
           having been created), we don't know whether pp will be
           deleted */
        break;
      }
    }
  }
  ddsi_entidx_enum_proxy_participant_fini (&it);

  if (pp != NULL)
  {
    GVTRACE ("make_participants_dependent_on_ddsi2: ddsi2 "PGUIDFMT" is no more, delete "PGUIDFMT"\n", PGUID (*ddsi2guid), PGUID (pp->e.guid));
    ddsi_delete_proxy_participant_by_guid (gv, &pp->e.guid, timestamp, 1);
  }
}

static int handle_spdp_alive (const struct ddsi_receiver_state *rst, ddsi_seqno_t seq, ddsrt_wctime_t timestamp, const ddsi_plist_t *datap)
{
  struct ddsi_domaingv * const gv = rst->gv;
  const unsigned bes_sedp_announcer_mask =
    DDSI_DISC_BUILTIN_ENDPOINT_SUBSCRIPTION_ANNOUNCER |
    DDSI_DISC_BUILTIN_ENDPOINT_PUBLICATION_ANNOUNCER;
  struct ddsi_addrset *as_meta, *as_default;
  uint32_t builtin_endpoint_set;
  ddsi_guid_t privileged_pp_guid;
  dds_duration_t lease_duration;
  unsigned custom_flags = 0;

  /* If advertised domain id or domain tag doesn't match, ignore the message.  Do this first to
     minimize the impact such messages have. */
  {
    const uint32_t domain_id = (datap->present & PP_DOMAIN_ID) ? datap->domain_id : gv->config.extDomainId.value;
    const char *domain_tag = (datap->present & PP_DOMAIN_TAG) ? datap->domain_tag : "";
    if (domain_id != gv->config.extDomainId.value || strcmp (domain_tag, gv->config.domainTag) != 0)
    {
      GVTRACE ("ignore remote participant in mismatching domain %"PRIu32" tag \"%s\"\n", domain_id, domain_tag);
      return 0;
    }
  }

  if (!(datap->present & PP_PARTICIPANT_GUID) || !(datap->present & PP_BUILTIN_ENDPOINT_SET))
  {
    GVWARNING ("data (SPDP, vendor %u.%u): no/invalid payload\n", rst->vendor.id[0], rst->vendor.id[1]);
    return 0;
  }

  /* At some point the RTI implementation didn't mention
     BUILTIN_ENDPOINT_DDSI_PARTICIPANT_MESSAGE_DATA_READER & ...WRITER, or
     so it seemed; and yet they are necessary for correct operation,
     so add them. */
  builtin_endpoint_set = datap->builtin_endpoint_set;
  if (ddsi_vendor_is_rti (rst->vendor) &&
      ((builtin_endpoint_set &
        (DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER |
         DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER))
       != (DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER |
           DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER)) &&
      gv->config.assume_rti_has_pmd_endpoints)
  {
    GVLOGDISC ("data (SPDP, vendor %u.%u): assuming unadvertised PMD endpoints do exist\n",
             rst->vendor.id[0], rst->vendor.id[1]);
    builtin_endpoint_set |=
      DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_READER |
      DDSI_BUILTIN_ENDPOINT_PARTICIPANT_MESSAGE_DATA_WRITER;
  }

  /* Do we know this GUID already? */
  {
    struct ddsi_entity_common *existing_entity;
    if ((existing_entity = ddsi_entidx_lookup_guid_untyped (gv->entity_index, &datap->participant_guid)) == NULL)
    {
      /* Local SPDP packets may be looped back, and that can include ones
         for participants currently being deleted.  The first thing that
         happens when deleting a participant is removing it from the hash
         table, and consequently the looped back packet may appear to be
         from an unknown participant.  So we handle that. */
      if (ddsi_is_deleted_participant_guid (gv->deleted_participants, &datap->participant_guid, DDSI_DELETED_PPGUID_REMOTE))
      {
        RSTTRACE ("SPDP ST0 "PGUIDFMT" (recently deleted)", PGUID (datap->participant_guid));
        return 0;
      }
    }
    else if (existing_entity->kind == DDSI_EK_PARTICIPANT)
    {
      RSTTRACE ("SPDP ST0 "PGUIDFMT" (local)", PGUID (datap->participant_guid));
      return 0;
    }
    else if (existing_entity->kind == DDSI_EK_PROXY_PARTICIPANT)
    {
      struct ddsi_proxy_participant *proxypp = (struct ddsi_proxy_participant *) existing_entity;
      struct ddsi_lease *lease;
      int interesting = 0;
      RSTTRACE ("SPDP ST0 "PGUIDFMT" (known)", PGUID (datap->participant_guid));
      /* SPDP processing is so different from normal processing that we are
         even skipping the automatic lease renewal. Note that proxy writers
         that are not alive are not set alive here. This is done only when
         data is received from a particular pwr (in handle_regular) */
      if ((lease = ddsrt_atomic_ldvoidp (&proxypp->minl_auto)) != NULL)
        ddsi_lease_renew (lease, ddsrt_time_elapsed ());
      ddsrt_mutex_lock (&proxypp->e.lock);
      if (proxypp->implicitly_created || seq > proxypp->seq)
      {
        interesting = 1;
        if (!(gv->logconfig.c.mask & DDS_LC_TRACE))
          GVLOGDISC ("SPDP ST0 "PGUIDFMT, PGUID (datap->participant_guid));
        GVLOGDISC (proxypp->implicitly_created ? " (NEW was-implicitly-created)" : " (update)");
        proxypp->implicitly_created = 0;
        ddsi_update_proxy_participant_plist_locked (proxypp, seq, datap, timestamp);
      }
      ddsrt_mutex_unlock (&proxypp->e.lock);
      return interesting;
    }
    else
    {
      /* mismatch on entity kind: that should never have gotten past the
         input validation */
      GVWARNING ("data (SPDP, vendor %u.%u): "PGUIDFMT" kind mismatch\n", rst->vendor.id[0], rst->vendor.id[1], PGUID (datap->participant_guid));
      return 0;
    }
  }

  const bool is_secure = ((datap->builtin_endpoint_set & DDSI_DISC_BUILTIN_ENDPOINT_PARTICIPANT_SECURE_ANNOUNCER) != 0 &&
                          (datap->present & PP_IDENTITY_TOKEN));
  /* Make sure we don't create any security builtin endpoint when it's considered unsecure. */
  if (!is_secure)
    builtin_endpoint_set &= DDSI_BES_MASK_NON_SECURITY;
  GVLOGDISC ("SPDP ST0 "PGUIDFMT" bes %"PRIx32"%s NEW", PGUID (datap->participant_guid), builtin_endpoint_set, is_secure ? " (secure)" : "");

  if (datap->present & PP_ADLINK_PARTICIPANT_VERSION_INFO) {
    if ((datap->adlink_participant_version_info.flags & DDSI_ADLINK_FL_DDSI2_PARTICIPANT_FLAG) &&
        (datap->adlink_participant_version_info.flags & DDSI_ADLINK_FL_PARTICIPANT_IS_DDSI2))
      custom_flags |= DDSI_CF_PARTICIPANT_IS_DDSI2;

    GVLOGDISC (" (0x%08"PRIx32"-0x%08"PRIx32"-0x%08"PRIx32"-0x%08"PRIx32"-0x%08"PRIx32" %s)",
               datap->adlink_participant_version_info.version,
               datap->adlink_participant_version_info.flags,
               datap->adlink_participant_version_info.unused[0],
               datap->adlink_participant_version_info.unused[1],
               datap->adlink_participant_version_info.unused[2],
               datap->adlink_participant_version_info.internals);
  }

  /* Can't do "mergein_missing" because of constness of *datap */
  if (datap->qos.present & DDSI_QP_LIVELINESS)
    lease_duration = datap->qos.liveliness.lease_duration;
  else
  {
    assert (ddsi_default_qos_participant.present & DDSI_QP_LIVELINESS);
    lease_duration = ddsi_default_qos_participant.liveliness.lease_duration;
  }
  /* If any of the SEDP announcer are missing AND the guid prefix of
     the SPDP writer differs from the guid prefix of the new participant,
     we make it dependent on the writer's participant.  See also the
     lease expiration handling.  Note that the entityid MUST be
     DDSI_ENTITYID_PARTICIPANT or entidx_lookup will assert.  So we only
     zero the prefix. */
  privileged_pp_guid.prefix = rst->src_guid_prefix;
  privileged_pp_guid.entityid.u = DDSI_ENTITYID_PARTICIPANT;
  if ((builtin_endpoint_set & bes_sedp_announcer_mask) != bes_sedp_announcer_mask &&
      memcmp (&privileged_pp_guid, &datap->participant_guid, sizeof (ddsi_guid_t)) != 0)
  {
    GVLOGDISC (" (depends on "PGUIDFMT")", PGUID (privileged_pp_guid));
    /* never expire lease for this proxy: it won't actually expire
       until the "privileged" one expires anyway */
    lease_duration = DDS_INFINITY;
  }
  else if (ddsi_vendor_is_eclipse_or_opensplice (rst->vendor) && !(custom_flags & DDSI_CF_PARTICIPANT_IS_DDSI2))
  {
    /* Non-DDSI2 participants are made dependent on DDSI2 (but DDSI2
       itself need not be discovered yet) */
    struct ddsi_proxy_participant *ddsi2;
    if ((ddsi2 = find_ddsi2_proxy_participant (gv->entity_index, &datap->participant_guid)) == NULL)
      memset (&privileged_pp_guid.prefix, 0, sizeof (privileged_pp_guid.prefix));
    else
    {
      privileged_pp_guid.prefix = ddsi2->e.guid.prefix;
      lease_duration = DDS_INFINITY;
      GVLOGDISC (" (depends on "PGUIDFMT")", PGUID (privileged_pp_guid));
    }
  }
  else
  {
    memset (&privileged_pp_guid.prefix, 0, sizeof (privileged_pp_guid.prefix));
  }

  /* Choose locators */
  {
    const ddsi_locators_t emptyset = { .n = 0, .first = NULL, .last = NULL };
    const ddsi_locators_t *uc;
    const ddsi_locators_t *mc;
    ddsi_locator_t srcloc;
    interface_set_t intfs;

    srcloc = rst->srcloc;
    uc = (datap->present & PP_DEFAULT_UNICAST_LOCATOR) ? &datap->default_unicast_locators : &emptyset;
    mc = (datap->present & PP_DEFAULT_MULTICAST_LOCATOR) ? &datap->default_multicast_locators : &emptyset;
    if (gv->config.tcp_use_peeraddr_for_unicast)
      uc = &emptyset; // force use of source locator
    else if (uc != &emptyset)
      ddsi_set_unspec_locator (&srcloc); // can't always use the source address

    interface_set_init (&intfs);
    as_default = addrset_from_locatorlists (gv, uc, mc, &srcloc, &intfs);

    srcloc = rst->srcloc;
    uc = (datap->present & PP_METATRAFFIC_UNICAST_LOCATOR) ? &datap->metatraffic_unicast_locators : &emptyset;
    mc = (datap->present & PP_METATRAFFIC_MULTICAST_LOCATOR) ? &datap->metatraffic_multicast_locators : &emptyset;
    if (gv->config.tcp_use_peeraddr_for_unicast)
      uc = &emptyset; // force use of source locator
    else if (uc != &emptyset)
      ddsi_set_unspec_locator (&srcloc); // can't always use the source address
    interface_set_init (&intfs);
    as_meta = addrset_from_locatorlists (gv, uc, mc, &srcloc, &intfs);

    ddsi_log_addrset (gv, DDS_LC_DISCOVERY, " (data", as_default);
    ddsi_log_addrset (gv, DDS_LC_DISCOVERY, " meta", as_meta);
    GVLOGDISC (")");
  }

  if (ddsi_addrset_empty_uc (as_default) || ddsi_addrset_empty_uc (as_meta))
  {
    GVLOGDISC (" (no unicast address");
    ddsi_unref_addrset (as_default);
    ddsi_unref_addrset (as_meta);
    return 1;
  }

  GVLOGDISC (" QOS={");
  ddsi_xqos_log (DDS_LC_DISCOVERY, &gv->logconfig, &datap->qos);
  GVLOGDISC ("}\n");

  maybe_add_pp_as_meta_to_as_disc (gv, as_meta);

  if (!ddsi_new_proxy_participant (gv, &datap->participant_guid, builtin_endpoint_set, &privileged_pp_guid, as_default, as_meta, datap, lease_duration, rst->vendor, custom_flags, timestamp, seq))
  {
    /* If no proxy participant was created, don't respond */
    return 0;
  }
  else
  {
    /* Force transmission of SPDP messages - we're not very careful
       in avoiding the processing of SPDP packets addressed to others
       so filter here */
    int have_dst = (rst->dst_guid_prefix.u[0] != 0 || rst->dst_guid_prefix.u[1] != 0 || rst->dst_guid_prefix.u[2] != 0);
    if (!have_dst)
    {
      GVLOGDISC ("broadcasted SPDP packet -> answering");
      respond_to_spdp (gv, &datap->participant_guid);
    }
    else
    {
      GVLOGDISC ("directed SPDP packet -> not responding\n");
    }

    if (custom_flags & DDSI_CF_PARTICIPANT_IS_DDSI2)
    {
      /* If we just discovered DDSI2, make sure any existing
         participants served by it are made dependent on it */
      make_participants_dependent_on_ddsi2 (gv, &datap->participant_guid, timestamp);
    }
    else if (privileged_pp_guid.prefix.u[0] || privileged_pp_guid.prefix.u[1] || privileged_pp_guid.prefix.u[2])
    {
      /* If we just created a participant dependent on DDSI2, make sure
         DDSI2 still exists.  There is a risk of racing the lease expiry
         of DDSI2. */
      if (ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, &privileged_pp_guid) == NULL)
      {
        GVLOGDISC ("make_participants_dependent_on_ddsi2: ddsi2 "PGUIDFMT" is no more, delete "PGUIDFMT"\n",
                   PGUID (privileged_pp_guid), PGUID (datap->participant_guid));
        ddsi_delete_proxy_participant_by_guid (gv, &datap->participant_guid, timestamp, 1);
      }
    }
    return 1;
  }
}

static void handle_spdp (const struct ddsi_receiver_state *rst, ddsi_entityid_t pwr_entityid, ddsi_seqno_t seq, const struct ddsi_serdata *serdata)
{
  struct ddsi_domaingv * const gv = rst->gv;
  ddsi_plist_t decoded_data;
  if (ddsi_serdata_to_sample (serdata, &decoded_data, NULL, NULL))
  {
    int interesting = 0;
    switch (serdata->statusinfo & (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER))
    {
      case 0:
        interesting = handle_spdp_alive (rst, seq, serdata->timestamp, &decoded_data);
        break;

      case DDSI_STATUSINFO_DISPOSE:
      case DDSI_STATUSINFO_UNREGISTER:
      case (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER):
        interesting = handle_spdp_dead (rst, pwr_entityid, serdata->timestamp, &decoded_data, serdata->statusinfo);
        break;
    }

    ddsi_plist_fini (&decoded_data);
    GVLOG (interesting ? DDS_LC_DISCOVERY : DDS_LC_TRACE, "\n");
  }
}

struct add_locator_to_ps_arg {
  struct ddsi_domaingv *gv;
  ddsi_plist_t *ps;
};

static void add_locator_to_ps (const ddsi_locator_t *loc, void *varg)
{
  struct add_locator_to_ps_arg *arg = varg;
  struct ddsi_locators_one *elem = ddsrt_malloc (sizeof (struct ddsi_locators_one));
  struct ddsi_locators *locs;
  unsigned present_flag;

  elem->loc = *loc;
  elem->next = NULL;

  if (ddsi_is_mcaddr (arg->gv, loc)) {
    locs = &arg->ps->multicast_locators;
    present_flag = PP_MULTICAST_LOCATOR;
  } else {
    locs = &arg->ps->unicast_locators;
    present_flag = PP_UNICAST_LOCATOR;
  }

  if (!(arg->ps->present & present_flag))
  {
    locs->n = 0;
    locs->first = locs->last = NULL;
    arg->ps->present |= present_flag;
  }
  locs->n++;
  if (locs->first)
    locs->last->next = elem;
  else
    locs->first = elem;
  locs->last = elem;
}

static void add_xlocator_to_ps (const ddsi_xlocator_t *loc, void *varg)
{
  add_locator_to_ps (&loc->c, varg);
}

#ifdef DDS_HAS_SHM
static void add_iox_locator_to_ps(const ddsi_locator_t* loc, struct add_locator_to_ps_arg *arg)
{
  struct ddsi_locators_one* elem = ddsrt_malloc(sizeof(struct ddsi_locators_one));
  struct ddsi_locators* locs = &arg->ps->unicast_locators;
  unsigned present_flag = PP_UNICAST_LOCATOR;

  elem->loc = *loc;
  elem->next = NULL;

  if (!(arg->ps->present & present_flag))
  {
    locs->n = 0;
    locs->first = locs->last = NULL;
    arg->ps->present |= present_flag;
  }

  //add iceoryx to the FRONT of the list of addresses, to indicate its higher priority
  if (locs->first)
    elem->next = locs->first;
  else
    locs->last = elem;
  locs->first = elem;
  locs->n++;
}
#endif

/******************************************************************************
 ***
 *** SEDP
 ***
 *****************************************************************************/

static struct ddsi_writer *get_sedp_writer (const struct ddsi_participant *pp, unsigned entityid)
{
  struct ddsi_writer *sedp_wr = ddsi_get_builtin_writer (pp, entityid);
  if (sedp_wr == NULL)
    DDS_FATAL ("sedp_write_writer: no SEDP builtin writer %x for "PGUIDFMT"\n", entityid, PGUID (pp->e.guid));
  return sedp_wr;
}

static int sedp_write_endpoint_impl
(
   struct ddsi_writer *wr, int alive, const ddsi_guid_t *guid,
   const struct ddsi_endpoint_common *epcommon,
   const dds_qos_t *xqos, struct ddsi_addrset *as, ddsi_security_info_t *security
#ifdef DDS_HAS_TYPE_DISCOVERY
   , const struct ddsi_sertype *sertype
#endif
)
{
  struct ddsi_domaingv * const gv = wr->e.gv;
  const dds_qos_t *defqos = NULL;
  if (ddsi_is_writer_entityid (guid->entityid))
    defqos = &ddsi_default_qos_writer;
  else if (ddsi_is_reader_entityid (guid->entityid))
    defqos = &ddsi_default_qos_reader;
  else
    assert (false);

  uint64_t qosdiff;
  ddsi_plist_t ps;

  ddsi_plist_init_empty (&ps);
  ps.present |= PP_ENDPOINT_GUID;
  ps.endpoint_guid = *guid;

#ifdef DDS_HAS_SECURITY
  if (security)
  {
    ps.present |= PP_ENDPOINT_SECURITY_INFO;
    memcpy(&ps.endpoint_security_info, security, sizeof(ddsi_security_info_t));
  }
#else
  (void)security;
  assert(security == NULL);
#endif

  if (!alive)
  {
    assert (xqos == NULL);
    assert (epcommon == NULL);
    qosdiff = 0;
  }
  else
  {
    assert (xqos != NULL);
    ps.present |= PP_PROTOCOL_VERSION | PP_VENDORID;
    ps.protocol_version.major = DDSI_RTPS_MAJOR;
    ps.protocol_version.minor = DDSI_RTPS_MINOR;
    ps.vendorid = DDSI_VENDORID_ECLIPSE;

    assert (epcommon != NULL);

    if (epcommon->group_guid.entityid.u != 0)
    {
      ps.present |= PP_GROUP_GUID;
      ps.group_guid = epcommon->group_guid;
    }

    if (!ddsi_is_writer_entityid (guid->entityid))
    {
      const struct ddsi_reader *rd = ddsi_entidx_lookup_reader_guid (gv->entity_index, guid);
      assert (rd);
      if (rd->request_keyhash)
      {
        ps.present |= PP_CYCLONE_REQUESTS_KEYHASH;
        ps.cyclone_requests_keyhash = 1u;
      }
    }

#ifdef DDS_HAS_SSM
    /* A bit of a hack -- the easy alternative would be to make it yet
    another parameter.  We only set "reader favours SSM" if we
    really do: no point in telling the world that everything is at
    the default. */
    if (ddsi_is_reader_entityid (guid->entityid))
    {
      const struct ddsi_reader *rd = ddsi_entidx_lookup_reader_guid (gv->entity_index, guid);
      assert (rd);
      if (rd->favours_ssm)
      {
        ps.present |= PP_READER_FAVOURS_SSM;
        ps.reader_favours_ssm.state = 1u;
      }
    }
#endif

    qosdiff = ddsi_xqos_delta (xqos, defqos, ~(uint64_t)0);
    if (gv->config.explicitly_publish_qos_set_to_default)
      qosdiff |= ~DDSI_QP_UNRECOGNIZED_INCOMPATIBLE_MASK;

    struct add_locator_to_ps_arg arg;
    arg.gv = gv;
    arg.ps = &ps;
    if (as)
      ddsi_addrset_forall (as, add_xlocator_to_ps, &arg);

#ifdef DDS_HAS_SHM
    assert(wr->xqos->present & DDSI_QP_LOCATOR_MASK);
    if (!(xqos->ignore_locator_type & DDSI_LOCATOR_KIND_SHEM))
    {
      if (!(arg.ps->present & PP_UNICAST_LOCATOR) || 0 == arg.ps->unicast_locators.n)
      {
        if (epcommon->pp->e.gv->config.many_sockets_mode == DDSI_MSM_MANY_UNICAST)
          add_locator_to_ps(&epcommon->pp->m_locator, &arg);
        else
        {
          // FIXME: same as what SPDP uses, should be refactored, now more than ever
          for (int i = 0; i < epcommon->pp->e.gv->n_interfaces; i++)
          {
            if (!epcommon->pp->e.gv->xmit_conns[i]->m_factory->m_enable_spdp)
            {
              // skip any interfaces where the address kind doesn't match the selected transport
              // as a reasonablish way of not advertising iceoryx locators here
              continue;
            }
            // FIXME: should have multiple loc_default_uc/loc_meta_uc or compute ports here
            ddsi_locator_t loc = epcommon->pp->e.gv->interfaces[i].extloc;
            loc.port = epcommon->pp->e.gv->loc_default_uc.port;
            add_locator_to_ps(&loc, &arg);
          }
        }
      }

      if (!(arg.ps->present & PP_MULTICAST_LOCATOR) || 0 == arg.ps->multicast_locators.n)
      {
        if (include_multicast_locator_in_discovery (epcommon->pp))
          add_locator_to_ps (&epcommon->pp->e.gv->loc_default_mc, &arg);
      }

      add_iox_locator_to_ps(&gv->loc_iceoryx_addr, &arg);
    }
#endif

#ifdef DDS_HAS_TYPE_DISCOVERY
    assert (sertype);
    if ((ps.qos.type_information = ddsi_sertype_typeinfo (sertype)))
      ps.qos.present |= DDSI_QP_TYPE_INFORMATION;
#endif
  }

  if (xqos)
    ddsi_xqos_mergein_missing (&ps.qos, xqos, qosdiff);
  return write_and_fini_plist (wr, &ps, alive);
}

#ifdef DDS_HAS_TOPIC_DISCOVERY

static int ddsi_sedp_write_topic_impl (struct ddsi_writer *wr, int alive, const ddsi_guid_t *guid, const dds_qos_t *xqos, ddsi_typeinfo_t *type_info)
{
  struct ddsi_domaingv * const gv = wr->e.gv;
  const dds_qos_t *defqos = &ddsi_default_qos_topic;

  ddsi_plist_t ps;
  ddsi_plist_init_empty (&ps);
  ps.present |= PP_CYCLONE_TOPIC_GUID;
  ps.topic_guid = *guid;

  assert (xqos != NULL);
  ps.present |= PP_PROTOCOL_VERSION | PP_VENDORID;
  ps.protocol_version.major = DDSI_RTPS_MAJOR;
  ps.protocol_version.minor = DDSI_RTPS_MINOR;
  ps.vendorid = DDSI_VENDORID_ECLIPSE;

  uint64_t qosdiff = ddsi_xqos_delta (xqos, defqos, ~(uint64_t)0);
  if (gv->config.explicitly_publish_qos_set_to_default)
    qosdiff |= ~DDSI_QP_UNRECOGNIZED_INCOMPATIBLE_MASK;

  if (type_info)
  {
    ps.qos.type_information = type_info;
    ps.qos.present |= DDSI_QP_TYPE_INFORMATION;
  }
  if (xqos)
    ddsi_xqos_mergein_missing (&ps.qos, xqos, qosdiff);
  return write_and_fini_plist (wr, &ps, alive);
}

int ddsi_sedp_write_topic (struct ddsi_topic *tp, bool alive)
{
  int res = 0;
  if (!(tp->pp->bes & DDSI_DISC_BUILTIN_ENDPOINT_TOPICS_ANNOUNCER))
    return res;
  if (!ddsi_is_builtin_entityid (tp->e.guid.entityid, DDSI_VENDORID_ECLIPSE) && !tp->e.onlylocal)
  {
    unsigned entityid = ddsi_determine_topic_writer (tp);
    struct ddsi_writer *sedp_wr = get_sedp_writer (tp->pp, entityid);
    ddsrt_mutex_lock (&tp->e.qos_lock);
    // the allocation type info object is freed with the plist
    res = ddsi_sedp_write_topic_impl (sedp_wr, alive, &tp->e.guid, tp->definition->xqos, ddsi_type_pair_complete_info (tp->e.gv, tp->definition->type_pair));
    ddsrt_mutex_unlock (&tp->e.qos_lock);
  }
  return res;
}

#endif /* DDS_HAS_TOPIC_DISCOVERY */

int ddsi_sedp_write_writer (struct ddsi_writer *wr)
{
  if ((!ddsi_is_builtin_entityid(wr->e.guid.entityid, DDSI_VENDORID_ECLIPSE)) && (!wr->e.onlylocal))
  {
    unsigned entityid = ddsi_determine_publication_writer(wr);
    struct ddsi_writer *sedp_wr = get_sedp_writer (wr->c.pp, entityid);
    ddsi_security_info_t *security = NULL;
#ifdef DDS_HAS_SSM
    struct ddsi_addrset *as = wr->ssm_as;
#else
    struct ddsi_addrset *as = NULL;
#endif
#ifdef DDS_HAS_SECURITY
    ddsi_security_info_t tmp;
    if (ddsi_omg_get_writer_security_info (wr, &tmp))
    {
      security = &tmp;
    }
#endif
#ifdef DDS_HAS_TYPE_DISCOVERY
    return sedp_write_endpoint_impl (sedp_wr, 1, &wr->e.guid, &wr->c, wr->xqos, as, security, wr->type);
#else
    return sedp_write_endpoint_impl (sedp_wr, 1, &wr->e.guid, &wr->c, wr->xqos, as, security);
#endif
  }
  return 0;
}

int ddsi_sedp_write_reader (struct ddsi_reader *rd)
{
  if (ddsi_is_builtin_entityid (rd->e.guid.entityid, DDSI_VENDORID_ECLIPSE) || rd->e.onlylocal)
    return 0;

  unsigned entityid = ddsi_determine_subscription_writer(rd);
  struct ddsi_writer *sedp_wr = get_sedp_writer (rd->c.pp, entityid);
  ddsi_security_info_t *security = NULL;
  struct ddsi_addrset *as = NULL;
#ifdef DDS_HAS_NETWORK_PARTITIONS
  if (rd->uc_as != NULL || rd->mc_as != NULL)
  {
    // FIXME: do this without first creating a temporary addrset
    as = ddsi_new_addrset ();
    // use a placeholder connection to avoid exploding the multicast addreses to multiple
    // interfaces
    for (const struct ddsi_networkpartition_address *a = rd->uc_as; a != NULL; a = a->next)
      ddsi_add_xlocator_to_addrset(rd->e.gv, as, &(const ddsi_xlocator_t) {
        .c = a->loc,
        .conn = rd->e.gv->xmit_conns[0] });
    for (const struct ddsi_networkpartition_address *a = rd->mc_as; a != NULL; a = a->next)
      ddsi_add_xlocator_to_addrset(rd->e.gv, as, &(const ddsi_xlocator_t) {
        .c = a->loc,
        .conn = rd->e.gv->xmit_conns[0] });
  }
#endif
#ifdef DDS_HAS_SECURITY
  ddsi_security_info_t tmp;
  if (ddsi_omg_get_reader_security_info (rd, &tmp))
  {
    security = &tmp;
  }
#endif
#ifdef DDS_HAS_TYPE_DISCOVERY
  const int ret = sedp_write_endpoint_impl (sedp_wr, 1, &rd->e.guid, &rd->c, rd->xqos, as, security, rd->type);
#else
  const int ret = sedp_write_endpoint_impl (sedp_wr, 1, &rd->e.guid, &rd->c, rd->xqos, as, security);
#endif
  ddsi_unref_addrset (as);
  return ret;
}

int ddsi_sedp_dispose_unregister_writer (struct ddsi_writer *wr)
{
  if ((!ddsi_is_builtin_entityid(wr->e.guid.entityid, DDSI_VENDORID_ECLIPSE)) && (!wr->e.onlylocal))
  {
    unsigned entityid = ddsi_determine_publication_writer(wr);
    struct ddsi_writer *sedp_wr = get_sedp_writer (wr->c.pp, entityid);
#ifdef DDS_HAS_TYPE_DISCOVERY
    return sedp_write_endpoint_impl (sedp_wr, 0, &wr->e.guid, NULL, NULL, NULL, NULL, NULL);
#else
    return sedp_write_endpoint_impl (sedp_wr, 0, &wr->e.guid, NULL, NULL, NULL, NULL);
#endif
  }
  return 0;
}

int ddsi_sedp_dispose_unregister_reader (struct ddsi_reader *rd)
{
  if ((!ddsi_is_builtin_entityid(rd->e.guid.entityid, DDSI_VENDORID_ECLIPSE)) && (!rd->e.onlylocal))
  {
    unsigned entityid = ddsi_determine_subscription_writer(rd);
    struct ddsi_writer *sedp_wr = get_sedp_writer (rd->c.pp, entityid);
#ifdef DDS_HAS_TYPE_DISCOVERY
    return sedp_write_endpoint_impl (sedp_wr, 0, &rd->e.guid, NULL, NULL, NULL, NULL, NULL);
#else
    return sedp_write_endpoint_impl (sedp_wr, 0, &rd->e.guid, NULL, NULL, NULL, NULL);
#endif
  }
  return 0;
}

static const char *durability_to_string (dds_durability_kind_t k)
{
  switch (k)
  {
    case DDS_DURABILITY_VOLATILE: return "volatile";
    case DDS_DURABILITY_TRANSIENT_LOCAL: return "transient-local";
    case DDS_DURABILITY_TRANSIENT: return "transient";
    case DDS_DURABILITY_PERSISTENT: return "persistent";
  }
  return "undefined-durability";
}

static struct ddsi_proxy_participant *implicitly_create_proxypp (struct ddsi_domaingv *gv, const ddsi_guid_t *ppguid, ddsi_plist_t *datap /* note: potentially modifies datap */, const ddsi_guid_prefix_t *src_guid_prefix, ddsi_vendorid_t vendorid, ddsrt_wctime_t timestamp, ddsi_seqno_t seq)
{
  ddsi_guid_t privguid;
  ddsi_plist_t pp_plist;

  if (memcmp (&ppguid->prefix, src_guid_prefix, sizeof (ppguid->prefix)) == 0)
    /* if the writer is owned by the participant itself, we're not interested */
    return NULL;

  privguid.prefix = *src_guid_prefix;
  privguid.entityid = ddsi_to_entityid (DDSI_ENTITYID_PARTICIPANT);
  ddsi_plist_init_empty(&pp_plist);

  if (ddsi_vendor_is_cloud (vendorid))
  {
    ddsi_vendorid_t actual_vendorid;
    /* Some endpoint that we discovered through the DS, but then it must have at least some locators */
    GVTRACE (" from-DS %"PRIx32":%"PRIx32":%"PRIx32":%"PRIx32, PGUID (privguid));
    /* avoid "no address" case, so we never create the proxy participant for nothing (FIXME: rework some of this) */
    if (!(datap->present & (PP_UNICAST_LOCATOR | PP_MULTICAST_LOCATOR)))
    {
      GVTRACE (" data locator absent\n");
      goto err;
    }
    GVTRACE (" new-proxypp "PGUIDFMT"\n", PGUID (*ppguid));
    /* We need to handle any source of entities, but we really want to try to keep the GIDs (and
       certainly the systemId component) unchanged for OSPL.  The new proxy participant will take
       the GID from the GUID if it is from a "modern" OSPL that advertises it includes all GIDs in
       the endpoint discovery; else if it is OSPL it will take at the systemId and fake the rest.
       However, (1) Cloud filters out the GIDs from the discovery, and (2) DDSI2 deliberately
       doesn't include the GID for internally generated endpoints (such as the fictitious transient
       data readers) to signal that these are internal and have no GID (and not including a GID if
       there is none is quite a reasonable approach).  Point (2) means we have no reliable way of
       determining whether GIDs are included based on the first endpoint, and so there is no point
       doing anything about (1).  That means we fall back to the legacy mode of locally generating
       GIDs but leaving the system id unchanged if the remote is OSPL.  */
    actual_vendorid = (datap->present & PP_VENDORID) ?  datap->vendorid : vendorid;
    (void) ddsi_new_proxy_participant (gv, ppguid, 0, &privguid, ddsi_new_addrset(), ddsi_new_addrset(), &pp_plist, DDS_INFINITY, actual_vendorid, DDSI_CF_IMPLICITLY_CREATED_PROXYPP, timestamp, seq);
  }
  else if (ppguid->prefix.u[0] == src_guid_prefix->u[0] && ddsi_vendor_is_eclipse_or_opensplice (vendorid))
  {
    /* FIXME: requires address sets to be those of ddsi2, no built-in
       readers or writers, only if remote ddsi2 is provably running
       with a minimal built-in endpoint set */
    struct ddsi_proxy_participant *privpp;
    if ((privpp = ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, &privguid)) == NULL) {
      GVTRACE (" unknown-src-proxypp?\n");
      goto err;
    } else if (!privpp->is_ddsi2_pp) {
      GVTRACE (" src-proxypp-not-ddsi2?\n");
      goto err;
    } else if (!privpp->minimal_bes_mode) {
      GVTRACE (" src-ddsi2-not-minimal-bes-mode?\n");
      goto err;
    } else {
      struct ddsi_addrset *as_default, *as_meta;
      ddsi_plist_t tmp_plist;
      GVTRACE (" from-ddsi2 "PGUIDFMT, PGUID (privguid));
      ddsi_plist_init_empty (&pp_plist);

      ddsrt_mutex_lock (&privpp->e.lock);
      as_default = ddsi_ref_addrset(privpp->as_default);
      as_meta = ddsi_ref_addrset(privpp->as_meta);
      /* copy just what we need */
      tmp_plist = *privpp->plist;
      tmp_plist.present = PP_PARTICIPANT_GUID | PP_ADLINK_PARTICIPANT_VERSION_INFO;
      tmp_plist.participant_guid = *ppguid;
      ddsi_plist_mergein_missing (&pp_plist, &tmp_plist, ~(uint64_t)0, ~(uint64_t)0);
      ddsrt_mutex_unlock (&privpp->e.lock);

      pp_plist.adlink_participant_version_info.flags &= ~DDSI_ADLINK_FL_PARTICIPANT_IS_DDSI2;
      ddsi_new_proxy_participant (gv, ppguid, 0, &privguid, as_default, as_meta, &pp_plist, DDS_INFINITY, vendorid, DDSI_CF_IMPLICITLY_CREATED_PROXYPP | DDSI_CF_PROXYPP_NO_SPDP, timestamp, seq);
    }
  }

 err:
  ddsi_plist_fini (&pp_plist);
  return ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, ppguid);
}

static bool check_sedp_kind_and_guid (ddsi_sedp_kind_t sedp_kind, const ddsi_guid_t *entity_guid)
{
  switch (sedp_kind)
  {
    case SEDP_KIND_TOPIC:
      return ddsi_is_topic_entityid (entity_guid->entityid);
    case SEDP_KIND_WRITER:
      return ddsi_is_writer_entityid (entity_guid->entityid);
    case SEDP_KIND_READER:
      return ddsi_is_reader_entityid (entity_guid->entityid);
  }
  assert (0);
  return false;
}

static bool handle_sedp_checks (struct ddsi_domaingv * const gv, ddsi_sedp_kind_t sedp_kind, ddsi_guid_t *entity_guid, ddsi_plist_t *datap,
    const ddsi_guid_prefix_t *src_guid_prefix, ddsi_vendorid_t vendorid, ddsrt_wctime_t timestamp,
    struct ddsi_proxy_participant **proxypp, ddsi_guid_t *ppguid)
{
#define E(msg, lbl) do { GVLOGDISC (msg); return false; } while (0)
  if (!check_sedp_kind_and_guid (sedp_kind, entity_guid))
    E (" SEDP topic/GUID entity kind mismatch\n", err);
  ppguid->prefix = entity_guid->prefix;
  ppguid->entityid.u = DDSI_ENTITYID_PARTICIPANT;
  // Accept the presence of a participant GUID, but only if it matches
  if ((datap->present & PP_PARTICIPANT_GUID) && memcmp (&datap->participant_guid, ppguid, sizeof (*ppguid)) != 0)
    E (" endpoint/participant GUID mismatch", err);
  if (ddsi_is_deleted_participant_guid (gv->deleted_participants, ppguid, DDSI_DELETED_PPGUID_REMOTE))
    E (" local dead pp?\n", err);
  if (ddsi_entidx_lookup_participant_guid (gv->entity_index, ppguid) != NULL)
    E (" local pp?\n", err);
  if (ddsi_is_builtin_entityid (entity_guid->entityid, vendorid))
    E (" built-in\n", err);
  if (!(datap->qos.present & DDSI_QP_TOPIC_NAME))
    E (" no topic?\n", err);
  if (!(datap->qos.present & DDSI_QP_TYPE_NAME))
    E (" no typename?\n", err);
  if ((*proxypp = ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, ppguid)) == NULL)
  {
    GVLOGDISC (" unknown-proxypp");
    if ((*proxypp = implicitly_create_proxypp (gv, ppguid, datap, src_guid_prefix, vendorid, timestamp, 0)) == NULL)
      E ("?\n", err);
    /* Repeat regular SEDP trace for convenience */
    GVLOGDISC ("SEDP ST0 "PGUIDFMT" (cont)", PGUID (*entity_guid));
  }
  return true;
#undef E
}

struct ddsi_addrset_from_locatorlists_collect_interfaces_arg {
  const struct ddsi_domaingv *gv;
  interface_set_t *intfs;
};

/** @brief Figure out which interfaces are touched by (extended) locator @p loc
 *
 * Does this by looking up the connection in @p loc in the set of transmit connections. (There's plenty of room for optimisation here.)
 *
 * @param[in] loc locator
 * @param[in] varg argument pointer, must point to a struct ddsi_addrset_from_locatorlists_collect_interfaces_arg
 */
static void addrset_from_locatorlists_collect_interfaces (const ddsi_xlocator_t *loc, void *varg)
{
  struct ddsi_addrset_from_locatorlists_collect_interfaces_arg *arg = varg;
  struct ddsi_domaingv const * const gv = arg->gv;
  for (int i = 0; i < gv->n_interfaces; i++)
  {
    //GVTRACE(" {%p,%p}", loc->conn, gv->xmit_conns[i]);
    if (loc->conn == gv->xmit_conns[i])
    {
      arg->intfs->xs[i] = true;
      break;
    }
  }
}

struct ddsi_addrset *ddsi_get_endpoint_addrset (const struct ddsi_domaingv *gv, const ddsi_plist_t *datap, struct ddsi_addrset *proxypp_as_default, const ddsi_locator_t *rst_srcloc)
{
  const ddsi_locators_t emptyset = { .n = 0, .first = NULL, .last = NULL };
  const ddsi_locators_t *uc = (datap->present & PP_UNICAST_LOCATOR) ? &datap->unicast_locators : &emptyset;
  const ddsi_locators_t *mc = (datap->present & PP_MULTICAST_LOCATOR) ? &datap->multicast_locators : &emptyset;
  ddsi_locator_t srcloc;
  if (rst_srcloc == NULL)
    ddsi_set_unspec_locator (&srcloc);
  else // force use of source locator
  {
    uc = &emptyset;
    srcloc = *rst_srcloc;
  }

  // any interface that works for the participant is presumed ok
  interface_set_t intfs;
  interface_set_init (&intfs);
  ddsi_addrset_forall (proxypp_as_default, addrset_from_locatorlists_collect_interfaces, &(struct ddsi_addrset_from_locatorlists_collect_interfaces_arg){
    .gv = gv, .intfs = &intfs
  });
  //GVTRACE(" {%d%d%d%d}", intfs.xs[0], intfs.xs[1], intfs.xs[2], intfs.xs[3]);
  struct ddsi_addrset *as = addrset_from_locatorlists (gv, uc, mc, &srcloc, &intfs);
  // if SEDP gives:
  // - no addresses, use ppant uni- and multicast addresses
  // - only multicast, use those for multicast and use ppant address for unicast
  // - only unicast, use only those (i.e., disable multicast for this reader)
  // - both, use only those
  // FIXME: then you can't do a specific unicast address + SSM ... oh well
  if (ddsi_addrset_empty (as))
    ddsi_copy_addrset_into_addrset_mc (gv, as, proxypp_as_default);
  if (ddsi_addrset_empty_uc (as))
    ddsi_copy_addrset_into_addrset_uc (gv, as, proxypp_as_default);
  return as;
}

static void handle_sedp_alive_endpoint (const struct ddsi_receiver_state *rst, ddsi_seqno_t seq, ddsi_plist_t *datap /* note: potentially modifies datap */, ddsi_sedp_kind_t sedp_kind, const ddsi_guid_prefix_t *src_guid_prefix, ddsi_vendorid_t vendorid, ddsrt_wctime_t timestamp)
{
#define E(msg, lbl) do { GVLOGDISC (msg); goto lbl; } while (0)
  struct ddsi_domaingv * const gv = rst->gv;
  struct ddsi_proxy_participant *proxypp;
  struct ddsi_proxy_writer * pwr = NULL;
  struct ddsi_proxy_reader * prd = NULL;
  ddsi_guid_t ppguid;
  dds_qos_t *xqos;
  int reliable;
  struct ddsi_addrset *as;
#ifdef DDS_HAS_SSM
  int ssm;
#endif

  assert (datap);
  assert (datap->present & PP_ENDPOINT_GUID);
  GVLOGDISC (" "PGUIDFMT, PGUID (datap->endpoint_guid));

  if (!handle_sedp_checks (gv, sedp_kind, &datap->endpoint_guid, datap, src_guid_prefix, vendorid, timestamp, &proxypp, &ppguid))
    goto err;

  xqos = &datap->qos;
  if (sedp_kind == SEDP_KIND_READER)
    ddsi_xqos_mergein_missing (xqos, &ddsi_default_qos_reader, ~(uint64_t)0);
  else if (sedp_kind == SEDP_KIND_WRITER)
  {
    ddsi_xqos_mergein_missing (xqos, &ddsi_default_qos_writer, ~(uint64_t)0);
    if (!ddsi_vendor_is_eclipse_or_adlink (vendorid))
    {
      // there is a difference in interpretation of autodispose between vendors
      xqos->writer_data_lifecycle.autodispose_unregistered_instances = 0;
    }
  }
  else
    E (" invalid entity kind\n", err);

  /* After copy + merge, should have at least the ones present in the
     input.  Also verify reliability and durability are present,
     because we explicitly read those. */
  assert ((xqos->present & datap->qos.present) == datap->qos.present);
  assert (xqos->present & DDSI_QP_RELIABILITY);
  assert (xqos->present & DDSI_QP_DURABILITY);
  reliable = (xqos->reliability.kind == DDS_RELIABILITY_RELIABLE);

  GVLOGDISC (" %s %s %s %s: %s%s.%s/%s",
             reliable ? "reliable" : "best-effort",
             durability_to_string (xqos->durability.kind),
             sedp_kind == SEDP_KIND_WRITER ? "writer" : "reader",
             (xqos->present & DDSI_QP_ENTITY_NAME) ? xqos->entity_name : "unnamed",
             ((!(xqos->present & DDSI_QP_PARTITION) || xqos->partition.n == 0 || *xqos->partition.strs[0] == '\0')
              ? "(default)" : xqos->partition.strs[0]),
             ((xqos->present & DDSI_QP_PARTITION) && xqos->partition.n > 1) ? "+" : "",
             xqos->topic_name, xqos->type_name);

  if (sedp_kind == SEDP_KIND_READER && (datap->present & PP_EXPECTS_INLINE_QOS) && datap->expects_inline_qos)
    E ("******* AARGH - it expects inline QoS ********\n", err);

  ddsi_omg_log_endpoint_protection (gv, datap);
  if (ddsi_omg_is_endpoint_protected (datap) && !ddsi_omg_proxy_participant_is_secure (proxypp))
    E (" remote endpoint is protected while local federation is not secure\n", err);

  if (sedp_kind == SEDP_KIND_WRITER)
    pwr = ddsi_entidx_lookup_proxy_writer_guid (gv->entity_index, &datap->endpoint_guid);
  else
    prd = ddsi_entidx_lookup_proxy_reader_guid (gv->entity_index, &datap->endpoint_guid);
  if (pwr || prd)
  {
    /* Re-bind the proxy participant to the discovery service - and do this if it is currently
       bound to another DS instance, because that other DS instance may have already failed and
       with a new one taking over, without our noticing it. */
    GVLOGDISC (" known%s", ddsi_vendor_is_cloud (vendorid) ? "-DS" : "");
    if (ddsi_vendor_is_cloud (vendorid) && proxypp->implicitly_created && memcmp (&proxypp->privileged_pp_guid.prefix, src_guid_prefix, sizeof(proxypp->privileged_pp_guid.prefix)) != 0)
    {
      GVLOGDISC (" "PGUIDFMT" attach-to-DS "PGUIDFMT, PGUID(proxypp->e.guid), PGUIDPREFIX(*src_guid_prefix), proxypp->privileged_pp_guid.entityid.u);
      ddsrt_mutex_lock (&proxypp->e.lock);
      proxypp->privileged_pp_guid.prefix = *src_guid_prefix;
      ddsi_lease_set_expiry (proxypp->lease, DDSRT_ETIME_NEVER);
      ddsrt_mutex_unlock (&proxypp->e.lock);
    }
    GVLOGDISC ("\n");
  }
  else
  {
    GVLOGDISC (" NEW");
  }

  as = ddsi_get_endpoint_addrset (gv, datap, proxypp->as_default, gv->config.tcp_use_peeraddr_for_unicast ? &rst->srcloc : NULL);
  if (ddsi_addrset_empty (as))
  {
    ddsi_unref_addrset (as);
    E (" no address", err);
  }

  ddsi_log_addrset(gv, DDS_LC_DISCOVERY, " (as", as);
#ifdef DDS_HAS_SSM
  ssm = 0;
  if (sedp_kind == SEDP_KIND_WRITER)
    ssm = ddsi_addrset_contains_ssm (gv, as);
  else if (datap->present & PP_READER_FAVOURS_SSM)
    ssm = (datap->reader_favours_ssm.state != 0);
  GVLOGDISC (" ssm=%u", ssm);
#endif
  GVLOGDISC (") QOS={");
  ddsi_xqos_log (DDS_LC_DISCOVERY, &gv->logconfig, xqos);
  GVLOGDISC ("}\n");

  if ((datap->endpoint_guid.entityid.u & DDSI_ENTITYID_SOURCE_MASK) == DDSI_ENTITYID_SOURCE_VENDOR && !ddsi_vendor_is_eclipse_or_adlink (vendorid))
  {
    GVLOGDISC ("ignoring vendor-specific endpoint "PGUIDFMT"\n", PGUID (datap->endpoint_guid));
  }
  else
  {
    if (sedp_kind == SEDP_KIND_WRITER)
    {
      if (pwr)
        ddsi_update_proxy_writer (pwr, seq, as, xqos, timestamp);
      else
      {
        /* not supposed to get here for built-in ones, so can determine the channel based on the transport priority */
        assert (!ddsi_is_builtin_entityid (datap->endpoint_guid.entityid, vendorid));
#ifdef DDS_HAS_NETWORK_CHANNELS
        {
          struct ddsi_config_channel_listelem *channel = ddsi_find_network_channel (&gv->config, xqos->transport_priority);
          ddsi_new_proxy_writer (gv, &ppguid, &datap->endpoint_guid, as, datap, channel->dqueue, channel->evq ? channel->evq : gv->xevents, timestamp, seq);
        }
#else
        ddsi_new_proxy_writer (gv, &ppguid, &datap->endpoint_guid, as, datap, gv->user_dqueue, gv->xevents, timestamp, seq);
#endif
      }
    }
    else
    {
      if (prd)
        ddsi_update_proxy_reader (prd, seq, as, xqos, timestamp);
      else
      {
#ifdef DDS_HAS_SSM
        ddsi_new_proxy_reader (gv, &ppguid, &datap->endpoint_guid, as, datap, timestamp, seq, ssm);
#else
        ddsi_new_proxy_reader (gv, &ppguid, &datap->endpoint_guid, as, datap, timestamp, seq);
#endif
      }
    }
  }
  ddsi_unref_addrset (as);

err:
  return;
#undef E
}

static void handle_sedp_dead_endpoint (const struct ddsi_receiver_state *rst, ddsi_plist_t *datap, ddsi_sedp_kind_t sedp_kind, ddsrt_wctime_t timestamp)
{
  struct ddsi_domaingv * const gv = rst->gv;
  int res = -1;
  assert (datap->present & PP_ENDPOINT_GUID);
  GVLOGDISC (" "PGUIDFMT" ", PGUID (datap->endpoint_guid));
  if (!check_sedp_kind_and_guid (sedp_kind, &datap->endpoint_guid))
    return;
  else if (sedp_kind == SEDP_KIND_WRITER)
    res = ddsi_delete_proxy_writer (gv, &datap->endpoint_guid, timestamp, 0);
  else
    res = ddsi_delete_proxy_reader (gv, &datap->endpoint_guid, timestamp, 0);
  GVLOGDISC (" %s\n", (res < 0) ? " unknown" : " delete");
}

#ifdef DDS_HAS_TOPIC_DISCOVERY

static void handle_sedp_alive_topic (const struct ddsi_receiver_state *rst, ddsi_seqno_t seq, ddsi_plist_t *datap /* note: potentially modifies datap */, const ddsi_guid_prefix_t *src_guid_prefix, ddsi_vendorid_t vendorid, ddsrt_wctime_t timestamp)
{
  struct ddsi_domaingv * const gv = rst->gv;
  struct ddsi_proxy_participant *proxypp;
  ddsi_guid_t ppguid;
  dds_qos_t *xqos;
  int reliable;
  const ddsi_typeid_t *type_id_minimal = NULL, *type_id_complete = NULL;

  assert (datap);
  assert (datap->present & PP_CYCLONE_TOPIC_GUID);
  GVLOGDISC (" "PGUIDFMT, PGUID (datap->topic_guid));

  if (!handle_sedp_checks (gv, SEDP_KIND_TOPIC, &datap->topic_guid, datap, src_guid_prefix, vendorid, timestamp, &proxypp, &ppguid))
    return;

  xqos = &datap->qos;
  ddsi_xqos_mergein_missing (xqos, &ddsi_default_qos_topic, ~(uint64_t)0);
  /* After copy + merge, should have at least the ones present in the
     input. Also verify reliability and durability are present,
     because we explicitly read those. */
  assert ((xqos->present & datap->qos.present) == datap->qos.present);
  assert (xqos->present & DDSI_QP_RELIABILITY);
  assert (xqos->present & DDSI_QP_DURABILITY);
  reliable = (xqos->reliability.kind == DDS_RELIABILITY_RELIABLE);

  GVLOGDISC (" %s %s %s: %s/%s",
             reliable ? "reliable" : "best-effort",
             durability_to_string (xqos->durability.kind),
             "topic", xqos->topic_name, xqos->type_name);
  if (xqos->present & DDSI_QP_TYPE_INFORMATION)
  {
    struct ddsi_typeid_str strm, strc;
    type_id_minimal = ddsi_typeinfo_minimal_typeid (xqos->type_information);
    type_id_complete = ddsi_typeinfo_complete_typeid (xqos->type_information);
    GVLOGDISC (" tid %s/%s", ddsi_make_typeid_str(&strm, type_id_minimal), ddsi_make_typeid_str(&strc, type_id_complete));
  }
  GVLOGDISC (" QOS={");
  ddsi_xqos_log (DDS_LC_DISCOVERY, &gv->logconfig, xqos);
  GVLOGDISC ("}\n");

  if ((datap->topic_guid.entityid.u & DDSI_ENTITYID_SOURCE_MASK) == DDSI_ENTITYID_SOURCE_VENDOR && !ddsi_vendor_is_eclipse_or_adlink (vendorid))
  {
    GVLOGDISC ("ignoring vendor-specific topic "PGUIDFMT"\n", PGUID (datap->topic_guid));
  }
  else
  {
    // FIXME: check compatibility with known topic definitions
    struct ddsi_proxy_topic *ptp = ddsi_lookup_proxy_topic (proxypp, &datap->topic_guid);
    if (ptp)
    {
      GVLOGDISC (" update known proxy-topic%s\n", ddsi_vendor_is_cloud (vendorid) ? "-DS" : "");
      ddsi_update_proxy_topic (proxypp, ptp, seq, xqos, timestamp);
    }
    else
    {
      GVLOGDISC (" NEW proxy-topic");
      if (ddsi_new_proxy_topic (proxypp, seq, &datap->topic_guid, type_id_minimal, type_id_complete, xqos, timestamp) != DDS_RETCODE_OK)
        GVLOGDISC (" failed");
    }
  }
}

static void handle_sedp_dead_topic (const struct ddsi_receiver_state *rst, ddsi_plist_t *datap, ddsrt_wctime_t timestamp)
{
  struct ddsi_proxy_participant *proxypp;
  struct ddsi_proxy_topic *proxytp;
  struct ddsi_domaingv * const gv = rst->gv;
  assert (datap->present & PP_CYCLONE_TOPIC_GUID);
  GVLOGDISC (" "PGUIDFMT" ", PGUID (datap->topic_guid));
  if (!check_sedp_kind_and_guid (SEDP_KIND_TOPIC, &datap->topic_guid))
    return;
  ddsi_guid_t ppguid = { .prefix = datap->topic_guid.prefix, .entityid.u = DDSI_ENTITYID_PARTICIPANT };
  if ((proxypp = ddsi_entidx_lookup_proxy_participant_guid (gv->entity_index, &ppguid)) == NULL)
    GVLOGDISC (" unknown proxypp\n");
  else if ((proxytp = ddsi_lookup_proxy_topic (proxypp, &datap->topic_guid)) == NULL)
    GVLOGDISC (" unknown proxy topic\n");
  else
  {
    ddsrt_mutex_lock (&proxypp->e.lock);
    int res = ddsi_delete_proxy_topic_locked (proxypp, proxytp, timestamp);
    GVLOGDISC (" %s\n", res == DDS_RETCODE_PRECONDITION_NOT_MET ? " already-deleting" : " delete");
    ddsrt_mutex_unlock (&proxypp->e.lock);
  }
}

#endif /* DDS_HAS_TOPIC_DISCOVERY */

static void handle_sedp (const struct ddsi_receiver_state *rst, ddsi_seqno_t seq, struct ddsi_serdata *serdata, ddsi_sedp_kind_t sedp_kind)
{
  ddsi_plist_t decoded_data;
  if (ddsi_serdata_to_sample (serdata, &decoded_data, NULL, NULL))
  {
    struct ddsi_domaingv * const gv = rst->gv;
    GVLOGDISC ("SEDP ST%"PRIx32, serdata->statusinfo);
    switch (serdata->statusinfo & (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER))
    {
      case 0:
        switch (sedp_kind)
        {
          case SEDP_KIND_TOPIC:
#ifdef DDS_HAS_TOPIC_DISCOVERY
            handle_sedp_alive_topic (rst, seq, &decoded_data, &rst->src_guid_prefix, rst->vendor, serdata->timestamp);
#endif
            break;
          case SEDP_KIND_READER:
          case SEDP_KIND_WRITER:
            handle_sedp_alive_endpoint (rst, seq, &decoded_data, sedp_kind, &rst->src_guid_prefix, rst->vendor, serdata->timestamp);
            break;
        }
        break;
      case DDSI_STATUSINFO_DISPOSE:
      case DDSI_STATUSINFO_UNREGISTER:
      case (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER):
        switch (sedp_kind)
        {
          case SEDP_KIND_TOPIC:
#ifdef DDS_HAS_TOPIC_DISCOVERY
            handle_sedp_dead_topic (rst, &decoded_data, serdata->timestamp);
#endif
            break;
          case SEDP_KIND_READER:
          case SEDP_KIND_WRITER:
            handle_sedp_dead_endpoint (rst, &decoded_data, sedp_kind, serdata->timestamp);
            break;
        }
        break;
    }
    ddsi_plist_fini (&decoded_data);
  }
}

#ifdef DDS_HAS_TYPE_DISCOVERY
static void handle_typelookup (const struct ddsi_receiver_state *rst, ddsi_entityid_t wr_entity_id, struct ddsi_serdata *serdata)
{
  if (!(serdata->statusinfo & (DDSI_STATUSINFO_DISPOSE | DDSI_STATUSINFO_UNREGISTER)))
  {
    struct ddsi_domaingv * const gv = rst->gv;
    if (wr_entity_id.u == DDSI_ENTITYID_TL_SVC_BUILTIN_REQUEST_WRITER)
      ddsi_tl_handle_request (gv, serdata);
    else if (wr_entity_id.u == DDSI_ENTITYID_TL_SVC_BUILTIN_REPLY_WRITER)
      ddsi_tl_handle_reply (gv, serdata);
    else
      assert (false);
  }
}
#endif

/******************************************************************************
 *****************************************************************************/

int ddsi_builtins_dqueue_handler (const struct ddsi_rsample_info *sampleinfo, const struct ddsi_rdata *fragchain, UNUSED_ARG (const ddsi_guid_t *rdguid), UNUSED_ARG (void *qarg))
{
  struct ddsi_domaingv * const gv = sampleinfo->rst->gv;
  struct ddsi_proxy_writer *pwr;
  unsigned statusinfo;
  int need_keyhash;
  ddsi_guid_t srcguid;
  ddsi_rtps_data_datafrag_common_t *msg;
  unsigned char data_smhdr_flags;
  ddsi_plist_t qos;

  /* Luckily, most of the Data and DataFrag headers are the same - and
     in particular, all that we care about here is the same.  The
     key/data flags of DataFrag are different from those of Data, but
     DDSI2 used to treat them all as if they are data :( so now,
     instead of splitting out all the code, we reformat these flags
     from the submsg to always conform to that of the "Data"
     submessage regardless of the input. */
  msg = (ddsi_rtps_data_datafrag_common_t *) DDSI_RMSG_PAYLOADOFF (fragchain->rmsg, DDSI_RDATA_SUBMSG_OFF (fragchain));
  data_smhdr_flags = ddsi_normalize_data_datafrag_flags (&msg->smhdr);
  srcguid.prefix = sampleinfo->rst->src_guid_prefix;
  srcguid.entityid = msg->writerId;

  pwr = sampleinfo->pwr;
  if (pwr == NULL)
  {
    /* NULL with DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER is normal. It is possible that
     * DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER has NULL as well if there
     * is a security mismatch being handled. */
    assert ((srcguid.entityid.u == DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER) ||
            (srcguid.entityid.u == DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER));
  }
  else
  {
    assert (ddsi_is_builtin_entityid (pwr->e.guid.entityid, pwr->c.vendor));
    assert (memcmp (&pwr->e.guid, &srcguid, sizeof (srcguid)) == 0);
    assert (srcguid.entityid.u != DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER);
  }

  /* If there is no payload, it is either a completely invalid message
     or a dispose/unregister in RTI style. We assume the latter,
     consequently expect to need the keyhash.  Then, if sampleinfo
     says it is a complex qos, or the keyhash is required, extract all
     we need from the inline qos. */
  need_keyhash = (sampleinfo->size == 0 || (data_smhdr_flags & (DDSI_DATA_FLAG_KEYFLAG | DDSI_DATA_FLAG_DATAFLAG)) == 0);
  if (!(sampleinfo->complex_qos || need_keyhash))
  {
    ddsi_plist_init_empty (&qos);
    statusinfo = sampleinfo->statusinfo;
  }
  else
  {
    ddsi_plist_src_t src;
    size_t qos_offset = DDSI_RDATA_SUBMSG_OFF (fragchain) + offsetof (ddsi_rtps_data_datafrag_common_t, octetsToInlineQos) + sizeof (msg->octetsToInlineQos) + msg->octetsToInlineQos;
    dds_return_t plist_ret;
    src.protocol_version = sampleinfo->rst->protocol_version;
    src.vendorid = sampleinfo->rst->vendor;
    src.encoding = (msg->smhdr.flags & DDSI_RTPS_SUBMESSAGE_FLAG_ENDIANNESS) ? DDSI_RTPS_PL_CDR_LE : DDSI_RTPS_PL_CDR_BE;
    src.buf = DDSI_RMSG_PAYLOADOFF (fragchain->rmsg, qos_offset);
    src.bufsz = DDSI_RDATA_PAYLOAD_OFF (fragchain) - qos_offset;
    src.strict = DDSI_SC_STRICT_P (gv->config);
    if ((plist_ret = ddsi_plist_init_frommsg (&qos, NULL, PP_STATUSINFO | PP_KEYHASH, 0, &src, gv, DDSI_PLIST_CONTEXT_INLINE_QOS)) < 0)
    {
      if (plist_ret != DDS_RETCODE_UNSUPPORTED)
        GVWARNING ("data(builtin, vendor %u.%u): "PGUIDFMT" #%"PRIu64": invalid inline qos\n",
                   src.vendorid.id[0], src.vendorid.id[1], PGUID (srcguid), sampleinfo->seq);
      goto done_upd_deliv;
    }
    /* Complex qos bit also gets set when statusinfo bits other than
       dispose/unregister are set.  They are not currently defined,
       but this may save us if they do get defined one day. */
    statusinfo = (qos.present & PP_STATUSINFO) ? qos.statusinfo : 0;
  }

  if (pwr && ddsrt_avl_is_empty (&pwr->readers))
  {
    /* Wasn't empty when enqueued, but needn't still be; SPDP has no
       proxy writer, and is always accepted */
    goto done_upd_deliv;
  }

  /* proxy writers don't reference a type object, SPDP doesn't have matched readers
     but all the GUIDs are known, so be practical and map that */
  const struct ddsi_sertype *type;
  switch (srcguid.entityid.u)
  {
    case DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER:
      type = gv->spdp_type;
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER:
      type = gv->sedp_writer_type;
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER:
      type = gv->sedp_reader_type;
      break;
#ifdef DDS_HAS_TOPIC_DISCOVERY
    case DDSI_ENTITYID_SEDP_BUILTIN_TOPIC_WRITER:
      type = gv->sedp_topic_type;
      break;
#endif
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_WRITER:
      type = gv->pmd_type;
      break;
#ifdef DDS_HAS_TYPE_DISCOVERY
    case DDSI_ENTITYID_TL_SVC_BUILTIN_REQUEST_WRITER:
      type = gv->tl_svc_request_type;
      break;
    case DDSI_ENTITYID_TL_SVC_BUILTIN_REPLY_WRITER:
      type = gv->tl_svc_reply_type;
      break;
#endif
#ifdef DDS_HAS_SECURITY
    case DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER:
      type = gv->spdp_secure_type;
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER:
      type = gv->sedp_writer_secure_type;
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER:
      type = gv->sedp_reader_secure_type;
      break;
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_WRITER:
      type = gv->pmd_secure_type;
      break;
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_WRITER:
      type = gv->pgm_stateless_type;
      break;
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER:
      type = gv->pgm_volatile_type;
      break;
#endif
    default:
      type = NULL;
      break;
  }
  if (type == NULL)
  {
    /* unrecognized source entity id => ignore */
    goto done_upd_deliv;
  }

  struct ddsi_serdata *d;
  if (data_smhdr_flags & DDSI_DATA_FLAG_DATAFLAG)
    d = ddsi_serdata_from_ser (type, SDK_DATA, fragchain, sampleinfo->size);
  else if (data_smhdr_flags & DDSI_DATA_FLAG_KEYFLAG)
    d = ddsi_serdata_from_ser (type, SDK_KEY, fragchain, sampleinfo->size);
  else if ((qos.present & PP_KEYHASH) && !DDSI_SC_STRICT_P(gv->config))
    d = ddsi_serdata_from_keyhash (type, &qos.keyhash);
  else
  {
    GVLOGDISC ("data(builtin, vendor %u.%u): "PGUIDFMT" #%"PRIu64": missing payload\n",
               sampleinfo->rst->vendor.id[0], sampleinfo->rst->vendor.id[1],
               PGUID (srcguid), sampleinfo->seq);
    goto done_upd_deliv;
  }
  if (d == NULL)
  {
    GVLOG (DDS_LC_DISCOVERY | DDS_LC_WARNING, "data(builtin, vendor %u.%u): "PGUIDFMT" #%"PRIu64": deserialization failed\n",
           sampleinfo->rst->vendor.id[0], sampleinfo->rst->vendor.id[1],
           PGUID (srcguid), sampleinfo->seq);
    goto done_upd_deliv;
  }

  d->timestamp = (sampleinfo->timestamp.v != DDSRT_WCTIME_INVALID.v) ? sampleinfo->timestamp : ddsrt_time_wallclock ();
  d->statusinfo = statusinfo;
  // set protocol version & vendor id for plist types
  // FIXME: find a better way then fixing these up afterward
  if (d->ops == &ddsi_serdata_ops_plist)
  {
    struct ddsi_serdata_plist *d_plist = (struct ddsi_serdata_plist *) d;
    d_plist->protoversion = sampleinfo->rst->protocol_version;
    d_plist->vendorid = sampleinfo->rst->vendor;
  }

  if (gv->logconfig.c.mask & DDS_LC_TRACE)
  {
    ddsi_guid_t guid;
    char tmp[2048];
    size_t res = 0;
    tmp[0] = 0;
    if (gv->logconfig.c.mask & DDS_LC_CONTENT)
      res = ddsi_serdata_print (d, tmp, sizeof (tmp));
    if (pwr) guid = pwr->e.guid; else memset (&guid, 0, sizeof (guid));
    GVTRACE ("data(builtin, vendor %u.%u): "PGUIDFMT" #%"PRIu64": ST%x %s/%s:%s%s\n",
             sampleinfo->rst->vendor.id[0], sampleinfo->rst->vendor.id[1],
             PGUID (guid), sampleinfo->seq, statusinfo,
             pwr ? pwr->c.xqos->topic_name : "", d->type->type_name,
             tmp, res < sizeof (tmp) - 1 ? "" : "(trunc)");
  }

  switch (srcguid.entityid.u)
  {
    case DDSI_ENTITYID_SPDP_BUILTIN_PARTICIPANT_WRITER:
    case DDSI_ENTITYID_SPDP_RELIABLE_BUILTIN_PARTICIPANT_SECURE_WRITER:
      handle_spdp (sampleinfo->rst, srcguid.entityid, sampleinfo->seq, d);
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_WRITER:
    case DDSI_ENTITYID_SEDP_BUILTIN_PUBLICATIONS_SECURE_WRITER:
      handle_sedp (sampleinfo->rst, sampleinfo->seq, d, SEDP_KIND_WRITER);
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_WRITER:
    case DDSI_ENTITYID_SEDP_BUILTIN_SUBSCRIPTIONS_SECURE_WRITER:
      handle_sedp (sampleinfo->rst, sampleinfo->seq, d, SEDP_KIND_READER);
      break;
    case DDSI_ENTITYID_SEDP_BUILTIN_TOPIC_WRITER:
      handle_sedp (sampleinfo->rst, sampleinfo->seq, d, SEDP_KIND_TOPIC);
      break;
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_WRITER:
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_MESSAGE_SECURE_WRITER:
      ddsi_handle_pmd_message (sampleinfo->rst, d);
      break;
#ifdef DDS_HAS_TYPE_DISCOVERY
    case DDSI_ENTITYID_TL_SVC_BUILTIN_REQUEST_WRITER:
    case DDSI_ENTITYID_TL_SVC_BUILTIN_REPLY_WRITER:
      handle_typelookup (sampleinfo->rst, srcguid.entityid, d);
      break;
#endif
#ifdef DDS_HAS_SECURITY
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_STATELESS_MESSAGE_WRITER:
      ddsi_handle_auth_handshake_message(sampleinfo->rst, srcguid.entityid, d);
      break;
    case DDSI_ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER:
      ddsi_handle_crypto_exchange_message(sampleinfo->rst, d);
      break;
#endif
    default:
      GVLOGDISC ("data(builtin, vendor %u.%u): "PGUIDFMT" #%"PRIu64": not handled\n",
                 sampleinfo->rst->vendor.id[0], sampleinfo->rst->vendor.id[1],
                 PGUID (srcguid), sampleinfo->seq);
      break;
  }

  ddsi_serdata_unref (d);

 done_upd_deliv:
  if (pwr)
  {
    /* No proxy writer for SPDP */
    ddsrt_atomic_st32 (&pwr->next_deliv_seq_lowword, (uint32_t) (sampleinfo->seq + 1));
  }
  return 0;
}
