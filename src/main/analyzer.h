/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
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
// analyzer.h author Russ Combs <rucombs@cisco.com>

#ifndef ANALYZER_H
#define ANALYZER_H

#include "snort_types.h"
#include "protocols/packet.h"

enum AnalyzerCommand
{
    AC_NONE,
    AC_STOP,
    AC_PAUSE,
    AC_RESUME,
    AC_ROTATE,
    AC_SWAP,
    AC_MAX
};

class Swapper;

class Analyzer {
public:
    Analyzer(const char* source);

    void operator()(unsigned, Swapper*);

    bool is_done() { return done; };
    uint64_t get_count() { return count; };
    const char* get_source() { return source; };

    // FIXIT add asynchronous response too
    void execute(AnalyzerCommand ac) { command = ac; };
    void set_config(Swapper* ps) { swap = ps; };
    bool swap_pending() { return swap != nullptr; };

private:
    void analyze();
    bool handle(AnalyzerCommand);

private:
    bool done;
    uint64_t count;
    const char* source;
    AnalyzerCommand command;
    Swapper* swap;
};

#endif

