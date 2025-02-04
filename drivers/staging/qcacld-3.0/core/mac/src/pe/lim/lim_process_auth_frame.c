/*
 * Copyright (c) 2011-2018 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 *
 * This file lim_process_auth_frame.cc contains the code
 * for processing received Authentication Frame.
 * Author:        Chandra Modumudi
 * Date:          03/11/02
 * History:-
 * Date           Modified by    Modification Information
 * --------------------------------------------------------------------
 * 05/12/2010     js             To support Shared key authentication at AP side
 *
 */

#include "wni_api.h"
#include "wni_cfg.h"
#include "ani_global.h"
#include "cfg_api.h"

#include "utils_api.h"
#include "lim_utils.h"
#include "lim_assoc_utils.h"
#include "lim_security_utils.h"
#include "lim_ser_des_utils.h"
#include "lim_ft.h"
#include "cds_utils.h"
#include "lim_send_messages.h"
#include "lim_process_fils.h"

/**
 * is_auth_valid
 *
 ***FUNCTION:
 * This function is called by lim_process_auth_frame() upon Authentication
 * frame reception.
 *
 ***LOGIC:
 * This function is used to test validity of auth frame:
 * - AUTH1 and AUTH3 must be received in AP mode
 * - AUTH2 and AUTH4 must be received in STA mode
 * - AUTH3 and AUTH4 must have challenge text IE, that is,'type' field has been set to
 *                 SIR_MAC_CHALLENGE_TEXT_EID by parser
 * -
 *
 ***ASSUMPTIONS:
 *
 ***NOTE:
 *
 * @param  *auth - Pointer to extracted auth frame body
 *
 * @return 0 or 1 (Valid)
 */

static inline unsigned int is_auth_valid(tpAniSirGlobal pMac,
					 tpSirMacAuthFrameBody auth,
					 tpPESession sessionEntry)
{
	unsigned int valid = 1;

	if (((auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_1) ||
	    (auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_3)) &&
	    (LIM_IS_STA_ROLE(sessionEntry)))
		valid = 0;

	if (((auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_2) ||
	    (auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_4)) &&
	    (LIM_IS_AP_ROLE(sessionEntry)))
		valid = 0;

	if (((auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_3) ||
	    (auth->authTransactionSeqNumber == SIR_MAC_AUTH_FRAME_4)) &&
	    (auth->type != SIR_MAC_CHALLENGE_TEXT_EID) &&
	    (auth->authAlgoNumber != eSIR_SHARED_KEY))
		valid = 0;

	return valid;
}

static void lim_process_auth_shared_system_algo(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		tSirMacAuthFrameBody *auth_frame,
		tpPESession pe_session)
{
	uint32_t val;
	uint8_t cfg_privacy_opt_imp;
	struct tLimPreAuthNode *auth_node;
	uint8_t challenge_txt_arr[SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH] = {0};

	pe_debug("=======> eSIR_SHARED_KEY");
	if (LIM_IS_AP_ROLE(pe_session))
		val = pe_session->privacy;
	else if (wlan_cfg_get_int(mac_ctx,
			WNI_CFG_PRIVACY_ENABLED, &val) != QDF_STATUS_SUCCESS)
		pe_warn("couldnt retrieve Privacy option");
	cfg_privacy_opt_imp = (uint8_t) val;
	if (!cfg_privacy_opt_imp) {
		pe_err("rx Auth frame for unsupported auth algorithm %d "
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authAlgoNumber,
			MAC_ADDR_ARRAY(mac_hdr->sa));

		/*
		 * Authenticator does not have WEP
		 * implemented.
		 * Reject by sending Authentication frame
		 * with Auth algorithm not supported status
		 * code.
		 */
		auth_frame->authAlgoNumber = rx_auth_frm_body->authAlgoNumber;
		auth_frame->authTransactionSeqNumber =
			rx_auth_frm_body->authTransactionSeqNumber + 1;
		auth_frame->authStatusCode =
			eSIR_MAC_AUTH_ALGO_NOT_SUPPORTED_STATUS;

		lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
		return;
	} else {
		/* Create entry for this STA in pre-auth list */
		auth_node = lim_acquire_free_pre_auth_node(mac_ctx,
					&mac_ctx->lim.gLimPreAuthTimerTable);
		if (auth_node == NULL) {
			pe_warn("Max preauth-nodes reached");
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);
			return;
		}

		qdf_mem_copy((uint8_t *) auth_node->peerMacAddr, mac_hdr->sa,
				sizeof(tSirMacAddr));
		auth_node->mlmState = eLIM_MLM_WT_AUTH_FRAME3_STATE;
		auth_node->authType =
			(tAniAuthType) rx_auth_frm_body->authAlgoNumber;
		auth_node->fSeen = 0;
		auth_node->fTimerStarted = 0;
		auth_node->seq_num = ((mac_hdr->seqControl.seqNumHi << 4) |
						(mac_hdr->seqControl.seqNumLo));
		auth_node->timestamp = qdf_mc_timer_get_system_ticks();
		lim_add_pre_auth_node(mac_ctx, auth_node);

		pe_debug("Alloc new data: %pK id: %d peer ",
			auth_node, auth_node->authNodeIdx);
		lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGD);
		/* / Create and activate Auth Response timer */
		if (tx_timer_change_context(&auth_node->timer,
				auth_node->authNodeIdx) != TX_SUCCESS) {
			/* Could not start Auth response timer. Log error */
			pe_warn("Unable to chg context auth response timer for peer");
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);

			/*
			 * Send Auth frame with unspecified failure status code.
			 */

			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode =
				eSIR_MAC_UNSPEC_FAILURE_STATUS;

			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			lim_delete_pre_auth_node(mac_ctx, mac_hdr->sa);
			return;
		}

		/*
		 * get random bytes and use as challenge text.
		 */
		get_random_bytes(challenge_txt_arr,
				 SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH);
		qdf_mem_zero(auth_node->challengeText,
			     SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH);
		if (!qdf_mem_cmp(challenge_txt_arr,
				 auth_node->challengeText,
				 SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH)) {
			pe_err("Challenge text preparation failed");
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);
			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode = eSIR_MAC_TRY_AGAIN_LATER;
			lim_send_auth_mgmt_frame(mac_ctx,
						 auth_frame,
						 mac_hdr->sa,
						 LIM_NO_WEP_IN_FC,
						 pe_session);
			lim_delete_pre_auth_node(mac_ctx, mac_hdr->sa);
			return;
		}

		lim_activate_auth_rsp_timer(mac_ctx, auth_node);
		auth_node->fTimerStarted = 1;

		qdf_mem_copy(auth_node->challengeText,
			     challenge_txt_arr,
			     sizeof(challenge_txt_arr));
		/*
		 * Sending Authenticaton frame with challenge.
		 */
		auth_frame->authAlgoNumber = rx_auth_frm_body->authAlgoNumber;
		auth_frame->authTransactionSeqNumber =
			rx_auth_frm_body->authTransactionSeqNumber + 1;
		auth_frame->authStatusCode = eSIR_MAC_SUCCESS_STATUS;
		auth_frame->type = SIR_MAC_CHALLENGE_TEXT_EID;
		auth_frame->length = SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH;
		qdf_mem_copy(auth_frame->challengeText,
				auth_node->challengeText,
				SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH);
		lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
	}
}

