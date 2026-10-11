/**
 ******************************************************************************
 * @file    cmd_table.c
 * @brief   Command-table console parser — PROVIDED MODULE (spec R25, §6).
 *          It assembles a line, tokenizes it, and dispatches it. It does
 *          NOT block, and it runs fully in the context of the task that
 *          calls cmd_table_poll().
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *          PROVIDED MODULE — do not change it. Read it. This is the same
 *          retype-free table pattern that you use again in P4.
 ******************************************************************************
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "cmd_table.h"
#include "console.h"

static const cmd_entry_t *s_table;
static uint32_t           s_count;
static char               s_line[CMD_MAX_LINE];
static uint32_t           s_fill;

void cmd_table_init(const cmd_entry_t *table, uint32_t count)
{
    s_table = table;
    s_count = count;
    s_fill  = 0u;
    printf("> ");
}

void cmd_table_help(void)
{
    for (uint32_t i = 0u; i < s_count; i++)
        printf("  %-10s %s\n", s_table[i].name, s_table[i].help);
}

static int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Tokenize s_line in-place and dispatch on argv[0]. */
static void dispatch(void)
{
    char *argv[CMD_MAX_ARGS];
    int   argc = 0;
    char *p    = s_line;

    while (*p && argc < (int)CMD_MAX_ARGS) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (argc == 0)
        return;                              /* empty line, reprompt only    */

    for (uint32_t i = 0u; i < s_count; i++) {
        if (str_ieq(argv[0], s_table[i].name)) {
            s_table[i].fn(argc, argv);
            return;
        }
    }
    printf("?? unknown command '%s' — try HELP\n", argv[0]);
}

void cmd_table_poll(void)
{
    int ch;
    while ((ch = console_poll()) >= 0) {     /* drain what has ARRIVED only  */
        if (ch == '\r' || ch == '\n') {
            printf("\n");
            s_line[s_fill] = '\0';
            dispatch();
            s_fill = 0u;
            printf("> ");
        } else if (ch == 0x08 || ch == 0x7F) {   /* backspace / DEL          */
            if (s_fill > 0u) {
                s_fill--;
                printf("\b \b");
            }
        } else if (ch >= 0x20 && ch < 0x7F) {    /* printable                */
            if (s_fill < CMD_MAX_LINE - 1u) {
                s_line[s_fill++] = (char)ch;
                printf("%c", (char)ch);          /* echo                     */
            }
        }                                        /* other control chars: eat */
    }
}
