/******************************************************************************
 *
 *  Copyright (C) 2003-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the GATT client discovery procedures and cache
 *  related functions.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_gattc"

#include "bt_target.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "bt_common.h"
#include "bta_gattc_int.h"
#include "bta_sys.h"
#include "btm_api.h"
#include "btm_ble_api.h"
#include "btm_int.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "sdp_api.h"
#include "sdpdefs.h"
#include "utl.h"

using bluetooth::Uuid;

static void bta_gattc_cache_write(const RawAddress& server_bda,
                                  uint16_t num_attr, tBTA_GATTC_NV_ATTR* attr);
static void bta_gattc_char_dscpt_disc_cmpl(uint16_t conn_id,
                                           tBTA_GATTC_SERV* p_srvc_cb);
static tGATT_STATUS bta_gattc_sdp_service_disc(uint16_t conn_id,
                                               tBTA_GATTC_SERV* p_server_cb);
tBTA_GATTC_SERVICE* bta_gattc_find_matching_service(
    std::list<tBTA_GATTC_SERVICE>& services, uint16_t handle);
const tBTA_GATTC_DESCRIPTOR* bta_gattc_get_descriptor_srcb(
    tBTA_GATTC_SERV* p_srcb, uint16_t handle);
tBTA_GATTC_CHARACTERISTIC* bta_gattc_get_characteristic_srcb(
    tBTA_GATTC_SERV* p_srcb, uint16_t handle);

#define BTA_GATT_SDP_DB_SIZE 4096

#define GATT_CACHE_PREFIX "/data/misc/bluetooth/gatt_cache_"
#define GATT_CACHE_VERSION 2

static void bta_gattc_generate_cache_file_name(char* buffer, size_t buffer_len,
                                               const RawAddress& bda) {
  snprintf(buffer, buffer_len, "%s%02x%02x%02x%02x%02x%02x", GATT_CACHE_PREFIX,
           bda.address[0], bda.address[1], bda.address[2], bda.address[3],
           bda.address[4], bda.address[5]);
}

/*****************************************************************************
 *  Constants and data types
 ****************************************************************************/

typedef struct {
  tSDP_DISCOVERY_DB* p_sdp_db;
  uint16_t sdp_conn_id;
} tBTA_GATTC_CB_DATA;

#if (BTA_GATT_DEBUG == TRUE)
static const char* bta_gattc_attr_type[] = {
    "I", /* Included Service */
    "C", /* Characteristic */
    "D"  /* Characteristic Descriptor */
};
/* utility functions */

/* debug function to display the server cache */
static void bta_gattc_display_cache_server(
    const std::list<tBTA_GATTC_SERVICE>& cache) {
  APPL_TRACE_ERROR("<================Start Server Cache =============>");

  for (const tBTA_GATTC_SERVICE& service : cache) {
    APPL_TRACE_ERROR("Service: handle[%d ~ %d] %s inst[%d]", service.s_handle,
                     service.e_handle, service.uuid.ToString().c_str(),
                     service.handle);

    if (service.characteristics.empty()) {
      APPL_TRACE_ERROR("\t No characteristics");
      continue;
    }

    for (const tBTA_GATTC_CHARACTERISTIC& c : service.characteristics) {
      APPL_TRACE_ERROR("\t Characteristic handle[%d] uuid[%s]  prop[0x%1x]",
                       c.handle, c.uuid.ToString().c_str(), c.properties);

      if (c.descriptors.empty()) {
        APPL_TRACE_ERROR("\t\t No descriptors");
        continue;
      }

      for (const tBTA_GATTC_DESCRIPTOR& d : c.descriptors) {
        APPL_TRACE_ERROR("\t\t Descriptor handle[%d] uuid[%s]", d.handle,
                         d.uuid.ToString().c_str());
      }
    }
  }

  APPL_TRACE_ERROR("<================End Server Cache =============>");
  APPL_TRACE_ERROR(" ");
}

/*******************************************************************************
 *
 * Function         bta_gattc_display_explore_record
 *
 * Description      debug function to display the exploration list
 *
 * Returns          none.
 *
 ******************************************************************************/
static void bta_gattc_display_explore_record(tBTA_GATTC_ATTR_REC* p_rec,
                                             uint8_t num_rec) {
  uint8_t i;
  tBTA_GATTC_ATTR_REC* pp = p_rec;

  APPL_TRACE_ERROR("<================Start Explore Queue =============>");
  for (i = 0; i < num_rec; i++, pp++) {
    APPL_TRACE_ERROR(
        "\t rec[%d] uuid[%s] s_handle[%d] e_handle[%d] is_primary[%d]", i + 1,
        pp->uuid.ToString().c_str(), pp->s_handle, pp->e_handle,
        pp->is_primary);
  }
  APPL_TRACE_ERROR("<================ End Explore Queue =============>");
  APPL_TRACE_ERROR(" ");
}
#endif /* BTA_GATT_DEBUG == TRUE */

/*******************************************************************************
 *
 * Function         bta_gattc_init_cache
 *
 * Description      Initialize the database cache and discovery related
 *                  resources.
 *
 * Returns          status
 *
 ******************************************************************************/
tGATT_STATUS bta_gattc_init_cache(tBTA_GATTC_SERV* p_srvc_cb) {
  p_srvc_cb->srvc_cache.clear();

  osi_free(p_srvc_cb->p_srvc_list);
  p_srvc_cb->p_srvc_list =
      (tBTA_GATTC_ATTR_REC*)osi_malloc(BTA_GATTC_ATTR_LIST_SIZE);
  p_srvc_cb->total_srvc = 0;
  p_srvc_cb->cur_srvc_idx = 0;
  p_srvc_cb->cur_char_idx = 0;
  p_srvc_cb->next_avail_idx = 0;

  return GATT_SUCCESS;
}

/** Add a service into database cache */
static void bta_gattc_add_srvc_to_cache(tBTA_GATTC_SERV* p_srvc_cb,
                                        uint16_t s_handle, uint16_t e_handle,
                                        const Uuid& uuid, bool is_primary) {
#if (BTA_GATT_DEBUG == TRUE)
  APPL_TRACE_DEBUG("Add a service into Service");
#endif

  p_srvc_cb->srvc_cache.emplace_back(tBTA_GATTC_SERVICE{
      .s_handle = s_handle,
      .e_handle = e_handle,
      .is_primary = is_primary,
      .uuid = uuid,
      .handle = s_handle,
  });
}

