/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2007-2013 Sourcefire, Inc.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/**
**  @file        sp_cvs.c
**
**  @author      Taimur Aslam
**  @author      Todd Wease
**
**  @brief       Decode and detect CVS vulnerabilities
**
**  This CVS detection plugin provides support for detecting published CVS vulnerabilities. The
**  vulnerabilities that can be detected are:
**  Bugtraq-10384, CVE-2004-0396: "Malformed Entry Modified and Unchanged flag insertion"
**
**  Detection Functions:
**
**  cvs: invalid-entry;
**
*/


#include <stdio.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <errno.h>

#include "snort_types.h"
#include "detection/treenodes.h"
#include "protocols/packet.h"
#include "parser.h"
#include "snort_debug.h"
#include "util.h"
#include "mstring.h"
#include "snort_bounds.h"
#include "profiler.h"
#include "fpdetect.h"
#include "sfhashfcn.h"
#include "detection/detection_defines.h"
#include "framework/ips_option.h"

#ifdef PERF_PROFILING
static THREAD_LOCAL PreprocStats cvsPerfStats;

static const char* s_name = "cvs";

static PreprocStats* cvs_get_profile(const char* key)
{
    if ( !strcmp(key, s_name) )
        return &cvsPerfStats;

    return nullptr;
}
#endif

#define CVS_CONFIG_DELIMITERS  " \t\n"

#define CVS_COMMAND_DELIMITER  '\n'
#define CVS_COMMAND_SEPARATOR  ' '

#define CVS_CONF_INVALID_ENTRY_STR  "invalid-entry"

#define CVS_NO_ALERT  0
#define CVS_ALERT     1

#define CVS_ENTRY_STR  "Entry"
#define CVS_ENTRY_VALID   0
#define CVS_ENTRY_INVALID 1

/* the types of vulnerabilities it will detect */
typedef enum _CvsTypes
{
    CVS_INVALID_ENTRY = 1,
    CVS_END_OF_ENUM
    
} CvsTypes;

typedef struct _CvsRuleOption
{
    CvsTypes type;

} CvsRuleOption;

/* represents a CVS command with argument */
typedef struct _CvsCommand
{
    const uint8_t  *cmd_str;        /* command string */
    int              cmd_str_len;
    const uint8_t  *cmd_arg;        /* command argument */
    int              cmd_arg_len;
    
} CvsCommand;

static int CvsDecode(const uint8_t *, uint16_t, CvsRuleOption *);
static void CvsGetCommand(const uint8_t *, const uint8_t *, CvsCommand *);
static int CvsCmdCompare(const char *, const uint8_t *, int);
static int CvsValidateEntry(const uint8_t *, const uint8_t *);
static void CvsGetEOL(const uint8_t *, const uint8_t *,
                      const uint8_t **, const uint8_t **);

class CvsOption : public IpsOption
{
public:
    CvsOption(const CvsRuleOption& c) :
        IpsOption(s_name)
    { config = c; };

    uint32_t hash() const;
    bool operator==(const IpsOption&) const;

    int eval(Packet*);

private:
    CvsRuleOption config;
};

//-------------------------------------------------------------------------
// class methods
//-------------------------------------------------------------------------

uint32_t CvsOption::hash() const
{
    uint32_t a,b,c;
    const CvsRuleOption *data = &config;

    a = data->type;
    b = 0;
    c = 0;

    mix_str(a,b,c,get_name());
    final(a,b,c);

    return c;
}

bool CvsOption::operator==(const IpsOption& ips) const
{
    if ( strcmp(get_name(), ips.get_name()) )
        return false;

    CvsOption& rhs = (CvsOption&)ips;
    CvsRuleOption *left = (CvsRuleOption*)&config;
    CvsRuleOption *right = (CvsRuleOption*)&rhs.config;

    if (left->type == right->type)
    {
        return true;
    }

    return false;
}

