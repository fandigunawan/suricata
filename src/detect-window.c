/**
 * \file
 * \author Pablo Rincon Crespo <pablo.rincon.crespo@gmail.com>
 *
 * WINDOW tcp window option, part of the detection engine.
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "detect-window.h"
#include "flow.h"
#include "flow-var.h"

#include "util-unittest.h"

/**
 * \brief Regex for parsing our window option
 */
#define PARSE_REGEX  "^\\s*([!])?\\s*([0-9]{1,9}+)\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectWindowMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectWindowSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void DetectWindowRegisterTests(void);
void DetectWindowFree(void *);

/**
 * \brief Registration function for window: keyword
 */
void DetectWindowRegister (void) {
    sigmatch_table[DETECT_WINDOW].name = "window";
    sigmatch_table[DETECT_WINDOW].Match = DetectWindowMatch;
    sigmatch_table[DETECT_WINDOW].Setup = DetectWindowSetup;
    sigmatch_table[DETECT_WINDOW].Free  = DetectWindowFree;
    sigmatch_table[DETECT_WINDOW].RegisterTests = DetectWindowRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

	#ifdef WINDOW_DEBUG
	printf("detect-window: Registering window rule option\n");
	#endif

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        printf("pcre compile of \"%s\" failed at offset %" PRId32 ": %s\n", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        printf("pcre study failed: %s\n", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/**
 * \brief This function is used to match the window size on a packet
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectWindowData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectWindowMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m)
{
    int ret=0;
    DetectWindowData *wd = (DetectWindowData *)m->ctx;

    if(wd != NULL)
    {

        /**
         * To match a packet with a widow size rule, we need a TCP packet,
         * and we must look if the size is negated or not
         */
        if (!(PKT_IS_TCP(p))) {
            return 0;
        }

        if((!wd->negated && wd->size==TCP_GET_WINDOW(p)) || (wd->negated && wd->size!=TCP_GET_WINDOW(p)))
	    {
		    #ifdef WINDOW_DEBUG
            printf("detect-window: packet is TCP Proto and matched with packet window size %d and rule option size %d negated: %d!!!\n", TCP_GET_WINDOW(p), wd->size, wd->negated);
		    #endif
		    ret=1;
        }
    }

    return ret;
}

/**
 * \brief This function is used to parse window options passed via window: keyword
 *
 * \param windowstr Pointer to the user provided window options (negation! and size)
 *
 * \retval wd pointer to DetectWindowData on success
 * \retval NULL on failure
 */
DetectWindowData *DetectWindowParse (char *windowstr)
{
    DetectWindowData *wd = NULL;
    char *args[3] = {NULL,NULL,NULL}; /* PR: Why PCRE MAX_SUBSTRING must be multiple of 3? */
	#define MAX_SUBSTRINGS 30

    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];


    ret = pcre_exec(parse_regex, parse_regex_study, windowstr, strlen(windowstr), 0, 0, ov, MAX_SUBSTRINGS);

    if (ret < 1 || ret > 3) {
        goto error;
    }

    wd = malloc(sizeof(DetectWindowData));
    if (wd == NULL) {
        printf("DetectWindowParse malloc failed\n");
        goto error;
    }


    if (ret > 1) {
        const char *str_ptr;
        res = pcre_get_substring((char *)windowstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            printf("DetectWindowParse: pcre_get_substring failed\n");
            goto error;
        }
        args[0] = (char *)str_ptr;
        // Detect if it's negated
        if(args[0][0]=='!')
            wd->negated=1;
        else
            wd->negated=0;

#ifdef WINDOW_DEBUG
        if(wd->negated)
            printf("detect-window: Negation: %s\n", windowstr);
#endif

        if (ret > 2) {
            res = pcre_get_substring((char *)windowstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
            if (res < 0) {
                printf("DetectWindowParse: pcre_get_substring failed\n");
                goto error;
            }
            wd->size = atoi((char *)str_ptr);

            // Get the window size if it's a valid value (in packets, we should alert if this doesn't happend from decode)
            if(wd->size > MAX_WINDOW_VALUE)
            {
                goto error;
            }

#ifdef WINDOW_DEBUG
            printf("detect-window: window size-> %u\n", wd->size);
#endif
        }
    }

	int i=0;
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) free(args[i]);
    }
    return wd;