static void bta_gattc_add_char_to_cache(tBTA_GATTC_SERV* p_srvc_cb,
                                        uint16_t attr_handle,
                                        uint16_t value_handle, const Uuid& uuid,
                                        uint8_t property) {
#if (BTA_GATT_DEBUG == TRUE)
  APPL_TRACE_DEBUG("%s: Add a characteristic into Service", __func__);
  APPL_TRACE_DEBUG("handle=%d uuid16=%s property=0x%x", value_handle,
                   uuid.ToString().c_str(), property);
#endif

  tBTA_GATTC_SERVICE* service =
      bta_gattc_find_matching_service(p_srvc_cb->srvc_cache, attr_handle);
  if (!service) {
    APPL_TRACE_ERROR(
        "Illegal action to add char/descr/incl srvc for non-existing service!");
    return;
  }

  /* TODO(jpawlowski): We should use attribute handle, not value handle to refer
     to characteristic.
     This is just a temporary workaround.
  */
  if (service->e_handle < value_handle) service->e_handle = value_handle;

  service->characteristics.emplace_back(
      tBTA_GATTC_CHARACTERISTIC{.handle = value_handle,
                                .properties = property,
                                .uuid = uuid,
                                .service = service});
  return;
}

/* Add an attribute into database cache buffer */
static void bta_gattc_add_attr_to_cache(tBTA_GATTC_SERV* p_srvc_cb,
                                        uint16_t handle, const Uuid& uuid,
                                        uint8_t property,
                                        uint16_t incl_srvc_s_handle,
                                        tBTA_GATTC_ATTR_TYPE type) {
#if (BTA_GATT_DEBUG == TRUE)
  APPL_TRACE_DEBUG("%s: Add a [%s] into Service", __func__,
                   bta_gattc_attr_type[type]);
  APPL_TRACE_DEBUG("handle=%d uuid=%s property=0x%x type=%d", handle,
                   uuid.ToString().c_str(), property, type);
#endif

  tBTA_GATTC_SERVICE* service =
      bta_gattc_find_matching_service(p_srvc_cb->srvc_cache, handle);
  if (!service) {
    APPL_TRACE_ERROR(
        "Illegal action to add char/descr/incl srvc for non-existing service!");
    return;
  }

  if (type == BTA_GATTC_ATTR_TYPE_INCL_SRVC) {
    tBTA_GATTC_SERVICE* included_service = bta_gattc_find_matching_service(
        p_srvc_cb->srvc_cache, incl_srvc_s_handle);
    if (!included_service) {
      APPL_TRACE_ERROR(
          "%s: Illegal action to add non-existing included service!", __func__);
      return;
    }

    service->included_svc.emplace_back(tBTA_GATTC_INCLUDED_SVC{
        .handle = handle,
        .uuid = uuid,
        .owning_service = service,
        .included_service = included_service,
    });
  } else if (type == BTA_GATTC_ATTR_TYPE_CHAR_DESCR) {
    if (service->characteristics.empty()) {
      APPL_TRACE_ERROR(
          "%s: Illegal action to add descriptor before adding a "
          "characteristic!",
          __func__);
      return;
    }

    tBTA_GATTC_CHARACTERISTIC& char_node = service->characteristics.back();
    char_node.descriptors.emplace_back(tBTA_GATTC_DESCRIPTOR{
        .handle = handle, .uuid = uuid, .characteristic = &char_node,
    });
  }
}

/*******************************************************************************
 *
 * Function         bta_gattc_get_disc_range
 *
 * Description      get discovery stating and ending handle range.
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_get_disc_range(tBTA_GATTC_SERV* p_srvc_cb, uint16_t* p_s_hdl,
                              uint16_t* p_e_hdl, bool is_srvc) {
  tBTA_GATTC_ATTR_REC* p_rec = NULL;

  if (is_srvc) {
    p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx;
    *p_s_hdl = p_rec->s_handle;
  } else {
    p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_char_idx;
    *p_s_hdl = p_rec->s_handle + 1;
  }

  *p_e_hdl = p_rec->e_handle;
#if (BTA_GATT_DEBUG == TRUE)
  APPL_TRACE_DEBUG("discover range [%d ~ %d]", p_rec->s_handle,
                   p_rec->e_handle);
#endif
  return;
}
/*******************************************************************************
 *
 * Function         bta_gattc_discover_pri_service
 *
 * Description      Start primary service discovery
 *
 * Returns          status of the operation.
 *
 ******************************************************************************/
tGATT_STATUS bta_gattc_discover_pri_service(uint16_t conn_id,
                                            tBTA_GATTC_SERV* p_server_cb,
                                            uint8_t disc_type) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
  tGATT_STATUS status = GATT_ERROR;

  if (p_clcb) {
    if (p_clcb->transport == BTA_TRANSPORT_LE)
      status = bta_gattc_discover_procedure(conn_id, p_server_cb, disc_type);
    else
      status = bta_gattc_sdp_service_disc(conn_id, p_server_cb);
  }

  return status;
}
/*******************************************************************************
 *
 * Function         bta_gattc_discover_procedure
 *
 * Description      Start a particular type of discovery procedure on server.
 *
 * Returns          status of the operation.
 *
 ******************************************************************************/
tGATT_STATUS bta_gattc_discover_procedure(uint16_t conn_id,
                                          tBTA_GATTC_SERV* p_server_cb,
                                          uint8_t disc_type) {
  tGATT_DISC_PARAM param;
  bool is_service = true;

  memset(&param, 0, sizeof(tGATT_DISC_PARAM));

  if (disc_type == GATT_DISC_SRVC_ALL || disc_type == GATT_DISC_SRVC_BY_UUID) {
    param.s_handle = 1;
    param.e_handle = 0xFFFF;
  } else {
    if (disc_type == GATT_DISC_CHAR_DSCPT) is_service = false;

    bta_gattc_get_disc_range(p_server_cb, &param.s_handle, &param.e_handle,
                             is_service);

    if (param.s_handle > param.e_handle) {
      return GATT_ERROR;
    }
  }
  return GATTC_Discover(conn_id, disc_type, &param);
}
/*******************************************************************************
 *
 * Function         bta_gattc_start_disc_include_srvc
 *
 * Description      Start discovery for included service
 *
 * Returns          status of the operation.
 *
 ******************************************************************************/
tGATT_STATUS bta_gattc_start_disc_include_srvc(uint16_t conn_id,
                                               tBTA_GATTC_SERV* p_srvc_cb) {
  return bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_INC_SRVC);
}
/*******************************************************************************
 *
 * Function         bta_gattc_start_disc_char
 *
 * Description      Start discovery for characteristic
 *
 * Returns          status of the operation.
 *
 ******************************************************************************/
