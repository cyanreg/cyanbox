/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The fake player's database connection, for the tests, which ask what
 * cb_link_client_browse does not (cyanbox/link.h). */

#ifndef CYANBOX_LINK_CLIENT_H
#define CYANBOX_LINK_CLIENT_H

#include "cyanbox/link.h"

#include "dbserver.h"

/* The connection to the source's database, made on first use from our
 * address (the port query, then the database port, set up as our
 * number); NULL with *err set when it cannot be made. */
CBDbClient *cb_link_client_db(CBLinkClient *c, int *err);

#endif /* CYANBOX_LINK_CLIENT_H */
