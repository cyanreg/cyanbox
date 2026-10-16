/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The cyanbox executable: everything is in the command line (src/cli.c),
 * which runs the interface when no subcommand is given. */

int cb_cli_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return cb_cli_main(argc, argv);
}