tGATT_STATUS bta_gattc_start_disc_char(uint16_t conn_id,
                                       tBTA_GATTC_SERV* p_srvc_cb) {
  p_srvc_cb->total_char = 0;

  return bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_CHAR);
}
/*******************************************************************************
 *
 * Function         bta_gattc_start_disc_char_dscp
 *
 * Description      Start discovery for characteristic descriptor
 *
 * Returns          none.
 *
 ******************************************************************************/
void bta_gattc_start_disc_char_dscp(uint16_t conn_id,
                                    tBTA_GATTC_SERV* p_srvc_cb) {
  APPL_TRACE_DEBUG("starting discover characteristics descriptor");

  if (bta_gattc_discover_procedure(conn_id, p_srvc_cb, GATT_DISC_CHAR_DSCPT) !=
      0)
    bta_gattc_char_dscpt_disc_cmpl(conn_id, p_srvc_cb);
}
/*******************************************************************************
 *
 * Function         bta_gattc_explore_srvc
 *
 * Description      process the service discovery complete event
 *
 * Returns          status
 *
 ******************************************************************************/
static void bta_gattc_explore_srvc(uint16_t conn_id,
                                   tBTA_GATTC_SERV* p_srvc_cb) {
  tBTA_GATTC_ATTR_REC* p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx;
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  APPL_TRACE_DEBUG("Start service discovery: srvc_idx = %d",
                   p_srvc_cb->cur_srvc_idx);

  p_srvc_cb->cur_char_idx = p_srvc_cb->next_avail_idx = p_srvc_cb->total_srvc;

  if (p_clcb == NULL) {
    APPL_TRACE_ERROR("unknown connection ID");
    return;
  }
  /* start expore a service if there is service not been explored */
  if (p_srvc_cb->cur_srvc_idx < p_srvc_cb->total_srvc) {
    /* add the first service into cache */
    bta_gattc_add_srvc_to_cache(p_srvc_cb, p_rec->s_handle, p_rec->e_handle,
                                p_rec->uuid, p_rec->is_primary);

    /* start discovering included services */
    bta_gattc_start_disc_include_srvc(conn_id, p_srvc_cb);
    return;
  }
  /* no service found at all, the end of server discovery*/
  LOG_WARN(LOG_TAG, "%s no more services found", __func__);

#if (BTA_GATT_DEBUG == TRUE)
  bta_gattc_display_cache_server(p_srvc_cb->srvc_cache);
#endif
  /* save cache to NV */
  p_clcb->p_srcb->state = BTA_GATTC_SERV_SAVE;

  if (btm_sec_is_a_bonded_dev(p_srvc_cb->server_bda)) {
    bta_gattc_cache_save(p_clcb->p_srcb, p_clcb->bta_conn_id);
  }

  bta_gattc_reset_discover_st(p_clcb->p_srcb, GATT_SUCCESS);
}
/*******************************************************************************
 *
 * Function         bta_gattc_incl_srvc_disc_cmpl
 *
 * Description      process the relationship discovery complete event
 *
 * Returns          status
 *
 ******************************************************************************/
static void bta_gattc_incl_srvc_disc_cmpl(uint16_t conn_id,
                                          tBTA_GATTC_SERV* p_srvc_cb) {
  p_srvc_cb->cur_char_idx = p_srvc_cb->total_srvc;

  /* start discoverying characteristic */
  bta_gattc_start_disc_char(conn_id, p_srvc_cb);
}
/*******************************************************************************
 *
 * Function         bta_gattc_char_disc_cmpl
 *
 * Description      process the characteristic discovery complete event
 *
 * Returns          status
 *
 ******************************************************************************/
static void bta_gattc_char_disc_cmpl(uint16_t conn_id,
                                     tBTA_GATTC_SERV* p_srvc_cb) {
  tBTA_GATTC_ATTR_REC* p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->cur_char_idx;

  /* if there are characteristic needs to be explored */
  if (p_srvc_cb->total_char > 0) {
    /* add the first characteristic into cache */
    bta_gattc_add_char_to_cache(p_srvc_cb, p_rec->char_decl_handle,
                                p_rec->s_handle, p_rec->uuid, p_rec->property);

    /* start discoverying characteristic descriptor , if failed, disc for next
     * char*/
    bta_gattc_start_disc_char_dscp(conn_id, p_srvc_cb);
  } else /* otherwise start with next service */
  {
    p_srvc_cb->cur_srvc_idx++;

    bta_gattc_explore_srvc(conn_id, p_srvc_cb);
  }
}
/*******************************************************************************
 *
 * Function         bta_gattc_char_dscpt_disc_cmpl
 *
 * Description      process the char descriptor discovery complete event
 *
 * Returns          status
 *
 ******************************************************************************/
static void bta_gattc_char_dscpt_disc_cmpl(uint16_t conn_id,
                                           tBTA_GATTC_SERV* p_srvc_cb) {
  tBTA_GATTC_ATTR_REC* p_rec = NULL;

  if (--p_srvc_cb->total_char > 0) {
    p_rec = p_srvc_cb->p_srvc_list + (++p_srvc_cb->cur_char_idx);
    /* add the next characteristic into cache */
    bta_gattc_add_char_to_cache(p_srvc_cb, p_rec->char_decl_handle,
                                p_rec->s_handle, p_rec->uuid, p_rec->property);

    /* start discoverying next characteristic for char descriptor */
    bta_gattc_start_disc_char_dscp(conn_id, p_srvc_cb);
  } else
  /* all characteristic has been explored, start with next service if any */
  {
#if (BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_ERROR("all char has been explored");
#endif
    p_srvc_cb->cur_srvc_idx++;
    bta_gattc_explore_srvc(conn_id, p_srvc_cb);
  }
}

