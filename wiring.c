/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2011 Brett Hagman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id: wiring.c 989 2011-08-26 20:35:40Z joerg_wunsch $ */

/*
 * avrdude interface for Wiring bootloaders
 *
 * http://wiring.org.co/
 *
 * The Wiring bootloader uses a near-complete STK500v2 protocol.
 * (Only ISP specific programming commands are not implemented
 * e.g. chip erase).
 * DTR and RTS signals are diddled to set the board into programming mode.
 *
 * Also includes an extended parameter to introduce a delay after opening
 * to accommodate multi-layered programmers/bootloaders.  If the extended
 * parameter 'snooze' > 0, then no DTR/RTS toggle takes place, and
 * AVRDUDE will wait that amount of time in milliseconds before syncing.
 *
 * Unfortunately, there is no way to easily chain private programmer data
 * when we "inherit" programmer types as we have (stk500v2).  Sooooo, a 
 * *cringe* global variable is used to store the snooze time.
 */

#include "ac_cfg.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <wiringPi.h>

#include "avrdude.h"
#include "pgm.h"
#include "stk500v2_private.h"
#include "stk500v2.h"
#include "serial.h"
#include "avrdude.h"
#include "pgm.h"
#include "stk500_private.h"
#include "stk500.h"
#include "serial.h"
#include "alamode.h"


/*
 * Private data for this programmer.
 */
struct wiringpdata
{
  /*
   * We just have the single snooze integer to carry around for now.
   */
  int snoozetime;
};


/* wiringpdata is our private data */
/* pdata is stk500v2's private data (inherited) */

#define WIRINGPDATA(x) ((struct wiringpdata *)(x))

#define STK500V2PDATA(pgm) ((struct pdata *)(pgm->cookie))


static void wiring_setup(PROGRAMMER * pgm)
{
  void *mycookie;

  /*
   * First, have STK500v2 backend allocate its own private data.
   */
  stk500v2_setup(pgm);

  /*
   * Now prepare our data
   */
  if ((mycookie = malloc(sizeof(struct wiringpdata))) == 0) {
    fprintf(stderr,
      "%s: wiring_setup(): Out of memory allocating private data\n",
      progname);
    exit(1);
  }
  memset(mycookie, 0, sizeof(struct wiringpdata));
  WIRINGPDATA(mycookie)->snoozetime = 0;

  /*
   * Store our own cookie in a safe place for the time being.
   */
  STK500V2PDATA(pgm)->chained_pdata = mycookie;
}

static void wiring_teardown(PROGRAMMER * pgm)
{
  void *mycookie;

  mycookie = STK500V2PDATA(pgm)->chained_pdata;

  free(mycookie);

  stk500v2_teardown(pgm);
}

static int wiring_parseextparms(PROGRAMMER * pgm, LISTID extparms)
{
  LNODEID ln;
  const char *extended_param;
  int rv = 0;
  void *mycookie = STK500V2PDATA(pgm)->chained_pdata;

  for (ln = lfirst(extparms); ln; ln = lnext(ln)) {
    extended_param = ldata(ln);

    if (strncmp(extended_param, "snooze=", strlen("snooze=")) == 0) {
      int newsnooze;
      if (sscanf(extended_param, "snooze=%i", &newsnooze) != 1 ||
          newsnooze < 0) {
        fprintf(stderr,
                "%s: wiring_parseextparms(): invalid snooze time '%s'\n",
                progname, extended_param);
        rv = -1;
        continue;
      }
      if (verbose >= 2) {
        fprintf(stderr,
                "%s: wiring_parseextparms(): snooze time set to %d ms\n",
                progname, newsnooze);
      }
      WIRINGPDATA(mycookie)->snoozetime = newsnooze;

      continue;
    }

    fprintf(stderr,
            "%s: wiring_parseextparms(): invalid extended parameter '%s'\n",
            progname, extended_param);
    rv = -1;
  }

  return rv;
}

static int wiring_open(PROGRAMMER * pgm, char * port)
{

  int timetosnooze;
  void *mycookie = STK500V2PDATA(pgm)->chained_pdata;

  strcpy(pgm->port, port);
  serial_open(port, pgm->baudrate ? pgm->baudrate: 115200, &pgm->fd);

  /* If we have a snoozetime, then we wait and do NOT toggle DTR/RTS */

  if (WIRINGPDATA(mycookie)->snoozetime > 0) {
    timetosnooze = WIRINGPDATA(mycookie)->snoozetime;

    if (verbose >= 2) {
      fprintf(stderr,
              "%s: wiring_open(): snoozing for %d ms\n",
              progname, timetosnooze);
    }
    while (timetosnooze--)
      usleep(1000);
    if (verbose >= 2) {
      fprintf(stderr,
              "%s: wiring_open(): done snoozing\n",
              progname);
    }

  } else {
    /* Perform Wiring programming mode RESET.           */
    /* This effectively *releases* both DTR and RTS.    */
    /* i.e. both DTR and RTS rise to a HIGH logic level */
    /* since they are active LOW signals.               */

     if (verbose >= 2) {
       fprintf(stderr,
               "%s: wiring_open(): releasing DTR/RTS\n",
               progname);
     }
     
     serial_set_dtr_rts(&pgm->fd, 0);
     usleep(50*1000);
     
     /* After releasing for 50 milliseconds, DTR and RTS */
     /* are asserted (i.e. logic LOW) again.             */
     
     if (verbose >= 2) {
       fprintf(stderr,
               "%s: wiring_open(): asserting DTR/RTS\n",
               progname);
     }
     
     serial_set_dtr_rts(&pgm->fd, 1);
     usleep(50*1000);

     
  }

  /* drain any extraneous input */
  stk500v2_drain(pgm, 0);

    wiringPiSetupGpio () ;
    pinMode(18,OUTPUT);

    printf ("Writing 0 to GPIO Pin18...") ;
    digitalWrite(18,0);
    usleep(50*1000);
    printf (" Done.\n") ;

    printf ("Writing 1 to GPIO Pin18...") ;
    digitalWrite(18,1);
    usleep(100*1000);
    printf (" Done.\n") ; 
    
       if (stk500v2_getsync(pgm) < 0)
         return -1;

    return 0;
}

static void wiring_close(PROGRAMMER * pgm)
{
  serial_set_dtr_rts(&pgm->fd, 0);
  serial_close(&pgm->fd);
  pgm->fd.ifd = -1;
}

void wiring_initpgm(PROGRAMMER * pgm)
{
  /* The Wiring bootloader uses a near-complete STK500v2 protocol. */

  stk500v2_initpgm(pgm);

  strcpy(pgm->type, "Wiring");
  pgm->open           = wiring_open;
  pgm->close          = wiring_close;

  pgm->setup          = wiring_setup;
  pgm->teardown       = wiring_teardown;
  pgm->parseextparams = wiring_parseextparms;
}

