/*******************************************************************************
 * Copyright (c) 2007, 2008 Wind River Systems, Inc. and others.
 * All rights reserved. This program and the accompanying materials 
 * are made available under the terms of the Eclipse Public License v1.0 
 * which accompanies this distribution, and is available at 
 * http://www.eclipse.org/legal/epl-v10.html 
 *  
 * Contributors:
 *     Wind River Systems - initial API and implementation
 *******************************************************************************/

/*
 * Implements input buf used by unbuffered channel transports.
 */

#include "mdep.h"
#include <stddef.h>
#include <errno.h>
#include <assert.h>
#include "exceptions.h"
#include "trace.h"
#include "inputbuf.h"

static void ibuf_new_message(InputBuf * ibuf) {
    ibuf->message_count++;
    ibuf->trigger_message(ibuf);
}

static void ibuf_eof(InputBuf * ibuf) {
    /* Treat eof as a message */
    ibuf->eof = 1;
    ibuf_new_message(ibuf);
}

void ibuf_trigger_read(InputBuf * ibuf) {
    int size;

    if (ibuf->full || ibuf->eof) return;
    if (ibuf->out <= ibuf->inp) size = ibuf->buf + BUF_SIZE - ibuf->inp;
    else size = ibuf->out - ibuf->inp;
    ibuf->post_read(ibuf, ibuf->inp, size);
}

int ibuf_get_more(InputBuf * ibuf, InputStream * inp, int peeking) {
    unsigned char *out = inp->cur;
    unsigned char *max;
    int esc = 0;
    int res;

    assert(ibuf->message_count > 0);
    assert(ibuf->handling_msg == HandleMsgActive);
    assert(out >= ibuf->buf && out <= ibuf->buf+BUF_SIZE);
    assert(out == inp->end);
    if (out == ibuf->buf+BUF_SIZE) {
        inp->end = inp->cur = out = ibuf->buf;
    }
    if (out != ibuf->out) {
        /* Data read - update buf */
        ibuf->out = out;
        ibuf->full = 0;
        ibuf_trigger_read(ibuf);
    }
    for (;;) {
        if (out == ibuf->inp && !ibuf->full) {
            /* No data available */
            assert(ibuf->long_msg || ibuf->eof);
            assert(ibuf->message_count == 1);
            if (ibuf->eof) return MARKER_EOS;
            ibuf_trigger_read(ibuf);
            ibuf->wait_read(ibuf);
            continue;
        }

        /* Data available */
        res = *out;
        if (esc) {
            switch (res) {
            case 0:
                res = ESC;
                if (peeking) return res;
                break;
            case 1:
                res = MARKER_EOM;
                if (peeking) return res;
                ibuf->message_count--;
                ibuf->handling_msg = HandleMsgIdle;
                if (ibuf->message_count) {
                    ibuf->trigger_message(ibuf);
                }
                break;
            case 2:
                res = MARKER_EOS;
                if (peeking) return res;
                break;
            }
            inp->cur = inp->end = ++out;
            return res;
        }
        if (res != ESC) {
            /* Plain data - fast path */
            inp->cur = out;
            max = out < ibuf->inp ? ibuf->inp : ibuf->buf+BUF_SIZE;
            while (++out != max && *out != ESC);
            inp->end = out;
            if (peeking) return res;
            inp->cur++;
            return res;
        }
        if (++out == ibuf->buf+BUF_SIZE) out = ibuf->buf;
        esc = 1;
    }
}

void ibuf_init(InputBuf * ibuf, InputStream * inp) {
    inp->cur = inp->end = ibuf->out = ibuf->inp = ibuf->buf;
}

void ibuf_flush(InputBuf * ibuf, InputStream * inp) {
    inp->cur = inp->end = ibuf->out = ibuf->inp;
    ibuf->full = 0;
    ibuf->message_count = 0;
}

void ibuf_read_done(InputBuf * ibuf, int len) {
    unsigned char * inp;
    int esc;

    assert(len >= 0);
    if (len == 0) {
        ibuf_eof(ibuf);
        return;
    }
    assert(!ibuf->eof);

    /* Preprocess newly read data to count messages */
    inp = ibuf->inp;
    esc = ibuf->esc;
    while (len-- > 0) {
        unsigned char ch = *inp++;
        if (inp == ibuf->buf+BUF_SIZE) inp = ibuf->buf;
        if (esc) {
            esc = 0;
            switch (ch) {
            case 0:
                /* ESC byte */
                break;
            case 1:
                /* EOM - End Of Message */
                if (ibuf->long_msg) {
                    ibuf->long_msg = 0;
                    assert(ibuf->message_count == 1);
                }
                else {
                    ibuf_new_message(ibuf);
                }
                break;
            case 2:
                /* EOS - End Of Stream */
                ibuf_eof(ibuf);
                break;
            default:
                /* Invalid escape sequence */
                trace(LOG_ALWAYS, "Protocol: Invalid escape sequence");
                ibuf_eof(ibuf);
                break;
            }
        }
        else if (ch == ESC) {
            esc = 1;
        }
    }
    ibuf->esc = esc;
    ibuf->inp = inp;

    if (inp == ibuf->out) {
        ibuf->full = 1;
        if (ibuf->message_count == 0) {
            /* Buffer full with incomplete message - start processing anyway */
            ibuf->long_msg = 1;
            ibuf_new_message(ibuf);
        }
    }
    else {
        ibuf_trigger_read(ibuf);
    }
}

int ibuf_start_message(InputBuf * ibuf) {
    assert(ibuf->handling_msg == HandleMsgTriggered);
    if (ibuf->message_count == 0) {
        ibuf->handling_msg = HandleMsgIdle;
        return 0;
    }
    if (ibuf->eof) {
        ibuf->handling_msg = HandleMsgIdle;
        return -1;
    }
    ibuf->handling_msg = HandleMsgActive;
    return 1;
}