static bool bta_gattc_srvc_in_list(tBTA_GATTC_SERV* p_srvc_cb,
                                   uint16_t s_handle, uint16_t e_handle, Uuid) {
  tBTA_GATTC_ATTR_REC* p_rec = NULL;
  uint8_t i;
  bool exist_srvc = false;

  if (!GATT_HANDLE_IS_VALID(s_handle) || !GATT_HANDLE_IS_VALID(e_handle)) {
    APPL_TRACE_ERROR("invalid included service handle: [0x%04x ~ 0x%04x]",
                     s_handle, e_handle);
    exist_srvc = true;
  } else {
    for (i = 0; i < p_srvc_cb->next_avail_idx; i++) {
      p_rec = p_srvc_cb->p_srvc_list + i;

      /* a new service should not have any overlap with other service handle
       * range */
      if (p_rec->s_handle == s_handle || p_rec->e_handle == e_handle) {
        exist_srvc = true;
        break;
      }
    }
  }
  return exist_srvc;
}
/*******************************************************************************
 *
 * Function         bta_gattc_add_srvc_to_list
 *
 * Description      Add a service into explore pending list
 *
 * Returns          status
 *
 ******************************************************************************/
static tGATT_STATUS bta_gattc_add_srvc_to_list(tBTA_GATTC_SERV* p_srvc_cb,
                                               uint16_t s_handle,
                                               uint16_t e_handle,
                                               const Uuid& uuid,
                                               bool is_primary) {
  tBTA_GATTC_ATTR_REC* p_rec = NULL;
  tGATT_STATUS status = GATT_SUCCESS;

  if (p_srvc_cb->p_srvc_list &&
      p_srvc_cb->next_avail_idx < BTA_GATTC_MAX_CACHE_CHAR) {
    p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->next_avail_idx;

    APPL_TRACE_DEBUG("%s handle=%d, service type=%s", __func__, s_handle,
                     uuid.ToString().c_str());

    p_rec->s_handle = s_handle;
    p_rec->e_handle = e_handle;
    p_rec->is_primary = is_primary;
    p_rec->uuid = uuid;

    p_srvc_cb->total_srvc++;
    p_srvc_cb->next_avail_idx++;
  } else { /* allocate bigger buffer ?? */
    status = GATT_DB_FULL;

    APPL_TRACE_ERROR("service not added, no resources or wrong state");
  }
  return status;
}
/*******************************************************************************
 *
 * Function         bta_gattc_add_char_to_list
 *
 * Description      Add a characteristic into explore pending list
 *
 * Returns          status
 *
 ******************************************************************************/
static tGATT_STATUS bta_gattc_add_char_to_list(tBTA_GATTC_SERV* p_srvc_cb,
                                               uint16_t decl_handle,
                                               uint16_t value_handle,
                                               const Uuid& uuid,
                                               uint8_t property) {
  tBTA_GATTC_ATTR_REC* p_rec = NULL;
  tGATT_STATUS status = GATT_SUCCESS;

  if (p_srvc_cb->p_srvc_list == NULL) {
    APPL_TRACE_ERROR("No service available, unexpected char discovery result");
    status = GATT_INTERNAL_ERROR;
  } else if (p_srvc_cb->next_avail_idx < BTA_GATTC_MAX_CACHE_CHAR) {
    p_rec = p_srvc_cb->p_srvc_list + p_srvc_cb->next_avail_idx;

    p_srvc_cb->total_char++;

    p_rec->s_handle = value_handle;
    p_rec->char_decl_handle = decl_handle;
    p_rec->property = property;
    p_rec->e_handle =
        (p_srvc_cb->p_srvc_list + p_srvc_cb->cur_srvc_idx)->e_handle;
    p_rec->uuid = uuid;

    /* update the endind handle of pervious characteristic if available */
    if (p_srvc_cb->total_char > 1) {
      p_rec -= 1;
      p_rec->e_handle = decl_handle - 1;
    }
    p_srvc_cb->next_avail_idx++;
  } else {
    APPL_TRACE_ERROR("char not added, no resources");
    /* allocate bigger buffer ?? */
    status = GATT_DB_FULL;
  }
  return status;
}

/*******************************************************************************
 *
 * Function         bta_gattc_sdp_callback
 *
 * Description      Process the discovery result from sdp
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_gattc_sdp_callback(uint16_t sdp_status, void* user_data) {
  tSDP_PROTOCOL_ELEM pe;
  tBTA_GATTC_CB_DATA* cb_data = (tBTA_GATTC_CB_DATA*)user_data;
  tBTA_GATTC_SERV* p_srvc_cb = bta_gattc_find_scb_by_cid(cb_data->sdp_conn_id);

  if (((sdp_status == SDP_SUCCESS) || (sdp_status == SDP_DB_FULL)) &&
      p_srvc_cb != NULL) {
    tSDP_DISC_REC* p_sdp_rec = NULL;
    do {
      /* find a service record, report it */
      p_sdp_rec = SDP_FindServiceInDb(cb_data->p_sdp_db, 0, p_sdp_rec);
      if (p_sdp_rec) {
        Uuid service_uuid;
        if (SDP_FindServiceUUIDInRec(p_sdp_rec, &service_uuid)) {
          if (SDP_FindProtocolListElemInRec(p_sdp_rec, UUID_PROTOCOL_ATT,
                                            &pe)) {
            uint16_t start_handle = (uint16_t)pe.params[0];
            uint16_t end_handle = (uint16_t)pe.params[1];

#if (BTA_GATT_DEBUG == TRUE)
            APPL_TRACE_EVENT("Found ATT service [%s] handle[0x%04x ~ 0x%04x]",
                             service_uuid.ToString().c_str(), start_handle,
                             end_handle);
#endif

            if (GATT_HANDLE_IS_VALID(start_handle) &&
                GATT_HANDLE_IS_VALID(end_handle) && p_srvc_cb != NULL) {
              /* discover services result, add services into a service list */
              bta_gattc_add_srvc_to_list(p_srvc_cb, start_handle, end_handle,
                                         service_uuid, true);
            } else {
              APPL_TRACE_ERROR("invalid start_handle = %d end_handle = %d",
                               start_handle, end_handle);
            }
          }
        }
      }
    } while (p_sdp_rec);
  }

  if (p_srvc_cb != NULL) {
    /* start discover primary service */
    bta_gattc_explore_srvc(cb_data->sdp_conn_id, p_srvc_cb);
  } else {
    APPL_TRACE_ERROR("GATT service discovery is done on unknown connection");
  }

  /* both were allocated in bta_gattc_sdp_service_disc */
  osi_free(cb_data->p_sdp_db);
  osi_free(cb_data);
}
/*******************************************************************************
 *
 * Function         bta_gattc_sdp_service_disc
 *
 * Description      Start DSP Service Discovert
 *
 * Returns          void
 *
 ******************************************************************************/