static void lim_process_auth_open_system_algo(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		tSirMacAuthFrameBody *auth_frame,
		tpPESession pe_session)
{
	struct tLimPreAuthNode *auth_node;

	pe_debug("=======> eSIR_OPEN_SYSTEM");
	/* Create entry for this STA in pre-auth list */
	auth_node = lim_acquire_free_pre_auth_node(mac_ctx,
				&mac_ctx->lim.gLimPreAuthTimerTable);
	if (auth_node == NULL) {
		pe_warn("Max pre-auth nodes reached ");
		lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);
		return;
	}
	pe_debug("Alloc new data: %pK peer", auth_node);
	lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGD);
	qdf_mem_copy((uint8_t *) auth_node->peerMacAddr,
			mac_hdr->sa, sizeof(tSirMacAddr));
	auth_node->mlmState = eLIM_MLM_AUTHENTICATED_STATE;
	auth_node->authType = (tAniAuthType) rx_auth_frm_body->authAlgoNumber;
	auth_node->fSeen = 0;
	auth_node->fTimerStarted = 0;
	auth_node->seq_num = ((mac_hdr->seqControl.seqNumHi << 4) |
				(mac_hdr->seqControl.seqNumLo));
	auth_node->timestamp = qdf_mc_timer_get_system_ticks();
	lim_add_pre_auth_node(mac_ctx, auth_node);
	/*
	 * Send Authenticaton frame with Success
	 * status code.
	 */
	auth_frame->authAlgoNumber = rx_auth_frm_body->authAlgoNumber;
	auth_frame->authTransactionSeqNumber =
			rx_auth_frm_body->authTransactionSeqNumber + 1;
	auth_frame->authStatusCode = eSIR_MAC_SUCCESS_STATUS;
	lim_send_auth_mgmt_frame(mac_ctx, auth_frame, mac_hdr->sa,
					LIM_NO_WEP_IN_FC,
					pe_session);
}

#ifdef WLAN_FEATURE_SAE
/**
 * lim_process_sae_auth_frame()-Process SAE authentication frame
 * @mac_ctx: MAC context
 * @rx_pkt_info: Rx packet
 * @pe_session: PE session
 *
 * Return: None
 */
static void lim_process_sae_auth_frame(tpAniSirGlobal mac_ctx,
		uint8_t *rx_pkt_info, tpPESession pe_session)
{
	tpSirMacMgmtHdr mac_hdr;
	uint32_t frame_len;
	uint8_t *body_ptr;

	mac_hdr = WMA_GET_RX_MAC_HEADER(rx_pkt_info);
	body_ptr = WMA_GET_RX_MPDU_DATA(rx_pkt_info);
	frame_len = WMA_GET_RX_PAYLOAD_LEN(rx_pkt_info);

	pe_nofl_info("Received SAE Auth frame type %d subtype %d",
		     mac_hdr->fc.type, mac_hdr->fc.subType);

	if (pe_session->limMlmState != eLIM_MLM_WT_SAE_AUTH_STATE)
		pe_err("received SAE auth response in unexpected state %x",
				pe_session->limMlmState);

	lim_send_sme_mgmt_frame_ind(mac_ctx, mac_hdr->fc.subType,
			(uint8_t *) mac_hdr,
			frame_len + sizeof(tSirMacMgmtHdr), 0,
			WMA_GET_RX_CH(rx_pkt_info), pe_session,
			WMA_GET_RX_RSSI_NORMALIZED(rx_pkt_info));
}
#else
static inline void  lim_process_sae_auth_frame(tpAniSirGlobal mac_ctx,
		uint8_t *rx_pkt_info, tpPESession pe_session)
{}
#endif

