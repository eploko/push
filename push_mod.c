/*
 * $Id$
 * 
 * APNs support module
 *
 * Copyright (C) 2013 Volodymyr Tarasenko
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <stdio.h>
#include <string.h>
#include <memory.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "../../core/sr_module.h"
#include "../../core/trim.h"
#include "../../core/dprint.h"
#include "../../core/mem/mem.h"
#include "../../core/parser/parse_to.h"
#include "../../core/parser/parse_uri.h"
#include "../../core/cfg/cfg_struct.h"

#include "push_mod.h"
#include "push.h"
#include "push_common.h"
#include "push_ssl_utils.h"
#include "apns_feedback.h"

MODULE_VERSION

static int mod_init(void);
static void destroy(void);
static int child_init(int rank);
//static void start_feedback_service();

static void feedback_service(int fd);
static void stop_feedback_service();

static int w_push_request(struct sip_msg *rq, const char *device_token);
static int w_push_message(struct sip_msg *rq, const char *device_token, const char *message);
static int w_push_custom_message(struct sip_msg *rq, const char *device_token, const char *message, const char* custom);
static int w_push_register(struct sip_msg *rq, const char *device_token);
static int w_push_msg(struct sip_msg *rq, const char *msg);
static int w_push_custom_msg(struct sip_msg *rq, const char *msg, const char* custom);
static int w_push_status(struct sip_msg *rq, const char* device_token, int code);

static int push_api_fixup(void** param, int param_no);
static int free_push_api_fixup(void** param, int param_no);

static void timer_cleanup_function(unsigned int ticks, void* param);

/* ----- PUSH variables ----------- */
/*@{*/

static char *apns_cert_file = 0;
static char *apns_cert_key  = 0;
static char *apns_cert_ca   = 0;
static char *apns_server = 0;
static char *apns_feedback_server = "feedback.sandbox.push.apple.com";
static char *apns_alert = "You have a call";
static int   apns_badge = -1;
static char *apns_sound = 0;
static int apns_feedback_port = 2196;
static int apns_port;
static int push_flag = 0;
static int apns_read_timeout = 100000;
static int apns_feedback_read_timeout = 500000;

static char *push_db = 0;
static char *push_table = "push_apns";

///void *rh;

/*@}*/

static PushServer* apns = 0;

static cmd_export_t cmds[] = {
    {"push_request", (cmd_function)w_push_request, 1,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},
    {"push_request", (cmd_function)w_push_message, 2,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},
    {"push_request", (cmd_function)w_push_custom_message, 3,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},
    {"push_register", (cmd_function)w_push_register, 1,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},
    {"push_message", (cmd_function)w_push_msg, 1,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},
    {"push_message", (cmd_function)w_push_custom_msg, 2,
     push_api_fixup, free_push_api_fixup,
     ANY_ROUTE},

    {0, 0, 0, 0, 0, 0}
};

static param_export_t params[] = {
    {"push_db",            STR_PARAM, &push_db            },
    {"push_table",         STR_PARAM, &push_table         },
    {"push_flag",          INT_PARAM, &push_flag          },
    {"push_apns_cert",     STR_PARAM, &apns_cert_file     },
    {"push_apns_key",      STR_PARAM, &apns_cert_key      },
    {"push_apns_cafile",   STR_PARAM, &apns_cert_ca       },
    {"push_apns_server",   STR_PARAM, &apns_server        },
    {"push_apns_port",     INT_PARAM, &apns_port          },
    {"push_apns_alert",    STR_PARAM, &apns_alert         },
    {"push_apns_sound",    STR_PARAM, &apns_sound         },
    {"push_apns_badge",    INT_PARAM, &apns_badge         },
    {"push_apns_rtimeout", INT_PARAM, &apns_read_timeout  },
    {"push_apns_feedback_server",   STR_PARAM, &apns_feedback_server },
    {"push_apns_feedback_port",     INT_PARAM, &apns_feedback_port   },
    {"push_apns_feedback_rtimeout", INT_PARAM, &apns_feedback_read_timeout },
    {0,0,0}
};

/* static proc_export_t procs[] = { */
/*         {"Feedback service",  0,  0, feedback_service, 1 }, */
/*         {0,0,0,0,0} */
/* }; */


