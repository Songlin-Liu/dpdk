/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2014-2020 Broadcom
 * All rights reserved.
 */

#include "bnxt.h"
#include "bnxt_tf_common.h"
#include "ulp_rte_parser.h"
#include "ulp_matcher.h"
#include "ulp_flow_db.h"
#include "ulp_mapper.h"
#include <rte_malloc.h>

static int32_t
bnxt_ulp_flow_validate_args(const struct rte_flow_attr *attr,
			    const struct rte_flow_item pattern[],
			    const struct rte_flow_action actions[],
			    struct rte_flow_error *error)
{
	/* Perform the validation of the arguments for null */
	if (!error)
		return BNXT_TF_RC_ERROR;

	if (!pattern) {
		rte_flow_error_set(error,
				   EINVAL,
				   RTE_FLOW_ERROR_TYPE_ITEM_NUM,
				   NULL,
				   "NULL pattern.");
		return BNXT_TF_RC_ERROR;
	}

	if (!actions) {
		rte_flow_error_set(error,
				   EINVAL,
				   RTE_FLOW_ERROR_TYPE_ACTION_NUM,
				   NULL,
				   "NULL action.");
		return BNXT_TF_RC_ERROR;
	}

	if (!attr) {
		rte_flow_error_set(error,
				   EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR,
				   NULL,
				   "NULL attribute.");
		return BNXT_TF_RC_ERROR;
	}

	if (attr->egress && attr->ingress) {
		rte_flow_error_set(error,
				   EINVAL,
				   RTE_FLOW_ERROR_TYPE_ATTR,
				   attr,
				   "EGRESS AND INGRESS UNSUPPORTED");
		return BNXT_TF_RC_ERROR;
	}
	return BNXT_TF_RC_SUCCESS;
}

/* Function to create the rte flow. */
static struct rte_flow *
bnxt_ulp_flow_create(struct rte_eth_dev			*dev,
		     const struct rte_flow_attr		*attr,
		     const struct rte_flow_item		pattern[],
		     const struct rte_flow_action	actions[],
		     struct rte_flow_error		*error)
{
	struct ulp_rte_hdr_bitmap hdr_bitmap;
	struct ulp_rte_hdr_field hdr_field[BNXT_ULP_PROTO_HDR_MAX];
	struct ulp_rte_act_bitmap act_bitmap;
	struct ulp_rte_act_prop act_prop;
	enum ulp_direction_type dir = ULP_DIR_INGRESS;
	struct bnxt_ulp_context *ulp_ctx = NULL;
	uint32_t class_id, act_tmpl;
	struct rte_flow *flow_id;
	uint32_t app_priority;
	uint32_t fid;
	uint8_t	*buffer;
	uint32_t vnic;
	int ret;

	if (bnxt_ulp_flow_validate_args(attr,
					pattern, actions,
					error) == BNXT_TF_RC_ERROR) {
		BNXT_TF_DBG(ERR, "Invalid arguments being passed\n");
		return NULL;
	}

	ulp_ctx = bnxt_ulp_eth_dev_ptr2_cntxt_get(dev);
	if (!ulp_ctx) {
		BNXT_TF_DBG(ERR, "ULP context is not initialized\n");
		return NULL;
	}

	/* clear the header bitmap and field structure */
	memset(&hdr_bitmap, 0, sizeof(struct ulp_rte_hdr_bitmap));
	memset(hdr_field, 0, sizeof(hdr_field));
	memset(&act_bitmap, 0, sizeof(act_bitmap));
	memset(&act_prop, 0, sizeof(act_prop));

	if (attr->egress)
		dir = ULP_DIR_EGRESS;

	/* copy the device port id and direction in svif for further process */
	buffer = hdr_field[BNXT_ULP_HDR_FIELD_SVIF_INDEX].spec;
	rte_memcpy(buffer, &dev->data->port_id, sizeof(uint16_t));
	rte_memcpy(buffer + sizeof(uint16_t), &dir, sizeof(uint32_t));

	/* Set the implicit vnic in the action property */
	vnic = (uint32_t)bnxt_get_vnic_id(dev->data->port_id);
	vnic = htonl(vnic);
	rte_memcpy(&act_prop.act_details[BNXT_ULP_ACT_PROP_IDX_VNIC],
		   &vnic, BNXT_ULP_ACT_PROP_SZ_VNIC);

	/* Parse the rte flow pattern */
	ret = bnxt_ulp_rte_parser_hdr_parse(pattern,
					    &hdr_bitmap,
					    hdr_field);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	/* Parse the rte flow action */
	ret = bnxt_ulp_rte_parser_act_parse(actions,
					    &act_bitmap,
					    &act_prop);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	ret = ulp_matcher_pattern_match(dir, &hdr_bitmap, hdr_field,
					&act_bitmap, &class_id);

	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	ret = ulp_matcher_action_match(dir, &act_bitmap, &act_tmpl);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	app_priority = attr->priority;
	/* call the ulp mapper to create the flow in the hardware */
	ret = ulp_mapper_flow_create(ulp_ctx,
				     app_priority,
				     &hdr_bitmap,
				     hdr_field,
				     &act_bitmap,
				     &act_prop,
				     class_id,
				     act_tmpl,
				     &fid);
	if (!ret) {
		flow_id = (struct rte_flow *)((uintptr_t)fid);
		return flow_id;
	}

parse_error:
	rte_flow_error_set(error, ret, RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
			   "Failed to create flow.");
	return NULL;
}

