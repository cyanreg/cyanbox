/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The rxbox executable: everything is in the command line (src/cli.c),
 * which runs the interface when no subcommand is given. */

int rx_cli_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return rx_cli_main(argc, argv);
}
