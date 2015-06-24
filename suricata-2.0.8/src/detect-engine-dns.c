/* Copyright (C) 2013 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/** \file
 *
 *  \author Victor Julien <victor@inliniac.net>
 *
 *  Based on detect-engine-uri.c
 */

#include "suricata-common.h"
#include "suricata.h"
#include "decode.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-parse.h"
#include "detect-engine-state.h"
#include "detect-engine-content-inspection.h"

#include "flow-util.h"
#include "util-debug.h"
#include "util-print.h"
#include "flow.h"

#include "app-layer.h"
#include "app-layer-parser.h"
#include "app-layer-protos.h"
#include "app-layer-dns-common.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

/** \brief Do the content inspection & validation for a signature
 *
 *  \param de_ctx Detection engine context
 *  \param det_ctx Detection engine thread context
 *  \param s Signature to inspect
 *  \param sm SigMatch to inspect
 *  \param f Flow
 *  \param flags app layer flags
 *  \param state App layer state
 *
 *  \retval 0 no match
 *  \retval 1 match
 */
int DetectEngineInspectDnsQueryName(ThreadVars *tv,
                                  DetectEngineCtx *de_ctx,
                                  DetectEngineThreadCtx *det_ctx,
                                  Signature *s, Flow *f, uint8_t flags,
                                  void *alstate, void *txv, uint64_t tx_id)
{
    DNSTransaction *tx = (DNSTransaction *)txv;
    DNSQueryEntry *query = NULL;
    uint8_t *buffer;
    uint16_t buffer_len;
    int r = 0;

    SCLogDebug("start");

    TAILQ_FOREACH(query, &tx->query_list, next) {
        SCLogDebug("tx %p query %p", tx, query);
        det_ctx->discontinue_matching = 0;
        det_ctx->buffer_offset = 0;
        det_ctx->inspection_recursion_counter = 0;

        buffer = (uint8_t *)((uint8_t *)query + sizeof(DNSQueryEntry));
        buffer_len = query->len;

        //PrintRawDataFp(stdout, buffer, buffer_len);

        r = DetectEngineContentInspection(de_ctx, det_ctx,
                s, s->sm_lists[DETECT_SM_LIST_DNSQUERY_MATCH],
                f, buffer, buffer_len, 0,
                DETECT_ENGINE_CONTENT_INSPECTION_MODE_DNSQUERY, NULL);
        if (r == 1)
            break;
    }
    return r;
}