struct module_exports exports= {
    "push",
    DEFAULT_DLFLAGS, /* dlopen flags */
    cmds,       /* exported functions */
    params,     /* exported params */
    0,          /* exported statistics */
    0,          /* exported MI functions */
    0,          /* exported pseudo-variables */
    0,          /* extra processes, depricated? */
    mod_init,   /* initialization module */
    0,          /* response function */
    destroy,    /* destroy function */
    child_init  /* per-child init function */
};

static int pipefd[2];

/************************** SIP helper functions ****************************/
#define USERNAME_MAX_SIZE      64
#define DOMAIN_MAX_SIZE        128

static int
get_callid(struct sip_msg* msg, str *cid)
{
    if (msg->callid == NULL) {
        if (parse_headers(msg, HDR_CALLID_F, 0) == -1) {
            LM_ERR("cannot parse Call-ID header\n");
            return -1;
        }
        if (msg->callid == NULL) {
            LM_ERR("missing Call-ID header\n");
            return -1;
        }
    }

    *cid = msg->callid->body;

    trim(cid);

    return 0;
}

#define MAX_AOR_LEN 256

/*! \brief
 * Extract Address of Record
 */
int extract_aor(str* _uri, str* _a, sip_uri_t *_pu)
{
    static char aor_buf[MAX_AOR_LEN];
//  str tmp;
    sip_uri_t turi;
    sip_uri_t *puri;
//  int user_len;
    str *uri;
//  str realm_prefix = {0};
    
    memset(aor_buf, 0, MAX_AOR_LEN);
    uri=_uri;

    if(_pu!=NULL)
        puri = _pu;
    else
        puri = &turi;

    if (parse_uri(uri->s, uri->len, puri) < 0) {
        LM_ERR("failed to parse AoR [%.*s]\n", uri->len, uri->s);
        return -1;
    }
    
    if ( (puri->user.len + puri->host.len + 1) > MAX_AOR_LEN
         || puri->user.len > USERNAME_MAX_SIZE
         ||  puri->host.len > DOMAIN_MAX_SIZE ) {
        LM_ERR("Address Of Record too long\n");
        return -2;
    }

    _a->s = aor_buf;
    _a->len = puri->user.len;

    if (un_escape(&puri->user, _a) < 0) {
        LM_ERR("failed to unescape username\n");
        return -3;
    }

//  user_len = _a->len;

    /* if (reg_use_domain) { */
    /*  if (user_len) */
    /*      aor_buf[_a->len++] = '@'; */
    /*  /\* strip prefix (if defined) *\/ */
    /*  realm_prefix.len = cfg_get(registrar, registrar_cfg, realm_pref).len; */
    /*  if(realm_prefix.len>0) { */
    /*      realm_prefix.s = cfg_get(registrar, registrar_cfg, realm_pref).s; */
    /*      LM_DBG("realm prefix is [%.*s]\n", realm_prefix.len, */
    /*                (realm_prefix.len>0)?realm_prefix.s:""); */
    /*  } */
    /*  if (realm_prefix.len>0 */
    /*         && realm_prefix.len<puri->host.len */
    /*         && (memcmp(realm_prefix.s, puri->host.s, realm_prefix.len)==0)) */
    /*  { */
    /*      memcpy(aor_buf + _a->len, puri->host.s + realm_prefix.len, */
    /*                puri->host.len - realm_prefix.len); */
    /*      _a->len += puri->host.len - realm_prefix.len; */
    /*  } else { */
    /*      memcpy(aor_buf + _a->len, puri->host.s, puri->host.len); */
    /*      _a->len += puri->host.len; */
    /*  } */
    /* } */

    /* if (cfg_get(registrar, registrar_cfg, case_sensitive) && user_len) { */
    /*  tmp.s = _a->s + user_len + 1; */
    /*  tmp.len = _a->s + _a->len - tmp.s; */
    /*  strlower(&tmp); */
    /* } else { */
    strlower(_a);
    /* } */

    return 0;
}


/************************** INTERFACE functions ****************************/