/* Function to validate the rte flow. */
static int
bnxt_ulp_flow_validate(struct rte_eth_dev *dev __rte_unused,
		       const struct rte_flow_attr *attr,
		       const struct rte_flow_item pattern[],
		       const struct rte_flow_action actions[],
		       struct rte_flow_error *error)
{
	struct ulp_rte_hdr_bitmap hdr_bitmap;
	struct ulp_rte_hdr_field hdr_field[BNXT_ULP_PROTO_HDR_MAX];
	struct ulp_rte_act_bitmap act_bitmap;
	struct ulp_rte_act_prop act_prop;
	enum ulp_direction_type dir = ULP_DIR_INGRESS;
	uint32_t class_id, act_tmpl;
	int ret;

	if (bnxt_ulp_flow_validate_args(attr,
					pattern, actions,
					error) == BNXT_TF_RC_ERROR) {
		BNXT_TF_DBG(ERR, "Invalid arguments being passed\n");
		return -EINVAL;
	}

	/* clear the header bitmap and field structure */
	memset(&hdr_bitmap, 0, sizeof(struct ulp_rte_hdr_bitmap));
	memset(hdr_field, 0, sizeof(hdr_field));
	memset(&act_bitmap, 0, sizeof(act_bitmap));
	memset(&act_prop, 0, sizeof(act_prop));

	/* Parse the rte flow pattern */
	ret = bnxt_ulp_rte_parser_hdr_parse(pattern,
					    &hdr_bitmap,
					    hdr_field);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	/* Parse the rte flow action */
	ret = bnxt_ulp_rte_parser_act_parse(actions,
					    &act_bitmap,
					    &act_prop);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	if (attr->egress)
		dir = ULP_DIR_EGRESS;

	ret = ulp_matcher_pattern_match(dir, &hdr_bitmap, hdr_field,
					&act_bitmap, &class_id);

	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	ret = ulp_matcher_action_match(dir, &act_bitmap, &act_tmpl);
	if (ret != BNXT_TF_RC_SUCCESS)
		goto parse_error;

	/* all good return success */
	return ret;

parse_error:
	rte_flow_error_set(error, ret, RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
			   "Failed to validate flow.");
	return -EINVAL;
}

/* Function to destroy the rte flow. */
static int
bnxt_ulp_flow_destroy(struct rte_eth_dev *dev,
		      struct rte_flow *flow,
		      struct rte_flow_error *error)
{
	int ret = 0;
	struct bnxt_ulp_context *ulp_ctx;
	uint32_t fid;

	ulp_ctx = bnxt_ulp_eth_dev_ptr2_cntxt_get(dev);
	if (!ulp_ctx) {
		BNXT_TF_DBG(ERR, "ULP context is not initialized\n");
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
				   "Failed to destroy flow.");
		return -EINVAL;
	}

	fid = (uint32_t)(uintptr_t)flow;

	ret = ulp_mapper_flow_destroy(ulp_ctx, fid);
	if (ret)
		rte_flow_error_set(error, -ret,
				   RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
				   "Failed to destroy flow.");

	return ret;
}

/* Function to destroy the rte flows. */
static int32_t
bnxt_ulp_flow_flush(struct rte_eth_dev *eth_dev,
		    struct rte_flow_error *error)
{
	struct bnxt_ulp_context *ulp_ctx;
	int32_t ret;
	struct bnxt *bp;

	ulp_ctx = bnxt_ulp_eth_dev_ptr2_cntxt_get(eth_dev);
	if (!ulp_ctx) {
		BNXT_TF_DBG(ERR, "ULP context is not initialized\n");
		rte_flow_error_set(error, EINVAL,
				   RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
				   "Failed to flush flow.");
		return -EINVAL;
	}
	bp = eth_dev->data->dev_private;

	/* Free the resources for the last device */
	if (!ulp_ctx_deinit_allowed(bp))
		return 0;

	ret = ulp_flow_db_flush_flows(ulp_ctx, BNXT_ULP_REGULAR_FLOW_TABLE);
	if (ret)
		rte_flow_error_set(error, ret,
				   RTE_FLOW_ERROR_TYPE_HANDLE, NULL,
				   "Failed to flush flow.");
	return ret;
}

const struct rte_flow_ops bnxt_ulp_rte_flow_ops = {
	.validate = bnxt_ulp_flow_validate,
	.create = bnxt_ulp_flow_create,
	.destroy = bnxt_ulp_flow_destroy,
	.flush = bnxt_ulp_flow_flush,
	.query = NULL,
	.isolate = NULL
};
