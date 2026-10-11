/**
 ******************************************************************************
 * @file    cmd_table.h
 * @brief   Command-table console parser (name → handler → help) over the
 *          course console — PROVIDED MODULE (spec R25, §6).
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *          PROVIDED MODULE — do not change it.
 *
 *  What this module does FOR you (plumbing, not the lesson):
 *    - it assembles a command line one character at a time from
 *      console_poll(), with echo, backspace and an overflow limit. It does
 *      NOT block. Each call drains only the characters that arrived before
 *      the call.
 *    - on ENTER it tokenizes the line (whitespace-split, up to 8 tokens).
 *      It then dispatches on the first token, case-insensitive, against the
 *      table that you registered.
 *    - it prints the one-line help text of each entry on cmd_table_help().
 *
 *  What stays YOURS (the graded surface, spec R25):
 *    - the HANDLERS — HELP, STATUS, MODE, SET PERIOD, SET THRESH, FORMAT
 *      (which SHALL keep its confirmation token), DOWNLOAD, and CPU.
 *    - the NON-BLOCKING console tick of about 50 ms in your task set. A task
 *      that you schedule calls cmd_table_poll(), and a handler runs in the
 *      context of THAT task. A handler can take mutexes, and it is not an
 *      ISR.
 *    - the echo of each accepted configuration value as a logged EVENT
 *      record.
 *
 *  Multi-word commands ("SET PERIOD 500"): dispatch is on argv[0] ("SET"),
 *  and your handler switches on argv[1]. One entry for each verb keeps the
 *  table honest and the help screen short.
 ******************************************************************************
 */
#ifndef CMD_TABLE_H
#define CMD_TABLE_H

#include <stdint.h>

#define CMD_MAX_LINE   64u   /* longest accepted command line (incl. NUL)     */
#define CMD_MAX_ARGS    8u   /* tokens in a line: verb + up to 7 arguments    */

/** A handler receives the tokenized line: argv[0] is the verb as typed. */
typedef void (*cmd_handler_t)(int argc, char *argv[]);

typedef struct {
    const char   *name;   /* verb, matched case-insensitively ("set"=="SET") */
    cmd_handler_t fn;     /* called on a completed line                      */
    const char   *help;   /* one line for cmd_table_help()                   */
} cmd_entry_t;

/** Register your table (a static const array you own). Call once, before the
 *  first cmd_table_poll(). The table is not copied — keep it alive. */
void cmd_table_init(const cmd_entry_t *table, uint32_t count);

/** Non-blocking service call: drain any received characters, echo them, and
 *  dispatch a completed line. Call this from your ~50 ms console tick (R25) —
 *  where that tick lives in your task set is YOUR design decision. */
void cmd_table_poll(void);

/** Print one help line for each registered entry. Use it in your HELP
 *  handler. */
void cmd_table_help(void);

#endif /* CMD_TABLE_H */
