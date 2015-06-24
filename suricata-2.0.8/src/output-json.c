/* Copyright (C) 2007-2013 Open Information Security Foundation
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

/**
 * \file
 *
 * \author Tom DeCanio <td@npulsetech.com>
 *
 * Logs alerts in JSON format.
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "util-debug.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"
#include "app-layer-parser.h"
#include "util-classification-config.h"
#include "util-syslog.h"

#include "output.h"
#include "output-json.h"

#include "util-byte.h"
#include "util-privs.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-optimize.h"
#include "util-buffer.h"
#include "util-logopenfile.h"


#ifndef HAVE_LIBJANSSON

/** Handle the case where no JSON support is compiled in.
 *
 */

TmEcode OutputJson (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode OutputJsonThreadInit(ThreadVars *, void *, void **);
TmEcode OutputJsonThreadDeinit(ThreadVars *, void *);
int OutputJsonOpenFileCtx(LogFileCtx *, char *);
void OutputJsonRegisterTests(void);

void TmModuleOutputJsonRegister (void) {
    tmm_modules[TMM_OUTPUTJSON].name = "OutputJSON";
    tmm_modules[TMM_OUTPUTJSON].ThreadInit = OutputJsonThreadInit;
    tmm_modules[TMM_OUTPUTJSON].Func = OutputJson;
    tmm_modules[TMM_OUTPUTJSON].ThreadDeinit = OutputJsonThreadDeinit;
    tmm_modules[TMM_OUTPUTJSON].RegisterTests = OutputJsonRegisterTests;
}

OutputCtx *OutputJsonInitCtx(ConfNode *conf)
{
    SCLogDebug("Can't init JSON output - JSON support was disabled during build.");
    return NULL;
}

TmEcode OutputJsonThreadInit(ThreadVars *t, void *initdata, void **data)
{
    SCLogDebug("Can't init JSON output thread - JSON support was disabled during build.");
    return TM_ECODE_FAILED;
}

TmEcode OutputJson (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    return TM_ECODE_OK;
}

TmEcode OutputJsonThreadDeinit(ThreadVars *t, void *data)
{
    return TM_ECODE_FAILED;
}

void OutputJsonRegisterTests (void)
{
}

#else /* implied we do have JSON support */

#include <jansson.h>

#define DEFAULT_LOG_FILENAME "eve.json"
#define DEFAULT_ALERT_SYSLOG_FACILITY_STR       "local0"
#define DEFAULT_ALERT_SYSLOG_FACILITY           LOG_LOCAL0
#define DEFAULT_ALERT_SYSLOG_LEVEL              LOG_INFO
#define MODULE_NAME "OutputJSON"

#define OUTPUT_BUFFER_SIZE 65535

#ifndef OS_WIN32
static int alert_syslog_level = DEFAULT_ALERT_SYSLOG_LEVEL;
#endif /* OS_WIN32 */

TmEcode OutputJson (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode OutputJsonThreadInit(ThreadVars *, void *, void **);
TmEcode OutputJsonThreadDeinit(ThreadVars *, void *);
void OutputJsonExitPrintStats(ThreadVars *, void *);
void OutputJsonRegisterTests(void);
static void OutputJsonDeInitCtx(OutputCtx *);

void TmModuleOutputJsonRegister (void) {
    tmm_modules[TMM_OUTPUTJSON].name = MODULE_NAME;
    tmm_modules[TMM_OUTPUTJSON].ThreadInit = OutputJsonThreadInit;
    tmm_modules[TMM_OUTPUTJSON].Func = OutputJson;
    tmm_modules[TMM_OUTPUTJSON].ThreadExitPrintStats = OutputJsonExitPrintStats;
    tmm_modules[TMM_OUTPUTJSON].ThreadDeinit = OutputJsonThreadDeinit;
    tmm_modules[TMM_OUTPUTJSON].RegisterTests = OutputJsonRegisterTests;
    tmm_modules[TMM_OUTPUTJSON].cap_flags = 0;

    OutputRegisterModule(MODULE_NAME, "eve-log", OutputJsonInitCtx);
}

/* Default Sensor ID value */
static int64_t sensor_id = -1; /* -1 = not defined */

static enum JsonOutput json_out = ALERT_FILE;

static enum JsonFormat format = COMPACT;

json_t *CreateJSONHeader(Packet *p, int direction_sensitive, char *event_type)
{
    char timebuf[64];
    char srcip[46], dstip[46];
    Port sp, dp;

    json_t *js = json_object();
    if (unlikely(js == NULL))
        return NULL;

    CreateIsoTimeString(&p->ts, timebuf, sizeof(timebuf));

    srcip[0] = '\0';
    dstip[0] = '\0';
    if (direction_sensitive) {
        if ((PKT_IS_TOSERVER(p))) {
            if (PKT_IS_IPV4(p)) {
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
            } else if (PKT_IS_IPV6(p)) {
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
            }
            sp = p->sp;
            dp = p->dp;
        } else {
            if (PKT_IS_IPV4(p)) {
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), dstip, sizeof(dstip));
            } else if (PKT_IS_IPV6(p)) {
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), dstip, sizeof(dstip));
            }
            sp = p->dp;
            dp = p->sp;
        }
    } else {
        if (PKT_IS_IPV4(p)) {
            PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
            PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));
        } else if (PKT_IS_IPV6(p)) {
            PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
            PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));
        }
        sp = p->sp;
        dp = p->dp;
    }

    char proto[16];
    if (SCProtoNameValid(IP_GET_IPPROTO(p)) == TRUE) {
        strlcpy(proto, known_proto[IP_GET_IPPROTO(p)], sizeof(proto));
    } else {
        snprintf(proto, sizeof(proto), "%03" PRIu32, IP_GET_IPPROTO(p));
    }

    /* time & tx */
    json_object_set_new(js, "timestamp", json_string(timebuf));

    /* sensor id */
    if (sensor_id >= 0)
        json_object_set_new(js, "sensor_id", json_integer(sensor_id));

    /* pcap_cnt */
    if (p->pcap_cnt != 0) {
        json_object_set_new(js, "pcap_cnt", json_integer(p->pcap_cnt));
    }

    if (event_type) {
        json_object_set_new(js, "event_type", json_string(event_type));
    }

    /* vlan */
    if (p->vlan_idx > 0) {
        json_t *js_vlan;
        switch (p->vlan_idx) {
            case 1:
                json_object_set_new(js, "vlan",
                                    json_integer(VLAN_GET_ID1(p)));
                break;
            case 2:
                js_vlan = json_array();
                if (unlikely(js != NULL)) {
                    json_array_append_new(js_vlan,
                                    json_integer(VLAN_GET_ID1(p)));
                    json_array_append_new(js_vlan,
                                    json_integer(VLAN_GET_ID2(p)));
                    json_object_set_new(js, "vlan", js_vlan);
                }
                break;
            default:
                /* shouldn't get here */
                break;
        }
    }

    /* tuple */
    json_object_set_new(js, "src_ip", json_string(srcip));
    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            json_object_set_new(js, "src_port", json_integer(sp));
            break;
    }
    json_object_set_new(js, "dest_ip", json_string(dstip));
    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            json_object_set_new(js, "dest_port", json_integer(dp));
            break;
    }
    json_object_set_new(js, "proto", json_string(proto));
    switch (p->proto) {
        case IPPROTO_ICMP:
            if (p->icmpv4h) {
                json_object_set_new(js, "icmp_type",
                                    json_integer(p->icmpv4h->type));
                json_object_set_new(js, "icmp_code",
                                    json_integer(p->icmpv4h->code));
            }
            break;
        case IPPROTO_ICMPV6:
            if (p->icmpv6h) {
                json_object_set_new(js, "icmp_type",
                                    json_integer(p->icmpv6h->type));
                json_object_set_new(js, "icmp_code",
                                    json_integer(p->icmpv6h->code));
            }
            break;
    }

    return js;
}