error:
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) free(args[i]);
    }
    if (wd != NULL) DetectWindowFree(wd);
    return NULL;

}

/**
 * \brief this function is used to add the parsed window sizedata into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param windowstr pointer to the user provided window options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectWindowSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *windowstr)
{
    DetectWindowData *wd = NULL;
    SigMatch *sm = NULL;

    wd = DetectWindowParse(windowstr);
    if (wd == NULL) goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_WINDOW;
    sm->ctx = (void *)wd;

    SigMatchAppend(s,m,sm);

    return 0;

error:
    if (wd != NULL) DetectWindowFree(wd);
    if (sm != NULL) free(sm);
    return -1;

}

/**
 * \brief this function will free memory associated with DetectWindowData
 *
 * \param wd pointer to DetectWindowData
 */
void DetectWindowFree(void *ptr) {
    DetectWindowData *wd = (DetectWindowData *)ptr;
    free(wd);
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test DetectWindowTestParse01 is a test to make sure that we set the size correctly
 *  when given valid window opt
 */
int DetectWindowTestParse01 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("35402");
    if (wd != NULL &&wd->size==35402) {
        DetectWindowFree(wd);
        result = 1;
    }

    return result;
}

/**
 * \test DetectWindowTestParse02 is a test for setting the window opt negated
 */
int DetectWindowTestParse02 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("!35402");
    if (wd != NULL) {
        if (wd->negated == 1 && wd->size==35402) {
            result = 1;
        } else {
            printf("expected wd->negated=1 and wd->size=35402\n");
        }
        DetectWindowFree(wd);
    }

    return result;
}

/**
 * \test DetectWindowTestParse03 is a test to check for an empty value
 */
int DetectWindowTestParse03 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("");
    if (wd == NULL) {
        result = 1;
    } else {
        printf("expected a NULL pointer (It was an empty string)\n");
    }
    DetectWindowFree(wd);

    return result;
}

/**
 * \test DetectWindowTestParse03 is a test to check for a big value
 */
int DetectWindowTestParse04 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;
    wd = DetectWindowParse("1235402");
    if (wd != NULL) {
        printf("expected a NULL pointer (It was exceeding the MAX window size)\n");
        DetectWindowFree(wd);
    }else
        result=1;

    return result;
}


/**
 * \test DetectWindowTestPacket01 is a test to check window with constructed packets, expecting to match a negated size
 * Parse Window Data: if th_win is not 55455 it should Match!
 * The packet is less than 55455 so it must match
 */