static tGATT_STATUS bta_gattc_sdp_service_disc(uint16_t conn_id,
                                               tBTA_GATTC_SERV* p_server_cb) {
  uint16_t num_attrs = 2;
  uint16_t attr_list[2];

  /*
   * On success, cb_data will be freed inside bta_gattc_sdp_callback,
   * otherwise it will be freed within this function.
   */
  tBTA_GATTC_CB_DATA* cb_data =
      (tBTA_GATTC_CB_DATA*)osi_malloc(sizeof(tBTA_GATTC_CB_DATA));

  cb_data->p_sdp_db = (tSDP_DISCOVERY_DB*)osi_malloc(BTA_GATT_SDP_DB_SIZE);
  attr_list[0] = ATTR_ID_SERVICE_CLASS_ID_LIST;
  attr_list[1] = ATTR_ID_PROTOCOL_DESC_LIST;

  Uuid uuid = Uuid::From16Bit(UUID_PROTOCOL_ATT);
  SDP_InitDiscoveryDb(cb_data->p_sdp_db, BTA_GATT_SDP_DB_SIZE, 1, &uuid,
                      num_attrs, attr_list);

  if (!SDP_ServiceSearchAttributeRequest2(p_server_cb->server_bda,
                                          cb_data->p_sdp_db,
                                          &bta_gattc_sdp_callback, cb_data)) {
    osi_free(cb_data->p_sdp_db);
    osi_free(cb_data);
    return GATT_ERROR;
  }

  cb_data->sdp_conn_id = conn_id;
  return GATT_SUCCESS;
}
/*******************************************************************************
 *
 * Function         bta_gattc_disc_res_cback
 *                  bta_gattc_disc_cmpl_cback
 *
 * Description      callback functions to GATT client stack.
 *
 * Returns          void
 *
 ******************************************************************************/
void bta_gattc_disc_res_cback(uint16_t conn_id, tGATT_DISC_TYPE disc_type,
                              tGATT_DISC_RES* p_data) {
  tBTA_GATTC_SERV* p_srvc_cb = NULL;
  bool pri_srvc;
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  p_srvc_cb = bta_gattc_find_scb_by_cid(conn_id);

  if (p_srvc_cb != NULL && p_clcb != NULL &&
      p_clcb->state == BTA_GATTC_DISCOVER_ST) {
    switch (disc_type) {
      case GATT_DISC_SRVC_ALL:
        /* discover services result, add services into a service list */
        bta_gattc_add_srvc_to_list(
            p_srvc_cb, p_data->handle, p_data->value.group_value.e_handle,
            p_data->value.group_value.service_type, true);

        break;
      case GATT_DISC_SRVC_BY_UUID:
        bta_gattc_add_srvc_to_list(
            p_srvc_cb, p_data->handle, p_data->value.group_value.e_handle,
            p_data->value.group_value.service_type, true);
        break;

      case GATT_DISC_INC_SRVC:
        /* add included service into service list if it's secondary or it never
           showed up
           in the primary service search */
        pri_srvc = bta_gattc_srvc_in_list(
            p_srvc_cb, p_data->value.incl_service.s_handle,
            p_data->value.incl_service.e_handle,
            p_data->value.incl_service.service_type);

        if (!pri_srvc)
          bta_gattc_add_srvc_to_list(
              p_srvc_cb, p_data->value.incl_service.s_handle,
              p_data->value.incl_service.e_handle,
              p_data->value.incl_service.service_type, false);
        /* add into database */
        bta_gattc_add_attr_to_cache(
            p_srvc_cb, p_data->handle, p_data->value.incl_service.service_type,
            pri_srvc, p_data->value.incl_service.s_handle,
            BTA_GATTC_ATTR_TYPE_INCL_SRVC);
        break;

      case GATT_DISC_CHAR:
        /* add char value into database */
        bta_gattc_add_char_to_list(p_srvc_cb, p_data->handle,
                                   p_data->value.dclr_value.val_handle,
                                   p_data->value.dclr_value.char_uuid,
                                   p_data->value.dclr_value.char_prop);
        break;

      case GATT_DISC_CHAR_DSCPT:
        bta_gattc_add_attr_to_cache(p_srvc_cb, p_data->handle, p_data->type, 0,
                                    0 /* incl_srvc_handle */,
                                    BTA_GATTC_ATTR_TYPE_CHAR_DESCR);
        break;
    }
  }
}

void bta_gattc_disc_cmpl_cback(uint16_t conn_id, tGATT_DISC_TYPE disc_type,
                               tGATT_STATUS status) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  if (p_clcb && (status != GATT_SUCCESS || p_clcb->status != GATT_SUCCESS)) {
    if (status == GATT_SUCCESS) p_clcb->status = status;
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_DISCOVER_CMPL_EVT, NULL);
    return;
  }

  tBTA_GATTC_SERV* p_srvc_cb = bta_gattc_find_scb_by_cid(conn_id);
  if (!p_srvc_cb) return;

  switch (disc_type) {
    case GATT_DISC_SRVC_ALL:
    case GATT_DISC_SRVC_BY_UUID:
#if (BTA_GATT_DEBUG == TRUE)
      bta_gattc_display_explore_record(p_srvc_cb->p_srvc_list,
                                       p_srvc_cb->next_avail_idx);
#endif
      bta_gattc_explore_srvc(conn_id, p_srvc_cb);
      break;

    case GATT_DISC_INC_SRVC:
      bta_gattc_incl_srvc_disc_cmpl(conn_id, p_srvc_cb);
      break;

    case GATT_DISC_CHAR:
#if (BTA_GATT_DEBUG == TRUE)
      bta_gattc_display_explore_record(p_srvc_cb->p_srvc_list,
                                       p_srvc_cb->next_avail_idx);
#endif
      bta_gattc_char_disc_cmpl(conn_id, p_srvc_cb);
      break;

    case GATT_DISC_CHAR_DSCPT:
      bta_gattc_char_dscpt_disc_cmpl(conn_id, p_srvc_cb);
      break;
  }
}

/*******************************************************************************
 *
 * Function         bta_gattc_search_service
 *
 * Description      search local cache for matching service record.
 *
 * Returns          false if map can not be found.
 *
 ******************************************************************************/
