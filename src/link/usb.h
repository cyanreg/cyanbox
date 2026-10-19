/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* An all-in-one on USB (cyanbox/link.h, CBLinkOptions.usb_card): what
 * rekordbox sends it on USB MIDI while its Link Export is on, every 200 ms
 * from a thread of its own. An XDJ-RX3 (firmware 1.20) is a USB hub with a
 * network adapter, on which Link runs as on any Ethernet, and its own MIDI
 * device; it lists the computer on SOURCE only while this keeps coming
 * (its PcControlCert takes it for a one-second lease; seen on a real one,
 * 2026-10-20). */

#ifndef CYANBOX_LINK_USB_H
#define CYANBOX_LINK_USB_H

typedef struct CBLinkUsb CBLinkUsb;

/* Starts sending to the first MIDI port of the sound card `card`: its id
 * (as /sys/class/sound/cardN/id has it, case aside) or its number. A card
 * not there, or gone, is looked for again every second, by its id again
 * (a unit plugged in again may be another number). `root`: NULL for the
 * system, else a directory standing in for / (the tests: its
 * sys/class/sound and dev/snd). */
int  cb_link_usb_start(CBLinkUsb **out, const char *card, const char *root);
void cb_link_usb_stop(CBLinkUsb **u);
/* The messages written so far. */
int  cb_link_usb_sent(CBLinkUsb *u);

#endif /* CYANBOX_LINK_USB_H */