int DetectWindowTestPacket01 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;


    wd = DetectWindowParse("!55455");
    if (wd == NULL)
    {
        printf("DetectWindowTestPacket01: expected a DetectWindowData pointer (got NULL)\n");
        return 0;
    }
    /* Buid and decode the packet */
    uint8_t raw_eth [] = {
    0x00,0x25,0x00,0x9e,0xfa,0xfe,0x00,0x02,0xcf,0x74,0xfe,0xe1,0x08,0x00,0x45,0x00
	,0x01,0xcc,0xcb,0x91,0x00,0x00,0x34,0x06,0xdf,0xa8,0xd1,0x55,0xe3,0x67,0xc0,0xa8
	,0x64,0x8c,0x00,0x50,0xc0,0xb7,0xd1,0x11,0xed,0x63,0x81,0xa9,0x9a,0x05,0x80,0x18
	,0x00,0x75,0x0a,0xdd,0x00,0x00,0x01,0x01,0x08,0x0a,0x09,0x8a,0x06,0xd0,0x12,0x21
	,0x2a,0x3b,0x48,0x54,0x54,0x50,0x2f,0x31,0x2e,0x31,0x20,0x33,0x30,0x32,0x20,0x46
	,0x6f,0x75,0x6e,0x64,0x0d,0x0a,0x4c,0x6f,0x63,0x61,0x74,0x69,0x6f,0x6e,0x3a,0x20
	,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77,0x77,0x77,0x2e,0x67,0x6f,0x6f,0x67,0x6c
	,0x65,0x2e,0x65,0x73,0x2f,0x0d,0x0a,0x43,0x61,0x63,0x68,0x65,0x2d,0x43,0x6f,0x6e
	,0x74,0x72,0x6f,0x6c,0x3a,0x20,0x70,0x72,0x69,0x76,0x61,0x74,0x65,0x0d,0x0a,0x43
	,0x6f,0x6e,0x74,0x65,0x6e,0x74,0x2d,0x54,0x79,0x70,0x65,0x3a,0x20,0x74,0x65,0x78
	,0x74,0x2f,0x68,0x74,0x6d,0x6c,0x3b,0x20,0x63,0x68,0x61,0x72,0x73,0x65,0x74,0x3d
	,0x55,0x54,0x46,0x2d,0x38,0x0d,0x0a,0x44,0x61,0x74,0x65,0x3a,0x20,0x4d,0x6f,0x6e
	,0x2c,0x20,0x31,0x34,0x20,0x53,0x65,0x70,0x20,0x32,0x30,0x30,0x39,0x20,0x30,0x38
	,0x3a,0x34,0x38,0x3a,0x33,0x31,0x20,0x47,0x4d,0x54,0x0d,0x0a,0x53,0x65,0x72,0x76
	,0x65,0x72,0x3a,0x20,0x67,0x77,0x73,0x0d,0x0a,0x43,0x6f,0x6e,0x74,0x65,0x6e,0x74
	,0x2d,0x4c,0x65,0x6e,0x67,0x74,0x68,0x3a,0x20,0x32,0x31,0x38,0x0d,0x0a,0x0d,0x0a
	,0x3c,0x48,0x54,0x4d,0x4c,0x3e,0x3c,0x48,0x45,0x41,0x44,0x3e,0x3c,0x6d,0x65,0x74
	,0x61,0x20,0x68,0x74,0x74,0x70,0x2d,0x65,0x71,0x75,0x69,0x76,0x3d,0x22,0x63,0x6f
	,0x6e,0x74,0x65,0x6e,0x74,0x2d,0x74,0x79,0x70,0x65,0x22,0x20,0x63,0x6f,0x6e,0x74
	,0x65,0x6e,0x74,0x3d,0x22,0x74,0x65,0x78,0x74,0x2f,0x68,0x74,0x6d,0x6c,0x3b,0x63
	,0x68,0x61,0x72,0x73,0x65,0x74,0x3d,0x75,0x74,0x66,0x2d,0x38,0x22,0x3e,0x0a,0x3c
	,0x54,0x49,0x54,0x4c,0x45,0x3e,0x33,0x30,0x32,0x20,0x4d,0x6f,0x76,0x65,0x64,0x3c
	,0x2f,0x54,0x49,0x54,0x4c,0x45,0x3e,0x3c,0x2f,0x48,0x45,0x41,0x44,0x3e,0x3c,0x42
	,0x4f,0x44,0x59,0x3e,0x0a,0x3c,0x48,0x31,0x3e,0x33,0x30,0x32,0x20,0x4d,0x6f,0x76
	,0x65,0x64,0x3c,0x2f,0x48,0x31,0x3e,0x0a,0x54,0x68,0x65,0x20,0x64,0x6f,0x63,0x75
	,0x6d,0x65,0x6e,0x74,0x20,0x68,0x61,0x73,0x20,0x6d,0x6f,0x76,0x65,0x64,0x0a,0x3c
	,0x41,0x20,0x48,0x52,0x45,0x46,0x3d,0x22,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77
	,0x77,0x77,0x2e,0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2e,0x65,0x73,0x2f,0x22,0x3e,0x68
	,0x65,0x72,0x65,0x3c,0x2f,0x41,0x3e,0x2e,0x0d,0x0a,0x3c,0x2f,0x42,0x4f,0x44,0x59
	,0x3e,0x3c,0x2f,0x48,0x54,0x4d,0x4c,0x3e,0x0d,0x0a };

    Packet q;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&q, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    FlowInitConfig(FLOW_QUIET);
    DecodeEthernet(&tv, &dtv, &q, raw_eth, sizeof(raw_eth), NULL);
    FlowShutdown();

    Packet *p=&q;

    if (!(PKT_IS_TCP(p))) {
        printf("detect-window: TestPacket01: Packet is not TCP\n");
        return 0;
    }


    /* We dont need DetectEngineThreadCtx inside DetectWindowMatch, its just to pass it to
     the function, since this is a test for this option
     Also a Signature is not really needed
    */

    DetectEngineThreadCtx *det_ctx=NULL;
    Signature *s=NULL;

    SigMatch m;
    m.ctx=wd;

    /* Now that we have what we need, just try to Match! */
    result=DetectWindowMatch (&tv, det_ctx, p, s, &m);

    return result;
}


