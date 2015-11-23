/*
  clock.h - part of USBasp

  Autor..........: Thomas Fischl <tfischl@gmx.de> 
  Description....: Provides functions for timing/waiting
  Licence........: GNU GPL v2 (see Readme.txt)
  Creation Date..: 2005-02-23
  Last change....: 2008-04-02 for metaboard hardware by Christian Starkjohann
*/

#ifndef __clock_h_included__
#define	__clock_h_included__

#define TIMERVALUE      TCNT0
#define CLOCK_T_320us	(uint8_t)(F_CPU / (64 * 3125L))

#ifdef __AVR_ATmega8__
#define TCCR0B  TCCR0
#endif

/* set prescaler to 64 */
#define clockInit()  TCCR0B = (1 << CS01) | (1 << CS00);

/* wait time * 320 us */
void clockWait(uint8_t time);

#endif /* __clock_h_included__ */