void bta_gattc_search_service(tBTA_GATTC_CLCB* p_clcb, Uuid* p_uuid) {
  for (const tBTA_GATTC_SERVICE& service : p_clcb->p_srcb->srvc_cache) {
    if (p_uuid && *p_uuid != service.uuid) continue;

#if (BTA_GATT_DEBUG == TRUE)
    APPL_TRACE_DEBUG("found service %s, inst[%d] handle [%d]",
                     service.uuid.ToString().c_str(), service.handle,
                     service.s_handle);
#endif
    if (!p_clcb->p_rcb->p_cback) continue;

    tBTA_GATTC cb_data;
    memset(&cb_data, 0, sizeof(tBTA_GATTC));
    cb_data.srvc_res.conn_id = p_clcb->bta_conn_id;
    cb_data.srvc_res.service_uuid.inst_id = service.handle;
    cb_data.srvc_res.service_uuid.uuid = service.uuid;

    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_SEARCH_RES_EVT, &cb_data);
  }
}

std::list<tBTA_GATTC_SERVICE>* bta_gattc_get_services_srcb(
    tBTA_GATTC_SERV* p_srcb) {
  if (!p_srcb || p_srcb->srvc_cache.empty()) return NULL;

  return &p_srcb->srvc_cache;
}

std::list<tBTA_GATTC_SERVICE>* bta_gattc_get_services(uint16_t conn_id) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  if (p_clcb == NULL) return NULL;

  tBTA_GATTC_SERV* p_srcb = p_clcb->p_srcb;

  return bta_gattc_get_services_srcb(p_srcb);
}

tBTA_GATTC_SERVICE* bta_gattc_find_matching_service(
    std::list<tBTA_GATTC_SERVICE>& services, uint16_t handle) {
  for (tBTA_GATTC_SERVICE& service : services) {
    if (handle >= service.s_handle && handle <= service.e_handle)
      return &service;
  }

  return NULL;
}

static tBTA_GATTC_SERVICE* bta_gattc_get_service_for_handle_srcb(
    tBTA_GATTC_SERV* p_srcb, uint16_t handle) {
  std::list<tBTA_GATTC_SERVICE>* services = bta_gattc_get_services_srcb(p_srcb);
  if (services == NULL) return NULL;
  return bta_gattc_find_matching_service(*services, handle);
}

const tBTA_GATTC_SERVICE* bta_gattc_get_service_for_handle(uint16_t conn_id,
                                                           uint16_t handle) {
  std::list<tBTA_GATTC_SERVICE>* services = bta_gattc_get_services(conn_id);
  if (services == NULL) return NULL;

  return bta_gattc_find_matching_service(*services, handle);
}

tBTA_GATTC_CHARACTERISTIC* bta_gattc_get_characteristic_srcb(
    tBTA_GATTC_SERV* p_srcb, uint16_t handle) {
  tBTA_GATTC_SERVICE* service =
      bta_gattc_get_service_for_handle_srcb(p_srcb, handle);

  if (!service) return NULL;

  for (tBTA_GATTC_CHARACTERISTIC& charac : service->characteristics) {
    if (handle == charac.handle) return &charac;
  }

  return NULL;
}

tBTA_GATTC_CHARACTERISTIC* bta_gattc_get_characteristic(uint16_t conn_id,
                                                        uint16_t handle) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  if (p_clcb == NULL) return NULL;

  tBTA_GATTC_SERV* p_srcb = p_clcb->p_srcb;
  return bta_gattc_get_characteristic_srcb(p_srcb, handle);
}

const tBTA_GATTC_DESCRIPTOR* bta_gattc_get_descriptor_srcb(
    tBTA_GATTC_SERV* p_srcb, uint16_t handle) {
  const tBTA_GATTC_SERVICE* service =
      bta_gattc_get_service_for_handle_srcb(p_srcb, handle);

  if (!service) {
    return NULL;
  }

  for (const tBTA_GATTC_CHARACTERISTIC& charac : service->characteristics) {
    for (const tBTA_GATTC_DESCRIPTOR& desc : charac.descriptors) {
      if (handle == desc.handle) return &desc;
    }
  }

  return NULL;
}

const tBTA_GATTC_DESCRIPTOR* bta_gattc_get_descriptor(uint16_t conn_id,
                                                      uint16_t handle) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  if (p_clcb == NULL) return NULL;

  tBTA_GATTC_SERV* p_srcb = p_clcb->p_srcb;
  return bta_gattc_get_descriptor_srcb(p_srcb, handle);
}

/*******************************************************************************
 *
 * Function         bta_gattc_fill_gatt_db_el
 *
 * Description      fill a btgatt_db_element_t value
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_fill_gatt_db_el(btgatt_db_element_t* p_attr,
                               bt_gatt_db_attribute_type_t type,
                               uint16_t att_handle, uint16_t s_handle,
                               uint16_t e_handle, uint16_t id, const Uuid& uuid,
                               uint8_t prop) {
  p_attr->type = type;
  p_attr->attribute_handle = att_handle;
  p_attr->start_handle = s_handle;
  p_attr->end_handle = e_handle;
  p_attr->id = id;
  p_attr->properties = prop;

  // Permissions are not discoverable using the attribute protocol.
  // Core 5.0, Part F, 3.2.5 Attribute Permissions
  p_attr->permissions = 0;
  p_attr->uuid = uuid;
}

/*******************************************************************************
 * Returns          number of elements inside db from start_handle to end_handle
 ******************************************************************************/
static size_t bta_gattc_get_db_size(
    const std::list<tBTA_GATTC_SERVICE>& services, uint16_t start_handle,
    uint16_t end_handle) {
  if (services.empty()) return 0;

  size_t db_size = 0;

  for (const tBTA_GATTC_SERVICE& service : services) {
    if (service.s_handle < start_handle) continue;

    if (service.e_handle > end_handle) break;

    db_size++;

    for (const tBTA_GATTC_CHARACTERISTIC& charac : service.characteristics) {
      db_size++;

      db_size += charac.descriptors.size();
    }

    db_size += service.included_svc.size();
  }

  return db_size;
}

/*******************************************************************************
 *
 * Function         bta_gattc_get_gatt_db_impl
 *
 * Description      copy the server GATT database into db parameter.
 *
 * Parameters       p_srvc_cb: server.
 *                  db: output parameter which will contain GATT database copy.
 *                      Caller is responsible for freeing it.
 *                  count: output parameter which will contain number of
 *                  elements in database.
 *
 * Returns          None.
 *
 ******************************************************************************/