int OutputJSONBuffer(json_t *js, LogFileCtx *file_ctx, MemBuffer *buffer) {
    char *js_s = json_dumps(js,
                            JSON_PRESERVE_ORDER|JSON_COMPACT|JSON_ENSURE_ASCII|
#ifdef JSON_ESCAPE_SLASH
                            JSON_ESCAPE_SLASH
#else
                            0
#endif
                            );
    if (unlikely(js_s == NULL))
        return TM_ECODE_OK;

    SCMutexLock(&file_ctx->fp_mutex);
    if (json_out == ALERT_SYSLOG) {
        syslog(alert_syslog_level, "%s", js_s);
    } else if (json_out == ALERT_FILE || json_out == ALERT_UNIX_DGRAM || json_out == ALERT_UNIX_STREAM) {
        MemBufferWriteString(buffer, "%s\n", js_s);
        file_ctx->Write((const char *)MEMBUFFER_BUFFER(buffer),
            MEMBUFFER_OFFSET(buffer), file_ctx);
    }
    SCMutexUnlock(&file_ctx->fp_mutex);
    free(js_s);
    return 0;
}

TmEcode OutputJson (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    return TM_ECODE_OK;
}

TmEcode OutputJsonThreadInit(ThreadVars *t, void *initdata, void **data)
{
    AlertJsonThread *aft = SCMalloc(sizeof(AlertJsonThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(AlertJsonThread));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for AlertJson.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode OutputJsonThreadDeinit(ThreadVars *t, void *data)
{
    AlertJsonThread *aft = (AlertJsonThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    SCFree(aft);
    return TM_ECODE_OK;
}

void OutputJsonExitPrintStats(ThreadVars *tv, void *data) {
    AlertJsonThread *aft = (AlertJsonThread *)data;
    if (aft == NULL) {
        return;
    }

    SCLogInfo("JSON output wrote %" PRIu64 " alerts", aft->file_ctx->alerts);

}

/**
 * \brief Create a new LogFileCtx for "fast" output style.
 * \param conf The configuration node for this output.
 * \return A LogFileCtx pointer on success, NULL on failure.
 */
OutputCtx *OutputJsonInitCtx(ConfNode *conf)
{
    OutputJsonCtx *json_ctx = SCCalloc(1, sizeof(OutputJsonCtx));;
    if (unlikely(json_ctx == NULL)) {
        SCLogDebug("AlertJsonInitCtx: Could not create new LogFileCtx");
        return NULL;
    }

    json_ctx->file_ctx = LogFileNewCtx();
    if (unlikely(json_ctx->file_ctx == NULL)) {
        SCLogDebug("AlertJsonInitCtx: Could not create new LogFileCtx");
        SCFree(json_ctx);
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        LogFileFreeCtx(json_ctx->file_ctx);
        SCFree(json_ctx);
        return NULL;
    }

    output_ctx->data = json_ctx;
    output_ctx->DeInit = OutputJsonDeInitCtx;

    if (conf) {
        const char *output_s = ConfNodeLookupChildValue(conf, "type");
        if (output_s != NULL) {
            if (strcmp(output_s, "file") == 0) {
                json_ctx->json_out = ALERT_FILE;
            } else if (strcmp(output_s, "syslog") == 0) {
                json_ctx->json_out = ALERT_SYSLOG;
            } else if (strcmp(output_s, "unix_dgram") == 0) {
                json_ctx->json_out = ALERT_UNIX_DGRAM;
            } else if (strcmp(output_s, "unix_stream") == 0) {
                json_ctx->json_out = ALERT_UNIX_STREAM;
            } else {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "Invalid JSON output option: %s", output_s);
                exit(EXIT_FAILURE);
            }
        }

        if (json_ctx->json_out == ALERT_FILE || json_ctx->json_out == ALERT_UNIX_DGRAM || json_ctx->json_out == ALERT_UNIX_STREAM) {

            if (SCConfLogOpenGeneric(conf, json_ctx->file_ctx, DEFAULT_LOG_FILENAME) < 0) {
                LogFileFreeCtx(json_ctx->file_ctx);
                SCFree(json_ctx);
                SCFree(output_ctx);
                return NULL;
            }
            OutputRegisterFileRotationFlag(&json_ctx->file_ctx->rotation_flag);

            const char *format_s = ConfNodeLookupChildValue(conf, "format");
            if (format_s != NULL) {
                if (strcmp(format_s, "indent") == 0) {
                    json_ctx->format = INDENT;
                } else if (strcmp(format_s, "compact") == 0) {
                    json_ctx->format = COMPACT;
                } else {
                    SCLogError(SC_ERR_INVALID_ARGUMENT,
                               "Invalid JSON format option: %s", format_s);
                    exit(EXIT_FAILURE);
                }
            }
        } else if (json_out == ALERT_SYSLOG) {
            const char *facility_s = ConfNodeLookupChildValue(conf, "facility");
            if (facility_s == NULL) {
                facility_s = DEFAULT_ALERT_SYSLOG_FACILITY_STR;
            }

            int facility = SCMapEnumNameToValue(facility_s, SCSyslogGetFacilityMap());
            if (facility == -1) {
                SCLogWarning(SC_ERR_INVALID_ARGUMENT, "Invalid syslog facility: \"%s\","
                        " now using \"%s\" as syslog facility", facility_s,
                        DEFAULT_ALERT_SYSLOG_FACILITY_STR);
                facility = DEFAULT_ALERT_SYSLOG_FACILITY;
            }

            const char *level_s = ConfNodeLookupChildValue(conf, "level");
            if (level_s != NULL) {
                int level = SCMapEnumNameToValue(level_s, SCSyslogGetLogLevelMap());
                if (level != -1) {
                    alert_syslog_level = level;
                }
            }

            const char *ident = ConfNodeLookupChildValue(conf, "identity");
            /* if null we just pass that to openlog, which will then
             * figure it out by itself. */

            openlog(ident, LOG_PID|LOG_NDELAY, facility);

        }

        const char *sensor_id_s = ConfNodeLookupChildValue(conf, "sensor-id");
        if (sensor_id_s != NULL) {
            if (ByteExtractStringUint64((uint64_t *)&sensor_id, 10, 0, sensor_id_s) == -1) {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "Failed to initialize JSON output, "
                           "invalid sensor-is: %s", sensor_id_s);
                exit(EXIT_FAILURE);
            }
        }

        format = json_ctx->format;
        json_out = json_ctx->json_out;
    }

    SCLogInfo("returning output_ctx %p", output_ctx);
    return output_ctx;
}

static void OutputJsonDeInitCtx(OutputCtx *output_ctx)
{
    OutputJsonCtx *json_ctx = (OutputJsonCtx *)output_ctx->data;
    LogFileCtx *logfile_ctx = json_ctx->file_ctx;
    OutputUnregisterFileRotationFlag(&logfile_ctx->rotation_flag);
    LogFileFreeCtx(logfile_ctx);
    SCFree(json_ctx);
    SCFree(output_ctx);
}

/*------------------------------Unittests-------------------------------------*/

#ifdef UNITTESTS

#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for AlertFastLog API.
 */
void OutputJsonRegisterTests(void)
{

#ifdef UNITTESTS

#endif /* UNITTESTS */

}
#endif