static int mod_init( void )
{
    LM_DBG("Init Push module\n");

    apns = create_push_server(apns_cert_file, 
                              apns_cert_key, 
                              apns_cert_ca, 
                              apns_server, 
                              apns_port);
    if (NULL == apns)
    {
        LM_ERR("Cannot create push structure, failed");
        return -1;
    }

    apns->read_timeout = apns_read_timeout;

    if ((push_db) && (-1 == push_check_db(apns, push_db, push_table)))
    {
        LM_ERR("Cannot connect database, failed");
        return -1;
    }

    ssl_init();

    register_timer(timer_cleanup_function,
                   apns,
                   2);

#ifdef ENABLE_FEEDBACK_SERVICE
    register_procs(1);
#endif

    /* do all staff in child init*/

    return 0;
}


static int child_init(int rank)
{
    LM_DBG("Child Init Push module\n");

#ifdef ENABLE_FEEDBACK_SERVICE
    if (rank == PROC_MAIN) 
    {
        pid_t pid;
        if (-1 == pipe(pipefd))
        {
            LM_ERR("cannot create feedback command pipe\n");
            return -1;
        }
        
        pid = fork_process(PROC_NOCHLDINIT, "MY PROC DESCRIPTION", 1);
        if (pid < 0)
            return -1; /* error */

        if(pid == 0)
        {
            /* child */
            close(pipefd[1]);
    
            /* initialize the config framework */
            if (cfg_child_init())
            {
                LM_ERR("cfg child init failed\n");
                return -1;
            }
            LM_DBG("Start feedback server");
            feedback_service(pipefd[0]);
            
            exit(0);
        }
        close(pipefd[0]);
    }
#endif

    if ((push_db) && (-1 == push_connect_db(apns, push_db, push_table, rank)))
    {
        LM_ERR("Cannot connect database, failed");
        return -1;
    }

    if (push_flag == ConnectEstablish)
        return establish_ssl_connection(apns);

    /* if (rank==PROC_INIT || rank==PROC_MAIN || rank==PROC_TCP_MAIN) */
    /*  return 0; /\* do nothing for the main process *\/ */

    return 0;
}


static void destroy(void)
{
    LM_DBG("Push destroy\n");
#ifdef ENABLE_FEEDBACK_SERVICE
    stop_feedback_service();
#endif

    destroy_push_server(apns);


    //ssl_shutdown();
}


static int push_api_fixup(void** param, int param_no)
{
    char *p;

    LM_DBG("Push push_api_fixup, param %d\n", param_no);

    p = (char*)*param;
    if (p==0 || p[0]==0) {
        LM_ERR("first parameter is empty\n");
        return E_SCRIPT;
    }

    return 0;
}


static int free_push_api_fixup(void** param, int param_no)
{
    LM_DBG("Push free_push_api_fixup, param %d\n", param_no);
    /* if(*param) */
    /* { */
    /*  pkg_free(*param); */
    /*  *param = 0; */
    /* } */

    return 0;
}


static int w_push_request(struct sip_msg *rq, const char *device_token)
{
//    str *ruri;
    str  callid;

    size_t token_len = strlen(device_token);
    LM_DBG("Push request started, token %s\n", device_token);
    if (token_len != DEVICE_TOKEN_LEN_STR)
    {
        LM_ERR("Device token length wrong, reject push\n");
        return -1;
    }

    // Working with sip message:
//    ruri = GET_RURI(rq);
    if (-1 == get_callid(rq, &callid))
    {
        LM_ERR("Geting CallID failed, reject push\n");
        return -1;
    }

    if (-1 == push_send(apns,  device_token, apns_alert, NULL, apns_badge))
    {
        LM_ERR("Push notification failed, call id %s, device token %s\n",
               callid.s, device_token);
        return -1;
    }

    LM_DBG("Success\n");

    return 1;
}

static int w_push_message(struct sip_msg *rq, const char *device_token, const char *message)
{
    return w_push_custom_message(rq, device_token, message, NULL);
}