/**
 * \test DetectWindowTestPacket02 is a test to check window with constructed packets, expecting to match a size
 * Parse Window Data: if th_win is 190 it should Match!
 * The packet tcp window is 190 so it must match
 */

int DetectWindowTestPacket02 (void) {
    int result = 0;
    DetectWindowData *wd = NULL;

    wd = DetectWindowParse("117");
    if (wd == NULL)
    {
        printf("DetectWindowTestPacket02: expected a DetectWindowData pointer (got NULL)\n");
        return 0;
    }

    uint8_t raw_eth [] = {
    0x00,0x25,0x00,0x9e,0xfa,0xfe,0x00,0x02,0xcf,0x74,0xfe,0xe1,0x08,0x00,0x45,0x00
	,0x01,0xcc,0xcb,0x91,0x00,0x00,0x34,0x06,0xdf,0xa8,0xd1,0x55,0xe3,0x67,0xc0,0xa8
	,0x64,0x8c,0x00,0x50,0xc0,0xb7,0xd1,0x11,0xed,0x63,0x81,0xa9,0x9a,0x05,0x80,0x18
	,0x00,0x75,0x0a,0xdd,0x00,0x00,0x01,0x01,0x08,0x0a,0x09,0x8a,0x06,0xd0,0x12,0x21
	,0x2a,0x3b,0x48,0x54,0x54,0x50,0x2f,0x31,0x2e,0x31,0x20,0x33,0x30,0x32,0x20,0x46
	,0x6f,0x75,0x6e,0x64,0x0d,0x0a,0x4c,0x6f,0x63,0x61,0x74,0x69,0x6f,0x6e,0x3a,0x20
	,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77,0x77,0x77,0x2e,0x67,0x6f,0x6f,0x67,0x6c
	,0x65,0x2e,0x65,0x73,0x2f,0x0d,0x0a,0x43,0x61,0x63,0x68,0x65,0x2d,0x43,0x6f,0x6e
	,0x74,0x72,0x6f,0x6c,0x3a,0x20,0x70,0x72,0x69,0x76,0x61,0x74,0x65,0x0d,0x0a,0x43
	,0x6f,0x6e,0x74,0x65,0x6e,0x74,0x2d,0x54,0x79,0x70,0x65,0x3a,0x20,0x74,0x65,0x78
	,0x74,0x2f,0x68,0x74,0x6d,0x6c,0x3b,0x20,0x63,0x68,0x61,0x72,0x73,0x65,0x74,0x3d
	,0x55,0x54,0x46,0x2d,0x38,0x0d,0x0a,0x44,0x61,0x74,0x65,0x3a,0x20,0x4d,0x6f,0x6e
	,0x2c,0x20,0x31,0x34,0x20,0x53,0x65,0x70,0x20,0x32,0x30,0x30,0x39,0x20,0x30,0x38
	,0x3a,0x34,0x38,0x3a,0x33,0x31,0x20,0x47,0x4d,0x54,0x0d,0x0a,0x53,0x65,0x72,0x76
	,0x65,0x72,0x3a,0x20,0x67,0x77,0x73,0x0d,0x0a,0x43,0x6f,0x6e,0x74,0x65,0x6e,0x74
	,0x2d,0x4c,0x65,0x6e,0x67,0x74,0x68,0x3a,0x20,0x32,0x31,0x38,0x0d,0x0a,0x0d,0x0a
	,0x3c,0x48,0x54,0x4d,0x4c,0x3e,0x3c,0x48,0x45,0x41,0x44,0x3e,0x3c,0x6d,0x65,0x74
	,0x61,0x20,0x68,0x74,0x74,0x70,0x2d,0x65,0x71,0x75,0x69,0x76,0x3d,0x22,0x63,0x6f
	,0x6e,0x74,0x65,0x6e,0x74,0x2d,0x74,0x79,0x70,0x65,0x22,0x20,0x63,0x6f,0x6e,0x74
	,0x65,0x6e,0x74,0x3d,0x22,0x74,0x65,0x78,0x74,0x2f,0x68,0x74,0x6d,0x6c,0x3b,0x63
	,0x68,0x61,0x72,0x73,0x65,0x74,0x3d,0x75,0x74,0x66,0x2d,0x38,0x22,0x3e,0x0a,0x3c
	,0x54,0x49,0x54,0x4c,0x45,0x3e,0x33,0x30,0x32,0x20,0x4d,0x6f,0x76,0x65,0x64,0x3c
	,0x2f,0x54,0x49,0x54,0x4c,0x45,0x3e,0x3c,0x2f,0x48,0x45,0x41,0x44,0x3e,0x3c,0x42
	,0x4f,0x44,0x59,0x3e,0x0a,0x3c,0x48,0x31,0x3e,0x33,0x30,0x32,0x20,0x4d,0x6f,0x76
	,0x65,0x64,0x3c,0x2f,0x48,0x31,0x3e,0x0a,0x54,0x68,0x65,0x20,0x64,0x6f,0x63,0x75
	,0x6d,0x65,0x6e,0x74,0x20,0x68,0x61,0x73,0x20,0x6d,0x6f,0x76,0x65,0x64,0x0a,0x3c
	,0x41,0x20,0x48,0x52,0x45,0x46,0x3d,0x22,0x68,0x74,0x74,0x70,0x3a,0x2f,0x2f,0x77
	,0x77,0x77,0x2e,0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2e,0x65,0x73,0x2f,0x22,0x3e,0x68
	,0x65,0x72,0x65,0x3c,0x2f,0x41,0x3e,0x2e,0x0d,0x0a,0x3c,0x2f,0x42,0x4f,0x44,0x59
	,0x3e,0x3c,0x2f,0x48,0x54,0x4d,0x4c,0x3e,0x0d,0x0a };

    Packet q;
    ThreadVars tv;
    DecodeThreadVars dtv;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&q, 0, sizeof(Packet));
    memset(&dtv, 0, sizeof(DecodeThreadVars));

    FlowInitConfig(FLOW_QUIET);
    DecodeEthernet(&tv, &dtv, &q, raw_eth, sizeof(raw_eth), NULL);
    FlowShutdown();

    Packet *p=&q;

    if (!(PKT_IS_TCP(p))) {
        printf("DetectWindowTestPacket02: TestPacket01: Packet is not TCP\n");
        return 0;
    }


    DetectEngineThreadCtx *det_ctx=NULL;
    Signature *s=NULL;
    SigMatch m;
    m.ctx=wd;

    /* Now that we have what we need, just try to Match! */
    result=DetectWindowMatch (&tv, det_ctx, p, s, &m);

    return result;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectWindow
 */
void DetectWindowRegisterTests(void) {
    #ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("DetectWindowTestParse01", DetectWindowTestParse01, 1);
    UtRegisterTest("DetectWindowTestParse02", DetectWindowTestParse02, 1);
    UtRegisterTest("DetectWindowTestParse03", DetectWindowTestParse03, 1);
    UtRegisterTest("DetectWindowTestParse04", DetectWindowTestParse04, 1);
    UtRegisterTest("DetectWindowTestPacket01"  , DetectWindowTestPacket01  , 1);
    UtRegisterTest("DetectWindowTestPacket02"  , DetectWindowTestPacket02  , 1);
    #endif /* UNITTESTS */
}