static void bta_gattc_get_gatt_db_impl(tBTA_GATTC_SERV* p_srvc_cb,
                                       uint16_t start_handle,
                                       uint16_t end_handle,
                                       btgatt_db_element_t** db, int* count) {
  APPL_TRACE_DEBUG("%s: start_handle 0x%04x, end_handle 0x%04x", __func__,
                   start_handle, end_handle);

  if (p_srvc_cb->srvc_cache.empty()) {
    *count = 0;
    *db = NULL;
    return;
  }

  size_t db_size =
      bta_gattc_get_db_size(p_srvc_cb->srvc_cache, start_handle, end_handle);

  void* buffer = osi_malloc(db_size * sizeof(btgatt_db_element_t));
  btgatt_db_element_t* curr_db_attr = (btgatt_db_element_t*)buffer;

  for (const tBTA_GATTC_SERVICE& service : p_srvc_cb->srvc_cache) {
    if (service.s_handle < start_handle) continue;

    if (service.e_handle > end_handle) break;

    bta_gattc_fill_gatt_db_el(curr_db_attr,
                              service.is_primary ? BTGATT_DB_PRIMARY_SERVICE
                                                 : BTGATT_DB_SECONDARY_SERVICE,
                              0 /* att_handle */, service.s_handle,
                              service.e_handle, service.s_handle, service.uuid,
                              0 /* prop */);
    curr_db_attr++;

    for (const tBTA_GATTC_CHARACTERISTIC& charac : service.characteristics) {
      bta_gattc_fill_gatt_db_el(curr_db_attr, BTGATT_DB_CHARACTERISTIC,
                                charac.handle, 0 /* s_handle */,
                                0 /* e_handle */, charac.handle, charac.uuid,
                                charac.properties);
      curr_db_attr++;

      for (const tBTA_GATTC_DESCRIPTOR& desc : charac.descriptors) {
        bta_gattc_fill_gatt_db_el(
            curr_db_attr, BTGATT_DB_DESCRIPTOR, desc.handle, 0 /* s_handle */,
            0 /* e_handle */, desc.handle, desc.uuid, 0 /* property */);
        curr_db_attr++;
      }
    }

    for (const tBTA_GATTC_INCLUDED_SVC& p_isvc : service.included_svc) {
      bta_gattc_fill_gatt_db_el(
          curr_db_attr, BTGATT_DB_INCLUDED_SERVICE, p_isvc.handle,
          p_isvc.included_service ? p_isvc.included_service->s_handle : 0,
          0 /* e_handle */, p_isvc.handle, p_isvc.uuid, 0 /* property */);
      curr_db_attr++;
    }
  }

  *db = (btgatt_db_element_t*)buffer;
  *count = db_size;
}

/*******************************************************************************
 *
 * Function         bta_gattc_get_gatt_db
 *
 * Description      copy the server GATT database into db parameter.
 *
 * Parameters       conn_id: connection ID which identify the server.
 *                  db: output parameter which will contain GATT database copy.
 *                      Caller is responsible for freeing it.
 *                  count: number of elements in database.
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_get_gatt_db(uint16_t conn_id, uint16_t start_handle,
                           uint16_t end_handle, btgatt_db_element_t** db,
                           int* count) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  LOG_DEBUG(LOG_TAG, "%s", __func__);
  if (p_clcb == NULL) {
    APPL_TRACE_ERROR("Unknown conn ID: %d", conn_id);
    return;
  }

  if (p_clcb->state != BTA_GATTC_CONN_ST) {
    APPL_TRACE_ERROR("server cache not available, CLCB state = %d",
                     p_clcb->state);
    return;
  }

  if (!p_clcb->p_srcb ||
      p_clcb->p_srcb->p_srvc_list || /* no active discovery */
      p_clcb->p_srcb->srvc_cache.empty()) {
    APPL_TRACE_ERROR("No server cache available");
    return;
  }

  bta_gattc_get_gatt_db_impl(p_clcb->p_srcb, start_handle, end_handle, db,
                             count);
}

/*******************************************************************************
 *
 * Function         bta_gattc_rebuild_cache
 *
 * Description      rebuild server cache from NV cache.
 *
 * Parameters
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_rebuild_cache(tBTA_GATTC_SERV* p_srvc_cb, uint16_t num_attr,
                             tBTA_GATTC_NV_ATTR* p_attr) {
  /* first attribute loading, initialize buffer */
  APPL_TRACE_ERROR("%s: bta_gattc_rebuild_cache", __func__);

  p_srvc_cb->srvc_cache.clear();

  while (num_attr > 0 && p_attr != NULL) {
    switch (p_attr->attr_type) {
      case BTA_GATTC_ATTR_TYPE_SRVC:
        bta_gattc_add_srvc_to_cache(p_srvc_cb, p_attr->s_handle,
                                    p_attr->e_handle, p_attr->uuid,
                                    p_attr->is_primary);
        break;

      case BTA_GATTC_ATTR_TYPE_CHAR:
        // TODO(jpawlowski): store decl_handle properly.
        bta_gattc_add_char_to_cache(p_srvc_cb, p_attr->s_handle,
                                    p_attr->s_handle, p_attr->uuid,
                                    p_attr->prop);
        break;

      case BTA_GATTC_ATTR_TYPE_CHAR_DESCR:
      case BTA_GATTC_ATTR_TYPE_INCL_SRVC:
        bta_gattc_add_attr_to_cache(p_srvc_cb, p_attr->s_handle, p_attr->uuid,
                                    p_attr->prop, p_attr->incl_srvc_handle,
                                    p_attr->attr_type);
        break;
    }
    p_attr++;
    num_attr--;
  }
}

/*******************************************************************************
 *
 * Function         bta_gattc_fill_nv_attr
 *
 * Description      fill a NV attribute entry value
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_fill_nv_attr(tBTA_GATTC_NV_ATTR* p_attr, uint8_t type,
                            uint16_t s_handle, uint16_t e_handle, Uuid uuid,
                            uint8_t prop, uint16_t incl_srvc_handle,
                            bool is_primary) {
  p_attr->s_handle = s_handle;
  p_attr->e_handle = e_handle;
  p_attr->attr_type = type;
  p_attr->is_primary = is_primary;
  p_attr->id = 0;
  p_attr->prop = prop;
  p_attr->incl_srvc_handle = incl_srvc_handle;
  p_attr->uuid = uuid;
}

/*******************************************************************************
 *
 * Function         bta_gattc_cache_save
 *
 * Description      save the server cache into NV
 *
 * Returns          None.
 *
 ******************************************************************************/