int CvsOption::eval(Packet *p)
{
    int ret;
    int rval = DETECTION_OPTION_NO_MATCH;
    CvsRuleOption *cvs_rule_option = &config;


    if (p == NULL)
    {
        return rval;
    }

    if ((p->tcph == NULL) || (p->data == NULL) || (p->dsize == 0))
    {
        return rval;
    }

    if (cvs_rule_option == NULL)
    {
        return rval;
    }

    DEBUG_WRAP(DebugMessage(DEBUG_PLUGIN, "CVS begin detection\n"););

    ret = CvsDecode(p->data, p->dsize, cvs_rule_option);

    if (ret == CVS_ALERT)
    {
        rval = DETECTION_OPTION_MATCH;
    }

    return rval;
}

//-------------------------------------------------------------------------
// helper methods
//-------------------------------------------------------------------------

static int CvsDecode(const uint8_t *data, uint16_t data_len,
                     CvsRuleOption *cvs_rule_option)
{
    const uint8_t *line, *end;
    const uint8_t *eol = NULL, *eolm = NULL;
    CvsCommand command;
    int ret;


    line = data;
    end = data + data_len;

    /* loop through data, analyzing a line at a time */
    while (line < end)
    {
        /* CVS commands are delimited by \n so break them up */
        CvsGetEOL(line, end, &eol, &eolm);

        /* Put command and argument into structure */
        CvsGetCommand(line, eolm, &command);

        /* shouldn't happen as long as line < end, but ... */
        if (command.cmd_str == NULL)
            return CVS_NO_ALERT;

        DEBUG_WRAP(DebugMessage(DEBUG_PLUGIN, "CVS command\n"
                                              "  comand: %.*s\n"
                                              "argument: %.*s\n",
                                command.cmd_str_len, (char *)command.cmd_str,
                                command.cmd_arg == NULL ? 4 : command.cmd_arg_len,
                                command.cmd_arg == NULL ? "none" : (char *)command.cmd_arg););

        switch (cvs_rule_option->type)
        {
            case CVS_INVALID_ENTRY:
                if (CvsCmdCompare(CVS_ENTRY_STR, command.cmd_str, command.cmd_str_len) == 0)
                {
                    ret = CvsValidateEntry(command.cmd_arg,
                                           (command.cmd_arg + command.cmd_arg_len));

                    if ((ret == CVS_ENTRY_INVALID)&&(eol < end))
                    {
                        return CVS_ALERT;
                    }
                }

                break;

            default:
                break;
        }

        line = eol;
    }

    return CVS_NO_ALERT;
}


/*
**  NAME
**    CvsCmdCompare
**       Compares two pointers to char to see if they are equal.
**       The first arg is NULL terminated.  The second is not and
**       it's length is passed in.
**
*/
/**
**  @return 0 if equal
**  @return 1 if not equal
**
*/

static int CvsCmdCompare(const char *cmd, const uint8_t *pkt_cmd, int pkt_cmd_len)
{
    if (((size_t)pkt_cmd_len == strlen(cmd)) &&
        (memcmp(pkt_cmd, cmd, pkt_cmd_len) == 0))
    {
        return 0;
    }

    return 1;
}


/*
**  NAME
**    CvsGetCommand
**       Takes a line and breaks it up into command and argument.
**       It modifies the data in the string by replacing the first
**       space character it sees with '\0'.  A pointer to the string
**       created by the replacement is put in the CvsCommand structure's
**       command member.  A pointer to the rest of the string after
**       the replacement '\0' is put into the structure's command
**       argument member.  If there isn't a space, the entire line
**       is put in the command and the command argument is set to
**       NULL.
**
*/
/**
**  @return None
**
*/

static void CvsGetCommand(const uint8_t *line, const uint8_t *end, CvsCommand *cmd)
{
    const uint8_t *cmd_end;


    if (cmd == NULL)
        return;

    /* no line, no command or args */
    if (line == NULL)
    {
        cmd->cmd_str = NULL;
        cmd->cmd_str_len = 0;
        cmd->cmd_arg = NULL;
        cmd->cmd_arg_len = 0;

        return;
    }

    cmd->cmd_str = line;

    cmd_end = (const uint8_t *)memchr(line, CVS_COMMAND_SEPARATOR, end - line);
    if (cmd_end != NULL)
    {
        cmd->cmd_str_len = cmd_end - line;
        cmd->cmd_arg = cmd_end + 1;
        cmd->cmd_arg_len = end - cmd_end - 1;
    }
    else
    {
        cmd->cmd_str_len = end - line;
        cmd->cmd_arg = NULL;
        cmd->cmd_arg_len = 0;
    }
}


