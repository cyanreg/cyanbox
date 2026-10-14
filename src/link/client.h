/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The fake player's database connection, for the tests, which ask what
 * rx_link_client_browse does not (rxbox/link.h). */

#ifndef RXBOX_LINK_CLIENT_H
#define RXBOX_LINK_CLIENT_H

#include "rxbox/link.h"

#include "dbserver.h"

/* The connection to the source's database, made on first use from our
 * address (the port query, then the database port, set up as our
 * number); NULL with *err set when it cannot be made. */
RXDbClient *rx_link_client_db(RXLinkClient *c, int *err);

#endif /* RXBOX_LINK_CLIENT_H */