void bta_gattc_cache_save(tBTA_GATTC_SERV* p_srvc_cb, uint16_t conn_id) {
  if (p_srvc_cb->srvc_cache.empty()) return;

  int i = 0;
  size_t db_size = bta_gattc_get_db_size(p_srvc_cb->srvc_cache, 0x0000, 0xFFFF);
  tBTA_GATTC_NV_ATTR* nv_attr =
      (tBTA_GATTC_NV_ATTR*)osi_malloc(db_size * sizeof(tBTA_GATTC_NV_ATTR));

  for (const tBTA_GATTC_SERVICE& service : p_srvc_cb->srvc_cache) {
    bta_gattc_fill_nv_attr(&nv_attr[i++], BTA_GATTC_ATTR_TYPE_SRVC,
                           service.s_handle, service.e_handle, service.uuid,
                           0 /* properties */, 0 /* incl_srvc_handle */,
                           service.is_primary);
  }

  for (const tBTA_GATTC_SERVICE& service : p_srvc_cb->srvc_cache) {
    for (const tBTA_GATTC_CHARACTERISTIC& charac : service.characteristics) {
      bta_gattc_fill_nv_attr(&nv_attr[i++], BTA_GATTC_ATTR_TYPE_CHAR,
                             charac.handle, 0, charac.uuid, charac.properties,
                             0 /* incl_srvc_handle */, false);

      for (const tBTA_GATTC_DESCRIPTOR& desc : charac.descriptors) {
        bta_gattc_fill_nv_attr(&nv_attr[i++], BTA_GATTC_ATTR_TYPE_CHAR_DESCR,
                               desc.handle, 0, desc.uuid, 0 /* properties */,
                               0 /* incl_srvc_handle */, false);
      }
    }

    for (const tBTA_GATTC_INCLUDED_SVC& p_isvc : service.included_svc) {
      bta_gattc_fill_nv_attr(&nv_attr[i++], BTA_GATTC_ATTR_TYPE_INCL_SRVC,
                             p_isvc.handle, 0, p_isvc.uuid, 0 /* properties */,
                             p_isvc.included_service->s_handle, false);
    }
  }

  bta_gattc_cache_write(p_srvc_cb->server_bda, db_size, nv_attr);
  osi_free(nv_attr);
}

/*******************************************************************************
 *
 * Function         bta_gattc_cache_load
 *
 * Description      Load GATT cache from storage for server.
 *
 * Parameter        p_clcb: pointer to server clcb, that will
 *                          be filled from storage
 * Returns          true on success, false otherwise
 *
 ******************************************************************************/
bool bta_gattc_cache_load(tBTA_GATTC_CLCB* p_clcb) {
  char fname[255] = {0};
  bta_gattc_generate_cache_file_name(fname, sizeof(fname),
                                     p_clcb->p_srcb->server_bda);

  FILE* fd = fopen(fname, "rb");
  if (!fd) {
    APPL_TRACE_ERROR("%s: can't open GATT cache file %s for reading, error: %s",
                     __func__, fname, strerror(errno));
    return false;
  }

  uint16_t cache_ver = 0;
  tBTA_GATTC_NV_ATTR* attr = NULL;
  bool success = false;
  uint16_t num_attr = 0;

  if (fread(&cache_ver, sizeof(uint16_t), 1, fd) != 1) {
    APPL_TRACE_ERROR("%s: can't read GATT cache version from: %s", __func__,
                     fname);
    goto done;
  }

  if (cache_ver != GATT_CACHE_VERSION) {
    APPL_TRACE_ERROR("%s: wrong GATT cache version: %s", __func__, fname);
    goto done;
  }

  if (fread(&num_attr, sizeof(uint16_t), 1, fd) != 1) {
    APPL_TRACE_ERROR("%s: can't read number of GATT attributes: %s", __func__,
                     fname);
    goto done;
  }

  if (num_attr > 0xFFFF) {
    APPL_TRACE_ERROR("%s: more than 0xFFFF GATT attributes: %s", __func__,
                     fname);
    goto done;
  }

  attr = (tBTA_GATTC_NV_ATTR*)osi_malloc(sizeof(tBTA_GATTC_NV_ATTR) * num_attr);

  if (fread(attr, sizeof(tBTA_GATTC_NV_ATTR), num_attr, fd) != num_attr) {
    APPL_TRACE_ERROR("%s: can't read GATT attributes: %s", __func__, fname);
    goto done;
  }

  bta_gattc_rebuild_cache(p_clcb->p_srcb, num_attr, attr);

  success = true;

done:
  osi_free(attr);
  fclose(fd);
  return success;
}

/*******************************************************************************
 *
 * Function         bta_gattc_cache_write
 *
 * Description      This callout function is executed by GATT when a server
 *                  cache is available to save.
 *
 * Parameter        server_bda: server bd address of this cache belongs to
 *                  num_attr: number of attribute to be save.
 *                  attr: pointer to the list of attributes to save.
 * Returns
 *
 ******************************************************************************/
static void bta_gattc_cache_write(const RawAddress& server_bda,
                                  uint16_t num_attr, tBTA_GATTC_NV_ATTR* attr) {
  char fname[255] = {0};
  bta_gattc_generate_cache_file_name(fname, sizeof(fname), server_bda);

  FILE* fd = fopen(fname, "wb");
  if (!fd) {
    APPL_TRACE_ERROR("%s: can't open GATT cache file for writing: %s", __func__,
                     fname);
    return;
  }

  uint16_t cache_ver = GATT_CACHE_VERSION;
  if (fwrite(&cache_ver, sizeof(uint16_t), 1, fd) != 1) {
    APPL_TRACE_ERROR("%s: can't write GATT cache version: %s", __func__, fname);
    fclose(fd);
    return;
  }

  if (fwrite(&num_attr, sizeof(uint16_t), 1, fd) != 1) {
    APPL_TRACE_ERROR("%s: can't write GATT cache attribute count: %s", __func__,
                     fname);
    fclose(fd);
    return;
  }

  if (fwrite(attr, sizeof(tBTA_GATTC_NV_ATTR), num_attr, fd) != num_attr) {
    APPL_TRACE_ERROR("%s: can't write GATT cache attributes: %s", __func__,
                     fname);
    fclose(fd);
    return;
  }

  fclose(fd);
}

/*******************************************************************************
 *
 * Function         bta_gattc_cache_reset
 *
 * Description      This callout function is executed by GATTC to reset cache in
 *                  application
 *
 * Parameter        server_bda: server bd address of this cache belongs to
 *
 * Returns          void.
 *
 ******************************************************************************/
void bta_gattc_cache_reset(const RawAddress& server_bda) {
  BTIF_TRACE_DEBUG("%s", __func__);
  char fname[255] = {0};
  bta_gattc_generate_cache_file_name(fname, sizeof(fname), server_bda);
  unlink(fname);
}