static void lim_process_auth_frame_type1(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		uint8_t *rx_pkt_info, uint16_t curr_seq_num,
		tSirMacAuthFrameBody *auth_frame, tpPESession pe_session)
{
	tpDphHashNode sta_ds_ptr = NULL;
	struct tLimPreAuthNode *auth_node;
	uint32_t maxnum_preauth;
	uint16_t associd = 0;

	/* AuthFrame 1 */
	sta_ds_ptr = dph_lookup_hash_entry(mac_ctx, mac_hdr->sa,
				&associd, &pe_session->dph.dphHashTable);
	if (sta_ds_ptr) {
		tLimMlmDisassocReq *pMlmDisassocReq = NULL;
		tLimMlmDeauthReq *pMlmDeauthReq = NULL;
		bool isConnected = true;

		pMlmDisassocReq =
			mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDisassocReq;
		if (pMlmDisassocReq &&
			(!qdf_mem_cmp((uint8_t *) mac_hdr->sa, (uint8_t *)
				&pMlmDisassocReq->peer_macaddr.bytes,
				QDF_MAC_ADDR_SIZE))) {
			pe_debug("TODO:Ack for disassoc frame is pending Issue delsta for "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(
					pMlmDisassocReq->peer_macaddr.bytes));
			lim_process_disassoc_ack_timeout(mac_ctx);
			isConnected = false;
		}
		pMlmDeauthReq =
			mac_ctx->lim.limDisassocDeauthCnfReq.pMlmDeauthReq;
		if (pMlmDeauthReq &&
			(!qdf_mem_cmp((uint8_t *) mac_hdr->sa, (uint8_t *)
				&pMlmDeauthReq->peer_macaddr.bytes,
				QDF_MAC_ADDR_SIZE))) {
			pe_debug("TODO:Ack for deauth frame is pending Issue delsta for "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(
					pMlmDeauthReq->peer_macaddr.bytes));
			lim_process_deauth_ack_timeout(mac_ctx);
			isConnected = false;
		}

		/*
		 * pStaDS != NULL and isConnected = 1 means the STA is already
		 * connected, But SAP received the Auth from that station.
		 * For non PMF connection send Deauth frame as STA will retry
		 * to connect back. The reason for above logic is captured in
		 * CR620403. If we silently drop the auth, the subsequent EAPOL
		 * exchange will fail & peer STA will keep trying until DUT
		 * SAP/GO gets a kickout event from FW & cleans up.
		 *
		 * For PMF connection the AP should not tear down or otherwise
		 * modify the state of the existing association until the
		 * SA-Query procedure determines that the original SA is
		 * invalid.
		 */
		if (isConnected
#ifdef WLAN_FEATURE_11W
			&& !sta_ds_ptr->rmfEnabled
#endif
		   ) {
			pe_err("STA is already connected but received auth frame"
					"Send the Deauth and lim Delete Station Context"
					"(staId: %d, associd: %d) ",
				sta_ds_ptr->staIndex, associd);
			lim_send_deauth_mgmt_frame(mac_ctx,
				eSIR_MAC_UNSPEC_FAILURE_REASON,
				(uint8_t *) mac_hdr->sa,
				pe_session, false);
			lim_trigger_sta_deletion(mac_ctx, sta_ds_ptr,
				pe_session);
			return;
		}
	}
	/* Check if there exists pre-auth context for this STA */
	auth_node = lim_search_pre_auth_list(mac_ctx, mac_hdr->sa);
	if (auth_node) {
		/* Pre-auth context exists for the STA */
		if (!(mac_hdr->fc.retry == 0 ||
					auth_node->seq_num != curr_seq_num)) {
			/*
			 * This can happen when first authentication frame is
			 * received but ACK lost at STA side, in this case 2nd
			 * auth frame is already in transmission queue
			 */
			pe_warn("STA is initiating Auth after ACK lost");
			return;
		}
		/*
		 * STA is initiating brand-new Authentication
		 * sequence after local Auth Response timeout Or STA
		 * retrying to transmit First Auth frame due to packet
		 * drop OTA Delete Pre-auth node and fall through.
		 */
		if (auth_node->fTimerStarted)
			lim_deactivate_and_change_per_sta_id_timer(
					mac_ctx, eLIM_AUTH_RSP_TIMER,
					auth_node->authNodeIdx);
		pe_debug("STA is initiating brand-new Auth");
		lim_delete_pre_auth_node(mac_ctx, mac_hdr->sa);
		/*
		 *  SAP Mode:Disassociate the station and
		 *  delete its entry if we have its entry
		 *  already and received "auth" from the
		 *  same station.
		 *  SAP dphHashTable.size = 8
		 */
		for (associd = 0; associd < pe_session->dph.dphHashTable.size;
			associd++) {
			sta_ds_ptr = dph_get_hash_entry(mac_ctx, associd,
						&pe_session->dph.dphHashTable);
			if (NULL == sta_ds_ptr)
				continue;
			if (sta_ds_ptr->valid && (!qdf_mem_cmp(
					(uint8_t *)&sta_ds_ptr->staAddr,
					(uint8_t *) &(mac_hdr->sa),
					(uint8_t) sizeof(tSirMacAddr))))
				break;
			sta_ds_ptr = NULL;
		}

		if (NULL != sta_ds_ptr
#ifdef WLAN_FEATURE_11W
			&& !sta_ds_ptr->rmfEnabled
#endif
		   ) {
			pe_debug("lim Delete Station Context staId: %d associd: %d",
				sta_ds_ptr->staIndex, associd);
			lim_send_deauth_mgmt_frame(mac_ctx,
				eSIR_MAC_UNSPEC_FAILURE_REASON,
				(uint8_t *)auth_node->peerMacAddr,
				pe_session, false);
			lim_trigger_sta_deletion(mac_ctx, sta_ds_ptr,
				pe_session);
			return;
		}
	}
	if (wlan_cfg_get_int(mac_ctx, WNI_CFG_MAX_NUM_PRE_AUTH,
				(uint32_t *) &maxnum_preauth) != QDF_STATUS_SUCCESS)
		pe_warn("could not retrieve MaxNumPreAuth");

	if (mac_ctx->lim.gLimNumPreAuthContexts == maxnum_preauth &&
			!lim_delete_open_auth_pre_auth_node(mac_ctx)) {
		pe_err("Max no of preauth context reached");
		/*
		 * Maximum number of pre-auth contexts reached.
		 * Send Authentication frame with unspecified failure
		 */
		auth_frame->authAlgoNumber = rx_auth_frm_body->authAlgoNumber;
		auth_frame->authTransactionSeqNumber =
			rx_auth_frm_body->authTransactionSeqNumber + 1;
		auth_frame->authStatusCode =
			eSIR_MAC_UNSPEC_FAILURE_STATUS;

		lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
		return;
	}
	/* No Pre-auth context exists for the STA. */
	if (lim_is_auth_algo_supported(mac_ctx,
			(tAniAuthType) rx_auth_frm_body->authAlgoNumber,
			pe_session)) {

		if (lim_get_session_by_macaddr(mac_ctx, mac_hdr->sa)) {

			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode =
				eSIR_MAC_WME_INVALID_PARAMS_STATUS;

			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			return;
		}

		switch (rx_auth_frm_body->authAlgoNumber) {
		case eSIR_OPEN_SYSTEM:
			lim_process_auth_open_system_algo(mac_ctx, mac_hdr,
				rx_auth_frm_body, auth_frame, pe_session);
			break;

		case eSIR_SHARED_KEY:
			lim_process_auth_shared_system_algo(mac_ctx, mac_hdr,
				rx_auth_frm_body, auth_frame, pe_session);
			break;
		default:
			pe_err("rx Auth frm for unsupported auth algo %d "
				MAC_ADDRESS_STR,
				rx_auth_frm_body->authAlgoNumber,
				MAC_ADDR_ARRAY(mac_hdr->sa));

			/*
			 * Responding party does not support the
			 * authentication algorithm requested by
			 * sending party.
			 * Reject by sending Authentication frame
			 * with auth algorithm not supported status code
			 */
			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_ALGO_NOT_SUPPORTED_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			return;
		}
	} else {
		pe_err("received Authentication frame for unsupported auth algorithm %d "
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authAlgoNumber,
			MAC_ADDR_ARRAY(mac_hdr->sa));

		/*
		 * Responding party does not support the
		 * authentication algorithm requested by sending party.
		 * Reject Authentication with StatusCode=13.
		 */
		auth_frame->authAlgoNumber = rx_auth_frm_body->authAlgoNumber;
		auth_frame->authTransactionSeqNumber =
			rx_auth_frm_body->authTransactionSeqNumber + 1;
		auth_frame->authStatusCode =
			eSIR_MAC_AUTH_ALGO_NOT_SUPPORTED_STATUS;

		lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa,
				LIM_NO_WEP_IN_FC,
				pe_session);
		return;
	}
}