static int w_push_custom_message(struct sip_msg *rq, const char *device_token, const char *message, const char* custom)
{
//    str *ruri;
    str  callid;

    size_t token_len = strlen(device_token);
    LM_DBG("Push request started, token %s, message %s\n", device_token, message);
    if (token_len != DEVICE_TOKEN_LEN_STR)
    {
        LM_ERR("Device token length wrong, reject push\n");
        return -1;
    }

    // Working with sip message:
    //  ruri = GET_RURI(rq);
    if (-1 == get_callid(rq, &callid))
    {
        LM_ERR("Geting CallID failed, reject push\n");
        return -1;
    }

    if (-1 == push_send(apns,  device_token, message, custom, apns_badge))
    {
        LM_ERR("Push notification failed, call id %s, device token %s, message %s\n",
               callid.s, device_token, message);
        return -1;
    }

    LM_DBG("Success\n");

    return 1;
}


static int w_push_register(struct sip_msg *rq, const char *device_token)
{
    str  callid;
    str uri, aor;

    size_t token_len = strlen(device_token);
    LM_DBG("Push request started, token %s\n", device_token);
    if (token_len != DEVICE_TOKEN_LEN_STR)
    {
        LM_ERR("Device token length wrong, reject push\n");
        return -1;
    }
    // Working with sip message:
    if (-1 == get_callid(rq, &callid))
    {
        LM_ERR("Geting CallID failed, reject push\n");
        return -1;
    }

    uri = get_to(rq)->uri; //rq->first_line.u.request.uri;

    LM_DBG("Push request, URI %s, token %s\n", uri.s, device_token);

    if (extract_aor(&uri, &aor, NULL) < 0) {
        LM_ERR("failed to extract address of record\n");
        return -1;
    }
    LM_DBG("Push request, AOR %s, token %s\n", aor.s, device_token);

    if (-1 == push_register_device(apns, aor.s, device_token, &callid, push_table))
    {
        LM_ERR("Push device registration failed, call id %s, device token %s\n",
               callid.s, device_token);
        return -1;
    }

    LM_DBG("Success\n");

    return 1;
}


static int w_push_msg(struct sip_msg *rq, const char* msg)
{
    return w_push_custom_msg(rq, msg, NULL);
}

static int w_push_custom_msg(struct sip_msg *rq, const char* msg, const char* custom)
{
    char* device_token = NULL;
    to_body_t* to;
    str /*uri,*/ aor;

    str  callid;

    if (-1 == get_callid(rq, &callid))
    {
        LM_ERR("Geting CallID failed, reject push\n");
        return -1;
    }

    if (0 != parse_to_header(rq))
    {
        LM_ERR("Parsing TO header failed, reject push\n");
        return -1;
    }
    
    to = get_to(rq);

    if (extract_aor(&to->uri, &aor, NULL) < 0) 
    {
        LM_ERR("failed to extract address of record\n");
        return -1;
    }

    LM_DBG("Send push message, aor [%s], getting token...\n", aor.s);

    if (-1 == push_get_device(apns, aor.s, (const char **)&device_token, push_table))
    {
        LM_ERR("Push failed, cannot get device token, call id %s\n",
               callid.s);
        return -1;
    }

    LM_DBG("Sending push message, aor [%s], token [%s], msg [%s], badge [%d]...\n", 
           aor.s, device_token, msg, apns_badge);

    if (-1 == push_send(apns, device_token, msg, custom, apns_badge))
    {
        LM_ERR("Push notification failed, call id %s, device token %s, message %s\n",
               callid.s, device_token, msg);
        free(device_token);
        return -1;
    }

    free(device_token);

    LM_DBG("Success\n");

    return 1;
}


static int w_push_status(struct sip_msg *rq, const char* device_token, int code)
{
    return -1;
}


static void feedback_service(int fd)
{
#define FEEDBACK_MSG_LEN 38
    PushServer *feedback;

    feedback = create_push_server(apns_cert_file, 
                                  apns_cert_key, 
                                  apns_cert_ca, 
                                  apns_feedback_server, 
                                  apns_feedback_port);

    if (feedback == NULL)
    {
        LM_ERR("Cannot initiale feedback service");
        return;
    }

    feedback->read_timeout = apns_feedback_read_timeout;

    run_feedback(feedback, fd);
}


static void stop_feedback_service()
{
    char cmd = 'q';
    write(pipefd[1],&cmd, 1);

    close(pipefd[1]);
}

static void timer_cleanup_function(unsigned int ticks, void* param)
{
    PushServer *server = (PushServer*)param;

    push_check_status(server);
}