/*
**  NAME
**    CvsValidateEntry
**       Checks Entry argument to make sure it is well formed
**       An entry sent to the server should look like:
**       /file/version///
**       e.g. '/cvs.c/1.5///'
**       There should be nothing between the third and
**       fourth slashes
**
*/
/**
**  @return CVS_ENTRY_VALID if valid
**  @return CVS_ENTRY_INVALID if invalid
**
*/

static int CvsValidateEntry(const uint8_t *entry_arg, const uint8_t *end_arg)
{
    int slashes = 0;


    if ((entry_arg == NULL) || (end_arg == NULL))
    {
        return CVS_ENTRY_VALID;
    }

    /* There should be exactly 5 slashes in the string */
    while (entry_arg < end_arg)
    {
        /* if on the 3rd slash, check for next char == '/' or '+'
         * This is where the heap overflow on multiple Is-Modified
         * commands occurs */
        if (slashes == 3)
        {
            if((*entry_arg != '/')&&(*entry_arg != '+'))
            {
                return CVS_ENTRY_INVALID;
            }
        }
        if (*entry_arg != '/')
        {
            entry_arg = (uint8_t*)memchr(entry_arg, '/', end_arg - entry_arg);
            if (entry_arg == NULL)
                break;
        }

        slashes++;
        entry_arg++;
    }

    if (slashes != 5)
    {
        return CVS_ENTRY_INVALID;
    }

    return CVS_ENTRY_VALID;
}


/*
**       Gets a line from the data string.
**       Sets an end-of-line marker to point to the marker
**       and an end-of-line pointer to point after marker
**
*/

static void CvsGetEOL(const uint8_t *ptr, const uint8_t *end,
                      const uint8_t **eol, const uint8_t **eolm)
{
    *eolm = (uint8_t *)memchr(ptr, CVS_COMMAND_DELIMITER, end - ptr);
    if (*eolm == NULL)
    {
        *eolm = end;
        *eol = end;
    }
    else
    {
        *eol = *eolm + 1;
    }
}

//-------------------------------------------------------------------------
// api methods
//-------------------------------------------------------------------------

static void cvs_parse(char *rule_args, CvsRuleOption *cvs_rule_option)
{
    char **toks;
    int num_toks = 0;


    toks = mSplit(rule_args, CVS_CONFIG_DELIMITERS, 2, &num_toks, 0);

    switch (num_toks)
    {
        /* no arguments */
        case 1:
            if (strcasecmp(toks[0], CVS_CONF_INVALID_ENTRY_STR) == 0)
            {
                cvs_rule_option->type = CVS_INVALID_ENTRY;
            }
            else
            {
                ParseError("Invalid argument specified for CVS rule: %s",
                           toks[0]);
            }

            break;

        default:
            ParseError("No or wrong number of arguments "
                       "specified for CVS rule");
                       
            break;
    }

    mSplitFree(&toks, num_toks);
}

static IpsOption* cvs_ctor(
    SnortConfig*, char *data, OptTreeNode*)
{
    CvsRuleOption cvs_rule_option;
    memset(&cvs_rule_option, 0, sizeof(cvs_rule_option));
    cvs_parse(data, &cvs_rule_option);
    return new CvsOption(cvs_rule_option);
}

static void cvs_dtor(IpsOption* p)
{
    delete p;
}

static void cvs_ginit(SnortConfig*)
{
#ifdef PERF_PROFILING
    RegisterOtnProfile(s_name, &cvsPerfStats, cvs_get_profile);
#endif
}

static const IpsApi cvs_api =
{
    {
        PT_IPS_OPTION,
        s_name,
        IPSAPI_PLUGIN_V0,
        0,
        nullptr,
        nullptr
    },
    OPT_TYPE_DETECTION,
    0, 0,
    cvs_ginit,
    nullptr,
    nullptr,
    nullptr,
    cvs_ctor,
    cvs_dtor,
    nullptr
};

#ifdef BUILDING_SO
SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &cvs_api.base,
    nullptr
};
#else
const BaseApi* ips_cvs = &cvs_api.base;
#endif