static void lim_process_auth_frame_type2(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		tSirMacAuthFrameBody *auth_frame,
		uint8_t *plainbody,
		uint8_t *body_ptr, uint16_t frame_len,
		tpPESession pe_session)
{
	uint8_t key_id, cfg_privacy_opt_imp;
	uint32_t val, key_length = 8;
	uint8_t defaultkey[SIR_MAC_KEY_LENGTH];
	struct tLimPreAuthNode *auth_node;
	uint8_t *encr_auth_frame;

	/* AuthFrame 2 */
	if (pe_session->limMlmState != eLIM_MLM_WT_AUTH_FRAME2_STATE) {
		/**
		 * Check if a Reassociation is in progress and this is a
		 * Pre-Auth frame
		 */
		if (LIM_IS_STA_ROLE(pe_session) &&
		    (pe_session->limSmeState == eLIM_SME_WT_REASSOC_STATE) &&
		    (rx_auth_frm_body->authStatusCode ==
				eSIR_MAC_SUCCESS_STATUS) &&
		    (pe_session->ftPEContext.pFTPreAuthReq != NULL) &&
		    (!qdf_mem_cmp(
			pe_session->ftPEContext.pFTPreAuthReq->preAuthbssId,
			mac_hdr->sa, sizeof(tSirMacAddr)))) {

			/* Update the FTIEs in the saved auth response */
			pe_warn("rx PreAuth frm2 in smestate: %d from: %pM",
				pe_session->limSmeState, mac_hdr->sa);
			pe_session->ftPEContext.saved_auth_rsp_length = 0;

			if ((body_ptr != NULL) && (frame_len < MAX_FTIE_SIZE)) {
				qdf_mem_copy(
					pe_session->ftPEContext.saved_auth_rsp,
					body_ptr, frame_len);
				pe_session->ftPEContext.saved_auth_rsp_length =
					frame_len;
			}
		} else {
			/*
			 * Received Auth frame2 in an unexpected state.
			 * Log error and ignore the frame.
			 */
			pe_debug("rx Auth frm2 from peer in state: %d addr",
				pe_session->limMlmState);
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGD);
		}
		return;
	}

	if (qdf_mem_cmp((uint8_t *) mac_hdr->sa,
			(uint8_t *) &mac_ctx->lim.gpLimMlmAuthReq->peerMacAddr,
			sizeof(tSirMacAddr))) {
		/*
		 * Received Authentication frame from an entity
		 * other than one request was initiated.
		 * Wait until Authentication Failure Timeout.
		 */

		pe_warn("received Auth frame2 from unexpected peer"
			MAC_ADDRESS_STR, MAC_ADDR_ARRAY(mac_hdr->sa));
		return;
	}

	if (rx_auth_frm_body->authStatusCode ==
			eSIR_MAC_AUTH_ALGO_NOT_SUPPORTED_STATUS) {
		/*
		 * Interoperability workaround: Linksys WAP4400N is returning
		 * wrong authType in OpenAuth response in case of
		 * SharedKey AP configuration. Pretend we don't see that,
		 * so upper layer can fallback to SharedKey authType,
		 * and successfully connect to the AP.
		 */
		if (rx_auth_frm_body->authAlgoNumber !=
				mac_ctx->lim.gpLimMlmAuthReq->authType) {
			rx_auth_frm_body->authAlgoNumber =
				mac_ctx->lim.gpLimMlmAuthReq->authType;
		}
	}

	if (rx_auth_frm_body->authAlgoNumber !=
			mac_ctx->lim.gpLimMlmAuthReq->authType) {
		/*
		 * Auth algo is open in rx auth frame when auth type is SAE and
		 * PMK is cached as driver sent auth algo as open in tx frame
		 * as well.
		 */
		if ((mac_ctx->lim.gpLimMlmAuthReq->authType ==
		    eSIR_AUTH_TYPE_SAE) && pe_session->sae_pmk_cached) {
			pe_debug("rx Auth frame2 auth algo %d in SAE PMK case",
				rx_auth_frm_body->authAlgoNumber);
		} else {
			/*
			 * Received Authentication frame with an auth
			 * algorithm other than one requested.
			 * Wait until Authentication Failure Timeout.
			 */

			pe_warn("rx Auth frame2 for unexpected auth algo %d"
				MAC_ADDRESS_STR,
				rx_auth_frm_body->authAlgoNumber,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			return;
		}
	}

	if (rx_auth_frm_body->authStatusCode != eSIR_MAC_SUCCESS_STATUS) {
		/*
		 * Authentication failure.
		 * Return Auth confirm with received failure code to SME
		 */
		pe_err("rx Auth frame from peer with failure code %d "
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authStatusCode,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		lim_restore_from_auth_state(mac_ctx, eSIR_SME_AUTH_REFUSED,
			rx_auth_frm_body->authStatusCode,
			pe_session);
		return;
	}

	if (lim_process_fils_auth_frame2(mac_ctx, pe_session,
					 rx_auth_frm_body)) {
		lim_restore_from_auth_state(mac_ctx, eSIR_SME_SUCCESS,
				rx_auth_frm_body->authStatusCode, pe_session);
		return;
	}

	if (rx_auth_frm_body->authAlgoNumber == eSIR_OPEN_SYSTEM) {
		pe_session->limCurrentAuthType = eSIR_OPEN_SYSTEM;
		auth_node = lim_acquire_free_pre_auth_node(mac_ctx,
				&mac_ctx->lim.gLimPreAuthTimerTable);
		if (auth_node == NULL) {
			pe_warn("Max pre-auth nodes reached");
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);
			return;
		}

		pe_debug("Alloc new data: %pK peer", auth_node);
		lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGD);
		qdf_mem_copy((uint8_t *) auth_node->peerMacAddr,
				mac_ctx->lim.gpLimMlmAuthReq->peerMacAddr,
				sizeof(tSirMacAddr));
		auth_node->fTimerStarted = 0;
		auth_node->authType =
			mac_ctx->lim.gpLimMlmAuthReq->authType;
		auth_node->seq_num =
			((mac_hdr->seqControl.seqNumHi << 4) |
			 (mac_hdr->seqControl.seqNumLo));
		auth_node->timestamp = qdf_mc_timer_get_system_ticks();
		lim_add_pre_auth_node(mac_ctx, auth_node);
		lim_restore_from_auth_state(mac_ctx, eSIR_SME_SUCCESS,
				rx_auth_frm_body->authStatusCode, pe_session);
	} else {
		/* Shared key authentication */
		if (LIM_IS_AP_ROLE(pe_session))
			val = pe_session->privacy;
		else if (wlan_cfg_get_int(mac_ctx,
					WNI_CFG_PRIVACY_ENABLED,
					&val) != QDF_STATUS_SUCCESS)
			pe_warn("couldnt retrieve Privacy option");
		cfg_privacy_opt_imp = (uint8_t) val;
		if (!cfg_privacy_opt_imp) {
			/*
			 * Requesting STA does not have WEP implemented.
			 * Reject with unsupported authentication algo
			 * Status code & wait until auth failure timeout
			 */

			pe_err("rx Auth frm from peer for unsupported auth algo %d "
						MAC_ADDRESS_STR,
					rx_auth_frm_body->authAlgoNumber,
					MAC_ADDR_ARRAY(mac_hdr->sa));

			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_ALGO_NOT_SUPPORTED_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
					mac_hdr->sa, LIM_NO_WEP_IN_FC,
					pe_session);
			return;
		}
		if (rx_auth_frm_body->type != SIR_MAC_CHALLENGE_TEXT_EID) {
			pe_err("rx auth frm with invalid challenge txtie");
			return;
		}
		if (wlan_cfg_get_int(mac_ctx, WNI_CFG_WEP_DEFAULT_KEYID,
					&val) != QDF_STATUS_SUCCESS)
			pe_warn("could not retrieve Default key_id");
		key_id = (uint8_t) val;
		val = SIR_MAC_KEY_LENGTH;
		if (LIM_IS_AP_ROLE(pe_session)) {
			tpSirKeys key_ptr =
				&pe_session->WEPKeyMaterial[key_id].key[0];
			qdf_mem_copy(defaultkey, key_ptr->key,
					key_ptr->keyLength);
		} else if (wlan_cfg_get_str(mac_ctx,
				(uint16_t)(WNI_CFG_WEP_DEFAULT_KEY_1 + key_id),
				defaultkey, &val) != QDF_STATUS_SUCCESS) {
			/* Couldnt get Default key from CFG. */
			pe_warn("cant retrieve Defaultkey");
			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				rx_auth_frm_body->authTransactionSeqNumber + 1;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx,
					auth_frame, mac_hdr->sa,
					LIM_NO_WEP_IN_FC,
					pe_session);
			lim_restore_from_auth_state(mac_ctx,
					eSIR_SME_INVALID_WEP_DEFAULT_KEY,
					eSIR_MAC_UNSPEC_FAILURE_REASON,
					pe_session);
			return;
		}
		key_length = val;
		((tpSirMacAuthFrameBody)plainbody)->authAlgoNumber =
			sir_swap_u16if_needed(rx_auth_frm_body->authAlgoNumber);
		((tpSirMacAuthFrameBody)plainbody)->authTransactionSeqNumber =
			sir_swap_u16if_needed((uint16_t)(
				rx_auth_frm_body->authTransactionSeqNumber
				+ 1));
		((tpSirMacAuthFrameBody)plainbody)->authStatusCode =
			eSIR_MAC_SUCCESS_STATUS;
		((tpSirMacAuthFrameBody)plainbody)->type =
			SIR_MAC_CHALLENGE_TEXT_EID;
		((tpSirMacAuthFrameBody)plainbody)->length =
			rx_auth_frm_body->length;
		qdf_mem_copy((uint8_t *) (
			(tpSirMacAuthFrameBody)plainbody)->challengeText,
			rx_auth_frm_body->challengeText,
			rx_auth_frm_body->length);
		encr_auth_frame = qdf_mem_malloc(rx_auth_frm_body->length +
						 LIM_ENCR_AUTH_INFO_LEN);
		if (!encr_auth_frame) {
			pe_err("failed to allocate memory");
			return;
		}
		lim_encrypt_auth_frame(mac_ctx, key_id,
				defaultkey, plainbody,
				encr_auth_frame, key_length);
		pe_session->limMlmState = eLIM_MLM_WT_AUTH_FRAME4_STATE;
		MTRACE(mac_trace(mac_ctx, TRACE_CODE_MLM_STATE,
					pe_session->peSessionId,
					pe_session->limMlmState));
		lim_send_auth_mgmt_frame(mac_ctx,
				(tpSirMacAuthFrameBody)encr_auth_frame,
				mac_hdr->sa, rx_auth_frm_body->length,
				pe_session);
		qdf_mem_free(encr_auth_frame);
		return;
	}
}

static void lim_process_auth_frame_type3(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		tSirMacAuthFrameBody *auth_frame,
		tpPESession pe_session)
{
	struct tLimPreAuthNode *auth_node;

	/* AuthFrame 3 */
	if (rx_auth_frm_body->authAlgoNumber != eSIR_SHARED_KEY) {
		pe_err("rx Auth frame3 from peer with auth algo number %d "
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authAlgoNumber,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		/*
		 * Received Authentication frame3 with algorithm other than
		 * Shared Key authentication type. Reject with Auth frame4
		 * with 'out of sequence' status code.
		 */
		auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
		auth_frame->authTransactionSeqNumber = SIR_MAC_AUTH_FRAME_4;
		auth_frame->authStatusCode =
			eSIR_MAC_AUTH_FRAME_OUT_OF_SEQ_STATUS;
		lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
			mac_hdr->sa, LIM_NO_WEP_IN_FC,
			pe_session);
		return;
	}

	if (LIM_IS_AP_ROLE(pe_session) ||
			LIM_IS_IBSS_ROLE(pe_session)) {
		/*
		 * Check if wep bit was set in FC. If not set,
		 * reject with Authentication frame4 with
		 * 'challenge failure' status code.
		 */
		if (!mac_hdr->fc.wep) {
			pe_err("received Auth frame3 from peer with no WEP bit set "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/* WEP bit is not set in FC of Auth Frame3 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
					mac_hdr->sa,
					LIM_NO_WEP_IN_FC,
					pe_session);
			return;
		}

		auth_node = lim_search_pre_auth_list(mac_ctx, mac_hdr->sa);
		if (auth_node == NULL) {
			pe_warn("received AuthFrame3 from peer that has no preauth context "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * No 'pre-auth' context exists for this STA that sent
			 * an Authentication frame3. Send Auth frame4 with
			 * 'out of sequence' status code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_FRAME_OUT_OF_SEQ_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			return;
		}

		if (auth_node->mlmState == eLIM_MLM_AUTH_RSP_TIMEOUT_STATE) {
			pe_warn("auth response timer timedout for peer "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * Received Auth Frame3 after Auth Response timeout.
			 * Reject by sending Auth Frame4 with
			 * Auth respone timeout Status Code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_RSP_TIMEOUT_STATUS;

			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			/* Delete pre-auth context of STA */
			lim_delete_pre_auth_node(mac_ctx, mac_hdr->sa);
			return;
		}
		if (rx_auth_frm_body->authStatusCode !=
				eSIR_MAC_SUCCESS_STATUS) {
			/*
			 * Received Authenetication Frame 3 with status code
			 * other than success. Wait until Auth response timeout
			 * to delete STA context.
			 */
			pe_err("rx Auth frm3 from peer with status code %d "
				MAC_ADDRESS_STR,
				rx_auth_frm_body->authStatusCode,
				MAC_ADDR_ARRAY(mac_hdr->sa));
				return;
		}
		/*
		 * Check if received challenge text is same as one sent in
		 * Authentication frame3
		 */
		if (!qdf_mem_cmp(rx_auth_frm_body->challengeText,
					auth_node->challengeText,
					SIR_MAC_SAP_AUTH_CHALLENGE_LENGTH)) {
			/*
			 * Challenge match. STA is autheticated
			 * Delete Authentication response timer if running
			 */
			lim_deactivate_and_change_per_sta_id_timer(mac_ctx,
				eLIM_AUTH_RSP_TIMER, auth_node->authNodeIdx);

			auth_node->fTimerStarted = 0;
			auth_node->mlmState = eLIM_MLM_AUTHENTICATED_STATE;

			/*
			 * Send Auth Frame4 with 'success' Status Code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_SUCCESS_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			return;
		} else {
			pe_warn("Challenge failure for peer "MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * Challenge Failure.
			 * Send Authentication frame4 with 'challenge failure'
			 * status code and wait until Auth response timeout to
			 * delete STA context.
			 */
			auth_frame->authAlgoNumber =
				rx_auth_frm_body->authAlgoNumber;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			return;
		}
	}
}

static void lim_process_auth_frame_type4(tpAniSirGlobal mac_ctx,
		tpSirMacMgmtHdr mac_hdr,
		tSirMacAuthFrameBody *rx_auth_frm_body,
		tpPESession pe_session)
{
	struct tLimPreAuthNode *auth_node;

	if (pe_session->limMlmState != eLIM_MLM_WT_AUTH_FRAME4_STATE) {
		/*
		 * Received Authentication frame4 in an unexpected state.
		 * Log error and ignore the frame.
		 */
		pe_warn("received unexpected Auth frame4 from peer in state %d, addr "
			MAC_ADDRESS_STR,
			pe_session->limMlmState,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		return;
	}

	if (rx_auth_frm_body->authAlgoNumber != eSIR_SHARED_KEY) {
		/*
		 * Received Authentication frame4 with algorithm other than
		 * Shared Key authentication type.
		 * Wait until Auth failure timeout to report authentication
		 * failure to SME.
		 */
		pe_err("received Auth frame4 from peer with invalid auth algo %d"
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authAlgoNumber,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		return;
	}

	if (qdf_mem_cmp((uint8_t *) mac_hdr->sa,
			(uint8_t *) &mac_ctx->lim.gpLimMlmAuthReq->peerMacAddr,
			sizeof(tSirMacAddr))) {
		/*
		 * Received Authentication frame from an entity
		 * other than one to which request was initiated.
		 * Wait until Authentication Failure Timeout.
		 */

		pe_warn("received Auth frame4 from unexpected peer "MAC_ADDRESS_STR,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		return;
	}

	if (rx_auth_frm_body->authAlgoNumber !=
			mac_ctx->lim.gpLimMlmAuthReq->authType) {
		/*
		 * Received Authentication frame with an auth algorithm
		 * other than one requested.
		 * Wait until Authentication Failure Timeout.
		 */

		pe_err("received Authentication frame from peer with invalid auth seq number %d "
			MAC_ADDRESS_STR,
			rx_auth_frm_body->authTransactionSeqNumber,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		return;
	}

	if (rx_auth_frm_body->authStatusCode == eSIR_MAC_SUCCESS_STATUS) {
		/*
		 * Authentication Success, Inform SME of same.
		 */
		pe_session->limCurrentAuthType = eSIR_SHARED_KEY;
		auth_node = lim_acquire_free_pre_auth_node(mac_ctx,
					&mac_ctx->lim.gLimPreAuthTimerTable);
		if (auth_node == NULL) {
			pe_warn("Max pre-auth nodes reached");
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGW);
			return;
		}
		pe_debug("Alloc new data: %pK peer", auth_node);
		lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGD);
		qdf_mem_copy((uint8_t *) auth_node->peerMacAddr,
				mac_ctx->lim.gpLimMlmAuthReq->peerMacAddr,
				sizeof(tSirMacAddr));
		auth_node->fTimerStarted = 0;
		auth_node->authType = mac_ctx->lim.gpLimMlmAuthReq->authType;
		auth_node->seq_num = ((mac_hdr->seqControl.seqNumHi << 4) |
					(mac_hdr->seqControl.seqNumLo));
		auth_node->timestamp = qdf_mc_timer_get_system_ticks();
		lim_add_pre_auth_node(mac_ctx, auth_node);
		lim_restore_from_auth_state(mac_ctx, eSIR_SME_SUCCESS,
				rx_auth_frm_body->authStatusCode, pe_session);
	} else {
		/*
		 * Authentication failure.
		 * Return Auth confirm with received failure code to SME
		 */
		pe_err("Authentication failure from peer "MAC_ADDRESS_STR,
			MAC_ADDR_ARRAY(mac_hdr->sa));
		lim_restore_from_auth_state(mac_ctx, eSIR_SME_AUTH_REFUSED,
				rx_auth_frm_body->authStatusCode,
				pe_session);
	}
}

/**
 * lim_process_auth_frame() - to process auth frame
 * @mac_ctx - Pointer to Global MAC structure
 * @rx_pkt_info - A pointer to Rx packet info structure
 * @session - A pointer to session
 *
 * This function is called by limProcessMessageQueue() upon Authentication
 * frame reception.
 *
 * LOGIC:
 * This function processes received Authentication frame and responds
 * with either next Authentication frame in sequence to peer MAC entity
 * or LIM_MLM_AUTH_IND on AP or LIM_MLM_AUTH_CNF on STA.
 *
 * NOTE:
 * 1. Authentication failures are reported to SME with same status code
 *    received from the peer MAC entity.
 * 2. Authentication frame2/4 received with alogirthm number other than
 *    one requested in frame1/3 are logged with an error and auth confirm
 *    will be sent to SME only after auth failure timeout.
 * 3. Inconsistency in the spec:
 *    On receiving Auth frame2, specs says that if WEP key mapping key
 *    or default key is NULL, Auth frame3 with a status code 15 (challenge
 *    failure to be returned to peer entity. However, section 7.2.3.10,
 *    table 14 says that status code field is 'reserved' for frame3 !
 *    In the current implementation, Auth frame3 is returned with status
 *    code 15 overriding section 7.2.3.10.
 * 4. If number pre-authentications reach configrable max limit,
 *    Authentication frame with 'unspecified failure' status code is
 *    returned to requesting entity.
 *
 * Return: None
 */
void
lim_process_auth_frame(tpAniSirGlobal mac_ctx, uint8_t *rx_pkt_info,
		       tpPESession pe_session)
{
	uint8_t *body_ptr, key_id, cfg_privacy_opt_imp;
	uint8_t defaultkey[SIR_MAC_KEY_LENGTH];
	uint8_t *plainbody = NULL;
	uint8_t decrypt_result;
	uint16_t frame_len, curr_seq_num = 0, auth_alg;
	uint32_t val, key_length = 8;
	tSirMacAuthFrameBody *rx_auth_frm_body, *rx_auth_frame, *auth_frame;
	tpSirMacMgmtHdr mac_hdr;
	struct tLimPreAuthNode *auth_node;

	/* Get pointer to Authentication frame header and body */
	mac_hdr = WMA_GET_RX_MAC_HEADER(rx_pkt_info);
	frame_len = WMA_GET_RX_PAYLOAD_LEN(rx_pkt_info);

	if (!frame_len) {
		/* Log error */
		pe_err("received Auth frame with no body from: %pM",
			mac_hdr->sa);
		return;
	}

	if (lim_is_group_addr(mac_hdr->sa)) {
		/*
		 * Received Auth frame from a BC/MC address
		 * Log error and ignore it
		 */
		pe_err("received Auth frame from a BC/MC addr: %pM",
			mac_hdr->sa);
		return;
	}
	curr_seq_num = (mac_hdr->seqControl.seqNumHi << 4) |
		(mac_hdr->seqControl.seqNumLo);

	pe_nofl_info("vid %d sys role %d lim_state %d Auth RX from "QDF_MAC_ADDR_STR" rssi %d",
		     pe_session->peSessionId, GET_LIM_SYSTEM_ROLE(pe_session),
		     pe_session->limMlmState, MAC_ADDR_ARRAY(mac_hdr->bssId),
		     WMA_GET_RX_RSSI_NORMALIZED(rx_pkt_info));

	if (pe_session->prev_auth_seq_num == curr_seq_num &&
	    mac_hdr->fc.retry) {
		pe_debug("auth frame, seq num: %d is already processed, drop it",
			 curr_seq_num);
		return;
	}

	/* save seq number in pe_session */
	pe_session->prev_auth_seq_num = curr_seq_num;

	body_ptr = WMA_GET_RX_MPDU_DATA(rx_pkt_info);

	if (frame_len < 2) {
		pe_err("invalid frame len: %d", frame_len);
		return;
	}
	auth_alg = *(uint16_t *) body_ptr;
	pe_debug("auth_alg %d ", auth_alg);

	/* Restore default failure timeout */
	if (QDF_P2P_CLIENT_MODE == pe_session->pePersona &&
			pe_session->defaultAuthFailureTimeout) {
		pe_debug("Restore default failure timeout");
		cfg_set_int(mac_ctx, WNI_CFG_AUTHENTICATE_FAILURE_TIMEOUT,
				pe_session->defaultAuthFailureTimeout);
	}

	rx_auth_frame = qdf_mem_malloc(sizeof(tSirMacAuthFrameBody));
	if (!rx_auth_frame) {
		pe_err("failed to allocate memory");
		return;
	}

	auth_frame = qdf_mem_malloc(sizeof(tSirMacAuthFrameBody));
	if (!auth_frame) {
		pe_err("failed to allocate memory");
		goto free;
	}

	plainbody = qdf_mem_malloc(LIM_ENCR_AUTH_BODY_LEN);
	if (!plainbody) {
		pe_err("failed to allocate memory for plainbody");
		goto free;
	}
	qdf_mem_zero(plainbody, LIM_ENCR_AUTH_BODY_LEN);

	/*
	 * Determine if WEP bit is set in the FC or received MAC header
	 * Note: WEP bit is set in FC of MAC header.
	 */
	if (mac_hdr->fc.wep) {
		/*
		 * If TKIP counter measures enabled then issue Deauth
		 * frame to station
		 */
		if (pe_session->bTkipCntrMeasActive &&
				LIM_IS_AP_ROLE(pe_session)) {
			pe_err("Tkip counter enabled, send deauth to: %pM",
				mac_hdr->sa);
			lim_send_deauth_mgmt_frame(mac_ctx,
					eSIR_MAC_MIC_FAILURE_REASON,
					mac_hdr->sa, pe_session, false);
			goto free;
		}
		if (frame_len < 4) {
			pe_err("invalid frame len: %d", frame_len);
			goto free;
		}
		/* Extract key ID from IV (most 2 bits of 4th byte of IV) */
		key_id = (*(body_ptr + 3)) >> 6;

		/*
		 * On STA in infrastructure BSS, Authentication frames received
		 * with WEP bit set in the FC must be rejected with challenge
		 * failure status code (weird thing in the spec - this should've
		 * been rejected with unspecified failure/unexpected assertion
		 * of wep bit (this status code does not exist though) or
		 * Out-of-sequence-Authentication-Frame status code.
		 */
		if (LIM_IS_STA_ROLE(pe_session)) {
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			/* Log error */
			pe_err("rx Auth frm with wep bit set role: %d %pM",
				GET_LIM_SYSTEM_ROLE(pe_session), mac_hdr->sa);
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}

		if ((frame_len < LIM_ENCR_AUTH_BODY_LEN_SAP) ||
		    (frame_len > LIM_ENCR_AUTH_BODY_LEN)) {
			/* Log error */
			pe_err("Not enough size: %d to decry rx Auth frm",
				frame_len);
			lim_print_mac_addr(mac_ctx, mac_hdr->sa, LOGE);
			goto free;
		}
		if (LIM_IS_AP_ROLE(pe_session)) {
			val = pe_session->privacy;
		} else if (wlan_cfg_get_int(mac_ctx, WNI_CFG_PRIVACY_ENABLED,
					&val) != QDF_STATUS_SUCCESS) {
			/*
			 * Accept Authentication frame only if Privacy is
			 * implemented, if Could not get Privacy option
			 * from CFG then Log fatal error
			 */
			pe_warn("could not retrieve Privacy option");
		}
		cfg_privacy_opt_imp = (uint8_t) val;

		if (!cfg_privacy_opt_imp) {
			pe_err("received Authentication frame3 from peer that while privacy option is turned OFF "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * Privacy option is not implemented.
			 * So reject Authentication frame received with
			 * WEP bit set by sending Authentication frame
			 * with 'challenge failure' status code. This is
			 * another strange thing in the spec. Status code
			 * should have been 'unsupported algorithm' status code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}

		/*
		 * Privacy option is implemented. Check if the received frame is
		 * Authentication frame3 and there is a context for requesting
		 * STA. If not, reject with unspecified failure status code
		 */
		auth_node = lim_search_pre_auth_list(mac_ctx, mac_hdr->sa);
		if (auth_node == NULL) {
			pe_err("rx Auth frame with no preauth ctx with WEP bit set "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * No 'pre-auth' context exists for this STA
			 * that sent an Authentication frame with FC
			 * bit set. Send Auth frame4 with
			 * 'out of sequence' status code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_FRAME_OUT_OF_SEQ_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}
		/* Change the auth-response timeout */
		lim_deactivate_and_change_per_sta_id_timer(mac_ctx,
				eLIM_AUTH_RSP_TIMER, auth_node->authNodeIdx);

		/* 'Pre-auth' status exists for STA */
		if ((auth_node->mlmState != eLIM_MLM_WT_AUTH_FRAME3_STATE) &&
			(auth_node->mlmState !=
				eLIM_MLM_AUTH_RSP_TIMEOUT_STATE)) {
			pe_err("received Authentication frame from peer that is in state %d "
				MAC_ADDRESS_STR,
				auth_node->mlmState,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/*
			 * Should not have received Authentication frame
			 * with WEP bit set in FC in other states.
			 * Reject by sending Authenticaton frame with
			 * out of sequence Auth frame status code.
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_AUTH_FRAME_OUT_OF_SEQ_STATUS;

			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}

		val = SIR_MAC_KEY_LENGTH;

		if (LIM_IS_AP_ROLE(pe_session)) {
			tpSirKeys key_ptr;

			key_ptr = &pe_session->WEPKeyMaterial[key_id].key[0];
			qdf_mem_copy(defaultkey, key_ptr->key,
					key_ptr->keyLength);
			val = key_ptr->keyLength;
		} else if (wlan_cfg_get_str(mac_ctx,
				(uint16_t) (WNI_CFG_WEP_DEFAULT_KEY_1 + key_id),
				defaultkey, &val) != QDF_STATUS_SUCCESS) {
			pe_warn("could not retrieve Default key");

			/*
			 * Send Authentication frame
			 * with challenge failure status code
			 */
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;
			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}

		key_length = val;
		decrypt_result = lim_decrypt_auth_frame(mac_ctx, defaultkey,
					body_ptr, plainbody, key_length,
					(uint16_t) (frame_len -
							SIR_MAC_WEP_IV_LENGTH));
		if (decrypt_result == LIM_DECRYPT_ICV_FAIL) {
			pe_err("received Authentication frame from peer that failed decryption: "
				MAC_ADDRESS_STR,
				MAC_ADDR_ARRAY(mac_hdr->sa));
			/* ICV failure */
			lim_delete_pre_auth_node(mac_ctx, mac_hdr->sa);
			auth_frame->authAlgoNumber = eSIR_SHARED_KEY;
			auth_frame->authTransactionSeqNumber =
				SIR_MAC_AUTH_FRAME_4;
			auth_frame->authStatusCode =
				eSIR_MAC_CHALLENGE_FAILURE_STATUS;

			lim_send_auth_mgmt_frame(mac_ctx, auth_frame,
				mac_hdr->sa, LIM_NO_WEP_IN_FC,
				pe_session);
			goto free;
		}
		if ((sir_convert_auth_frame2_struct(mac_ctx, plainbody,
				frame_len - 8, rx_auth_frame) != QDF_STATUS_SUCCESS)
				|| (!is_auth_valid(mac_ctx, rx_auth_frame,
							pe_session))) {
			pe_err("failed to convert Auth Frame to structure or Auth is not valid");
			goto free;
		}
	} else if ((auth_alg ==
		    eSIR_AUTH_TYPE_SAE) && (LIM_IS_STA_ROLE(pe_session))) {
		lim_process_sae_auth_frame(mac_ctx,
					rx_pkt_info, pe_session);
		goto free;
	} else if ((sir_convert_auth_frame2_struct(mac_ctx, body_ptr,
				frame_len, rx_auth_frame) != QDF_STATUS_SUCCESS)
				|| (!is_auth_valid(mac_ctx, rx_auth_frame,
						pe_session))) {
			pe_err("failed to convert Auth Frame to structure or Auth is not valid");
			goto free;
	}

	rx_auth_frm_body = rx_auth_frame;

	pe_info("Rx Auth frame SN=%d type: %d TSN: %d status: %d %d",
		curr_seq_num,
		(uint32_t) rx_auth_frm_body->authAlgoNumber,
		(uint32_t) rx_auth_frm_body->authTransactionSeqNumber,
		(uint32_t) rx_auth_frm_body->authStatusCode,
		(uint32_t) mac_ctx->lim.gLimNumPreAuthContexts);

	if (!lim_is_valid_fils_auth_frame(mac_ctx, pe_session,
			rx_auth_frm_body)) {
		pe_err("Received invalid FILS auth packet");
		goto free;
	}

	/*
	 * IOT Workaround: with invalid WEP key, some APs reply
	 * AuthFrame 4 with invalid seqNumber. This AuthFrame
	 * will be dropped by driver, thus driver sends the
	 * generic status code instead of protocol status code.
	 * As a workaround, override AuthFrame 4's seqNumber.
	 */
	if ((pe_session->limMlmState ==
		eLIM_MLM_WT_AUTH_FRAME4_STATE) &&
		(rx_auth_frm_body->authTransactionSeqNumber !=
		SIR_MAC_AUTH_FRAME_1) &&
		(rx_auth_frm_body->authTransactionSeqNumber !=
		SIR_MAC_AUTH_FRAME_2) &&
		(rx_auth_frm_body->authTransactionSeqNumber !=
		SIR_MAC_AUTH_FRAME_3)) {
		pe_warn("Override AuthFrame 4's seqNumber to 4");
		rx_auth_frm_body->authTransactionSeqNumber =
			SIR_MAC_AUTH_FRAME_4;
	}


	switch (rx_auth_frm_body->authTransactionSeqNumber) {
	case SIR_MAC_AUTH_FRAME_1:
		lim_process_auth_frame_type1(mac_ctx,
			mac_hdr, rx_auth_frm_body, rx_pkt_info,
			curr_seq_num, auth_frame, pe_session);
		break;
	case SIR_MAC_AUTH_FRAME_2:
		lim_process_auth_frame_type2(mac_ctx,
			mac_hdr, rx_auth_frm_body, auth_frame, plainbody,
			body_ptr, frame_len, pe_session);
		break;
	case SIR_MAC_AUTH_FRAME_3:
		lim_process_auth_frame_type3(mac_ctx,
			mac_hdr, rx_auth_frm_body, auth_frame, pe_session);
		break;
	case SIR_MAC_AUTH_FRAME_4:
		lim_process_auth_frame_type4(mac_ctx,
			mac_hdr, rx_auth_frm_body, pe_session);
		break;
	default:
		/* Invalid Authentication Frame received. Ignore it. */
		pe_warn("rx auth frm with invalid authseq no: %d from: %pM",
			rx_auth_frm_body->authTransactionSeqNumber,
			mac_hdr->sa);
		break;
	}
free:
	if (auth_frame)
		qdf_mem_free(auth_frame);
	if (rx_auth_frame)
		qdf_mem_free(rx_auth_frame);
	if (plainbody)
		qdf_mem_free(plainbody);
}

/*----------------------------------------------------------------------
 *
 * Pass the received Auth frame. This is possibly the pre-auth from the
 * neighbor AP, in the same mobility domain.
 * This will be used in case of 11r FT.
 *
 * !!!! This is going to be renoved for the next checkin. We will be creating
 * the session before sending out the Auth. Thus when auth response
 * is received we will have a session in progress. !!!!!
 ***----------------------------------------------------------------------
 */
QDF_STATUS lim_process_auth_frame_no_session(tpAniSirGlobal pMac, uint8_t *pBd,
						void *body)
{
	tpSirMacMgmtHdr pHdr;
	tpPESession psessionEntry = NULL;
	uint8_t *pBody;
	uint16_t frameLen;
	tSirMacAuthFrameBody rxAuthFrame;
	tSirMacAuthFrameBody *pRxAuthFrameBody = NULL;
	QDF_STATUS ret_status = QDF_STATUS_E_FAILURE;
	int i;

	pHdr = WMA_GET_RX_MAC_HEADER(pBd);
	pBody = WMA_GET_RX_MPDU_DATA(pBd);
	frameLen = WMA_GET_RX_PAYLOAD_LEN(pBd);

	pe_debug("Auth Frame Received: BSSID " MAC_ADDRESS_STR " (RSSI %d)",
		 MAC_ADDR_ARRAY(pHdr->bssId),
		 (uint) abs((int8_t) WMA_GET_RX_RSSI_NORMALIZED(pBd)));

	/* Auth frame has come on a new BSS, however, we need to find the session
	 * from where the auth-req was sent to the new AP
	 */
	for (i = 0; i < pMac->lim.maxBssId; i++) {
		/* Find first free room in session table */
		if (pMac->lim.gpSession[i].valid == true &&
		    pMac->lim.gpSession[i].ftPEContext.ftPreAuthSession ==
		    true) {
			/* Found the session */
			psessionEntry = &pMac->lim.gpSession[i];
			pMac->lim.gpSession[i].ftPEContext.ftPreAuthSession =
				false;
		}
	}

	if (psessionEntry == NULL) {
		pe_debug("cannot find session id in FT pre-auth phase");
		return QDF_STATUS_E_FAILURE;
	}

	if (psessionEntry->ftPEContext.pFTPreAuthReq == NULL) {
		pe_err("Error: No FT");
		/* No FT in progress. */
		return QDF_STATUS_E_FAILURE;
	}

	if (frameLen == 0) {
		pe_err("Error: Frame len = 0");
		return QDF_STATUS_E_FAILURE;
	}
	lim_print_mac_addr(pMac, pHdr->bssId, LOGD);
	lim_print_mac_addr(pMac,
			   psessionEntry->ftPEContext.pFTPreAuthReq->preAuthbssId,
			   LOGD);
	pe_debug("seqControl: 0x%X",
		((pHdr->seqControl.seqNumHi << 8) |
		 (pHdr->seqControl.seqNumLo << 4) |
		 (pHdr->seqControl.fragNum)));

	/* Check that its the same bssId we have for preAuth */
	if (qdf_mem_cmp
		    (psessionEntry->ftPEContext.pFTPreAuthReq->preAuthbssId,
		    pHdr->bssId, sizeof(tSirMacAddr))) {
		pe_err("Error: Same bssid as preauth BSSID");
		/* In this case SME if indeed has triggered a */
		/* pre auth it will time out. */
		return QDF_STATUS_E_FAILURE;
	}

	if (true ==
	    psessionEntry->ftPEContext.pFTPreAuthReq->bPreAuthRspProcessed) {
		/*
		 * This is likely a duplicate for the same pre-auth request.
		 * PE/LIM already posted a response to SME. Hence, drop it.
		 * TBD:
		 * 1) How did we even receive multiple auth responses?
		 * 2) Do we need to delete pre-auth session? Suppose we
		 * previously received an auth resp with failure which
		 * would not have created the session and forwarded to SME.
		 * And, we subsequently received an auth resp with success
		 * which would have created the session. This will now be
		 * dropped without being forwarded to SME! However, it is
		 * very unlikely to receive auth responses from the same
		 * AP with different reason codes.
		 * NOTE: return QDF_STATUS_SUCCESS so that the packet is dropped
		 * as this was indeed a response from the BSSID we tried to
		 * pre-auth.
		 */
		pe_debug("Auth rsp already posted to SME"
			       " (session %pK, FT session %pK)", psessionEntry,
			       psessionEntry);
		return QDF_STATUS_SUCCESS;
	} else {
		pe_warn("Auth rsp not yet posted to SME"
			       " (session %pK, FT session %pK)", psessionEntry,
			       psessionEntry);
		psessionEntry->ftPEContext.pFTPreAuthReq->bPreAuthRspProcessed =
			true;
	}

	/* Stopping timer now, that we have our unicast from the AP */
	/* of our choice. */
	lim_deactivate_and_change_timer(pMac, eLIM_FT_PREAUTH_RSP_TIMER);

	/* Save off the auth resp. */
	if ((sir_convert_auth_frame2_struct(pMac, pBody, frameLen, &rxAuthFrame) !=
	     QDF_STATUS_SUCCESS)) {
		pe_err("failed to convert Auth frame to struct");
		lim_handle_ft_pre_auth_rsp(pMac, QDF_STATUS_E_FAILURE, NULL, 0,
					   psessionEntry);
		return QDF_STATUS_E_FAILURE;
	}
	pRxAuthFrameBody = &rxAuthFrame;

	pe_debug("Received Auth frame with type: %d seqnum: %d status: %d %d",
		       (uint32_t) pRxAuthFrameBody->authAlgoNumber,
		       (uint32_t) pRxAuthFrameBody->authTransactionSeqNumber,
		       (uint32_t) pRxAuthFrameBody->authStatusCode,
		       (uint32_t) pMac->lim.gLimNumPreAuthContexts);
	switch (pRxAuthFrameBody->authTransactionSeqNumber) {
	case SIR_MAC_AUTH_FRAME_2:
		if (pRxAuthFrameBody->authStatusCode != eSIR_MAC_SUCCESS_STATUS) {
			pe_err("Auth status code received is %d",
				(uint32_t) pRxAuthFrameBody->authStatusCode);
			if (eSIR_MAC_MAX_ASSOC_STA_REACHED_STATUS ==
			    pRxAuthFrameBody->authStatusCode)
				ret_status = QDF_STATUS_E_NOSPC;
		} else {
			ret_status = QDF_STATUS_SUCCESS;
		}
		break;

	default:
		pe_warn("Seq. no incorrect expected 2 received %d",
			(uint32_t) pRxAuthFrameBody->authTransactionSeqNumber);
		break;
	}

	/* Send the Auth response to SME */
	lim_handle_ft_pre_auth_rsp(pMac, ret_status, pBody, frameLen, psessionEntry);

	return ret_status;
}
